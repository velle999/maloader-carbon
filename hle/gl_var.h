// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Apple's vertex array range, fence and vertex array object extensions over
// ARB vertex buffer objects. See gl_var.c.

#ifndef HLE_GL_VAR_H_
#define HLE_GL_VAR_H_

#include <stddef.h>

// Whether the extensions are emulated: HLE_VAR is not 0, and the GL library
// has the ARB buffer object functions.
int hle_gl_var_enabled(void);

// |real|, the GL function |name| names, or the emulation's function in its
// place: Apple's functions, and the array pointer and client state functions
// the emulation sees first.
void* hle_gl_var_wrap(const char* name, void* real);

// Forgets which buffer object is bound, for another context made current.
void hle_gl_var_context_changed(void);

// Describes the bound vertex array object's first arrays, as the game gave
// them and as GL was last given them, into |out|.
void hle_gl_var_describe(char* out, size_t size);

#endif  // HLE_GL_VAR_H_
