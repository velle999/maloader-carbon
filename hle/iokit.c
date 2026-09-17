// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// IOKit, as far as the game looks.
//
// The registry holds the user's disc, when one is configured, as an
// IOCDMedia or IODVDMedia with one IOMedia partition whose BSD name
// getmntinfo agrees with. The disc is whichever of the two the game looks
// for: Halo came on a CD and Age of Empires III on a DVD. There are no HID
// devices, so input arrives as Carbon events.
// Objects are pointers, which fit an io_object_t on i386.

#include <stdlib.h>
#include <string.h>

#include "carbon.h"

typedef int kern_return_t;
typedef uint32_t io_object_t;
typedef char io_name_t[128];

enum {
  KERN_SUCCESS = 0,
  kIOReturnNoDevice = (int)0xe00002c0,
  kIOReturnBadArgument = (int)0xe00002c2,
  kIOReturnUnsupported = (int)0xe00002c7,
};

enum {
  kMagicEntry = 'IOen',
  kMagicIterator = 'IOit',
  kMagicNotifyPort = 'IOnp',
  kMasterPort = 0x1103,
};

typedef struct io_entry {
  uint32_t magic;
  const char* class_name;
  struct io_entry* parent;
  struct io_entry* child;
  int whole;
} io_entry;

typedef struct {
  uint32_t magic;
  io_entry* items[4];
  int count;
  int next;
} io_iterator;

typedef struct {
  uint32_t magic;
  CFTypeRef source;
} io_notify_port;

static io_entry cd_partition;
static io_entry cd_media = { kMagicEntry, "IOCDMedia", NULL, &cd_partition, 1 };
static io_entry cd_partition = { kMagicEntry, "IOMedia", &cd_media, NULL, 0 };

static io_entry* entry_of(io_object_t object) {
  io_entry* e = (io_entry*)(uintptr_t)object;
  return e && e->magic == kMagicEntry ? e : NULL;
}

static io_iterator* iterator_of(io_object_t object) {
  io_iterator* it = (io_iterator*)(uintptr_t)object;
  return it && it->magic == kMagicIterator ? it : NULL;
}

static io_object_t new_iterator(io_entry** items, int count) {
  io_iterator* it = calloc(1, sizeof(*it));
  it->magic = kMagicIterator;
  for (int i = 0; i < count && i < 4; i++) {
    it->items[it->count++] = items[i];
  }
  return (io_object_t)(uintptr_t)it;
}

// MACH_PORT_NULL, which every IOKit call takes as the master port.
const uint32_t kIOMasterPortDefault = 0;

kern_return_t IOMasterPort(uint32_t bootstrap, uint32_t* port) {
  if (port) {
    *port = kMasterPort;
  }
  return KERN_SUCCESS;
}

CFMutableDictionaryRef IOServiceMatching(const char* name) {
  CFMutableDictionaryRef dict = cf_dict_create();
  CFStringRef key = cf_string_from_utf8("IOProviderClass", 15);
  CFStringRef value = cf_string_from_utf8(name, strlen(name));
  cf_dict_set(dict, key, value);
  CFRelease(key);
  CFRelease(value);
  return dict;
}

// The services a matching dictionary finds. Consumes the dictionary.
static io_object_t find_services(CFDictionaryRef matching) {
  io_entry* found[1];
  int count = 0;
  CFTypeRef provider =
      matching ? cf_dict_get_ascii(matching, "IOProviderClass") : NULL;
  if (provider && CFGetTypeID(provider) == CFStringGetTypeID() &&
      hle_cd_volume_name()) {
    if (cf_string_is_ascii((CFStringRef)provider, "IOCDMedia")) {
      cd_media.class_name = "IOCDMedia";
      found[count++] = &cd_media;
    } else if (cf_string_is_ascii((CFStringRef)provider, "IODVDMedia")) {
      cd_media.class_name = "IODVDMedia";
      found[count++] = &cd_media;
    }
  }
  if (matching) {
    CFRelease(matching);
  }
  return new_iterator(found, count);
}

kern_return_t IOServiceGetMatchingServices(uint32_t port,
                                           CFDictionaryRef matching,
                                           io_object_t* iterator) {
  io_object_t it = find_services(matching);
  if (iterator) {
    *iterator = it;
  } else {
    free((io_iterator*)(uintptr_t)it);
  }
  return KERN_SUCCESS;
}

io_object_t IOIteratorNext(io_object_t iterator) {
  io_iterator* it = iterator_of(iterator);
  if (!it || it->next >= it->count) {
    return 0;
  }
  return (io_object_t)(uintptr_t)it->items[it->next++];
}

kern_return_t IOObjectRelease(io_object_t object) {
  io_iterator* it = iterator_of(object);
  if (it) {
    it->magic = 0;
    free(it);
  }
  return KERN_SUCCESS;
}

kern_return_t IOObjectGetClass(io_object_t object, io_name_t name) {
  io_entry* e = entry_of(object);
  const char* class_name = e ? e->class_name
                             : iterator_of(object) ? "IOUserIterator" : NULL;
  if (!class_name) {
    return kIOReturnBadArgument;
  }
  strncpy(name, class_name, sizeof(io_name_t) - 1);
  name[sizeof(io_name_t) - 1] = '\0';
  return KERN_SUCCESS;
}

// Every entry below |object|, depth first.
kern_return_t IORegistryEntryCreateIterator(io_object_t object,
                                            const char* plane, uint32_t options,
                                            io_object_t* iterator) {
  io_entry* found[4];
  int count = 0;
  io_entry* e = entry_of(object);
  for (io_entry* c = e ? e->child : NULL; c && count < 4; c = c->child) {
    found[count++] = c;
  }
  if (iterator) {
    *iterator = new_iterator(found, count);
  }
  return KERN_SUCCESS;
}

kern_return_t IORegistryEntryGetParentEntry(io_object_t object,
                                            const char* plane,
                                            io_object_t* parent) {
  io_entry* e = entry_of(object);
  if (!e || !e->parent) {
    return kIOReturnNoDevice;
  }
  if (parent) {
    *parent = (io_object_t)(uintptr_t)e->parent;
  }
  return KERN_SUCCESS;
}

static CFTypeRef property(io_entry* e, const char* key) {
  if (!e || !hle_cd_volume_name()) {
    return NULL;
  }
  if (!strcmp(key, "BSD Name")) {
    const char* partition = hle_cd_bsd_name();
    // The whole disc is the partition's name without its slice.
    size_t len = strlen(partition);
    if (e->whole) {
      const char* slice = strrchr(partition, 's');
      if (slice && slice > partition + 4) {
        len = slice - partition;
      }
    }
    return cf_string_from_utf8(partition, len);
  }
  if (!strcmp(key, "Ejectable") || !strcmp(key, "Removable")) {
    return CFRetain(kCFBooleanTrue);
  }
  if (!strcmp(key, "Whole")) {
    return CFRetain(e->whole ? kCFBooleanTrue : kCFBooleanFalse);
  }
  if (!strcmp(key, "Writable")) {
    return CFRetain(kCFBooleanFalse);
  }
  return NULL;
}

CFTypeRef IORegistryEntryCreateCFProperty(io_object_t object, CFStringRef key,
                                          CFAllocatorRef allocator,
                                          uint32_t options) {
  char* name = cf_string_utf8(key);
  CFTypeRef value = property(entry_of(object), name);
  free(name);
  return value;
}

kern_return_t IORegistryEntryCreateCFProperties(io_object_t object,
                                                CFMutableDictionaryRef* out,
                                                CFAllocatorRef allocator,
                                                uint32_t options) {
  CFMutableDictionaryRef dict = cf_dict_create();
  static const char* const kKeys[] = { "BSD Name", "Ejectable", "Removable",
                                       "Whole", "Writable" };
  io_entry* e = entry_of(object);
  for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
    CFTypeRef value = property(e, kKeys[i]);
    if (value) {
      CFStringRef key = cf_string_from_utf8(kKeys[i], strlen(kKeys[i]));
      cf_dict_set(dict, key, value);
      CFRelease(key);
      CFRelease(value);
    }
  }
  if (out) {
    *out = dict;
  } else {
    CFRelease(dict);
  }
  return KERN_SUCCESS;
}

// ---------------------------------------------------------------------------
// Notifications: accepted, and never delivered, as nothing arrives.

io_notify_port* IONotificationPortCreate(uint32_t master_port) {
  io_notify_port* port = calloc(1, sizeof(*port));
  port->magic = kMagicNotifyPort;
  return port;
}

// The run loop only keeps its sources, so any CF object will do.
CFTypeRef IONotificationPortGetRunLoopSource(io_notify_port* port) {
  if (!port || port->magic != kMagicNotifyPort) {
    return NULL;
  }
  if (!port->source) {
    port->source = cf_data_create(NULL, 0);
  }
  return port->source;
}

kern_return_t IOServiceAddMatchingNotification(io_notify_port* port,
                                               const char* type,
                                               CFDictionaryRef matching,
                                               void* callback, void* refcon,
                                               io_object_t* iterator) {
  // Existing matches are found by iterating, which also arms the
  // notification; later arrivals never come.
  io_object_t it = find_services(matching);
  if (iterator) {
    *iterator = it;
  } else {
    free((io_iterator*)(uintptr_t)it);
  }
  return KERN_SUCCESS;
}

kern_return_t IOServiceAddInterestNotification(io_notify_port* port,
                                               io_object_t service,
                                               const char* type,
                                               void* callback, void* refcon,
                                               io_object_t* notification) {
  if (notification) {
    *notification = new_iterator(NULL, 0);
  }
  return KERN_SUCCESS;
}

kern_return_t IOCreatePlugInInterfaceForService(io_object_t service,
                                                CFTypeRef plugin_type,
                                                CFTypeRef interface_type,
                                                void*** interface,
                                                int32_t* score) {
  if (interface) {
    *interface = NULL;
  }
  return kIOReturnUnsupported;
}

kern_return_t IODestroyPlugInInterface(void** interface) {
  return KERN_SUCCESS;
}
