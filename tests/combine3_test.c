// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/gl_combine3.c against the GL driver: a texture unit set up with
// ATI's combine functions through the emulation colors a quad, drawn into a
// framebuffer object of a hidden window, as the ATI extension's formulas say,
// and goes back to plain COMBINE functions when they are set again. Needs a
// display and a driver with GL_NV_texture_env_combine4; says so and passes
// when there is none.
//
//   make tests/combine3_test && tests/combine3_test

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "../hle/gl_combine3.h"

enum {
  GL_QUADS = 0x0007,
  GL_ADD = 0x0104,
  GL_SRC_COLOR = 0x0300,
  GL_SRC_ALPHA = 0x0302,
  GL_ONE_MINUS_SRC_ALPHA = 0x0303,
  GL_EXTENSIONS = 0x1F03,
  GL_TEXTURE_2D = 0x0DE1,
  GL_UNSIGNED_BYTE = 0x1401,
  GL_RGBA = 0x1908,
  GL_REPLACE = 0x1E01,
  GL_MODULATE = 0x2100,
  GL_NEAREST = 0x2600,
  GL_TEXTURE_ENV_MODE = 0x2200,
  GL_TEXTURE_ENV_COLOR = 0x2201,
  GL_TEXTURE_ENV = 0x2300,
  GL_TEXTURE_MAG_FILTER = 0x2800,
  GL_TEXTURE_MIN_FILTER = 0x2801,
  GL_RGBA8 = 0x8058,
  GL_COMBINE = 0x8570,
  GL_COMBINE_RGB = 0x8571,
  GL_COMBINE_ALPHA = 0x8572,
  GL_INTERPOLATE = 0x8575,
  GL_CONSTANT = 0x8576,
  GL_PRIMARY_COLOR = 0x8577,
  GL_TEXTURE = 0x1702,
  GL_SOURCE0_RGB = 0x8580,
  GL_SOURCE0_ALPHA = 0x8588,
  GL_OPERAND0_RGB = 0x8590,
  GL_OPERAND0_ALPHA = 0x8598,
  GL_MODULATE_ADD_ATI = 0x8744,
  GL_MODULATE_SIGNED_ADD_ATI = 0x8745,
  GL_FRAMEBUFFER_EXT = 0x8D40,
  GL_COLOR_ATTACHMENT0_EXT = 0x8CE0,
  GL_FRAMEBUFFER_COMPLETE_EXT = 0x8CD5,
};

static struct {
  const uint8_t* (*get_string)(uint32_t);
  void (*enable)(uint32_t);
  void (*gen_textures)(int32_t, uint32_t*);
  void (*bind_texture)(uint32_t, uint32_t);
  void (*tex_image_2d)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                       uint32_t, uint32_t, const void*);
  void (*tex_parameteri)(uint32_t, uint32_t, int32_t);
  void (*gen_framebuffers)(int32_t, uint32_t*);
  void (*bind_framebuffer)(uint32_t, uint32_t);
  void (*framebuffer_texture_2d)(uint32_t, uint32_t, uint32_t, uint32_t,
                                 int32_t);
  uint32_t (*check_framebuffer_status)(uint32_t);
  void (*viewport)(int32_t, int32_t, int32_t, int32_t);
  void (*color_4f)(float, float, float, float);
  void (*begin)(uint32_t);
  void (*vertex_2f)(float, float);
  void (*end)(void);
  void (*get_tex_image)(uint32_t, int32_t, uint32_t, uint32_t, void*);
} gl;

// The texture drawn into, and the one the unit reads.
static uint32_t target;
static uint32_t framebuffer;
static uint32_t texture;

static void (*tex_envi)(uint32_t, uint32_t, int32_t);
static void (*tex_envfv)(uint32_t, uint32_t, const float*);

static int failures;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static int load(void) {
  static const struct {
    const char* name;
    void** slot;
  } kFunctions[] = {
    { "glGetString", (void**)&gl.get_string },
    { "glEnable", (void**)&gl.enable },
    { "glGenTextures", (void**)&gl.gen_textures },
    { "glBindTexture", (void**)&gl.bind_texture },
    { "glTexImage2D", (void**)&gl.tex_image_2d },
    { "glTexParameteri", (void**)&gl.tex_parameteri },
    { "glGenFramebuffersEXT", (void**)&gl.gen_framebuffers },
    { "glBindFramebufferEXT", (void**)&gl.bind_framebuffer },
    { "glFramebufferTexture2DEXT", (void**)&gl.framebuffer_texture_2d },
    { "glCheckFramebufferStatusEXT", (void**)&gl.check_framebuffer_status },
    { "glViewport", (void**)&gl.viewport },
    { "glColor4f", (void**)&gl.color_4f },
    { "glBegin", (void**)&gl.begin },
    { "glVertex2f", (void**)&gl.vertex_2f },
    { "glEnd", (void**)&gl.end },
    { "glGetTexImage", (void**)&gl.get_tex_image },
  };
  for (size_t i = 0; i < sizeof(kFunctions) / sizeof(kFunctions[0]); i++) {
    *kFunctions[i].slot = dlsym(RTLD_DEFAULT, kFunctions[i].name);
    if (!*kFunctions[i].slot) {
      printf("the GL library has no %s\n", kFunctions[i].name);
      return 0;
    }
  }
  tex_envi =
      hle_gl_combine3_wrap("glTexEnvi", dlsym(RTLD_DEFAULT, "glTexEnvi"));
  tex_envfv =
      hle_gl_combine3_wrap("glTexEnvfv", dlsym(RTLD_DEFAULT, "glTexEnvfv"));
  return tex_envi && tex_envfv;
}

// Draws the quad and returns a pixel, in 0-255. NVIDIA's 304 driver reads
// nothing but zeros from a framebuffer object of a hidden window, so the
// pixel comes from the texture drawn into.
static void draw(uint8_t out[4]) {
  gl.begin(GL_QUADS);
  gl.vertex_2f(-1, -1);
  gl.vertex_2f(1, -1);
  gl.vertex_2f(1, 1);
  gl.vertex_2f(-1, 1);
  gl.end();
  uint8_t pixels[4 * 4 * 4];
  gl.bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
  gl.bind_texture(GL_TEXTURE_2D, target);
  gl.get_tex_image(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  gl.bind_texture(GL_TEXTURE_2D, texture);
  gl.bind_framebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
  memcpy(out, pixels + (2 * 4 + 2) * 4, 4);
}

static int near(const uint8_t got[4], const float want[4]) {
  for (int i = 0; i < 4; i++) {
    float w = want[i] < 0 ? 0 : want[i] > 1 ? 1 : want[i];
    if (fabs(got[i] - w * 255) > 3) {
      return 0;
    }
  }
  return 1;
}

static void check_pixel(const float want[4], const char* what) {
  uint8_t got[4];
  draw(got);
  if (!near(got, want)) {
    printf("  %s: got %u %u %u %u, want %.0f %.0f %.0f %.0f\n", what, got[0],
           got[1], got[2], got[3], want[0] * 255, want[1] * 255,
           want[2] * 255, want[3] * 255);
  }
  check(near(got, want), what);
}

int main(void) {
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    printf("no display (%s): nothing checked\nall passed\n", SDL_GetError());
    return 0;
  }
  SDL_Window* window = SDL_CreateWindow("combine3_test", 0, 0, 8, 8,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
  if (!context || !load()) {
    printf("no GL context (%s): nothing checked\nall passed\n",
           SDL_GetError());
    return 0;
  }
  const char* extensions = (const char*)gl.get_string(GL_EXTENSIONS);
  if (!extensions || !strstr(extensions, "GL_NV_texture_env_combine4") ||
      strstr(extensions, "GL_ATI_texture_env_combine3")) {
    printf("the driver has nothing to emulate or nothing to emulate with: "
           "nothing checked\nall passed\n");
    return 0;
  }

  // Draw into a texture: a hidden window's pixels are not there to read.
  gl.gen_textures(1, &target);
  gl.bind_texture(GL_TEXTURE_2D, target);
  gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl.tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, NULL);
  gl.gen_framebuffers(1, &framebuffer);
  gl.bind_framebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
  gl.framebuffer_texture_2d(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                            GL_TEXTURE_2D, target, 0);
  if (gl.check_framebuffer_status(GL_FRAMEBUFFER_EXT) !=
      GL_FRAMEBUFFER_COMPLETE_EXT) {
    printf("FAIL: the framebuffer object is not complete\n");
    return 1;
  }
  gl.viewport(0, 0, 4, 4);

  // The texture the unit reads: one texel.
  static const uint8_t kTexel[4] = { 64, 128, 191, 204 };
  static const float kT[4] = { 64 / 255.0f, 128 / 255.0f, 191 / 255.0f,
                               204 / 255.0f };
  static const float kP[4] = { 0.5f, 0.25f, 1.0f, 0.6f };
  static const float kC[4] = { 0.2f, 0.1f, 0.0f, 0.3f };
  gl.gen_textures(1, &texture);
  gl.bind_texture(GL_TEXTURE_2D, texture);
  gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl.tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl.tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, kTexel);
  gl.enable(GL_TEXTURE_2D);
  gl.color_4f(kP[0], kP[1], kP[2], kP[3]);
  tex_envfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, kC);

  // As the game's Direct3D layer sets D3DTOP_MULTIPLYADD: the mode first,
  // then the function, then the arguments.
  tex_envi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
  tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE_ADD_ATI);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
  tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + 1, GL_CONSTANT);
  tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + 1, GL_SRC_COLOR);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + 2, GL_TEXTURE);
  tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + 2, GL_SRC_COLOR);
  float want[4];
  for (int i = 0; i < 3; i++) {
    want[i] = kP[i] * kT[i] + kC[i];
  }
  want[3] = kP[3];
  check_pixel(want, "MODULATE_ADD_ATI: primary * texture + constant, "
                    "alpha replaced by the primary color's");

  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE_SIGNED_ADD_ATI);
  for (int i = 0; i < 3; i++) {
    want[i] = kP[i] * kT[i] + kC[i] - 0.5f;
  }
  check_pixel(want, "MODULATE_SIGNED_ADD_ATI: less a half");

  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE_ADD_ATI);
  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_INTERPOLATE);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + 1, GL_CONSTANT);
  tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + 1, GL_SRC_ALPHA);
  tex_envi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + 2, GL_PRIMARY_COLOR);
  tex_envi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + 2, GL_ONE_MINUS_SRC_ALPHA);
  for (int i = 0; i < 3; i++) {
    want[i] = kP[i] * kT[i] + kC[i];
  }
  // INTERPOLATE: Arg0 * Arg2 + Arg1 * (1 - Arg2), Arg2 = 1 - primary alpha.
  want[3] = kT[3] * (1 - kP[3]) + kC[3] * kP[3];
  check_pixel(want, "an alpha INTERPOLATE beside MODULATE_ADD_ATI");

  // Back to a function GL has: the unit is COMBINE again, with the game's
  // arguments.
  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
  for (int i = 0; i < 3; i++) {
    want[i] = kP[i] * kC[i];
  }
  want[3] = kT[3];
  check_pixel(want, "MODULATE after MODULATE_ADD_ATI: primary * constant, "
                    "alpha the texture's");

  tex_envi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD);
  for (int i = 0; i < 3; i++) {
    want[i] = kP[i] + kC[i];
  }
  check_pixel(want, "ADD while not emulated");

  SDL_GL_DeleteContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  if (failures) {
    printf("%d failed\n", failures);
    return 1;
  }
  printf("all passed\n");
  return 0;
}
