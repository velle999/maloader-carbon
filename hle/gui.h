// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The objects behind QuickDraw, the Window Manager, controls and menus.
//
// Nothing here draws the classic way. Windows the game renders into with
// OpenGL get an SDL window; dialogs from NIBs stay models the game queries
// and are answered for the user (see windows.c).

#ifndef HLE_GUI_H_
#define HLE_GUI_H_

#include "toolbox.h"

typedef int32_t Fixed;

#pragma pack(push, 2)

typedef struct {
  UInt16 red;
  UInt16 green;
  UInt16 blue;
} RGBColor;

typedef struct {
  Ptr baseAddr;
  SInt16 rowBytes;  // 0x8000 marks a PixMap rather than a BitMap
  Rect bounds;
  SInt16 pmVersion;
  SInt16 packType;
  SInt32 packSize;
  Fixed hRes;
  Fixed vRes;
  SInt16 pixelType;
  SInt16 pixelSize;
  SInt16 cmpCount;
  SInt16 cmpSize;
  OSType pixelFormat;
  Handle pmTable;
  void* pmExt;
} PixMap;

typedef struct {
  SInt16 gdRefNum;
  SInt16 gdID;
  SInt16 gdType;
  Handle gdITable;
  SInt16 gdResPref;
  Handle gdSearchProc;
  Handle gdCompProc;
  SInt16 gdFlags;
  PixMap** gdPMap;
  SInt32 gdRefCon;
  Handle gdNextGD;
  Rect gdRect;
  SInt32 gdMode;
  SInt16 gdCCBytes;
  SInt16 gdCCDepth;
  Handle gdCCXData;
  Handle gdCCXMask;
  Handle gdExt;
} GDevice;

typedef struct {
  Point pnLoc;
  Point pnSize;
  SInt16 pnMode;
  uint8_t pnPat[8];
} PenState;

// A region is only ever a rectangle here.
typedef struct {
  UInt16 rgnSize;
  Rect rgnBBox;
} Region;

#pragma pack(pop)

#ifdef __i386__
_Static_assert(sizeof(PixMap) == 50, "PixMap");
_Static_assert(offsetof(GDevice, gdRect) == 34, "GDevice.gdRect");
_Static_assert(sizeof(PenState) == 18, "PenState");
#endif

typedef PixMap** PixMapHandle;
typedef GDevice** GDHandle;
typedef Region** RgnHandle;

struct hle_window;
struct hle_control;
struct hle_menu;

// A graphics port: a window's, a GWorld's with its own pixels, or a bare
// port from CreateNewPort.
typedef struct hle_port {
  uint32_t magic;
  struct hle_window* window;
  Rect bounds;
  RGBColor fore;
  RGBColor back;
  RgnHandle clip;
  PenState pen;
  PixMapHandle pixmap;
  GDHandle device;
  uint8_t* pixels;  // owned by GWorlds from NewGWorld
  // The QuickDraw font: a family number (0 is the system font), a style and
  // a size in points (0 is the default size).
  SInt16 text_font;
  uint8_t text_face;
  SInt16 text_size;
} hle_port;

enum {
  kHleMagicPort = 'port',
  kHleMagicWindow = 'wind',
  kHleMagicControl = 'cntl',
  kHleMagicMenu = 'menu',
};

typedef struct hle_window {
  hle_target target;  // first, so a WindowRef is its own event target
  uint32_t magic;
  hle_port port;
  UInt32 window_class;
  UInt32 attributes;
  Rect content;  // the content area in global coordinates
  CFStringRef title;
  SInt32 refcon;
  SInt16 kind;
  int visible;
  int modified;
  RGBColor content_color;
  struct hle_window* next;  // front to back
  struct hle_control* root;
  const char* nib_name;  // the NIB object this window came from
  int answered;          // its modal loop has been given a command
  int is_dialog;         // made by GetNewDialog from a DLOG resource
  void* sdl_window;
} hle_window;

typedef struct hle_control_data {
  SInt16 part;
  OSType tag;
  UInt32 size;
  struct hle_control_data* next;
  uint8_t bytes[];
} hle_control_data;

typedef struct hle_control {
  hle_target target;  // first, as for windows
  uint32_t magic;
  hle_window* window;
  struct hle_control* parent;
  struct hle_control* children;
  struct hle_control* next;
  const char* nib_class;  // "IBCarbonButton" and so on
  int button_type;        // 1 for a default button, 2 for a cancel button
  OSType signature;
  SInt32 id;
  OSType command;
  Rect bounds;
  SInt32 value;
  SInt32 minimum;
  SInt32 maximum;
  CFStringRef title;
  int visible;
  int active;
  struct hle_menu* menu;  // a popup button's
  void* action;
  hle_control_data* data;
} hle_control;

typedef struct {
  CFStringRef text;
  OSType command;
  int enabled;
  int checked;
  int separator;
  struct hle_menu* submenu;
} hle_menu_item;

typedef struct hle_menu {
  hle_target target;  // first
  uint32_t magic;
  SInt16 id;
  CFStringRef title;
  int count;
  hle_menu_item* items;  // items[0] is item 1
  int enabled;
} hle_menu;

// QuickDraw (quickdraw.c)
hle_port* hle_port_current(void);
void hle_port_init(hle_port* port, const Rect* bounds);
// Makes the screen's port current again if |port| is, before it goes away.
void hle_port_forget(hle_port* port);
GDHandle hle_main_device(void);

// Windows (windows.c)
hle_window* hle_window_from(void* ref);
hle_window* hle_front_window(void);
// Whether |window| covers a captured display, which is how the game plays
// full screen, and HLE_WINDOWED has not asked for a window instead.
int hle_window_wants_fullscreen(const hle_window* window);

// Displays (display.c). Modes are the primary display's, in pixels.
typedef struct {
  int width;
  int height;
  int refresh;  // Hz, 0 when unknown
} hle_display_mode;
int hle_display_modes(hle_display_mode* modes, int max);
void hle_display_current(hle_display_mode* mode);
int hle_display_captured(void);

// QuickDraw (quickdraw.c): lays |coverage| (0-255) of |color| over the
// port's pixel at (h, v), in port coordinates. Does nothing on a port
// without pixels of its own or outside its bounds.
void hle_port_blend(hle_port* port, int h, int v, int coverage,
                    const RGBColor* color);

// SDL (sdl.c)
int hle_sdl_video(void);
// Creates or resizes the SDL window behind |window| for OpenGL drawing.
void* hle_sdl_window_for(hle_window* window, int fullscreen, int width,
                         int height);
void hle_sdl_window_destroy(hle_window* window);
// OpenGL (gl.c): moves the contexts drawing in |sdl_window| off it, before
// the window is destroyed.
void hle_gl_window_destroyed(void* sdl_window);
// The AGL context current on this thread, and whether |ref| is a context
// gl.c made, for gl_probe.c.
void* hle_gl_current_context(void);
int hle_gl_is_context(void* ref);

// Screenshots (screenshot.c): Print Screen asks for one, and the next frame
// swapped in |sdl_window| is saved.
void hle_screenshot_request(void);
void hle_screenshot_take(void* sdl_window);
int hle_sdl_switch_mode(const hle_display_mode* mode);
// Moves the pointer to a global position, in the game window's terms.
void hle_sdl_warp_mouse(int x, int y);

#endif  // HLE_GUI_H_
