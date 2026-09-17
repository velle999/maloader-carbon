// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Displays: Quartz Display Services, the Display Manager and the main
// GDevice, all describing the display SDL numbers 0.
//
// A mode switch changes what the game is told and the size of its SDL
// window; whether the real screen changes mode is the SDL layer's call.
// Gamma tables are kept but not applied, since a desktop's gamma would
// outlive a crash.

#define _GNU_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "gui.h"

typedef uint32_t CGDirectDisplayID;
typedef int32_t CGError;
typedef float CGFloat;

typedef struct {
  CGFloat x;
  CGFloat y;
} CGPoint;

typedef struct {
  CGPoint origin;
  struct {
    CGFloat width;
    CGFloat height;
  } size;
} CGRect;

enum {
  kCGErrorSuccess = 0,
  kCGErrorIllegalArgument = 1001,
  kCGErrorRangeCheck = 1007,
};

enum {
  kMainDisplayID = 0x042c0040,
  kMaxModes = 64,
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Modes

static hle_display_mode modes[kMaxModes];
static int mode_count;
static hle_display_mode desktop;
static hle_display_mode current;
static int captured;

static int compare_modes(const void* a, const void* b) {
  const hle_display_mode* x = a;
  const hle_display_mode* y = b;
  if (x->width != y->width) {
    return x->width - y->width;
  }
  if (x->height != y->height) {
    return x->height - y->height;
  }
  return x->refresh - y->refresh;
}

static void add_mode(int width, int height, int refresh) {
  for (int i = 0; i < mode_count; i++) {
    if (modes[i].width == width && modes[i].height == height &&
        modes[i].refresh == refresh) {
      return;
    }
  }
  if (mode_count < kMaxModes && width >= 640 && height >= 480) {
    modes[mode_count++] = (hle_display_mode){ width, height, refresh };
  }
}

// Called with the lock held.
static void load_modes(void) {
  if (mode_count) {
    return;
  }
  SDL_DisplayMode m;
  if (hle_sdl_video() && SDL_GetDesktopDisplayMode(0, &m) == 0) {
    desktop = (hle_display_mode){ m.w, m.h, m.refresh_rate };
    int n = SDL_GetNumDisplayModes(0);
    for (int i = 0; i < n; i++) {
      if (SDL_GetDisplayMode(0, i, &m) == 0) {
        add_mode(m.w, m.h, m.refresh_rate);
      }
    }
    add_mode(desktop.width, desktop.height, desktop.refresh);
  }
  if (mode_count == 0) {
    desktop = (hle_display_mode){ 1024, 768, 60 };
    add_mode(640, 480, 60);
    add_mode(800, 600, 60);
    add_mode(1024, 768, 60);
  }
  qsort(modes, mode_count, sizeof(modes[0]), compare_modes);
  current = desktop;
  cf_trace("display: %d modes, desktop %dx%d@%d", mode_count, desktop.width,
           desktop.height, desktop.refresh);
}

int hle_display_modes(hle_display_mode* out, int max) {
  pthread_mutex_lock(&lock);
  load_modes();
  int n = mode_count < max ? mode_count : max;
  memcpy(out, modes, n * sizeof(*out));
  pthread_mutex_unlock(&lock);
  return n;
}

void hle_display_current(hle_display_mode* mode) {
  pthread_mutex_lock(&lock);
  load_modes();
  *mode = current;
  pthread_mutex_unlock(&lock);
}

int hle_display_captured(void) {
  return captured;
}

// ---------------------------------------------------------------------------
// Quartz mode dictionaries, made once per mode and depth and never freed,
// as the game holds on to the ones it is given without retaining them.

typedef struct mode_dict {
  hle_display_mode mode;
  int bits;
  CFDictionaryRef dict;
  struct mode_dict* next;
} mode_dict;

static mode_dict* dicts;

static void set_number(CFMutableDictionaryRef dict, const char* key,
                       CFNumberRef value) {
  CFStringRef k = cf_string_from_utf8(key, strlen(key));
  cf_dict_set(dict, k, value);
  CFRelease(k);
  CFRelease(value);
}

static void set_bool(CFMutableDictionaryRef dict, const char* key, int value) {
  CFStringRef k = cf_string_from_utf8(key, strlen(key));
  cf_dict_set(dict, k, value ? kCFBooleanTrue : kCFBooleanFalse);
  CFRelease(k);
}

// Called with the lock held.
static CFDictionaryRef dict_for(const hle_display_mode* mode, int bits) {
  for (mode_dict* d = dicts; d; d = d->next) {
    if (d->bits == bits && !compare_modes(&d->mode, mode)) {
      return d->dict;
    }
  }
  int index = 0;
  for (int i = 0; i < mode_count; i++) {
    if (!compare_modes(&modes[i], mode)) {
      index = i;
    }
  }
  CFMutableDictionaryRef dict = cf_dict_create();
  set_number(dict, "Width", cf_number_int(mode->width));
  set_number(dict, "Height", cf_number_int(mode->height));
  set_number(dict, "BitsPerPixel", cf_number_int(bits));
  set_number(dict, "BitsPerSample", cf_number_int(bits == 16 ? 5 : 8));
  set_number(dict, "SamplesPerPixel", cf_number_int(3));
  set_number(dict, "RefreshRate", cf_number_double(mode->refresh));
  set_number(dict, "Mode", cf_number_int(index + 1));
  set_number(dict, "IOFlags", cf_number_int(7));
  set_number(dict, "kCGDisplayBytesPerRow",
             cf_number_int(mode->width * (bits == 16 ? 2 : 4)));
  set_bool(dict, "UsableForDesktopGUI", 1);
  set_bool(dict, "kCGDisplayModeIsSafeForHardware", 1);
  set_bool(dict, "kCGDisplayModeIsInterlaced", 0);
  set_bool(dict, "kCGDisplayModeIsStretched", 0);
  set_bool(dict, "kCGDisplayModeIsTelevisionOutput", 0);
  mode_dict* d = calloc(1, sizeof(*d));
  d->mode = *mode;
  d->bits = bits;
  d->dict = dict;
  d->next = dicts;
  dicts = d;
  return dict;
}

static int64_t dict_int(CFDictionaryRef dict, const char* key) {
  CFTypeRef value = dict ? cf_dict_get_ascii(dict, key) : NULL;
  return value && CFGetTypeID(value) == CFNumberGetTypeID()
             ? cf_number_as_int((CFNumberRef)value)
             : 0;
}

// The closest mode to a request: the same size if there is one, else the
// smallest that holds it, else the largest; then the refresh rate nearest
// |refresh|, or the desktop's when that is 0.
static const hle_display_mode* best_mode(int width, int height,
                                         double refresh) {
  const hle_display_mode* best = NULL;
  int best_area = 0;
  for (int i = 0; i < mode_count; i++) {
    const hle_display_mode* m = &modes[i];
    if (m->width < width || m->height < height) {
      continue;
    }
    int area = m->width * m->height;
    if (!best || area < best_area) {
      best = m;
      best_area = area;
    }
  }
  if (!best) {
    best = &modes[mode_count - 1];
  }
  double target = refresh > 0 ? refresh : desktop.refresh;
  const hle_display_mode* chosen = best;
  for (int i = 0; i < mode_count; i++) {
    const hle_display_mode* m = &modes[i];
    if (m->width == best->width && m->height == best->height &&
        fabs(m->refresh - target) < fabs(chosen->refresh - target)) {
      chosen = m;
    }
  }
  return chosen;
}

CGDirectDisplayID CGMainDisplayID(void) {
  return kMainDisplayID;
}

CGError CGGetActiveDisplayList(uint32_t max, CGDirectDisplayID* displays,
                               uint32_t* count) {
  if (displays && max > 0) {
    displays[0] = kMainDisplayID;
  }
  if (count) {
    *count = displays ? (max > 0 ? 1 : 0) : 1;
  }
  return kCGErrorSuccess;
}

uint32_t CGDisplayIDToOpenGLDisplayMask(CGDirectDisplayID display) {
  return display == kMainDisplayID ? 1 : 0;
}

CGRect CGDisplayBounds(CGDirectDisplayID display) {
  hle_display_mode mode;
  hle_display_current(&mode);
  CGRect r = { { 0, 0 }, { mode.width, mode.height } };
  return r;
}

size_t CGDisplayPixelsWide(CGDirectDisplayID display) {
  hle_display_mode mode;
  hle_display_current(&mode);
  return mode.width;
}

size_t CGDisplayPixelsHigh(CGDirectDisplayID display) {
  hle_display_mode mode;
  hle_display_current(&mode);
  return mode.height;
}

size_t CGDisplayBitsPerPixel(CGDirectDisplayID display) {
  return 32;
}

// The physical size in millimeters, as a CGSize of two floats, which Darwin
// returns in EAX:EDX. It is the size the desktop mode's pixels have at 96
// dots an inch: games work out their dots per inch from it and size text
// by that, and 96 is the resolution Windows games are made for.
uint64_t CGDisplayScreenSize(CGDirectDisplayID display) {
  pthread_mutex_lock(&lock);
  load_modes();
  float width = desktop.width * 25.4f / 96;
  float height = desktop.height * 25.4f / 96;
  pthread_mutex_unlock(&lock);
  uint32_t w, h;
  memcpy(&w, &width, 4);
  memcpy(&h, &height, 4);
  return (uint64_t)h << 32 | w;
}

// The display's IOKit service; there is none to query.
uint32_t CGDisplayIOServicePort(CGDirectDisplayID display) {
  return 0;
}

CFDictionaryRef CGDisplayCurrentMode(CGDirectDisplayID display) {
  pthread_mutex_lock(&lock);
  load_modes();
  CFDictionaryRef dict = dict_for(&current, 32);
  pthread_mutex_unlock(&lock);
  return dict;
}

CFDictionaryRef CGDisplayBestModeForParametersAndRefreshRate(
    CGDirectDisplayID display, uint32_t bits, uint32_t width, uint32_t height,
    double refresh, int* exact) {
  pthread_mutex_lock(&lock);
  load_modes();
  const hle_display_mode* m = best_mode(width, height, refresh);
  int depth = bits <= 16 ? 16 : 32;
  CFDictionaryRef dict = dict_for(m, depth);
  if (exact) {
    *exact = m->width == (int)width && m->height == (int)height &&
             depth == (int)bits &&
             (refresh <= 0 || fabs(m->refresh - refresh) < 1);
  }
  pthread_mutex_unlock(&lock);
  cf_trace("CGDisplayBestMode(%ux%u, %u bits, %.0f Hz) = %dx%d@%d", width,
           height, bits, refresh, m->width, m->height, m->refresh);
  return dict;
}

CFDictionaryRef CGDisplayBestModeForParameters(CGDirectDisplayID display,
                                               uint32_t bits, uint32_t width,
                                               uint32_t height, int* exact) {
  return CGDisplayBestModeForParametersAndRefreshRate(display, bits, width,
                                                      height, 0, exact);
}

CGError CGDisplaySwitchToMode(CGDirectDisplayID display, CFDictionaryRef dict) {
  if (!dict) {
    return kCGErrorIllegalArgument;
  }
  hle_display_mode mode = { dict_int(dict, "Width"), dict_int(dict, "Height"),
                            0 };
  CFTypeRef rate = cf_dict_get_ascii(dict, "RefreshRate");
  if (rate && CFGetTypeID(rate) == CFNumberGetTypeID()) {
    mode.refresh = (int)(cf_number_as_double((CFNumberRef)rate) + 0.5);
  }
  if (mode.width <= 0 || mode.height <= 0) {
    return kCGErrorIllegalArgument;
  }
  pthread_mutex_lock(&lock);
  load_modes();
  int changed = compare_modes(&mode, &current) != 0;
  current = mode;
  pthread_mutex_unlock(&lock);
  cf_trace("CGDisplaySwitchToMode(%dx%d@%d)", mode.width, mode.height,
           mode.refresh);
  if (changed) {
    hle_sdl_switch_mode(&mode);
  }
  return kCGErrorSuccess;
}

CGError CGCaptureAllDisplays(void) {
  captured = 1;
  return kCGErrorSuccess;
}

CGError CGReleaseAllDisplays(void) {
  captured = 0;
  return kCGErrorSuccess;
}

int CGDisplayIsCaptured(CGDirectDisplayID display) {
  return captured;
}

// The level of the shield window capturing puts over other windows.
int32_t CGShieldingWindowLevel(void) {
  return 2147483630;
}

// ---------------------------------------------------------------------------
// Gamma

enum {
  kGammaEntries = 256,
};

static float gamma_tables[3][kGammaEntries];
static int gamma_set;

CGError CGGetDisplayTransferByTable(CGDirectDisplayID display,
                                    uint32_t capacity, float* red,
                                    float* green, float* blue,
                                    uint32_t* count) {
  uint32_t n = capacity < kGammaEntries ? capacity : kGammaEntries;
  for (uint32_t i = 0; i < n; i++) {
    float identity = (float)i / (kGammaEntries - 1);
    red[i] = gamma_set ? gamma_tables[0][i] : identity;
    green[i] = gamma_set ? gamma_tables[1][i] : identity;
    blue[i] = gamma_set ? gamma_tables[2][i] : identity;
  }
  if (count) {
    *count = n;
  }
  return kCGErrorSuccess;
}

CGError CGSetDisplayTransferByTable(CGDirectDisplayID display, uint32_t size,
                                    const float* red, const float* green,
                                    const float* blue) {
  if (size == 0) {
    return kCGErrorRangeCheck;
  }
  for (int i = 0; i < kGammaEntries; i++) {
    uint32_t j = (uint32_t)((double)i * (size - 1) / (kGammaEntries - 1));
    gamma_tables[0][i] = red[j];
    gamma_tables[1][i] = green[j];
    gamma_tables[2][i] = blue[j];
  }
  gamma_set = 1;
  cf_warn_once("CGSetDisplayTransferByTable: gamma is recorded, not applied");
  return kCGErrorSuccess;
}

// ---------------------------------------------------------------------------
// The pointer

CGError CGWarpMouseCursorPosition(CGPoint point) {
  hle_sdl_warp_mouse((int)point.x, (int)point.y);
  return kCGErrorSuccess;
}

// Synthesizes input for other applications; there are none to receive it.
CGError CGPostMouseEvent(CGPoint point, int update_position, uint32_t buttons,
                         int down, ...) {
  if (update_position) {
    hle_sdl_warp_mouse((int)point.x, (int)point.y);
  }
  return kCGErrorSuccess;
}

CGError CGSetLocalEventsSuppressionInterval(double seconds) {
  return kCGErrorSuccess;
}

// ---------------------------------------------------------------------------
// The main GDevice

enum {
  directType = 2,
  kDeviceFlags = (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 15),
};

static GDHandle main_device;

GDHandle hle_main_device(void) {
  hle_display_mode mode;
  hle_display_current(&mode);
  pthread_mutex_lock(&lock);
  if (!main_device) {
    main_device = (GDHandle)hle_handle_new(NULL, sizeof(GDevice), 0);
    PixMapHandle pm = (PixMapHandle)hle_handle_new(NULL, sizeof(PixMap), 0);
    GDevice* gd = *main_device;
    gd->gdRefNum = -1;
    gd->gdType = directType;
    gd->gdFlags = (SInt16)kDeviceFlags;
    gd->gdPMap = pm;
    gd->gdResPref = 4;
    PixMap* p = *pm;
    p->rowBytes = (SInt16)0x8000;
    p->pmVersion = 4;
    p->hRes = p->vRes = 72 << 16;
    p->pixelType = 16;  // RGBDirect
    p->pixelSize = 32;
    p->cmpCount = 3;
    p->cmpSize = 8;
    p->pixelFormat = 'ARGB';
  }
  GDevice* gd = *main_device;
  gd->gdRect.top = gd->gdRect.left = 0;
  gd->gdRect.bottom = mode.height;
  gd->gdRect.right = mode.width;
  PixMap* p = *gd->gdPMap;
  p->bounds = gd->gdRect;
  p->rowBytes = (SInt16)(0x8000 | (mode.width * 4 & 0x7FFF));
  pthread_mutex_unlock(&lock);
  return main_device;
}

GDHandle GetMainDevice(void) {
  return hle_main_device();
}

GDHandle GetDeviceList(void) {
  return hle_main_device();
}

GDHandle GetNextDevice(GDHandle device) {
  return NULL;
}

GDHandle GetGDevice(void) {
  return hle_main_device();
}

void SetGDevice(GDHandle device) {
}

// ---------------------------------------------------------------------------
// The Display Manager

typedef uint32_t DisplayIDType;

#pragma pack(push, 2)

typedef struct {
  UInt16 csMode;
  UInt32 csData;
  UInt16 csPage;
  Ptr csBaseAddr;
  UInt32 csReserved;
} VDSwitchInfoRec;

typedef struct {
  UInt32 csPreviousDisplayModeID;
  UInt32 csDisplayModeID;
  UInt32 csHorizontalPixels;
  UInt32 csVerticalLines;
  Fixed csRefreshRate;
  UInt16 csMaxDepthMode;
  UInt32 csResolutionFlags;
  UInt32 csReserved;
} VDResolutionInfoRec;

typedef struct {
  Ptr vpBaseOffset;
  SInt16 vpRowBytes;
  Rect vpBounds;
  SInt16 vpVersion;
  SInt16 vpPackType;
  SInt32 vpPackSize;
  SInt32 vpHRes;
  SInt32 vpVRes;
  SInt16 vpPixelType;
  SInt16 vpPixelSize;
  SInt16 vpCmpCount;
  SInt16 vpCmpSize;
  SInt32 vpPlaneBytes;
} VPBlock;

typedef struct {
  VDSwitchInfoRec* depthSwitchInfo;
  VPBlock* depthVPBlock;
  UInt32 depthFlags;
  UInt32 depthReserved1;
  UInt32 depthReserved2;
} DMDepthInfo;

typedef struct {
  UInt32 depthBlockCount;
  DMDepthInfo* depthVPBlock;
  UInt32 depthBlockFlags;
  UInt32 depthBlockReserved1;
  UInt32 depthBlockReserved2;
} DMDepthInfoBlock;

typedef struct {
  UInt32 displayModeFlags;
  VDSwitchInfoRec* displayModeSwitchInfo;
  VDResolutionInfoRec* displayModeResolutionInfo;
  void* displayModeTimingInfo;
  DMDepthInfoBlock* displayModeDepthBlockInfo;
  UInt32 displayModeVersion;
  StringPtr displayModeName;
  void* displayModeDisplayInfo;
} DMDisplayModeListEntryRec;

#pragma pack(pop)

#ifdef __i386__
_Static_assert(offsetof(VPBlock, vpPixelSize) == 32, "VPBlock");
_Static_assert(sizeof(DMDepthInfo) == 20, "DMDepthInfo");
_Static_assert(sizeof(DMDisplayModeListEntryRec) == 32, "DMDisplayModeListEntryRec");
#endif

enum {
  kDepthCount = 2,
  kDepthMode16 = 132,
  kDepthMode32 = 133,
};

typedef struct {
  DMDisplayModeListEntryRec entry;
  VDResolutionInfoRec resolution;
  DMDepthInfoBlock depth_block;
  DMDepthInfo depths[kDepthCount];
  VPBlock vp[kDepthCount];
  VDSwitchInfoRec switches[kDepthCount];
  unsigned char name[32];
} dm_mode;

typedef struct {
  uint32_t count;
  dm_mode* modes;
} dm_list;

int DMGetDisplayIDByGDevice(GDHandle device, DisplayIDType* id,
                            Boolean fail_to_main) {
  if (id) {
    *id = kMainDisplayID;
  }
  return noErr;
}

int DMGetGDeviceByDisplayID(DisplayIDType id, GDHandle* device,
                            Boolean fail_to_main) {
  if (device) {
    *device = hle_main_device();
  }
  return noErr;
}

GDHandle DMGetFirstScreenDevice(Boolean active_only) {
  return hle_main_device();
}

GDHandle DMGetNextScreenDevice(GDHandle device, Boolean active_only) {
  return NULL;
}

void* NewDMDisplayModeListIteratorUPP(void* proc) {
  return proc;
}

void DisposeDMDisplayModeListIteratorUPP(void* upp) {
}

int DMNewDisplayModeList(DisplayIDType id, UInt32 flags, void* reserved,
                         UInt32* count, dm_list** out) {
  hle_display_mode found[kMaxModes];
  int n = hle_display_modes(found, kMaxModes);
  dm_list* list = calloc(1, sizeof(*list));
  list->count = n;
  list->modes = calloc(n, sizeof(dm_mode));
  static const SInt16 kDepths[kDepthCount] = { 16, 32 };
  for (int i = 0; i < n; i++) {
    dm_mode* m = &list->modes[i];
    UInt32 mode_id = 0x1000 + i;
    m->entry.displayModeFlags = 0;
    m->entry.displayModeSwitchInfo = &m->switches[1];
    m->entry.displayModeResolutionInfo = &m->resolution;
    m->entry.displayModeDepthBlockInfo = &m->depth_block;
    m->entry.displayModeVersion = 1;
    snprintf((char*)m->name + 1, sizeof(m->name) - 1, "%dx%d", found[i].width,
             found[i].height);
    m->name[0] = strlen((char*)m->name + 1);
    m->entry.displayModeName = m->name;
    m->resolution.csDisplayModeID = mode_id;
    m->resolution.csHorizontalPixels = found[i].width;
    m->resolution.csVerticalLines = found[i].height;
    m->resolution.csRefreshRate = found[i].refresh << 16;
    m->resolution.csMaxDepthMode = kDepthMode32;
    m->depth_block.depthBlockCount = kDepthCount;
    m->depth_block.depthVPBlock = m->depths;
    for (int d = 0; d < kDepthCount; d++) {
      m->switches[d].csMode = d == 0 ? kDepthMode16 : kDepthMode32;
      m->switches[d].csData = mode_id;
      m->depths[d].depthSwitchInfo = &m->switches[d];
      m->depths[d].depthVPBlock = &m->vp[d];
      m->vp[d].vpBounds.bottom = found[i].height;
      m->vp[d].vpBounds.right = found[i].width;
      m->vp[d].vpRowBytes = found[i].width * kDepths[d] / 8;
      m->vp[d].vpVersion = 4;
      m->vp[d].vpHRes = m->vp[d].vpVRes = 72 << 16;
      m->vp[d].vpPixelType = 16;
      m->vp[d].vpPixelSize = kDepths[d];
      m->vp[d].vpCmpCount = 3;
      m->vp[d].vpCmpSize = kDepths[d] == 16 ? 5 : 8;
    }
  }
  if (count) {
    *count = n;
  }
  if (out) {
    *out = list;
  }
  return noErr;
}

int DMGetIndexedDisplayModeFromList(
    dm_list* list, UInt32 index, UInt32 reserved,
    void (*proc)(void* user, UInt32 index, DMDisplayModeListEntryRec* entry),
    void* user) {
  if (!list || index >= list->count) {
    return paramErr;
  }
  if (proc) {
    proc(user, index, &list->modes[index].entry);
  }
  return noErr;
}

int DMDisposeList(dm_list* list) {
  if (list) {
    free(list->modes);
    free(list);
  }
  return noErr;
}
