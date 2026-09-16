// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// A report of GL's state at the draws made with chosen vertex programs.
//
// HLE_PROBE=<text> puts a wrapper in front of the draw calls. A draw made
// while a vertex program whose source holds <text> is bound is looked at;
// HLE_PROBE=* looks at every draw, fixed function ones too. The first draw in
// each distinct state is reported to stderr: each vertex attribute array the
// draw may read, whether it is enabled, where it points and the values it
// gives the first vertices drawn; the emulated vertex array object; the
// program's environment constants; and the fragment stage: the fragment
// program, register combiners and texture shaders, each texture unit's
// texture, whether that texture is complete and how the unit combines it,
// alpha testing and blending. A state is what those are, less the texture
// names, pointers and values. Up to HLE_PROBE_COUNT states are reported (64
// by default).
//
// Each report also gives the texture stage states of the game's Direct3D
// layer, the device's own record of what it was asked for, for the first
// stages: the Direct3D operations with no OpenGL counterpart there leave
// OpenGL's combiners as they were. The device is found once, in the
// process's writable memory, by the two AGL contexts it holds.
//
// HLE_PROBE_TEXTURES=<directory> also saves the first level of each texture a
// reported draw has enabled, once per texture, as a PAM file (RGBA) named for
// the texture, and asks for a screenshot of the frame (see screenshot.c).
//
// The game makes its GL calls on one thread.

#define _GNU_SOURCE

#include "gl_probe.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "gl_var.h"
#include "gui.h"

enum {
  GL_BYTE = 0x1400,
  GL_UNSIGNED_BYTE = 0x1401,
  GL_SHORT = 0x1402,
  GL_UNSIGNED_SHORT = 0x1403,
  GL_UNSIGNED_INT = 0x1405,
  GL_FLOAT = 0x1406,
  GL_CULL_FACE = 0x0B44,
  GL_DEPTH_TEST = 0x0B71,
  GL_DEPTH_WRITEMASK = 0x0B72,
  GL_ALPHA_TEST = 0x0BC0,
  GL_ALPHA_TEST_FUNC = 0x0BC1,
  GL_ALPHA_TEST_REF = 0x0BC2,
  GL_VIEWPORT = 0x0BA2,
  GL_BLEND_DST = 0x0BE0,
  GL_BLEND_SRC = 0x0BE1,
  GL_BLEND = 0x0BE2,
  GL_PACK_ALIGNMENT = 0x0D05,
  GL_TEXTURE_2D = 0x0DE1,
  GL_TEXTURE_WIDTH = 0x1000,
  GL_TEXTURE_HEIGHT = 0x1001,
  GL_TEXTURE_INTERNAL_FORMAT = 0x1003,
  GL_RGBA = 0x1908,
  GL_TEXTURE_ENV_MODE = 0x2200,
  GL_TEXTURE_ENV_COLOR = 0x2201,
  GL_TEXTURE_ENV = 0x2300,
  GL_NEAREST_MIPMAP_NEAREST = 0x2700,
  GL_LINEAR_MIPMAP_LINEAR = 0x2703,
  GL_TEXTURE_MIN_FILTER = 0x2801,
  GL_BLEND_EQUATION = 0x8009,
  GL_TEXTURE_BINDING_2D = 0x8069,
  GL_VERTEX_ARRAY = 0x8074,
  GL_NORMAL_ARRAY = 0x8075,
  GL_COLOR_ARRAY = 0x8076,
  GL_TEXTURE_COORD_ARRAY = 0x8078,
  GL_TEXTURE_BASE_LEVEL = 0x813C,
  GL_TEXTURE_MAX_LEVEL = 0x813D,
  GL_TEXTURE0 = 0x84C0,
  GL_ACTIVE_TEXTURE = 0x84E0,
  GL_CLIENT_ACTIVE_TEXTURE = 0x84E1,
  GL_TEXTURE_RECTANGLE = 0x84F5,
  GL_TEXTURE_CUBE_MAP = 0x8513,
  GL_REGISTER_COMBINERS_NV = 0x8522,
  GL_COMBINE_RGB = 0x8571,
  GL_COMBINE_ALPHA = 0x8572,
  GL_SOURCE0_RGB = 0x8580,
  GL_SOURCE0_ALPHA = 0x8588,
  GL_OPERAND0_RGB = 0x8590,
  GL_OPERAND0_ALPHA = 0x8598,
  GL_VERTEX_PROGRAM_ARB = 0x8620,
  GL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622,
  GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623,
  GL_VERTEX_ATTRIB_ARRAY_STRIDE = 0x8624,
  GL_VERTEX_ATTRIB_ARRAY_TYPE = 0x8625,
  GL_CURRENT_VERTEX_ATTRIB = 0x8626,
  GL_VERTEX_ATTRIB_ARRAY_POINTER = 0x8645,
  GL_PROGRAM_BINDING_ARB = 0x8677,
  GL_TEXTURE_SHADER_NV = 0x86DE,
  GL_SHADER_OPERATION_NV = 0x86DF,
  GL_BUFFER_SIZE = 0x8764,
  GL_FRAGMENT_PROGRAM_ARB = 0x8804,
  GL_VERTEX_ATTRIB_ARRAY_NORMALIZED = 0x886A,
  GL_ARRAY_BUFFER = 0x8892,
  GL_ARRAY_BUFFER_BINDING = 0x8894,
  GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895,
  GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING = 0x889F,
  kAttributes = 4,
  kUnits = 4,
  kLevels = 13,
  kVerticesShown = 4,
  kMaxStates = 1024,
  kMaxSavedTextures = 512,
  // In the game's IDirect3DDevice9 (0x2badb4 is its SetTextureStageState).
  kDeviceSecondContext = 0x1f14,
  kDeviceContext = 0x1f18,
  kDeviceStageCount = 0x16a4,
  kDeviceStageStates = 0xfe4,  // + (stage * 34 + state) * 4
  kDeviceColorSwapped = 0x1474,  // + stage: SELECTARG2 moved to source 0
  kDeviceAlphaSwapped = 0x147c,
  kStagesShown = 2,
  kScanChunk = 1 << 20,
};

typedef struct {
  uint32_t target;
  int32_t id;
  int matches;  // a vertex program whose source holds the probe's text
  int shown;    // its source has been reported
  char* text;
} probe_program;

typedef struct {
  int32_t enabled;  // kTexture2D, kCube, kRectangle
  int32_t format;
  int32_t mode;
  int32_t combine[2];  // rgb, alpha
  int32_t source[6];   // rgb 0-2, alpha 0-2
  int32_t operand[6];
  int32_t shader_operation;
} probe_unit;

// What makes one draw's state differ from another's, for reporting each once.
// Filled whole, padding included, so it can be compared as bytes.
typedef struct {
  int32_t vertex_program;    // -1 when vertex programs are off
  int32_t fragment_program;  // -1 when fragment programs are off
  int32_t flags;             // kCombiners, kTextureShaders, ...
  int32_t blend_source;
  int32_t blend_destination;
  int32_t blend_equation;
  int32_t alpha_function;
  probe_unit units[kUnits];
  struct {
    int32_t enabled;
    int32_t size;
    int32_t type;
    int32_t normalized;
  } attributes[kAttributes];
  int32_t client_arrays;  // kVertexArray, ... for fixed function draws
} probe_state;

enum {
  kCombiners = 1,
  kTextureShaders = 2,
  kBlend = 4,
  kAlphaTest = 8,
  kDepthTest = 16,
  kDepthWrite = 32,
  kCull = 64,
  kTexture2D = 1,
  kCube = 2,
  kRectangle = 4,
  kVertexArray = 1,
  kNormalArray = 2,
  kColorArray = 4,
  kTexCoordArray = 8,
};

static const char* probe_text;
static int probe_everything;
static int looked;
static int limit = 64;
static unsigned long matching_draws;
static probe_program* programs;
static size_t program_count;
static size_t program_capacity;
static probe_state* states;
static int state_count;
static const char* texture_directory;
static int32_t saved_textures[kMaxSavedTextures];
static int saved_texture_count;

static struct {
  void (*get_integer_v)(uint32_t, int32_t*);
  void (*get_float_v)(uint32_t, float*);
  uint8_t (*is_enabled)(uint32_t);
  void (*get_program_iv)(uint32_t, uint32_t, int32_t*);
  void (*get_program_env_fv)(uint32_t, uint32_t, float*);
  void (*active_texture)(uint32_t);
  void (*get_tex_env_iv)(uint32_t, uint32_t, int32_t*);
  void (*get_tex_env_fv)(uint32_t, uint32_t, float*);
  void (*get_tex_parameter_iv)(uint32_t, uint32_t, int32_t*);
  void (*get_tex_level_parameter_iv)(uint32_t, int32_t, uint32_t, int32_t*);
  void (*get_tex_image)(uint32_t, int32_t, uint32_t, uint32_t, void*);
  void (*pixel_store_i)(uint32_t, int32_t);
  void (*get_vertex_attrib_iv)(uint32_t, uint32_t, int32_t*);
  void (*get_vertex_attrib_fv)(uint32_t, uint32_t, float*);
  void (*get_vertex_attrib_pointer_v)(uint32_t, uint32_t, void**);
  void (*bind_buffer)(uint32_t, uint32_t);
  void (*get_buffer_parameter_iv)(uint32_t, uint32_t, int32_t*);
  void (*get_buffer_sub_data)(uint32_t, intptr_t, intptr_t, void*);
} gl;

static void (*real_draw_arrays)(uint32_t, int32_t, int32_t);
static void (*real_draw_elements)(uint32_t, int32_t, uint32_t, const void*);
static void (*real_draw_range_elements)(uint32_t, uint32_t, uint32_t, int32_t,
                                        uint32_t, const void*);

static const char* text_to_probe(void) {
  if (looked) {
    return probe_text;
  }
  looked = 1;
  const char* text = getenv("HLE_PROBE");
  if (!text || !*text) {
    return NULL;
  }
  const char* count = getenv("HLE_PROBE_COUNT");
  if (count && atoi(count) > 0) {
    limit = atoi(count) < kMaxStates ? atoi(count) : kMaxStates;
  }
  texture_directory = getenv("HLE_PROBE_TEXTURES");
  if (texture_directory && !*texture_directory) {
    texture_directory = NULL;
  }
  gl.get_integer_v = dlsym(RTLD_DEFAULT, "glGetIntegerv");
  gl.get_float_v = dlsym(RTLD_DEFAULT, "glGetFloatv");
  gl.is_enabled = dlsym(RTLD_DEFAULT, "glIsEnabled");
  gl.get_program_iv = dlsym(RTLD_DEFAULT, "glGetProgramivARB");
  gl.get_program_env_fv =
      dlsym(RTLD_DEFAULT, "glGetProgramEnvParameterfvARB");
  gl.active_texture = dlsym(RTLD_DEFAULT, "glActiveTextureARB");
  gl.get_tex_env_iv = dlsym(RTLD_DEFAULT, "glGetTexEnviv");
  gl.get_tex_env_fv = dlsym(RTLD_DEFAULT, "glGetTexEnvfv");
  gl.get_tex_parameter_iv = dlsym(RTLD_DEFAULT, "glGetTexParameteriv");
  gl.get_tex_level_parameter_iv =
      dlsym(RTLD_DEFAULT, "glGetTexLevelParameteriv");
  gl.get_tex_image = dlsym(RTLD_DEFAULT, "glGetTexImage");
  gl.pixel_store_i = dlsym(RTLD_DEFAULT, "glPixelStorei");
  gl.get_vertex_attrib_iv = dlsym(RTLD_DEFAULT, "glGetVertexAttribivARB");
  gl.get_vertex_attrib_fv = dlsym(RTLD_DEFAULT, "glGetVertexAttribfvARB");
  gl.get_vertex_attrib_pointer_v =
      dlsym(RTLD_DEFAULT, "glGetVertexAttribPointervARB");
  gl.bind_buffer = dlsym(RTLD_DEFAULT, "glBindBufferARB");
  gl.get_buffer_parameter_iv =
      dlsym(RTLD_DEFAULT, "glGetBufferParameterivARB");
  gl.get_buffer_sub_data = dlsym(RTLD_DEFAULT, "glGetBufferSubDataARB");
  states = calloc(kMaxStates, sizeof(*states));
  if (!gl.get_integer_v || !gl.get_float_v || !gl.is_enabled ||
      !gl.get_program_iv || !gl.get_program_env_fv || !gl.active_texture ||
      !gl.get_tex_env_iv || !gl.get_tex_env_fv || !gl.get_tex_parameter_iv ||
      !gl.get_tex_level_parameter_iv || !gl.get_tex_image ||
      !gl.pixel_store_i || !gl.get_vertex_attrib_iv ||
      !gl.get_vertex_attrib_fv || !gl.get_vertex_attrib_pointer_v ||
      !gl.bind_buffer || !gl.get_buffer_parameter_iv ||
      !gl.get_buffer_sub_data || !states) {
    fprintf(stderr, "hle: HLE_PROBE: the GL library lacks a function the "
            "probe reads state with\n");
    return NULL;
  }
  probe_everything = !strcmp(text, "*");
  probe_text = text;
  return probe_text;
}

static probe_program* program_of(uint32_t target, int32_t id) {
  for (size_t i = 0; i < program_count; i++) {
    if (programs[i].target == target && programs[i].id == id) {
      return &programs[i];
    }
  }
  return NULL;
}

void hle_gl_probe_program(uint32_t target, const char* text, int32_t length) {
  if (!text_to_probe() || !text || length <= 0) {
    return;
  }
  int32_t id = 0;
  gl.get_program_iv(target, GL_PROGRAM_BINDING_ARB, &id);
  probe_program* p = program_of(target, id);
  if (!p) {
    if (program_count == program_capacity) {
      size_t n = program_capacity ? program_capacity * 2 : 64;
      probe_program* grown = realloc(programs, n * sizeof(*programs));
      if (!grown) {
        return;
      }
      programs = grown;
      program_capacity = n;
    }
    p = &programs[program_count++];
    p->target = target;
    p->id = id;
    p->text = NULL;
  }
  free(p->text);
  p->text = strndup(text, length);
  p->shown = 0;
  p->matches = target == GL_VERTEX_PROGRAM_ARB &&
               (probe_everything ||
                memmem(text, length, probe_text, strlen(probe_text)) != NULL);
}

// Copies |size| bytes at |address| without faulting where nothing is mapped.
static int read_memory(uintptr_t address, void* out, size_t size) {
  struct iovec local = { out, size };
  struct iovec remote = { (void*)address, size };
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
         (ssize_t)size;
}

static void show_source(probe_program* p, const char* what) {
  if (!p || p->shown || !p->text) {
    return;
  }
  p->shown = 1;
  fprintf(stderr, "hle probe:   %s %d source:\n", what, p->id);
  const char* line = p->text;
  while (*line) {
    const char* end = strchr(line, '\n');
    int n = end ? (int)(end - line) : (int)strlen(line);
    fprintf(stderr, "hle probe:     | %.*s\n", n, line);
    if (!end) {
      break;
    }
    line = end + 1;
  }
}

static size_t type_size(int32_t type) {
  switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
      return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
      return 2;
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
      return 4;
    default:
      return 0;
  }
}

// Appends one vertex's components, read from |raw|, to |out|.
static void append_values(char* out, size_t size, const unsigned char* raw,
                          int32_t count, int32_t type) {
  size_t n = strlen(out);
  n += snprintf(out + n, n < size ? size - n : 0, " (");
  for (int32_t i = 0; i < count && n < size; i++) {
    const unsigned char* at = raw + (size_t)i * type_size(type);
    const char* space = i ? " " : "";
    if (type == GL_FLOAT) {
      float v;
      memcpy(&v, at, 4);
      n += snprintf(out + n, size - n, "%s%g", space, v);
    } else if (type == GL_UNSIGNED_BYTE) {
      n += snprintf(out + n, size - n, "%s%u", space, at[0]);
    } else if (type == GL_SHORT) {
      int16_t v;
      memcpy(&v, at, 2);
      n += snprintf(out + n, size - n, "%s%d", space, v);
    } else {
      n += snprintf(out + n, size - n, "%s?", space);
    }
  }
  if (n < size) {
    snprintf(out + n, size - n, ")");
  }
}

// Where the first vertices a vertex program draws land in the window, with
// program.env[0..3] as the rows of the matrix it projects attribute 0 with,
// as the game's programs do.
static void report_screen_positions(long vertex, int32_t vertices) {
  int32_t on = 0;
  int32_t size = 0;
  int32_t stride = 0;
  int32_t type = 0;
  int32_t buffer = 0;
  void* pointer = NULL;
  gl.get_vertex_attrib_iv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
  gl.get_vertex_attrib_iv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
  gl.get_vertex_attrib_iv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
  gl.get_vertex_attrib_iv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
  gl.get_vertex_attrib_iv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
  gl.get_vertex_attrib_pointer_v(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
  if (!on || buffer || type != GL_FLOAT || size < 3 || size > 4 ||
      vertex < 0) {
    return;
  }
  float rows[4][4];
  for (uint32_t i = 0; i < 4; i++) {
    gl.get_program_env_fv(GL_VERTEX_PROGRAM_ARB, i, rows[i]);
  }
  int32_t viewport[4] = { 0 };
  gl.get_integer_v(GL_VIEWPORT, viewport);
  char line[400] = "";
  size_t n = 0;
  size_t step = stride ? (size_t)stride : (size_t)size * sizeof(float);
  int32_t shown = vertices < kVerticesShown ? vertices : kVerticesShown;
  for (int32_t k = 0; k < shown && n < sizeof(line); k++) {
    float v[4] = { 0, 0, 0, 1 };
    if (!read_memory((uintptr_t)pointer + step * (size_t)(vertex + k), v,
                     (size_t)size * sizeof(float))) {
      break;
    }
    float clip[4];
    for (int i = 0; i < 4; i++) {
      clip[i] = rows[i][0] * v[0] + rows[i][1] * v[1] + rows[i][2] * v[2] +
                rows[i][3] * v[3];
    }
    if (clip[3] <= 0) {
      n += (size_t)snprintf(line + n, sizeof(line) - n, " (behind)");
      continue;
    }
    // From the top left, as a screenshot shows it.
    float x = viewport[0] + (clip[0] / clip[3] + 1) / 2 * viewport[2];
    float y = viewport[3] - (viewport[1] + (clip[1] / clip[3] + 1) / 2 *
                                               viewport[3]);
    n += (size_t)snprintf(line + n, sizeof(line) - n, " (%.0f %.0f)", x, y);
  }
  fprintf(stderr, "hle probe:   in a %dx%d viewport the vertices land at%s\n",
          viewport[2], viewport[3], *line ? line : " (not read)");
}

static void report_attribute(uint32_t index, long vertex, int32_t vertices) {
  int32_t on = 0;
  int32_t size = 0;
  int32_t stride = 0;
  int32_t type = 0;
  int32_t normalized = 0;
  int32_t buffer = 0;
  void* pointer = NULL;
  gl.get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &on);
  gl.get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
  gl.get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
  gl.get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
  gl.get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                          &normalized);
  gl.get_vertex_attrib_iv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                          &buffer);
  gl.get_vertex_attrib_pointer_v(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                 &pointer);
  char values[400] = "";
  size_t component = type_size(type);
  if (on && component && size >= 1 && size <= 4 && vertex >= 0) {
    size_t bytes = (size_t)size * component;
    size_t step = stride ? (size_t)stride : bytes;
    int32_t shown = vertices < kVerticesShown ? vertices : kVerticesShown;
    for (int32_t k = 0; k < shown; k++) {
      uintptr_t at = (uintptr_t)pointer + step * (size_t)(vertex + k);
      unsigned char raw[16] = { 0 };
      if (buffer) {
        int32_t bound = 0;
        int32_t buffer_size = 0;
        gl.get_integer_v(GL_ARRAY_BUFFER_BINDING, &bound);
        gl.bind_buffer(GL_ARRAY_BUFFER, buffer);
        gl.get_buffer_parameter_iv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE,
                                   &buffer_size);
        int inside = at + bytes <= (size_t)buffer_size;
        if (inside) {
          gl.get_buffer_sub_data(GL_ARRAY_BUFFER, at, bytes, raw);
        }
        gl.bind_buffer(GL_ARRAY_BUFFER, bound);
        if (!inside) {
          size_t n = strlen(values);
          snprintf(values + n, sizeof(values) - n,
                   " past the buffer's %d bytes", buffer_size);
          break;
        }
      } else if (!read_memory(at, raw, bytes)) {
        size_t n = strlen(values);
        snprintf(values + n, sizeof(values) - n, " unmapped at %#lx",
                 (unsigned long)at);
        break;
      }
      append_values(values, sizeof(values), raw, size, type);
    }
  }
  float current[4] = { 0, 0, 0, 0 };
  if (index != 0) {
    // Attribute 0's current value is not there to read.
    gl.get_vertex_attrib_fv(index, GL_CURRENT_VERTEX_ATTRIB, current);
  }
  fprintf(stderr,
          "hle probe:   attrib %u: %s, size %d type %#x%s stride %d, buffer "
          "%d pointer %p; from vertex %ld:%s; current (%g %g %g %g)\n",
          index, on ? "ENABLED" : "disabled", size, (unsigned)type,
          normalized ? " normalized" : "", stride, buffer, pointer, vertex,
          *values ? values : " not read", current[0], current[1], current[2],
          current[3]);
}

static const char* texture_completeness(int32_t min_filter, int32_t base,
                                        int32_t max, const int32_t* widths,
                                        const int32_t* heights) {
  if (base < 0 || base >= kLevels || widths[base] <= 0) {
    return "NO IMAGE";
  }
  if (min_filter < GL_NEAREST_MIPMAP_NEAREST ||
      min_filter > GL_LINEAR_MIPMAP_LINEAR) {
    return "complete";
  }
  int32_t w = widths[base];
  int32_t h = heights[base];
  for (int32_t level = base; level <= max && level < kLevels; level++) {
    if (widths[level] != w || heights[level] != h) {
      return "INCOMPLETE MIPMAPS";
    }
    if (w == 1 && h == 1) {
      return "complete";
    }
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  return max < kLevels - 1 ? "complete" : "INCOMPLETE MIPMAPS";
}

// Saves the bound 2D texture's first level as |texture|.pam, once.
static void save_texture(int32_t texture, int32_t width, int32_t height) {
  if (!texture_directory || texture <= 0 || width <= 0 || height <= 0 ||
      width > 4096 || height > 4096) {
    return;
  }
  for (int i = 0; i < saved_texture_count; i++) {
    if (saved_textures[i] == texture) {
      return;
    }
  }
  if (saved_texture_count == kMaxSavedTextures) {
    return;
  }
  saved_textures[saved_texture_count++] = texture;
  unsigned char* pixels = malloc((size_t)width * height * 4);
  if (!pixels) {
    return;
  }
  int32_t alignment = 4;
  gl.get_integer_v(GL_PACK_ALIGNMENT, &alignment);
  gl.pixel_store_i(GL_PACK_ALIGNMENT, 1);
  gl.get_tex_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  gl.pixel_store_i(GL_PACK_ALIGNMENT, alignment);
  mkdir(texture_directory, 0755);
  char path[4200];
  snprintf(path, sizeof(path), "%s/texture-%d.pam", texture_directory,
           texture);
  FILE* f = fopen(path, "wb");
  if (f) {
    // GL's rows run bottom to top.
    fprintf(f,
            "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE "
            "RGB_ALPHA\nENDHDR\n",
            width, height);
    for (int32_t y = height - 1; y >= 0; y--) {
      fwrite(pixels + (size_t)y * width * 4, 4, width, f);
    }
    fclose(f);
  } else {
    fprintf(stderr, "hle probe: cannot write %s: %m\n", path);
  }
  free(pixels);
}

static void read_unit(int unit, probe_state* s) {
  gl.active_texture(GL_TEXTURE0 + unit);
  s->units[unit].enabled = (gl.is_enabled(GL_TEXTURE_2D) ? kTexture2D : 0) |
                           (gl.is_enabled(GL_TEXTURE_CUBE_MAP) ? kCube : 0) |
                           (gl.is_enabled(GL_TEXTURE_RECTANGLE) ? kRectangle
                                                                : 0);
  gl.get_tex_level_parameter_iv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                                &s->units[unit].format);
  gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
                    &s->units[unit].mode);
  gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_COMBINE_RGB,
                    &s->units[unit].combine[0]);
  gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,
                    &s->units[unit].combine[1]);
  for (int i = 0; i < 3; i++) {
    gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_SOURCE0_RGB + i,
                      &s->units[unit].source[i]);
    gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + i,
                      &s->units[unit].source[3 + i]);
    gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_OPERAND0_RGB + i,
                      &s->units[unit].operand[i]);
    gl.get_tex_env_iv(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + i,
                      &s->units[unit].operand[3 + i]);
  }
  gl.get_tex_env_iv(GL_TEXTURE_SHADER_NV, GL_SHADER_OPERATION_NV,
                    &s->units[unit].shader_operation);
}

static void read_state(probe_state* s) {
  memset(s, 0, sizeof(*s));
  s->vertex_program = -1;
  if (gl.is_enabled(GL_VERTEX_PROGRAM_ARB)) {
    gl.get_program_iv(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB,
                      &s->vertex_program);
  }
  s->fragment_program = -1;
  if (gl.is_enabled(GL_FRAGMENT_PROGRAM_ARB)) {
    gl.get_program_iv(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB,
                      &s->fragment_program);
  }
  int32_t depth_write = 0;
  gl.get_integer_v(GL_DEPTH_WRITEMASK, &depth_write);
  s->flags = (gl.is_enabled(GL_REGISTER_COMBINERS_NV) ? kCombiners : 0) |
             (gl.is_enabled(GL_TEXTURE_SHADER_NV) ? kTextureShaders : 0) |
             (gl.is_enabled(GL_BLEND) ? kBlend : 0) |
             (gl.is_enabled(GL_ALPHA_TEST) ? kAlphaTest : 0) |
             (gl.is_enabled(GL_DEPTH_TEST) ? kDepthTest : 0) |
             (depth_write ? kDepthWrite : 0) |
             (gl.is_enabled(GL_CULL_FACE) ? kCull : 0);
  gl.get_integer_v(GL_BLEND_SRC, &s->blend_source);
  gl.get_integer_v(GL_BLEND_DST, &s->blend_destination);
  gl.get_integer_v(GL_BLEND_EQUATION, &s->blend_equation);
  gl.get_integer_v(GL_ALPHA_TEST_FUNC, &s->alpha_function);
  int32_t active = GL_TEXTURE0;
  gl.get_integer_v(GL_ACTIVE_TEXTURE, &active);
  for (int unit = 0; unit < kUnits; unit++) {
    read_unit(unit, s);
  }
  gl.active_texture(active);
  if (s->vertex_program >= 0) {
    for (uint32_t i = 0; i < kAttributes; i++) {
      gl.get_vertex_attrib_iv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                              &s->attributes[i].enabled);
      gl.get_vertex_attrib_iv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                              &s->attributes[i].size);
      gl.get_vertex_attrib_iv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                              &s->attributes[i].type);
      gl.get_vertex_attrib_iv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                              &s->attributes[i].normalized);
    }
  } else {
    s->client_arrays =
        (gl.is_enabled(GL_VERTEX_ARRAY) ? kVertexArray : 0) |
        (gl.is_enabled(GL_NORMAL_ARRAY) ? kNormalArray : 0) |
        (gl.is_enabled(GL_COLOR_ARRAY) ? kColorArray : 0) |
        (gl.is_enabled(GL_TEXTURE_COORD_ARRAY) ? kTexCoordArray : 0);
  }
}

// The game's Direct3D device, or 0.
static uintptr_t find_device(void) {
  static uintptr_t device;
  static int searched;
  if (searched) {
    return device;
  }
  searched = 1;
  uintptr_t context = (uintptr_t)hle_gl_current_context();
  FILE* maps = context ? fopen("/proc/self/maps", "r") : NULL;
  uint32_t* words = maps ? malloc(kScanChunk) : NULL;
  char line[512];
  while (words && !device && fgets(line, sizeof(line), maps)) {
    unsigned long lo;
    unsigned long hi;
    char perms[8];
    int path = 0;
    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &path) <
            3 ||
        perms[0] != 'r' || perms[1] != 'w' ||
        (line[path] != '\0' && strncmp(line + path, "[heap]", 6) != 0)) {
      continue;
    }
    for (unsigned long at = lo; at < hi && !device; at += kScanChunk) {
      size_t size = hi - at < kScanChunk ? hi - at : kScanChunk;
      if (!read_memory(at, words, size)) {
        continue;
      }
      for (size_t i = 1; i < size / 4; i++) {
        uintptr_t candidate = at + i * 4 - kDeviceContext;
        uint32_t stages = 0;
        if (words[i] == context && candidate >= lo &&
            hle_gl_is_context((void*)(uintptr_t)words[i - 1]) &&
            read_memory(candidate + kDeviceStageCount, &stages,
                        sizeof(stages)) &&
            stages >= 1 && stages <= 8) {
          device = candidate;
          break;
        }
      }
    }
  }
  free(words);
  if (maps) {
    fclose(maps);
  }
  if (device) {
    fprintf(stderr, "hle probe: the Direct3D device is at %#lx\n",
            (unsigned long)device);
  } else {
    fprintf(stderr, "hle probe: the Direct3D device was not found\n");
  }
  return device;
}

static const char* texture_operation_name(uint32_t op) {
  static const char* const kNames[] = {
    "0", "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X",
    "MODULATE4X", "ADD", "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH",
    "BLENDDIFFUSEALPHA", "BLENDTEXTUREALPHA", "BLENDFACTORALPHA",
    "BLENDTEXTUREALPHAPM", "BLENDCURRENTALPHA", "PREMODULATE",
    "MODULATEALPHA_ADDCOLOR", "MODULATECOLOR_ADDALPHA",
    "MODULATEINVALPHA_ADDCOLOR", "MODULATEINVCOLOR_ADDALPHA", "BUMPENVMAP",
    "BUMPENVMAPLUMINANCE", "DOTPRODUCT3", "MULTIPLYADD", "LERP",
  };
  return op < sizeof(kNames) / sizeof(kNames[0]) ? kNames[op] : "?";
}

static void report_device_stages(void) {
  uintptr_t device = find_device();
  if (!device) {
    return;
  }
  for (uint32_t stage = 0; stage < kStagesShown; stage++) {
    uint32_t states[7] = { 0 };  // [1..6]: COLOROP to ALPHAARG2
    uint8_t swapped[2] = { 0 };
    if (!read_memory(device + kDeviceStageStates + stage * 34 * 4, states,
                     sizeof(states)) ||
        !read_memory(device + kDeviceColorSwapped + stage, &swapped[0], 1) ||
        !read_memory(device + kDeviceAlphaSwapped + stage, &swapped[1], 1)) {
      return;
    }
    fprintf(stderr,
            "hle probe:   Direct3D stage %u: color %s %#x(%#x, %#x)%s, alpha "
            "%s %#x(%#x, %#x)%s\n",
            stage, texture_operation_name(states[1]), states[1], states[2],
            states[3], swapped[0] ? " swapped" : "",
            texture_operation_name(states[4]), states[4], states[5],
            states[6], swapped[1] ? " swapped" : "");
  }
}

static void report_units(const probe_state* s) {
  int32_t active = GL_TEXTURE0;
  gl.get_integer_v(GL_ACTIVE_TEXTURE, &active);
  for (int unit = 0; unit < kUnits; unit++) {
    gl.active_texture(GL_TEXTURE0 + unit);
    int32_t texture = 0;
    int32_t min_filter = 0;
    int32_t base = 0;
    int32_t max = 0;
    int32_t widths[kLevels] = { 0 };
    int32_t heights[kLevels] = { 0 };
    float color[4] = { 0 };
    gl.get_integer_v(GL_TEXTURE_BINDING_2D, &texture);
    gl.get_tex_env_fv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
    gl.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            &min_filter);
    gl.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
    gl.get_tex_parameter_iv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &max);
    char levels[kLevels * 12] = "";
    size_t n = 0;
    for (int level = 0; level < kLevels; level++) {
      gl.get_tex_level_parameter_iv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH,
                                    &widths[level]);
      gl.get_tex_level_parameter_iv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT,
                                    &heights[level]);
      if (widths[level] <= 0) {
        break;
      }
      int k = snprintf(levels + n, sizeof(levels) - n, "%s%dx%d",
                       n ? " " : "", widths[level], heights[level]);
      if (k < 0 || (size_t)k >= sizeof(levels) - n) {
        break;
      }
      n += (size_t)k;
    }
    const probe_unit* u = &s->units[unit];
    fprintf(stderr,
            "hle probe:   unit %d: %s%s%s%s texture %d format %#x levels [%s] "
            "min filter %#x base %d max %d: %s; env mode %#x, combine rgb "
            "%#x (%#x %#x, %#x %#x, %#x %#x) alpha %#x (%#x %#x, %#x %#x, "
            "%#x %#x), color (%g %g %g %g); texture shader operation %#x\n",
            unit, u->enabled ? "" : "off,",
            u->enabled & kTexture2D ? " 2D" : "",
            u->enabled & kCube ? " CUBE" : "",
            u->enabled & kRectangle ? " RECTANGLE" : "", texture,
            (unsigned)u->format, levels, (unsigned)min_filter, base, max,
            texture_completeness(min_filter, base, max, widths, heights),
            (unsigned)u->mode, (unsigned)u->combine[0],
            (unsigned)u->source[0], (unsigned)u->operand[0],
            (unsigned)u->source[1], (unsigned)u->operand[1],
            (unsigned)u->source[2], (unsigned)u->operand[2],
            (unsigned)u->combine[1], (unsigned)u->source[3],
            (unsigned)u->operand[3], (unsigned)u->source[4],
            (unsigned)u->operand[4], (unsigned)u->source[5],
            (unsigned)u->operand[5], color[0], color[1], color[2], color[3],
            (unsigned)u->shader_operation);
    if (u->enabled & kTexture2D) {
      save_texture(texture, widths[0], heights[0]);
    }
  }
  gl.active_texture(active);
}

static void report(const char* call, uint32_t mode, long vertex,
                   int32_t count, const probe_state* s) {
  fprintf(stderr, "hle probe: state %d, at draw %lu: %s mode %#x, %d "
          "vertices or indices, the first vertex %ld; vertex program %d\n",
          state_count, matching_draws, call, (unsigned)mode, count, vertex,
          s->vertex_program);
  if (s->vertex_program >= 0) {
    show_source(program_of(GL_VERTEX_PROGRAM_ARB, s->vertex_program),
                "vertex program");
    for (uint32_t i = 0; i < kAttributes; i++) {
      report_attribute(i, vertex, count);
    }
    report_screen_positions(vertex, count);
    char described[1024];
    hle_gl_var_describe(described, sizeof(described));
    fprintf(stderr, "hle probe:   %s\n", described);
  } else {
    int32_t client = GL_TEXTURE0;
    gl.get_integer_v(GL_CLIENT_ACTIVE_TEXTURE, &client);
    fprintf(stderr,
            "hle probe:   fixed function: vertex array %s, normal %s, color "
            "%s, texcoord %s (client unit %d)\n",
            s->client_arrays & kVertexArray ? "on" : "off",
            s->client_arrays & kNormalArray ? "on" : "off",
            s->client_arrays & kColorArray ? "on" : "off",
            s->client_arrays & kTexCoordArray ? "on" : "off",
            client - GL_TEXTURE0);
  }
  float c10[4] = { 0 };
  float c95[4] = { 0 };
  float alpha_reference = 0;
  gl.get_program_env_fv(GL_VERTEX_PROGRAM_ARB, 10, c10);
  gl.get_program_env_fv(GL_VERTEX_PROGRAM_ARB, 95, c95);
  gl.get_float_v(GL_ALPHA_TEST_REF, &alpha_reference);
  fprintf(stderr,
          "hle probe:   env c[10] (%g %g %g %g) c[95] (%g %g %g %g); "
          "fragment program %d, register combiners %s, texture shaders "
          "%s; blend %s %#x %#x equation %#x, alpha test %s %#x %g, depth "
          "test %s write %s, cull %s\n",
          c10[0], c10[1], c10[2], c10[3], c95[0], c95[1], c95[2], c95[3],
          s->fragment_program,
          s->flags & kCombiners ? "ENABLED" : "disabled",
          s->flags & kTextureShaders ? "ENABLED" : "disabled",
          s->flags & kBlend ? "on" : "off", (unsigned)s->blend_source,
          (unsigned)s->blend_destination, (unsigned)s->blend_equation,
          s->flags & kAlphaTest ? "on" : "off", (unsigned)s->alpha_function,
          alpha_reference, s->flags & kDepthTest ? "on" : "off",
          s->flags & kDepthWrite ? "on" : "off",
          s->flags & kCull ? "on" : "off");
  if (s->fragment_program >= 0) {
    show_source(program_of(GL_FRAGMENT_PROGRAM_ARB, s->fragment_program),
                "fragment program");
  }
  report_units(s);
  report_device_stages();
  if (texture_directory) {
    hle_screenshot_request();
  }
}

// Whether the draw about to be made is in a state not reported yet; if so,
// the state is remembered and |s| holds it.
static int state_to_report(probe_state* s) {
  if (state_count >= limit) {
    return 0;
  }
  int vertex_program = gl.is_enabled(GL_VERTEX_PROGRAM_ARB);
  if (!probe_everything) {
    if (!vertex_program) {
      return 0;
    }
    int32_t id = 0;
    gl.get_program_iv(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &id);
    const probe_program* p = program_of(GL_VERTEX_PROGRAM_ARB, id);
    if (!p || !p->matches) {
      return 0;
    }
  }
  matching_draws++;
  read_state(s);
  for (int i = 0; i < state_count; i++) {
    if (!memcmp(&states[i], s, sizeof(*s))) {
      return 0;
    }
  }
  states[state_count++] = *s;
  return 1;
}

static long first_index(uint32_t type, const void* indices) {
  int32_t buffer = 0;
  gl.get_integer_v(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
  unsigned char raw[4] = { 0 };
  size_t size = type == GL_UNSIGNED_INT     ? 4
                : type == GL_UNSIGNED_SHORT ? 2
                : type == GL_UNSIGNED_BYTE  ? 1
                                            : 0;
  if (buffer || !indices || !size ||
      !read_memory((uintptr_t)indices, raw, size)) {
    return -1;
  }
  if (size == 4) {
    uint32_t v;
    memcpy(&v, raw, 4);
    return v;
  }
  if (size == 2) {
    uint16_t v;
    memcpy(&v, raw, 2);
    return v;
  }
  return raw[0];
}

static void draw_arrays(uint32_t mode, int32_t first, int32_t count) {
  probe_state s;
  if (state_to_report(&s)) {
    report("glDrawArrays", mode, first, count, &s);
  }
  real_draw_arrays(mode, first, count);
}

// For indexed draws only the first index's vertex is shown.
static void draw_elements(uint32_t mode, int32_t count, uint32_t type,
                          const void* indices) {
  probe_state s;
  if (state_to_report(&s)) {
    report("glDrawElements", mode, first_index(type, indices), 1, &s);
  }
  real_draw_elements(mode, count, type, indices);
}

static void draw_range_elements(uint32_t mode, uint32_t start, uint32_t end,
                                int32_t count, uint32_t type,
                                const void* indices) {
  probe_state s;
  if (state_to_report(&s)) {
    report("glDrawRangeElements", mode, first_index(type, indices), 1, &s);
  }
  real_draw_range_elements(mode, start, end, count, type, indices);
}

typedef struct {
  const char* name;
  void* wrapper;
  void** real;
} probe_wrap;

static const probe_wrap wraps[] = {
  { "glDrawArrays", draw_arrays, (void**)&real_draw_arrays },
  { "glDrawElements", draw_elements, (void**)&real_draw_elements },
  { "glDrawRangeElements", draw_range_elements,
    (void**)&real_draw_range_elements },
  { "glDrawRangeElementsEXT", draw_range_elements,
    (void**)&real_draw_range_elements },
};

void* hle_gl_probe_wrap(const char* name, void* real) {
  if (!real || !text_to_probe()) {
    return real;
  }
  for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
    if (!strcmp(name, wraps[i].name)) {
      *wraps[i].real = real;
      return wraps[i].wrapper;
    }
  }
  return real;
}
