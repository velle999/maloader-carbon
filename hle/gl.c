// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// OpenGL contexts: CGL and AGL over SDL's.
//
// A Mac context object begins with its renderer pointer and the dispatch
// table code built with aglMacro.h calls through, so contexts here begin
// the same way, the table holding gl_dispatch.c's thunks. Each SDL context
// is created against a hidden window of its pixel format and moves to the
// SDL window of the Carbon window, or of the screen, the game attaches.
//
// The renderer described is one accelerated NVIDIA renderer. HLE_VRAM_MB
// sets the video memory it claims (256 by default), and HLE_WINDOWED=1
// keeps a full-screen context in a window of the same size.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>

#include "arb_program.h"
#include "gl_dispatch.h"
#include "gl_stats.h"
#include "gl_var.h"
#include "gui.h"
#include "profile.h"

enum {
  kMagicPixelFormat = 'pixf',
  kMagicContext = 'aglc',
  kMagicRendererInfo = 'rinf',
  kRendererGeForceFX = 0x00022400,
};

// Attribute names, shared by AGL and CGL.
enum {
  AGL_NONE = 0,
  AGL_ALL_RENDERERS = 1,
  AGL_BUFFER_SIZE = 2,
  AGL_LEVEL = 3,
  AGL_RGBA = 4,
  AGL_DOUBLEBUFFER = 5,
  AGL_STEREO = 6,
  AGL_AUX_BUFFERS = 7,
  AGL_RED_SIZE = 8,
  AGL_GREEN_SIZE = 9,
  AGL_BLUE_SIZE = 10,
  AGL_ALPHA_SIZE = 11,
  AGL_DEPTH_SIZE = 12,
  AGL_STENCIL_SIZE = 13,
  AGL_ACCUM_RED_SIZE = 14,
  AGL_ACCUM_GREEN_SIZE = 15,
  AGL_ACCUM_BLUE_SIZE = 16,
  AGL_ACCUM_ALPHA_SIZE = 17,
  AGL_PIXEL_SIZE = 50,
  AGL_OFFSCREEN = 53,
  AGL_FULLSCREEN = 54,
  AGL_SAMPLE_BUFFERS_ARB = 55,
  AGL_SAMPLES_ARB = 56,
  AGL_RENDERER_ID = 70,
  AGL_ACCELERATED = 73,
  AGL_WINDOW = 80,
  AGL_VIRTUAL_SCREEN = 82,
  kCGLPFADisplayMask = 84,
  AGL_PBUFFER = 90,
  AGL_BUFFER_MODES = 100,
  AGL_COLOR_MODES = 103,
  AGL_VIDEO_MEMORY = 120,
  AGL_TEXTURE_MEMORY = 121,
  AGL_RENDERER_COUNT = 128,
  AGL_SWAP_INTERVAL = 222,
};

enum {
  AGL_NO_ERROR = 0,
  AGL_BAD_ATTRIBUTE = 10000,
  AGL_BAD_PROPERTY = 10001,
  AGL_BAD_PIXELFMT = 10002,
  AGL_BAD_RENDINFO = 10003,
  AGL_BAD_CONTEXT = 10004,
  AGL_BAD_DRAWABLE = 10005,
  AGL_BAD_VALUE = 10008,
  AGL_BAD_ALLOC = 10016,
};

typedef struct {
  uint32_t magic;
  int double_buffer;
  int depth;
  int stencil;
  int alpha;
  int color;
  int samples;
  int fullscreen;
  int pbuffer;
  int plain;          // the requested visual was not there; SDL's default is
  SDL_Window* probe;  // hidden, for creating contexts of this format
} hle_pixel_format;

typedef struct hle_gl_context {
  void* rend;
  void* disp[686];
  void* priv;
  void* stak;
  uint32_t magic;
  SDL_GLContext gl;
  SDL_Window* window;  // where it draws now
  hle_pixel_format* format;
  void* drawable;
  int fullscreen;
  struct hle_pbuffer* pbuffer;  // the pbuffer it draws in, or NULL
  struct hle_gl_context* next;  // in contexts
} hle_gl_context;

typedef struct {
  uint32_t magic;
  int count;
} hle_renderer_info;

// Every context, so that a window's destruction moves them off it; guarded
// by lock.
static hle_gl_context* contexts;
static unsigned context_switches;  // real ones since the last frame trace
static __thread hle_gl_context* current;
static __thread int agl_error;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int dispatch_resolved;

// ---------------------------------------------------------------------------
// The extension string
//
// The game copies GL_EXTENSIONS into a 4096-byte buffer, and a later driver
// lists far more than a 2006 Mac did. It is given the extensions whose names
// its executable contains, the only ones it can ask about, plus
// EXT_texture_rectangle where the driver has ARB_texture_rectangle, which
// uses the same enumerants.

enum {
  GL_EXTENSIONS = 0x1F03,
  kMaxExtensionString = 4000,
};

static int is_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// Whether |text| holds the |length| bytes at |name| as a whole word.
static int has_word(const char* text, size_t size, const char* name,
                    size_t length) {
  const char* end = text + size;
  for (const char* p = text; p < end &&
       (p = memmem(p, end - p, name, length)); p += length) {
    int starts = p == text || !is_name_char(p[-1]);
    int ends = p + length == end || !is_name_char(p[length]);
    if (starts && ends) {
      return 1;
    }
  }
  return 0;
}

static char* read_executable(size_t* size) {
  FILE* f = fopen(__darwin_executable_path, "rb");
  if (!f) {
    return NULL;
  }
  cf_buf contents = { 0 };
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    cf_buf_append(&contents, chunk, n);
  }
  fclose(f);
  *size = contents.len;
  return contents.data;
}

static const char* game_extensions(const char* all) {
  static pthread_mutex_t extensions_lock = PTHREAD_MUTEX_INITIALIZER;
  static char* extensions;
  pthread_mutex_lock(&extensions_lock);
  if (!extensions) {
    size_t image_size = 0;
    char* image = read_executable(&image_size);
    cf_buf out = { 0 };
    cf_buf_appends(&out, "");
    for (const char* p = all; *p;) {
      while (*p == ' ') {
        p++;
      }
      const char* end = p;
      while (*end && *end != ' ') {
        end++;
      }
      size_t length = end - p;
      if (length && (!image || has_word(image, image_size, p, length)) &&
          out.len + length + 1 < kMaxExtensionString) {
        cf_buf_append(&out, p, length);
        cf_buf_append(&out, " ", 1);
      }
      p = end;
    }
    static const char kArbRectangle[] = "GL_ARB_texture_rectangle";
    static const char kExtRectangle[] = "GL_EXT_texture_rectangle";
    size_t all_size = strlen(all);
    if (image &&
        has_word(all, all_size, kArbRectangle, sizeof(kArbRectangle) - 1) &&
        !has_word(all, all_size, kExtRectangle, sizeof(kExtRectangle) - 1) &&
        has_word(image, image_size, kExtRectangle, sizeof(kExtRectangle) - 1)) {
      cf_buf_appends(&out, kExtRectangle);
      cf_buf_append(&out, " ", 1);
    }
    // Apple's vertex array range and fence, over buffer objects (gl_var.c).
    static const char kArbBuffers[] = "GL_ARB_vertex_buffer_object";
    if (hle_gl_var_enabled() &&
        has_word(all, all_size, kArbBuffers, sizeof(kArbBuffers) - 1)) {
      cf_buf_appends(&out, "GL_APPLE_vertex_array_range GL_APPLE_fence ");
    }
    free(image);
    extensions = out.data;
    cf_trace("GL_EXTENSIONS: %zu bytes of the driver's %zu, the extensions "
             "the game names", strlen(extensions), all_size);
  }
  pthread_mutex_unlock(&extensions_lock);
  return extensions;
}

// The game's glGetString, through rename.tab and the dispatch table.
const uint8_t* __darwin_glGetString(uint32_t name) {
  static const uint8_t* (*real_get_string)(uint32_t);
  if (!real_get_string) {
    real_get_string = dlsym(RTLD_DEFAULT, "glGetString");
  }
  const uint8_t* s = real_get_string ? real_get_string(name) : NULL;
  if (s && name == GL_EXTENSIONS) {
    return (const uint8_t*)game_extensions((const char*)s);
  }
  return s;
}

enum {
  GL_PROGRAM_ERROR_POSITION_ARB = 0x864B,
  GL_PROGRAM_ERROR_STRING_ARB = 0x8874,
  kMaxProgramReports = 20,
};

// The game's glProgramStringARB, through rename.tab and the dispatch table.
// Its programs get arb_program.c's ALIAS rewrite, and a program the driver
// still refuses is reported with the driver's message. The error position
// is read rather than glGetError, which would take the error from the game.
void __darwin_glProgramStringARB(uint32_t target, uint32_t format,
                                 int32_t length, const void* program) {
  static void (*real_program_string)(uint32_t, uint32_t, int32_t,
                                     const void*);
  static void (*get_integer)(uint32_t, int32_t*);
  static const uint8_t* (*get_string)(uint32_t);
  static int reports;
  static int rewrote;
  if (!real_program_string) {
    real_program_string = dlsym(RTLD_DEFAULT, "glProgramStringARB");
    get_integer = dlsym(RTLD_DEFAULT, "glGetIntegerv");
    get_string = dlsym(RTLD_DEFAULT, "glGetString");
  }
  if (!real_program_string) {
    return;
  }
  char* rewritten = program ? hle_arb_rewrite_aliases(program, &length)
                            : NULL;
  if (rewritten && !rewrote) {
    rewrote = 1;
    cf_trace("glProgramStringARB: ALIAS of a result or vertex binding is "
             "declared as OUTPUT or ATTRIB");
  }
  const char* text = rewritten ? rewritten : program;
  real_program_string(target, format, length, text);
  if (get_integer && reports < kMaxProgramReports) {
    int32_t position = -1;
    get_integer(GL_PROGRAM_ERROR_POSITION_ARB, &position);
    if (position != -1) {
      reports++;
      const char* message =
          get_string ? (const char*)get_string(GL_PROGRAM_ERROR_STRING_ARB)
                     : NULL;
      char context[80];
      int n = 0;
      for (int32_t i = position; text && i < length && n < 79 &&
           text[i] != '\n'; i++) {
        context[n++] = text[i];
      }
      context[n] = '\0';
      fprintf(stderr, "hle: glProgramStringARB(%#x) refused at %d: %s\n"
              "hle:   there: %s\n", (unsigned)target, position,
              message ? message : "", context);
    }
  }
  free(rewritten);
}

static void* lookup_gl(const char* name) {
  if (!strcmp(name, "glGetString")) {
    return __darwin_glGetString;
  }
  if (!strcmp(name, "glProgramStringARB")) {
    return __darwin_glProgramStringARB;
  }
  return hle_gl_stats_wrap(name,
                           hle_gl_var_wrap(name, dlsym(RTLD_DEFAULT, name)));
}

// ---------------------------------------------------------------------------
// The renderer

static int vram_bytes(void) {
  const char* mb = getenv("HLE_VRAM_MB");
  long n = mb ? strtol(mb, NULL, 10) : 256;
  if (n <= 0 || n > 2047) {
    n = 256;
  }
  return (int)(n << 20);
}

static int describe_renderer(int property, int32_t* value) {
  switch (property) {
    case AGL_RENDERER_ID: *value = kRendererGeForceFX; break;
    case AGL_ACCELERATED: *value = 1; break;
    case 75: *value = 0; break;           // robust
    case 76: *value = 1; break;           // backing store
    case 78: *value = 1; break;           // MP safe
    case AGL_WINDOW: *value = 1; break;
    case 81: *value = 0; break;           // multiscreen
    case 83: *value = 1; break;           // compliant
    case kCGLPFADisplayMask: *value = 1; break;
    case AGL_OFFSCREEN: *value = 0; break;
    case AGL_FULLSCREEN: *value = 1; break;
    case AGL_BUFFER_MODES: *value = 0x1 | 0x4 | 0x8; break;
    case 101: case 102: *value = 0; break;  // min and max level
    case AGL_COLOR_MODES: *value = 0x00004000 | 0x00008000; break;
    case 104: *value = 0x00004000 | 0x00008000; break;  // accum modes
    case 105: *value = 0x1 | 0x400 | 0x800; break;      // depth 0, 16, 24
    case 106: *value = 0x1 | 0x80; break;               // stencil 0, 8
    case 107: *value = 0; break;          // max aux buffers
    case 108: *value = 1; break;          // max sample buffers
    case 109: *value = 4; break;          // max samples
    case 110: *value = 0x2; break;        // multisample
    case 111: *value = 1; break;          // sample alpha
    case AGL_VIDEO_MEMORY:
    case AGL_TEXTURE_MEMORY: *value = vram_bytes(); break;
    case 122: case 123: *value = 1; break;  // GPU vertex, fragment processing
    case AGL_RENDERER_COUNT: *value = 1; break;
    default: return 0;
  }
  return 1;
}

static hle_renderer_info* new_renderer_info(void) {
  hle_renderer_info* info = calloc(1, sizeof(*info));
  info->magic = kMagicRendererInfo;
  info->count = 1;
  return info;
}

static hle_renderer_info* renderer_info_of(void* ref) {
  hle_renderer_info* info = ref;
  return info && info->magic == kMagicRendererInfo ? info : NULL;
}

// ---------------------------------------------------------------------------
// Pixel formats

static int takes_value(int attribute, int cgl) {
  switch (attribute) {
    case AGL_BUFFER_SIZE: case AGL_LEVEL: case AGL_AUX_BUFFERS:
    case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE:
    case AGL_ALPHA_SIZE: case AGL_DEPTH_SIZE: case AGL_STENCIL_SIZE:
    case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
    case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_PIXEL_SIZE:
    case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB: case AGL_RENDERER_ID:
    case AGL_VIRTUAL_SCREEN:
      return 1;
    case kCGLPFADisplayMask:
      return cgl;
    default:
      return 0;
  }
}

// CGL's color size counts all channels where AGL's counts one.
static hle_pixel_format* parse_format(const int32_t* attributes, int cgl) {
  hle_pixel_format* f = calloc(1, sizeof(*f));
  f->magic = kMagicPixelFormat;
  f->color = 24;
  int sample_buffers = 0;
  for (const int32_t* a = attributes; a && *a != AGL_NONE; a++) {
    int value = takes_value(*a, cgl) ? a[1] : 0;
    switch (*a) {
      case AGL_DOUBLEBUFFER: f->double_buffer = 1; break;
      case AGL_DEPTH_SIZE: f->depth = value; break;
      case AGL_STENCIL_SIZE: f->stencil = value; break;
      case AGL_ALPHA_SIZE: f->alpha = value; break;
      case AGL_RED_SIZE:
        f->color = cgl ? value : value * 3;
        break;
      case AGL_BUFFER_SIZE: f->color = value; break;
      case AGL_SAMPLE_BUFFERS_ARB: sample_buffers = value; break;
      case AGL_SAMPLES_ARB: f->samples = value; break;
      case AGL_FULLSCREEN: f->fullscreen = 1; break;
      case AGL_PBUFFER: f->pbuffer = 1; break;
    }
    if (takes_value(*a, cgl)) {
      a++;
    }
  }
  if (!sample_buffers) {
    f->samples = 0;
  }
  return f;
}

static hle_pixel_format* format_of(void* ref) {
  hle_pixel_format* f = ref;
  return f && f->magic == kMagicPixelFormat ? f : NULL;
}

// The visual every window and context of a format is made with. A GLX
// server need not offer single-buffered or depthless visuals, and a larger
// one serves the same drawing, so those are always requested. When even
// that fails, |f->plain| falls back to SDL's defaults.
static void apply_format(const hle_pixel_format* f) {
  SDL_GL_ResetAttributes();
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  if (f->plain) {
    return;
  }
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, f->alpha ? 8 : 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, f->depth > 0 && f->depth <= 16 ? 16
                                                                         : 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, f->stencil ? 8 : 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, f->samples ? 1 : 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, f->samples);
}

static void destroy_format(hle_pixel_format* f) {
  if (f->probe) {
    SDL_DestroyWindow(f->probe);
  }
  f->magic = 0;
  free(f);
}

// ---------------------------------------------------------------------------
// Contexts

static hle_gl_context* context_of(void* ref) {
  hle_gl_context* c = ref;
  return c && c->magic == kMagicContext ? c : NULL;
}

static void make_current(hle_gl_context* c) {
  SDL_Window* window = c ? c->window : NULL;
  SDL_GLContext gl = c ? c->gl : NULL;
  // SDL does nothing when both are current already; anything else is a
  // glXMakeCurrent, a round trip to the X server.
  if (window != SDL_GL_GetCurrentWindow() ||
      gl != SDL_GL_GetCurrentContext()) {
    context_switches++;
  }
  if (gl != SDL_GL_GetCurrentContext()) {
    // Buffer bindings belong to a context.
    hle_gl_var_context_changed();
  }
  SDL_GL_MakeCurrent(window, gl);
  current = c;
}

static hle_gl_context* create_context(hle_pixel_format* f,
                                      hle_gl_context* share) {
  if (!hle_sdl_video()) {
    agl_error = AGL_BAD_CONTEXT;
    return NULL;
  }
  pthread_mutex_lock(&lock);
  if (!dispatch_resolved) {
    int missing = hle_gl_resolve(lookup_gl);
    cf_trace("GL dispatch: %d of %d entries have no GL function", missing,
             hle_gl_thunk_count);
    dispatch_resolved = 1;
  }
  pthread_mutex_unlock(&lock);

  apply_format(f);
  if (!f->probe) {
    f->probe = SDL_CreateWindow("", 0, 0, 16, 16,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!f->probe && !f->plain) {
      fprintf(stderr, "hle: GL visual (depth %d, stencil %d, alpha %d, "
              "%d samples): %s; trying SDL's default\n", f->depth, f->stencil,
              f->alpha, f->samples, SDL_GetError());
      f->plain = 1;
      apply_format(f);
      f->probe = SDL_CreateWindow("", 0, 0, 16, 16,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    }
    if (!f->probe) {
      fprintf(stderr, "hle: GL window: %s\n", SDL_GetError());
      agl_error = AGL_BAD_PIXELFMT;
      return NULL;
    }
  }
  hle_gl_context* previous = current;
  if (share) {
    SDL_GL_MakeCurrent(share->window, share->gl);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  }
  SDL_GLContext gl = SDL_GL_CreateContext(f->probe);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  // Creating a context makes it current; the game did not ask for that.
  make_current(previous);
  if (!gl) {
    fprintf(stderr, "hle: GL context: %s\n", SDL_GetError());
    agl_error = AGL_BAD_CONTEXT;
    return NULL;
  }
  hle_gl_context* c = calloc(1, sizeof(*c));
  c->rend = c;
  memcpy(c->disp, hle_gl_thunks, sizeof(void*) * hle_gl_thunk_count);
  c->magic = kMagicContext;
  c->gl = gl;
  c->window = f->probe;
  c->format = f;
  pthread_mutex_lock(&lock);
  c->next = contexts;
  contexts = c;
  pthread_mutex_unlock(&lock);
  return c;
}

// Pbuffers, below. A context given a drawable stops drawing in its pbuffer,
// and a context destroyed takes its framebuffer objects with it.
static void leave_pbuffer(hle_gl_context* c);
static void forget_pbuffers_of(hle_gl_context* c);

static void destroy_context(hle_gl_context* c) {
  forget_pbuffers_of(c);
  if (current == c) {
    make_current(NULL);
  }
  pthread_mutex_lock(&lock);
  for (hle_gl_context** link = &contexts; *link; link = &(*link)->next) {
    if (*link == c) {
      *link = c->next;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  SDL_GL_DeleteContext(c->gl);
  c->magic = 0;
  free(c);
}

// Moves |c| to |window|, keeping it current if it was.
static void move_to(hle_gl_context* c, SDL_Window* window) {
  c->window = window ? window : c->format->probe;
  if (current == c) {
    make_current(c);
  }
}

void hle_gl_window_destroyed(void* sdl_window) {
  // A context detached from its window still draws there (see
  // aglSetDrawable) and moves to its hidden probe window now. The game draws
  // on one thread, so only a context current on this one is rebound.
  pthread_mutex_lock(&lock);
  for (hle_gl_context* c = contexts; c; c = c->next) {
    if (c->window == sdl_window) {
      move_to(c, NULL);
    }
  }
  pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// AGL

void aglGetVersion(int32_t* major, int32_t* minor) {
  if (major) {
    *major = 3;
  }
  if (minor) {
    *minor = 0;
  }
}

uint32_t aglGetError(void) {
  int err = agl_error;
  agl_error = AGL_NO_ERROR;
  return err;
}

void* aglChoosePixelFormat(const void* devices, int32_t count,
                           const int32_t* attributes) {
  return parse_format(attributes, 0);
}

void aglDestroyPixelFormat(void* pix) {
  hle_pixel_format* f = format_of(pix);
  if (f) {
    destroy_format(f);
  }
}

void* aglNextPixelFormat(void* pix) {
  return NULL;
}

Boolean aglDescribePixelFormat(void* pix, int32_t attribute, int32_t* value) {
  hle_pixel_format* f = format_of(pix);
  if (!f || !value) {
    agl_error = AGL_BAD_PIXELFMT;
    return 0;
  }
  switch (attribute) {
    case AGL_DOUBLEBUFFER: *value = f->double_buffer; break;
    case AGL_DEPTH_SIZE: *value = f->depth; break;
    case AGL_STENCIL_SIZE: *value = f->stencil; break;
    case AGL_ALPHA_SIZE: *value = f->alpha; break;
    case AGL_SAMPLES_ARB: *value = f->samples; break;
    case AGL_SAMPLE_BUFFERS_ARB: *value = f->samples ? 1 : 0; break;
    case AGL_RGBA: *value = 1; break;
    case AGL_PIXEL_SIZE: *value = 32; break;
    default:
      if (!describe_renderer(attribute, value)) {
        *value = 0;
      }
  }
  return 1;
}

void* aglQueryRendererInfo(const void* devices, int32_t count) {
  return new_renderer_info();
}

void* aglNextRendererInfo(void* rend) {
  return NULL;
}

void aglDestroyRendererInfo(void* rend) {
  hle_renderer_info* info = renderer_info_of(rend);
  if (info) {
    info->magic = 0;
    free(info);
  }
}

Boolean aglDescribeRenderer(void* rend, int32_t property, int32_t* value) {
  if (!renderer_info_of(rend) || !value) {
    agl_error = AGL_BAD_RENDINFO;
    return 0;
  }
  if (!describe_renderer(property, value)) {
    agl_error = AGL_BAD_PROPERTY;
    return 0;
  }
  return 1;
}

// The game destroys a pixel format as soon as it has a context from it, so
// a context keeps a copy of its own, with its own hidden window.
static hle_pixel_format* copy_format(const hle_pixel_format* f) {
  hle_pixel_format* own = malloc(sizeof(*own));
  *own = *f;
  own->probe = NULL;
  return own;
}

void* aglCreateContext(void* pix, void* share) {
  hle_pixel_format* f = format_of(pix);
  if (!f) {
    agl_error = AGL_BAD_PIXELFMT;
    return NULL;
  }
  hle_pixel_format* own = copy_format(f);
  hle_gl_context* c = create_context(own, context_of(share));
  if (!c) {
    destroy_format(own);
  }
  return c;
}

Boolean aglDestroyContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  hle_pixel_format* f = c->format;
  destroy_context(c);
  destroy_format(f);
  return 1;
}

Boolean aglSetCurrentContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (ctx && !c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  make_current(c);
  return 1;
}

void* aglGetCurrentContext(void) {
  return current;
}

Boolean aglSetDrawable(void* ctx, void* drawable) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  leave_pbuffer(c);
  c->drawable = drawable;
  c->fullscreen = 0;
  if (!drawable) {
    // Detached, the context stays bound to the window it drew in. The game
    // detaches its window after each pbuffer it draws in and attaches it
    // again at once, and a move to the hidden probe window and back would be
    // two glXMakeCurrent calls. Swaps do nothing until it is attached again,
    // and a window destroyed first moves it (hle_gl_window_destroyed).
    return 1;
  }
  hle_port* port = drawable;
  hle_window* window = port->magic == kHleMagicPort ? port->window : NULL;
  if (!window) {
    agl_error = AGL_BAD_DRAWABLE;
    return 0;
  }
  apply_format(c->format);
  SDL_Window* sdl = hle_sdl_window_for(
      window, hle_window_wants_fullscreen(window),
      window->content.right - window->content.left,
      window->content.bottom - window->content.top);
  if (!sdl) {
    agl_error = AGL_BAD_DRAWABLE;
    return 0;
  }
  move_to(c, sdl);
  return 1;
}

void* aglGetDrawable(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  return c ? c->drawable : NULL;
}

Boolean aglSetFullScreen(void* ctx, int32_t width, int32_t height,
                         int32_t frequency, int32_t device) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  leave_pbuffer(c);
  const char* windowed = getenv("HLE_WINDOWED");
  int fullscreen = !(windowed && *windowed == '1');
  apply_format(c->format);
  SDL_Window* sdl = hle_sdl_window_for(NULL, fullscreen, width, height);
  if (!sdl) {
    agl_error = AGL_BAD_ALLOC;
    return 0;
  }
  cf_trace("aglSetFullScreen(%dx%d@%d)%s", width, height, frequency,
           fullscreen ? "" : " in a window");
  c->drawable = NULL;
  c->fullscreen = 1;
  move_to(c, sdl);
  return 1;
}

Boolean aglUpdateContext(void* ctx) {
  return context_of(ctx) != NULL;
}

enum {
  GL_BACK = 0x0405,
  GL_READ_BUFFER = 0x0C02,
  GL_PACK_ALIGNMENT = 0x0D05,
  GL_UNSIGNED_BYTE = 0x1401,
  GL_RGB = 0x1907,
  kFramesPerTrace = 600,
};

// HLE_FRAME_DUMP=<directory> saves each frame aglSwapBuffers traces, as the
// game drew it, to frame-<n>.ppm there.
static void dump_frame(hle_gl_context* c, unsigned frame) {
  static const char* directory;
  static int looked;
  if (!looked) {
    looked = 1;
    directory = getenv("HLE_FRAME_DUMP");
  }
  if (!directory || !*directory || c != current) {
    return;
  }
  void (*get_integer)(uint32_t, int32_t*) =
      dlsym(RTLD_DEFAULT, "glGetIntegerv");
  void (*read_buffer)(uint32_t) = dlsym(RTLD_DEFAULT, "glReadBuffer");
  void (*pixel_store)(uint32_t, int32_t) =
      dlsym(RTLD_DEFAULT, "glPixelStorei");
  void (*read_pixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                      uint32_t, void*) = dlsym(RTLD_DEFAULT, "glReadPixels");
  int width = 0;
  int height = 0;
  SDL_GL_GetDrawableSize(c->window, &width, &height);
  unsigned char* pixels =
      width > 0 && height > 0 ? malloc((size_t)width * 3 * height) : NULL;
  if (!get_integer || !read_buffer || !pixel_store || !read_pixels ||
      !pixels) {
    free(pixels);
    return;
  }
  int32_t buffer = GL_BACK;
  int32_t alignment = 4;
  get_integer(GL_READ_BUFFER, &buffer);
  get_integer(GL_PACK_ALIGNMENT, &alignment);
  read_buffer(GL_BACK);
  pixel_store(GL_PACK_ALIGNMENT, 1);
  read_pixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
  pixel_store(GL_PACK_ALIGNMENT, alignment);
  read_buffer(buffer);

  char path[4096];
  snprintf(path, sizeof(path), "%s/frame-%06u.ppm", directory, frame);
  FILE* f = fopen(path, "wb");
  if (f) {
    // OpenGL's rows run bottom to top.
    size_t row = (size_t)width * 3;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; y--) {
      fwrite(pixels + row * y, 1, row, f);
    }
    fclose(f);
    cf_trace("aglSwapBuffers: frame %u saved to %s", frame, path);
  } else {
    fprintf(stderr, "hle: HLE_FRAME_DUMP: cannot write %s: %m\n", path);
  }
  free(pixels);
}

static double clock_seconds(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

void aglSwapBuffers(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    return;
  }
  if (c->window == c->format->probe || (!c->drawable && !c->fullscreen)) {
    cf_warn_once("aglSwapBuffers on a context with no drawable");
    return;
  }
  if (c->pbuffer) {
    // A pbuffer has one buffer; there is nothing to swap.
    return;
  }
  // HLE_PROFILE starts at the first frame, from the game's call.
  hle_profile_start(__builtin_return_address(0));
  static unsigned swaps;
  static Uint32 traced_at;
  static Uint32 last_swap;
  static Uint32 longest;  // between two swaps since the last trace, in ms
  static unsigned over_50;
  static unsigned over_100;
  static double swapping;  // seconds in SDL_GL_SwapWindow since the trace
  static double traced_wall;
  static double traced_thread_cpu;
  static double traced_process_cpu;
  int traced = ++swaps == 1 || swaps % kFramesPerTrace == 0;
  if (traced) {
    dump_frame(c, swaps);
  }
  if (c == current) {
    hle_screenshot_take(c->window);
  }
  double before = clock_seconds(CLOCK_MONOTONIC);
  SDL_GL_SwapWindow(c->window);
  double after = clock_seconds(CLOCK_MONOTONIC);
  swapping += after - before;
  static double last_swapped;
  if (last_swapped > 0) {
    hle_profile_frame(swaps, (after - last_swapped) * 1000,
                      (after - before) * 1000);
  }
  last_swapped = after;
  Uint32 now = SDL_GetTicks();
  if (last_swap) {
    Uint32 frame = now - last_swap;
    longest = frame > longest ? frame : longest;
    over_50 += frame > 50;
    over_100 += frame > 100;
  }
  last_swap = now;
  if (traced) {
    double thread_cpu = clock_seconds(CLOCK_THREAD_CPUTIME_ID);
    double process_cpu = clock_seconds(CLOCK_PROCESS_CPUTIME_ID);
    // The frames since the last trace.
    unsigned frames = swaps == 1                 ? 1
                      : swaps == kFramesPerTrace ? kFramesPerTrace - 1
                                                 : kFramesPerTrace;
    char gl_counts[512];
    hle_gl_stats_take(gl_counts, sizeof(gl_counts), frames);
    if (swaps == 1) {
      cf_trace("aglSwapBuffers: frame 1");
    } else {
      Uint32 elapsed = now - traced_at;
      double ms = 1000.0 / frames;
      // Where a frame's time goes: this thread, the game's, working; every
      // thread working, the sound mixer's included; and waiting in the swap.
      cf_trace("aglSwapBuffers: frame %u, %.1f frames a second, the longest "
               "%u ms, %u over 50 ms and %u over 100; a frame takes %.1f ms, "
               "%.1f of them on this thread's CPU and %.1f on all threads', "
               "%.1f in the swap; %u context switches%s",
               swaps, frames * 1000.0 / (elapsed ? elapsed : 1), longest,
               over_50, over_100, (after - traced_wall) * ms,
               (thread_cpu - traced_thread_cpu) * ms,
               (process_cpu - traced_process_cpu) * ms, swapping * ms,
               context_switches, gl_counts);
    }
    traced_at = now;
    longest = 0;
    over_50 = 0;
    over_100 = 0;
    swapping = 0;
    context_switches = 0;
    traced_wall = after;
    traced_thread_cpu = thread_cpu;
    traced_process_cpu = process_cpu;
  }
}

Boolean aglSetInteger(void* ctx, uint32_t name, const int32_t* params) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  if (name == AGL_SWAP_INTERVAL && params) {
    hle_gl_context* previous = current;
    make_current(c);
    SDL_GL_SetSwapInterval(params[0]);
    make_current(previous);
  }
  return 1;
}

Boolean aglGetInteger(void* ctx, uint32_t name, int32_t* params) {
  hle_gl_context* c = context_of(ctx);
  if (!c || !params) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  params[0] = name == AGL_SWAP_INTERVAL ? SDL_GL_GetSwapInterval() : 0;
  return 1;
}

Boolean aglEnable(void* ctx, uint32_t name) {
  return context_of(ctx) != NULL;
}

Boolean aglDisable(void* ctx, uint32_t name) {
  return context_of(ctx) != NULL;
}

int32_t aglGetVirtualScreen(void* ctx) {
  return 0;
}

// ---------------------------------------------------------------------------
// Pbuffers
//
// A pbuffer is a framebuffer object here. aglTexImagePBuffer takes the
// texture bound to the pbuffer's target as the pbuffer's image, and
// aglSetPBuffer binds a framebuffer object drawing into that texture, so
// what the game draws in the pbuffer is at once the texture's image, as on
// a Mac. Giving the context a drawable, or the full screen, draws in the
// window again. Halo makes one the size of its screen and three small ones.

enum {
  kMagicPBuffer = 'pbuf',
  kTracedPBuffers = 8,
  GL_TEXTURE_2D = 0x0DE1,
  GL_RGBA = 0x1908,
  GL_TEXTURE_BINDING_2D = 0x8069,
  GL_TEXTURE_RECTANGLE_ARB = 0x84F5,
  GL_TEXTURE_BINDING_RECTANGLE_ARB = 0x84F6,
  GL_DEPTH24_STENCIL8_EXT = 0x88F0,
  GL_FRAMEBUFFER_BINDING_EXT = 0x8CA6,
  GL_FRAMEBUFFER_COMPLETE_EXT = 0x8CD5,
  GL_COLOR_ATTACHMENT0_EXT = 0x8CE0,
  GL_DEPTH_ATTACHMENT_EXT = 0x8D00,
  GL_STENCIL_ATTACHMENT_EXT = 0x8D20,
  GL_FRAMEBUFFER_EXT = 0x8D40,
  GL_RENDERBUFFER_EXT = 0x8D41,
};

typedef struct hle_pbuffer {
  uint32_t magic;
  int32_t width;
  int32_t height;
  uint32_t target;
  int32_t internal_format;
  uint32_t texture;  // whose image the pbuffer is
  int own_texture;  // made here, when the game gave none
  hle_gl_context* owner;  // the context its framebuffer object is in
  uint32_t framebuffer;
  uint32_t depth_stencil;
  int complete;
  struct hle_pbuffer* next;
} hle_pbuffer;

static hle_pbuffer* pbuffers;

static struct {
  void (*get_integer)(uint32_t, int32_t*);
  void (*gen_textures)(int32_t, uint32_t*);
  void (*delete_textures)(int32_t, const uint32_t*);
  void (*bind_texture)(uint32_t, uint32_t);
  void (*tex_image_2d)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                       uint32_t, uint32_t, const void*);
  void (*gen_framebuffers)(int32_t, uint32_t*);
  void (*delete_framebuffers)(int32_t, const uint32_t*);
  void (*bind_framebuffer)(uint32_t, uint32_t);
  void (*framebuffer_texture_2d)(uint32_t, uint32_t, uint32_t, uint32_t,
                                 int32_t);
  uint32_t (*check_framebuffer_status)(uint32_t);
  void (*gen_renderbuffers)(int32_t, uint32_t*);
  void (*delete_renderbuffers)(int32_t, const uint32_t*);
  void (*bind_renderbuffer)(uint32_t, uint32_t);
  void (*renderbuffer_storage)(uint32_t, uint32_t, int32_t, int32_t);
  void (*framebuffer_renderbuffer)(uint32_t, uint32_t, uint32_t, uint32_t);
} fbo;

static int resolve_fbo(void) {
  static int resolved;
  if (!resolved) {
    resolved = 1;
    fbo.get_integer = dlsym(RTLD_DEFAULT, "glGetIntegerv");
    fbo.gen_textures = dlsym(RTLD_DEFAULT, "glGenTextures");
    fbo.delete_textures = dlsym(RTLD_DEFAULT, "glDeleteTextures");
    fbo.bind_texture = dlsym(RTLD_DEFAULT, "glBindTexture");
    fbo.tex_image_2d = dlsym(RTLD_DEFAULT, "glTexImage2D");
    fbo.gen_framebuffers = dlsym(RTLD_DEFAULT, "glGenFramebuffersEXT");
    fbo.delete_framebuffers = dlsym(RTLD_DEFAULT, "glDeleteFramebuffersEXT");
    fbo.bind_framebuffer = dlsym(RTLD_DEFAULT, "glBindFramebufferEXT");
    fbo.framebuffer_texture_2d =
        dlsym(RTLD_DEFAULT, "glFramebufferTexture2DEXT");
    fbo.check_framebuffer_status =
        dlsym(RTLD_DEFAULT, "glCheckFramebufferStatusEXT");
    fbo.gen_renderbuffers = dlsym(RTLD_DEFAULT, "glGenRenderbuffersEXT");
    fbo.delete_renderbuffers =
        dlsym(RTLD_DEFAULT, "glDeleteRenderbuffersEXT");
    fbo.bind_renderbuffer = dlsym(RTLD_DEFAULT, "glBindRenderbufferEXT");
    fbo.renderbuffer_storage = dlsym(RTLD_DEFAULT, "glRenderbufferStorageEXT");
    fbo.framebuffer_renderbuffer =
        dlsym(RTLD_DEFAULT, "glFramebufferRenderbufferEXT");
  }
  return fbo.get_integer && fbo.gen_textures && fbo.delete_textures &&
         fbo.bind_texture && fbo.tex_image_2d && fbo.gen_framebuffers &&
         fbo.delete_framebuffers && fbo.bind_framebuffer &&
         fbo.framebuffer_texture_2d && fbo.check_framebuffer_status &&
         fbo.gen_renderbuffers && fbo.delete_renderbuffers &&
         fbo.bind_renderbuffer && fbo.renderbuffer_storage &&
         fbo.framebuffer_renderbuffer;
}

static hle_pbuffer* pbuffer_of(void* ref) {
  hle_pbuffer* pb = ref;
  return pb && pb->magic == kMagicPBuffer ? pb : NULL;
}

// Makes |c| current for a moment; give_back restores what was.
static hle_gl_context* borrow(hle_gl_context* c) {
  hle_gl_context* previous = current;
  if (previous != c) {
    make_current(c);
  }
  return previous;
}

static void give_back(hle_gl_context* c, hle_gl_context* previous) {
  if (previous != c) {
    make_current(previous);
  }
}

static uint32_t binding_of(uint32_t target) {
  return target == GL_TEXTURE_RECTANGLE_ARB ? GL_TEXTURE_BINDING_RECTANGLE_ARB
                                            : GL_TEXTURE_BINDING_2D;
}

// Gives the pbuffer's texture storage of its size, leaving the texture bound
// to the target as it was. Called with a context current.
static void size_texture(hle_pbuffer* pb) {
  int32_t bound = 0;
  fbo.get_integer(binding_of(pb->target), &bound);
  fbo.bind_texture(pb->target, pb->texture);
  fbo.tex_image_2d(pb->target, 0, pb->internal_format, pb->width, pb->height,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  fbo.bind_texture(pb->target, (uint32_t)bound);
}

// Called with the owner current.
static void attach_texture(hle_pbuffer* pb) {
  int32_t bound = 0;
  fbo.get_integer(GL_FRAMEBUFFER_BINDING_EXT, &bound);
  fbo.bind_framebuffer(GL_FRAMEBUFFER_EXT, pb->framebuffer);
  fbo.framebuffer_texture_2d(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                             pb->target, pb->texture, 0);
  fbo.bind_framebuffer(GL_FRAMEBUFFER_EXT, (uint32_t)bound);
}

// Makes the pbuffer's framebuffer object in |c|, drawing into its texture,
// with a depth and stencil buffer of its size. A framebuffer object belongs
// to one context; one made in another is left to that context. Called with
// |c| current.
static int make_framebuffer(hle_pbuffer* pb, hle_gl_context* c) {
  if (pb->owner == c && pb->framebuffer) {
    return pb->complete;
  }
  pb->owner = c;
  fbo.gen_framebuffers(1, &pb->framebuffer);
  fbo.gen_renderbuffers(1, &pb->depth_stencil);
  fbo.bind_renderbuffer(GL_RENDERBUFFER_EXT, pb->depth_stencil);
  fbo.renderbuffer_storage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT,
                           pb->width, pb->height);
  fbo.bind_renderbuffer(GL_RENDERBUFFER_EXT, 0);
  attach_texture(pb);
  int32_t bound = 0;
  fbo.get_integer(GL_FRAMEBUFFER_BINDING_EXT, &bound);
  fbo.bind_framebuffer(GL_FRAMEBUFFER_EXT, pb->framebuffer);
  fbo.framebuffer_renderbuffer(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                               GL_RENDERBUFFER_EXT, pb->depth_stencil);
  fbo.framebuffer_renderbuffer(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                               GL_RENDERBUFFER_EXT, pb->depth_stencil);
  uint32_t status = fbo.check_framebuffer_status(GL_FRAMEBUFFER_EXT);
  fbo.bind_framebuffer(GL_FRAMEBUFFER_EXT, (uint32_t)bound);
  pb->complete = status == GL_FRAMEBUFFER_COMPLETE_EXT;
  if (!pb->complete) {
    fprintf(stderr, "hle: a %dx%d pbuffer's framebuffer object is "
            "incomplete (%#x)\n", pb->width, pb->height, (unsigned)status);
  } else {
    cf_trace("aglSetPBuffer: a %dx%d framebuffer object draws into texture "
             "%u", pb->width, pb->height, pb->texture);
  }
  return pb->complete;
}

static void leave_pbuffer(hle_gl_context* c) {
  if (!c->pbuffer) {
    return;
  }
  hle_gl_context* previous = borrow(c);
  fbo.bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
  give_back(c, previous);
  c->pbuffer = NULL;
}

static void forget_pbuffers_of(hle_gl_context* c) {
  for (hle_pbuffer* pb = pbuffers; pb; pb = pb->next) {
    if (pb->owner == c) {
      pb->owner = NULL;
      pb->framebuffer = 0;
      pb->depth_stencil = 0;
      pb->complete = 0;
    }
  }
}

Boolean aglCreatePBuffer(int32_t width, int32_t height, uint32_t target,
                         uint32_t internal_format, int32_t max_level,
                         void** pbuffer) {
  static int traced;
  if (traced < kTracedPBuffers) {
    traced++;
    cf_trace("aglCreatePBuffer(%dx%d, target %#x, format %#x, levels to %d)",
             width, height, (unsigned)target, (unsigned)internal_format,
             max_level);
  }
  if (pbuffer) {
    *pbuffer = NULL;
  }
  if (!pbuffer || width <= 0 || height <= 0 ||
      (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE_ARB)) {
    agl_error = AGL_BAD_VALUE;
    return 0;
  }
  if (!resolve_fbo()) {
    cf_warn_once("aglCreatePBuffer: OpenGL has no framebuffer objects");
    agl_error = AGL_BAD_ALLOC;
    return 0;
  }
  hle_pbuffer* pb = calloc(1, sizeof(*pb));
  pb->magic = kMagicPBuffer;
  pb->width = width;
  pb->height = height;
  pb->target = target;
  pb->internal_format = (int32_t)internal_format;
  pb->next = pbuffers;
  pbuffers = pb;
  *pbuffer = pb;
  return 1;
}

Boolean aglDestroyPBuffer(void* pbuffer) {
  hle_pbuffer* pb = pbuffer_of(pbuffer);
  if (!pb) {
    agl_error = AGL_BAD_VALUE;
    return 0;
  }
  hle_gl_context* owner = pb->owner;
  if (owner) {
    if (owner->pbuffer == pb) {
      leave_pbuffer(owner);
    }
    hle_gl_context* previous = borrow(owner);
    if (pb->framebuffer) {
      fbo.delete_framebuffers(1, &pb->framebuffer);
    }
    if (pb->depth_stencil) {
      fbo.delete_renderbuffers(1, &pb->depth_stencil);
    }
    if (pb->own_texture && pb->texture) {
      fbo.delete_textures(1, &pb->texture);
    }
    give_back(owner, previous);
  }
  for (hle_pbuffer** link = &pbuffers; *link; link = &(*link)->next) {
    if (*link == pb) {
      *link = pb->next;
      break;
    }
  }
  pb->magic = 0;
  free(pb);
  return 1;
}

Boolean aglSetPBuffer(void* ctx, void* pbuffer, int32_t face, int32_t level,
                      int32_t screen) {
  static unsigned calls;
  if (++calls == 1 || calls % kFramesPerTrace == 0) {
    cf_trace("aglSetPBuffer(%p, face %d, level %d): call %u", pbuffer, face,
             level, calls);
  }
  hle_gl_context* c = context_of(ctx);
  hle_pbuffer* pb = pbuffer_of(pbuffer);
  if (!c || !pb) {
    agl_error = c ? AGL_BAD_VALUE : AGL_BAD_CONTEXT;
    return 0;
  }
  hle_gl_context* previous = borrow(c);
  if (!pb->texture) {
    fbo.gen_textures(1, &pb->texture);
    pb->own_texture = 1;
    size_texture(pb);
  }
  int ok = make_framebuffer(pb, c);
  fbo.bind_framebuffer(GL_FRAMEBUFFER_EXT, ok ? pb->framebuffer : 0);
  c->pbuffer = ok ? pb : NULL;
  give_back(c, previous);
  if (!ok) {
    agl_error = AGL_BAD_ALLOC;
    return 0;
  }
  return 1;
}

Boolean aglTexImagePBuffer(void* ctx, void* pbuffer, int32_t source) {
  static unsigned calls;
  if (++calls == 1 || calls % kFramesPerTrace == 0) {
    cf_trace("aglTexImagePBuffer(%p, source %#x): call %u", pbuffer,
             (unsigned)source, calls);
  }
  hle_gl_context* c = context_of(ctx);
  hle_pbuffer* pb = pbuffer_of(pbuffer);
  if (!c || !pb) {
    agl_error = c ? AGL_BAD_VALUE : AGL_BAD_CONTEXT;
    return 0;
  }
  hle_gl_context* previous = borrow(c);
  int32_t bound = 0;
  fbo.get_integer(binding_of(pb->target), &bound);
  if (bound && (uint32_t)bound != pb->texture) {
    if (pb->own_texture && pb->texture) {
      fbo.delete_textures(1, &pb->texture);
    }
    pb->texture = (uint32_t)bound;
    pb->own_texture = 0;
    size_texture(pb);
    if (pb->owner == c && pb->framebuffer) {
      attach_texture(pb);
    }
  }
  give_back(c, previous);
  return 1;
}

// ---------------------------------------------------------------------------
// CGL

enum {
  kCGLNoError = 0,
  kCGLBadAttribute = 10000,
  kCGLBadProperty = 10001,
  kCGLBadPixelFormat = 10002,
  kCGLBadRendererInfo = 10003,
  kCGLBadContext = 10004,
};

int CGLChoosePixelFormat(const int32_t* attributes, void** pix,
                         int32_t* count) {
  if (!pix) {
    return kCGLBadAttribute;
  }
  *pix = parse_format(attributes, 1);
  if (count) {
    *count = 1;
  }
  return kCGLNoError;
}

int CGLDestroyPixelFormat(void* pix) {
  hle_pixel_format* f = format_of(pix);
  if (!f) {
    return kCGLBadPixelFormat;
  }
  destroy_format(f);
  return kCGLNoError;
}

int CGLDescribePixelFormat(void* pix, int32_t index, int32_t attribute,
                           int32_t* value) {
  if (!format_of(pix)) {
    return kCGLBadPixelFormat;
  }
  if (attribute == 128) {  // kCGLPFAVirtualScreenCount
    *value = 1;
    return kCGLNoError;
  }
  aglDescribePixelFormat(pix, attribute, value);
  return kCGLNoError;
}

int CGLQueryRendererInfo(uint32_t display_mask, void** rend, int32_t* count) {
  if (!rend) {
    return kCGLBadRendererInfo;
  }
  *rend = new_renderer_info();
  if (count) {
    *count = 1;
  }
  return kCGLNoError;
}

int CGLDescribeRenderer(void* rend, int32_t index, int32_t property,
                        int32_t* value) {
  hle_renderer_info* info = renderer_info_of(rend);
  if (!info || index < 0 || index >= info->count) {
    return kCGLBadRendererInfo;
  }
  return value && describe_renderer(property, value) ? kCGLNoError
                                                     : kCGLBadProperty;
}

int CGLDestroyRendererInfo(void* rend) {
  aglDestroyRendererInfo(rend);
  return kCGLNoError;
}

int CGLCreateContext(void* pix, void* share, void** ctx) {
  hle_pixel_format* f = format_of(pix);
  if (!f || !ctx) {
    return kCGLBadPixelFormat;
  }
  // The context may outlive the format it came from.
  hle_pixel_format* own = malloc(sizeof(*own));
  *own = *f;
  own->probe = NULL;
  *ctx = create_context(own, context_of(share));
  if (!*ctx) {
    free(own);
    return kCGLBadContext;
  }
  return kCGLNoError;
}

int CGLDestroyContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    return kCGLBadContext;
  }
  hle_pixel_format* f = c->format;
  destroy_context(c);
  destroy_format(f);
  return kCGLNoError;
}

int CGLSetCurrentContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (ctx && !c) {
    return kCGLBadContext;
  }
  make_current(c);
  return kCGLNoError;
}

void* CGLGetCurrentContext(void) {
  return current;
}

// ---------------------------------------------------------------------------
// GLU

// Whether |name| is one of the space-separated words of |extensions|.
Boolean gluCheckExtension(const uint8_t* name, const uint8_t* extensions) {
  if (!name || !extensions) {
    return 0;
  }
  size_t len = strlen((const char*)name);
  const char* p = (const char*)extensions;
  while ((p = strstr(p, (const char*)name))) {
    if ((p == (const char*)extensions || p[-1] == ' ') &&
        (p[len] == ' ' || p[len] == '\0')) {
      return 1;
    }
    p += len;
  }
  return 0;
}
