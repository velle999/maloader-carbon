// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// How the File Manager's names for things map onto Linux paths: dates,
// names, node IDs, FSSpecs with their partial pathnames, FSRefs, and the
// special folders.
//
// There is one volume, the system disk, and it is /. The folders a Mac keeps
// in the home and on the system disk live under hle_mac_home() and
// hle_mac_root().

#define _GNU_SOURCE

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "carbon.h"

static const char kVolumeName[] = "Macintosh HD";

// ---------------------------------------------------------------------------
// Dates

void hle_utc_from_unix(time_t t, UTCDateTime* out) {
  uint64_t seconds = (uint64_t)((int64_t)t + HLE_MAC_EPOCH_OFFSET);
  out->highSeconds = (UInt16)(seconds >> 32);
  out->lowSeconds = (UInt32)seconds;
  out->fraction = 0;
}

UInt32 hle_mac_local_seconds(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return (UInt32)((int64_t)t + tm.tm_gmtoff + HLE_MAC_EPOCH_OFFSET);
}

// ---------------------------------------------------------------------------
// Names

static void swap_char(char* s, char from, char to) {
  for (; *s; s++) {
    if (*s == from) {
      *s = to;
    }
  }
}

static char* macroman_to_posix(const unsigned char* bytes, size_t n) {
  char buf[256];
  if (n > 255) {
    n = 255;
  }
  memcpy(buf, bytes, n);
  buf[n] = '\0';
  CFStringRef s = CFStringCreateWithCString(NULL, buf, kCFStringEncodingMacRoman);
  char* utf8 = cf_string_utf8(s);
  CFRelease(s);
  swap_char(utf8, '/', ':');
  return utf8;
}

char* hle_name_from_pascal(ConstStringPtr name) {
  return macroman_to_posix(name + 1, name[0]);
}

char* hle_name_from_unicode(const UniChar* name, UniCharCount length) {
  CFStringRef s = CFStringCreateWithCharacters(NULL, name, length);
  char* utf8 = cf_string_utf8(s);
  CFRelease(s);
  swap_char(utf8, '/', ':');
  return utf8;
}

void hle_name_to_pascal(const char* posix_name, StringPtr out, size_t size) {
  char* copy = strdup(posix_name);
  swap_char(copy, ':', '/');
  CFStringRef s = cf_string_from_utf8(copy, strlen(copy));
  CFIndex room = size - 1 < 255 ? (CFIndex)size - 1 : 255;
  CFIndex used = 0;
  CFStringGetBytes(s, (CFRange){ 0, cf_string_length(s) },
                   kCFStringEncodingMacRoman, '?', 0, out + 1, room, &used);
  out[0] = (unsigned char)used;
  CFRelease(s);
  free(copy);
}

void hle_name_to_unicode(const char* posix_name, HFSUniStr255* out) {
  char* copy = strdup(posix_name);
  swap_char(copy, ':', '/');
  CFStringRef s = cf_string_from_utf8(copy, strlen(copy));
  CFIndex n = cf_string_length(s) < 255 ? cf_string_length(s) : 255;
  CFStringGetCharacters(s, (CFRange){ 0, n }, out->unicode);
  out->length = (UInt16)n;
  CFRelease(s);
  free(copy);
}

// ---------------------------------------------------------------------------
// Node IDs

#define FIRST_NODE_ID 16

static pthread_mutex_t node_lock = PTHREAD_MUTEX_INITIALIZER;
static char** node_paths;
static UInt32 node_count;
static UInt32 node_capacity;

static void canonical_path(const char* path, char* out, size_t size) {
  char resolved[PATH_MAX];
  if (realpath(path, resolved)) {
    snprintf(out, size, "%s", resolved);
  } else {
    snprintf(out, size, "%s", path);
  }
}

UInt32 hle_node_id(const char* path) {
  char canonical[PATH_MAX];
  canonical_path(path, canonical, sizeof(canonical));
  if (!strcmp(canonical, "/")) {
    return fsRtDirID;
  }
  pthread_mutex_lock(&node_lock);
  UInt32 i;
  for (i = 0; i < node_count; i++) {
    if (!strcmp(node_paths[i], canonical)) {
      break;
    }
  }
  if (i == node_count) {
    if (node_count == node_capacity) {
      node_capacity = node_capacity ? node_capacity * 2 : 256;
      node_paths = realloc(node_paths, sizeof(char*) * node_capacity);
    }
    node_paths[node_count++] = strdup(canonical);
  }
  pthread_mutex_unlock(&node_lock);
  return FIRST_NODE_ID + i;
}

const char* hle_node_path(UInt32 id) {
  if (id == fsRtDirID) {
    return "/";
  }
  pthread_mutex_lock(&node_lock);
  const char* path = id >= FIRST_NODE_ID && id - FIRST_NODE_ID < node_count
                         ? node_paths[id - FIRST_NODE_ID]
                         : NULL;
  pthread_mutex_unlock(&node_lock);
  return path;
}

// ---------------------------------------------------------------------------
// The default directory

static UInt32 default_dir_id;

UInt32 hle_default_dir_id(void) {
  if (default_dir_id) {
    return default_dir_id;
  }
  char cwd[PATH_MAX];
  return getcwd(cwd, sizeof(cwd)) ? hle_node_id(cwd) : fsRtDirID;
}

int HSetVol(ConstStringPtr volume_name, SInt16 vref, int32_t dir_id) {
  UInt32 id = dir_id ? (UInt32)dir_id : fsRtDirID;
  const char* path = hle_node_path(id);
  struct stat st;
  if (!path || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return dirNFErr;
  }
  default_dir_id = id;
  cf_trace("HSetVol(%s)", path);
  return noErr;
}

int HGetVol(StringPtr volume_name, SInt16* vref, int32_t* dir_id) {
  if (volume_name) {
    hle_name_to_pascal(kVolumeName, volume_name, 28);
  }
  if (vref) {
    *vref = kHleVolumeRefNum;
  }
  if (dir_id) {
    *dir_id = (int32_t)hle_default_dir_id();
  }
  return noErr;
}

// ---------------------------------------------------------------------------
// Paths

static char mac_root[PATH_MAX];
static pthread_once_t mac_root_once = PTHREAD_ONCE_INIT;

static void init_mac_root(void) {
  const char* explicit_root = getenv("HALO_MAC_ROOT");
  const char* home = hle_mac_home();
  size_t n = strlen(home);
  if (explicit_root && *explicit_root) {
    snprintf(mac_root, sizeof(mac_root), "%s", explicit_root);
  } else if (n > 5 && !strcmp(home + n - 5, "/home")) {
    snprintf(mac_root, sizeof(mac_root), "%.*s/root", (int)(n - 5), home);
  } else {
    snprintf(mac_root, sizeof(mac_root), "%s/.root", home);
  }
  hle_mkdirs(mac_root);
}

const char* hle_mac_root(void) {
  pthread_once(&mac_root_once, init_mac_root);
  return mac_root;
}

int hle_join(const char* dir, const char* name, char* out, size_t size) {
  char joined[PATH_MAX];
  size_t len = strlen(dir);
  int n = snprintf(joined, sizeof(joined), "%s%s%s", dir,
                   len && dir[len - 1] == '/' ? "" : "/", name);
  if (n < 0 || (size_t)n >= sizeof(joined)) {
    return 0;
  }
  if (hle_path_resolve(joined, out, size)) {
    return 1;
  }
  if ((size_t)n + 1 > size) {
    return 0;
  }
  memcpy(out, joined, n + 1);
  return 1;
}

static void parent_of(char* path) {
  char* slash = strrchr(path, '/');
  if (!slash || slash == path) {
    strcpy(path, "/");
  } else {
    *slash = '\0';
  }
}

OSErr hle_ref_to_path(const FSRef* ref, char* out, size_t size) {
  const char* path = ref ? hle_fsref_path(ref->hidden) : NULL;
  if (!path) {
    return errFSBadFSRef;
  }
  struct stat st;
  if (lstat(path, &st) != 0) {
    return fnfErr;
  }
  if (strlen(path) + 1 > size) {
    return pathTooLongErr;
  }
  strcpy(out, path);
  return noErr;
}

// Resolves a classic name against a directory: "name", a relative partial
// pathname (":a:b", "::up"), or a full one ("Volume:dir:name"). Colons
// separate components; each extra colon climbs one level. An empty name is
// the directory itself.
static OSErr resolve_classic_name(const char* dir, ConstStringPtr name,
                                  char* out, size_t size) {
  char path[PATH_MAX];
  const unsigned char* p = name ? name + 1 : NULL;
  const unsigned char* end = name ? name + 1 + name[0] : NULL;
  if (!name || name[0] == 0) {
    snprintf(out, size, "%s", dir);
    return noErr;
  }
  if (memchr(p, ':', end - p) && *p != ':') {
    strcpy(path, "/");
    const unsigned char* colon = memchr(p, ':', end - p);
    p = colon + 1;  // the volume name: every volume is /
  } else {
    snprintf(path, sizeof(path), "%s", dir);
    if (*p == ':') {
      p++;
    }
  }
  while (p < end) {
    if (*p == ':') {
      parent_of(path);
      p++;
      continue;
    }
    const unsigned char* colon = memchr(p, ':', end - p);
    size_t n = colon ? (size_t)(colon - p) : (size_t)(end - p);
    char* component = macroman_to_posix(p, n);
    char joined[PATH_MAX];
    int ok = hle_join(path, component, joined, sizeof(joined));
    free(component);
    if (!ok) {
      return bdNamErr;
    }
    strcpy(path, joined);
    p += n;
    if (p < end) {
      p++;  // the separator after a component
    }
  }
  if (strlen(path) + 1 > size) {
    return bdNamErr;
  }
  strcpy(out, path);
  return noErr;
}

OSErr hle_spec_to_path(const FSSpec* spec, char* out, size_t size) {
  if (spec->parID == fsRtParID) {
    snprintf(out, size, "/");
    return noErr;
  }
  const char* dir = hle_node_path(spec->parID ? (UInt32)spec->parID
                                              : hle_default_dir_id());
  struct stat st;
  if (!dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return dirNFErr;
  }
  return resolve_classic_name(dir, spec->name, out, size);
}

OSErr hle_path_to_spec(const char* path, FSSpec* spec) {
  memset(spec, 0, sizeof(*spec));
  spec->vRefNum = kHleVolumeRefNum;
  char absolute[PATH_MAX];
  canonical_path(path, absolute, sizeof(absolute));
  if (!strcmp(absolute, "/")) {
    spec->parID = fsRtParID;
    hle_name_to_pascal(kVolumeName, spec->name, sizeof(spec->name));
    return noErr;
  }
  char parent[PATH_MAX];
  snprintf(parent, sizeof(parent), "%s", absolute);
  parent_of(parent);
  struct stat st;
  if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return dirNFErr;
  }
  const char* slash = strrchr(absolute, '/');
  spec->parID = hle_node_id(parent);
  hle_name_to_pascal(slash ? slash + 1 : absolute, spec->name,
                     sizeof(spec->name));
  return stat(absolute, &st) == 0 ? noErr : fnfErr;
}

// ---------------------------------------------------------------------------
// Special folders

enum {
  UNDER_HOME,
  UNDER_ROOT,
};

// Whether a stock Mac OS X 10.4 install has the folder. One it always has is
// made the first time it is looked up, whatever the caller asks; one it may
// not have is made only when the caller asks for that.
enum {
  MAYBE_MISSING,
  ALWAYS_THERE,
};

typedef struct {
  OSType type;
  int base;
  const char* relative;
  int presence;
} folder;

static const folder kUserFolders[] = {
  { 'pref', UNDER_HOME, "Library/Preferences", ALWAYS_THERE },
  { 'asup', UNDER_HOME, "Library/Application Support", ALWAYS_THERE },
  { 'docs', UNDER_HOME, "Documents", ALWAYS_THERE },
  { 'desk', UNDER_HOME, "Desktop", ALWAYS_THERE },
  { 'cusr', UNDER_HOME, "", ALWAYS_THERE },
  { 'dlib', UNDER_HOME, "Library", ALWAYS_THERE },
  { 'fram', UNDER_HOME, "Library/Frameworks", MAYBE_MISSING },
  { 'font', UNDER_HOME, "Library/Fonts", ALWAYS_THERE },
  { 'trsh', UNDER_HOME, ".Trash", MAYBE_MISSING },
  { 'apps', UNDER_HOME, "Applications", MAYBE_MISSING },
  { 'temp', UNDER_ROOT, "tmp", ALWAYS_THERE },
  { 'flnt', UNDER_ROOT, "tmp/Cleanup At Startup", MAYBE_MISSING },
};

static const folder kLocalFolders[] = {
  { 'pref', UNDER_ROOT, "Library/Preferences", ALWAYS_THERE },
  { 'asup', UNDER_ROOT, "Library/Application Support", ALWAYS_THERE },
  { 'dlib', UNDER_ROOT, "Library", ALWAYS_THERE },
  { 'fram', UNDER_ROOT, "Library/Frameworks", ALWAYS_THERE },
  { 'font', UNDER_ROOT, "Library/Fonts", ALWAYS_THERE },
  { 'apps', UNDER_ROOT, "Applications", ALWAYS_THERE },
  { 'usrs', UNDER_ROOT, "Users", ALWAYS_THERE },
  { 'sdat', UNDER_ROOT, "Users/Shared", ALWAYS_THERE },
  { 'temp', UNDER_ROOT, "tmp", ALWAYS_THERE },
};

static const folder kSystemFolders[] = {
  { 'macs', UNDER_ROOT, "System", ALWAYS_THERE },
  { 'dlib', UNDER_ROOT, "System/Library", ALWAYS_THERE },
  { 'fram', UNDER_ROOT, "System/Library/Frameworks", ALWAYS_THERE },
  { 'font', UNDER_ROOT, "System/Library/Fonts", ALWAYS_THERE },
  { 'pref', UNDER_ROOT, "System/Library/Preferences", ALWAYS_THERE },
  { 'temp', UNDER_ROOT, "tmp", ALWAYS_THERE },
};

static const folder* find_folder(const folder* table, size_t n, OSType type) {
  for (size_t i = 0; i < n; i++) {
    if (table[i].type == type) {
      return &table[i];
    }
  }
  return NULL;
}

#define FOLDERS(table) (table), sizeof(table) / sizeof((table)[0])

static OSErr folder_path(SInt16 vref, OSType type, char* out, size_t size,
                         int* always_there) {
  const folder* f = NULL;
  switch (vref) {
    case kUserDomain:
      f = find_folder(FOLDERS(kUserFolders), type);
      break;
    case kLocalDomain:
    case kNetworkDomain:
      f = find_folder(FOLDERS(kLocalFolders), type);
      break;
    case kSystemDomain:
      f = find_folder(FOLDERS(kSystemFolders), type);
      break;
    default:
      // kOnSystemDisk, kOnAppropriateDisk or a volume: the domain a Mac
      // would pick for the type. The system owns its frameworks and library.
      if (type == 'fram' || type == 'dlib' || type == 'macs') {
        f = find_folder(FOLDERS(kSystemFolders), type);
      } else if (type == 'usrs') {
        f = find_folder(FOLDERS(kLocalFolders), type);
      } else {
        f = find_folder(FOLDERS(kUserFolders), type);
      }
  }
  if (!f) {
    char what[64];
    snprintf(what, sizeof(what), "FSFindFolder '%c%c%c%c' in domain %d",
             (int)(type >> 24) & 0xff, (int)(type >> 16) & 0xff,
             (int)(type >> 8) & 0xff, (int)type & 0xff, vref);
    fprintf(stderr, "hle: not implemented yet: %s\n", what);
    return fnfErr;
  }
  *always_there = f->presence == ALWAYS_THERE;
  const char* base = f->base == UNDER_HOME ? hle_mac_home() : hle_mac_root();
  int n = snprintf(out, size, "%s%s%s", base, *f->relative ? "/" : "",
                   f->relative);
  return n < 0 || (size_t)n >= size ? pathTooLongErr : noErr;
}

static OSErr find_folder_path(SInt16 vref, OSType type, unsigned int create,
                              char* path, size_t size) {
  int always_there = 0;
  OSErr err = folder_path(vref, type, path, size, &always_there);
  if (err) {
    return err;
  }
  if (access(path, F_OK) != 0) {
    if (!always_there && !(create & 0xff)) {
      cf_trace("FSFindFolder('%c%c%c%c', %d): %s does not exist",
               (int)(type >> 24) & 0xff, (int)(type >> 16) & 0xff,
               (int)(type >> 8) & 0xff, (int)type & 0xff, vref, path);
      return fnfErr;
    }
    if (!hle_mkdirs(path)) {
      return permErr;
    }
  }
  cf_trace("FSFindFolder('%c%c%c%c', %d) = %s", (int)(type >> 24) & 0xff,
           (int)(type >> 16) & 0xff, (int)(type >> 8) & 0xff,
           (int)type & 0xff, vref, path);
  return noErr;
}

int FSFindFolder(SInt16 vref, OSType type, unsigned int create,
                 FSRef* found) {
  char path[PATH_MAX];
  OSErr err = find_folder_path(vref, type, create, path, sizeof(path));
  if (err) {
    return err;
  }
  return hle_fsref_make(path, found->hidden) ? noErr : fnfErr;
}

int FindFolder(SInt16 vref, OSType type, unsigned int create,
               SInt16* found_vref, int32_t* found_dir_id) {
  char path[PATH_MAX];
  OSErr err = find_folder_path(vref, type, create, path, sizeof(path));
  if (err) {
    return err;
  }
  *found_vref = kHleVolumeRefNum;
  *found_dir_id = hle_node_id(path);
  return noErr;
}

// ---------------------------------------------------------------------------
// Making specs and refs

int FSMakeFSSpec(SInt16 vref, int32_t dir_id, ConstStringPtr name,
                 FSSpec* spec) {
  const char* dir = hle_node_path(dir_id ? (UInt32)dir_id
                                         : hle_default_dir_id());
  struct stat st;
  if (!dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return dirNFErr;
  }
  char path[PATH_MAX];
  OSErr err = resolve_classic_name(dir, name, path, sizeof(path));
  if (err) {
    return err;
  }
  err = hle_path_to_spec(path, spec);
  cf_trace("FSMakeFSSpec(%s) = %d", path, err);
  return err;
}

int FSpMakeFSRef(const FSSpec* spec, FSRef* ref) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  if (err) {
    return err;
  }
  return hle_fsref_make(path, ref->hidden) ? noErr : fnfErr;
}

int FSPathMakeRef(const UInt8* path, FSRef* ref, Boolean* is_directory) {
  char resolved[PATH_MAX];
  struct stat st;
  if (!hle_path_resolve((const char*)path, resolved, sizeof(resolved)) ||
      stat(resolved, &st) != 0 || !hle_fsref_make(resolved, ref->hidden)) {
    return fnfErr;
  }
  if (is_directory) {
    *is_directory = S_ISDIR(st.st_mode);
  }
  return noErr;
}

int FSRefMakePath(const FSRef* ref, UInt8* path, UInt32 size) {
  char resolved[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, resolved, sizeof(resolved));
  if (err) {
    return err;
  }
  size_t n = strlen(resolved);
  if (n + 1 > size) {
    return pathTooLongErr;
  }
  memcpy(path, resolved, n + 1);
  return noErr;
}

int FSMakeFSRefUnicode(const FSRef* parent, UniCharCount length,
                       const UniChar* name, TextEncoding hint, FSRef* ref) {
  char dir[PATH_MAX];
  OSErr err = hle_ref_to_path(parent, dir, sizeof(dir));
  if (err) {
    return err;
  }
  char* posix = hle_name_from_unicode(name, length);
  char path[PATH_MAX];
  int ok = hle_join(dir, posix, path, sizeof(path));
  free(posix);
  struct stat st;
  if (!ok || stat(path, &st) != 0 || !hle_fsref_make(path, ref->hidden)) {
    return fnfErr;
  }
  return noErr;
}

int FSCompareFSRefs(const FSRef* a, const FSRef* b) {
  const char* pa = hle_fsref_path(a->hidden);
  const char* pb = hle_fsref_path(b->hidden);
  if (!pa || !pb) {
    return errFSBadFSRef;
  }
  return strcmp(pa, pb) ? errFSRefsDifferent : noErr;
}

// FSRefs name resolved paths already, so a symbolic link has been followed
// by the time one exists; Mac aliases are not supported.
int FSResolveAliasFile(FSRef* ref, unsigned int resolve_chains,
                       Boolean* target_is_folder, Boolean* was_aliased) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    return err;
  }
  struct stat st;
  if (stat(path, &st) != 0) {
    return fnfErr;
  }
  if (target_is_folder) {
    *target_is_folder = S_ISDIR(st.st_mode);
  }
  if (was_aliased) {
    *was_aliased = 0;
  }
  return noErr;
}
