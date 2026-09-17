// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// libxml2 is Linux's own, loaded for the Mac library it stands in for, and
// its structures and callbacks match. The calls that open a file by name
// come through here first: Mac OS X volumes ignore case, and Age of Empires
// III asks for data/StringTable.xml where the file is stringtable.xml.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>

#include "cf.h"

static void* libxml2(void) {
  static void* handle;
  if (!handle) {
    handle = dlopen("libxml2.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
      fprintf(stderr, "hle: libxml2 did not load: %s\n", dlerror());
    }
  }
  return handle;
}

// The file |path| names, whatever the case of its components; |path| itself
// when nothing by that name exists.
static const char* resolved(const char* path, char* out, size_t size) {
  return path && hle_path_resolve(path, out, size) ? out : path;
}

int xmlSAXUserParseFile(void* sax, void* user_data, const char* filename) {
  void* handle = libxml2();
  int (*real)(void*, void*, const char*) =
      handle ? dlsym(handle, "xmlSAXUserParseFile") : NULL;
  if (!real) {
    return -1;
  }
  char path[PATH_MAX];
  return real(sax, user_data, resolved(filename, path, sizeof(path)));
}
