// Copyright 2011 Shinichiro Hamaji. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of  conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above
//      copyright notice, this list of conditions and the following
//      disclaimer in the documentation and/or other materials
//      provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY Shinichiro Hamaji ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL Shinichiro Hamaji OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
// USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
// OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

// A Mach-O loader for linux.

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <unordered_map>

#include "env_flags.h"
#include "fat.h"
#include "log.h"
#include "mach-o.h"

using namespace std;

DEFINE_bool(TRACE_FUNCTIONS, false, "Show calling functions");
DEFINE_bool(PRINT_TIME, false, "Print time spent in this loader");

class MachO;

static map<string, string> g_rename;
static vector<string> g_bound_names;
static set<string> g_no_trampoline;

struct Timer {
  Timer() : start_time(0) {}

  void start() {
    if (FLAGS_PRINT_TIME) {
      start_time = clock();
    }
  }

  void print(const char* name) {
    if (FLAGS_PRINT_TIME) {
      double elapsed = ((double)clock() - start_time) / CLOCKS_PER_SEC;
      printf("Elapsed time (%s): %f sec\n", name, elapsed);
    }
  }

  clock_t start_time;
};

class FileMap {
 public:
  void add(const MachO& mach, uintptr_t slide, uintptr_t base) {
    SymbolMap* symbol_map = new SymbolMap();
    symbol_map->filename = mach.filename();
    symbol_map->base = base;
    if (!maps_.insert(make_pair(base, symbol_map)).second) {
      err(1, "dupicated base addr: %p in %s",
          (void*)base, mach.filename().c_str());
    }

    for (size_t i = 0; i < mach.symbols().size(); i++) {
      MachO::Symbol sym = mach.symbols()[i];
      if (sym.name.empty() || sym.name[0] != '_')
        continue;
      sym.addr += slide;
      if (sym.addr < base)
        continue;
      symbol_map->symbols.insert(make_pair(sym.addr, sym.name.substr(1)));
    }
  }

  void addWatchDog(uintptr_t addr) {
    bool r = maps_.insert(make_pair(addr, (SymbolMap*)NULL)).second;
    CHECK(r);
  }

  const char* dumpSymbol(void* p) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    map<uintptr_t, SymbolMap*>::const_iterator found = maps_.upper_bound(addr);
    if (found == maps_.begin() || found == maps_.end()) {
      return NULL;
    }

    --found;
    return dumpSymbolFromMap(*found->second, addr);
  }

 private:
  struct SymbolMap {
    string filename;
    map<uintptr_t, string> symbols;
    uintptr_t base;
  };

  const char* dumpSymbolFromMap(const SymbolMap& symbol_map, uintptr_t addr) {
    uintptr_t file_offset = addr - symbol_map.base;

    // Use lower_bound as PC may be in just after call.
    map<uintptr_t, string>::const_iterator found =
        symbol_map.symbols.lower_bound(addr);
    if (found == symbol_map.symbols.begin()) {
      snprintf(dumped_stack_frame_buf_, 4095, "%s [%p(%lx)]",
               symbol_map.filename.c_str(), (void*)addr, (long)file_offset);
      return dumped_stack_frame_buf_;
    }

    --found;
    const char* name = found->second.c_str();
    uintptr_t func_offset = addr - found->first;
    snprintf(dumped_stack_frame_buf_, 4095, "%s(%s+%lx) [%p(%lx)]",
             symbol_map.filename.c_str(), name, (long)func_offset,
             (void*)addr, (long)file_offset);
    return dumped_stack_frame_buf_;
  }

  map<uintptr_t, SymbolMap*> maps_;
  char dumped_stack_frame_buf_[4096];
};

static FileMap g_file_map;

#ifdef __x86_64__
static const char* ARCH_NAME = "x86-64";
static const int BITS = 64;
#else
static const char* ARCH_NAME = "i386";
static const int BITS = 32;
#endif

static char* g_darwin_executable_path;

static Timer g_timer;

class MachOLoader;
static MachOLoader* g_loader;

static void initRename() {
#define RENAME(src, dst) g_rename.insert(make_pair(#src, #dst));
#define WRAP(src) RENAME(src, __darwin_ ## src)
#include "rename.tab"
#undef RENAME
#undef WRAP
}

static void initNoTrampoline() {
#define NO_TRAMPOLINE(name) g_no_trampoline.insert(#name);
#include "no_trampoline.tab"
#undef NO_TRAMPOLINE
}

static void doNothing() {
}

// A classic crt1 calls this through __DATA,__dyld with a pointer-sized out
// parameter; a 64-bit write here overran its stack slot on i386. Module
// initializers are run by runInitFuncs, so every dyld hook can do nothing.
static bool lookupDyldFunction(const char* name, uintptr_t* addr) {
  LOG << "lookupDyldFunction: " << name << endl;
  *addr = (uintptr_t)&doNothing;
  return true;
}

// Imports nothing provides are bound into a PROT_NONE region, one slot per
// symbol, so the first use -- a call through the jump table, a read through
// __IMPORT,__pointers, a CFString isa -- faults at an address that names the
// symbol. 16 bytes per slot leaves room for in-place addends (vtable+8).
static const uintptr_t kUndefinedSlot = 16;
static const size_t kUndefinedRegion = 1 << 16;
static char* g_undefined_base;
static vector<string> g_undefined_names;
static map<string, size_t> g_undefined_index;

static char* undefinedSymbolAddress(const string& name) {
  if (!g_undefined_base) {
    g_undefined_base = (char*)mmap(NULL, kUndefinedRegion, PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                   -1, 0);
    if (g_undefined_base == MAP_FAILED) {
      err(1, "mmap(undefined symbols)");
    }
  }
  size_t index;
  map<string, size_t>::const_iterator found = g_undefined_index.find(name);
  if (found != g_undefined_index.end()) {
    index = found->second;
  } else {
    index = g_undefined_names.size();
    g_undefined_names.push_back(name);
    g_undefined_index[name] = index;
  }
  CHECK((index + 1) * kUndefinedSlot <= kUndefinedRegion);
  return g_undefined_base + index * kUndefinedSlot;
}

static const char* undefinedSymbolAt(uintptr_t addr) {
  uintptr_t base = (uintptr_t)g_undefined_base;
  if (!base || addr < base || addr >= base + kUndefinedRegion) {
    return NULL;
  }
  size_t index = (addr - base) / kUndefinedSlot;
  if (index >= g_undefined_names.size()) {
    return NULL;
  }
  return g_undefined_names[index].c_str();
}

#ifndef __x86_64__
// Names a classic i386 image imports that glibc spells differently.
static const char* const kClassicRenames[][2] = {
  // crt1 zeroes the variable; reads go through __error().
  { "errno", "__darwin_errno_global" },
  // operator new(unsigned long) and new[]: size_t is unsigned int on Linux.
  { "_Znwm", "_Znwj" },
  { "_Znam", "_Znaj" },
};
#endif

// Overwrites an i386 __IMPORT,__jump_table entry with JMP rel32.
static void writeJump(uintptr_t at, uintptr_t target) {
  uintptr_t page = at & ~(uintptr_t)0xfff;
  if (mprotect((void*)page, at + 5 - page,
               PROT_READ | PROT_WRITE | PROT_EXEC)) {
    err(1, "mprotect(jump table entry at %p)", (void*)at);
  }
  unsigned char* p = (unsigned char*)at;
  int32_t rel = (int32_t)(target - (at + 5));
  p[0] = 0xe9;
  memcpy(p + 1, &rel, sizeof(rel));
}

#ifndef __x86_64__
// LD_MAC_TRACE_IMPORTS=1 logs each import the first time the image calls it
// through its jump table, with the caller; LD_MAC_TRACE_IMPORTS=all logs
// every call. A traced import's jump goes to a stub that pushes the import's
// index and enters a common routine, which saves the registers and flags,
// logs, restores them, and returns into the import with the stack as the
// caller left it.
static const size_t kTraceCapacity = 8192;
static const char* g_trace_mode;
static const char** g_trace_names;
static uintptr_t* g_trace_targets;
static unsigned char* g_trace_seen;
static unsigned char* g_trace_code;
static size_t g_trace_count;
static size_t g_trace_offset;

__attribute__((force_align_arg_pointer))
static void traceImport(uintptr_t index, uintptr_t caller) {
  static unsigned long calls;
  calls++;
  if (strcmp(g_trace_mode, "all") != 0) {
    if (g_trace_seen[index]) {
      return;
    }
    g_trace_seen[index] = 1;
  }
  fprintf(stderr, "import %lu: %s from %p\n", calls, g_trace_names[index],
          (void*)caller);
}

static void emitTrace(const unsigned char* bytes, size_t n) {
  memcpy(g_trace_code + g_trace_offset, bytes, n);
  g_trace_offset += n;
}

static void emitTrace32(uint32_t value) {
  memcpy(g_trace_code + g_trace_offset, &value, sizeof(value));
  g_trace_offset += sizeof(value);
}

// What an import's jump should reach: |target| itself, or when tracing, a
// stub that logs on the way to it.
static uintptr_t traceTarget(const string& name, uintptr_t target) {
  if (!g_trace_mode) {
    const char* mode = getenv("LD_MAC_TRACE_IMPORTS");
    g_trace_mode = mode && *mode ? mode : "";
  }
  if (!*g_trace_mode || undefinedSymbolAt(target) ||
      g_trace_count == kTraceCapacity) {
    return target;
  }
  if (!g_trace_code) {
    size_t size = 128 + kTraceCapacity * 10;
    g_trace_code = (unsigned char*)mmap(NULL, size,
                                        PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_trace_code == MAP_FAILED) {
      err(1, "mmap(import trace)");
    }
    g_trace_names = new const char*[kTraceCapacity];
    g_trace_targets = new uintptr_t[kTraceCapacity];
    g_trace_seen = new unsigned char[kTraceCapacity]();
    static const unsigned char kSave[] = {
      0x9c,                    // pushfl
      0x60,                    // pushal
      0x8b, 0x4c, 0x24, 0x28,  // movl 0x28(%esp), %ecx: the caller
      0x51,                    // pushl %ecx
      0x8b, 0x44, 0x24, 0x28,  // movl 0x28(%esp), %eax: the index
      0x50,                    // pushl %eax
      0xb8,                    // movl $traceImport, %eax
    };
    emitTrace(kSave, sizeof(kSave));
    emitTrace32((uint32_t)(uintptr_t)&traceImport);
    static const unsigned char kRestore[] = {
      0xff, 0xd0,              // call *%eax
      0x83, 0xc4, 0x08,        // addl $8, %esp
      0x61,                    // popal
      0x9d,                    // popfl
      0x87, 0x04, 0x24,        // xchgl %eax, (%esp): the index into %eax
      0x8b, 0x04, 0x85,        // movl targets(,%eax,4), %eax
    };
    emitTrace(kRestore, sizeof(kRestore));
    emitTrace32((uint32_t)(uintptr_t)g_trace_targets);
    static const unsigned char kContinue[] = {
      0x87, 0x04, 0x24,        // xchgl %eax, (%esp): the target onto the stack
      0xc3,                    // ret, into the import
    };
    emitTrace(kContinue, sizeof(kContinue));
  }
  size_t index = g_trace_count++;
  g_trace_names[index] = strdup(name.c_str());
  g_trace_targets[index] = target;
  uintptr_t stub = (uintptr_t)g_trace_code + g_trace_offset;
  static const unsigned char kPush = 0x68;  // pushl $index
  emitTrace(&kPush, 1);
  emitTrace32((uint32_t)index);
  static const unsigned char kJump = 0xe9;  // jmp common
  emitTrace(&kJump, 1);
  emitTrace32((uint32_t)((uintptr_t)g_trace_code -
                         ((uintptr_t)g_trace_code + g_trace_offset + 4)));
  return stub;
}
#endif

static void reportMapFailure(const MachO& mach, const char* segment,
                             uintptr_t vmaddr) {
  int e = errno;
  fprintf(stderr, "%s: cannot map %s at %p: %s\n",
          mach.filename().c_str(), segment, (void*)vmaddr, strerror(e));
  if (e == EPERM) {
    fprintf(stderr, "  the address is below vm.mmap_min_addr "
            "(cat /proc/sys/vm/mmap_min_addr)\n");
  } else if (e == EEXIST) {
    fprintf(stderr, "  something is already mapped there; "
            "is the loader built -no-pie?\n");
  }
  exit(1);
}

static uint64_t alignMem(uint64_t p, uint64_t a) {
  a--;
  return (p + a) & ~a;
}

static void dumpInt(int bound_name_id) {
  if (bound_name_id < 0) {
    fprintf(stderr, "%d: negative bound function id\n", bound_name_id);
    return;
  }
  if (bound_name_id >= (int)g_bound_names.size()) {
    fprintf(stderr, "%d: bound function id overflow\n", bound_name_id);
    return;
  }
  if (g_bound_names[bound_name_id].empty()) {
    fprintf(stderr, "%d: unbound function id\n", bound_name_id);
    return;
  }
  printf("calling %s(%d)\n",
         g_bound_names[bound_name_id].c_str(), bound_name_id);
  fflush(stdout);
}

static MachO* loadDylib(string dylib) {
  static const char executable_str[] = "@executable_path";
  static const size_t executable_str_len = strlen(executable_str);
  if (!strncmp(dylib.c_str(), executable_str, executable_str_len)) {
    string dir = g_darwin_executable_path;
    size_t found = dir.rfind('/');
    if (found == string::npos) {
      dir = ".";
    } else {
      dir = dir.substr(0, found);
    }
    dylib.replace(0, executable_str_len, dir);
  }

  return MachO::read(dylib.c_str(), ARCH_NAME);
}

typedef unordered_map<string, MachO::Export> Exports;

class MachOLoader {
#ifdef __x86_64__
  typedef uint64_t intptr;
  typedef segment_command_64 Segment;

  static const vector<Segment*>& getSegments(const MachO& mach) {
    return mach.segments64();
  }
#else
  typedef uint32_t intptr;
  typedef segment_command Segment;

  static const vector<Segment*>& getSegments(const MachO& mach) {
    return mach.segments();
  }
#endif

 public:
  MachOLoader()
    : last_addr_(0) {
    dylib_to_so_["/System/Library/Frameworks/CoreFoundation.framework"
                 "/Versions/A/CoreFoundation"].push_back(
                     "libCoreFoundation.so");

    // From Xcode 5.1, clang requires libncurses. However, since libncurses.so
    // looks a linker script (at least ubuntu 12.04), we cannot dlopen it.
    // We need to load libtinfo.so and libncurses.so.5 both.
    dylib_to_so_["/usr/lib/libncurses.5.4.dylib"].push_back("libtinfo.so");
    dylib_to_so_["/usr/lib/libncurses.5.4.dylib"].push_back("libncurses.so.5");

    symbol_to_so_.insert(make_pair("uuid_clear", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_compare", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_copy", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_generate", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_generate_random", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_generate_time", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_is_null", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_pack", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_parse", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_unpack", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_unparse", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_unparse_lower", "libuuid.so"));
    symbol_to_so_.insert(make_pair("uuid_unparse_upper", "libuuid.so"));

    symbol_to_so_.insert(make_pair("MD5", "libcrypto.so"));
    symbol_to_so_.insert(make_pair("MD5_Final", "libcrypto.so"));
    symbol_to_so_.insert(make_pair("MD5_Init", "libcrypto.so"));
    symbol_to_so_.insert(make_pair("MD5_Update", "libcrypto.so"));

    // Every function of these, not only those listed below.
    dylib_to_so_["/usr/lib/libz.1.dylib"].push_back("libz.so");
    dylib_to_so_["/usr/lib/libxml2.2.dylib"].push_back("libxml2.so");

    symbol_to_so_.insert(make_pair("compress", "libz.so"));
    symbol_to_so_.insert(make_pair("compressBound", "libz.so"));
    symbol_to_so_.insert(make_pair("deflate", "libz.so"));
    symbol_to_so_.insert(make_pair("deflateEnd", "libz.so"));
    symbol_to_so_.insert(make_pair("deflateInit_", "libz.so"));
    symbol_to_so_.insert(make_pair("inflate", "libz.so"));
    symbol_to_so_.insert(make_pair("inflateEnd", "libz.so"));
    symbol_to_so_.insert(make_pair("inflateInit_", "libz.so"));
    symbol_to_so_.insert(make_pair("inflateReset", "libz.so"));
    symbol_to_so_.insert(make_pair("zError", "libz.so"));

    if (FLAGS_TRACE_FUNCTIONS) {
      // Push all arguments into stack.

      // push %rax
      pushTrampolineCode(0x50);
      // push %rdi
      pushTrampolineCode(0x57);
      // push %rsi
      pushTrampolineCode(0x56);
      // push %rdx
      pushTrampolineCode(0x52);
      // push %rcx
      pushTrampolineCode(0x51);
      // push %r8
      pushTrampolineCode(0x5041);
      // push %r9
      pushTrampolineCode(0x5141);

      // push %xmm0..%xmm7
      for (int i = 0; i < 8; i++) {
        // sub $8, %rsp
        pushTrampolineCode(0x08ec8348);

        // movq %xmmN, (%rsp)
        pushTrampolineCode(0xd60f66);
        pushTrampolineCode(4 + i * 8);
        pushTrampolineCode(0x24);
      }

      // mov %r10, %rdi
      pushTrampolineCode(0xd7894c);

      // mov $func, %rdx
      pushTrampolineCode(0xba48);
      pushTrampolineCode64((unsigned long long)(void*)&dumpInt);

      // call *%rdx
      pushTrampolineCode(0xd2ff);

      // pop %xmm7..%xmm0
      for (int i = 7; i >= 0; i--) {
        // movq (%rsp), %xmmN
        pushTrampolineCode(0x7e0ff3);
        pushTrampolineCode(4 + i * 8);
        pushTrampolineCode(0x24);

        // add $8, %rsp
        pushTrampolineCode(0x08c48348);
      }

      // pop %r9
      pushTrampolineCode(0x5941);
      // pop %r8
      pushTrampolineCode(0x5841);
      // pop %rcx
      pushTrampolineCode(0x59);
      // pop %rdx
      pushTrampolineCode(0x5a);
      // pop %rsi
      pushTrampolineCode(0x5e);
      // pop %rdi
      pushTrampolineCode(0x5f);
      // pop %rax
      pushTrampolineCode(0x58);

      // ret
      pushTrampolineCode(0xc3);
    }
  }

  void loadSegments(const MachO& mach, intptr* slide, intptr* base) {
    *base = 0;
    --*base;

    const vector<Segment*>& segments = getSegments(mach);
    for (size_t i = 0; i < segments.size(); i++) {
      Segment* seg = segments[i];
      const char* name = seg->segname;
      if (!strcmp(name, SEG_PAGEZERO)) {
        continue;
      }

      LOG << seg->segname << ": "
          << "fileoff=" << seg->fileoff
          << ", vmaddr=" << seg->vmaddr << endl;

      int prot = 0;
      if (seg->initprot & VM_PROT_READ) {
        prot |= PROT_READ;
      }
      if (seg->initprot & VM_PROT_WRITE) {
        prot |= PROT_WRITE;
      }
      if (seg->initprot & VM_PROT_EXECUTE) {
        prot |= PROT_EXEC;
      }

      intptr filesize = alignMem(seg->filesize, 0x1000);
      intptr vmaddr = seg->vmaddr + *slide;
      if (vmaddr < last_addr_) {
        LOG << "will rebase: filename=" << mach.filename()
            << ", vmaddr=" << (void*)vmaddr
            << ", last_addr=" << (void*)last_addr_ << endl;
        CHECK(i == 0);
        vmaddr = last_addr_;
        *slide = vmaddr - seg->vmaddr;
      }
      *base = min(*base, vmaddr);

      intptr vmsize = alignMem(seg->vmsize, 0x1000);
      LOG << "mmap(file) " << mach.filename() << ' ' << name
          << ": " << (void*)vmaddr << "-" << (void*)(vmaddr + filesize)
          << " offset=" << mach.offset() + seg->fileoff << endl;
      if (filesize == 0) {
        continue;
      }
      // NOREPLACE: an unslid image landing on the loader or a library must
      // fail here rather than overwrite it.
      void* mapped = mmap((void*)vmaddr, filesize, prot,
                          MAP_PRIVATE | MAP_FIXED_NOREPLACE,
                          mach.fd(), mach.offset() + seg->fileoff);
      if (mapped == MAP_FAILED) {
        reportMapFailure(mach, name, vmaddr);
      }

      if (vmsize != filesize) {
        LOG << "mmap(anon) " << mach.filename() << ' ' << name
            << ": " << (void*)(vmaddr + filesize) << "-"
            << (void*)(vmaddr + vmsize)
            << endl;
        CHECK(vmsize > filesize);
        void* mapped = mmap((void*)(vmaddr + filesize),
                            vmsize - filesize, prot,
                            MAP_PRIVATE | MAP_FIXED_NOREPLACE | MAP_ANONYMOUS,
                            -1, 0);
        if (mapped == MAP_FAILED) {
          reportMapFailure(mach, name, vmaddr + filesize);
        }
      }

      last_addr_ = max(last_addr_, (intptr)vmaddr + vmsize);
    }
  }

  void doRebase(const MachO& mach, intptr slide) {
    for (size_t i = 0; i < mach.rebases().size(); i++) {
      const MachO::Rebase& rebase = *mach.rebases()[i];
      switch (rebase.type) {
      case REBASE_TYPE_POINTER: {
        char** ptr = (char**)(rebase.vmaddr + slide);
        LOG << "rebase: " << i << ": " << (void*)rebase.vmaddr << ' '
            << (void*)*ptr << " => "
            << (void*)(*ptr + slide) << " @" << ptr << endl;
        *ptr += slide;
        break;
      }

      default:
        fprintf(stderr, "Unknown rebase type: %d\n", rebase.type);
        exit(1);
      }
    }
  }

  void loadInitFuncs(const MachO& mach, intptr slide) {
    for (size_t i = 0; i < mach.init_funcs().size(); i++) {
      intptr addr = mach.init_funcs()[i] + slide;
      LOG << "Registering init func " << (void*)addr
          << " from " << mach.filename() << endl;
      init_funcs_.push_back(addr);
    }
  }

  // An image mapped with its exports registered, waiting for its binds.
  struct MappedImage {
    const MachO* mach;
    intptr slide;
    intptr base;
    const Exports* own;  // this image's exports
    // By LC_LOAD_DYLIB order: a Mach-O library's exports, or NULL for a
    // system library, which libmac and Linux stand in for.
    vector<const Exports*> libraries;
  };

  // A Linux library by the name the tables give, or by the runtime names a
  // system without development packages has (libz.so.1 for libz.so).
  static void* openLibrary(const string& so) {
    static const char* const kSuffixes[] = { "", ".1", ".2", ".3", ".1.1" };
    for (size_t i = 0; i < sizeof(kSuffixes) / sizeof(kSuffixes[0]); i++) {
      if (void* handle = dlopen((so + kSuffixes[i]).c_str(),
                                RTLD_LAZY | RTLD_GLOBAL)) {
        return handle;
      }
    }
    return NULL;
  }

  void mapDylibs(const MachO& mach, vector<MappedImage>* mapped,
                 vector<const Exports*>* libraries) {
    for (size_t i = 0; i < mach.dylibs().size(); i++) {
      string dylib = mach.dylibs()[i];

      if (!loaded_dylibs_.insert(dylib).second) {
        map<string, Exports*>::const_iterator found =
            dylib_exports_.find(dylib);
        libraries->push_back(found == dylib_exports_.end() ? NULL
                                                          : found->second);
        continue;
      }

      if (dylib_to_so_.count(dylib)) {
        const vector<string>& sos = dylib_to_so_[dylib];
        for (size_t i = 0; i < sos.size(); ++i) {
          const string& so = sos[i];
          LOG << "Loading " << so << " for " << dylib << endl;
          if (!openLibrary(so)) {
            fprintf(stderr, "Couldn't load %s for %s: %s\n",
                    so.c_str(), dylib.c_str(), dlerror());
          }
        }
      }

      // For now, we assume a dylib is a system library if its path
      // starts with /
      // TODO(hamaji): Do something?
      if (dylib[0] == '/') {
        libraries->push_back(NULL);
        continue;
      }

      // Kept for the process's life: its binds are applied only once every
      // image is mapped.
      images_.emplace_back(loadDylib(dylib));
      image_exports_.emplace_back(new Exports());
      Exports* exports = image_exports_.back().get();
      dylib_exports_[dylib] = exports;
      libraries->push_back(exports);
      mapImage(*images_.back(), exports, mapped);
    }
  }

  // Resolves a Darwin symbol name, without its leading underscore, the way
  // an import binds: Mach-O exports unless |mach_exports| is false, renames,
  // then libmac and the Linux libraries. NULL when nothing implements it.
  // The game's own run-time lookups (CFBundleGetFunctionPointerForName,
  // dlsym) come through here too, and they ask for system libraries, which
  // an image exporting a function by the same name does not stand in for.
  char* resolveName(string name, const string& mach_name,
                    bool mach_exports = true) {
    if (mach_exports) {
      const Exports::const_iterator export_found = exports_.find(mach_name);
      if (export_found != exports_.end()) {
        return (char*)export_found->second.addr;
      }
    }
#ifndef __x86_64__
    static const char* SUF_UNIX03 = "$UNIX2003";
    static const size_t SUF_UNIX03_LEN = strlen(SUF_UNIX03);
    if (name.size() > SUF_UNIX03_LEN &&
        !strcmp(name.c_str() + name.size() - SUF_UNIX03_LEN, SUF_UNIX03)) {
      name = name.substr(0, name.size() - SUF_UNIX03_LEN);
    }
    for (size_t r = 0;
         r < sizeof(kClassicRenames) / sizeof(kClassicRenames[0]); r++) {
      if (name == kClassicRenames[r][0]) {
        name = kClassicRenames[r][1];
        break;
      }
    }
#endif
    map<string, string>::const_iterator found = g_rename.find(name);
    if (found != g_rename.end()) {
      LOG << "Applying renaming: " << name << " => " << found->second << endl;
      name = found->second;
    }
    char* sym = (char*)dlsym(RTLD_DEFAULT, name.c_str());
    if (!sym) {
      map<string, string>::const_iterator iter = symbol_to_so_.find(name);
      if (iter != symbol_to_so_.end()) {
        if (openLibrary(iter->second)) {
          sym = (char*)dlsym(RTLD_DEFAULT, name.c_str());
        } else {
          fprintf(stderr, "Couldn't load %s for %s: %s\n",
                  iter->second.c_str(), name.c_str(), dlerror());
        }
      }
    }
    return sym;
  }

  // Resolves an import of |image| from the library its ordinal names, as a
  // two-level namespace image binds on Mac OS X: Age of Empires III exports
  // its own towlower, and its imports of towlower name libSystem, not
  // itself. An image with a flat namespace, or a dynamic lookup, looks
  // everywhere.
  char* resolveBind(const MappedImage& image, const MachO::Bind& bind,
                    const string& name) {
    if (!image.mach->twolevel()) {
      return resolveName(name, bind.name);
    }
    const Exports* in = NULL;
    switch (bind.ordinal) {
      case 0:  // SELF_LIBRARY_ORDINAL
        in = image.own;
        break;
      case 0xfe:  // DYNAMIC_LOOKUP_ORDINAL
        return resolveName(name, bind.name);
      case 0xff:  // EXECUTABLE_ORDINAL
        in = executable_exports_;
        break;
      default:
        if (bind.ordinal <= image.libraries.size()) {
          in = image.libraries[bind.ordinal - 1];
        }
        break;
    }
    if (in) {
      const Exports::const_iterator found = in->find(bind.name);
      if (found != in->end()) {
        return (char*)found->second.addr;
      }
    }
    // A Mach-O library may re-export another's symbols; a system library's
    // stand-ins are never the game's own.
    return resolveName(name, bind.name, in != NULL);
  }

  void doBind(const MappedImage& image) {
    const MachO& mach = *image.mach;
    intptr slide = image.slide;
    string last_weak_name = "";
    char* last_weak_sym = NULL;
    size_t seen_weak_bind_index = 0;
    size_t seen_weak_binds_orig_size = seen_weak_binds_.size();

    unsigned int common_code_size = (unsigned int)trampoline_.size();
    // Ensure that we won't change the address.
    trampoline_.reserve(common_code_size +
                        (1 + 6 + 5 + 10 + 3 + 2 + 1) * mach.binds().size());
    g_bound_names.resize(mach.binds().size());

    for (size_t i = 0; i < mach.binds().size(); i++) {
      MachO::Bind* bind = mach.binds()[i];
      if (bind->name[0] != '_') {
        LOG << bind->name << ": skipping" << endl;
        continue;
      }

      if (bind->type == BIND_TYPE_POINTER ||
          bind->type == MachO::BIND_TYPE_JUMP_TABLE ||
          bind->type == MachO::BIND_TYPE_EXTERNAL_RELOC) {
        string name = bind->name + 1;
        void** ptr = (void**)(bind->vmaddr + slide);
        char* sym = NULL;

        // A stub or relocation for a weak symbol the image defines itself
        // (a coalesced C++ template) binds to that definition.
        if (bind->is_classic && bind->is_weak &&
            bind->type != BIND_TYPE_POINTER) {
          if (bind->type == MachO::BIND_TYPE_JUMP_TABLE) {
            writeJump(bind->vmaddr + slide, (uintptr_t)bind->value);
          } else {
            *(uintptr_t*)ptr += (uintptr_t)bind->value;
          }
          continue;
        }

        if (bind->is_weak) {
          if (last_weak_name == name) {
            sym = last_weak_sym;
          } else {
            last_weak_name = name;
            if (seen_weak_bind_index != seen_weak_binds_orig_size &&
                !strcmp(seen_weak_binds_[seen_weak_bind_index].first.c_str(),
                        name.c_str())) {
              last_weak_sym = sym =
                  seen_weak_binds_[seen_weak_bind_index].second;
              seen_weak_bind_index++;
            } else {
              if (bind->is_classic) {
                *ptr = last_weak_sym = (char*)bind->value;
              } else {
                const Exports::const_iterator export_found =
                    exports_.find(bind->name);
                if (export_found != exports_.end()) {
                  *ptr = last_weak_sym = (char*)export_found->second.addr;
                } else {
                  last_weak_sym = (char*)*ptr;
                }
              }
              seen_weak_binds_.push_back(make_pair(name, last_weak_sym));
              while (seen_weak_bind_index != seen_weak_binds_orig_size &&
                     strcmp(
                         seen_weak_binds_[seen_weak_bind_index].first.c_str(),
                         name.c_str()) <= 0) {
                seen_weak_bind_index++;
              }
              continue;
            }
          }
        } else {
          sym = resolveBind(image, *bind, name);
          if (!sym) {
            LOG << name << ": undefined symbol" << endl;
            sym = undefinedSymbolAddress(name);
          }
          sym += bind->addend;
        }

        LOG << "bind " << name << ": "
            << *ptr << " => " << (void*)sym << " @" << ptr << endl;

        if (bind->type == MachO::BIND_TYPE_JUMP_TABLE) {
#ifndef __x86_64__
          writeJump(bind->vmaddr + slide, traceTarget(name, (uintptr_t)sym));
#else
          writeJump(bind->vmaddr + slide, (uintptr_t)sym);
#endif
          continue;
        }
        if (bind->type == MachO::BIND_TYPE_EXTERNAL_RELOC) {
          *(uintptr_t*)ptr += (uintptr_t)sym;
          continue;
        }

        if (FLAGS_TRACE_FUNCTIONS && !g_no_trampoline.count(name)) {
          LOG << "Generating trampoline for " << name << "..." << endl;

          *ptr = &trampoline_[0] + trampoline_.size();
          g_bound_names[i] = name;

          // push %rax  ; to adjust alignment for sse
          pushTrampolineCode(0x50);

          // mov $i, %r10d
          pushTrampolineCode(0xba41);
          pushTrampolineCode32((unsigned int)i);

          // call &trampoline_[0]
          pushTrampolineCode(0xe8);
          pushTrampolineCode32((unsigned int)(-4-trampoline_.size()));

          // mov $sym, %r10
          pushTrampolineCode(0xba49);
          pushTrampolineCode64((unsigned long long)(void*)sym);
          // call *%r10
          pushTrampolineCode(0xd2ff41);

          // pop %r10
          pushTrampolineCode(0x5a41);

          // ret
          pushTrampolineCode(0xc3);
        } else {
          *ptr = sym;
        }
      } else {
        fprintf(stderr, "Unknown bind type: %d\n", bind->type);
        abort();
      }
    }

    inplace_merge(seen_weak_binds_.begin(),
                  seen_weak_binds_.begin() + seen_weak_binds_orig_size,
                  seen_weak_binds_.end());
  }

  // Registers |mach|'s exports in |own|, which its binds and a dlopen handle
  // search, and in exports_, which flat lookups search. Two libraries built
  // from the same static library export the same names; each binds to its
  // own, and a flat lookup finds the first.
  void loadExports(const MachO& mach, intptr base, Exports* own) {
    own->rehash(own->size() + mach.exports().size());
    exports_.rehash(exports_.size() + mach.exports().size());
    size_t duplicates = 0;
    for (size_t i = 0; i < mach.exports().size(); i++) {
      MachO::Export exp = *mach.exports()[i];
      exp.addr += base;
      own->insert(make_pair(exp.name, exp));
      if (!exports_.insert(make_pair(exp.name, exp)).second) {
        duplicates++;
      }
    }
    if (duplicates) {
      LOG << mach.filename() << ": " << duplicates
          << " exported symbols another image exports too" << endl;
    }
  }

  void loadSymbols(const MachO& mach, intptr slide, intptr base) {
    g_file_map.add(mach, slide, base);
  }

  // Maps |mach| and registers its exports, then does the same for its
  // libraries, and queues their initializers ahead of its own. Nothing is
  // bound yet: a library may import from the executable or from a library
  // mapped after it, as the executable and its bundled PhysX libraries in
  // Age of Empires III do, and dyld binds only once every image is there.
  void mapImage(const MachO& mach, Exports* own,
                vector<MappedImage>* mapped) {
    MappedImage image;
    image.mach = &mach;
    image.slide = 0;
    image.base = 0;
    image.own = own;
    loadSegments(mach, &image.slide, &image.base);
    doRebase(mach, image.slide);
    loadExports(mach, image.base, own);
    mapDylibs(mach, mapped, &image.libraries);
    loadInitFuncs(mach, image.slide);
    mapped->push_back(image);
  }

  // Loads the executable, or with |exports| a library the program opens,
  // whose handle keeps its exports there.
  void load(const MachO& mach, Exports* exports = NULL) {
    if (!exports) {
      image_exports_.emplace_back(new Exports());
      exports = image_exports_.back().get();
      if (!executable_exports_) {
        executable_exports_ = exports;
      }
    }
    vector<MappedImage> mapped;
    mapImage(mach, exports, &mapped);
    for (size_t i = 0; i < mapped.size(); i++) {
      doBind(mapped[i]);
      loadSymbols(*mapped[i].mach, mapped[i].slide, mapped[i].base);
    }
  }

  void setupDyldData(const MachO& mach) {
    if (!mach.dyld_data())
      return;

    void** dyld_data = (void**)mach.dyld_data();
    dyld_data[1] = reinterpret_cast<void*>(&lookupDyldFunction);
  }

  void runInitFuncs(int argc, char** argv, char** envp, char** apple) {
    for (size_t i = 0; i < init_funcs_.size(); i++) {
      void** init_func = (void**)init_funcs_[i];
      LOG << "calling initializer function " << *init_func << endl;
      if (argc >= 0) {
        ((void(*)(int, char**, char**, char**))*init_func)(
            argc, argv, envp, apple);
      } else {
        ((void(*)())*init_func)();
      }
    }
    init_funcs_.clear();
  }

  void run(MachO& mach, int argc, char** argv, char** envp) {
    // I don't understand what it is.
    char* apple[2];
    apple[0] = argv[0];
    apple[1] = NULL;

    load(mach);
    setupDyldData(mach);
    if (!g_undefined_names.empty()) {
      fprintf(stderr, "ld-mac: %zu imports have no implementation yet; "
              "the first one used will name itself\n",
              g_undefined_names.size());
      if (getenv("LD_MAC_LIST_UNDEFINED")) {
        for (size_t i = 0; i < g_undefined_names.size(); i++) {
          fprintf(stderr, "  %s\n", g_undefined_names[i].c_str());
        }
      }
    }

    g_file_map.addWatchDog(last_addr_ + 1);

    char* trampoline_start_addr =
        (char*)(((uintptr_t)&trampoline_[0]) & ~0xfff);
    uint64_t trampoline_size =
        alignMem(&trampoline_[0] + trampoline_.size() - trampoline_start_addr,
                 0x1000);
    mprotect(trampoline_start_addr, trampoline_size,
             PROT_READ | PROT_WRITE | PROT_EXEC);

    g_timer.print(mach.filename().c_str());

    mach.close();

    runInitFuncs(argc, argv, envp, apple);

    LOG << "booting from " << (void*)mach.entry() << "..." << endl;
    fflush(stdout);
    CHECK(argc > 0);

    if (mach.is_lc_main_entry()) {
      uint64_t entry = mach.entry() + mach.text_base();
      exit(((int (*)(int, char**, char**))entry)(argc, argv, envp));
    } else {
      boot(mach.entry(), argc, argv, envp);
    }
    /*
      int (*fp)(int, char**, char**) =
      (int(*)(int, char**, char**))mach.entry();
      int ret = fp(argc, argv, envp);
      exit(ret);
    */
  }

 private:
  // Because .loop64 cannot be redefined.
  __attribute__((noinline))
  void boot(uint64_t entry, int argc, char** argv, char** envp);

  void pushTrampolineCode(unsigned int c) {
    while (c) {
      trampoline_.push_back(c & 255);
      c = c >> 8;
    }
  }

  void pushTrampolineCode64(unsigned long long c) {
    for (int i = 0; i < 8; i++) {
      trampoline_.push_back(c & 255);
      c = c >> 8;
    }
  }

  void pushTrampolineCode32(unsigned int c) {
    for (int i = 0; i < 4; i++) {
      trampoline_.push_back(c & 255);
      c = c >> 8;
    }
  }

  string trampoline_;
  intptr last_addr_;
  vector<uint64_t> init_funcs_;
  Exports exports_;
  vector<pair<string, char*> > seen_weak_binds_;
  map<string, vector<string> > dylib_to_so_;
  map<string, string> symbol_to_so_;
  set<string> loaded_dylibs_;
  vector<unique_ptr<MachO> > images_;
  vector<unique_ptr<Exports> > image_exports_;
  map<string, Exports*> dylib_exports_;  // by the path images load them by
  const Exports* executable_exports_ = NULL;
};

#ifndef __x86_64__
// Switches to a prepared Darwin initial stack and jumps to the image's entry.
extern "C" void ld_mac_jump_to_entry(uintptr_t* frame, uintptr_t entry)
    __attribute__((noreturn));
__asm__(".text\n"
        ".globl ld_mac_jump_to_entry\n"
        ".type ld_mac_jump_to_entry, @function\n"
        "ld_mac_jump_to_entry:\n"
        "  mov 8(%esp), %eax\n"
        "  mov 4(%esp), %esp\n"
        "  jmp *%eax\n");
#endif

void MachOLoader::boot(
    uint64_t entry, int argc, char** argv, char** envp) {
#ifdef __x86_64__
  // 0x08: argc
  // 0x10: argv[0]
  // 0x18: argv[1]
  //  ...: argv[n]
  //       0
  //       envp[0]
  //       envp[1]
  //       envp[n]
  __asm__ volatile(" mov %1, %%eax;\n"
                   " mov %2, %%rdx;\n"
                   " push $0;\n"
                   // TODO(hamaji): envp
                   " push $0;\n"
                   ".loop64:\n"
                   " sub $8, %%rdx;\n"
                   " push (%%rdx);\n"
                   " dec %%eax;\n"
                   " jnz .loop64;\n"
                   " mov %1, %%eax;\n"
                   " push %%rax;\n"
                   " jmp *%0;\n"
                   ::"r"(entry), "r"(argc), "r"(argv + argc), "r"(envp)
                   :"%rax", "%rdx");
  //fprintf(stderr, "done!\n");
#else
  // A classic crt1 walks argc, argv, envp and then apple[] up the initial
  // stack, laid out the way the Darwin kernel does it. Pushing argv alone
  // left start's envp walk reading whatever lay above.
  size_t envc = 0;
  while (envp[envc]) {
    envc++;
  }
  size_t words = 1 + argc + 1 + envc + 1 + 2;
  uintptr_t* frame =
      (uintptr_t*)__builtin_alloca(words * sizeof(uintptr_t));
  size_t n = 0;
  frame[n++] = argc;
  for (int i = 0; i < argc; i++) {
    frame[n++] = (uintptr_t)argv[i];
  }
  frame[n++] = 0;
  for (size_t i = 0; i < envc; i++) {
    frame[n++] = (uintptr_t)envp[i];
  }
  frame[n++] = 0;
  frame[n++] = (uintptr_t)g_darwin_executable_path;
  frame[n++] = 0;
  ld_mac_jump_to_entry(frame, (uintptr_t)entry);
#endif
}

void runMachO(MachO& mach, int argc, char** argv, char** envp) {
  MachOLoader loader;
  g_loader = &loader;
  loader.run(mach, argc, argv, envp);
  g_loader = NULL;
}

#if 0
static int getBacktrace(void** trace, int max_depth) {
    typedef struct frame {
        struct frame *bp;
        void *ret;
    } frame;

    int depth;
    frame* bp = (frame*)__builtin_frame_address(0);
    for (depth = 0; bp && depth < max_depth; depth++) {
        trace[depth] = bp->ret;
        bp = bp->bp;
    }
    return depth;
}
#endif

extern "C" {
  const char* dumpSymbol(void* p) {
    return g_file_map.dumpSymbol(p);
  }
}

#ifndef __x86_64__
// /proc/self/maps, read at the crash into a buffer set aside for it. Code a
// library generated at run time has no symbol for dladdr, and the end of a
// block an overrun ran off is only visible as the edge of a mapping.
static char g_crash_maps[1 << 17];

static bool findMapping(uintptr_t addr, bool code_only, const char** line,
                        int* length) {
  for (const char* p = g_crash_maps; *p;) {
    const char* end = strchr(p, '\n');
    if (!end) {
      end = p + strlen(p);
    }
    unsigned long lo, hi;
    char perms[5];
    if (sscanf(p, "%lx-%lx %4s", &lo, &hi, perms) == 3 && addr >= lo &&
        addr < hi && (!code_only || perms[2] == 'x')) {
      *line = p;
      *length = (int)(end - p);
      return true;
    }
    p = *end ? end + 1 : end;
  }
  return false;
}

// Says which mappings hold the faulting code and the address it touched,
// and lists code addresses left on the stack, the likely callers when the
// code keeps no frame pointer.
static void reportMappings(uintptr_t eip, uintptr_t fault, uintptr_t esp) {
  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0) {
    return;
  }
  size_t n = 0;
  ssize_t got;
  while (n + 1 < sizeof(g_crash_maps) &&
         (got = read(fd, g_crash_maps + n,
                     sizeof(g_crash_maps) - 1 - n)) > 0) {
    n += got;
  }
  close(fd);
  g_crash_maps[n] = '\0';
  const char* line;
  int length;
  if (findMapping(eip, false, &line, &length)) {
    fprintf(stderr, "eip is in %.*s\n", length, line);
  } else {
    fprintf(stderr, "eip is in no mapping\n");
  }
  if (findMapping(fault, false, &line, &length)) {
    fprintf(stderr, "the fault address is in %.*s\n", length, line);
  } else if (fault && findMapping(fault - 1, false, &line, &length)) {
    fprintf(stderr, "the fault address is just past %.*s\n", length, line);
  }
  // The heap grows up through low addresses, where the game maps memory at
  // addresses of its own choosing.
  if (const char* heap = strstr(g_crash_maps, "[heap]")) {
    const char* start = heap;
    while (start > g_crash_maps && start[-1] != '\n') {
      start--;
    }
    fprintf(stderr, "the heap is %.*s\n", (int)(heap + 6 - start), start);
  }
  unsigned long stack_lo, stack_hi;
  if (!findMapping(esp, false, &line, &length) ||
      sscanf(line, "%lx-%lx", &stack_lo, &stack_hi) != 2) {
    return;
  }
  uintptr_t stack_end = esp + 4096 < stack_hi ? esp + 4096 : stack_hi;
  fprintf(stderr, "code addresses on the stack:\n");
  int shown = 0;
  for (uintptr_t p = esp; p + sizeof(uintptr_t) <= stack_end && shown < 16;
       p += sizeof(uintptr_t)) {
    uintptr_t value = *(uintptr_t*)p;
    if (value >= 0x1000 && findMapping(value, true, &line, &length)) {
      fprintf(stderr, "  [esp+%#lx] %p in %.*s\n", (unsigned long)(p - esp),
              (void*)value, length, line);
      shown++;
    }
  }
}

// A fault in the Mac image. Names the import when the address is one of the
// undefined-symbol slots, then walks the image's frame-pointer chain, which
// glibc's backtrace() cannot see.
static void reportClassicFault(int signum, siginfo_t* siginfo,
                               ucontext_t* uc) {
  greg_t* r = uc->uc_mcontext.gregs;
  uintptr_t eip = r[REG_EIP];
  uintptr_t esp = r[REG_ESP];
  uintptr_t ebp = r[REG_EBP];
  uintptr_t fault = (uintptr_t)siginfo->si_addr;
  fflush(stdout);
  fprintf(stderr, "\n%s at eip %p, fault address %p\n",
          strsignal(signum), (void*)eip, (void*)fault);
  const char* name = undefinedSymbolAt(eip);
  if (name) {
    fprintf(stderr, "UNIMPLEMENTED: %s called from %p\n",
            name, (void*)*(uintptr_t*)esp);
  } else if ((name = undefinedSymbolAt(fault)) != NULL) {
    fprintf(stderr, "UNIMPLEMENTED: %s+%lu used as data at eip %p\n",
            name,
            (unsigned long)((fault - (uintptr_t)g_undefined_base) %
                            kUndefinedSlot),
            (void*)eip);
  } else if (eip < 0x1000) {
    fprintf(stderr, "a call through a NULL function pointer, from %p\n",
            (void*)*(uintptr_t*)esp);
  } else {
    // A fault outside the Mac image: name the library and the symbol.
    Dl_info info;
    if (dladdr((void*)eip, &info) && info.dli_fname) {
      fprintf(stderr, "in %s", info.dli_fname);
      if (info.dli_sname) {
        fprintf(stderr, " (%s+%#lx)", info.dli_sname,
                (unsigned long)(eip - (uintptr_t)info.dli_saddr));
      }
      fprintf(stderr, "\n");
    }
  }
  fprintf(stderr,
          "eax %08lx ebx %08lx ecx %08lx edx %08lx\n"
          "esi %08lx edi %08lx ebp %08lx esp %08lx\n",
          (unsigned long)r[REG_EAX], (unsigned long)r[REG_EBX],
          (unsigned long)r[REG_ECX], (unsigned long)r[REG_EDX],
          (unsigned long)r[REG_ESI], (unsigned long)r[REG_EDI],
          (unsigned long)ebp, (unsigned long)esp);
  fprintf(stderr, "return addresses (ebp chain):\n");
  for (int depth = 0; depth < 32; depth++) {
    if (ebp < esp || ebp >= esp + (8 << 20) || (ebp & 3)) {
      break;
    }
    uintptr_t* frame = (uintptr_t*)ebp;
    fprintf(stderr, "  %p\n", (void*)frame[1]);
    if (frame[0] <= ebp) {
      break;
    }
    ebp = frame[0];
  }
  reportMappings(eip, fault, esp);
  // libmac keeps a record of the memory the game asked mmap for.
  void (*report_game_mappings)(uintptr_t) =
      (void (*)(uintptr_t))dlsym(RTLD_DEFAULT, "hle_report_game_mappings");
  if (report_game_mappings) {
    report_game_mappings(fault);
  }
  // A profile being taken is written as it would be at exit.
  void (*write_profile)(void) =
      (void (*)(void))dlsym(RTLD_DEFAULT, "hle_profile_write");
  if (write_profile) {
    write_profile();
  }
  _exit(128 + signum);
}
#endif

// libmac's hle_recover_fault, which carries the game past faults it is
// known to take on its own. Looked up before any fault, as a lookup in the
// handler could wait on a lock the faulting code holds.
static int (*g_recover_fault)(int, siginfo_t*, void*);

static void installSignalHandler(int signum);

/* signal handler for fatal errors */
static void handleSignal(int signum, siginfo_t* siginfo, void* vuc) {
  ucontext_t *uc = (ucontext_t*)vuc;
#ifndef __x86_64__
  if (g_recover_fault && g_recover_fault(signum, siginfo, vuc)) {
    // SA_RESETHAND took the handler away on the way in.
    installSignalHandler(signum);
    return;
  }
  reportClassicFault(signum, siginfo, uc);
#endif
  void* pc = (void*)uc->uc_mcontext.gregs[
#ifdef __x86_64__
    REG_RIP
#else
    REG_EIP
#endif
    ];

  fprintf(stderr, "%s(%d) %d (@%p) PC: %p\n\n",
          strsignal(signum), signum, siginfo->si_code, siginfo->si_addr, pc);

  void* trace[100];
  int len = backtrace(trace, 99);
  //int len = getBacktrace(trace, 99);
  char** syms = backtrace_symbols(trace, len);
  for (int i = len - 1; i > 0; i--) {
    if (syms[i] && syms[i][0] != '[') {
      fprintf(stderr, "%s\n", syms[i]);
    } else {
      const char* s = dumpSymbol(trace[i]);
      if (s) {
        fprintf(stderr, "%s\n", s);
      } else {
        fprintf(stderr, "%p\n", trace[i]);
      }
    }
  }
}

static void installSignalHandler(int signum) {
  struct sigaction sigact;
  sigact.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigact.sa_sigaction = handleSignal;
  sigemptyset(&sigact.sa_mask);
  sigaction(signum, &sigact, NULL);
}

/* Generate a stack backtrace when a CPU exception occurs. */
static void initSignalHandler() {
  installSignalHandler(SIGFPE);
  installSignalHandler(SIGILL);
  installSignalHandler(SIGSEGV);
  installSignalHandler(SIGBUS);
  installSignalHandler(SIGABRT);
}

static bool loadLibMac(const char* mypath) {
  if (dlopen("libmac.so", RTLD_LAZY | RTLD_GLOBAL)) {
    return true;
  }

  char buf[PATH_MAX + 100];
  strcpy(buf, mypath);
  char* p = strrchr(buf, '/');
  if (!p) {
    fprintf(stderr, "Weird loader path: %s\n", mypath);
    exit(1);
  }
  strcpy(p, "/libmac.so");

  if (dlopen(buf, RTLD_LAZY | RTLD_GLOBAL)) {
    return true;
  }

  sprintf(p, "/libmac%d.so", BITS);
  if (dlopen(buf, RTLD_LAZY | RTLD_GLOBAL)) {
    return true;
  }

  return false;
}

static void initLibMac() {
  char mypath[PATH_MAX + 1];
  ssize_t l = readlink("/proc/self/exe", mypath, PATH_MAX);
  if (l < 0) {
    err(1, "readlink for /proc/self/exe");
  }
  mypath[l] = '\0';

  if (!loadLibMac(mypath)) {
    fprintf(stderr, "libmac not found\n");
    exit(1);
  }
  g_recover_fault = (int (*)(int, siginfo_t*, void*))dlsym(
      RTLD_DEFAULT, "hle_recover_fault");

  int* LIBMAC_LOG = (int*)dlsym(RTLD_DEFAULT, "LIBMAC_LOG");
  if (LIBMAC_LOG) {
    *LIBMAC_LOG = FLAGS_LOG;
  }

  char* loader_path = (char*)dlsym(RTLD_DEFAULT, "__loader_path");
  if (!loader_path) {
    fprintf(stderr, "wrong libmac: __loader_path not found\n");
    exit(1);
  }
  strcpy(loader_path, mypath);
}

static string ld_mac_dlerror_buf;
static bool ld_mac_dlerror_is_set;

static void* ld_mac_resolve_impl(const char* name) {
  MachOLoader* loader = g_loader;
  if (!loader) {
    return dlsym(RTLD_DEFAULT, name);
  }
  return loader->resolveName(name, string("_") + name, false);
}

// What dlopen returns for a Mac library that is not here as a Mach-O file,
// which is every system dylib and framework: its symbols resolve the way
// imports do.
static Exports g_system_library;

static bool isSystemLibraryHandle(void* handle) {
  // Darwin's RTLD_NEXT, RTLD_DEFAULT, RTLD_SELF and RTLD_MAIN_ONLY are -1..-5.
  return handle == &g_system_library ||
         ((intptr_t)handle < 0 && (intptr_t)handle >= -5);
}

static void* ld_mac_dlopen(const char* filename, int flag) {
  LOG << "ld_mac_dlopen: " << (filename ? filename : "(null)") << " "
      << flag << endl;
  if (!filename || access(filename, R_OK) != 0) {
    fprintf(stderr, "ld-mac: dlopen(%s): no Mach-O file there; its symbols "
            "resolve like imports\n", filename ? filename : "NULL");
    return &g_system_library;
  }

  Timer timer;
  timer.start();

  // TODO(hamaji): Handle failures.
  unique_ptr<MachO> dylib_mach(loadDylib(filename));

  MachOLoader* loader = g_loader;
  CHECK(loader);
  Exports* exports = new Exports();
  loader->load(*dylib_mach, exports);

  timer.print(filename);

  loader->runInitFuncs(-1, NULL, NULL, NULL);
  return exports;
}

static int ld_mac_dlclose(void* handle) {
  LOG << "ld_mac_dlclose" << endl;

  if (!isSystemLibraryHandle(handle)) {
    delete (Exports*)handle;
  }
  return 0;
}

static const char* ld_mac_dlerror(void) {
  LOG << "ld_mac_dlerror" << endl;

  if (!ld_mac_dlerror_is_set)
    return NULL;
  ld_mac_dlerror_is_set = false;
  return ld_mac_dlerror_buf.c_str();
}

static void* ld_mac_dlsym(void* handle, const char* symbol) {
  LOG << "ld_mac_dlsym: " << symbol << endl;

  if (isSystemLibraryHandle(handle)) {
    void* sym = ld_mac_resolve_impl(symbol);
    if (!sym) {
      ld_mac_dlerror_is_set = true;
      ld_mac_dlerror_buf = string("undefined symbol: ") + symbol;
      fprintf(stderr, "ld-mac: dlsym(%s): nothing implements it\n", symbol);
    }
    return sym;
  }
  Exports* exports = (Exports*)handle;
  Exports::const_iterator found = exports->find(string("_") + symbol);
  if (found == exports->end()) {
    ld_mac_dlerror_is_set = true;
    ld_mac_dlerror_buf = string("undefined symbol: ") + symbol;
    return NULL;
  }
  return (void*)found->second.addr;
}

void initDlfcn() {
#define SET_DLFCN_FUNC(f)                                   \
  do {                                                      \
    void** p = (void**)dlsym(RTLD_DEFAULT, "ld_mac_" #f);   \
    *p = (void*)&ld_mac_ ## f;                              \
  } while (0)

  SET_DLFCN_FUNC(dlopen);
  SET_DLFCN_FUNC(dlclose);
  SET_DLFCN_FUNC(dlerror);
  SET_DLFCN_FUNC(dlsym);

  // hle/ looks functions up by name, for CFBundleGetFunctionPointerForName.
  void** resolve = (void**)dlsym(RTLD_DEFAULT, "ld_mac_resolve");
  if (resolve) {
    *resolve = (void*)&ld_mac_resolve_impl;
  }
}

int main(int argc, char* argv[], char* envp[]) {
#ifdef __GLIBC__
  // glibc gives memory back to the kernel as soon as a block at the top of
  // the heap, or one it mapped on its own, is freed, more eagerly than Mac
  // OS X's allocator. Halo has faulted in NVIDIA's driver drawing a vertex
  // array from a page no longer mapped. Every block comes from the heap,
  // and the heap never shrinks.
  mallopt(M_MMAP_MAX, 0);
  mallopt(M_TRIM_THRESHOLD, -1);
  // NVIDIA's driver copies client vertex arrays on the CPU, and the game
  // draws past the ends of its vertex buffers: its Direct3D layer asks
  // glDrawRangeElements for one vertex more than a buffer holds, and a crash
  // in glDrawArrays read 57 KB past the heap's break. The Mac game pads the
  // buffers it can, and the GPU read them there. The heap grows 64 MB at a
  // time rather than 128 KB, so a buffer at its top rarely has that little
  // mapped memory above it.
  mallopt(M_TOP_PAD, 64 << 20);
#endif
  g_timer.start();
  initSignalHandler();
  initRename();
  initNoTrampoline();
  initLibMac();
  initDlfcn();

  argc--;
  argv++;
  for (;;) {
    if (argc == 0) {
      fprintf(stderr, "An argument required.\n");
      exit(1);
    }

    const char* arg = argv[0];
    if (arg[0] != '-') {
      break;
    }

    // TODO(hamaji): Do something for switches.

    argc--;
    argv++;
  }

  g_darwin_executable_path =
      (char*)dlsym(RTLD_DEFAULT, "__darwin_executable_path");
  if (!realpath(argv[0], g_darwin_executable_path)) {
  }

  unique_ptr<MachO> mach(MachO::read(argv[0], ARCH_NAME));
#ifdef __x86_64__
  if (!mach->is64()) {
    fprintf(stderr, "%s: not 64bit binary\n", argv[0]);
    exit(1);
  }
#else
  if (mach->is64()) {
    fprintf(stderr, "%s: not 32bit binary\n", argv[0]);
    exit(1);
  }
#endif
  runMachO(*mach, argc, argv, envp);
}
