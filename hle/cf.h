// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// CoreFoundation as a 10.4-era Mac OS X i386 executable sees it: the calls
// Halo's Mac build imports, not a general CF. Darwin's i386 ABI applies to
// every exported function:
//
// - long is 32 bits, so CFIndex and CFTypeID are too.
// - A Boolean or UniChar result must fill all of EAX, so exported functions
//   return int or unsigned int where CF's prototype says Boolean or UniChar.
// - A struct returned in memory has its hidden pointer popped by the callee
//   (ret $4), as GCC does on Linux, so such functions return it by value.
//   Structs of 1, 2, 4 or 8 bytes come back in EAX:EDX instead, so those
//   return an integer of the same size.

#ifndef HLE_CF_H_
#define HLE_CF_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned char Boolean;
typedef uint8_t UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef uint16_t UniChar;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef unsigned long CFOptionFlags;
typedef unsigned long CFHashCode;
typedef uint32_t CFStringEncoding;

typedef struct {
  CFIndex location;
  CFIndex length;
} CFRange;

// Darwin's CFRuntimeBase on i386. The compiler-emitted constant strings in
// __DATA,__cfstring start with it too:
//   { &__CFConstantStringClassReference, 0x7c8, const char* bytes, long length }
typedef struct {
  const void* isa;
  uint32_t info;
} CFRuntimeBase;

typedef const void* CFTypeRef;
typedef const struct __CFString* CFStringRef;
typedef struct __CFString* CFMutableStringRef;
typedef const struct __CFArray* CFArrayRef;
typedef struct __CFArray* CFMutableArrayRef;
typedef const struct __CFDictionary* CFDictionaryRef;
typedef struct __CFDictionary* CFMutableDictionaryRef;
typedef const struct __CFNumber* CFNumberRef;
typedef const struct __CFBoolean* CFBooleanRef;
typedef const struct __CFData* CFDataRef;
typedef const struct __CFURL* CFURLRef;
typedef struct __CFBundle* CFBundleRef;
typedef const struct __CFAllocator* CFAllocatorRef;
typedef const struct __CFLocale* CFLocaleRef;
typedef struct __CFNumberFormatter* CFNumberFormatterRef;
typedef struct __CFRunLoop* CFRunLoopRef;
typedef struct __CFRunLoopSource* CFRunLoopSourceRef;
typedef const struct __CFUUID* CFUUIDRef;
typedef const struct __CFCharacterSet* CFCharacterSetRef;
typedef CFTypeRef CFPropertyListRef;

typedef struct {
  UInt8 bytes[16];
} CFUUIDBytes;

enum {
  kCFStringEncodingMacRoman = 0,
  kCFStringEncodingUnicode = 0x0100,
  kCFStringEncodingISOLatin1 = 0x0201,
  kCFStringEncodingWindowsLatin1 = 0x0500,
  kCFStringEncodingASCII = 0x0600,
  kCFStringEncodingNextStepLatin = 0x0B01,
  kCFStringEncodingNonLossyASCII = 0x0BFF,
  kCFStringEncodingUTF8 = 0x08000100,
  kCFStringEncodingUTF16BE = 0x10000100,
  kCFStringEncodingUTF16LE = 0x14000100,
};

enum {
  kCFURLPOSIXPathStyle = 0,
  kCFURLHFSPathStyle = 1,
};

// Type IDs. The game only compares them against *GetTypeID() results.
enum {
  CF_TYPE_ALLOCATOR = 1,
  CF_TYPE_STRING,
  CF_TYPE_ARRAY,
  CF_TYPE_DICTIONARY,
  CF_TYPE_NUMBER,
  CF_TYPE_BOOLEAN,
  CF_TYPE_DATA,
  CF_TYPE_URL,
  CF_TYPE_BUNDLE,
  CF_TYPE_LOCALE,
  CF_TYPE_NUMBER_FORMATTER,
  CF_TYPE_RUN_LOOP,
  CF_TYPE_RUN_LOOP_SOURCE,
  CF_TYPE_UUID,
  CF_TYPE_CHARACTER_SET,
};

// A growable UTF-8 buffer, always NUL-terminated once anything is appended.
typedef struct cf_buf {
  char* data;
  size_t len;
  size_t cap;
} cf_buf;

void cf_buf_append(cf_buf* b, const char* s, size_t n);
void cf_buf_appends(cf_buf* b, const char* s);
void cf_buf_appendf(cf_buf* b, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
void cf_buf_free(cf_buf* b);

// What an object's isa points at, for every object this library allocates.
typedef struct cf_class {
  CFTypeID type_id;
  const char* name;
  void (*finalize)(CFTypeRef obj);
  // Called only for two objects of the same class.
  int (*equal)(CFTypeRef a, CFTypeRef b);
  CFHashCode (*hash)(CFTypeRef obj);
  // Appends a UTF-8 description, for %@ and CFShow.
  void (*describe)(CFTypeRef obj, cf_buf* out);
} cf_class;

// The header of every allocated object. Constant strings have no refcount
// and are never freed.
typedef struct {
  CFRuntimeBase base;
  int32_t rc;
} CFObject;

extern int __CFConstantStringClassReference[12];

void* cf_alloc(const cf_class* cls, size_t size);
const cf_class* cf_class_of(CFTypeRef obj);
int cf_is(CFTypeRef obj, CFTypeID type_id);
int cf_equal(CFTypeRef a, CFTypeRef b);
CFHashCode cf_hash(CFTypeRef obj);
void cf_describe(CFTypeRef obj, cf_buf* out);

// Tracing, on when HLE_TRACE is set in the environment.
extern int cf_trace_enabled;
void cf_trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Says once, on stderr, that a call took a path not implemented yet.
void cf_warn_once(const char* what);

CFTypeRef CFRetain(CFTypeRef obj);
void CFRelease(CFTypeRef obj);
CFTypeID CFGetTypeID(CFTypeRef obj);
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFStringGetTypeID(void);
CFTypeID CFDictionaryGetTypeID(void);
CFTypeID CFArrayGetTypeID(void);

// Public CF used between the files of this library.
extern const CFAllocatorRef kCFAllocatorNull;
extern const CFStringRef kCFPreferencesCurrentApplication;
CFStringRef CFStringCreateWithCharacters(CFAllocatorRef allocator,
                                         const UniChar* chars, CFIndex length);
CFStringRef CFStringCreateWithCString(CFAllocatorRef allocator, const char* str,
                                      CFStringEncoding encoding);
CFStringRef CFStringCreateWithBytes(CFAllocatorRef allocator,
                                    const UInt8* bytes, CFIndex length,
                                    CFStringEncoding encoding,
                                    unsigned int external_representation);
CFIndex CFStringGetLength(CFStringRef str);
CFStringRef CFStringCreateWithSubstring(CFAllocatorRef allocator,
                                        CFStringRef str, CFRange range);
CFIndex CFStringGetBytes(CFStringRef str, CFRange range,
                         CFStringEncoding encoding, unsigned int loss_byte,
                         unsigned int external_representation, UInt8* buffer,
                         CFIndex max_length, CFIndex* used_length);
void CFStringGetCharacters(CFStringRef str, CFRange range, UniChar* buffer);
CFBundleRef CFBundleGetMainBundle(void);
CFStringRef CFBundleGetIdentifier(CFBundleRef bundle);

// Strings.
extern const cf_class cf_string_class;
CFIndex cf_string_length(CFStringRef s);
UniChar cf_string_char(CFStringRef s, CFIndex i);
// A malloc'd, NUL-terminated UTF-8 copy.
char* cf_string_utf8(CFStringRef s);
CFStringRef cf_string_from_utf8(const char* s, size_t n);
int cf_string_is_ascii(CFStringRef s, const char* ascii);

// Collections, for the property-list, bundle and preferences code.
CFMutableArrayRef cf_array_create(void);
void cf_array_append(CFMutableArrayRef array, CFTypeRef value);
CFIndex cf_array_count(CFArrayRef array);
CFTypeRef cf_array_get(CFArrayRef array, CFIndex i);
CFMutableDictionaryRef cf_dict_create(void);
void cf_dict_set(CFMutableDictionaryRef dict, CFTypeRef key, CFTypeRef value);
CFTypeRef cf_dict_get(CFDictionaryRef dict, CFTypeRef key);
CFTypeRef cf_dict_get_ascii(CFDictionaryRef dict, const char* key);
void cf_dict_remove(CFMutableDictionaryRef dict, CFTypeRef key);
CFIndex cf_dict_count(CFDictionaryRef dict);
void cf_dict_pair(CFDictionaryRef dict, CFIndex i, CFTypeRef* key,
                  CFTypeRef* value);

// Scalars.
extern const CFBooleanRef kCFBooleanTrue;
extern const CFBooleanRef kCFBooleanFalse;
CFNumberRef cf_number_int(int64_t value);
CFNumberRef cf_number_double(double value);
int cf_number_is_float(CFNumberRef number);
int64_t cf_number_as_int(CFNumberRef number);
double cf_number_as_double(CFNumberRef number);
CFDataRef cf_data_create(const UInt8* bytes, CFIndex length);
const UInt8* cf_data_bytes(CFDataRef data);
CFIndex cf_data_length(CFDataRef data);

// XML property lists. Parsed containers are mutable.
CFPropertyListRef cf_plist_parse(const char* xml, size_t len,
                                 const char** error);
void cf_plist_write(CFPropertyListRef plist, cf_buf* out);
CFPropertyListRef cf_plist_read_file(const char* path);
int cf_plist_write_file(CFPropertyListRef plist, const char* path);

// Files. Mac OS X volumes are case-insensitive, and the game's names do not
// always match the case on disk. Resolves each component of |path| against
// the directory it is in; false when something does not exist.
int hle_path_resolve(const char* path, char* out, size_t out_size);
// The directory that stands in for the Mac home folder: Library/Preferences
// and the rest live under it. $HALO_MAC_HOME, else
// $XDG_DATA_HOME/halo-mac-loader/home, else ~/.local/share/...
const char* hle_mac_home(void);
int hle_mkdirs(const char* dir);

// An FSRef is 80 opaque bytes. Here it carries a tag and an index into a
// table of resolved paths, shared by CFURL and the File Manager.
// hle_fsref_make fails (0) when |path| does not exist, as FSRefs must name
// existing objects. hle_fsref_path is NULL for bytes this code did not make.
int hle_fsref_make(const char* path, UInt8* fsref);
const char* hle_fsref_path(const UInt8* fsref);

// The executable's path, set by ld-mac.
extern char __darwin_executable_path[];

// Set by ld-mac: resolves a Darwin symbol name, without its leading
// underscore, the way an import is bound. NULL when nothing implements it.
extern void* (*ld_mac_resolve)(const char* name);

#endif  // HLE_CF_H_
