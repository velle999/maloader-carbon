// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Sound Manager buffers as samples, and one voice mixed into stereo output.
//
// A bufferCmd names a sound header: standard (8-bit mono), extended (8- or
// 16-bit, mono or stereo) or compressed, whose format code says how the
// samples are stored. A voice plays a buffer at the buffer's rate times the
// channel's rate multiplier, scaled by the channel's volume for each side
// and its amplitude.

#include <string.h>

#include "sound_mix.h"

enum {
  notCompressed = 0,
  fixedCompression = -1,
};

enum {
  kSoundNotCompressed = 'NONE',
  k8BitOffsetBinaryFormat = 'raw ',
  k16BitBigEndianFormat = 'twos',
  k16BitLittleEndianFormat = 'sowt',
  kIMACompression = 'ima4',
  kMACE3Compression = 'MAC3',
  kMACE6Compression = 'MAC6',
};

#ifdef __i386__
_Static_assert(offsetof(SoundHeader, sampleArea) == 22, "SoundHeader");
_Static_assert(offsetof(ExtSoundHeader, numFrames) == 22, "numFrames");
_Static_assert(offsetof(ExtSoundHeader, sampleSize) == 48, "ExtSoundHeader");
_Static_assert(offsetof(ExtSoundHeader, sampleArea) == 64, "ExtSoundHeader");
_Static_assert(offsetof(CmpSoundHeader, format) == 40, "CmpSoundHeader");
_Static_assert(offsetof(CmpSoundHeader, compressionID) == 56, "compressionID");
_Static_assert(offsetof(CmpSoundHeader, sampleSize) == 62, "CmpSoundHeader");
_Static_assert(offsetof(CmpSoundHeader, sampleArea) == 64, "CmpSoundHeader");
#endif

int hle_sound_buffer_from_header(const uint8_t* header,
                                 hle_sound_buffer* buffer, uint32_t* format) {
  memset(buffer, 0, sizeof(*buffer));
  if (format) {
    *format = 0;
  }
  if (!header) {
    return kHleSoundBadHeader;
  }
  const SoundHeader* standard = (const SoundHeader*)header;
  buffer->rate = standard->sampleRate;
  switch (standard->encode) {
    case stdSH:
      buffer->data = standard->samplePtr ? (const uint8_t*)standard->samplePtr
                                         : standard->sampleArea;
      buffer->frames = standard->length;
      buffer->channels = 1;
      buffer->bytes = 1;
      buffer->offset_binary = 1;
      break;
    case extSH: {
      const ExtSoundHeader* extended = (const ExtSoundHeader*)header;
      buffer->data = extended->samplePtr ? (const uint8_t*)extended->samplePtr
                                         : extended->sampleArea;
      buffer->frames = extended->numFrames;
      buffer->channels = extended->numChannels;
      buffer->bytes = extended->sampleSize / 8;
      // 8-bit samples are offset binary, and 16-bit ones in the machine's
      // byte order: little-endian on an Intel Mac. Halo's buffers read
      // smoothly only that way.
      buffer->offset_binary = buffer->bytes == 1;
      break;
    }
    case cmpSH: {
      const CmpSoundHeader* compressed = (const CmpSoundHeader*)header;
      if (format) {
        *format = compressed->format;
      }
      buffer->data = compressed->samplePtr
                         ? (const uint8_t*)compressed->samplePtr
                         : compressed->sampleArea;
      buffer->frames = compressed->numFrames;
      buffer->channels = compressed->numChannels;
      buffer->bytes = compressed->sampleSize / 8;
      if (compressed->compressionID != notCompressed &&
          compressed->compressionID != fixedCompression) {
        return kHleSoundUnsupported;
      }
      switch (compressed->format) {
        case 0:
        case kSoundNotCompressed:
          // As in an extended header.
          buffer->offset_binary = buffer->bytes == 1;
          break;
        case k8BitOffsetBinaryFormat:
          buffer->offset_binary = 1;
          break;
        case k16BitBigEndianFormat:
          buffer->big_endian = 1;
          break;
        case k16BitLittleEndianFormat:
          break;
        default:
          return kHleSoundUnsupported;
      }
      break;
    }
    default:
      return kHleSoundBadHeader;
  }
  if (!buffer->data || buffer->channels < 1 || buffer->channels > 2 ||
      buffer->bytes < 1 || buffer->bytes > 2 || buffer->rate < 0x10000) {
    return kHleSoundUnsupported;
  }
  return kHleSoundOk;
}

uint64_t hle_sound_header_duration_ns(const uint8_t* header,
                                      uint32_t rate_multiplier) {
  const SoundHeader* standard = (const SoundHeader*)header;
  if (!header || standard->sampleRate < 0x10000 || !rate_multiplier) {
    return 0;
  }
  double frames;
  if (standard->encode == stdSH) {
    frames = standard->length;
  } else {
    frames = ((const ExtSoundHeader*)header)->numFrames;
    if (standard->encode == cmpSH) {
      switch (((const CmpSoundHeader*)header)->format) {
        case kIMACompression: frames *= 64; break;
        case kMACE3Compression: frames *= 3; break;
        case kMACE6Compression: frames *= 6; break;
      }
    }
  }
  double rate = standard->sampleRate / 65536.0 * (rate_multiplier / 65536.0);
  return (uint64_t)(frames / rate * 1e9);
}

// Inlined: the mixer's thread shares the Pentium 4's core with the game's,
// and this was called up to four times for every sample of every voice.
static inline __attribute__((always_inline)) int32_t sample_at(
    const hle_sound_buffer* buffer, uint32_t frame, int side) {
  const uint8_t* p = buffer->data +
      ((size_t)frame * buffer->channels + (buffer->channels == 2 ? side : 0)) *
          buffer->bytes;
  if (buffer->bytes == 1) {
    return buffer->offset_binary ? ((int32_t)p[0] - 0x80) * 256
                                 : (int32_t)(int8_t)p[0] * 256;
  }
  return buffer->big_endian ? (int16_t)(p[0] << 8 | p[1])
                            : (int16_t)(p[1] << 8 | p[0]);
}

int hle_sound_voice_mix(hle_sound_voice* voice, int32_t* mix, int frames,
                        int output_rate) {
  if (!voice->playing || !voice->rate_multiplier || output_rate <= 0) {
    return 0;
  }
  const hle_sound_buffer* buffer = &voice->buffer;
  // Buffer frames for each output frame, 32.32.
  uint64_t step = (uint64_t)buffer->rate * voice->rate_multiplier /
                  (uint64_t)output_rate;
  int32_t left = (int32_t)voice->left * voice->amplitude / 255;
  int32_t right = (int32_t)voice->right * voice->amplitude / 255;
  int i = 0;
  for (; i < frames; i++) {
    uint32_t frame = (uint32_t)(voice->position >> 32);
    if (frame >= buffer->frames) {
      break;
    }
    int32_t l = sample_at(buffer, frame, 0);
    int32_t r = buffer->channels == 2 ? sample_at(buffer, frame, 1) : l;
    if (voice->interpolate && frame + 1 < buffer->frames) {
      int64_t fraction = (voice->position >> 16) & 0xFFFF;
      int32_t next_l = sample_at(buffer, frame + 1, 0);
      int32_t next_r =
          buffer->channels == 2 ? sample_at(buffer, frame + 1, 1) : next_l;
      l += (int32_t)(((int64_t)(next_l - l) * fraction) >> 16);
      r += (int32_t)(((int64_t)(next_r - r) * fraction) >> 16);
    }
    mix[2 * i] += (l * left) >> 8;
    mix[2 * i + 1] += (r * right) >> 8;
    voice->position += step;
  }
  if ((voice->position >> 32) >= buffer->frames) {
    voice->playing = 0;
  }
  return i;
}

uint64_t hle_sound_voice_remaining_ns(const hle_sound_voice* voice) {
  uint64_t end = (uint64_t)voice->buffer.frames << 32;
  if (!voice->playing || voice->position >= end) {
    return 0;
  }
  if (!voice->rate_multiplier || !voice->buffer.rate) {
    return UINT64_MAX;
  }
  double frames = (end - voice->position) / 4294967296.0;
  double rate = voice->buffer.rate / 65536.0 *
                (voice->rate_multiplier / 65536.0);
  return (uint64_t)(frames / rate * 1e9);
}

int16_t hle_sound_limit(int32_t sum) {
  enum {
    kKnee = 24576,
    kRoom = 32767 - kKnee,
  };
  int64_t magnitude = sum < 0 ? -(int64_t)sum : sum;
  if (magnitude <= kKnee) {
    return (int16_t)sum;
  }
  // Above the knee the slope starts at 1 and falls, and the curve nears
  // full scale without reaching it.
  int64_t over = magnitude - kKnee;
  int32_t bent = kKnee + (int32_t)(over * kRoom / (over + kRoom));
  return (int16_t)(sum < 0 ? -bent : bent);
}
