// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// CFArray and CFDictionary. Collections this library builds (property lists,
// bundle info, preferences) retain what they hold and compare keys with
// CFEqual, as CF's kCFType callbacks do, and so does one created with any
// callbacks; kCFCopyStringDictionaryKeyCallBacks also copies string keys.
// One created with NULL callbacks holds raw pointers and compares them by
// address. The dictionaries involved are small, so lookups are linear.

#include <stdlib.h>
#include <string.h>

#include "cf.h"

struct __CFArray {
  CFObject obj;
  CFTypeRef* values;
  CFIndex count;
  CFIndex capacity;
  int cf_types;
};

struct __CFDictionary {
  CFObject obj;
  CFTypeRef* keys;
  CFTypeRef* values;
  CFIndex count;
  CFIndex capacity;
  int cf_types;
  int copy_string_keys;
};

// The callback structures. The collections here only look at which one they
// were given; the functions are CF's own, for code that calls them.
typedef struct {
  CFIndex version;
  CFTypeRef (*retain)(CFAllocatorRef allocator, CFTypeRef value);
  void (*release)(CFAllocatorRef allocator, CFTypeRef value);
  CFStringRef (*copyDescription)(CFTypeRef value);
  unsigned int (*equal)(CFTypeRef a, CFTypeRef b);
  CFHashCode (*hash)(CFTypeRef value);
} cf_callbacks;

static CFTypeRef callback_retain(CFAllocatorRef allocator, CFTypeRef value) {
  return CFRetain(value);
}

static CFTypeRef callback_copy_string(CFAllocatorRef allocator,
                                      CFTypeRef value) {
  return CFStringCreateWithSubstring(NULL, value,
                                     (CFRange){ 0, cf_string_length(value) });
}

static void callback_release(CFAllocatorRef allocator, CFTypeRef value) {
  CFRelease(value);
}

static unsigned int callback_equal(CFTypeRef a, CFTypeRef b) {
  return cf_equal(a, b);
}

const cf_callbacks kCFTypeArrayCallBacks = {
  0, callback_retain, callback_release, NULL, callback_equal, NULL,
};
const cf_callbacks kCFTypeDictionaryKeyCallBacks = {
  0, callback_retain, callback_release, NULL, callback_equal, cf_hash,
};
const cf_callbacks kCFCopyStringDictionaryKeyCallBacks = {
  0, callback_copy_string, callback_release, NULL, callback_equal, cf_hash,
};
const cf_callbacks kCFTypeDictionaryValueCallBacks = {
  0, callback_retain, callback_release, NULL, callback_equal, NULL,
};

// ---------------------------------------------------------------------------
// Arrays

static void array_finalize(CFTypeRef obj) {
  struct __CFArray* a = (struct __CFArray*)obj;
  if (a->cf_types) {
    for (CFIndex i = 0; i < a->count; i++) {
      CFRelease(a->values[i]);
    }
  }
  free(a->values);
}

static int array_equal(CFTypeRef x, CFTypeRef y) {
  const struct __CFArray* a = x;
  const struct __CFArray* b = y;
  if (a->count != b->count) {
    return 0;
  }
  for (CFIndex i = 0; i < a->count; i++) {
    if (!cf_equal(a->values[i], b->values[i])) {
      return 0;
    }
  }
  return 1;
}

static void array_describe(CFTypeRef obj, cf_buf* out) {
  const struct __CFArray* a = obj;
  cf_buf_appends(out, "(");
  for (CFIndex i = 0; i < a->count; i++) {
    cf_buf_appends(out, i ? ", " : " ");
    cf_describe(a->values[i], out);
  }
  cf_buf_appends(out, a->count ? " )" : ")");
}

static const cf_class cf_array_class = {
  CF_TYPE_ARRAY, "CFArray", array_finalize, array_equal, NULL, array_describe,
};

static struct __CFArray* array_new(int cf_types) {
  struct __CFArray* a = cf_alloc(&cf_array_class, sizeof(*a));
  a->cf_types = cf_types;
  return a;
}

CFMutableArrayRef cf_array_create(void) {
  return array_new(1);
}

void cf_array_append(CFMutableArrayRef array, CFTypeRef value) {
  if (array->count == array->capacity) {
    array->capacity = array->capacity ? array->capacity * 2 : 8;
    array->values = realloc(array->values,
                            sizeof(CFTypeRef) * array->capacity);
  }
  if (array->cf_types) {
    CFRetain(value);
  }
  array->values[array->count++] = value;
}

CFIndex cf_array_count(CFArrayRef array) {
  return array->count;
}

CFTypeRef cf_array_get(CFArrayRef array, CFIndex i) {
  return array->values[i];
}

CFTypeID CFArrayGetTypeID(void) {
  return CF_TYPE_ARRAY;
}

CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef allocator,
                                       CFIndex capacity,
                                       const void* callbacks) {
  return array_new(callbacks != NULL);
}

void CFArrayAppendValue(CFMutableArrayRef array, CFTypeRef value) {
  cf_array_append(array, value);
}

CFIndex CFArrayGetCount(CFArrayRef array) {
  return array->count;
}

CFTypeRef CFArrayGetValueAtIndex(CFArrayRef array, CFIndex i) {
  return array->values[i];
}

CFMutableArrayRef CFArrayCreateMutableCopy(CFAllocatorRef allocator,
                                           CFIndex capacity, CFArrayRef array) {
  struct __CFArray* copy = array_new(array->cf_types);
  for (CFIndex i = 0; i < array->count; i++) {
    cf_array_append(copy, array->values[i]);
  }
  return copy;
}

// The index of the first value in |range| equal to |value|; -1 when none is.
CFIndex CFArrayGetFirstIndexOfValue(CFArrayRef array, CFRange range,
                                    CFTypeRef value) {
  for (CFIndex i = range.location; i < range.location + range.length; i++) {
    if (array->cf_types ? cf_equal(array->values[i], value)
                        : array->values[i] == value) {
      return i;
    }
  }
  return -1;
}

// At the count, appends.
void CFArraySetValueAtIndex(CFMutableArrayRef array, CFIndex i,
                            CFTypeRef value) {
  if (i == array->count) {
    cf_array_append(array, value);
    return;
  }
  if (array->cf_types) {
    CFRetain(value);
    CFRelease(array->values[i]);
  }
  array->values[i] = value;
}

void CFArrayApplyFunction(CFArrayRef array, CFRange range,
                          void (*applier)(CFTypeRef value, void* context),
                          void* context) {
  for (CFIndex i = range.location; i < range.location + range.length; i++) {
    applier(array->values[i], context);
  }
}

// ---------------------------------------------------------------------------
// Dictionaries

static void dict_finalize(CFTypeRef obj) {
  struct __CFDictionary* d = (struct __CFDictionary*)obj;
  if (d->cf_types) {
    for (CFIndex i = 0; i < d->count; i++) {
      CFRelease(d->keys[i]);
      CFRelease(d->values[i]);
    }
  }
  free(d->keys);
  free(d->values);
}

static CFIndex dict_index(CFDictionaryRef dict, CFTypeRef key) {
  for (CFIndex i = 0; i < dict->count; i++) {
    if (dict->cf_types ? cf_equal(dict->keys[i], key) : dict->keys[i] == key) {
      return i;
    }
  }
  return -1;
}

static int dict_equal(CFTypeRef x, CFTypeRef y) {
  const struct __CFDictionary* a = x;
  const struct __CFDictionary* b = y;
  if (a->count != b->count) {
    return 0;
  }
  for (CFIndex i = 0; i < a->count; i++) {
    CFIndex j = dict_index(b, a->keys[i]);
    if (j < 0 || !cf_equal(a->values[i], b->values[j])) {
      return 0;
    }
  }
  return 1;
}

static void dict_describe(CFTypeRef obj, cf_buf* out) {
  const struct __CFDictionary* d = obj;
  cf_buf_appends(out, "{");
  for (CFIndex i = 0; i < d->count; i++) {
    cf_buf_appends(out, " ");
    cf_describe(d->keys[i], out);
    cf_buf_appends(out, " = ");
    cf_describe(d->values[i], out);
    cf_buf_appends(out, ";");
  }
  cf_buf_appends(out, d->count ? " }" : "}");
}

static const cf_class cf_dictionary_class = {
  CF_TYPE_DICTIONARY, "CFDictionary", dict_finalize, dict_equal, NULL,
  dict_describe,
};

static struct __CFDictionary* dict_new(int cf_types) {
  struct __CFDictionary* d = cf_alloc(&cf_dictionary_class, sizeof(*d));
  d->cf_types = cf_types;
  return d;
}

CFMutableDictionaryRef cf_dict_create(void) {
  return dict_new(1);
}

void cf_dict_set(CFMutableDictionaryRef dict, CFTypeRef key, CFTypeRef value) {
  if (dict->cf_types) {
    CFRetain(value);
  }
  CFIndex i = dict_index(dict, key);
  if (i >= 0) {
    if (dict->cf_types) {
      CFRelease(dict->values[i]);
    }
    dict->values[i] = value;
    return;
  }
  if (dict->count == dict->capacity) {
    dict->capacity = dict->capacity ? dict->capacity * 2 : 8;
    dict->keys = realloc(dict->keys, sizeof(CFTypeRef) * dict->capacity);
    dict->values = realloc(dict->values, sizeof(CFTypeRef) * dict->capacity);
  }
  if (dict->copy_string_keys && cf_is(key, CF_TYPE_STRING)) {
    key = callback_copy_string(NULL, key);
  } else if (dict->cf_types) {
    CFRetain(key);
  }
  dict->keys[dict->count] = key;
  dict->values[dict->count] = value;
  dict->count++;
}

CFTypeRef cf_dict_get(CFDictionaryRef dict, CFTypeRef key) {
  CFIndex i = dict_index(dict, key);
  return i >= 0 ? dict->values[i] : NULL;
}

CFTypeRef cf_dict_get_ascii(CFDictionaryRef dict, const char* key) {
  for (CFIndex i = 0; i < dict->count; i++) {
    if (cf_is(dict->keys[i], CF_TYPE_STRING) &&
        cf_string_is_ascii(dict->keys[i], key)) {
      return dict->values[i];
    }
  }
  return NULL;
}

void cf_dict_remove(CFMutableDictionaryRef dict, CFTypeRef key) {
  CFIndex i = dict_index(dict, key);
  if (i < 0) {
    return;
  }
  CFTypeRef old_key = dict->keys[i];
  CFTypeRef old_value = dict->values[i];
  dict->count--;
  memmove(dict->keys + i, dict->keys + i + 1,
          sizeof(CFTypeRef) * (dict->count - i));
  memmove(dict->values + i, dict->values + i + 1,
          sizeof(CFTypeRef) * (dict->count - i));
  if (dict->cf_types) {
    CFRelease(old_key);
    CFRelease(old_value);
  }
}

CFIndex cf_dict_count(CFDictionaryRef dict) {
  return dict->count;
}

void cf_dict_pair(CFDictionaryRef dict, CFIndex i, CFTypeRef* key,
                  CFTypeRef* value) {
  *key = dict->keys[i];
  *value = dict->values[i];
}

CFTypeID CFDictionaryGetTypeID(void) {
  return CF_TYPE_DICTIONARY;
}

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef allocator,
                                                 CFIndex capacity,
                                                 const void* key_callbacks,
                                                 const void* value_callbacks) {
  struct __CFDictionary* d = dict_new(key_callbacks != NULL);
  d->copy_string_keys = key_callbacks == &kCFCopyStringDictionaryKeyCallBacks;
  return d;
}

CFMutableDictionaryRef CFDictionaryCreateMutableCopy(CFAllocatorRef allocator,
                                                     CFIndex capacity,
                                                     CFDictionaryRef dict) {
  struct __CFDictionary* copy = dict_new(dict->cf_types);
  copy->copy_string_keys = dict->copy_string_keys;
  for (CFIndex i = 0; i < dict->count; i++) {
    cf_dict_set(copy, dict->keys[i], dict->values[i]);
  }
  return copy;
}

CFTypeRef CFDictionaryGetValue(CFDictionaryRef dict, CFTypeRef key) {
  return cf_dict_get(dict, key);
}

int CFDictionaryGetValueIfPresent(CFDictionaryRef dict, CFTypeRef key,
                                  CFTypeRef* value) {
  CFIndex i = dict_index(dict, key);
  if (i < 0) {
    return 0;
  }
  if (value) {
    *value = dict->values[i];
  }
  return 1;
}

void CFDictionarySetValue(CFMutableDictionaryRef dict, CFTypeRef key,
                          CFTypeRef value) {
  cf_dict_set(dict, key, value);
}

void CFDictionaryRemoveValue(CFMutableDictionaryRef dict, CFTypeRef key) {
  cf_dict_remove(dict, key);
}

CFIndex CFDictionaryGetCount(CFDictionaryRef dict) {
  return dict->count;
}

void CFDictionaryApplyFunction(CFDictionaryRef dict,
                               void (*applier)(CFTypeRef key, CFTypeRef value,
                                               void* context),
                               void* context) {
  for (CFIndex i = 0; i < dict->count; i++) {
    applier(dict->keys[i], dict->values[i], context);
  }
}
