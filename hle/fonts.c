// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Fonts: the Font Manager's activation and lookups, ATS's font calls and
// QuickDraw's text measurement, over FreeType.
//
// Fonts come from the game: a folder of suitcases, whose faces are in
// resource forks, or font files it has in memory. FreeType reads a
// resource fork's 'sfnt' resources as it reads a file. A name the Mac
// system has fonts for (Lucida Grande, Helvetica, Times, Monaco, Georgia
// and the like) finds the closest font fontconfig knows, when nothing the
// game activated has that name.

#define _GNU_SOURCE

#include "fonts.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H

#include "carbon.h"
#include "gui.h"

typedef uint32_t ATSFontRef;
typedef uint32_t ATSFontFamilyRef;
typedef uint32_t ATSFontContainerRef;

enum {
  kATSInvalidFontAccess = -8812,
  kATSInvalidFontContainerAccess = -8813,
  kATSInvalidFontTableAccess = -8814,
  kFMInvalidFontFamilyErr = -981,
};

enum {
  kStyleBold = 1,
  kStyleItalic = 2,
};

typedef struct {
  FT_Face face;
  char* family;
  char* full_name;
  char* postscript_name;
  int style;
  ATSFontContainerRef container;
} font;

typedef struct {
  char* name;
} family;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FT_Library library;
static font* fonts;
static uint32_t font_count;
static family* families;
static uint32_t family_count;
static ATSFontContainerRef container_count;

void hle_fonts_lock(void) {
  pthread_mutex_lock(&lock);
}

void hle_fonts_unlock(void) {
  pthread_mutex_unlock(&lock);
}

static int library_ready(void) {
  if (!library && FT_Init_FreeType(&library)) {
    fprintf(stderr, "hle: FreeType did not start; no fonts\n");
    library = NULL;
  }
  return library != NULL;
}

FT_Face hle_font_face(uint32_t f) {
  return f >= 1 && f <= font_count ? fonts[f - 1].face : NULL;
}

int hle_font_style(uint32_t f) {
  hle_fonts_lock();
  int style = f >= 1 && f <= font_count ? fonts[f - 1].style : 0;
  hle_fonts_unlock();
  return style;
}

// ---------------------------------------------------------------------------
// Names

// A name from the face's name table as UTF-8: the Windows English one,
// else the Mac Roman one. NULL when it has neither.
static char* sfnt_name(FT_Face face, int name_id) {
  FT_UInt count = FT_Get_Sfnt_Name_Count(face);
  char* mac = NULL;
  for (FT_UInt i = 0; i < count; i++) {
    FT_SfntName name;
    if (FT_Get_Sfnt_Name(face, i, &name) || name.name_id != name_id) {
      continue;
    }
    if (name.platform_id == TT_PLATFORM_MICROSOFT &&
        (name.language_id & 0x3FF) == 0x09) {
      CFStringRef s = CFStringCreateWithBytes(
          NULL, name.string, name.string_len, kCFStringEncodingUTF16BE, 0);
      if (s) {
        char* utf8 = cf_string_utf8(s);
        CFRelease(s);
        free(mac);
        return utf8;
      }
    } else if (!mac && name.platform_id == TT_PLATFORM_MACINTOSH &&
               name.encoding_id == TT_MAC_ID_ROMAN &&
               name.language_id == TT_MAC_LANGID_ENGLISH) {
      CFStringRef s = CFStringCreateWithBytes(
          NULL, name.string, name.string_len, kCFStringEncodingMacRoman, 0);
      if (s) {
        mac = cf_string_utf8(s);
        CFRelease(s);
      }
    }
  }
  return mac;
}

static uint32_t family_named(const char* name, int create) {
  for (uint32_t i = 0; i < family_count; i++) {
    if (!strcasecmp(families[i].name, name)) {
      return i + 1;
    }
  }
  if (!create) {
    return 0;
  }
  families = realloc(families, sizeof(*families) * (family_count + 1));
  families[family_count].name = strdup(name);
  return ++family_count;
}

// Adds every face in |data|, which the fonts keep. Returns how many.
static int add_faces(const uint8_t* data, size_t length,
                     ATSFontContainerRef container, const char* source) {
  if (!library_ready()) {
    return 0;
  }
  int added = 0;
  long faces = 1;
  for (long i = 0; i < faces; i++) {
    FT_Face face;
    if (FT_New_Memory_Face(library, data, (FT_Long)length, i, &face)) {
      continue;
    }
    faces = face->num_faces;
    font f = { .face = face, .container = container };
    f.family = sfnt_name(face, TT_NAME_ID_FONT_FAMILY);
    if (!f.family) {
      f.family = strdup(face->family_name ? face->family_name : "");
    }
    f.full_name = sfnt_name(face, TT_NAME_ID_FULL_NAME);
    const char* postscript = FT_Get_Postscript_Name(face);
    f.postscript_name = postscript ? strdup(postscript) : NULL;
    f.style = ((face->style_flags & FT_STYLE_FLAG_BOLD) ? kStyleBold : 0) |
              ((face->style_flags & FT_STYLE_FLAG_ITALIC) ? kStyleItalic : 0);
    family_named(f.family, 1);
    fonts = realloc(fonts, sizeof(*fonts) * (font_count + 1));
    fonts[font_count++] = f;
    added++;
    cf_trace("font %u: %s (%s) from %s", font_count,
             f.full_name ? f.full_name : f.family, f.family, source);
  }
  return added;
}

// ---------------------------------------------------------------------------
// Mac system fonts, from fontconfig

static const struct {
  const char* mac_name;
  const char* pattern;
} kSystemFonts[] = {
  { "Lucida Grande", "sans-serif" },
  { "Geneva", "sans-serif" },
  { "Helvetica", "Helvetica,sans-serif" },
  { "Helvetica Neue", "Helvetica,sans-serif" },
  { "Arial", "Arial,sans-serif" },
  { "Verdana", "Verdana,sans-serif" },
  { "Tahoma", "Tahoma,sans-serif" },
  { "Times", "Times,serif" },
  { "Times New Roman", "Times New Roman,serif" },
  { "Georgia", "Georgia,serif" },
  { "Palatino", "Palatino,serif" },
  { "New York", "serif" },
  { "Monaco", "monospace" },
  { "Courier", "Courier,monospace" },
  { "Courier New", "Courier New,monospace" },
};

typedef void FcPattern;
typedef void FcConfig;

// The file fontconfig picks for |pattern|, malloc'd, or NULL.
static char* fontconfig_file(const char* pattern) {
  static void* fc;
  static int tried;
  if (!tried) {
    tried = 1;
    fc = dlopen("libfontconfig.so.1", RTLD_LAZY | RTLD_LOCAL);
  }
  if (!fc) {
    return NULL;
  }
  FcConfig* (*init)(void) = dlsym(fc, "FcInitLoadConfigAndFonts");
  FcPattern* (*parse)(const uint8_t*) = dlsym(fc, "FcNameParse");
  int (*substitute)(FcConfig*, FcPattern*, int) =
      dlsym(fc, "FcConfigSubstitute");
  void (*defaults)(FcPattern*) = dlsym(fc, "FcDefaultSubstitute");
  FcPattern* (*match)(FcConfig*, FcPattern*, int*) = dlsym(fc, "FcFontMatch");
  int (*get_string)(const FcPattern*, const char*, int, uint8_t**) =
      dlsym(fc, "FcPatternGetString");
  void (*destroy)(FcPattern*) = dlsym(fc, "FcPatternDestroy");
  if (!init || !parse || !substitute || !defaults || !match || !get_string ||
      !destroy) {
    return NULL;
  }
  static FcConfig* config;
  if (!config) {
    config = init();
  }
  FcPattern* request = parse((const uint8_t*)pattern);
  if (!config || !request) {
    return NULL;
  }
  substitute(config, request, 0);  // FcMatchPattern
  defaults(request);
  int result;
  FcPattern* found = match(config, request, &result);
  destroy(request);
  char* path = NULL;
  uint8_t* file;
  if (found && get_string(found, "file", 0, &file) == 0) {  // FcResultMatch
    path = strdup((const char*)file);
  }
  if (found) {
    destroy(found);
  }
  return path;
}

static uint8_t* read_file(const char* path, size_t* length) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* data = size > 0 ? malloc(size) : NULL;
  if (data && fread(data, 1, size, f) != (size_t)size) {
    free(data);
    data = NULL;
  }
  fclose(f);
  *length = data ? (size_t)size : 0;
  return data;
}

// Registers the system font a Mac would have under |name|, with that name
// as its family. 0 when |name| is not one or fontconfig has nothing.
static uint32_t add_system_font(const char* name) {
  for (size_t i = 0; i < sizeof(kSystemFonts) / sizeof(kSystemFonts[0]); i++) {
    if (strcasecmp(kSystemFonts[i].mac_name, name)) {
      continue;
    }
    char* path = fontconfig_file(kSystemFonts[i].pattern);
    size_t length;
    uint8_t* data = path ? read_file(path, &length) : NULL;
    if (!data || !library_ready()) {
      free(path);
      free(data);
      return 0;
    }
    FT_Face face;
    if (FT_New_Memory_Face(library, data, (FT_Long)length, 0, &face)) {
      free(path);
      free(data);
      return 0;
    }
    font f = { .face = face };
    f.family = strdup(kSystemFonts[i].mac_name);
    f.full_name = strdup(kSystemFonts[i].mac_name);
    f.style = 0;
    family_named(f.family, 1);
    fonts = realloc(fonts, sizeof(*fonts) * (font_count + 1));
    fonts[font_count++] = f;
    fprintf(stderr, "hle: font %s is %s\n", kSystemFonts[i].mac_name, path);
    free(path);
    return font_count;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Activation

typedef struct {
  uint32_t flags;
  uint32_t filter_type;
  void* selector;
} FMFilter;

// Every font in the folder |container| names, or in the file it names:
// data forks that hold fonts, and suitcases' resource forks.
int FMActivateFonts(const FSSpec* container, const FMFilter* filter,
                    void* refcon, uint32_t options) {
  char path[PATH_MAX];
  if (!container || hle_spec_to_path(container, path, sizeof(path)) != noErr) {
    return paramErr;
  }
  char resolved[PATH_MAX];
  if (hle_path_resolve(path, resolved, sizeof(resolved))) {
    snprintf(path, sizeof(path), "%s", resolved);
  }
  DIR* dir = opendir(path);
  char** files = NULL;
  int file_count = 0;
  if (dir) {
    struct dirent* e;
    while ((e = readdir(dir))) {
      if (e->d_name[0] == '.') {
        continue;
      }
      files = realloc(files, sizeof(*files) * (file_count + 1));
      if (asprintf(&files[file_count], "%s/%s", path, e->d_name) >= 0) {
        file_count++;
      }
    }
    closedir(dir);
  } else {
    files = malloc(sizeof(*files));
    files[file_count++] = strdup(path);
  }
  hle_fonts_lock();
  ATSFontContainerRef ref = ++container_count;
  int added = 0;
  for (int i = 0; i < file_count; i++) {
    size_t length;
    uint8_t* data = read_file(files[i], &length);
    if (data) {
      int n = add_faces(data, length, ref, files[i]);
      if (n) {
        added += n;
      } else {
        free(data);
      }
    }
    uint8_t* fork;
    uint32_t fork_length;
    if (hle_resource_fork_load(files[i], &fork, &fork_length) == noErr) {
      int n = add_faces(fork, fork_length, ref, files[i]);
      if (n) {
        added += n;
      } else {
        free(fork);
      }
    }
    free(files[i]);
  }
  hle_fonts_unlock();
  free(files);
  cf_trace("FMActivateFonts(%s): %d fonts", path, added);
  return noErr;
}

// Activated fonts stay: nothing here holds them open.
int FMDeactivateFonts(const FSSpec* container, const FMFilter* filter,
                      void* refcon, uint32_t options) {
  return noErr;
}

// Keeps a copy of |data|, which the caller may free.
int32_t ATSFontActivateFromMemory(const void* data, uint32_t length,
                                  uint32_t context, uint32_t format,
                                  void* reserved, uint32_t options,
                                  ATSFontContainerRef* container) {
  if (!data || !length) {
    return paramErr;
  }
  uint8_t* copy = malloc(length);
  memcpy(copy, data, length);
  hle_fonts_lock();
  ATSFontContainerRef ref = ++container_count;
  int added = add_faces(copy, length, ref, "memory");
  hle_fonts_unlock();
  if (!added) {
    free(copy);
    return kATSInvalidFontContainerAccess;
  }
  if (container) {
    *container = ref;
  }
  return noErr;
}

int32_t ATSFontFindFromContainer(ATSFontContainerRef container,
                                 uint32_t options, uint32_t count,
                                 ATSFontRef* found, uint32_t* found_count) {
  hle_fonts_lock();
  uint32_t n = 0;
  for (uint32_t i = 0; i < font_count; i++) {
    if (fonts[i].container != container) {
      continue;
    }
    if (found && n < count) {
      found[n] = i + 1;
    }
    n++;
  }
  hle_fonts_unlock();
  if (found_count) {
    *found_count = n;
  }
  return n ? noErr : kATSInvalidFontContainerAccess;
}

// ---------------------------------------------------------------------------
// Lookups

// The font whose full or PostScript name is |name|.
ATSFontRef ATSFontFindFromName(CFStringRef name, uint32_t options) {
  if (!name) {
    return 0;
  }
  char* utf8 = cf_string_utf8(name);
  hle_fonts_lock();
  ATSFontRef found = 0;
  for (uint32_t i = 0; i < font_count && !found; i++) {
    if ((fonts[i].full_name && !strcasecmp(fonts[i].full_name, utf8)) ||
        (fonts[i].postscript_name &&
         !strcasecmp(fonts[i].postscript_name, utf8))) {
      found = i + 1;
    }
  }
  if (!found) {
    found = add_system_font(utf8);
  }
  hle_fonts_unlock();
  cf_trace("ATSFontFindFromName(%s) = %u", utf8, found);
  free(utf8);
  return found;
}

ATSFontFamilyRef ATSFontFamilyFindFromName(CFStringRef name,
                                           uint32_t options) {
  if (!name) {
    return 0;
  }
  char* utf8 = cf_string_utf8(name);
  hle_fonts_lock();
  ATSFontFamilyRef found = family_named(utf8, 0);
  if (!found && add_system_font(utf8)) {
    found = family_named(utf8, 0);
  }
  hle_fonts_unlock();
  cf_trace("ATSFontFamilyFindFromName(%s) = %u", utf8, found);
  free(utf8);
  return found;
}

// An FMFontFamily is a short; the result fills EAX.
int32_t FMGetFontFamilyFromATSFontFamilyRef(ATSFontFamilyRef family) {
  return (int16_t)family;
}

ATSFontRef FMGetATSFontRefFromFont(uint32_t font) {
  return font;
}

uint32_t FMGetFontFromATSFontRef(ATSFontRef font) {
  return font;
}

uint32_t hle_font_for_family(int family_number, int style) {
  hle_fonts_lock();
  const char* name = NULL;
  if (family_number >= 1 && (uint32_t)family_number <= family_count) {
    name = families[family_number - 1].name;
  }
  uint32_t best = 0;
  int best_score = -1;
  for (uint32_t i = 0; name && i < font_count; i++) {
    if (strcasecmp(fonts[i].family, name)) {
      continue;
    }
    // Every style bit that matches counts.
    int score = 2 - __builtin_popcount((fonts[i].style ^ style) & 3);
    if (score > best_score) {
      best = i + 1;
      best_score = score;
    }
  }
  hle_fonts_unlock();
  return best;
}

int32_t FMGetFontFromFontFamilyInstance(int32_t family_number, int32_t style,
                                        uint32_t* font_out,
                                        int32_t* intrinsic_style) {
  uint32_t f = hle_font_for_family((int16_t)family_number, style & 3);
  if (!f) {
    return kFMInvalidFontFamilyErr;
  }
  if (font_out) {
    *font_out = f;
  }
  if (intrinsic_style) {
    *intrinsic_style = hle_font_style(f);
  }
  return noErr;
}

// ---------------------------------------------------------------------------
// Font data

typedef struct {
  uint32_t version;
  float ascent;
  float descent;
  float leading;
  float avgAdvanceWidth;
  float maxAdvanceWidth;
  float minLeftSideBearing;
  float minRightSideBearing;
  float stemWidth;
  float stemHeight;
  float capHeight;
  float xHeight;
  float italicAngle;
  float underlinePosition;
  float underlineThickness;
} ATSFontMetrics;

// Metrics in ems, from the font's own tables.
int32_t ATSFontGetHorizontalMetrics(ATSFontRef f, uint32_t options,
                                    ATSFontMetrics* metrics) {
  if (!metrics) {
    return paramErr;
  }
  hle_fonts_lock();
  FT_Face face = hle_font_face(f);
  if (!face || !face->units_per_EM) {
    hle_fonts_unlock();
    return kATSInvalidFontAccess;
  }
  float em = face->units_per_EM;
  memset(metrics, 0, sizeof(*metrics));
  metrics->version = 0x00010000;
  metrics->ascent = face->ascender / em;
  metrics->descent = face->descender / em;
  metrics->leading = (face->height - face->ascender + face->descender) / em;
  metrics->maxAdvanceWidth = face->max_advance_width / em;
  metrics->underlinePosition = face->underline_position / em;
  metrics->underlineThickness = face->underline_thickness / em;
  TT_HoriHeader* hhea = FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);
  if (hhea) {
    metrics->minLeftSideBearing = hhea->min_Left_Side_Bearing / em;
    metrics->minRightSideBearing = hhea->min_Right_Side_Bearing / em;
  }
  TT_OS2* os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
  if (os2 && os2->version != 0xFFFF) {
    metrics->avgAdvanceWidth = os2->xAvgCharWidth / em;
    metrics->capHeight = os2->sCapHeight / em;
    metrics->xHeight = os2->sxHeight / em;
  } else {
    metrics->avgAdvanceWidth = metrics->maxAdvanceWidth / 2;
  }
  TT_Postscript* post = FT_Get_Sfnt_Table(face, FT_SFNT_POST);
  if (post) {
    metrics->italicAngle = post->italicAngle / 65536.0f;
  }
  hle_fonts_unlock();
  return noErr;
}

// Copies up to |max_size| bytes of the table from |offset|; the table's
// whole size when |buffer| is NULL.
int32_t ATSFontGetTable(ATSFontRef f, uint32_t tag, uint32_t offset,
                        uint32_t max_size, void* buffer, uint32_t* size) {
  hle_fonts_lock();
  FT_Face face = hle_font_face(f);
  if (!face) {
    hle_fonts_unlock();
    return kATSInvalidFontAccess;
  }
  FT_ULong length = 0;
  int32_t err = noErr;
  if (FT_Load_Sfnt_Table(face, tag, 0, NULL, &length)) {
    err = kATSInvalidFontTableAccess;
  } else if (buffer) {
    FT_ULong want = offset < length ? length - offset : 0;
    if (want > max_size) {
      want = max_size;
    }
    if (want && FT_Load_Sfnt_Table(face, tag, offset, buffer, &want)) {
      err = kATSInvalidFontTableAccess;
    }
    length = want;
  }
  hle_fonts_unlock();
  if (size) {
    *size = err == noErr ? (uint32_t)length : 0;
  }
  return err;
}

// ---------------------------------------------------------------------------
// QuickDraw text

// Family numbers the system reserves: systemFont and applFont.
static const char* reserved_family_name(int number) {
  return number == 0 ? "Lucida Grande" : number == 1 ? "Geneva" : NULL;
}

void GetFontName(int32_t family_number, unsigned char* name) {
  family_number = (int16_t)family_number;
  name[0] = 0;
  const char* reserved = reserved_family_name(family_number);
  hle_fonts_lock();
  const char* found = reserved;
  if (!found && family_number >= 1 && (uint32_t)family_number <= family_count) {
    found = families[family_number - 1].name;
  }
  if (found) {
    hle_name_to_pascal(found, name, 256);
  }
  hle_fonts_unlock();
}

// The font and pixel size the current port draws text in.
static FT_Face port_face(int* pixels) {
  hle_port* port = hle_port_current();
  *pixels = port->text_size > 0 ? port->text_size : 12;
  const char* reserved = reserved_family_name(port->text_font);
  uint32_t f = 0;
  if (reserved) {
    hle_fonts_lock();
    uint32_t family_number = family_named(reserved, 0);
    if (!family_number && add_system_font(reserved)) {
      family_number = family_named(reserved, 0);
    }
    hle_fonts_unlock();
    f = family_number ? hle_font_for_family(family_number, port->text_face & 3)
                      : 0;
  } else {
    f = hle_font_for_family(port->text_font, port->text_face & 3);
  }
  hle_fonts_lock();
  FT_Face face = hle_font_face(f);
  if (face && FT_Set_Pixel_Sizes(face, 0, *pixels)) {
    face = NULL;
  }
  if (!face) {
    hle_fonts_unlock();
  }
  return face;
}

typedef struct {
  SInt16 ascent;
  SInt16 descent;
  SInt16 widMax;
  SInt16 leading;
} FontInfo;

void GetFontInfo(FontInfo* info) {
  int pixels;
  FT_Face face = port_face(&pixels);
  if (!face) {
    info->ascent = pixels * 3 / 4;
    info->descent = pixels / 4;
    info->widMax = pixels;
    info->leading = 0;
    return;
  }
  FT_Size_Metrics* m = &face->size->metrics;
  info->ascent = (SInt16)((m->ascender + 63) >> 6);
  info->descent = (SInt16)((-m->descender + 63) >> 6);
  info->widMax = (SInt16)((m->max_advance + 63) >> 6);
  int leading = (int)((m->height - m->ascender + m->descender) >> 6);
  info->leading = (SInt16)(leading > 0 ? leading : 0);
  hle_fonts_unlock();
}

// The width of Mac Roman text in the current port's font.
int32_t TextWidth(const char* text, int32_t first, int32_t count) {
  first = (int16_t)first;
  count = (int16_t)count;
  if (!text || count <= 0) {
    return 0;
  }
  CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8*)text + first,
                                          count, kCFStringEncodingMacRoman, 0);
  int pixels;
  FT_Face face = port_face(&pixels);
  int32_t width = 0;
  CFIndex n = s ? CFStringGetLength(s) : 0;
  if (!face) {
    width = n * pixels / 2;
  } else {
    FT_Pos advance = 0;
    for (CFIndex i = 0; i < n; i++) {
      FT_UInt glyph = FT_Get_Char_Index(face, cf_string_char(s, i));
      if (!FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT)) {
        advance += face->glyph->advance.x;
      }
    }
    width = (int32_t)((advance + 32) >> 6);
    hle_fonts_unlock();
  }
  if (s) {
    CFRelease(s);
  }
  return (int16_t)width;
}
