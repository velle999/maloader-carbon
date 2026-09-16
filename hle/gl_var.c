// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// GL_APPLE_vertex_array_range, GL_APPLE_fence and GL_APPLE_vertex_array_object
// over ARB vertex buffer objects. HLE_VAR=0 leaves them out.
//
// Halo's Direct3D layer gives each vertex buffer a vertex array object and a
// vertex array range over the buffer's memory, flushes the bytes a lock
// changed, and draws with array pointers into that memory, but only when
// GL_EXTENSIONS names both GL_APPLE_vertex_array_range and GL_APPLE_fence.
// Otherwise it draws with client arrays, which NVIDIA's driver copies on the
// CPU for every draw.
//
// A vertex array object holds its client arrays, which are enabled and where
// they point, and the game counts on that: it enables only the arrays a
// buffer has. Binding an object here applies its arrays to GL, disabling
// those the last one had and it lacks. What GL has is mirrored, so that only
// the arrays that differ are set.
//
// A range's storage hint says what a flush means. GL_STORAGE_CACHED_APPLE
// memory is copied to the GPU as it is flushed, and the game flushes all it
// writes there, so such a range is uploaded to buffer objects: one for each
// span a flush names, as the game keeps many buffers in one range, and a
// buffer object written while the GPU reads it makes the driver wait. A span
// flushed whole gets a new store, which nothing waits for. With the default,
// GL_STORAGE_CLIENT_APPLE, or GL_STORAGE_SHARED_APPLE, the GPU reads the
// game's memory itself, the game does not flush all it writes, and effects
// drawn from buffer objects came out stale, smeared through walls; those
// ranges stay client arrays. An array pointer into a flushed span of a
// cached range with GL_VERTEX_ARRAY_RANGE_APPLE enabled becomes an offset
// into the span's buffer object. Fences are always finished, as the GPU
// never reads the game's memory.
//
// A buffer object whose span goes is kept, emptied, for a later span: an
// array still pointing into a deleted one would become a client pointer to a
// small address, and a draw with it enabled would fault. The game makes its
// GL calls on one thread, in contexts that share objects.

#define _GNU_SOURCE

#include "gl_var.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "var_ranges.h"

void cf_trace(const char* fmt, ...);

enum {
  GL_NO_ERROR = 0,
  GL_VERTEX_ARRAY = 0x8074,
  GL_NORMAL_ARRAY = 0x8075,
  GL_COLOR_ARRAY = 0x8076,
  GL_TEXTURE_COORD_ARRAY = 0x8078,
  GL_EDGE_FLAG_ARRAY = 0x8079,
  GL_FOG_COORD_ARRAY = 0x8457,
  GL_SECONDARY_COLOR_ARRAY = 0x845E,
  GL_TEXTURE0 = 0x84C0,
  GL_VERTEX_ARRAY_RANGE_APPLE = 0x851D,
  GL_VERTEX_ARRAY_STORAGE_HINT_APPLE = 0x851F,
  GL_STORAGE_CLIENT_APPLE = 0x85B4,
  GL_STORAGE_CACHED_APPLE = 0x85BE,
  GL_ARRAY_BUFFER_ARB = 0x8892,
  GL_STATIC_DRAW_ARB = 0x88E4,
};

// The client arrays mirrored: the fixed ones, texture coordinates for each
// client texture unit, and vertex attributes. Others pass straight to GL.
enum {
  kTextureUnits = 8,
  kAttributes = 16,
  kVertex = 0,
  kNormal,
  kColor,
  kSecondaryColor,
  kFogCoord,
  kEdgeFlag,
  kTexCoord,                             // + the client texture unit
  kAttribute = kTexCoord + kTextureUnits,  // + the attribute's index
  kArrays = kAttribute + kAttributes,
};

typedef struct {
  uint8_t enabled;
  uint8_t set;  // given a pointer
  uint8_t normalized;
  int32_t size;
  uint32_t type;
  int32_t stride;
  const void* pointer;
} client_array;

// An array as GL has it: |pointer| is an offset when |buffer| is not 0.
typedef struct {
  uint8_t enable_known;
  uint8_t pointer_known;
  client_array array;
  uint32_t buffer;
} gl_array;

typedef struct {
  int named;          // generated, and not deleted
  int range_enabled;  // GL_VERTEX_ARRAY_RANGE_APPLE
  uint32_t hint;      // GL_VERTEX_ARRAY_STORAGE_HINT_APPLE
  uintptr_t base;     // its range, when length is not 0
  size_t length;
  client_array arrays[kArrays];
} var_object;

typedef struct {
  uintptr_t base;
  size_t length;
  uint32_t buffer;
} var_span;

// Vertex array objects by name; 0 is the default object, always there.
static var_object* objects;
static uint32_t object_slots;
static uint32_t next_name = 1;
static uint32_t* free_names;
static uint32_t free_name_count;
static uint32_t free_name_capacity;
static uint32_t bound;
static hle_var_ranges ranges;  // each range's value is its object's name

// Flushed spans by id; 0 is never used.
static var_span* spans;
static uint32_t span_slots;
static uint32_t next_span = 1;
static uint32_t* free_spans;
static uint32_t free_span_count;
static uint32_t free_span_capacity;
static hle_var_ranges span_map;  // each span's value is its id

static uint32_t* spare_buffers;
static uint32_t spare_count;
static uint32_t spare_capacity;

static gl_array driver[kArrays];
static uint32_t client_unit;  // as the game set it
static uint32_t driver_unit;
static int driver_unit_known;
static uint32_t array_buffer;
static int array_buffer_known;
static int enabled = -1;
static unsigned spans_made;

static struct {
  void (*bind_buffer)(uint32_t, uint32_t);
  void (*gen_buffers)(int32_t, uint32_t*);
  void (*buffer_data)(uint32_t, intptr_t, const void*, uint32_t);
  void (*buffer_sub_data)(uint32_t, intptr_t, intptr_t, const void*);
  uint32_t (*get_error)(void);
  void (*client_active_texture)(uint32_t);
  void (*enable_client_state)(uint32_t);
  void (*disable_client_state)(uint32_t);
  void (*enable_vertex_attrib_array)(uint32_t);
  void (*disable_vertex_attrib_array)(uint32_t);
  void (*vertex_pointer)(int32_t, uint32_t, int32_t, const void*);
  void (*normal_pointer)(uint32_t, int32_t, const void*);
  void (*color_pointer)(int32_t, uint32_t, int32_t, const void*);
  void (*secondary_color_pointer)(int32_t, uint32_t, int32_t, const void*);
  void (*fog_coord_pointer)(uint32_t, int32_t, const void*);
  void (*edge_flag_pointer)(int32_t, const void*);
  void (*tex_coord_pointer)(int32_t, uint32_t, int32_t, const void*);
  void (*vertex_attrib_pointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t,
                                const void*);
} gl;

int hle_gl_var_enabled(void) {
  if (enabled < 0) {
    const char* setting = getenv("HLE_VAR");
    gl.bind_buffer = dlsym(RTLD_DEFAULT, "glBindBufferARB");
    gl.gen_buffers = dlsym(RTLD_DEFAULT, "glGenBuffersARB");
    gl.buffer_data = dlsym(RTLD_DEFAULT, "glBufferDataARB");
    gl.buffer_sub_data = dlsym(RTLD_DEFAULT, "glBufferSubDataARB");
    gl.get_error = dlsym(RTLD_DEFAULT, "glGetError");
    enabled = !(setting && strcmp(setting, "0") == 0) && gl.bind_buffer &&
              gl.gen_buffers && gl.buffer_data && gl.buffer_sub_data &&
              gl.get_error;
  }
  return enabled;
}

void hle_gl_var_context_changed(void) {
  memset(driver, 0, sizeof(driver));
  driver_unit_known = 0;
  array_buffer_known = 0;
}

// Makes |*array|, of |*capacity| elements of |element| bytes, hold |wanted|,
// the new elements zeroed.
static int grow(void* array, uint32_t* capacity, uint32_t wanted,
                size_t element) {
  if (wanted <= *capacity) {
    return 1;
  }
  uint32_t n = *capacity ? *capacity : 64;
  while (n < wanted) {
    n *= 2;
  }
  void** p = array;
  void* grown = realloc(*p, n * element);
  if (!grown) {
    return 0;
  }
  memset((char*)grown + *capacity * element, 0, (n - *capacity) * element);
  *p = grown;
  *capacity = n;
  return 1;
}

static int push(uint32_t** stack, uint32_t* count, uint32_t* capacity,
                uint32_t value) {
  if (!grow(stack, capacity, *count + 1, sizeof(uint32_t))) {
    return 0;
  }
  (*stack)[(*count)++] = value;
  return 1;
}

static var_object* object_of(uint32_t name) {
  if (!object_slots) {
    if (!grow(&objects, &object_slots, 1, sizeof(*objects))) {
      return NULL;
    }
    objects[0].hint = GL_STORAGE_CLIENT_APPLE;
  }
  if (name == 0) {
    return &objects[0];
  }
  return name < object_slots && objects[name].named ? &objects[name] : NULL;
}

// ---------------------------------------------------------------------------
// What GL has

static void bind_array_buffer(uint32_t buffer) {
  if (!array_buffer_known || array_buffer != buffer) {
    gl.bind_buffer(GL_ARRAY_BUFFER_ARB, buffer);
    array_buffer = buffer;
    array_buffer_known = 1;
  }
}

static void set_driver_unit(uint32_t unit) {
  if (!driver_unit_known || driver_unit != unit) {
    gl.client_active_texture(GL_TEXTURE0 + unit);
    driver_unit = unit;
    driver_unit_known = 1;
  }
}

// Where GL is to read an array the game points at |pointer|.
static const void* translate(const void* pointer, uint32_t* buffer) {
  *buffer = 0;
  if (!pointer) {
    return pointer;
  }
  const hle_var_range* range =
      hle_var_ranges_find(&ranges, (uintptr_t)pointer);
  if (!range || !objects[range->value].range_enabled ||
      objects[range->value].hint != GL_STORAGE_CACHED_APPLE) {
    return pointer;
  }
  const hle_var_range* span =
      hle_var_ranges_find(&span_map, (uintptr_t)pointer);
  if (!span) {
    return pointer;
  }
  *buffer = spans[span->value].buffer;
  return (const void*)((uintptr_t)pointer - span->base);
}

static int pointer_function_exists(int slot) {
  switch (slot) {
    case kVertex: return gl.vertex_pointer != NULL;
    case kNormal: return gl.normal_pointer != NULL;
    case kColor: return gl.color_pointer != NULL;
    case kSecondaryColor: return gl.secondary_color_pointer != NULL;
    case kFogCoord: return gl.fog_coord_pointer != NULL;
    case kEdgeFlag: return gl.edge_flag_pointer != NULL;
  }
  return slot < kAttribute ? gl.tex_coord_pointer != NULL
                           : gl.vertex_attrib_pointer != NULL;
}

// Gives GL |want| for |slot|, where GL does not have it already.
static void apply(int slot, const client_array* want) {
  gl_array* have = &driver[slot];
  int unit = slot >= kTexCoord && slot < kAttribute ? slot - kTexCoord : -1;
  if (want->set && pointer_function_exists(slot)) {
    uint32_t buffer;
    const void* pointer = translate(want->pointer, &buffer);
    if (!have->pointer_known || !have->array.set ||
        have->array.pointer != pointer || have->buffer != buffer ||
        have->array.size != want->size || have->array.type != want->type ||
        have->array.stride != want->stride ||
        have->array.normalized != want->normalized) {
      bind_array_buffer(buffer);
      if (unit >= 0) {
        set_driver_unit(unit);
      }
      switch (slot) {
        case kVertex:
          gl.vertex_pointer(want->size, want->type, want->stride, pointer);
          break;
        case kNormal:
          gl.normal_pointer(want->type, want->stride, pointer);
          break;
        case kColor:
          gl.color_pointer(want->size, want->type, want->stride, pointer);
          break;
        case kSecondaryColor:
          gl.secondary_color_pointer(want->size, want->type, want->stride,
                                     pointer);
          break;
        case kFogCoord:
          gl.fog_coord_pointer(want->type, want->stride, pointer);
          break;
        case kEdgeFlag:
          gl.edge_flag_pointer(want->stride, pointer);
          break;
        default:
          if (unit >= 0) {
            gl.tex_coord_pointer(want->size, want->type, want->stride,
                                 pointer);
          } else {
            gl.vertex_attrib_pointer(slot - kAttribute, want->size,
                                     want->type, want->normalized,
                                     want->stride, pointer);
          }
          break;
      }
      have->array.set = 1;
      have->array.size = want->size;
      have->array.type = want->type;
      have->array.stride = want->stride;
      have->array.normalized = want->normalized;
      have->array.pointer = pointer;
      have->buffer = buffer;
      have->pointer_known = 1;
    }
  }
  if (!have->enable_known || have->array.enabled != want->enabled) {
    static const uint32_t kFixed[kTexCoord] = {
      GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY,
      GL_SECONDARY_COLOR_ARRAY, GL_FOG_COORD_ARRAY, GL_EDGE_FLAG_ARRAY,
    };
    if (slot >= kAttribute) {
      if (want->enabled && gl.enable_vertex_attrib_array) {
        gl.enable_vertex_attrib_array(slot - kAttribute);
      } else if (!want->enabled && gl.disable_vertex_attrib_array) {
        gl.disable_vertex_attrib_array(slot - kAttribute);
      }
    } else {
      if (unit >= 0) {
        set_driver_unit(unit);
      }
      uint32_t array = unit >= 0 ? GL_TEXTURE_COORD_ARRAY : kFixed[slot];
      if (want->enabled) {
        gl.enable_client_state(array);
      } else {
        gl.disable_client_state(array);
      }
    }
    have->array.enabled = want->enabled;
    have->enable_known = 1;
  }
}

// Gives GL the bound object's arrays, as after a bind or when where they
// point has changed.
static void apply_bound(void) {
  var_object* o = object_of(bound);
  if (!o) {
    return;
  }
  for (int slot = 0; slot < kArrays; slot++) {
    apply(slot, &o->arrays[slot]);
  }
  if (client_unit < kTextureUnits) {
    set_driver_unit(client_unit);
  }
}

// ---------------------------------------------------------------------------
// Spans and ranges

static void spare(uint32_t buffer) {
  if (push(&spare_buffers, &spare_count, &spare_capacity, buffer)) {
    // Emptied, it holds no memory, and an array still pointing into it has
    // nothing the CPU could fault on.
    bind_array_buffer(buffer);
    gl.buffer_data(GL_ARRAY_BUFFER_ARB, 1, NULL, GL_STATIC_DRAW_ARB);
  }
}

static uint32_t take_buffer(void) {
  if (spare_count) {
    return spare_buffers[--spare_count];
  }
  uint32_t buffer = 0;
  gl.gen_buffers(1, &buffer);
  return buffer;
}

static void forget_span(uint32_t id) {
  if (spans[id].buffer) {
    spare(spans[id].buffer);
  }
  memset(&spans[id], 0, sizeof(spans[id]));
  push(&free_spans, &free_span_count, &free_span_capacity, id);
}

// Drops the spans overlapping [base, base + length) and returns how far they
// reached: the lowest base in |*low| and the highest end in |*high|, which
// are left alone when there are none.
static void drop_spans(uintptr_t base, size_t length, uintptr_t* low,
                       uintptr_t* high) {
  uint32_t ids[64];
  size_t taken =
      hle_var_ranges_take_overlapping(&span_map, base, length, ids, 64);
  for (size_t i = 0; i < taken && i < 64; i++) {
    const var_span* s = &spans[ids[i]];
    if (s->base < *low) {
      *low = s->base;
    }
    if (s->base + s->length > *high) {
      *high = s->base + s->length;
    }
    forget_span(ids[i]);
  }
  for (uint32_t id = 1; taken > 64 && id < next_span; id++) {
    const var_span* s = &spans[id];
    if (s->length && s->base < base + length && s->base + s->length > base) {
      if (s->base < *low) {
        *low = s->base;
      }
      if (s->base + s->length > *high) {
        *high = s->base + s->length;
      }
      forget_span(id);
    }
  }
}

static void drop_spans_of(var_object* o) {
  uintptr_t low = UINTPTR_MAX;
  uintptr_t high = 0;
  if (o->length) {
    drop_spans(o->base, o->length, &low, &high);
  }
}

static void drop_range(var_object* o) {
  if (!o->length) {
    return;
  }
  hle_var_ranges_remove(&ranges, o->base, NULL);
  drop_spans_of(o);
  o->base = 0;
  o->length = 0;
}

// Uploads [start, end) of a cached range to the span holding it, or to a new
// span covering it and the spans it touches.
static void upload(uintptr_t start, uintptr_t end) {
  const hle_var_range* found = hle_var_ranges_find(&span_map, start);
  if (found && end <= found->base + found->length) {
    const var_span* s = &spans[found->value];
    bind_array_buffer(s->buffer);
    if (start == s->base && end == s->base + s->length) {
      gl.buffer_data(GL_ARRAY_BUFFER_ARB, s->length, (const void*)s->base,
                     GL_STATIC_DRAW_ARB);
    } else {
      gl.buffer_sub_data(GL_ARRAY_BUFFER_ARB, start - s->base, end - start,
                         (const void*)start);
    }
    return;
  }
  uintptr_t low = start;
  uintptr_t high = end;
  drop_spans(start, end - start, &low, &high);
  uint32_t id = free_span_count ? free_spans[--free_span_count] : next_span;
  if (!grow(&spans, &span_slots, id + 1, sizeof(*spans))) {
    return;
  }
  if (id == next_span) {
    next_span++;
  }
  uint32_t buffer = take_buffer();
  bind_array_buffer(buffer);
  gl.get_error();
  gl.buffer_data(GL_ARRAY_BUFFER_ARB, high - low, (const void*)low,
                 GL_STATIC_DRAW_ARB);
  if (!buffer || gl.get_error() != GL_NO_ERROR ||
      !hle_var_ranges_add(&span_map, low, high - low, id)) {
    fprintf(stderr, "hle: %lu bytes of a vertex array range stay in client "
            "memory: a buffer object was refused\n",
            (unsigned long)(high - low));
    if (buffer) {
      spare(buffer);
    }
    push(&free_spans, &free_span_count, &free_span_capacity, id);
    apply_bound();
    return;
  }
  spans[id] = (var_span){ low, high - low, buffer };
  if (++spans_made == 1) {
    cf_trace("cached vertex array ranges are buffer objects, flushed span by "
             "span: %lu bytes in the first", (unsigned long)(high - low));
  }
  // Arrays pointing here can use it now, and none may use a span dropped.
  apply_bound();
}

// ---------------------------------------------------------------------------
// GL_APPLE_vertex_array_range

static void vertex_array_range(int32_t length, const void* pointer) {
  var_object* o = object_of(bound);
  if (!o) {
    return;
  }
  drop_range(o);
  if (length > 0 && pointer) {
    // Memory another object's range held, freed and allocated again.
    uint32_t names[64];
    size_t taken = hle_var_ranges_take_overlapping(
        &ranges, (uintptr_t)pointer, length, names, 64);
    for (size_t i = 0; i < taken && i < 64; i++) {
      var_object* other = &objects[names[i]];
      drop_spans_of(other);
      other->base = 0;
      other->length = 0;
    }
    if (hle_var_ranges_add(&ranges, (uintptr_t)pointer, length, bound)) {
      o->base = (uintptr_t)pointer;
      o->length = length;
    }
  }
  apply_bound();
}

static void flush_vertex_array_range(int32_t length, const void* pointer) {
  const hle_var_range* range =
      length > 0 && pointer ? hle_var_ranges_find(&ranges, (uintptr_t)pointer)
                            : NULL;
  if (!range || objects[range->value].hint != GL_STORAGE_CACHED_APPLE) {
    return;
  }
  uintptr_t start = (uintptr_t)pointer;
  uintptr_t end = start + (size_t)length;
  uintptr_t range_end = range->base + range->length;
  if (end > range_end || end < start) {
    end = range_end;
  }
  upload(start, end);
}

static void vertex_array_parameteri(uint32_t pname, int32_t param) {
  var_object* o = object_of(bound);
  if (!o || pname != GL_VERTEX_ARRAY_STORAGE_HINT_APPLE ||
      o->hint == (uint32_t)param) {
    return;
  }
  o->hint = param;
  if (param != GL_STORAGE_CACHED_APPLE) {
    // What was uploaded for it is no longer what the GPU reads.
    drop_spans_of(o);
  }
  apply_bound();
}

// ---------------------------------------------------------------------------
// Client arrays

static int slot_of(uint32_t array) {
  switch (array) {
    case GL_VERTEX_ARRAY: return kVertex;
    case GL_NORMAL_ARRAY: return kNormal;
    case GL_COLOR_ARRAY: return kColor;
    case GL_SECONDARY_COLOR_ARRAY: return kSecondaryColor;
    case GL_FOG_COORD_ARRAY: return kFogCoord;
    case GL_EDGE_FLAG_ARRAY: return kEdgeFlag;
    case GL_TEXTURE_COORD_ARRAY:
      return client_unit < kTextureUnits ? kTexCoord + (int)client_unit : -1;
  }
  return -1;
}

static void set_enabled(int slot, uint8_t on) {
  var_object* o = object_of(bound);
  if (o) {
    o->arrays[slot].enabled = on;
    apply(slot, &o->arrays[slot]);
  }
}

static void set_range_enabled(int on) {
  var_object* o = object_of(bound);
  if (o && o->range_enabled != on) {
    o->range_enabled = on;
    apply_bound();
  }
}

static void enable_client_state(uint32_t array) {
  int slot = slot_of(array);
  if (array == GL_VERTEX_ARRAY_RANGE_APPLE) {
    set_range_enabled(1);
  } else if (slot >= 0) {
    set_enabled(slot, 1);
  } else {
    set_driver_unit(client_unit);
    gl.enable_client_state(array);
  }
}

static void disable_client_state(uint32_t array) {
  int slot = slot_of(array);
  if (array == GL_VERTEX_ARRAY_RANGE_APPLE) {
    set_range_enabled(0);
  } else if (slot >= 0) {
    set_enabled(slot, 0);
  } else {
    set_driver_unit(client_unit);
    gl.disable_client_state(array);
  }
}

static void enable_vertex_attrib_array(uint32_t index) {
  if (index < kAttributes) {
    set_enabled(kAttribute + index, 1);
  } else {
    gl.enable_vertex_attrib_array(index);
  }
}

static void disable_vertex_attrib_array(uint32_t index) {
  if (index < kAttributes) {
    set_enabled(kAttribute + index, 0);
  } else {
    gl.disable_vertex_attrib_array(index);
  }
}

static void client_active_texture(uint32_t texture) {
  client_unit = texture - GL_TEXTURE0;
  driver_unit_known = 0;
  set_driver_unit(client_unit);
}

static void set_pointer(int slot, int32_t size, uint32_t type, int32_t stride,
                        uint8_t normalized, const void* pointer) {
  var_object* o = object_of(bound);
  if (!o) {
    return;
  }
  client_array* a = &o->arrays[slot];
  a->set = 1;
  a->size = size;
  a->type = type;
  a->stride = stride;
  a->normalized = normalized;
  a->pointer = pointer;
  apply(slot, a);
}

static void vertex_pointer(int32_t size, uint32_t type, int32_t stride,
                           const void* pointer) {
  set_pointer(kVertex, size, type, stride, 0, pointer);
}

static void normal_pointer(uint32_t type, int32_t stride,
                           const void* pointer) {
  set_pointer(kNormal, 3, type, stride, 0, pointer);
}

static void color_pointer(int32_t size, uint32_t type, int32_t stride,
                          const void* pointer) {
  set_pointer(kColor, size, type, stride, 0, pointer);
}

static void secondary_color_pointer(int32_t size, uint32_t type,
                                    int32_t stride, const void* pointer) {
  set_pointer(kSecondaryColor, size, type, stride, 0, pointer);
}

static void fog_coord_pointer(uint32_t type, int32_t stride,
                              const void* pointer) {
  set_pointer(kFogCoord, 1, type, stride, 0, pointer);
}

static void edge_flag_pointer(int32_t stride, const void* pointer) {
  set_pointer(kEdgeFlag, 1, 0, stride, 0, pointer);
}

static void tex_coord_pointer(int32_t size, uint32_t type, int32_t stride,
                              const void* pointer) {
  if (client_unit < kTextureUnits) {
    set_pointer(kTexCoord + client_unit, size, type, stride, 0, pointer);
  } else {
    bind_array_buffer(0);
    set_driver_unit(client_unit);
    gl.tex_coord_pointer(size, type, stride, pointer);
  }
}

static void vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
                                  uint8_t normalized, int32_t stride,
                                  const void* pointer) {
  if (index < kAttributes) {
    set_pointer(kAttribute + index, size, type, stride, normalized, pointer);
  } else {
    bind_array_buffer(0);
    gl.vertex_attrib_pointer(index, size, type, normalized, stride, pointer);
  }
}

// ---------------------------------------------------------------------------
// GL_APPLE_vertex_array_object

static void gen_vertex_arrays(int32_t n, uint32_t* names) {
  for (int32_t i = 0; names && i < n; i++) {
    uint32_t name =
        free_name_count ? free_names[--free_name_count] : next_name++;
    if (!grow(&objects, &object_slots, name + 1, sizeof(*objects))) {
      names[i] = 0;
      continue;
    }
    memset(&objects[name], 0, sizeof(objects[name]));
    objects[name].named = 1;
    objects[name].hint = GL_STORAGE_CLIENT_APPLE;
    names[i] = name;
  }
}

static void bind_vertex_array(uint32_t name) {
  uint32_t next = object_of(name) ? name : 0;
  if (next != bound) {
    bound = next;
    apply_bound();
  }
}

static void delete_vertex_arrays(int32_t n, const uint32_t* names) {
  for (int32_t i = 0; names && i < n; i++) {
    var_object* o = names[i] ? object_of(names[i]) : NULL;
    if (!o) {
      continue;
    }
    drop_range(o);
    o->named = 0;
    push(&free_names, &free_name_count, &free_name_capacity, names[i]);
    if (bound == names[i]) {
      bound = 0;
    }
  }
  apply_bound();
}

static uint8_t is_vertex_array(uint32_t name) {
  return name != 0 && object_of(name) != NULL;
}

// ---------------------------------------------------------------------------
// GL_APPLE_fence

static void gen_fences(int32_t n, uint32_t* fences) {
  static uint32_t next = 1;
  for (int32_t i = 0; fences && i < n; i++) {
    fences[i] = next++;
  }
}

static void delete_fences(int32_t n, const uint32_t* fences) {
}

static void set_fence(uint32_t fence) {
}

static uint8_t is_fence(uint32_t fence) {
  return fence != 0;
}

static uint8_t test_fence(uint32_t fence) {
  return 1;
}

static void finish_fence(uint32_t fence) {
}

static uint8_t test_object(uint32_t object, uint32_t name) {
  return 1;
}

static void finish_object(uint32_t object, int32_t name) {
}

// ---------------------------------------------------------------------------
// For gl_probe.c

static void append(char* out, size_t size, size_t* n, const char* format,
                   ...) {
  if (*n + 1 >= size) {
    return;
  }
  va_list args;
  va_start(args, format);
  int k = vsnprintf(out + *n, size - *n, format, args);
  va_end(args);
  if (k > 0) {
    *n += (size_t)k < size - *n ? (size_t)k : size - *n - 1;
  }
}

void hle_gl_var_describe(char* out, size_t size) {
  static const struct {
    int slot;
    const char* name;
  } kShown[] = {
    { kVertex, "vertex" },          { kTexCoord, "texcoord0" },
    { kAttribute, "attrib0" },      { kAttribute + 1, "attrib1" },
    { kAttribute + 2, "attrib2" },  { kAttribute + 3, "attrib3" },
  };
  size_t n = 0;
  if (!size) {
    return;
  }
  out[0] = '\0';
  if (!hle_gl_var_enabled()) {
    append(out, size, &n, "vertex array objects are not emulated");
    return;
  }
  const var_object* o = object_of(bound);
  if (!o) {
    append(out, size, &n, "vertex array object %u is gone", bound);
    return;
  }
  append(out, size, &n, "vertex array object %u, range %#lx+%lu %s hint %#x",
         bound, (unsigned long)o->base, (unsigned long)o->length,
         o->range_enabled ? "enabled" : "disabled", (unsigned)o->hint);
  for (size_t i = 0; i < sizeof(kShown) / sizeof(kShown[0]); i++) {
    const client_array* a = &o->arrays[kShown[i].slot];
    const gl_array* have = &driver[kShown[i].slot];
    uint32_t buffer = 0;
    const void* pointer = translate(a->pointer, &buffer);
    append(out, size, &n,
           "; %s %s%s %p (buffer %u at %p), GL given %s %p buffer %u%s",
           kShown[i].name, a->enabled ? "on" : "off",
           a->set ? "" : " unset", a->pointer, buffer, pointer,
           !have->enable_known ? "?" : have->array.enabled ? "on" : "off",
           have->array.pointer, have->buffer,
           have->pointer_known ? "" : " (pointer unknown)");
  }
}

// ---------------------------------------------------------------------------

typedef struct {
  const char* name;
  void* function;
  void** real;  // NULL for the emulation's own functions
} var_entry;

static const var_entry entries[] = {
  { "glVertexArrayRangeAPPLE", vertex_array_range, NULL },
  { "glFlushVertexArrayRangeAPPLE", flush_vertex_array_range, NULL },
  { "glVertexArrayParameteriAPPLE", vertex_array_parameteri, NULL },
  { "glGenVertexArraysAPPLE", gen_vertex_arrays, NULL },
  { "glBindVertexArrayAPPLE", bind_vertex_array, NULL },
  { "glDeleteVertexArraysAPPLE", delete_vertex_arrays, NULL },
  { "glIsVertexArrayAPPLE", is_vertex_array, NULL },
  { "glGenFencesAPPLE", gen_fences, NULL },
  { "glDeleteFencesAPPLE", delete_fences, NULL },
  { "glSetFenceAPPLE", set_fence, NULL },
  { "glIsFenceAPPLE", is_fence, NULL },
  { "glTestFenceAPPLE", test_fence, NULL },
  { "glFinishFenceAPPLE", finish_fence, NULL },
  { "glTestObjectAPPLE", test_object, NULL },
  { "glFinishObjectAPPLE", finish_object, NULL },
  { "glClientActiveTexture", client_active_texture,
    (void**)&gl.client_active_texture },
  { "glClientActiveTextureARB", client_active_texture,
    (void**)&gl.client_active_texture },
  { "glEnableClientState", enable_client_state,
    (void**)&gl.enable_client_state },
  { "glDisableClientState", disable_client_state,
    (void**)&gl.disable_client_state },
  { "glEnableVertexAttribArray", enable_vertex_attrib_array,
    (void**)&gl.enable_vertex_attrib_array },
  { "glEnableVertexAttribArrayARB", enable_vertex_attrib_array,
    (void**)&gl.enable_vertex_attrib_array },
  { "glDisableVertexAttribArray", disable_vertex_attrib_array,
    (void**)&gl.disable_vertex_attrib_array },
  { "glDisableVertexAttribArrayARB", disable_vertex_attrib_array,
    (void**)&gl.disable_vertex_attrib_array },
  { "glVertexPointer", vertex_pointer, (void**)&gl.vertex_pointer },
  { "glNormalPointer", normal_pointer, (void**)&gl.normal_pointer },
  { "glColorPointer", color_pointer, (void**)&gl.color_pointer },
  { "glSecondaryColorPointer", secondary_color_pointer,
    (void**)&gl.secondary_color_pointer },
  { "glSecondaryColorPointerEXT", secondary_color_pointer,
    (void**)&gl.secondary_color_pointer },
  { "glFogCoordPointer", fog_coord_pointer, (void**)&gl.fog_coord_pointer },
  { "glFogCoordPointerEXT", fog_coord_pointer,
    (void**)&gl.fog_coord_pointer },
  { "glEdgeFlagPointer", edge_flag_pointer, (void**)&gl.edge_flag_pointer },
  { "glTexCoordPointer", tex_coord_pointer, (void**)&gl.tex_coord_pointer },
  { "glVertexAttribPointer", vertex_attrib_pointer,
    (void**)&gl.vertex_attrib_pointer },
  { "glVertexAttribPointerARB", vertex_attrib_pointer,
    (void**)&gl.vertex_attrib_pointer },
};

void* hle_gl_var_wrap(const char* name, void* real) {
  if (!hle_gl_var_enabled()) {
    return real;
  }
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    if (strcmp(name, entries[i].name) != 0) {
      continue;
    }
    if (!entries[i].real) {
      return entries[i].function;
    }
    if (!real) {
      return NULL;
    }
    *entries[i].real = real;
    return entries[i].function;
  }
  return real;
}
