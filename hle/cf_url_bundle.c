// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Paths, CFURL and CFBundle: how the game finds Info.plist, its resources,
// its localized strings, and the system frameworks it looks functions up in.

#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cf.h"

void* (*ld_mac_resolve)(const char* name);

// ---------------------------------------------------------------------------
// Paths

int hle_path_resolve(const char* path, char* out, size_t out_size) {
  if (!path || !*path) {
    return 0;
  }
  if (access(path, F_OK) == 0) {
    if (strlen(path) + 1 > out_size) {
      return 0;
    }
    strcpy(out, path);
    return 1;
  }
  char current[PATH_MAX];
  size_t len = 0;
  const char* p = path;
  if (*p == '/') {
    current[len++] = '/';
    while (*p == '/') {
      p++;
    }
  }
  current[len] = '\0';
  while (*p) {
    const char* slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (len + n + 2 > sizeof(current)) {
      return 0;
    }
    char component[NAME_MAX + 1];
    if (n > NAME_MAX) {
      return 0;
    }
    memcpy(component, p, n);
    component[n] = '\0';
    size_t base = len;
    if (len && current[len - 1] != '/') {
      current[len++] = '/';
    }
    memcpy(current + len, component, n + 1);
    if (access(current, F_OK) != 0) {
      current[base] = '\0';
      DIR* dir = opendir(base ? current : ".");
      int found = 0;
      if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir))) {
          if (!strcasecmp(entry->d_name, component)) {
            memcpy(component, entry->d_name, n + 1);
            found = 1;
            break;
          }
        }
        closedir(dir);
      }
      if (!found) {
        return 0;
      }
      len = base;
      if (len && current[len - 1] != '/') {
        current[len++] = '/';
      }
      memcpy(current + len, component, n + 1);
    }
    len += n;
    p += n;
    while (*p == '/') {
      p++;
    }
  }
  if (len + 1 > out_size) {
    return 0;
  }
  memcpy(out, current, len + 1);
  return 1;
}

int hle_mkdirs(const char* dir) {
  char tmp[PATH_MAX];
  if (strlen(dir) + 1 > sizeof(tmp)) {
    return 0;
  }
  strcpy(tmp, dir);
  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return 0;
      }
      *p = '/';
    }
  }
  return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static char mac_home[PATH_MAX];
static pthread_once_t mac_home_once = PTHREAD_ONCE_INIT;

static void init_mac_home(void) {
  const char* explicit_home = getenv("HALO_MAC_HOME");
  const char* xdg = getenv("XDG_DATA_HOME");
  const char* home = getenv("HOME");
  if (explicit_home && *explicit_home) {
    snprintf(mac_home, sizeof(mac_home), "%s", explicit_home);
  } else if (xdg && *xdg) {
    snprintf(mac_home, sizeof(mac_home), "%s/halo-mac-loader/home", xdg);
  } else {
    snprintf(mac_home, sizeof(mac_home), "%s/.local/share/halo-mac-loader/home",
             home ? home : "/tmp");
  }
  hle_mkdirs(mac_home);
}

const char* hle_mac_home(void) {
  pthread_once(&mac_home_once, init_mac_home);
  return mac_home;
}

// ---------------------------------------------------------------------------
// FSRefs

typedef struct {
  char tag[4];
  uint32_t index;
} fsref_header;

static pthread_mutex_t fsref_lock = PTHREAD_MUTEX_INITIALIZER;
static char** fsref_paths;
static uint32_t fsref_count;
static uint32_t fsref_capacity;

int hle_fsref_make(const char* path, UInt8* fsref) {
  char resolved[PATH_MAX];
  if (!hle_path_resolve(path, resolved, sizeof(resolved))) {
    return 0;
  }
  char absolute[PATH_MAX];
  if (!realpath(resolved, absolute)) {
    return 0;
  }
  pthread_mutex_lock(&fsref_lock);
  uint32_t i;
  for (i = 0; i < fsref_count; i++) {
    if (!strcmp(fsref_paths[i], absolute)) {
      break;
    }
  }
  if (i == fsref_count) {
    if (fsref_count == fsref_capacity) {
      fsref_capacity = fsref_capacity ? fsref_capacity * 2 : 64;
      fsref_paths = realloc(fsref_paths, sizeof(char*) * fsref_capacity);
    }
    fsref_paths[fsref_count++] = strdup(absolute);
  }
  pthread_mutex_unlock(&fsref_lock);
  memset(fsref, 0, 80);
  fsref_header header = { { 'h', 'l', 'e', 'F' }, i };
  memcpy(fsref, &header, sizeof(header));
  return 1;
}

const char* hle_fsref_path(const UInt8* fsref) {
  fsref_header header;
  memcpy(&header, fsref, sizeof(header));
  if (memcmp(header.tag, "hleF", 4)) {
    return NULL;
  }
  pthread_mutex_lock(&fsref_lock);
  const char* path = header.index < fsref_count ? fsref_paths[header.index]
                                                : NULL;
  pthread_mutex_unlock(&fsref_lock);
  return path;
}

// ---------------------------------------------------------------------------
// CFURL: file URLs only, held as a POSIX path.

struct __CFURL {
  CFObject obj;
  char* path;
  int is_directory;
};

static void url_finalize(CFTypeRef obj) {
  free(((struct __CFURL*)obj)->path);
}

static int url_equal(CFTypeRef a, CFTypeRef b) {
  return !strcmp(((const struct __CFURL*)a)->path,
                 ((const struct __CFURL*)b)->path);
}

static void url_describe(CFTypeRef obj, cf_buf* out) {
  const struct __CFURL* url = obj;
  cf_buf_appendf(out, "file://localhost%s%s", url->path,
                 url->is_directory && strcmp(url->path, "/") ? "/" : "");
}

static const cf_class cf_url_class = {
  CF_TYPE_URL, "CFURL", url_finalize, url_equal, NULL, url_describe,
};

static CFURLRef url_new(const char* path, size_t n, int is_directory) {
  struct __CFURL* url = cf_alloc(&cf_url_class, sizeof(*url));
  while (n > 1 && path[n - 1] == '/') {
    n--;
  }
  url->path = strndup(path, n);
  url->is_directory = is_directory;
  return url;
}

CFTypeID CFURLGetTypeID(void) {
  return CF_TYPE_URL;
}

CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef allocator,
                                       CFStringRef path, CFIndex style,
                                       unsigned int is_directory) {
  char* utf8 = cf_string_utf8(path);
  CFURLRef url;
  if ((style & 0xff) == kCFURLHFSPathStyle) {
    // "Volume:dir:file". Every volume is the root here; '/' inside an HFS
    // name is ':' on disk.
    const char* rest = strchr(utf8, ':');
    cf_buf posix = { 0 };
    cf_buf_appends(&posix, "/");
    for (const char* p = rest ? rest + 1 : ""; *p; p++) {
      cf_buf_append(&posix, *p == ':' ? "/" : *p == '/' ? ":" : p, 1);
    }
    url = url_new(posix.data, posix.len, is_directory & 0xff);
    cf_buf_free(&posix);
  } else {
    url = url_new(utf8, strlen(utf8), is_directory & 0xff);
  }
  free(utf8);
  return url;
}

CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef allocator,
                                                 const UInt8* buffer,
                                                 CFIndex length,
                                                 unsigned int is_directory) {
  return url_new((const char*)buffer, strnlen((const char*)buffer, length),
                 is_directory & 0xff);
}

int CFURLGetFileSystemRepresentation(CFURLRef url, unsigned int resolve,
                                     UInt8* buffer, CFIndex size) {
  size_t n = strlen(url->path);
  if ((CFIndex)n + 1 > size) {
    return 0;
  }
  memcpy(buffer, url->path, n + 1);
  return 1;
}

CFStringRef CFURLCopyFileSystemPath(CFURLRef url, CFIndex style) {
  return cf_string_from_utf8(url->path, strlen(url->path));
}

CFStringRef CFURLCopyLastPathComponent(CFURLRef url) {
  const char* slash = strrchr(url->path, '/');
  const char* last = slash && slash[1] ? slash + 1 : url->path;
  return cf_string_from_utf8(last, strlen(last));
}

CFURLRef CFURLCreateCopyAppendingPathComponent(CFAllocatorRef allocator,
                                               CFURLRef url,
                                               CFStringRef component,
                                               unsigned int is_directory) {
  char* utf8 = cf_string_utf8(component);
  cf_buf path = { 0 };
  cf_buf_appends(&path, url->path);
  if (path.len && path.data[path.len - 1] != '/') {
    cf_buf_appends(&path, "/");
  }
  cf_buf_appends(&path, utf8);
  CFURLRef result = url_new(path.data, path.len, is_directory & 0xff);
  cf_buf_free(&path);
  free(utf8);
  return result;
}

CFURLRef CFURLCreateCopyDeletingLastPathComponent(CFAllocatorRef allocator,
                                                  CFURLRef url) {
  const char* slash = strrchr(url->path, '/');
  if (!slash) {
    return url_new(".", 1, 1);
  }
  if (slash == url->path) {
    return url_new("/", 1, 1);
  }
  return url_new(url->path, slash - url->path, 1);
}

enum {
  kCFURLResourceNotFoundError = -12,
  kCFURLResourceAccessViolationError = -13,
};

int CFURLCreateDataAndPropertiesFromResource(CFAllocatorRef allocator,
                                             CFURLRef url, CFDataRef* data,
                                             CFDictionaryRef* properties,
                                             CFArrayRef desired,
                                             SInt32* error_code) {
  if (properties) {
    *properties = NULL;
    if (desired) {
      cf_warn_once("CFURLCreateDataAndPropertiesFromResource properties");
    }
  }
  char resolved[PATH_MAX];
  if (!hle_path_resolve(url->path, resolved, sizeof(resolved))) {
    if (error_code) {
      *error_code = kCFURLResourceNotFoundError;
    }
    return 0;
  }
  if (data) {
    FILE* f = fopen(resolved, "rb");
    if (!f) {
      if (error_code) {
        *error_code = kCFURLResourceAccessViolationError;
      }
      return 0;
    }
    cf_buf contents = { 0 };
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
      cf_buf_append(&contents, chunk, n);
    }
    fclose(f);
    *data = cf_data_create((const UInt8*)contents.data, contents.len);
    cf_buf_free(&contents);
  }
  if (error_code) {
    *error_code = 0;
  }
  return 1;
}

int CFURLGetFSRef(CFURLRef url, UInt8* fsref) {
  return hle_fsref_make(url->path, fsref);
}

CFURLRef CFURLCreateFromFSRef(CFAllocatorRef allocator, const UInt8* fsref) {
  const char* path = hle_fsref_path(fsref);
  if (!path) {
    return NULL;
  }
  struct stat st;
  int is_directory = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
  return url_new(path, strlen(path), is_directory);
}

// ---------------------------------------------------------------------------
// CFBundle

struct __CFBundle {
  CFObject obj;
  char* path;
  // A Mac OS X system framework: nothing on disk, functions come from the
  // loader's import resolver.
  int is_system_framework;
  CFDictionaryRef info;
  // Parsed .strings tables, by table name.
  CFMutableDictionaryRef tables;
};

static void bundle_describe(CFTypeRef obj, cf_buf* out) {
  cf_buf_appendf(out, "CFBundle %s", ((const struct __CFBundle*)obj)->path);
}

static const cf_class cf_bundle_class = {
  CF_TYPE_BUNDLE, "CFBundle", NULL, NULL, NULL, bundle_describe,
};

static pthread_mutex_t bundle_lock = PTHREAD_MUTEX_INITIALIZER;

CFTypeID CFBundleGetTypeID(void) {
  return CF_TYPE_BUNDLE;
}

static struct __CFBundle* bundle_new(const char* path, int system_framework) {
  struct __CFBundle* b = cf_alloc(&cf_bundle_class, sizeof(*b));
  b->obj.rc = -0x40000000;  // bundles live as long as the process, as in CF
  b->path = strdup(path);
  b->is_system_framework = system_framework;
  return b;
}

CFBundleRef CFBundleGetMainBundle(void) {
  static struct __CFBundle* main_bundle;
  pthread_mutex_lock(&bundle_lock);
  if (!main_bundle) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", __darwin_executable_path);
    char* marker = strstr(path, "/Contents/MacOS/");
    if (marker) {
      *marker = '\0';
    } else {
      char* slash = strrchr(path, '/');
      if (slash) {
        *slash = '\0';
      }
    }
    main_bundle = bundle_new(path, 0);
  }
  pthread_mutex_unlock(&bundle_lock);
  return main_bundle;
}

// Frameworks every Mac has. OpenAL and MiniAL are optional installs and stay
// absent until audio is wired up, so the game does not call into them yet.
static const char* const kSystemFrameworks[] = {
  "System", "ApplicationServices", "OpenGL", "AGL", "AudioUnit",
  "QuickTime", "Carbon", "CoreFoundation", "CoreServices", "IOKit",
};

CFBundleRef CFBundleCreate(CFAllocatorRef allocator, CFURLRef url) {
  const char* slash = strrchr(url->path, '/');
  const char* name = slash ? slash + 1 : url->path;
  const char* dot = strrchr(name, '.');
  if (dot && !strcasecmp(dot, ".framework")) {
    for (size_t i = 0;
         i < sizeof(kSystemFrameworks) / sizeof(kSystemFrameworks[0]); i++) {
      size_t n = strlen(kSystemFrameworks[i]);
      if ((size_t)(dot - name) == n &&
          !strncasecmp(name, kSystemFrameworks[i], n)) {
        return bundle_new(url->path, 1);
      }
    }
  }
  char resolved[PATH_MAX];
  struct stat st;
  if (!hle_path_resolve(url->path, resolved, sizeof(resolved)) ||
      stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
    cf_trace("CFBundleCreate: no bundle at %s", url->path);
    return NULL;
  }
  return bundle_new(resolved, 0);
}

CFURLRef CFBundleCopyBundleURL(CFBundleRef bundle) {
  return url_new(bundle->path, strlen(bundle->path), 1);
}

int CFBundleLoadExecutable(CFBundleRef bundle) {
  if (bundle->is_system_framework) {
    return 1;
  }
  cf_warn_once("CFBundleLoadExecutable for a bundle with its own code");
  return 0;
}

int CFBundleIsExecutableLoaded(CFBundleRef bundle) {
  return bundle->is_system_framework;
}

void* CFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef name) {
  char* cname = cf_string_utf8(name);
  void* function = NULL;
  if (bundle->is_system_framework) {
    function = ld_mac_resolve ? ld_mac_resolve(cname)
                              : dlsym(RTLD_DEFAULT, cname);
  }
  if (!function) {
    // The game may take another path when a lookup fails, so say so.
    fprintf(stderr, "hle: %s has no function %s\n", bundle->path, cname);
  }
  cf_trace("CFBundleGetFunctionPointerForName(%s, %s) = %p", bundle->path,
           cname, function);
  free(cname);
  return function;
}

// Contents/Resources for a bundle, the bundle itself for a flat one.
static void resources_dir(CFBundleRef bundle, char* out, size_t size) {
  char contents[PATH_MAX];
  out[0] = '\0';
  int n = snprintf(contents, sizeof(contents), "%s/Contents", bundle->path);
  if (n < 0 || (size_t)n >= sizeof(contents)) {
    return;
  }
  struct stat st;
  if (stat(contents, &st) == 0 && S_ISDIR(st.st_mode)) {
    n = snprintf(out, size, "%s/Resources", contents);
  } else {
    n = snprintf(out, size, "%s", bundle->path);
  }
  if (n < 0 || (size_t)n >= size) {
    out[0] = '\0';
  }
}

// Finds |file| under Resources, then its English localization.
static int find_resource(CFBundleRef bundle, const char* subdir,
                         const char* file, char* out, size_t size) {
  static const char* const kLocalizations[] = { "", "English.lproj/",
                                                "en.lproj/" };
  char resources[PATH_MAX];
  resources_dir(bundle, resources, sizeof(resources));
  for (size_t i = 0; i < 3; i++) {
    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/%s%s%s%s", resources,
                     kLocalizations[i], subdir ? subdir : "",
                     subdir ? "/" : "", file);
    if (n > 0 && (size_t)n < sizeof(candidate) &&
        hle_path_resolve(candidate, out, size)) {
      return 1;
    }
  }
  return 0;
}

CFURLRef CFBundleCopyResourceURL(CFBundleRef bundle, CFStringRef name,
                                 CFStringRef type, CFStringRef subdir) {
  if (bundle->is_system_framework || !name) {
    return NULL;
  }
  char* cname = cf_string_utf8(name);
  char* ctype = type && cf_string_length(type) ? cf_string_utf8(type) : NULL;
  char* csubdir = subdir && cf_string_length(subdir) ? cf_string_utf8(subdir)
                                                     : NULL;
  char file[NAME_MAX + 1];
  snprintf(file, sizeof(file), "%s%s%s", cname, ctype ? "." : "",
           ctype ? ctype : "");
  char found[PATH_MAX];
  CFURLRef url = NULL;
  if (find_resource(bundle, csubdir, file, found, sizeof(found))) {
    struct stat st;
    url = url_new(found, strlen(found),
                  stat(found, &st) == 0 && S_ISDIR(st.st_mode));
  }
  cf_trace("CFBundleCopyResourceURL(%s, %s) = %s", file,
           csubdir ? csubdir : "", url ? found : "(none)");
  free(cname);
  free(ctype);
  free(csubdir);
  return url;
}

// .strings files: "key" = "value"; with C escapes and comments, in UTF-16
// (with a byte-order mark) or UTF-8.
static CFMutableDictionaryRef parse_strings(const UInt8* bytes, size_t n) {
  size_t count = 0;
  UniChar* chars = malloc(sizeof(UniChar) * (n + 1));
  if (n >= 2 && ((bytes[0] == 0xFE && bytes[1] == 0xFF) ||
                 (bytes[0] == 0xFF && bytes[1] == 0xFE))) {
    int big_endian = bytes[0] == 0xFE;
    for (size_t i = 2; i + 1 < n; i += 2) {
      chars[count++] = big_endian ? (bytes[i] << 8) | bytes[i + 1]
                                  : bytes[i] | (bytes[i + 1] << 8);
    }
  } else {
    CFStringRef s = cf_string_from_utf8((const char*)bytes, n);
    count = cf_string_length(s);
    for (size_t i = 0; i < count; i++) {
      chars[i] = cf_string_char(s, i);
    }
    CFRelease(s);
  }

  CFMutableDictionaryRef table = cf_dict_create();
  size_t i = 0;
  CFStringRef pending_key = NULL;
  int expect = 0;  // 0: key, 1: '=' or ';', 2: value, 3: ';'
  while (i < count) {
    UniChar c = chars[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xFEFF) {
      i++;
    } else if (c == '/' && i + 1 < count && chars[i + 1] == '*') {
      i += 2;
      while (i + 1 < count && !(chars[i] == '*' && chars[i + 1] == '/')) {
        i++;
      }
      i += 2;
    } else if (c == '/' && i + 1 < count && chars[i + 1] == '/') {
      while (i < count && chars[i] != '\n') {
        i++;
      }
    } else if (c == '"' && (expect == 0 || expect == 2)) {
      UniChar* text = malloc(sizeof(UniChar) * (count - i));
      size_t len = 0;
      for (i++; i < count && chars[i] != '"'; i++) {
        UniChar ch = chars[i];
        if (ch == '\\' && i + 1 < count) {
          ch = chars[++i];
          if (ch == 'n') {
            ch = '\n';
          } else if (ch == 't') {
            ch = '\t';
          } else if (ch == 'r') {
            ch = '\r';
          } else if ((ch == 'U' || ch == 'u') && i + 4 < count) {
            char hex[5] = { chars[i + 1], chars[i + 2], chars[i + 3],
                            chars[i + 4], 0 };
            ch = (UniChar)strtoul(hex, NULL, 16);
            i += 4;
          }
        }
        text[len++] = ch;
      }
      i++;
      CFStringRef s = CFStringCreateWithCharacters(NULL, text, len);
      free(text);
      if (expect == 0) {
        pending_key = s;
        expect = 1;
      } else {
        cf_dict_set(table, pending_key, s);
        CFRelease(pending_key);
        CFRelease(s);
        pending_key = NULL;
        expect = 3;
      }
    } else if (c == '=' && expect == 1) {
      expect = 2;
      i++;
    } else if (c == ';') {
      if (expect == 1 && pending_key) {
        cf_dict_set(table, pending_key, pending_key);
        CFRelease(pending_key);
        pending_key = NULL;
      }
      expect = 0;
      i++;
    } else {
      i++;  // bare words and stray characters: skipped
    }
  }
  if (pending_key) {
    CFRelease(pending_key);
  }
  free(chars);
  return table;
}

static CFDictionaryRef strings_table(CFBundleRef bundle, const char* name) {
  CFStringRef key = cf_string_from_utf8(name, strlen(name));
  pthread_mutex_lock(&bundle_lock);
  if (!bundle->tables) {
    bundle->tables = cf_dict_create();
  }
  CFDictionaryRef table = cf_dict_get(bundle->tables, key);
  if (!table) {
    char file[NAME_MAX + 1];
    char found[PATH_MAX];
    snprintf(file, sizeof(file), "%s.strings", name);
    CFMutableDictionaryRef parsed = NULL;
    if (!bundle->is_system_framework &&
        find_resource(bundle, NULL, file, found, sizeof(found))) {
      FILE* f = fopen(found, "rb");
      if (f) {
        cf_buf contents = { 0 };
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
          cf_buf_append(&contents, chunk, n);
        }
        fclose(f);
        parsed = parse_strings((const UInt8*)contents.data, contents.len);
        cf_buf_free(&contents);
      }
    }
    if (!parsed) {
      parsed = cf_dict_create();
    }
    cf_dict_set(bundle->tables, key, parsed);
    CFRelease(parsed);
    table = parsed;
  }
  pthread_mutex_unlock(&bundle_lock);
  CFRelease(key);
  return table;
}

CFStringRef CFBundleCopyLocalizedString(CFBundleRef bundle, CFStringRef key,
                                        CFStringRef value,
                                        CFStringRef table_name) {
  char* table = table_name && cf_string_length(table_name)
                    ? cf_string_utf8(table_name)
                    : strdup("Localizable");
  CFStringRef found = key ? cf_dict_get(strings_table(bundle, table), key)
                          : NULL;
  free(table);
  CFStringRef result = found ? found
                     : value && cf_string_length(value) ? value : key;
  return result ? CFRetain(result) : NULL;
}

static CFDictionaryRef info_dictionary(CFBundleRef bundle) {
  pthread_mutex_lock(&bundle_lock);
  if (!bundle->info) {
    char path[PATH_MAX];
    char resolved[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Contents/Info.plist", bundle->path);
    CFPropertyListRef plist = NULL;
    if (!bundle->is_system_framework &&
        hle_path_resolve(path, resolved, sizeof(resolved))) {
      plist = cf_plist_read_file(resolved);
    }
    if (!plist || !cf_is(plist, CF_TYPE_DICTIONARY)) {
      if (plist) {
        CFRelease(plist);
      }
      plist = cf_dict_create();
    }
    bundle->info = plist;
  }
  pthread_mutex_unlock(&bundle_lock);
  return bundle->info;
}

CFDictionaryRef CFBundleGetInfoDictionary(CFBundleRef bundle) {
  return info_dictionary(bundle);
}

// InfoPlist.strings overrides Info.plist, as CF localizes these values.
CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFBundleRef bundle,
                                               CFStringRef key) {
  CFTypeRef localized = cf_dict_get(strings_table(bundle, "InfoPlist"), key);
  return localized ? localized : cf_dict_get(info_dictionary(bundle), key);
}

CFStringRef CFBundleGetIdentifier(CFBundleRef bundle) {
  return cf_dict_get_ascii(info_dictionary(bundle), "CFBundleIdentifier");
}

enum { kUnknownCode = 0x3F3F3F3F };  // '????'

// A four-character code from the Info.plist string |key|, else '????'.
static UInt32 info_code(CFBundleRef bundle, const char* key) {
  CFTypeRef value = cf_dict_get_ascii(info_dictionary(bundle), key);
  UInt32 code = kUnknownCode;
  if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
      cf_string_length(value) == 4) {
    code = 0;
    for (CFIndex i = 0; i < 4; i++) {
      code = code << 8 | (cf_string_char(value, i) & 0xFF);
    }
  }
  return code;
}

// The type and creator codes: from Contents/PkgInfo, else the Info.plist.
void CFBundleGetPackageInfo(CFBundleRef bundle, UInt32* type,
                            UInt32* creator) {
  UInt32 t = kUnknownCode;
  UInt32 c = kUnknownCode;
  char path[PATH_MAX];
  unsigned char bytes[8];
  FILE* f = NULL;
  if (bundle && !bundle->is_system_framework &&
      snprintf(path, sizeof(path), "%s/Contents/PkgInfo", bundle->path) <
          (int)sizeof(path)) {
    f = fopen(path, "rb");
  }
  if (f && fread(bytes, 1, 8, f) == 8) {
    t = (UInt32)bytes[0] << 24 | bytes[1] << 16 | bytes[2] << 8 | bytes[3];
    c = (UInt32)bytes[4] << 24 | bytes[5] << 16 | bytes[6] << 8 | bytes[7];
  } else if (bundle && !bundle->is_system_framework) {
    t = info_code(bundle, "CFBundlePackageType");
    c = info_code(bundle, "CFBundleSignature");
  }
  if (f) {
    fclose(f);
  }
  if (type) {
    *type = t;
  }
  if (creator) {
    *creator = c;
  }
}
