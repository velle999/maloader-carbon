// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// ATSUI: styles, text layouts, measurement and drawing into QuickDraw ports,
// with glyphs from FreeType.
//
// A layout has one style for all its text and sets it on one line, glyph
// after glyph, without kerning or reordering. Sizes are points at 72 dots
// an inch, so a point is a pixel. Glyphs are anti-aliased, except on a
// 1-bit port. A style asking for bold or italic that its font does not
// have gets FreeType's synthetic emboldening or slant.

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "fonts.h"

#include FT_SYNTHESIS_H

#include "carbon.h"
#include "gui.h"

enum {
  kATSUInvalidTextLayoutErr = -8790,
  kATSUInvalidStyleErr = -8791,
  kATSUInvalidTextRangeErr = -8792,
  kATSUInvalidFontErr = -8796,
};

enum {
  kATSUQDBoldfaceTag = 256,
  kATSUQDItalicTag = 257,
  kATSUQDUnderlineTag = 258,
  kATSUFontTag = 261,
  kATSUSizeTag = 262,
  kATSUColorTag = 263,
  kATSUStyleStrikeThroughTag = 292,
};

enum {
  kATSUFromTextBeginning = 0xFFFFFFFFu,
  kATSUToTextEnd = 0xFFFFFFFFu,
  kATSUUseGrafPortPenLoc = (int32_t)0xFFFFFFFF,
  kATSUDirectDataLayoutRecordATSLayoutRecordCurrent = 100,
};

enum {
  kMagicStyle = 'ATSs',
  kMagicLayout = 'ATSl',
};

typedef struct {
  uint32_t magic;
  uint32_t font;
  Fixed size;
  int bold;
  int italic;
  RGBColor color;
} atsu_style;

// ATSLayoutRecord, with 2-byte packing like the other Carbon structures.
// Age of Empires III reads only the first record's glyphID, which comes
// first either way.
#pragma pack(push, 2)
typedef struct {
  UInt16 glyphID;
  UInt32 flags;
  UInt32 originalOffset;
  Fixed realPos;
} ATSLayoutRecord;
#pragma pack(pop)

typedef struct {
  uint32_t magic;
  const UniChar* text;
  uint32_t offset;
  uint32_t length;
  atsu_style* style;
  ATSLayoutRecord* records;
} atsu_layout;

typedef struct {
  float x;
  float y;
} Float32Point;

typedef struct {
  Float32Point deviceAdvance;
  Float32Point topLeft;
  UInt32 height;
  UInt32 width;
  Float32Point sideBearing;
  Float32Point otherSideBearing;
} ATSGlyphScreenMetrics;

static atsu_style* style_of(void* ref) {
  atsu_style* style = ref;
  return style && style->magic == kMagicStyle ? style : NULL;
}

static atsu_layout* layout_of(void* ref) {
  atsu_layout* layout = ref;
  return layout && layout->magic == kMagicLayout ? layout : NULL;
}

int32_t ATSUCreateStyle(atsu_style** out) {
  if (!out) {
    return paramErr;
  }
  atsu_style* style = calloc(1, sizeof(*style));
  style->magic = kMagicStyle;
  style->size = 12 << 16;
  *out = style;
  return noErr;
}

int32_t ATSUDisposeStyle(atsu_style* style) {
  style = style_of(style);
  if (!style) {
    return kATSUInvalidStyleErr;
  }
  style->magic = 0;
  free(style);
  return noErr;
}

// Booleans are a byte.
int32_t ATSUSetAttributes(atsu_style* style, uint32_t count,
                          const uint32_t* tags, const uint32_t* sizes,
                          const void* const* values) {
  style = style_of(style);
  if (!style) {
    return kATSUInvalidStyleErr;
  }
  for (uint32_t i = 0; i < count; i++) {
    const void* value = values[i];
    if (!value) {
      continue;
    }
    switch (tags[i]) {
      case kATSUFontTag:
        style->font = *(const uint32_t*)value;
        break;
      case kATSUSizeTag:
        style->size = *(const Fixed*)value;
        break;
      case kATSUColorTag:
        style->color = *(const RGBColor*)value;
        break;
      case kATSUQDBoldfaceTag:
        style->bold = *(const uint8_t*)value != 0;
        break;
      case kATSUQDItalicTag:
        style->italic = *(const uint8_t*)value != 0;
        break;
      case kATSUQDUnderlineTag:
      case kATSUStyleStrikeThroughTag:
        break;
      default:
        cf_trace("ATSUSetAttributes: tag %u is not used", tags[i]);
        break;
    }
  }
  return noErr;
}

int32_t ATSUCreateTextLayout(atsu_layout** out) {
  if (!out) {
    return paramErr;
  }
  atsu_layout* layout = calloc(1, sizeof(*layout));
  layout->magic = kMagicLayout;
  *out = layout;
  return noErr;
}

int32_t ATSUDisposeTextLayout(atsu_layout* layout) {
  layout = layout_of(layout);
  if (!layout) {
    return kATSUInvalidTextLayoutErr;
  }
  layout->magic = 0;
  free(layout->records);
  free(layout);
  return noErr;
}

int32_t ATSUSetTextPointerLocation(atsu_layout* layout, const UniChar* text,
                                   uint32_t offset, uint32_t length,
                                   uint32_t total_length) {
  layout = layout_of(layout);
  if (!layout) {
    return kATSUInvalidTextLayoutErr;
  }
  layout->text = text;
  layout->offset = offset == kATSUFromTextBeginning ? 0 : offset;
  layout->length = length == kATSUToTextEnd ? total_length - layout->offset
                                            : length;
  return noErr;
}

// The one style applies to all the layout's text, whatever the range.
int32_t ATSUSetRunStyle(atsu_layout* layout, atsu_style* style,
                        uint32_t offset, uint32_t length) {
  layout = layout_of(layout);
  if (!layout) {
    return kATSUInvalidTextLayoutErr;
  }
  if (!style_of(style)) {
    return kATSUInvalidStyleErr;
  }
  layout->style = style;
  return noErr;
}

// ---------------------------------------------------------------------------
// Glyphs

// The characters [*start, *end) of the layout a call's range names.
static int line_range(const atsu_layout* layout, uint32_t offset,
                      uint32_t length, uint32_t* start, uint32_t* end) {
  *start = offset == kATSUFromTextBeginning ? layout->offset : offset;
  uint32_t layout_end = layout->offset + layout->length;
  *end = length == kATSUToTextEnd ? layout_end : *start + length;
  if (*end > layout_end) {
    *end = layout_end;
  }
  return layout->text && *start <= *end;
}

// Sizes the style's face and loads glyph |index| into its slot, rendered
// when |render|. NULL when the style's font is gone. Call with the fonts
// lock held.
static FT_GlyphSlot load_glyph(const atsu_style* style, FT_UInt index,
                               int render, int mono) {
  FT_Face face = hle_font_face(style->font);
  if (!face) {
    return NULL;
  }
  FT_F26Dot6 size = style->size >> 10;
  if (FT_Set_Char_Size(face, 0, size > 0 ? size : 1, 72, 72)) {
    return NULL;
  }
  FT_Int32 flags = FT_LOAD_NO_BITMAP |
                   (mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_LIGHT);
  if (FT_Load_Glyph(face, index, flags)) {
    return NULL;
  }
  FT_GlyphSlot slot = face->glyph;
  int intrinsic = (face->style_flags & FT_STYLE_FLAG_BOLD ? 1 : 0) |
                  (face->style_flags & FT_STYLE_FLAG_ITALIC ? 2 : 0);
  if (style->bold && !(intrinsic & 1)) {
    FT_GlyphSlot_Embolden(slot);
  }
  if (style->italic && !(intrinsic & 2)) {
    FT_GlyphSlot_Oblique(slot);
  }
  if (render && slot->format != FT_GLYPH_FORMAT_BITMAP &&
      FT_Render_Glyph(slot, mono ? FT_RENDER_MODE_MONO
                                 : FT_RENDER_MODE_NORMAL)) {
    return NULL;
  }
  return slot;
}

static FT_UInt glyph_index(const atsu_style* style, UniChar c) {
  FT_Face face = hle_font_face(style->font);
  return face ? FT_Get_Char_Index(face, c) : 0;
}

// Calls |each| for every glyph of the characters [start, end), rendered, at
// its origin |x| (26.6, from the line's start). Stops at a glyph that does
// not load. Call with the fonts lock held.
static void for_each_glyph(const atsu_layout* layout, uint32_t start,
                           uint32_t end, int mono,
                           void (*each)(void* context, FT_GlyphSlot slot,
                                        FT_Pos x),
                           void* context) {
  FT_Pos x = 0;
  for (uint32_t i = start; i < end; i++) {
    FT_GlyphSlot slot =
        load_glyph(layout->style, glyph_index(layout->style, layout->text[i]),
                   1, mono);
    if (!slot) {
      return;
    }
    each(context, slot, x);
    x += slot->advance.x;
  }
}

typedef struct {
  int any;
  int top;
  int left;
  int bottom;
  int right;
} ink_box;

static void grow_box(void* context, FT_GlyphSlot slot, FT_Pos x) {
  ink_box* box = context;
  if (!slot->bitmap.width || !slot->bitmap.rows) {
    return;
  }
  int left = (int)((x + 32) >> 6) + slot->bitmap_left;
  int top = -slot->bitmap_top;
  int right = left + (int)slot->bitmap.width;
  int bottom = top + (int)slot->bitmap.rows;
  if (!box->any) {
    *box = (ink_box){ 1, top, left, bottom, right };
    return;
  }
  box->top = top < box->top ? top : box->top;
  box->left = left < box->left ? left : box->left;
  box->bottom = bottom > box->bottom ? bottom : box->bottom;
  box->right = right > box->right ? right : box->right;
}

// The box around the ink of the line's glyphs, from the origin (x, y).
int32_t ATSUMeasureTextImage(atsu_layout* layout, uint32_t offset,
                             uint32_t length, Fixed x, Fixed y, Rect* rect) {
  layout = layout_of(layout);
  if (!layout) {
    return kATSUInvalidTextLayoutErr;
  }
  uint32_t start, end;
  if (!layout->style || !line_range(layout, offset, length, &start, &end)) {
    return kATSUInvalidTextRangeErr;
  }
  ink_box box = { 0 };
  hle_fonts_lock();
  for_each_glyph(layout, start, end, 0, grow_box, &box);
  hle_fonts_unlock();
  int ox = x >> 16;
  int oy = y >> 16;
  if (rect) {
    rect->top = oy + box.top;
    rect->left = ox + box.left;
    rect->bottom = oy + box.bottom;
    rect->right = ox + box.right;
  }
  return noErr;
}

// One record for each glyph of the line starting at |offset|, and one for
// the line's end. The array lives until the layout does.
int32_t ATSUDirectGetLayoutDataArrayPtrFromTextLayout(atsu_layout* layout,
                                                      uint32_t offset,
                                                      uint32_t selector,
                                                      void** data,
                                                      uint32_t* count) {
  layout = layout_of(layout);
  if (!layout) {
    return kATSUInvalidTextLayoutErr;
  }
  if (selector != kATSUDirectDataLayoutRecordATSLayoutRecordCurrent) {
    if (data) {
      *data = NULL;
    }
    if (count) {
      *count = 0;
    }
    return paramErr;
  }
  uint32_t start, end;
  if (!layout->style || !line_range(layout, offset, kATSUToTextEnd, &start,
                                    &end)) {
    return kATSUInvalidTextRangeErr;
  }
  free(layout->records);
  uint32_t n = end - start;
  layout->records = calloc(n + 1, sizeof(ATSLayoutRecord));
  hle_fonts_lock();
  FT_Pos x = 0;
  for (uint32_t i = 0; i < n; i++) {
    FT_UInt index = glyph_index(layout->style, layout->text[start + i]);
    FT_GlyphSlot slot = load_glyph(layout->style, index, 0, 0);
    layout->records[i].glyphID = (UInt16)index;
    layout->records[i].originalOffset = i * sizeof(UniChar);
    layout->records[i].realPos = (Fixed)(x << 10);
    x += slot ? slot->advance.x : 0;
  }
  hle_fonts_unlock();
  layout->records[n].glyphID = 0xFFFF;
  layout->records[n].originalOffset = n * sizeof(UniChar);
  layout->records[n].realPos = (Fixed)(x << 10);
  if (data) {
    *data = layout->records;
  }
  if (count) {
    *count = n + 1;
  }
  return noErr;
}

// Each glyph as it draws on the screen: its advance, and where its image
// sits from its origin, y growing upward.
int32_t ATSUGlyphGetScreenMetrics(atsu_style* style, uint32_t count,
                                  const UInt16* glyphs, uint32_t stride,
                                  unsigned int forcing_anti_alias,
                                  unsigned int anti_alias_switch,
                                  ATSGlyphScreenMetrics* metrics) {
  style = style_of(style);
  if (!style) {
    return kATSUInvalidStyleErr;
  }
  if (!glyphs || !metrics) {
    return paramErr;
  }
  if (!stride) {
    stride = sizeof(UInt16);
  }
  int32_t err = noErr;
  hle_fonts_lock();
  for (uint32_t i = 0; i < count; i++) {
    UInt16 glyph = *(const UInt16*)((const uint8_t*)glyphs + i * stride);
    FT_GlyphSlot slot = load_glyph(style, glyph, 1, 0);
    ATSGlyphScreenMetrics* m = &metrics[i];
    memset(m, 0, sizeof(*m));
    if (!slot) {
      err = kATSUInvalidFontErr;
      continue;
    }
    float advance = slot->advance.x / 64.0f;
    m->deviceAdvance.x = advance;
    m->topLeft.x = slot->bitmap_left;
    m->topLeft.y = slot->bitmap_top;
    m->width = slot->bitmap.width;
    m->height = slot->bitmap.rows;
    m->sideBearing.x = slot->bitmap_left;
    m->otherSideBearing.x =
        advance - (slot->bitmap_left + (float)slot->bitmap.width);
  }
  hle_fonts_unlock();
  return err;
}

// ---------------------------------------------------------------------------
// Drawing

typedef struct {
  hle_port* port;
  int x;
  int y;
  const RGBColor* color;
} draw_context;

static void draw_glyph(void* context, FT_GlyphSlot slot, FT_Pos x) {
  draw_context* d = context;
  const FT_Bitmap* bitmap = &slot->bitmap;
  int left = d->x + (int)((x + 32) >> 6) + slot->bitmap_left;
  int top = d->y - slot->bitmap_top;
  int pitch = bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch;
  for (unsigned row = 0; row < bitmap->rows; row++) {
    const uint8_t* line = bitmap->buffer + row * pitch;
    for (unsigned col = 0; col < bitmap->width; col++) {
      int coverage;
      if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        coverage = line[col / 8] & (0x80 >> (col % 8)) ? 255 : 0;
      } else if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        coverage = line[col] * 255 / (bitmap->num_grays > 1
                                          ? bitmap->num_grays - 1
                                          : 255);
      } else {
        continue;
      }
      hle_port_blend(d->port, left + (int)col, top + (int)row, coverage,
                     d->color);
    }
  }
}

// Draws at (x, y) in the current port, or at its pen when both are
// kATSUUseGrafPortPenLoc; the pen stays where it is.
int32_t ATSUDrawText(atsu_layout* layout, uint32_t offset, uint32_t length,
                     Fixed x, Fixed y) {
  layout = layout_of(layout);
  if (!layout) {
    return kATSUInvalidTextLayoutErr;
  }
  uint32_t start, end;
  if (!layout->style || !line_range(layout, offset, length, &start, &end)) {
    return kATSUInvalidTextRangeErr;
  }
  hle_port* port = hle_port_current();
  draw_context d = { port, 0, 0, &layout->style->color };
  if (x == kATSUUseGrafPortPenLoc && y == kATSUUseGrafPortPenLoc) {
    d.x = port->pen.pnLoc.h;
    d.y = port->pen.pnLoc.v;
  } else {
    d.x = (x + 0x8000) >> 16;
    d.y = (y + 0x8000) >> 16;
  }
  int mono = port->pixmap && (*port->pixmap)->pixelSize == 1;
  hle_fonts_lock();
  for_each_glyph(layout, start, end, mono, draw_glyph, &d);
  hle_fonts_unlock();
  return noErr;
}
