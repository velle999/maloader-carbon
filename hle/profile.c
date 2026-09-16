// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// A sampling profiler for the game, for machines with no profiler
// installed.
//
// HLE_PROFILE=<file> starts it at the game's first frame. For each
// millisecond of CPU time the process uses, SIGPROF records the address the
// thread that used it was at, the game's code that thread was called from,
// and the thread. At exit the counts are written to <file> with the
// process's mappings, for tools/profile_report.py.
//
// The kernel signals the thread that was running when the millisecond ran
// out, unless that thread blocks SIGPROF, as SDL's threads do; the sound
// callback calls hle_profile_thread so that its thread is sampled too.
//
// HLE_LONG_FRAMES=<ms> samples the same way, with or without HLE_PROFILE,
// and reports each frame that takes at least that long: how much of it the
// game's thread spent working and how much in the swap, and where the
// samples taken on that thread during the frame were, grouped by the game's
// code they were called from. Time the thread spends blocked uses no CPU
// and is not sampled that way, so a watching thread also reads, every
// millisecond, the system call the game's thread is blocked in, from
// /proc/self/task/<id>/syscall, which leaves the thread undisturbed, and
// finds the game's code that made the call from the stack pointer there.

#define _GNU_SOURCE

#include "profile.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__i386__)

enum {
  kSlots = 1 << 18,   // distinct (address, caller, thread) triples kept
  kProbes = 32,
  kStackChunk = 256,  // stack words read at a time, looking for a caller
  kStackChunks = 8,
  kFrameSamples = 2048,  // the main thread's, in one frame
  kFramePlaces = 64,     // distinct callers counted in a long frame
  kPlacesShown = 6,
};

typedef struct {
  uint32_t address;
  uint32_t caller;
  uint32_t thread;
  uint32_t count;
} sample_slot;

static sample_slot* slots;
static int table_busy;
static uint32_t slots_used;
static uint32_t samples;
static uint32_t dropped;
static uintptr_t game_lo;
static uintptr_t game_hi;
static pid_t pid;
static char* output;

typedef struct {
  uint32_t address;  // or, for the watching thread's, the system call
  uint32_t caller;
  uint32_t micros;   // the time it stands for
} frame_sample;

static frame_sample frame_samples[kFrameSamples];
static volatile sig_atomic_t frame_sample_count;
static volatile sig_atomic_t frame_reading;  // no appending while set
static double long_frame_ms;                 // 0: not reported

// The watching thread's samples of the game's thread blocked, guarded by
// blocked_busy.
static frame_sample blocked_samples[kFrameSamples];
static uint32_t blocked_sample_count;
static int blocked_busy;

static int in_game(uintptr_t address) {
  return address >= game_lo && address < game_hi;
}

// Whether |address| in the game's code follows a call instruction, so that
// a word on a stack holding it is likely a return address.
static int after_call(uintptr_t address) {
  if (address < game_lo + 7 || address >= game_hi) {
    return 0;
  }
  const uint8_t* p = (const uint8_t*)address;
  if (p[-5] == 0xe8) {
    return 1;  // call rel32
  }
  // call *r/m32: 0xff, then a ModRM byte whose middle bits are 010.
  return (p[-2] == 0xff &&
          ((p[-1] & 0xf8) == 0xd0 ||  // *%reg
           ((p[-1] & 0xf8) == 0x10 && (p[-1] & 7) != 4 &&
            (p[-1] & 7) != 5))) ||  // *(%reg)
         (p[-3] == 0xff &&
          (((p[-2] & 0xf8) == 0x50 && (p[-2] & 7) != 4) ||  // *disp8(%reg)
           p[-2] == 0x14)) ||                                // *(sib)
         (p[-4] == 0xff && p[-3] == 0x54) ||                 // *disp8(sib)
         (p[-6] == 0xff &&
          (((p[-5] & 0xf8) == 0x90 && (p[-5] & 7) != 4) ||  // *disp32(%reg)
           p[-5] == 0x15)) ||                                // *address
         (p[-7] == 0xff && p[-6] == 0x94);                   // *disp32(sib)
}

// The game's code a thread at |address| was called from: in the game, the
// return address in the current frame, as the game keeps frame pointers;
// elsewhere, the first return address into the game up the stack.
static uint32_t find_caller(uintptr_t address, uintptr_t esp, uintptr_t ebp) {
  uintptr_t words[kStackChunk];
  if (in_game(address)) {
    struct iovec local = { words, 2 * sizeof(uintptr_t) };
    struct iovec remote = { (void*)ebp, 2 * sizeof(uintptr_t) };
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) ==
            (ssize_t)(2 * sizeof(uintptr_t)) &&
        after_call(words[1])) {
      return words[1];
    }
    return 0;
  }
  for (int chunk = 0; chunk < kStackChunks; chunk++) {
    struct iovec local = { words, sizeof(words) };
    struct iovec remote = { (void*)(esp + chunk * sizeof(words)),
                            sizeof(words) };
    ssize_t got = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    for (ssize_t i = 0; (i + 1) * (ssize_t)sizeof(uintptr_t) <= got; i++) {
      if (after_call(words[i])) {
        return words[i];
      }
    }
    if (got < (ssize_t)sizeof(words)) {
      break;
    }
  }
  return 0;
}

static void record(uint32_t address, uint32_t caller, uint32_t thread) {
  if (__sync_lock_test_and_set(&table_busy, 1)) {
    __sync_fetch_and_add(&dropped, 1);
    return;
  }
  uint32_t hash = (address * 2654435761u) ^ (caller * 2246822519u) ^ thread;
  for (int i = 0; i < kProbes; i++) {
    sample_slot* s = &slots[(hash + i) & (kSlots - 1)];
    if (s->count &&
        (s->address != address || s->caller != caller ||
         s->thread != thread)) {
      continue;
    }
    if (!s->count) {
      if (slots_used >= kSlots / 4 * 3) {
        break;
      }
      s->address = address;
      s->caller = caller;
      s->thread = thread;
      slots_used++;
    }
    s->count++;
    samples++;
    __sync_lock_release(&table_busy);
    return;
  }
  __sync_fetch_and_add(&dropped, 1);
  __sync_lock_release(&table_busy);
}

static void on_sample(int signum, siginfo_t* info, void* context) {
  int saved_errno = errno;
  greg_t* r = ((ucontext_t*)context)->uc_mcontext.gregs;
  uintptr_t address = (uintptr_t)r[REG_EIP];
  uint32_t caller =
      find_caller(address, (uintptr_t)r[REG_ESP], (uintptr_t)r[REG_EBP]);
  uint32_t thread = (uint32_t)syscall(SYS_gettid);
  if (slots) {
    record(address, caller, thread);
  }
  if (long_frame_ms > 0 && thread == (uint32_t)pid && !frame_reading &&
      frame_sample_count < kFrameSamples) {
    frame_samples[frame_sample_count].address = address;
    frame_samples[frame_sample_count].caller = caller;
    frame_samples[frame_sample_count].micros = 1000;
    frame_sample_count++;
  }
  errno = saved_errno;
}

static int written;
static void write_profile(void);

void hle_profile_write(void) {
  if (slots && output) {
    write_profile();
  }
}

static void write_profile(void) {
  if (written) {
    return;
  }
  written = 1;
  struct itimerval off;
  memset(&off, 0, sizeof(off));
  setitimer(ITIMER_PROF, &off, NULL);
  // Nothing is recorded after this. A crash in the sampler would leave the
  // table taken, so the wait has a limit.
  for (int tries = 0;
       __sync_lock_test_and_set(&table_busy, 1) && tries < 1000; tries++) {
    sched_yield();
  }
  FILE* f = fopen(output, "w");
  if (!f) {
    fprintf(stderr, "hle: HLE_PROFILE: cannot write %s: %m\n", output);
    return;
  }
  fprintf(f, "# hle profile: %u samples, %u dropped, each a millisecond of "
          "CPU time\n", samples, dropped);
  fprintf(f, "# game code %#lx-%#lx, main thread %d\n",
          (unsigned long)game_lo, (unsigned long)game_hi, (int)pid);
  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
      fprintf(f, "# map %s", line);
    }
    fclose(maps);
  }
  fprintf(f, "# count thread address caller symbol+offset\n");
  for (uint32_t i = 0; i < kSlots; i++) {
    const sample_slot* s = &slots[i];
    if (!s->count) {
      continue;
    }
    Dl_info info;
    const char* symbol = "?";
    unsigned long offset = 0;
    if (!in_game(s->address) && dladdr((void*)(uintptr_t)s->address, &info) &&
        info.dli_sname) {
      symbol = info.dli_sname;
      offset = s->address - (uintptr_t)info.dli_saddr;
    }
    fprintf(f, "%u %u %#x %#x %s+%#lx\n", s->count, s->thread, s->address,
            s->caller, symbol, offset);
  }
  fclose(f);
  fprintf(stderr, "hle: a profile of %u samples is in %s\n", samples, output);
}

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Samples what the game's thread is blocked in, every millisecond.
static void* watch_game_thread(void* unused) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPROF);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", (int)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "hle: HLE_LONG_FRAMES: cannot read %s: %m\n", path);
    return NULL;
  }
  double last = monotonic_seconds();
  for (;;) {
    struct timespec pause = { 0, 1000000 };
    nanosleep(&pause, NULL);
    char text[256];
    ssize_t n = pread(fd, text, sizeof(text) - 1, 0);
    double now = monotonic_seconds();
    uint32_t micros = (uint32_t)((now - last) * 1e6);
    last = now;
    if (n <= 0 || text[0] == 'r') {
      continue;  // "running"
    }
    text[n] = '\0';
    long call;
    unsigned long args[6];
    unsigned long sp;
    unsigned long pc;
    if (sscanf(text, "%ld %lx %lx %lx %lx %lx %lx %lx %lx", &call, &args[0],
               &args[1], &args[2], &args[3], &args[4], &args[5], &sp, &pc) !=
        9) {
      if (sscanf(text, "%ld %lx %lx", &call, &sp, &pc) != 3) {
        continue;
      }
    }
    uint32_t caller = find_caller(pc, sp, 0);
    while (__sync_lock_test_and_set(&blocked_busy, 1)) {
      sched_yield();
    }
    if (blocked_sample_count < kFrameSamples) {
      frame_sample* s = &blocked_samples[blocked_sample_count++];
      s->address = (uint32_t)call;
      s->caller = caller;
      s->micros = micros;
    }
    __sync_lock_release(&blocked_busy);
  }
  return NULL;
}

void hle_profile_start(void* game_code) {
  static int started;
  const char* path = getenv("HLE_PROFILE");
  const char* long_frames = getenv("HLE_LONG_FRAMES");
  int profile = path && *path;
  if (started || (!profile && !(long_frames && atof(long_frames) > 0))) {
    return;
  }
  started = 1;
  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
      unsigned long lo, hi;
      if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
          (uintptr_t)game_code >= lo && (uintptr_t)game_code < hi) {
        game_lo = lo;
        game_hi = hi;
      }
    }
    fclose(maps);
  }
  sample_slot* table = profile ? mmap(NULL, kSlots * sizeof(*table),
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                              : NULL;
  if (!game_hi || table == MAP_FAILED) {
    fprintf(stderr, "hle: HLE_PROFILE: the game's code was not found\n");
    return;
  }
  pid = getpid();
  long_frame_ms = long_frames ? atof(long_frames) : 0;
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_sample;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGPROF, &action, NULL);
  if (profile) {
    output = strdup(path);
    slots = table;
    atexit(write_profile);
  }
  struct itimerval every = { { 0, 1000 }, { 0, 1000 } };
  setitimer(ITIMER_PROF, &every, NULL);
  if (profile) {
    fprintf(stderr, "hle: profiling the game's code at %#lx-%#lx into %s\n",
            (unsigned long)game_lo, (unsigned long)game_hi, output);
  }
  if (long_frame_ms > 0) {
    pthread_t watcher;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&watcher, &attributes, watch_game_thread, NULL) != 0) {
      fprintf(stderr, "hle: HLE_LONG_FRAMES: no thread to watch with\n");
    }
    pthread_attr_destroy(&attributes);
    fprintf(stderr, "hle: reporting frames of %g ms or more\n",
            long_frame_ms);
  }
}

// Names |address| into |out|: the game's code, or a library and symbol.
static void name_address(uint32_t address, char* out, size_t size) {
  Dl_info info;
  if (in_game(address)) {
    snprintf(out, size, "game %#x", address);
  } else if (dladdr((void*)(uintptr_t)address, &info) && info.dli_fname) {
    const char* file = strrchr(info.dli_fname, '/');
    file = file ? file + 1 : info.dli_fname;
    if (info.dli_sname) {
      snprintf(out, size, "%s %s+%#lx", file, info.dli_sname,
               (unsigned long)(address - (uintptr_t)info.dli_saddr));
    } else {
      snprintf(out, size, "%s %#x", *file ? file : "?", address);
    }
  } else {
    snprintf(out, size, "%#x (no library)", address);
  }
}

typedef struct {
  uint32_t caller;
  uint32_t address;  // the first sample's, to name where the time went
  uint32_t count;
} frame_place;

static const char* system_call_name(uint32_t call) {
  switch ((int32_t)call) {
    case -1: return "no system call (a page fault?)";
    case 3: return "read";
    case 4: return "write";
    case 5: return "open";
    case 54: return "ioctl";
    case 91: return "munmap";
    case 142: return "select";
    case 158: return "sched_yield";
    case 162: return "nanosleep";
    case 168: return "poll";
    case 180: return "pread64";
    case 192: return "mmap2";
    case 240: return "futex";
    case 265: return "clock_gettime";
    case 267: return "clock_nanosleep";
    case 407: return "clock_nanosleep_time64";
    case 414: return "ppoll_time64";
    case 422: return "futex_time64";
  }
  return NULL;
}

// Appends |samples| grouped by the game's code they were called from, the
// groups with the most time first, to |line|.
static size_t append_places(char* line, size_t size, size_t n,
                            const frame_sample* samples, uint32_t count,
                            int calls) {
  frame_place places[kFramePlaces];
  uint32_t place_count = 0;
  uint32_t unplaced = 0;
  for (uint32_t i = 0; i < count; i++) {
    const frame_sample* s = &samples[i];
    uint32_t p = 0;
    while (p < place_count &&
           (places[p].caller != s->caller ||
            (calls && places[p].address != s->address))) {
      p++;
    }
    if (p == place_count) {
      if (place_count == kFramePlaces) {
        unplaced += s->micros;
        continue;
      }
      places[p].caller = s->caller;
      places[p].address = s->address;
      places[p].count = 0;
      place_count++;
    }
    places[p].count += s->micros;
  }
  for (int shown = 0; shown < kPlacesShown && n < size; shown++) {
    uint32_t best = place_count;
    for (uint32_t p = 0; p < place_count; p++) {
      if (places[p].count &&
          (best == place_count || places[p].count > places[best].count)) {
        best = p;
      }
    }
    if (best == place_count) {
      break;
    }
    char where[256];
    if (calls) {
      const char* name = system_call_name(places[best].address);
      if (name) {
        snprintf(where, sizeof(where), "%s", name);
      } else {
        snprintf(where, sizeof(where), "system call %d",
                 (int)places[best].address);
      }
    } else {
      name_address(places[best].address, where, sizeof(where));
    }
    if (places[best].caller) {
      n += (size_t)snprintf(line + n, size - n, "%s %.0f in %s from game %#x",
                            shown ? ";" : ":", places[best].count / 1000.0,
                            where, places[best].caller);
    } else {
      n += (size_t)snprintf(line + n, size - n, "%s %.0f in %s",
                            shown ? ";" : ":", places[best].count / 1000.0,
                            where);
    }
    places[best].count = 0;
  }
  if (unplaced && n < size) {
    n += (size_t)snprintf(line + n, size - n, "; %.0f elsewhere",
                          unplaced / 1000.0);
  }
  return n < size ? n : size;
}

static void report_long_frame(unsigned frame, double wall_ms, double cpu_ms,
                              double waiting_ms, double swap_ms,
                              uint32_t count, const frame_sample* blocked,
                              uint32_t blocked_count) {
  char line[2048];
  size_t n = (size_t)snprintf(
      line, sizeof(line),
      "hle: long frame %u: %.0f ms, %.0f of them on this thread's CPU, %.0f "
      "ready to run but waiting for a CPU, and %.1f in the swap; CPU samples "
      "in ms", frame, wall_ms, cpu_ms, waiting_ms, swap_ms);
  n = append_places(line, sizeof(line), n, frame_samples, count, 0);
  double blocked_ms = 0;
  for (uint32_t i = 0; i < blocked_count; i++) {
    blocked_ms += blocked[i].micros / 1000.0;
  }
  if (n < sizeof(line)) {
    n += (size_t)snprintf(line + n, sizeof(line) - n, "; blocked %.0f ms",
                          blocked_ms);
  }
  n = append_places(line, sizeof(line), n, blocked, blocked_count, 1);
  fprintf(stderr, "%s\n", line);
}

// The game's thread's time on a CPU and waiting in a run queue, in seconds,
// as the scheduler counts them.
static int read_schedstat(int fd, double* running, double* waiting) {
  char text[128];
  ssize_t n = fd >= 0 ? pread(fd, text, sizeof(text) - 1, 0) : -1;
  unsigned long long on_cpu;
  unsigned long long queued;
  if (n <= 0) {
    return 0;
  }
  text[n] = '\0';
  if (sscanf(text, "%llu %llu", &on_cpu, &queued) != 2) {
    return 0;
  }
  *running = on_cpu / 1e9;
  *waiting = queued / 1e9;
  return 1;
}

void hle_profile_frame(unsigned frame, double wall_ms, double swap_ms) {
  static double cpu_at;
  static double waiting_at;
  static int schedstat = -2;
  if (long_frame_ms <= 0) {
    return;
  }
  if (schedstat == -2) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", (int)pid);
    schedstat = open(path, O_RDONLY | O_CLOEXEC);
  }
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  double cpu = ts.tv_sec + ts.tv_nsec / 1e9;
  double running = 0;
  double waiting = 0;
  read_schedstat(schedstat, &running, &waiting);
  static frame_sample blocked[kFrameSamples];
  while (__sync_lock_test_and_set(&blocked_busy, 1)) {
    sched_yield();
  }
  uint32_t blocked_count = blocked_sample_count;
  memcpy(blocked, blocked_samples, blocked_count * sizeof(*blocked));
  blocked_sample_count = 0;
  __sync_lock_release(&blocked_busy);
  frame_reading = 1;
  if (wall_ms >= long_frame_ms && cpu_at > 0) {
    report_long_frame(frame, wall_ms, (cpu - cpu_at) * 1000,
                      (waiting - waiting_at) * 1000, swap_ms,
                      (uint32_t)frame_sample_count, blocked, blocked_count);
  }
  frame_sample_count = 0;
  frame_reading = 0;
  cpu_at = cpu;
  waiting_at = waiting;
}

void hle_profile_thread(void) {
  static __thread int unblocked;
  if (!slots || unblocked) {
    return;
  }
  unblocked = 1;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPROF);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

#else

void hle_profile_start(void* game_code) {
  if (getenv("HLE_PROFILE") || getenv("HLE_LONG_FRAMES")) {
    fprintf(stderr, "hle: HLE_PROFILE samples i386 code only\n");
  }
}

void hle_profile_frame(unsigned frame, double wall_ms, double swap_ms) {
}

void hle_profile_write(void) {
}

void hle_profile_thread(void) {
}

#endif
