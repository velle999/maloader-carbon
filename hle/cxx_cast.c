// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// __dynamic_cast, remembering its answers.
//
// Halo's Direct3D layer casts its interface pointers with dynamic_cast for
// every draw and state change, and libstdc++ walks the class hierarchy each
// time: over 3% of the game's CPU time in its time demo. The answer depends
// only on the object's dynamic type and on where the source subobject lies
// inside it, and the vtable pointer at the source fixes both (a vtable holds
// its subobject's offset to the top, and an object under construction has a
// vtable of its own). So the thread that casts first, the game's, remembers
// for a vtable pointer, source type, destination type and hint how far the
// result lies from the source, or that there is none; other threads cast as
// they would. (A thread-local cache in a library loaded at run time cost a
// __tls_get_addr call per cast.) HLE_CAST_CACHE=0 leaves it out, and
// HLE_CAST_STATS=1 prints how often the cache answers.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kEntries = 1 << 10,
};

typedef struct {
  const void* vtable;  // NULL: empty
  const void* source_type;
  const void* destination_type;
  ptrdiff_t hint;
  ptrdiff_t displacement;
  int found;
} cast_entry;

typedef void* cast_function(const void* source, const void* source_type,
                            const void* destination_type, ptrdiff_t hint);

// libstdc++'s, looked up rather than linked: libmac.so is C, and the loader
// that loads it has libstdc++ already.
static cast_function* real_cast;
static cast_entry* entries;
static pthread_t owner;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int enabled;
static int stats;
static unsigned long hits;
static unsigned long misses;

static void start(void) {
  const char* setting = getenv("HLE_CAST_CACHE");
  const char* print = getenv("HLE_CAST_STATS");
  real_cast = dlsym(RTLD_DEFAULT, "__dynamic_cast");
  stats = print && strcmp(print, "0") != 0;
  owner = pthread_self();
  entries = calloc(kEntries, sizeof(*entries));
  enabled = !(setting && strcmp(setting, "0") == 0) && entries;
}

void* __darwin___dynamic_cast(const void* source, const void* source_type,
                              const void* destination_type, ptrdiff_t hint) {
  pthread_once(&once, start);
  if (!source || !real_cast) {
    return NULL;
  }
  if (!enabled || !pthread_equal(pthread_self(), owner)) {
    return real_cast(source, source_type, destination_type, hint);
  }
  const void* vtable = *(const void* const*)source;
  uintptr_t key = (uintptr_t)vtable * 2654435761u ^
                  (uintptr_t)source_type * 2246822519u ^
                  (uintptr_t)destination_type * 3266489917u ^ (uintptr_t)hint;
  cast_entry* e = &entries[(key ^ key >> 15) & (kEntries - 1)];
  if (e->vtable != vtable || e->source_type != source_type ||
      e->destination_type != destination_type || e->hint != hint) {
    void* result = real_cast(source, source_type, destination_type, hint);
    if (stats && ++misses % 100000 == 0) {
      fprintf(stderr, "hle: dynamic_cast: %lu answered from the cache, %lu "
              "not\n", hits, misses);
    }
    e->vtable = vtable;
    e->source_type = source_type;
    e->destination_type = destination_type;
    e->hint = hint;
    e->found = result != NULL;
    e->displacement = (const char*)result - (const char*)source;
    return result;
  }
  if (stats && ++hits % 1000000 == 0) {
    fprintf(stderr, "hle: dynamic_cast: %lu answered from the cache, %lu "
            "not\n", hits, misses);
  }
  return e->found ? (void*)((const char*)source + e->displacement) : NULL;
}
