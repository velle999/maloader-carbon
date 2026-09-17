// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// QuickDraw without the screen: ports, GWorlds with real pixels (1, 16 or
// 32 bits deep), colors, pens, rectangles, rectangular regions and pictures
// that draw nothing. Text is drawn by ATSUI (atsui.c).
//
// The game paints a window black before OpenGL takes it over and shows
// nothing else through QuickDraw, so fills reach a GWorld's pixels and are
// dropped on window ports. Quartz drawing is stubbed at the end: images do
// not load and contexts are not made.

#define _GNU_SOURCE

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

Handle GetResource(OSType type, SInt16 id);

typedef uint8_t Pattern[8];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static hle_port screen_port;
static hle_port* current;
static GDHandle current_device;
static int32_t random_seed = 1;

void hle_port_init(hle_port* port, const Rect* bounds) {
  memset(port, 0, sizeof(*port));
  port->magic = kHleMagicPort;
  if (bounds) {
    port->bounds = *bounds;
  }
  port->fore = (RGBColor){ 0, 0, 0 };
  port->back = (RGBColor){ 0xFFFF, 0xFFFF, 0xFFFF };
  port->pen.pnSize = (Point){ 1, 1 };
  port->pen.pnMode = 8;  // patCopy
  memset(port->pen.pnPat, 0xFF, sizeof(port->pen.pnPat));
  port->clip = (RgnHandle)hle_handle_new(NULL, sizeof(Region), 0);
  (*port->clip)->rgnSize = sizeof(Region);
  (*port->clip)->rgnBBox = port->bounds;
}

hle_port* hle_port_current(void) {
  pthread_mutex_lock(&lock);
  if (!current) {
    GDHandle device = hle_main_device();
    hle_port_init(&screen_port, &(*device)->gdRect);
    screen_port.pixmap = (*device)->gdPMap;
    screen_port.device = device;
    current = &screen_port;
    current_device = device;
  }
  hle_port* port = current;
  pthread_mutex_unlock(&lock);
  return port;
}

void hle_port_forget(hle_port* port) {
  pthread_mutex_lock(&lock);
  if (current == port) {
    current = &screen_port;
  }
  pthread_mutex_unlock(&lock);
}

static hle_port* port_of(void* ref) {
  hle_port* port = ref;
  return port && port->magic == kHleMagicPort ? port : NULL;
}

// ---------------------------------------------------------------------------
// Ports and GWorlds

void GetPort(hle_port** port) {
  if (port) {
    *port = hle_port_current();
  }
}

void SetPort(hle_port* port) {
  hle_port_current();
  if (port_of(port)) {
    current = port;
  }
}

void GetGWorld(hle_port** port, GDHandle* device) {
  if (port) {
    *port = hle_port_current();
  }
  if (device) {
    *device = current_device ? current_device : hle_main_device();
  }
}

void SetGWorld(hle_port* port, GDHandle device) {
  SetPort(port);
  current_device = device ? device
                          : port_of(port) && port->device ? port->device
                                                          : hle_main_device();
}

Rect* GetPortBounds(hle_port* port, Rect* rect) {
  port = port_of(port);
  if (rect) {
    if (port) {
      *rect = port->bounds;
    } else {
      memset(rect, 0, sizeof(*rect));
    }
  }
  return rect;
}

void SetPortBounds(hle_port* port, const Rect* rect) {
  port = port_of(port);
  if (port && rect) {
    port->bounds = *rect;
  }
}

PixMapHandle GetPortPixMap(hle_port* port) {
  port = port_of(port);
  if (!port) {
    return NULL;
  }
  if (!port->pixmap) {
    port->pixmap = (PixMapHandle)hle_handle_new(NULL, sizeof(PixMap), 0);
    PixMap* p = *port->pixmap;
    p->rowBytes = (SInt16)0x8000;
    p->bounds = port->bounds;
    p->pmVersion = 4;
    p->hRes = p->vRes = 72 << 16;
    p->pixelType = 16;
    p->pixelSize = 32;
    p->cmpCount = 3;
    p->cmpSize = 8;
    p->pixelFormat = 'ARGB';
  }
  return port->pixmap;
}

PixMapHandle GetGWorldPixMap(hle_port* port) {
  return GetPortPixMap(port);
}

// A port's pixel map, which CopyBits takes as a BitMap.
const PixMap* GetPortBitMapForCopyBits(hle_port* port) {
  PixMapHandle pixmap = GetPortPixMap(port);
  return pixmap ? *pixmap : NULL;
}

hle_port* CreateNewPort(void) {
  hle_port* port = calloc(1, sizeof(*port));
  Rect empty = { 0, 0, 0, 0 };
  hle_port_init(port, &empty);
  port->device = hle_main_device();
  return port;
}

void DisposePort(hle_port* port) {
  port = port_of(port);
  if (port && port != &screen_port && !port->window) {
    if (current == port) {
      current = &screen_port;
    }
    port->magic = 0;
    free(port);
  }
}

enum {
  k1MonochromePixelFormat = 0x00000001,
  k16BE555PixelFormat = 0x00000010,
  k32ARGBPixelFormat = 0x00000020,
  qdErrMemFull = -108,
};

static int32_t make_gworld(hle_port** out, int depth, UInt32 format,
                           const Rect* bounds, GDHandle device, uint8_t* pixels,
                           int32_t row_bytes) {
  if (!out || !bounds) {
    return paramErr;
  }
  int width = bounds->right - bounds->left;
  int height = bounds->bottom - bounds->top;
  if (width < 0 || height < 0) {
    return paramErr;
  }
  hle_port* port = calloc(1, sizeof(*port));
  hle_port_init(port, bounds);
  port->device = device ? device : hle_main_device();
  if (!pixels) {
    row_bytes = ((width * depth + 31) / 32) * 4;
    port->pixels = calloc((size_t)row_bytes * (height ? height : 1), 1);
    if (!port->pixels) {
      free(port);
      return qdErrMemFull;
    }
    pixels = port->pixels;
  }
  PixMap* p = *GetPortPixMap(port);
  p->baseAddr = (Ptr)pixels;
  p->rowBytes = (SInt16)(0x8000 | (row_bytes & 0x7FFF));
  p->bounds = *bounds;
  p->pixelSize = depth;
  p->cmpCount = depth == 1 ? 1 : 3;
  p->cmpSize = depth == 1 ? 1 : depth == 16 ? 5 : 8;
  p->pixelFormat = format;
  *out = port;
  return noErr;
}

// Depths other than 1 and 16 get 32 bits.
int32_t NewGWorld(hle_port** out, SInt16 depth, const Rect* bounds,
                  Handle color_table, GDHandle device, UInt32 flags) {
  int bits = depth == 1 || depth == 16 ? depth : 32;
  return make_gworld(out, bits,
                     bits == 1    ? (UInt32)k1MonochromePixelFormat
                     : bits == 16 ? (UInt32)k16BE555PixelFormat
                                  : (UInt32)k32ARGBPixelFormat,
                     bounds, device, NULL, 0);
}

int32_t NewGWorldFromPtr(hle_port** out, UInt32 format, const Rect* bounds,
                         Handle color_table, GDHandle device, UInt32 flags,
                         Ptr buffer, int32_t row_bytes) {
  int bits = format == k16BE555PixelFormat || format == 'B555' ? 16 : 32;
  return make_gworld(out, bits, format, bounds, device, (uint8_t*)buffer,
                     row_bytes);
}

void DisposeGWorld(hle_port* port) {
  port = port_of(port);
  if (!port || port->window || port == &screen_port) {
    return;
  }
  if (current == port) {
    current = &screen_port;
  }
  free(port->pixels);
  if (port->pixmap) {
    DisposeHandle((Handle)port->pixmap);
  }
  if (port->clip) {
    DisposeHandle((Handle)port->clip);
  }
  port->magic = 0;
  free(port);
}

int QDFlushPortBuffer(hle_port* port, RgnHandle region) {
  return noErr;
}

void SetQDGlobalsRandomSeed(int32_t seed) {
  random_seed = seed;
}

int32_t GetQDGlobalsRandomSeed(void) {
  return random_seed;
}

Pattern* GetQDGlobalsBlack(Pattern* black) {
  memset(*black, 0xFF, sizeof(Pattern));
  return black;
}

Pattern* GetQDGlobalsWhite(Pattern* white) {
  memset(*white, 0, sizeof(Pattern));
  return white;
}

Pattern* GetQDGlobalsGray(Pattern* gray) {
  static const Pattern kGray = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
  memcpy(*gray, kGray, sizeof(Pattern));
  return gray;
}

Pattern* GetQDGlobalsLightGray(Pattern* gray) {
  static const Pattern kLightGray = { 0x88, 0x22, 0x88, 0x22,
                                      0x88, 0x22, 0x88, 0x22 };
  memcpy(*gray, kLightGray, sizeof(Pattern));
  return gray;
}

Pattern* GetQDGlobalsDarkGray(Pattern* gray) {
  static const Pattern kDarkGray = { 0x77, 0xDD, 0x77, 0xDD,
                                     0x77, 0xDD, 0x77, 0xDD };
  memcpy(*gray, kDarkGray, sizeof(Pattern));
  return gray;
}

GDHandle GetGWorldDevice(hle_port* port) {
  port = port_of(port);
  return port && port->device ? port->device : hle_main_device();
}

// ---------------------------------------------------------------------------
// Pixel access

// Pixels never move, so locking always succeeds.
unsigned int LockPixels(PixMapHandle pixmap) {
  return pixmap != NULL;
}

void UnlockPixels(PixMapHandle pixmap) {
}

Ptr GetPixBaseAddr(PixMapHandle pixmap) {
  return pixmap && *pixmap ? (*pixmap)->baseAddr : NULL;
}

int32_t GetPixRowBytes(PixMapHandle pixmap) {
  return pixmap && *pixmap ? (*pixmap)->rowBytes & 0x3FFF : 0;
}

static int luminance_is_dark(const RGBColor* color) {
  return (color->red * 299u + color->green * 587u + color->blue * 114u) /
             1000u < 0x8000;
}

// The pixel at (h, v) of the current port, where it has pixels; white
// elsewhere.
void GetCPixel(SInt16 h, SInt16 v, RGBColor* color) {
  hle_port* port = hle_port_current();
  *color = (RGBColor){ 0xFFFF, 0xFFFF, 0xFFFF };
  if (!port->pixels || !port->pixmap) {
    return;
  }
  PixMap* p = *port->pixmap;
  if (h < p->bounds.left || h >= p->bounds.right || v < p->bounds.top ||
      v >= p->bounds.bottom) {
    return;
  }
  int row_bytes = p->rowBytes & 0x3FFF;
  uint8_t* row = port->pixels + (v - p->bounds.top) * row_bytes;
  int i = h - p->bounds.left;
  if (p->pixelSize == 1) {
    if (row[i / 8] & (0x80 >> (i % 8))) {
      *color = (RGBColor){ 0, 0, 0 };
    }
  } else if (p->pixelSize == 16) {
    uint16_t px = (uint16_t)(row[i * 2] << 8 | row[i * 2 + 1]);
    color->red = ((px >> 10) & 0x1F) * 0xFFFF / 0x1F;
    color->green = ((px >> 5) & 0x1F) * 0xFFFF / 0x1F;
    color->blue = (px & 0x1F) * 0xFFFF / 0x1F;
  } else {
    color->red = row[i * 4 + 1] * 0x101;
    color->green = row[i * 4 + 2] * 0x101;
    color->blue = row[i * 4 + 3] * 0x101;
  }
}

void hle_port_blend(hle_port* port, int h, int v, int coverage,
                    const RGBColor* fore) {
  if (!port || !port->pixels || !port->pixmap || coverage <= 0) {
    return;
  }
  PixMap* p = *port->pixmap;
  if (h < p->bounds.left || h >= p->bounds.right || v < p->bounds.top ||
      v >= p->bounds.bottom) {
    return;
  }
  if (coverage > 255) {
    coverage = 255;
  }
  int row_bytes = p->rowBytes & 0x3FFF;
  uint8_t* row = port->pixels + (v - p->bounds.top) * row_bytes;
  int i = h - p->bounds.left;
  if (p->pixelSize == 1) {
    if (coverage >= 128) {
      if (luminance_is_dark(fore)) {
        row[i / 8] |= 0x80 >> (i % 8);
      } else {
        row[i / 8] &= ~(0x80 >> (i % 8));
      }
    }
  } else if (p->pixelSize == 16) {
    uint16_t px = (uint16_t)(row[i * 2] << 8 | row[i * 2 + 1]);
    int channels[3] = { (px >> 10) & 0x1F, (px >> 5) & 0x1F, px & 0x1F };
    int fores[3] = { fore->red >> 11, fore->green >> 11, fore->blue >> 11 };
    uint16_t out = 0;
    for (int c = 0; c < 3; c++) {
      int mixed = (channels[c] * (255 - coverage) + fores[c] * coverage + 127) /
                  255;
      out = (uint16_t)(out << 5 | mixed);
    }
    row[i * 2] = out >> 8;
    row[i * 2 + 1] = out;
  } else {
    uint8_t* px = row + i * 4;
    int fores[3] = { fore->red >> 8, fore->green >> 8, fore->blue >> 8 };
    px[0] = 0xFF;
    for (int c = 0; c < 3; c++) {
      px[1 + c] = (px[1 + c] * (255 - coverage) + fores[c] * coverage + 127) /
                  255;
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing into GWorld pixels

// Fills |r| with |color| where the current port has pixels of its own.
static void fill(const Rect* r, const RGBColor* color) {
  hle_port* port = hle_port_current();
  if (!r || !port->pixels || !port->pixmap) {
    return;
  }
  PixMap* p = *port->pixmap;
  int row_bytes = p->rowBytes & 0x7FFF;
  int top = r->top > p->bounds.top ? r->top : p->bounds.top;
  int left = r->left > p->bounds.left ? r->left : p->bounds.left;
  int bottom = r->bottom < p->bounds.bottom ? r->bottom : p->bounds.bottom;
  int right = r->right < p->bounds.right ? r->right : p->bounds.right;
  for (int y = top; y < bottom; y++) {
    uint8_t* row = port->pixels + (y - p->bounds.top) * row_bytes;
    for (int x = left; x < right; x++) {
      int i = x - p->bounds.left;
      if (p->pixelSize == 1) {
        if (luminance_is_dark(color)) {
          row[i / 8] |= 0x80 >> (i % 8);
        } else {
          row[i / 8] &= ~(0x80 >> (i % 8));
        }
      } else if (p->pixelSize == 16) {
        // xRRRRRGGGGGBBBBB, big-endian
        uint16_t v = (color->red >> 11) << 10 | (color->green >> 11) << 5 |
                     (color->blue >> 11);
        row[i * 2] = v >> 8;
        row[i * 2 + 1] = v;
      } else {
        uint8_t* px = row + i * 4;
        px[0] = 0xFF;
        px[1] = color->red >> 8;
        px[2] = color->green >> 8;
        px[3] = color->blue >> 8;
      }
    }
  }
}

static int pattern_is(const Pattern* pat, uint8_t byte) {
  for (int i = 0; i < 8; i++) {
    if ((*pat)[i] != byte) {
      return 0;
    }
  }
  return 1;
}

void PaintRect(const Rect* r) {
  hle_port* port = hle_port_current();
  fill(r, &port->fore);
}

void EraseRect(const Rect* r) {
  hle_port* port = hle_port_current();
  fill(r, &port->back);
}

// Patterns other than solid ones draw as the foreground color.
void FillRect(const Rect* r, const Pattern* pat) {
  hle_port* port = hle_port_current();
  fill(r, pat && pattern_is(pat, 0) ? &port->back : &port->fore);
}

int CopyBits(const PixMap* src, const PixMap* dst, const Rect* src_rect,
             const Rect* dst_rect, SInt16 mode, RgnHandle mask) {
  if (!src || !dst || !src->baseAddr || !dst->baseAddr || !src_rect ||
      !dst_rect || src->pixelSize != dst->pixelSize || src->pixelSize < 8) {
    return noErr;
  }
  int bytes = src->pixelSize / 8;
  int src_row = src->rowBytes & 0x7FFF;
  int dst_row = dst->rowBytes & 0x7FFF;
  int sw = src_rect->right - src_rect->left;
  int sh = src_rect->bottom - src_rect->top;
  int dw = dst_rect->right - dst_rect->left;
  int dh = dst_rect->bottom - dst_rect->top;
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
    return noErr;
  }
  for (int y = 0; y < dh; y++) {
    int dy = dst_rect->top + y - dst->bounds.top;
    int sy = src_rect->top + y * sh / dh - src->bounds.top;
    if (dy < 0 || dy >= dst->bounds.bottom - dst->bounds.top || sy < 0 ||
        sy >= src->bounds.bottom - src->bounds.top) {
      continue;
    }
    for (int x = 0; x < dw; x++) {
      int dx = dst_rect->left + x - dst->bounds.left;
      int sx = src_rect->left + x * sw / dw - src->bounds.left;
      if (dx < 0 || dx >= dst->bounds.right - dst->bounds.left || sx < 0 ||
          sx >= src->bounds.right - src->bounds.left) {
        continue;
      }
      memcpy(dst->baseAddr + dy * dst_row + dx * bytes,
             src->baseAddr + sy * src_row + sx * bytes, bytes);
    }
  }
  return noErr;
}

// ---------------------------------------------------------------------------
// Colors and pens

static const struct {
  int32_t color;
  RGBColor rgb;
} kClassicColors[] = {
  { 33, { 0, 0, 0 } },                      // blackColor
  { 30, { 0xFFFF, 0xFFFF, 0xFFFF } },       // whiteColor
  { 205, { 0xFFFF, 0, 0 } },                // redColor
  { 341, { 0, 0xFFFF, 0 } },                // greenColor
  { 409, { 0, 0, 0xFFFF } },                // blueColor
  { 273, { 0, 0xFFFF, 0xFFFF } },           // cyanColor
  { 137, { 0xFFFF, 0, 0xFFFF } },           // magentaColor
  { 69, { 0xFFFF, 0xFFFF, 0 } },            // yellowColor
};

static RGBColor classic_color(int32_t color) {
  for (size_t i = 0; i < sizeof(kClassicColors) / sizeof(kClassicColors[0]);
       i++) {
    if (kClassicColors[i].color == color) {
      return kClassicColors[i].rgb;
    }
  }
  return (RGBColor){ 0, 0, 0 };
}

void RGBForeColor(const RGBColor* color) {
  if (color) {
    hle_port_current()->fore = *color;
  }
}

void RGBBackColor(const RGBColor* color) {
  if (color) {
    hle_port_current()->back = *color;
  }
}

void ForeColor(int32_t color) {
  hle_port_current()->fore = classic_color(color);
}

void BackColor(int32_t color) {
  hle_port_current()->back = classic_color(color);
}

void GetForeColor(RGBColor* color) {
  if (color) {
    *color = hle_port_current()->fore;
  }
}

void GetBackColor(RGBColor* color) {
  if (color) {
    *color = hle_port_current()->back;
  }
}

void GetPenState(PenState* pen) {
  if (pen) {
    *pen = hle_port_current()->pen;
  }
}

void SetPenState(const PenState* pen) {
  if (pen) {
    hle_port_current()->pen = *pen;
  }
}

void MoveTo(SInt16 h, SInt16 v) {
  hle_port* port = hle_port_current();
  port->pen.pnLoc = (Point){ v, h };
}

void GetPortForeColor(hle_port* port, RGBColor* color) {
  port = port_of(port);
  if (color) {
    *color = port ? port->fore : (RGBColor){ 0, 0, 0 };
  }
}

SInt16 GetPortTextFont(hle_port* port) {
  port = port_of(port);
  return port ? port->text_font : 0;
}

// A Style is a byte; the result fills EAX.
unsigned int GetPortTextFace(hle_port* port) {
  port = port_of(port);
  return port ? port->text_face : 0;
}

void PenNormal(void) {
  hle_port* port = hle_port_current();
  port->pen.pnSize = (Point){ 1, 1 };
  port->pen.pnMode = 8;
  memset(port->pen.pnPat, 0xFF, sizeof(port->pen.pnPat));
}

// ---------------------------------------------------------------------------
// Palettes: kept for the game, never applied to a device.

#pragma pack(push, 2)
typedef struct {
  RGBColor ciRGB;
  SInt16 ciUsage;
  SInt16 ciTolerance;
  SInt16 ciDataFields[3];
} ColorInfo;

typedef struct {
  SInt16 pmEntries;
  SInt16 pmDataFields[7];
  ColorInfo pmInfo[1];
} Palette;
#pragma pack(pop)

typedef Palette** PaletteHandle;

PaletteHandle NewPalette(int32_t entries, Handle colors, int32_t usage,
                         int32_t tolerance) {
  entries = (SInt16)entries;
  if (entries < 0) {
    return NULL;
  }
  Size size = offsetof(Palette, pmInfo) +
              sizeof(ColorInfo) * (entries > 0 ? entries : 1);
  PaletteHandle palette = (PaletteHandle)hle_handle_new(NULL, size, 0);
  if (!palette) {
    return NULL;
  }
  memset(*palette, 0, size);
  (*palette)->pmEntries = (SInt16)entries;
  for (int i = 0; i < entries; i++) {
    (*palette)->pmInfo[i].ciUsage = (SInt16)usage;
    (*palette)->pmInfo[i].ciTolerance = (SInt16)tolerance;
  }
  return palette;
}

void DisposePalette(PaletteHandle palette) {
  if (palette) {
    DisposeHandle((Handle)palette);
  }
}

void SetEntryColor(PaletteHandle palette, int32_t entry,
                   const RGBColor* color) {
  entry = (SInt16)entry;
  if (palette && color && entry >= 0 && entry < (*palette)->pmEntries) {
    (*palette)->pmInfo[entry].ciRGB = *color;
  }
}

void GetEntryColor(PaletteHandle palette, int32_t entry, RGBColor* color) {
  entry = (SInt16)entry;
  if (palette && color && entry >= 0 && entry < (*palette)->pmEntries) {
    *color = (*palette)->pmInfo[entry].ciRGB;
  }
}

// ---------------------------------------------------------------------------
// Rectangles and points

void SetRect(Rect* r, SInt16 left, SInt16 top, SInt16 right, SInt16 bottom) {
  r->left = left;
  r->top = top;
  r->right = right;
  r->bottom = bottom;
}

void OffsetRect(Rect* r, SInt16 dh, SInt16 dv) {
  r->left += dh;
  r->right += dh;
  r->top += dv;
  r->bottom += dv;
}

Boolean EmptyRect(const Rect* r) {
  return r->bottom <= r->top || r->right <= r->left;
}

Boolean EqualRect(const Rect* a, const Rect* b) {
  return a->top == b->top && a->left == b->left && a->bottom == b->bottom &&
         a->right == b->right;
}

Boolean PtInRect(Point pt, const Rect* r) {
  return pt.v >= r->top && pt.v < r->bottom && pt.h >= r->left &&
         pt.h < r->right;
}

// The point moved into the rectangle, as vertical then horizontal words.
int32_t PinRect(const Rect* r, Point pt) {
  SInt16 v = pt.v < r->top ? r->top : pt.v >= r->bottom ? r->bottom - 1 : pt.v;
  SInt16 h = pt.h < r->left ? r->left : pt.h >= r->right ? r->right - 1 : pt.h;
  return (int32_t)((uint32_t)(UInt16)v << 16 | (UInt16)h);
}

void AddPt(Point src, Point* dst) {
  dst->v += src.v;
  dst->h += src.h;
}

void SubPt(Point src, Point* dst) {
  dst->v -= src.v;
  dst->h -= src.h;
}

Boolean EqualPt(Point a, Point b) {
  return a.v == b.v && a.h == b.h;
}

// A window port's local origin is the top left of its content.
void LocalToGlobal(Point* pt) {
  hle_port* port = hle_port_current();
  if (port->window) {
    pt->v += port->window->content.top - port->bounds.top;
    pt->h += port->window->content.left - port->bounds.left;
  }
}

void GlobalToLocal(Point* pt) {
  hle_port* port = hle_port_current();
  if (port->window) {
    pt->v -= port->window->content.top - port->bounds.top;
    pt->h -= port->window->content.left - port->bounds.left;
  }
}

// ---------------------------------------------------------------------------
// Regions: rectangles only.

RgnHandle NewRgn(void) {
  RgnHandle rgn = (RgnHandle)hle_handle_new(NULL, sizeof(Region), 0);
  (*rgn)->rgnSize = sizeof(Region);
  return rgn;
}

void DisposeRgn(RgnHandle rgn) {
  if (rgn) {
    DisposeHandle((Handle)rgn);
  }
}

Rect* GetRegionBounds(RgnHandle rgn, Rect* bounds) {
  if (bounds) {
    if (rgn) {
      *bounds = (*rgn)->rgnBBox;
    } else {
      memset(bounds, 0, sizeof(*bounds));
    }
  }
  return bounds;
}

// A Boolean result fills EAX.
unsigned int EmptyRgn(RgnHandle rgn) {
  if (!rgn) {
    return 1;
  }
  const Rect* r = &(*rgn)->rgnBBox;
  return r->bottom <= r->top || r->right <= r->left;
}

// Everything in a port is visible: nothing overlaps it.
RgnHandle GetPortVisibleRegion(hle_port* port, RgnHandle rgn) {
  port = port_of(port);
  if (rgn) {
    if (port) {
      (*rgn)->rgnBBox = port->bounds;
    } else {
      memset(&(*rgn)->rgnBBox, 0, sizeof(Rect));
    }
  }
  return rgn;
}

void RectRgn(RgnHandle rgn, const Rect* r) {
  if (rgn && r) {
    (*rgn)->rgnBBox = *r;
  }
}

RgnHandle GetGrayRgn(void) {
  static RgnHandle gray;
  if (!gray) {
    gray = NewRgn();
  }
  (*gray)->rgnBBox = (*hle_main_device())->gdRect;
  return gray;
}

void ClipRect(const Rect* r) {
  hle_port* port = hle_port_current();
  if (r && port->clip) {
    (*port->clip)->rgnBBox = *r;
  }
}

void SetClip(RgnHandle rgn) {
  hle_port* port = hle_port_current();
  if (rgn && port->clip) {
    (*port->clip)->rgnBBox = (*rgn)->rgnBBox;
  }
}

void GetClip(RgnHandle rgn) {
  hle_port* port = hle_port_current();
  if (rgn && port->clip) {
    (*rgn)->rgnBBox = (*port->clip)->rgnBBox;
  }
}

// ---------------------------------------------------------------------------
// Pictures draw nothing.

Handle OpenCPicture(const void* params) {
  return hle_handle_new(NULL, 10, 0);
}

void ClosePicture(void) {
}

void DrawPicture(Handle picture, const Rect* dst) {
}

void KillPicture(Handle picture) {
  if (picture && !hle_handle_is_resource(picture)) {
    DisposeHandle(picture);
  }
}

Handle GetPicture(SInt16 id) {
  return GetResource('PICT', id);
}

// ---------------------------------------------------------------------------
// Quartz: geometry works; images and contexts do not exist.

typedef float CGFloat;

typedef struct {
  CGFloat x, y, width, height;
} CGRect;

typedef struct {
  CGFloat a, b, c, d, tx, ty;
} CGAffineTransform;

const CGAffineTransform CGAffineTransformIdentity = { 1, 0, 0, 1, 0, 0 };

CGRect CGRectInset(CGRect r, CGFloat dx, CGFloat dy) {
  CGRect out = { r.x + dx, r.y + dy, r.width - 2 * dx, r.height - 2 * dy };
  if (out.width < 0 || out.height < 0) {
    // CGRectNull
    CGRect null = { 1.0f / 0.0f, 1.0f / 0.0f, 0, 0 };
    return null;
  }
  return out;
}

CGAffineTransform CGAffineTransformScale(CGAffineTransform t, CGFloat sx,
                                         CGFloat sy) {
  CGAffineTransform out = { t.a * sx, t.b * sx, t.c * sy, t.d * sy, t.tx,
                            t.ty };
  return out;
}

int CreateCGContextForPort(hle_port* port, void** context) {
  if (context) {
    *context = NULL;
  }
  cf_warn_once("CreateCGContextForPort: Quartz drawing is not supported");
  return paramErr;
}

void* CGColorSpaceCreateDeviceRGB(void) {
  static int device_rgb;
  return &device_rgb;
}

void CGColorSpaceRelease(void* space) {
}

void* CGDataProviderCreateWithURL(CFURLRef url) {
  return NULL;
}

void CGDataProviderRelease(void* provider) {
}

void* CGImageCreateWithPNGDataProvider(void* provider, const CGFloat* decode,
                                       int interpolate, int intent) {
  return NULL;
}

void CGImageRelease(void* image) {
}

size_t CGImageGetWidth(void* image) {
  return 0;
}

size_t CGImageGetHeight(void* image) {
  return 0;
}

void* CGBitmapContextCreate(void* data, size_t width, size_t height,
                            size_t bits_per_component, size_t bytes_per_row,
                            void* space, uint32_t info) {
  return NULL;
}

void CGContextRelease(void* context) {
}

void CGContextDrawImage(void* context, CGRect rect, void* image) {
}

void CGContextClearRect(void* context, CGRect rect) {
}

void CGContextStrokeRect(void* context, CGRect rect) {
}

void CGContextSaveGState(void* context) {
}

void CGContextRestoreGState(void* context) {
}

void CGContextSetTextMatrix(void* context, CGAffineTransform t) {
}
