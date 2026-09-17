// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The Window Manager, controls, menus, dialogs and alerts, and the NIBs
// that describe them.
//
// Windows are records the game queries, never drawn. One the game renders
// into gets an SDL window from the GL layer. Dialogs are answered for the
// user: when the game runs a modal loop for a NIB window, the loop hook
// sets any controls HLE_CONTROL_<signature> names, then sends the command
// of the window's default button, or the command HLE_DIALOG_<name> gives.
// Alerts print to stderr and take their default button.

#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gui.h"
#include "nib.h"

void SetPort(hle_port* port);

enum {
  errDataNotSupported = -30581,
  errUnknownControl = -30584,
  errInvalidWindowRef = -5600,
  errWindowRegionCodeInvalid = -30593,
  menuItemNotFoundErr = -5622,
  kIBCarbonRuntimeCantFindNibFile = -10960,
  kIBCarbonRuntimeObjectNotOfRequestedType = -10961,
  kIBCarbonRuntimeCantFindObject = -10962,
};

enum {
  kAlertWindowClass = 1,
  kMovableAlertWindowClass = 2,
  kModalWindowClass = 3,
  kMovableModalWindowClass = 4,
  kFloatingWindowClass = 5,
  kDocumentWindowClass = 6,
  kUtilityWindowClass = 8,
  kHelpWindowClass = 10,
  kSheetWindowClass = 11,
  kToolbarWindowClass = 12,
  kPlainWindowClass = 13,
  kOverlayWindowClass = 14,
  kSheetAlertWindowClass = 15,
  kAltPlainWindowClass = 16,
  kSimpleWindowClass = 18,
  kDrawerWindowClass = 20,
};

enum {
  kWindowTitleBarRgn = 0,
  kWindowTitleTextRgn = 1,
  kWindowDragRgn = 5,
  kWindowStructureRgn = 32,
  kWindowContentRgn = 33,
  kWindowUpdateRgn = 34,
  kWindowOpaqueRgn = 35,
  kWindowGlobalPortRgn = 40,
};

enum {
  inDesk = 0,
  inMenuBar = 1,
  inContent = 3,
  inDrag = 4,
};

enum {
  kHICommandFromControl = 1,
  kMenuItemAttrDisabled = 1 << 0,
  kMenuItemAttrSeparator = 1 << 6,
};

// ---------------------------------------------------------------------------
// Windows

static hle_window* windows;  // front to back

hle_window* hle_window_from(void* ref) {
  hle_window* w = ref;
  return w && w->magic == kHleMagicWindow ? w : NULL;
}

hle_window* hle_front_window(void) {
  for (hle_window* w = windows; w; w = w->next) {
    if (w->visible) {
      return w;
    }
  }
  return NULL;
}

static void unlink_window(hle_window* w) {
  for (hle_window** link = &windows; *link; link = &(*link)->next) {
    if (*link == w) {
      *link = w->next;
      break;
    }
  }
  w->next = NULL;
}

static void link_front(hle_window* w) {
  unlink_window(w);
  w->next = windows;
  windows = w;
}

static int title_bar_height(const hle_window* w) {
  switch (w->window_class) {
    case kAlertWindowClass:
    case kModalWindowClass:
    case kHelpWindowClass:
    case kSheetWindowClass:
    case kToolbarWindowClass:
    case kPlainWindowClass:
    case kOverlayWindowClass:
    case kSheetAlertWindowClass:
    case kAltPlainWindowClass:
    case kSimpleWindowClass:
    case kDrawerWindowClass:
      return 0;
    case kFloatingWindowClass:
    case kUtilityWindowClass:
      return 16;
    default:
      return 22;
  }
}

int hle_window_wants_fullscreen(const hle_window* w) {
  const char* windowed = getenv("HLE_WINDOWED");
  if ((windowed && *windowed == '1') || !hle_display_captured()) {
    return 0;
  }
  hle_display_mode mode;
  hle_display_current(&mode);
  return w->content.left <= 0 && w->content.top <= 0 &&
         w->content.right - w->content.left >= mode.width &&
         w->content.bottom - w->content.top >= mode.height;
}

static void set_content(hle_window* w, const Rect* r) {
  int width = r->right - r->left;
  int height = r->bottom - r->top;
  int resized = width != w->content.right - w->content.left ||
                height != w->content.bottom - w->content.top;
  w->content = *r;
  w->port.bounds.top = 0;
  w->port.bounds.left = 0;
  w->port.bounds.bottom = height;
  w->port.bounds.right = width;
  if (resized && w->sdl_window) {
    hle_sdl_window_for(w, hle_window_wants_fullscreen(w), width, height);
  }
}

static hle_window* new_window(size_t size, UInt32 window_class,
                              UInt32 attributes, const Rect* content) {
  hle_window* w = calloc(1, size);
  hle_target_init(&w->target, hle_application_target());
  w->magic = kHleMagicWindow;
  hle_port_init(&w->port, NULL);
  w->port.window = w;
  w->port.device = hle_main_device();
  w->window_class = window_class;
  w->attributes = attributes;
  w->content_color = (RGBColor){ 0xFFFF, 0xFFFF, 0xFFFF };
  w->kind = 8;  // userKind
  set_content(w, content);
  link_front(w);
  return w;
}

static void dispose_control(struct hle_control* c);

hle_window* NewCWindow(void* storage, const Rect* bounds,
                       ConstStringPtr title, Boolean visible, SInt16 proc_id,
                       void* behind, Boolean go_away, SInt32 refcon) {
  UInt32 window_class;
  switch (proc_id & 0xF) {
    case 1: window_class = kModalWindowClass; break;
    case 2:
    case 3: window_class = kPlainWindowClass; break;
    case 5: window_class = kMovableModalWindowClass; break;
    default: window_class = kDocumentWindowClass; break;
  }
  hle_window* w = new_window(sizeof(hle_window), window_class, 0, bounds);
  if (title) {
    char* text = hle_name_from_pascal(title);
    w->title = cf_string_from_utf8(text, strlen(text));
    free(text);
  }
  w->visible = visible;
  w->refcon = refcon;
  return w;
}

int CreateNewWindow(UInt32 window_class, UInt32 attributes,
                    const Rect* bounds, hle_window** out) {
  if (!bounds || !out) {
    return paramErr;
  }
  *out = new_window(sizeof(hle_window), window_class, attributes, bounds);
  cf_trace("CreateNewWindow(class %u, attributes %#x, %d,%d %dx%d) = %p",
           (unsigned)window_class, (unsigned)attributes, bounds->left,
           bounds->top, bounds->right - bounds->left,
           bounds->bottom - bounds->top, (void*)*out);
  return noErr;
}

void DisposeWindow(void* ref) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return;
  }
  hle_sdl_window_destroy(w);
  unlink_window(w);
  hle_port_forget(&w->port);
  if (w->root) {
    dispose_control(w->root);
  }
  hle_target_destroy(&w->target);
  if (w->title) {
    CFRelease(w->title);
  }
  if (w->port.clip) {
    DisposeHandle((Handle)w->port.clip);
  }
  if (w->port.pixmap) {
    DisposeHandle((Handle)w->port.pixmap);
  }
  free((char*)w->nib_name);
  w->magic = 0;
  free(w);
}

void ShowWindow(void* ref) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    w->visible = 1;
  }
}

void HideWindow(void* ref) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    w->visible = 0;
  }
}

Boolean IsWindowVisible(void* ref) {
  hle_window* w = hle_window_from(ref);
  return w && w->visible;
}

void SelectWindow(void* ref) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    link_front(w);
    hle_set_focus(&w->target);
  }
}

void BringToFront(void* ref) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    link_front(w);
  }
}

int ActivateWindow(void* ref, Boolean activate) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  if (activate) {
    hle_set_focus(&w->target);
  }
  return noErr;
}

hle_window* FrontWindow(void) {
  return hle_front_window();
}

hle_window* ActiveNonFloatingWindow(void) {
  for (hle_window* w = windows; w; w = w->next) {
    if (w->visible && w->window_class != kFloatingWindowClass &&
        w->window_class != kUtilityWindowClass) {
      return w;
    }
  }
  return NULL;
}

hle_window* GetNextWindow(void* ref) {
  hle_window* w = hle_window_from(ref);
  return w ? w->next : NULL;
}

SInt16 FindWindow(Point where, hle_window** out) {
  for (hle_window* w = windows; w; w = w->next) {
    if (!w->visible) {
      continue;
    }
    Rect structure = w->content;
    structure.top -= title_bar_height(w);
    if (where.v >= structure.top && where.v < structure.bottom &&
        where.h >= structure.left && where.h < structure.right) {
      if (out) {
        *out = w;
      }
      return where.v < w->content.top ? inDrag : inContent;
    }
  }
  if (out) {
    *out = NULL;
  }
  return inDesk;
}

int GetWindowBounds(void* ref, UInt16 region, Rect* out) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  if (!out) {
    return paramErr;
  }
  Rect r = w->content;
  int bar = title_bar_height(w);
  switch (region) {
    case kWindowContentRgn:
    case kWindowUpdateRgn:
    case kWindowOpaqueRgn:
    case kWindowGlobalPortRgn:
      break;
    case kWindowStructureRgn:
      r.top -= bar;
      break;
    case kWindowTitleBarRgn:
    case kWindowTitleTextRgn:
    case kWindowDragRgn:
      r.bottom = r.top;
      r.top -= bar;
      break;
    default:
      return errWindowRegionCodeInvalid;
  }
  *out = r;
  return noErr;
}

int SetWindowBounds(void* ref, UInt16 region, const Rect* bounds) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  if (!bounds) {
    return paramErr;
  }
  Rect r = *bounds;
  if (region == kWindowStructureRgn) {
    r.top += title_bar_height(w);
  } else if (region != kWindowContentRgn) {
    return errWindowRegionCodeInvalid;
  }
  set_content(w, &r);
  return noErr;
}

// Regions are rectangles here, so a window region is its bounds.
int GetWindowRegion(void* ref, UInt16 region, RgnHandle rgn) {
  if (!rgn) {
    return paramErr;
  }
  Rect r;
  int err = GetWindowBounds(ref, region, &r);
  if (err == noErr) {
    (*rgn)->rgnBBox = r;
  }
  return err;
}

Rect* GetWindowPortBounds(void* ref, Rect* out) {
  hle_window* w = hle_window_from(ref);
  if (out) {
    if (w) {
      *out = w->port.bounds;
    } else {
      memset(out, 0, sizeof(*out));
    }
  }
  return out;
}

// Centers the window on the main screen; alert positions sit a third of
// the way down. Cascades leave it where it is.
int RepositionWindow(void* ref, void* parent, UInt32 method) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  const Rect* screen = &(*hle_main_device())->gdRect;
  int width = w->content.right - w->content.left;
  int height = w->content.bottom - w->content.top;
  int bar = title_bar_height(w);
  int free_h = screen->right - screen->left - width;
  int free_v = screen->bottom - screen->top - height - bar;
  int top;
  switch (method) {
    case 1:
    case 2:
    case 3:
      top = free_v / 2;
      break;
    case 7:
    case 8:
    case 9:
      top = free_v / 3;
      break;
    default:
      return noErr;
  }
  Rect r = { .top = screen->top + top + bar,
             .left = screen->left + free_h / 2,
             .bottom = screen->top + top + bar + height,
             .right = screen->left + free_h / 2 + width };
  set_content(w, &r);
  return noErr;
}

hle_port* GetWindowPort(void* ref) {
  hle_window* w = hle_window_from(ref);
  return w ? &w->port : NULL;
}

hle_window* GetWindowFromPort(hle_port* port) {
  return port && port->magic == kHleMagicPort ? port->window : NULL;
}

void SetPortWindowPort(void* ref) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    SetPort(&w->port);
  }
}

void SetWRefCon(void* ref, SInt32 refcon) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    w->refcon = refcon;
  }
}

SInt32 GetWRefCon(void* ref) {
  hle_window* w = hle_window_from(ref);
  return w ? w->refcon : 0;
}

void SetWindowKind(void* ref, SInt16 kind) {
  hle_window* w = hle_window_from(ref);
  if (w) {
    w->kind = kind;
  }
}

SInt16 GetWindowKind(void* ref) {
  hle_window* w = hle_window_from(ref);
  return w ? w->kind : 0;
}

int SetWindowTitleWithCFString(void* ref, CFStringRef title) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  if (title) {
    CFRetain(title);
  }
  if (w->title) {
    CFRelease(w->title);
  }
  w->title = title;
  return noErr;
}

int SetWindowProxyCreatorAndType(void* ref, OSType creator, OSType type,
                                 SInt16 vref) {
  return hle_window_from(ref) ? noErr : errInvalidWindowRef;
}

int SetWindowModified(void* ref, Boolean modified) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  w->modified = modified;
  return noErr;
}

int SetWindowContentColor(void* ref, const RGBColor* color) {
  hle_window* w = hle_window_from(ref);
  if (!w) {
    return errInvalidWindowRef;
  }
  if (color) {
    w->content_color = *color;
  }
  return noErr;
}

int InvalWindowRect(void* ref, const Rect* r) {
  return noErr;
}

int ValidWindowRect(void* ref, const Rect* r) {
  return noErr;
}

void BeginUpdate(void* ref) {
}

void EndUpdate(void* ref) {
}

// ---------------------------------------------------------------------------
// Controls

static hle_control* control_of(void* ref) {
  hle_control* c = ref;
  return c && c->magic == kHleMagicControl ? c : NULL;
}

static hle_control* new_control(hle_window* w, hle_control* parent,
                                const char* nib_class) {
  hle_control* c = calloc(1, sizeof(*c));
  hle_target_init(&c->target, w ? &w->target : hle_application_target());
  c->magic = kHleMagicControl;
  c->window = w;
  c->nib_class = nib_class;
  c->visible = 1;
  c->active = 1;
  if (parent) {
    c->parent = parent;
    hle_control** link = &parent->children;
    while (*link) {
      link = &(*link)->next;
    }
    *link = c;
  }
  return c;
}

static hle_control* root_of(hle_window* w) {
  if (!w->root) {
    w->root = new_control(w, NULL, "IBCarbonRootControl");
    w->root->bounds = w->port.bounds;
  }
  return w->root;
}

static void unlink_control(hle_control* c) {
  if (!c->parent) {
    return;
  }
  for (hle_control** link = &c->parent->children; *link;
       link = &(*link)->next) {
    if (*link == c) {
      *link = c->next;
      break;
    }
  }
  c->parent = NULL;
  c->next = NULL;
}

static void dispose_control(hle_control* c) {
  while (c->children) {
    dispose_control(c->children);
  }
  unlink_control(c);
  if (c->window && c->window->root == c) {
    c->window->root = NULL;
  }
  hle_target_destroy(&c->target);
  if (c->title) {
    CFRelease(c->title);
  }
  while (c->data) {
    hle_control_data* d = c->data;
    c->data = d->next;
    free(d);
  }
  c->magic = 0;
  free(c);
}

static hle_control* find_control(hle_control* c, OSType signature, SInt32 id) {
  for (; c; c = c->next) {
    if (c->signature == signature && c->id == id) {
      return c;
    }
    hle_control* found = find_control(c->children, signature, id);
    if (found) {
      return found;
    }
  }
  return NULL;
}

typedef struct {
  OSType signature;
  SInt32 id;
} ControlID;

int GetControlByID(void* window, const ControlID* id, hle_control** out) {
  hle_window* w = hle_window_from(window);
  if (!w || !id) {
    return paramErr;
  }
  hle_control* c = w->root ? find_control(w->root, id->signature, id->id)
                           : NULL;
  if (out) {
    *out = c;
  }
  return c ? noErr : errUnknownControl;
}

int GetControlID(void* ref, ControlID* out) {
  hle_control* c = control_of(ref);
  if (!c || !out) {
    return paramErr;
  }
  out->signature = c->signature;
  out->id = c->id;
  return noErr;
}

int SetControlID(void* ref, const ControlID* id) {
  hle_control* c = control_of(ref);
  if (!c || !id) {
    return paramErr;
  }
  c->signature = id->signature;
  c->id = id->id;
  return noErr;
}

static int is_popup(const hle_control* c) {
  return c->menu != NULL;
}

static SInt32 maximum_of(const hle_control* c) {
  return is_popup(c) ? c->menu->count : c->maximum;
}

static void set_value(hle_control* c, SInt32 value) {
  SInt32 max = maximum_of(c);
  if (value > max) {
    value = max;
  }
  if (value < c->minimum) {
    value = c->minimum;
  }
  c->value = value;
  if (is_popup(c)) {
    for (int i = 0; i < c->menu->count; i++) {
      c->menu->items[i].checked = i + 1 == value;
    }
  }
}

SInt32 GetControl32BitValue(void* ref) {
  hle_control* c = control_of(ref);
  return c ? c->value : 0;
}

void SetControl32BitValue(void* ref, SInt32 value) {
  hle_control* c = control_of(ref);
  if (c) {
    set_value(c, value);
  }
}

SInt16 GetControlValue(void* ref) {
  return (SInt16)GetControl32BitValue(ref);
}

void SetControlValue(void* ref, SInt16 value) {
  SetControl32BitValue(ref, value);
}

SInt16 GetControlMinimum(void* ref) {
  hle_control* c = control_of(ref);
  return c ? c->minimum : 0;
}

SInt16 GetControlMaximum(void* ref) {
  hle_control* c = control_of(ref);
  return c ? maximum_of(c) : 0;
}

void SetControlMinimum(void* ref, SInt16 value) {
  hle_control* c = control_of(ref);
  if (c) {
    c->minimum = value;
    set_value(c, c->value);
  }
}

void SetControlMaximum(void* ref, SInt16 value) {
  hle_control* c = control_of(ref);
  if (c) {
    c->maximum = value;
    set_value(c, c->value);
  }
}

enum {
  kControlTextTag = 'text',
  kControlCFStringTag = 'cfst',
  kControlPopupButtonMenuHandleTag = 'mhan',
  kControlPopupButtonMenuIDTag = 'mnid',
};

static hle_control_data* find_data(hle_control* c, SInt16 part, OSType tag) {
  for (hle_control_data* d = c->data; d; d = d->next) {
    if (d->tag == tag && d->part == part) {
      return d;
    }
  }
  return NULL;
}

int GetControlData(void* ref, SInt16 part, OSType tag, Size size,
                   void* buffer, Size* actual) {
  hle_control* c = control_of(ref);
  if (!c) {
    return paramErr;
  }
  if (tag == kControlCFStringTag) {
    if (actual) {
      *actual = sizeof(CFStringRef);
    }
    if (buffer && size >= (Size)sizeof(CFStringRef)) {
      *(CFStringRef*)buffer = c->title ? (CFStringRef)CFRetain(c->title)
                                       : cf_string_from_utf8("", 0);
    }
    return noErr;
  }
  if (tag == kControlTextTag) {
    char* text = c->title ? cf_string_utf8(c->title) : strdup("");
    Size len = strlen(text);
    if (actual) {
      *actual = len;
    }
    if (buffer) {
      memcpy(buffer, text, len < size ? len : size);
    }
    free(text);
    return noErr;
  }
  if (tag == kControlPopupButtonMenuHandleTag && c->menu) {
    if (actual) {
      *actual = sizeof(void*);
    }
    if (buffer && size >= (Size)sizeof(void*)) {
      *(hle_menu**)buffer = c->menu;
    }
    return noErr;
  }
  if (tag == kControlPopupButtonMenuIDTag && c->menu) {
    if (actual) {
      *actual = sizeof(SInt16);
    }
    if (buffer && size >= (Size)sizeof(SInt16)) {
      *(SInt16*)buffer = c->menu->id;
    }
    return noErr;
  }
  hle_control_data* d = find_data(c, part, tag);
  if (!d) {
    return errDataNotSupported;
  }
  if (actual) {
    *actual = d->size;
  }
  if (buffer) {
    memcpy(buffer, d->bytes, (Size)d->size < size ? (Size)d->size : size);
  }
  return noErr;
}

int SetControlData(void* ref, SInt16 part, OSType tag, Size size,
                   const void* data) {
  hle_control* c = control_of(ref);
  if (!c || (size && !data)) {
    return paramErr;
  }
  if (tag == kControlCFStringTag && size >= (Size)sizeof(CFStringRef)) {
    CFStringRef title = *(const CFStringRef*)data;
    if (title) {
      CFRetain(title);
    }
    if (c->title) {
      CFRelease(c->title);
    }
    c->title = title;
    return noErr;
  }
  if (tag == kControlTextTag) {
    if (c->title) {
      CFRelease(c->title);
    }
    c->title = cf_string_from_utf8(data, strnlen(data, size));
    return noErr;
  }
  if (tag == kControlPopupButtonMenuHandleTag && size >= (Size)sizeof(void*)) {
    c->menu = *(hle_menu* const*)data;
    set_value(c, c->value);
    return noErr;
  }
  hle_control_data* d = find_data(c, part, tag);
  if (d) {
    for (hle_control_data** link = &c->data; *link; link = &(*link)->next) {
      if (*link == d) {
        *link = d->next;
        break;
      }
    }
    free(d);
  }
  d = malloc(sizeof(*d) + size);
  d->part = part;
  d->tag = tag;
  d->size = size;
  memcpy(d->bytes, data, size);
  d->next = c->data;
  c->data = d;
  return noErr;
}

int GetControlCommandID(void* ref, UInt32* out) {
  hle_control* c = control_of(ref);
  if (!c || !out) {
    return paramErr;
  }
  *out = c->command;
  return noErr;
}

hle_menu* GetControlPopupMenuHandle(void* ref) {
  hle_control* c = control_of(ref);
  return c ? c->menu : NULL;
}

Rect* GetControlBounds(void* ref, Rect* out) {
  hle_control* c = control_of(ref);
  if (out) {
    if (c) {
      *out = c->bounds;
    } else {
      memset(out, 0, sizeof(*out));
    }
  }
  return out;
}

hle_window* GetControlOwner(void* ref) {
  hle_control* c = control_of(ref);
  return c ? c->window : NULL;
}

void ShowControl(void* ref) {
  hle_control* c = control_of(ref);
  if (c) {
    c->visible = 1;
  }
}

void HideControl(void* ref) {
  hle_control* c = control_of(ref);
  if (c) {
    c->visible = 0;
  }
}

int ActivateControl(void* ref) {
  hle_control* c = control_of(ref);
  if (c) {
    c->active = 1;
  }
  return noErr;
}

int DeactivateControl(void* ref) {
  hle_control* c = control_of(ref);
  if (c) {
    c->active = 0;
  }
  return noErr;
}

Boolean IsControlActive(void* ref) {
  hle_control* c = control_of(ref);
  return c && c->active;
}

void DisposeControl(void* ref) {
  hle_control* c = control_of(ref);
  if (c) {
    dispose_control(c);
  }
}

void Draw1Control(void* ref) {
}

void SetControlAction(void* ref, void* action) {
  hle_control* c = control_of(ref);
  if (c) {
    c->action = action;
  }
}

void* NewControlActionUPP(void* proc) {
  return proc;
}

void DisposeControlActionUPP(void* upp) {
}

void* NewControlUserPaneDrawUPP(void* proc) {
  return proc;
}

void DisposeControlUserPaneDrawUPP(void* upp) {
}

int CreateScrollingTextBoxControl(void* window, const Rect* bounds,
                                  SInt16 resource_id, Boolean auto_scroll,
                                  UInt32 delay, UInt32 interval,
                                  UInt16 amount, hle_control** out) {
  hle_window* w = hle_window_from(window);
  if (!w || !bounds || !out) {
    return paramErr;
  }
  hle_control* c = new_control(w, root_of(w), "ScrollingTextBox");
  c->bounds = *bounds;
  *out = c;
  return noErr;
}

int ClearKeyboardFocus(void* window) {
  return noErr;
}

int AdvanceKeyboardFocus(void* window) {
  return noErr;
}

void TESetSelect(SInt32 start, SInt32 end, Handle text_edit) {
}

// ---------------------------------------------------------------------------
// HIView, over the same controls

typedef struct {
  OSType signature;
  SInt32 id;
} HIViewID;

typedef struct {
  float x;
  float y;
  float width;
  float height;
} HIRect;

const HIViewID kHIViewWindowContentID = { 'wind', 1 };

hle_control* HIViewGetRoot(void* window) {
  hle_window* w = hle_window_from(window);
  return w ? root_of(w) : NULL;
}

int HIViewFindByID(void* start, HIViewID id, hle_control** out) {
  hle_control* c = control_of(start);
  hle_control* found = NULL;
  if (c && id.signature == kHIViewWindowContentID.signature &&
      id.id == kHIViewWindowContentID.id && c->window) {
    found = root_of(c->window);
  } else if (c) {
    found = find_control(c->children, id.signature, id.id);
  }
  if (out) {
    *out = found;
  }
  return found ? noErr : errUnknownControl;
}

int HIViewAddSubview(void* parent_ref, void* child_ref) {
  hle_control* parent = control_of(parent_ref);
  hle_control* child = control_of(child_ref);
  if (!parent || !child) {
    return paramErr;
  }
  unlink_control(child);
  child->window = parent->window;
  child->parent = parent;
  hle_control** link = &parent->children;
  while (*link) {
    link = &(*link)->next;
  }
  *link = child;
  return noErr;
}

int HIViewSetVisible(void* ref, Boolean visible) {
  hle_control* c = control_of(ref);
  if (c) {
    c->visible = visible;
  }
  return c ? noErr : paramErr;
}

int HIViewSetNeedsDisplay(void* ref, Boolean needs) {
  return noErr;
}

int HIViewGetBounds(void* ref, HIRect* out) {
  hle_control* c = control_of(ref);
  if (!c || !out) {
    return paramErr;
  }
  out->x = out->y = 0;
  out->width = c->bounds.right - c->bounds.left;
  out->height = c->bounds.bottom - c->bounds.top;
  return noErr;
}

int HIViewSetFrame(void* ref, const HIRect* frame) {
  hle_control* c = control_of(ref);
  if (!c || !frame) {
    return paramErr;
  }
  c->bounds.left = (SInt16)frame->x;
  c->bounds.top = (SInt16)frame->y;
  c->bounds.right = (SInt16)(frame->x + frame->width);
  c->bounds.bottom = (SInt16)(frame->y + frame->height);
  return noErr;
}

int HIViewSetBoundsOrigin(void* ref, float x, float y) {
  return noErr;
}

int HIImageViewSetImage(void* ref, void* image) {
  return noErr;
}

int HIImageViewSetScaleToFit(void* ref, Boolean scale) {
  return noErr;
}

int HIScrollViewCreate(UInt32 options, hle_control** out) {
  if (!out) {
    return paramErr;
  }
  *out = new_control(NULL, NULL, "HIScrollView");
  return noErr;
}

int HIObjectRegisterSubclass(CFStringRef class_id, CFStringRef base_class_id,
                             UInt32 options, void* construct, UInt32 count,
                             const EventTypeSpec* events, void* data,
                             void** out) {
  static int registered;
  if (out) {
    *out = &registered;
  }
  return noErr;
}

// Custom HIObject classes are not built; the one the game has is its About
// box's.
int HIObjectCreate(CFStringRef class_id, EventRef construct, void** out) {
  if (out) {
    *out = NULL;
  }
  cf_warn_once("HIObjectCreate: custom HIObject classes are not supported");
  return paramErr;
}

// ---------------------------------------------------------------------------
// Menus

enum {
  kMaxMenuBar = 32,
};

static hle_menu* menu_bar[kMaxMenuBar];
static int menu_bar_count;
static int menu_bar_visible = 1;

static hle_menu* menu_of(void* ref) {
  hle_menu* m = ref;
  return m && m->magic == kHleMagicMenu ? m : NULL;
}

static hle_menu* new_menu(SInt16 id, CFStringRef title) {
  hle_menu* m = calloc(1, sizeof(*m));
  hle_target_init(&m->target, hle_application_target());
  m->magic = kHleMagicMenu;
  m->id = id;
  m->title = title;
  m->enabled = 1;
  return m;
}

static hle_menu_item* append_item(hle_menu* m) {
  m->items = realloc(m->items, sizeof(hle_menu_item) * (m->count + 1));
  hle_menu_item* item = &m->items[m->count++];
  memset(item, 0, sizeof(*item));
  item->enabled = 1;
  return item;
}

static hle_menu* find_menu_id(hle_menu* m, SInt16 id) {
  if (!m) {
    return NULL;
  }
  if (m->id == id) {
    return m;
  }
  for (int i = 0; i < m->count; i++) {
    hle_menu* found = find_menu_id(m->items[i].submenu, id);
    if (found) {
      return found;
    }
  }
  return NULL;
}

static hle_menu_item* find_command(hle_menu* m, OSType command) {
  for (int i = 0; m && i < m->count; i++) {
    if (m->items[i].command == command) {
      return &m->items[i];
    }
    hle_menu_item* found = find_command(m->items[i].submenu, command);
    if (found) {
      return found;
    }
  }
  return NULL;
}

// A command's item in |m|, or anywhere in the menu bar when |m| is NULL.
static hle_menu_item* command_item(void* ref, OSType command) {
  hle_menu* m = menu_of(ref);
  if (m) {
    return find_command(m, command);
  }
  for (int i = 0; i < menu_bar_count; i++) {
    hle_menu_item* found = find_command(menu_bar[i], command);
    if (found) {
      return found;
    }
  }
  return NULL;
}

hle_menu* GetMenuHandle(SInt16 id) {
  for (int i = 0; i < menu_bar_count; i++) {
    hle_menu* found = find_menu_id(menu_bar[i], id);
    if (found) {
      return found;
    }
  }
  return NULL;
}

UInt16 CountMenuItems(void* ref) {
  hle_menu* m = menu_of(ref);
  return m ? m->count : 0;
}

void CheckMenuItem(void* ref, UInt16 index, Boolean checked) {
  hle_menu* m = menu_of(ref);
  if (m && index >= 1 && index <= m->count) {
    m->items[index - 1].checked = checked;
  }
}

static void enable_item(void* ref, UInt16 index, int enabled) {
  hle_menu* m = menu_of(ref);
  if (!m) {
    return;
  }
  if (index == 0) {
    m->enabled = enabled;
  } else if (index <= m->count) {
    m->items[index - 1].enabled = enabled;
  }
}

void EnableMenuItem(void* ref, UInt16 index) {
  enable_item(ref, index, 1);
}

void DisableMenuItem(void* ref, UInt16 index) {
  enable_item(ref, index, 0);
}

void EnableMenuCommand(void* ref, UInt32 command) {
  hle_menu_item* item = command_item(ref, command);
  if (item) {
    item->enabled = 1;
  }
}

void DisableMenuCommand(void* ref, UInt32 command) {
  hle_menu_item* item = command_item(ref, command);
  if (item) {
    item->enabled = 0;
  }
}

int GetMenuItemCommandID(void* ref, SInt16 index, UInt32* out) {
  hle_menu* m = menu_of(ref);
  if (!m || index < 1 || index > m->count) {
    return menuItemNotFoundErr;
  }
  if (out) {
    *out = m->items[index - 1].command;
  }
  return noErr;
}

int AppendMenuItemTextWithCFString(void* ref, CFStringRef text,
                                   UInt32 attributes, UInt32 command,
                                   UInt16* out) {
  hle_menu* m = menu_of(ref);
  if (!m) {
    return paramErr;
  }
  hle_menu_item* item = append_item(m);
  item->text = text ? (CFStringRef)CFRetain(text) : NULL;
  item->command = command;
  item->enabled = !(attributes & kMenuItemAttrDisabled);
  item->separator = (attributes & kMenuItemAttrSeparator) != 0;
  if (out) {
    *out = m->count;
  }
  return noErr;
}

int DeleteMenuItems(void* ref, UInt16 first, UInt32 count) {
  hle_menu* m = menu_of(ref);
  if (!m || first < 1 || first > m->count) {
    return menuItemNotFoundErr;
  }
  UInt32 available = m->count - (first - 1);
  if (count > available) {
    count = available;
  }
  for (UInt32 i = 0; i < count; i++) {
    if (m->items[first - 1 + i].text) {
      CFRelease(m->items[first - 1 + i].text);
    }
  }
  memmove(&m->items[first - 1], &m->items[first - 1 + count],
          sizeof(hle_menu_item) * (available - count));
  m->count -= count;
  return noErr;
}

// No menu is ever pulled down.
int32_t MenuSelect(Point start) {
  return 0;
}

void HideMenuBar(void) {
  menu_bar_visible = 0;
}

void ShowMenuBar(void) {
  menu_bar_visible = 1;
}

Boolean IsMenuBarVisible(void) {
  return menu_bar_visible;
}

// ---------------------------------------------------------------------------
// NIBs

static OSType parse_ostype(const char* text) {
  unsigned char c[4] = { ' ', ' ', ' ', ' ' };
  for (int i = 0; i < 4 && text && text[i]; i++) {
    c[i] = text[i];
  }
  return (OSType)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
}

static int parse_bool(const char* text) {
  return text && (!strcasecmp(text, "TRUE") || !strcmp(text, "1"));
}

static int parse_rect(const char* text, Rect* r) {
  int top, left, bottom, right;
  if (!text || sscanf(text, "%d %d %d %d", &top, &left, &bottom, &right) != 4) {
    return 0;
  }
  r->top = top;
  r->left = left;
  r->bottom = bottom;
  r->right = right;
  return 1;
}

static int nib_int(const xml_node* object, const char* name, int fallback) {
  const char* text = hle_nib_property(object, name);
  return text ? (int)strtol(text, NULL, 10) : fallback;
}

static CFStringRef nib_string(const xml_node* object, const char* name) {
  const char* text = hle_nib_property(object, name);
  return text ? cf_string_from_utf8(text, strlen(text)) : NULL;
}

// Class names live as long as the controls naming them.
static const char* intern_class(const char* name) {
  static const char* const kKnown[] = {
    "IBCarbonButton", "IBCarbonCheckBox", "IBCarbonPopupButton",
    "IBCarbonStaticText", "IBCarbonEditText", "IBCarbonGroupBox",
    "IBCarbonUserPane", "IBCarbonRootControl", "IBCarbonIcon",
    "IBCarbonLittleArrows", "IBCarbonRelevanceBar", "IBCarbonSeparator",
  };
  for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); i++) {
    if (name && !strcmp(name, kKnown[i])) {
      return kKnown[i];
    }
  }
  return name ? strdup(name) : "";
}

static hle_menu* build_menu(hle_nib* nib, xml_node* node) {
  node = hle_nib_resolve(nib, node);
  if (!node) {
    return NULL;
  }
  hle_menu* m = new_menu(nib_int(node, "menuID", 0), nib_string(node, "title"));
  xml_node* items = xml_named_child(node, "items");
  for (int i = 0; items && i < items->child_count; i++) {
    xml_node* child = hle_nib_resolve(nib, items->children[i]);
    if (!child) {
      continue;
    }
    hle_menu_item* item = append_item(m);
    item->text = nib_string(child, "title");
    const char* command = hle_nib_property(child, "command");
    item->command = command ? parse_ostype(command) : 0;
    item->enabled = !parse_bool(hle_nib_property(child, "disabled"));
    item->checked = parse_bool(hle_nib_property(child, "checked"));
    item->separator = parse_bool(hle_nib_property(child, "separator"));
    item->submenu = build_menu(nib, hle_nib_object(nib, child, "submenu"));
  }
  return m;
}

static hle_control* build_control(hle_nib* nib, xml_node* node, hle_window* w,
                                  hle_control* parent) {
  node = hle_nib_resolve(nib, node);
  if (!node) {
    return NULL;
  }
  const char* nib_class = intern_class(xml_attribute(node, "class"));
  hle_control* c = new_control(w, parent, nib_class);
  parse_rect(hle_nib_property(node, "bounds"), &c->bounds);
  const char* signature = hle_nib_property(node, "controlSignature");
  c->signature = signature ? parse_ostype(signature) : 0;
  c->id = nib_int(node, "controlID", 0);
  const char* command = hle_nib_property(node, "command");
  c->command = command ? parse_ostype(command) : 0;
  c->title = nib_string(node, "title");
  c->button_type = nib_int(node, "buttonType", 0);
  int is_checkbox = !strcmp(nib_class, "IBCarbonCheckBox");
  c->minimum = nib_int(node, "minimumValue", 0);
  c->maximum = nib_int(node, "maximumValue", is_checkbox ? 1 : 0);
  c->value = nib_int(node, "initialValue", 0);
  if (is_checkbox && parse_bool(hle_nib_property(node, "checked"))) {
    c->value = 1;
  }
  c->active = !parse_bool(hle_nib_property(node, "disabled"));
  c->visible = !parse_bool(hle_nib_property(node, "hidden"));
  xml_node* menu = hle_nib_object(nib, node, "menu");
  if (menu) {
    c->menu = build_menu(nib, menu);
    c->minimum = 1;
    c->value = 1;
    for (int i = 0; c->menu && i < c->menu->count; i++) {
      if (c->menu->items[i].checked) {
        c->value = i + 1;
      }
    }
  }
  xml_node* subviews = xml_named_child(node, "subviews");
  for (int i = 0; subviews && i < subviews->child_count; i++) {
    build_control(nib, subviews->children[i], w, c);
  }
  return c;
}

int CreateNibReference(CFStringRef name, hle_nib** out) {
  if (!name || !out) {
    return paramErr;
  }
  char* text = cf_string_utf8(name);
  *out = hle_nib_open(text);
  if (!*out) {
    fprintf(stderr, "hle: %s.nib not found in the application bundle\n",
            text);
  }
  free(text);
  return *out ? noErr : kIBCarbonRuntimeCantFindNibFile;
}

void DisposeNibReference(hle_nib* nib) {
  hle_nib_close(nib);
}

int CreateWindowFromNib(hle_nib* nib, CFStringRef name, hle_window** out) {
  if (!nib || !name || !out) {
    return paramErr;
  }
  *out = NULL;
  char* text = cf_string_utf8(name);
  xml_node* object = hle_nib_named(nib, text);
  if (!object) {
    cf_trace("CreateWindowFromNib(%s): no such object", text);
    free(text);
    return kIBCarbonRuntimeCantFindObject;
  }
  const char* object_class = xml_attribute(object, "class");
  if (!object_class || strcmp(object_class, "IBCarbonWindow")) {
    free(text);
    return kIBCarbonRuntimeObjectNotOfRequestedType;
  }
  Rect content = { .top = 100, .left = 100, .bottom = 400, .right = 500 };
  parse_rect(hle_nib_property(object, "windowRect"), &content);
  hle_window* w = new_window(
      sizeof(hle_window),
      nib_int(object, "carbonWindowClass", kDocumentWindowClass), 0, &content);
  w->title = nib_string(object, "title");
  w->nib_name = text;
  xml_node* root = hle_nib_object(nib, object, "rootControl");
  if (root) {
    w->root = build_control(nib, root, w, NULL);
  }
  int position = nib_int(object, "windowPosition", 0);
  if (position) {
    RepositionWindow(w, NULL, position);
  }
  cf_trace("CreateWindowFromNib(%s)", text);
  *out = w;
  return noErr;
}

int SetMenuBarFromNib(hle_nib* nib, CFStringRef name) {
  if (!nib || !name) {
    return paramErr;
  }
  char* text = cf_string_utf8(name);
  xml_node* object = hle_nib_named(nib, text);
  free(text);
  if (!object) {
    return kIBCarbonRuntimeCantFindObject;
  }
  hle_menu* bar = build_menu(nib, object);
  menu_bar_count = 0;
  for (int i = 0; bar && i < bar->count && menu_bar_count < kMaxMenuBar; i++) {
    if (bar->items[i].submenu) {
      menu_bar[menu_bar_count++] = bar->items[i].submenu;
    }
  }
  return noErr;
}

// ---------------------------------------------------------------------------
// Answering dialogs

static void signature_text(OSType signature, char out[5]) {
  for (int i = 0; i < 4; i++) {
    out[i] = (char)(signature >> (24 - 8 * i));
  }
  out[4] = '\0';
}

// An environment variable name for |prefix| and |name|: letters, digits and
// underscores only. Returns 0 if |name| would need changing to fit.
static int env_name(const char* prefix, const char* name, char* out,
                    size_t size) {
  if (!*name) {
    return 0;
  }
  for (const char* p = name; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '_') {
      return 0;
    }
  }
  return snprintf(out, size, "%s%s", prefix, name) < (int)size;
}

static int is_text_control(const hle_control* c) {
  return !strcmp(c->nib_class, "IBCarbonEditText") ||
         !strcmp(c->nib_class, "IBCarbonStaticText");
}

static void apply_overrides(hle_control* c) {
  for (; c; c = c->next) {
    char sig[5];
    char name[32];
    signature_text(c->signature, sig);
    const char* value = NULL;
    if (c->signature && env_name("HLE_CONTROL_", sig, name, sizeof(name))) {
      value = getenv(name);
    }
    if (!value && c->signature == 'Wind') {
      // Playing in a window follows HLE_WINDOWED unless named directly.
      const char* windowed = getenv("HLE_WINDOWED");
      value = windowed && *windowed ? windowed : NULL;
      snprintf(name, sizeof(name), "HLE_WINDOWED");
    }
    if (value) {
      if (is_text_control(c)) {
        if (c->title) {
          CFRelease(c->title);
        }
        c->title = cf_string_from_utf8(value, strlen(value));
      } else {
        set_value(c, strtol(value, NULL, 10));
      }
      fprintf(stderr, "hle:   %s sets '%s' to %s\n", name, sig, value);
    }
    apply_overrides(c->children);
  }
}

static hle_control* button_with(hle_control* c, OSType command,
                                int default_only) {
  for (; c; c = c->next) {
    if (c->command && (default_only ? c->button_type == 1
                                    : c->command == command)) {
      return c;
    }
    hle_control* found = button_with(c->children, command, default_only);
    if (found) {
      return found;
    }
  }
  return NULL;
}

static void send_command(hle_window* w, OSType command, hle_control* source) {
  HICommandExtended cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.attributes = source ? kHICommandFromControl : 0;
  cmd.commandID = command;
  cmd.source.control = source;
  EventRef event = hle_event_new(kEventClassCommand, kEventCommandProcess);
  hle_event_set(event, kEventParamDirectObject, typeHICommand, sizeof(cmd),
                &cmd);
  // Straight to the window: a dialog run under the application event loop,
  // as the EULA is, has its handler there and nothing routes to it.
  SendEventToEventTarget(event, &w->target);
  ReleaseEvent(event);
}

static void answer(hle_window* w) {
  const char* nib_name = w->nib_name ? w->nib_name : "";
  fprintf(stderr, "hle: the game shows the %s dialog\n",
          *nib_name ? nib_name : "untitled");
  apply_overrides(w->root);

  char name[64];
  const char* choice = env_name("HLE_DIALOG_", nib_name, name, sizeof(name))
                           ? getenv(name)
                           : NULL;
  OSType command;
  hle_control* button;
  if (choice && *choice) {
    command = parse_ostype(choice);
    button = w->root ? button_with(w->root, command, 0) : NULL;
  } else {
    button = w->root ? button_with(w->root, 0, 1) : NULL;
    command = button ? button->command : kHICommandOK;
  }
  char sig[5];
  signature_text(command, sig);
  char* title = button && button->title ? cf_string_utf8(button->title) : NULL;
  fprintf(stderr, "hle:   answering with '%s'%s%s%s (HLE_DIALOG_%s picks another)\n",
          sig, title ? " (" : "", title ? title : "", title ? ")" : "",
          *nib_name ? nib_name : "<name>");
  free(title);
  send_command(w, command, button);
}

// Answers the window of the innermost modal loop, or else the frontmost NIB
// window on screen: the game runs its EULA under the application event loop.
static void loop_hook(void) {
  hle_window* w = hle_window_from(hle_modal_window());
  if (!w) {
    for (hle_window* shown = windows; shown; shown = shown->next) {
      if (shown->visible && shown->nib_name && !shown->answered) {
        w = shown;
        break;
      }
    }
  }
  if (w && !w->answered) {
    w->answered = 1;
    answer(w);
  }
}

__attribute__((constructor)) static void install_loop_hook(void) {
  hle_loop_hook = loop_hook;
}

// ---------------------------------------------------------------------------
// Dialogs and alerts

#pragma pack(push, 2)
typedef struct {
  UInt32 version;
  Boolean movable;
  Boolean helpButton;
  CFStringRef defaultText;
  CFStringRef cancelText;
  CFStringRef otherText;
  SInt16 defaultButton;
  SInt16 cancelButton;
  UInt16 position;
  UInt32 flags;
} AlertStdCFStringAlertParamRec;
#pragma pack(pop)

#ifdef __i386__
_Static_assert(sizeof(AlertStdCFStringAlertParamRec) == 28,
               "AlertStdCFStringAlertParamRec");
#endif

// An item of a classic dialog, from its DITL resource. A text item keeps its
// text as a Pascal string in a 256-byte handle, which is what the game gets
// from GetDialogItem and hands back to Get and SetDialogItemText.
typedef struct {
  uint8_t kind;  // 4 button, 5 check box, 6 radio, 8 static text, 16 edit
  Rect rect;
  Handle text;
} hle_dialog_item;

typedef struct {
  hle_window window;  // first: a DialogRef is its window
  SInt16 alert_type;
  CFStringRef error;
  CFStringRef explanation;
  AlertStdCFStringAlertParamRec params;
  SInt16 resource_id;  // the DLOG a classic dialog came from
  int item_count;
  hle_dialog_item* items;  // items[0] is item 1
} hle_dialog;

static hle_dialog* dialog_of(void* ref) {
  return (hle_dialog*)hle_window_from(ref);
}

static hle_dialog* new_dialog(UInt32 window_class) {
  Rect content = { .top = 0, .left = 0, .bottom = 120, .right = 420 };
  hle_dialog* d = (hle_dialog*)new_window(sizeof(hle_dialog), window_class, 0,
                                          &content);
  RepositionWindow(&d->window, NULL, 7);
  return d;
}

int GetStandardAlertDefaultParams(AlertStdCFStringAlertParamRec* params,
                                  UInt32 version) {
  if (!params) {
    return paramErr;
  }
  memset(params, 0, sizeof(*params));
  params->version = version;
  params->defaultText = (CFStringRef)-1;  // kAlertDefaultOKText
  params->defaultButton = 1;              // kAlertStdAlertOKButton
  return noErr;
}

int CreateStandardAlert(SInt16 type, CFStringRef error,
                        CFStringRef explanation,
                        const AlertStdCFStringAlertParamRec* params,
                        hle_dialog** out) {
  if (!out) {
    return paramErr;
  }
  hle_dialog* d = new_dialog(kMovableAlertWindowClass);
  d->alert_type = type;
  d->error = error ? (CFStringRef)CFRetain(error) : NULL;
  d->explanation = explanation ? (CFStringRef)CFRetain(explanation) : NULL;
  if (params) {
    d->params = *params;
  } else {
    GetStandardAlertDefaultParams(&d->params, 0);
  }
  *out = d;
  return noErr;
}

void DisposeDialog(void* ref) {
  hle_dialog* d = dialog_of(ref);
  if (!d) {
    return;
  }
  if (d->error) {
    CFRelease(d->error);
  }
  if (d->explanation) {
    CFRelease(d->explanation);
  }
  for (int i = 0; i < d->item_count; i++) {
    if (d->items[i].text) {
      DisposeHandle(d->items[i].text);
    }
  }
  free(d->items);
  DisposeWindow(&d->window);
}

int RunStandardAlert(void* ref, void* filter, SInt16* item_hit) {
  hle_dialog* d = dialog_of(ref);
  if (!d) {
    return paramErr;
  }
  char* error = d->error ? cf_string_utf8(d->error) : strdup("");
  char* explanation = d->explanation ? cf_string_utf8(d->explanation) : NULL;
  SInt16 hit = d->params.defaultButton ? d->params.defaultButton : 1;
  fprintf(stderr, "hle: alert: %s\n", error);
  if (explanation && *explanation) {
    fprintf(stderr, "hle:   %s\n", explanation);
  }
  fprintf(stderr, "hle:   answering with button %d\n", hit);
  free(error);
  free(explanation);
  if (item_hit) {
    *item_hit = hit;
  }
  DisposeDialog(d);
  return noErr;
}

// ---------------------------------------------------------------------------
// Classic dialogs and alerts, from DLOG, DITL and ALRT resources
//
// A modal loop over a classic dialog is answered too. Its edit fields take
// the text of HLE_DIALOG_<DLOG id>, split at dashes, and its first button is
// hit. Without that variable, or when the game does not accept the text,
// the second button is hit instead, Cancel or Quit by convention, and
// stderr says what the dialog asked for.

Handle GetResource(ResType type, SInt16 id);

enum {
  kItemButton = 4,
  kItemCheckBox = 5,
  kItemRadio = 6,
  kItemStaticText = 8,
  kItemEditText = 16,
  kMaxEditFields = 16,
};

// The dialog whose text was last filled in, and whether an alert has come
// since: the game showing that dialog again then means it refused the text.
static SInt16 last_filled_id;
static int alert_after_fill;

static uint16_t be16(const uint8_t* p) {
  return (uint16_t)(p[0] << 8 | p[1]);
}

static Rect be_rect(const uint8_t* p) {
  Rect r = { .top = (SInt16)be16(p), .left = (SInt16)be16(p + 2),
             .bottom = (SInt16)be16(p + 4), .right = (SInt16)be16(p + 6) };
  return r;
}

static char* pascal_utf8(ConstStringPtr p) {
  char buffer[256];
  memcpy(buffer, p + 1, p[0]);
  buffer[p[0]] = '\0';
  CFStringRef s = CFStringCreateWithCString(NULL, buffer,
                                            kCFStringEncodingMacRoman);
  char* out = s ? cf_string_utf8(s) : strdup(buffer);
  if (s) {
    CFRelease(s);
  }
  return out;
}

// A DITL holds its item count less one, then for each item a placeholder, a
// rectangle, a kind, and length-prefixed data padded to an even size.
static void load_items(hle_dialog* d, SInt16 ditl_id) {
  Handle h = GetResource('DITL', ditl_id);
  Size size = h ? GetHandleSize(h) : 0;
  if (size < 2) {
    return;
  }
  const uint8_t* p = (const uint8_t*)*h;
  int count = be16(p) + 1;
  d->items = calloc(count, sizeof(hle_dialog_item));
  Size at = 2;
  for (int i = 0; i < count && at + 14 <= size; i++) {
    hle_dialog_item* item = &d->items[d->item_count++];
    item->rect = be_rect(p + at + 4);
    item->kind = p[at + 12];
    uint8_t length = p[at + 13];
    int kind = item->kind & 0x7F;
    if (kind == kItemButton || kind == kItemCheckBox || kind == kItemRadio ||
        kind == kItemStaticText || kind == kItemEditText) {
      Size n = length;
      if (at + 14 + n > size) {
        n = size - at - 14;
      }
      item->text = hle_handle_new(NULL, 256, 0);
      memset(*item->text, 0, 256);
      (*item->text)[0] = (char)n;
      memcpy(*item->text + 1, p + at + 14, n);
    }
    at += 14 + length + (length & 1);
  }
}

static void free_items(hle_dialog* d) {
  for (int i = 0; i < d->item_count; i++) {
    if (d->items[i].text) {
      DisposeHandle(d->items[i].text);
    }
  }
  free(d->items);
  d->items = NULL;
  d->item_count = 0;
}

// The text of the items of |kind|, joined by spaces, for messages.
static char* items_text(const hle_dialog* d, int kind) {
  cf_buf out = { 0 };
  for (int i = 0; i < d->item_count; i++) {
    if ((d->items[i].kind & 0x7F) != kind || !d->items[i].text) {
      continue;
    }
    char* text = pascal_utf8((ConstStringPtr)*d->items[i].text);
    if (*text) {
      if (out.len) {
        cf_buf_appends(&out, " ");
      }
      cf_buf_appends(&out, text);
    }
    free(text);
  }
  return out.data ? out.data : strdup("");
}

static SInt16 run_alert(SInt16 id) {
  Handle alrt = GetResource('ALRT', id);
  char* text = NULL;
  if (alrt && GetHandleSize(alrt) >= 10) {
    hle_dialog items;
    memset(&items, 0, sizeof(items));
    load_items(&items, (SInt16)be16((const uint8_t*)*alrt + 8));
    text = items_text(&items, kItemStaticText);
    free_items(&items);
  }
  fprintf(stderr, "hle: alert %d: %s\n", id,
          text && *text ? text : "(no ALRT resource)");
  fprintf(stderr, "hle:   answering with its first button\n");
  free(text);
  if (last_filled_id) {
    alert_after_fill = 1;
  }
  return 1;
}

SInt16 StopAlert(SInt16 id, void* filter) {
  return run_alert(id);
}

SInt16 NoteAlert(SInt16 id, void* filter) {
  return run_alert(id);
}

SInt16 CautionAlert(SInt16 id, void* filter) {
  return run_alert(id);
}

SInt16 Alert(SInt16 id, void* filter) {
  return run_alert(id);
}

hle_dialog* GetNewDialog(SInt16 id, void* storage, void* behind) {
  Handle dlog = GetResource('DLOG', id);
  if (!dlog || GetHandleSize(dlog) < 20) {
    fprintf(stderr, "hle: dialog %d: no DLOG resource\n", id);
    return NULL;
  }
  const uint8_t* p = (const uint8_t*)*dlog;
  Rect bounds = be_rect(p);
  hle_dialog* d = (hle_dialog*)new_window(
      sizeof(hle_dialog), kMovableModalWindowClass, 0, &bounds);
  d->window.is_dialog = 1;
  d->window.kind = 2;  // dialogKind
  d->resource_id = id;
  if (GetHandleSize(dlog) > 20) {
    char* title = pascal_utf8(p + 20);
    d->window.title = cf_string_from_utf8(title, strlen(title));
    free(title);
  }
  load_items(d, (SInt16)be16(p + 18));
  RepositionWindow(&d->window, NULL, 7);
  return d;
}

// Puts |value| into the dialog's edit fields in order, split at dashes, or
// into equal parts when it has none and divides evenly.
static int fill_fields(hle_dialog* d, const char* value) {
  int fields[kMaxEditFields];
  int n = 0;
  for (int i = 0; i < d->item_count && n < kMaxEditFields; i++) {
    if ((d->items[i].kind & 0x7F) == kItemEditText && d->items[i].text) {
      fields[n++] = i;
    }
  }
  size_t length = strlen(value);
  size_t part = !strchr(value, '-') && n > 1 && length % n == 0 ? length / n
                                                                 : 0;
  const char* p = value;
  for (int k = 0; k < n; k++) {
    size_t take;
    if (part) {
      take = part;
    } else {
      const char* dash = strchr(p, '-');
      take = dash ? (size_t)(dash - p) : strlen(p);
    }
    if (take > 255) {
      take = 255;
    }
    Handle h = d->items[fields[k]].text;
    (*h)[0] = (char)take;
    memcpy(*h + 1, p, take);
    p += take;
    if (*p == '-') {
      p++;
    }
  }
  return n;
}

static SInt16 answer_classic(hle_dialog* d) {
  char name[32];
  snprintf(name, sizeof(name), "HLE_DIALOG_%d", d->resource_id);
  const char* value = getenv(name);
  SInt16 second = d->item_count >= 2 ? 2 : 1;
  int first_time = !d->window.answered++;

  if (first_time) {
    char* title = d->window.title ? cf_string_utf8(d->window.title)
                                  : strdup("");
    char* note = items_text(d, kItemStaticText);
    fprintf(stderr, "hle: the game shows dialog %d: %s\n", d->resource_id,
            title);
    if (*note) {
      fprintf(stderr, "hle:   %s\n", note);
    }
    free(title);
    free(note);
  }
  const char* refusal = NULL;
  if (!first_time) {
    refusal = "the game did not take that text";
  } else if (value && *value && last_filled_id == d->resource_id &&
             alert_after_fill) {
    refusal = "the game refused that text";
  }
  if (refusal || !value || !*value) {
    char* button = d->item_count >= second && d->items[second - 1].text
                       ? pascal_utf8((ConstStringPtr)*d->items[second - 1].text)
                       : strdup("");
    if (refusal) {
      fprintf(stderr, "hle:   %s from %s; answering with its button %d (%s)\n",
              refusal, name, second, button);
    } else {
      fprintf(stderr, "hle:   set %s to fill its text fields, separated by "
              "dashes; answering with its button %d (%s)\n", name, second,
              button);
    }
    free(button);
    last_filled_id = 0;
    alert_after_fill = 0;
    return second;
  }
  int fields = fill_fields(d, value);
  fprintf(stderr, "hle:   filling its %d text field%s from %s\n", fields,
          fields == 1 ? "" : "s", name);
  last_filled_id = d->resource_id;
  alert_after_fill = 0;
  return 1;
}

// Answers the frontmost classic dialog on screen.
void ModalDialog(void* filter, SInt16* item_hit) {
  SInt16 hit = 1;
  for (hle_window* w = windows; w; w = w->next) {
    if (w->visible && w->is_dialog) {
      hit = answer_classic((hle_dialog*)w);
      break;
    }
  }
  if (item_hit) {
    *item_hit = hit;
  }
}

void GetDialogItem(void* ref, SInt16 item, SInt16* type, Handle* handle,
                   Rect* box) {
  hle_dialog* d = dialog_of(ref);
  const hle_dialog_item* it =
      d && d->window.is_dialog && item >= 1 && item <= d->item_count
          ? &d->items[item - 1]
          : NULL;
  if (type) {
    *type = it ? it->kind : 0;
  }
  if (handle) {
    *handle = it ? it->text : NULL;
  }
  if (box) {
    if (it) {
      *box = it->rect;
    } else {
      memset(box, 0, sizeof(*box));
    }
  }
}

void GetDialogItemText(Handle item, StringPtr text) {
  if (!text) {
    return;
  }
  if (!item || !*item) {
    text[0] = 0;
    return;
  }
  memcpy(text, *item, (uint8_t)(*item)[0] + 1);
}

void SetDialogItemText(Handle item, ConstStringPtr text) {
  if (item && *item && text) {
    memcpy(*item, text, (size_t)text[0] + 1);
  }
}

int SetDialogDefaultItem(void* dialog, SInt16 item) {
  return noErr;
}

Handle GetDialogTextEditHandle(void* dialog) {
  return NULL;
}

hle_window* GetDialogWindow(void* dialog) {
  return hle_window_from(dialog);
}

void SetPortDialogPort(void* dialog) {
  SetPortWindowPort(dialog);
}

hle_window* NavDialogGetWindow(void* dialog) {
  return NULL;
}

// ---------------------------------------------------------------------------
// Appearance Manager text

int GetThemeTextDimensions(CFStringRef text, UInt16 font, UInt32 state,
                           Boolean wrap, Point* bounds, SInt16* baseline) {
  CFIndex length = text ? cf_string_length(text) : 0;
  if (bounds) {
    if (!wrap || bounds->h <= 0) {
      bounds->h = (SInt16)(length * 7);
      bounds->v = 16;
    } else {
      int lines = (int)(length * 7 / bounds->h) + 1;
      bounds->v = (SInt16)(lines * 16);
    }
  }
  if (baseline) {
    *baseline = -4;
  }
  return noErr;
}

int DrawThemeTextBox(CFStringRef text, UInt16 font, UInt32 state, Boolean wrap,
                     const Rect* box, SInt16 justification, void* context) {
  return noErr;
}
