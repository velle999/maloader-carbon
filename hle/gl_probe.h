// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// A report of GL's state at the draws made with a chosen vertex program. See
// gl_probe.c.

#ifndef HLE_GL_PROBE_H_
#define HLE_GL_PROBE_H_

#include <stdint.h>

// |real|, the GL function |name| names, or a wrapper that reports before it
// draws, when HLE_PROBE is set.
void* hle_gl_probe_wrap(const char* name, void* real);

// Notes the source glProgramStringARB has just given the program bound to
// |target|.
void hle_gl_probe_program(uint32_t target, const char* text, int32_t length);

#endif  // HLE_GL_PROBE_H_
