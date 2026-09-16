// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// GL_ATI_texture_env_combine3 over GL_NV_texture_env_combine4.
//
// Apple's OpenGL gives every renderer ATI's MODULATE_ADD combine function,
// and Halo's Direct3D layer sets it for D3DTOP_MULTIPLYADD without looking
// for the extension, as it does for bullet marks. NVIDIA's driver lacks the
// extension and refuses the function, so the texture unit went on combining
// with whatever function it had before: a decal drawn with the primary color
// alone, a white square doubling the wall behind it.
//
// NVIDIA's COMBINE4_NV computes Arg0 * Arg1 + Arg2 * Arg3, which holds ATI's
// Arg0 * Arg2 + Arg1. While the game has a unit in COMBINE mode with an ATI
// function, that unit is in COMBINE4_NV with the game's arguments rearranged,
// its other function (alpha or color) rewritten the same way, and it goes
// back to COMBINE when the game changes the function. What the game set is
// kept for each unit, as GL no longer holds it; the game never reads the
// texture environment back, nor pushes or pops it. HLE_COMBINE3=0 leaves it
// out, as does a driver with the ATI extension. The game makes its GL calls
// on one thread.

#define _GNU_SOURCE

#include "gl_combine3.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  GL_ZERO = 0,
  GL_ONE = 1,
  GL_ADD = 0x0104,
  GL_SRC_COLOR = 0x0300,
  GL_ONE_MINUS_SRC_COLOR = 0x0301,
  GL_SRC_ALPHA = 0x0302,
  GL_ONE_MINUS_SRC_ALPHA = 0x0303,
  GL_EXTENSIONS = 0x1F03,
  GL_REPLACE = 0x1E01,
  GL_MODULATE = 0x2100,
  GL_TEXTURE_ENV_MODE = 0x2200,
  GL_TEXTURE_ENV = 0x2300,
  GL_TEXTURE0 = 0x84C0,
  GL_ACTIVE_TEXTURE = 0x84E0,
  GL_SUBTRACT = 0x84E7,
  GL_COMBINE4_NV = 0x8503,
  GL_COMBINE = 0x8570,
  GL_COMBINE_RGB = 0x8571,
  GL_COMBINE_ALPHA = 0x8572,
  GL_ADD_SIGNED = 0x8574,
  GL_INTERPOLATE = 0x8575,
  GL_PRIMARY_COLOR = 0x8577,
  GL_PREVIOUS = 0x8578,
  GL_SOURCE0_RGB = 0x8580,
  GL_SOURCE3_RGB_NV = 0x8583,
  GL_SOURCE0_ALPHA = 0x8588,
  GL_SOURCE3_ALPHA_NV = 0x858B,
  GL_OPERAND0_RGB = 0x8590,
  GL_OPERAND3_RGB_NV = 0x8593,
  GL_OPERAND0_ALPHA = 0x8598,
  GL_OPERAND3_ALPHA_NV = 0x859B,
  GL_TEXTURE = 0x1702,
  GL_MODULATE_ADD_ATI = 0x8744,
  GL_MODULATE_SIGNED_ADD_ATI = 0x8745,
  GL_MODULATE_SUBTRACT_ATI = 0x8746,
  kUnits = 8,
};

// A unit's texture environment as the game set it.
typedef struct {
  int32_t mode;
  int32_t combine[2];  // color, alpha
  int32_t source[6];   // color 0-2, alpha 0-2
  int32_t operand[6];
  int emulated;        // GL has the unit in COMBINE4_NV
} unit_env;

typedef struct {
  int32_t source;
  int32_t operand;
} combine_arg;

static unit_env units[kUnits];
static int enabled = -1;

static struct {
  void (*tex_envi)(uint32_t, uint32_t, int32_t);
  void (*tex_envf)(uint32_t, uint32_t, float);
  void (*tex_enviv)(uint32_t, uint32_t, const int32_t*);
  void (*tex_envfv)(uint32_t, uint32_t, const float*);
  void (*get_integer_v)(uint32_t, int32_t*);
  const uint8_t* (*get_string)(uint32_t);
} gl;

static int has_extension(const char* all, const char* name) {
  size_t n = strlen(name);
  for (const char* at = all; (at = strstr(at, name)) != NULL; at += n) {
    if ((at == all || at[-1] == ' ') && (at[n] == ' ' || at[n] == '\0')) {
      return 1;
    }
  }
  return 0;
}

// Decided at the first texture environment call, when a context is current.
static int emulating(void) {
  if (enabled >= 0) {
    return enabled;
  }
  const char* setting = getenv("HLE_COMBINE3");
  const char* all =
      gl.get_string ? (const char*)gl.get_string(GL_EXTENSIONS) : NULL;
  enabled = !(setting && strcmp(setting, "0") == 0) && all &&
            gl.tex_envi && gl.get_integer_v &&
            !has_extension(all, "GL_ATI_texture_env_combine3") &&
            has_extension(all, "GL_NV_texture_env_combine4");
  if (enabled) {
    for (int i = 0; i < kUnits; i++) {
      unit_env* u = &units[i];
      u->mode = GL_MODULATE;
      u->combine[0] = GL_MODULATE;
      u->combine[1] = GL_MODULATE;
      for (int c = 0; c < 2; c++) {
        u->source[3 * c] = GL_TEXTURE;
        u->source[3 * c + 1] = GL_PREVIOUS;
        u->source[3 * c + 2] = GL_PRIMARY_COLOR;
      }
      u->operand[0] = GL_SRC_COLOR;
      u->operand[1] = GL_SRC_COLOR;
      u->operand[2] = GL_SRC_ALPHA;
      u->operand[3] = GL_SRC_ALPHA;
      u->operand[4] = GL_SRC_ALPHA;
      u->operand[5] = GL_SRC_ALPHA;
    }
    fprintf(stderr, "hle: GL_ATI_texture_env_combine3 is emulated with "
            "GL_NV_texture_env_combine4\n");
  }
  return enabled;
}

static int is_ati(int32_t function) {
  return function == GL_MODULATE_ADD_ATI ||
         function == GL_MODULATE_SIGNED_ADD_ATI ||
         function == GL_MODULATE_SUBTRACT_ATI;
}

static int32_t* slot_of(unit_env* u, uint32_t pname) {
  if (pname == GL_TEXTURE_ENV_MODE) {
    return &u->mode;
  }
  if (pname == GL_COMBINE_RGB || pname == GL_COMBINE_ALPHA) {
    return &u->combine[pname - GL_COMBINE_RGB];
  }
  if (pname >= GL_SOURCE0_RGB && pname < GL_SOURCE0_RGB + 3) {
    return &u->source[pname - GL_SOURCE0_RGB];
  }
  if (pname >= GL_SOURCE0_ALPHA && pname < GL_SOURCE0_ALPHA + 3) {
    return &u->source[3 + pname - GL_SOURCE0_ALPHA];
  }
  if (pname >= GL_OPERAND0_RGB && pname < GL_OPERAND0_RGB + 3) {
    return &u->operand[pname - GL_OPERAND0_RGB];
  }
  if (pname >= GL_OPERAND0_ALPHA && pname < GL_OPERAND0_ALPHA + 3) {
    return &u->operand[3 + pname - GL_OPERAND0_ALPHA];
  }
  return NULL;
}

static int32_t inverted(int32_t operand) {
  switch (operand) {
    case GL_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return GL_SRC_COLOR;
    case GL_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GL_SRC_ALPHA;
  }
  return operand;
}

// The |i|th argument of |channel| (0 color, 1 alpha), with ATI's ONE source,
// which COMBINE4_NV lacks, as ZERO inverted.
static combine_arg argument(const unit_env* u, int channel, int i) {
  combine_arg a = { u->source[3 * channel + i], u->operand[3 * channel + i] };
  if (a.source == GL_ONE) {
    a.source = GL_ZERO;
    a.operand = inverted(a.operand);
  }
  return a;
}

// Which COMBINE4_NV function and four arguments give |channel|'s function.
// Returns 0 for one they cannot.
static int combine4_form(const unit_env* u, int channel, int32_t* function,
                         combine_arg* args) {
  const combine_arg one = { GL_ZERO, channel ? GL_ONE_MINUS_SRC_ALPHA
                                             : GL_ONE_MINUS_SRC_COLOR };
  const combine_arg zero = { GL_ZERO, channel ? GL_SRC_ALPHA : GL_SRC_COLOR };
  combine_arg a0 = argument(u, channel, 0);
  combine_arg a1 = argument(u, channel, 1);
  combine_arg a2 = argument(u, channel, 2);
  *function = GL_ADD;
  switch (u->combine[channel]) {
    case GL_REPLACE:
      args[0] = a0, args[1] = one, args[2] = zero, args[3] = zero;
      return 1;
    case GL_MODULATE:
      args[0] = a0, args[1] = a1, args[2] = zero, args[3] = zero;
      return 1;
    case GL_ADD_SIGNED:
      *function = GL_ADD_SIGNED;
      // Fall through.
    case GL_ADD:
      args[0] = a0, args[1] = one, args[2] = a1, args[3] = one;
      return 1;
    case GL_INTERPOLATE:
      args[0] = a0, args[1] = a2, args[2] = a1;
      args[3] = (combine_arg){ a2.source, inverted(a2.operand) };
      return 1;
    case GL_MODULATE_SIGNED_ADD_ATI:
      *function = GL_ADD_SIGNED;
      // Fall through.
    case GL_MODULATE_ADD_ATI:
      args[0] = a0, args[1] = a2, args[2] = a1, args[3] = one;
      return 1;
  }
  args[0] = a0, args[1] = one, args[2] = zero, args[3] = zero;
  return 0;
}

static void set_combine4(const unit_env* u) {
  static const uint32_t kFunction[2] = { GL_COMBINE_RGB, GL_COMBINE_ALPHA };
  static const uint32_t kSource[2] = { GL_SOURCE0_RGB, GL_SOURCE0_ALPHA };
  static const uint32_t kSource3[2] = { GL_SOURCE3_RGB_NV,
                                        GL_SOURCE3_ALPHA_NV };
  static const uint32_t kOperand[2] = { GL_OPERAND0_RGB, GL_OPERAND0_ALPHA };
  static const uint32_t kOperand3[2] = { GL_OPERAND3_RGB_NV,
                                         GL_OPERAND3_ALPHA_NV };
  gl.tex_envi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE4_NV);
  for (int channel = 0; channel < 2; channel++) {
    int32_t function;
    combine_arg args[4];
    if (!combine4_form(u, channel, &function, args)) {
      static int warned;
      if (!warned) {
        warned = 1;
        fprintf(stderr, "hle: combine function %#x has no COMBINE4_NV form; "
                "its first argument stands in\n",
                (unsigned)u->combine[channel]);
      }
    }
    gl.tex_envi(GL_TEXTURE_ENV, kFunction[channel], function);
    for (int i = 0; i < 4; i++) {
      gl.tex_envi(GL_TEXTURE_ENV,
                  i < 3 ? kSource[channel] + i : kSource3[channel],
                  args[i].source);
      gl.tex_envi(GL_TEXTURE_ENV,
                  i < 3 ? kOperand[channel] + i : kOperand3[channel],
                  args[i].operand);
    }
  }
}

static void set_combine(const unit_env* u) {
  gl.tex_envi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, u->mode);
  gl.tex_envi(GL_TEXTURE_ENV, GL_COMBINE_RGB, u->combine[0]);
  gl.tex_envi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, u->combine[1]);
  for (int i = 0; i < 3; i++) {
    gl.tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + i, u->source[i]);
    gl.tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + i, u->source[3 + i]);
    gl.tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + i, u->operand[i]);
    gl.tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + i, u->operand[3 + i]);
  }
}

// Records a texture environment setting of the active unit and gives GL
// what it needs for it. Returns 0 for a setting that goes to GL as it is.
static int handle(uint32_t target, uint32_t pname, int32_t value) {
  if (target != GL_TEXTURE_ENV || !emulating()) {
    return 0;
  }
  int32_t active = GL_TEXTURE0;
  gl.get_integer_v(GL_ACTIVE_TEXTURE, &active);
  unsigned unit = (unsigned)(active - GL_TEXTURE0);
  int32_t* slot = unit < kUnits ? slot_of(&units[unit], pname) : NULL;
  if (!slot) {
    return 0;
  }
  unit_env* u = &units[unit];
  *slot = value;
  if (u->mode == GL_COMBINE &&
      (is_ati(u->combine[0]) || is_ati(u->combine[1]))) {
    set_combine4(u);
    u->emulated = 1;
  } else if (u->emulated) {
    set_combine(u);
    u->emulated = 0;
  } else {
    gl.tex_envi(target, pname, value);
  }
  return 1;
}

static void tex_envi(uint32_t target, uint32_t pname, int32_t param) {
  if (!handle(target, pname, param)) {
    gl.tex_envi(target, pname, param);
  }
}

static void tex_envf(uint32_t target, uint32_t pname, float param) {
  if (!slot_of(&units[0], pname) || !handle(target, pname, (int32_t)param)) {
    gl.tex_envf(target, pname, param);
  }
}

static void tex_enviv(uint32_t target, uint32_t pname, const int32_t* params) {
  if (!params || !slot_of(&units[0], pname) ||
      !handle(target, pname, params[0])) {
    gl.tex_enviv(target, pname, params);
  }
}

static void tex_envfv(uint32_t target, uint32_t pname, const float* params) {
  if (!params || !slot_of(&units[0], pname) ||
      !handle(target, pname, (int32_t)params[0])) {
    gl.tex_envfv(target, pname, params);
  }
}

void* hle_gl_combine3_wrap(const char* name, void* real) {
  static const struct {
    const char* name;
    void* wrapper;
    void** real;
  } kWraps[] = {
    { "glTexEnvi", tex_envi, (void**)&gl.tex_envi },
    { "glTexEnvf", tex_envf, (void**)&gl.tex_envf },
    { "glTexEnviv", tex_enviv, (void**)&gl.tex_enviv },
    { "glTexEnvfv", tex_envfv, (void**)&gl.tex_envfv },
  };
  if (!real) {
    return real;
  }
  for (size_t i = 0; i < sizeof(kWraps) / sizeof(kWraps[0]); i++) {
    if (!strcmp(name, kWraps[i].name)) {
      *kWraps[i].real = real;
      if (!gl.get_integer_v) {
        gl.get_integer_v = dlsym(RTLD_DEFAULT, "glGetIntegerv");
        gl.get_string = dlsym(RTLD_DEFAULT, "glGetString");
      }
      return kWraps[i].wrapper;
    }
  }
  return real;
}
