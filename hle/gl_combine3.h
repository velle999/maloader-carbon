// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// GL_ATI_texture_env_combine3 over GL_NV_texture_env_combine4. See
// gl_combine3.c.

#ifndef HLE_GL_COMBINE3_H_
#define HLE_GL_COMBINE3_H_

// |real|, the GL function |name| names, or the emulation's function in its
// place: the texture environment functions.
void* hle_gl_combine3_wrap(const char* name, void* real);

#endif  // HLE_GL_COMBINE3_H_
