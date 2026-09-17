// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The fonts the game can name, shared by the Font Manager and ATS calls
// (fonts.c) and ATSUI (atsui.c).
//
// A font here is one FreeType face. Its ATSFontRef and its FMFont are the
// same number, as are a family's ATSFontFamilyRef and FMFontFamily.

#ifndef HLE_FONTS_H_
#define HLE_FONTS_H_

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdint.h>

// FreeType is not safe to use from two threads at once; everything that
// touches a face holds this.
void hle_fonts_lock(void);
void hle_fonts_unlock(void);

// The face for a font number, or NULL. Call with the lock held.
FT_Face hle_font_face(uint32_t font);

// The font to draw a QuickDraw family in, with |style| (bold 1, italic 2)
// where the family has it. 0 when there is none.
uint32_t hle_font_for_family(int family, int style);

// The style bits (bold 1, italic 2) a font draws in by itself.
int hle_font_style(uint32_t font);

#endif  // HLE_FONTS_H_
