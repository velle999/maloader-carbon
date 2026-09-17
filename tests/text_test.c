// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks fonts and ATSUI against libmac.so the way Age of Empires III draws
// a glyph: measure its image, get its screen metrics, draw it black on a
// white GWorld from its top left and read the pixels back.
//
//   make tests/text_test
//   tests/text_test FONT_FILE [SUITCASE_FOLDER]
//
// FONT_FILE is any TrueType or OpenType file. SUITCASE_FOLDER is a folder
// of Mac font suitcases, their resource forks in AppleDouble files, such as
// the game's Contents/Resources/Fonts.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle/carbon.h"
#include "../hle/gui.h"

typedef void* ATSUStyle;
typedef void* ATSUTextLayout;

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

typedef struct {
  uint32_t version;
  float ascent;
  float descent;
  float rest[12];
} ATSFontMetrics;

typedef struct {
  SInt16 ascent;
  SInt16 descent;
  SInt16 widMax;
  SInt16 leading;
} FontInfo;

int32_t ATSFontActivateFromMemory(const void*, uint32_t, uint32_t, uint32_t,
                                  void*, uint32_t, uint32_t*);
int32_t ATSFontFindFromContainer(uint32_t, uint32_t, uint32_t, uint32_t*,
                                 uint32_t*);
uint32_t ATSFontFindFromName(CFStringRef, uint32_t);
int32_t ATSFontGetHorizontalMetrics(uint32_t, uint32_t, ATSFontMetrics*);
int32_t FMActivateFonts(const FSSpec*, void*, void*, uint32_t);
uint32_t FMGetFontFromATSFontRef(uint32_t);
int32_t ATSUCreateStyle(ATSUStyle*);
int32_t ATSUSetAttributes(ATSUStyle, uint32_t, const uint32_t*,
                          const uint32_t*, const void* const*);
int32_t ATSUCreateTextLayout(ATSUTextLayout*);
int32_t ATSUSetTextPointerLocation(ATSUTextLayout, const UniChar*, uint32_t,
                                   uint32_t, uint32_t);
int32_t ATSUSetRunStyle(ATSUTextLayout, ATSUStyle, uint32_t, uint32_t);
int32_t ATSUMeasureTextImage(ATSUTextLayout, uint32_t, uint32_t, Fixed, Fixed,
                             Rect*);
int32_t ATSUDirectGetLayoutDataArrayPtrFromTextLayout(ATSUTextLayout, uint32_t,
                                                      uint32_t, void**,
                                                      uint32_t*);
int32_t ATSUGlyphGetScreenMetrics(ATSUStyle, uint32_t, const UInt16*, uint32_t,
                                  unsigned int, unsigned int,
                                  ATSGlyphScreenMetrics*);
int32_t ATSUDrawText(ATSUTextLayout, uint32_t, uint32_t, Fixed, Fixed);
int32_t NewGWorld(hle_port**, SInt16, const Rect*, Handle, GDHandle, UInt32);
void SetGWorld(hle_port*, GDHandle);
PixMapHandle GetGWorldPixMap(hle_port*);
Ptr GetPixBaseAddr(PixMapHandle);
int32_t GetPixRowBytes(PixMapHandle);
void EraseRect(const Rect*);
void MoveTo(SInt16, SInt16);
void GetFontInfo(FontInfo*);
int32_t TextWidth(const char*, int32_t, int32_t);
OSErr hle_path_to_spec(const char*, FSSpec*);

enum {
  kFromTextBeginning = 0xFFFFFFFFu,
  kToTextEnd = 0xFFFFFFFFu,
  kUsePenLoc = (Fixed)0xFFFFFFFF,
};

static int failures;

static void check(int ok, const char* what) {
  printf("%s - %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

static uint8_t* read_file(const char* path, uint32_t* length) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* data = malloc(size);
  if (fread(data, 1, size, f) != (size_t)size) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *length = size;
  return data;
}

// Draws |c| as the game does and checks its ink stays inside the box the
// metrics give. |depth| is the GWorld's: 32 or 1.
static void draw_glyph(uint32_t font, UniChar c, int size, int depth,
                       const char* name) {
  char what[160];
  ATSUStyle style;
  ATSUCreateStyle(&style);
  uint32_t fm_font = FMGetFontFromATSFontRef(font);
  Fixed fixed_size = size << 16;
  static const uint32_t tags[] = { 261, 262 };
  static const uint32_t sizes[] = { 4, 4 };
  const void* values[] = { &fm_font, &fixed_size };
  ATSUSetAttributes(style, 2, tags, sizes, values);
  ATSUTextLayout layout;
  ATSUCreateTextLayout(&layout);
  ATSUSetTextPointerLocation(layout, &c, kFromTextBeginning, 1, 1);
  ATSUSetRunStyle(layout, style, kFromTextBeginning, kToTextEnd);
  Rect image;
  check(ATSUMeasureTextImage(layout, kFromTextBeginning, 1, 0, 0, &image) ==
            noErr,
        "measure text image");
  void* records;
  uint32_t record_count;
  check(ATSUDirectGetLayoutDataArrayPtrFromTextLayout(
            layout, 0, 100, &records, &record_count) == noErr &&
            record_count == 2,
        "layout records: a glyph and the line end");
  ATSGlyphScreenMetrics m;
  check(ATSUGlyphGetScreenMetrics(style, 1, records, 0, 0, 0, &m) == noErr,
        "screen metrics");
  snprintf(what, sizeof(what), "%s '%c' advances %.1f", name, c,
           m.deviceAdvance.x);
  check(m.deviceAdvance.x > 0, what);
  check(image.right - image.left == (int)m.width &&
            image.bottom - image.top == (int)m.height,
        "image box and screen metrics agree");

  hle_port* world;
  Rect bounds = { 0, 0, 64, 64 };
  check(NewGWorld(&world, depth, &bounds, NULL, NULL, 0) == noErr,
        "NewGWorld");
  SetGWorld(world, NULL);
  EraseRect(&bounds);
  int x = (int)-m.topLeft.x;
  int y = (int)m.topLeft.y + 1;
  MoveTo(x, y);
  check(ATSUDrawText(layout, kFromTextBeginning, 1, kUsePenLoc, kUsePenLoc) ==
            noErr,
        "draw text");
  PixMapHandle pixmap = GetGWorldPixMap(world);
  uint8_t* base = (uint8_t*)GetPixBaseAddr(pixmap);
  int row_bytes = GetPixRowBytes(pixmap);
  int top = 64, left = 64, bottom = -1, right = -1, ink = 0;
  for (int row = 0; row < 32 && row < 64; row++) {
    char line[65];
    for (int col = 0; col < 64; col++) {
      int dark;
      if (depth == 1) {
        dark = (base[row * row_bytes + col / 8] >> (7 - col % 8)) & 1 ? 255
                                                                       : 0;
      } else {
        dark = 255 - base[row * row_bytes + col * 4 + 3];
      }
      if (dark >= 64) {
        ink++;
        top = row < top ? row : top;
        bottom = row > bottom ? row : bottom;
        left = col < left ? col : left;
        right = col > right ? col : right;
      }
      line[col] = dark >= 192 ? '#' : dark >= 64 ? '+' : '.';
    }
    line[40] = '\0';
    if (row <= (int)m.height + 2) {
      printf("  %s\n", line);
    }
  }
  snprintf(what, sizeof(what), "%d-bit ink: %d pixels", depth, ink);
  check(ink > 0, what);
  // Drawn from (-left bearing, top + 1), the image's top left is (0, 1).
  check(left >= 0 && top >= 1 && right < (int)m.width &&
            bottom <= (int)m.height,
        "ink inside the glyph's box");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s FONT_FILE [SUITCASE_FOLDER]\n", argv[0]);
    return 2;
  }
  uint32_t length;
  uint8_t* data = read_file(argv[1], &length);
  check(data != NULL, "read the font file");
  if (!data) {
    return 1;
  }
  uint32_t container = 0;
  check(ATSFontActivateFromMemory(data, length, 2, 0, NULL, 0, &container) ==
            noErr,
        "activate from memory");
  free(data);  // the fonts keep their own copy
  uint32_t font = 0, count = 0;
  check(ATSFontFindFromContainer(container, 0, 1, &font, &count) == noErr &&
            count >= 1 && font,
        "fonts in the container");
  ATSFontMetrics metrics;
  check(ATSFontGetHorizontalMetrics(font, 0, &metrics) == noErr &&
            metrics.ascent > 0 && metrics.descent < 0,
        "horizontal metrics in ems");
  draw_glyph(font, 'A', 24, 32, "file");
  draw_glyph(font, 'g', 12, 32, "file");
  draw_glyph(font, 'W', 14, 1, "file");

  CFStringRef helvetica =
      CFStringCreateWithCString(NULL, "Helvetica", kCFStringEncodingUTF8);
  check(ATSFontFindFromName(helvetica, 0) != 0,
        "a Mac system font name finds a local font");
  CFRelease(helvetica);
  FontInfo info;
  GetFontInfo(&info);
  check(info.ascent > 0 && info.descent > 0, "system font info");
  check(TextWidth("Hello", 0, 5) > 10, "system font text width");

  if (argc > 2) {
    FSSpec spec;
    check(hle_path_to_spec(argv[2], &spec) == noErr &&
              FMActivateFonts(&spec, NULL, NULL, 0) == noErr,
          "activate a folder of suitcases");
    CFStringRef arial_bold =
        CFStringCreateWithCString(NULL, "Arial Bold", kCFStringEncodingUTF8);
    uint32_t bold = ATSFontFindFromName(arial_bold, 0);
    check(bold != 0, "Arial Bold from its suitcase");
    CFRelease(arial_bold);
    if (bold) {
      draw_glyph(bold, 'R', 16, 32, "suitcase");
    }
  }
  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
