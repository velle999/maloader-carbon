# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Mac OS X icon files (.icns) as PNG, for desktop entries.

Newer icons hold PNG images, which are taken as they are. Older ones hold
RGB images run-length encoded a channel at a time, with separate 8-bit
masks: it32 (128x128, after four zero bytes), ih32 (48x48), il32 (32x32) and
is32 (16x16).
"""

import struct
import zlib

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# RGB image type: (width, mask type).
RGB_TYPES = {
    b"it32": (128, b"t8mk"),
    b"ih32": (48, b"h8mk"),
    b"il32": (32, b"l8mk"),
    b"is32": (16, b"s8mk"),
}


def read_chunks(data):
    """The icon's images and masks by type."""
    if data[:4] != b"icns" or len(data) < 8:
        raise ValueError("not an icns file")
    limit = min(struct.unpack(">I", data[4:8])[0], len(data))
    chunks = {}
    at = 8
    while at + 8 <= limit:
        kind = data[at:at + 4]
        size = struct.unpack(">I", data[at + 4:at + 8])[0]
        if size < 8 or at + size > limit:
            break
        chunks.setdefault(kind, data[at + 8:at + size])
        at += size
    return chunks


def unpack_channels(data, pixels):
    """The three channels of a run-length encoded image, one after another.

    A byte under 0x80 is followed by that many plus one bytes as they are;
    any other byte is followed by one byte repeated that many less 125
    times."""
    need = pixels * 3
    out = bytearray()
    at = 0
    while len(out) < need and at < len(data):
        n = data[at]
        at += 1
        if n < 0x80:
            out += data[at:at + n + 1]
            at += n + 1
        elif at < len(data):
            out += bytes((data[at],)) * (n - 125)
            at += 1
    if len(out) < need:
        raise ValueError("a run-length encoded image ends early")
    return bytes(out[:need])


def decode_rgb(kind, data, mask=None):
    """(width, RGBA bytes) of an RGB image and its mask, if any."""
    width = RGB_TYPES[kind][0]
    pixels = width * width
    if kind == b"it32":
        data = data[4:]
    rgba = bytearray(pixels * 4)
    if len(data) == pixels * 4:
        # Uncompressed ARGB.
        rgba[0::4] = data[1::4]
        rgba[1::4] = data[2::4]
        rgba[2::4] = data[3::4]
    else:
        channels = unpack_channels(data, pixels)
        rgba[0::4] = channels[:pixels]
        rgba[1::4] = channels[pixels:2 * pixels]
        rgba[2::4] = channels[2 * pixels:]
    rgba[3::4] = mask if mask is not None and len(mask) == pixels else (
        b"\xff" * pixels)
    return width, bytes(rgba)


def encode_png(width, height, rgba):
    """A PNG file of 8-bit RGBA rows."""

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    stride = width * 4
    rows = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride]
                    for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (PNG_MAGIC + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def png_width(data):
    if data[:8] != PNG_MAGIC or len(data) < 24:
        return 0
    return struct.unpack(">I", data[16:20])[0]


def largest_png(data):
    """The largest image in an icns file, as PNG, or None."""
    chunks = read_chunks(data)
    best_width = 0
    best = None
    for kind, body in chunks.items():
        width = png_width(body)
        if width > best_width:
            best_width, best = width, body
    for kind, (width, mask_kind) in RGB_TYPES.items():
        if kind in chunks and width > best_width:
            try:
                width, rgba = decode_rgb(kind, chunks[kind],
                                         chunks.get(mask_kind))
            except ValueError:
                continue
            best_width, best = width, encode_png(width, width, rgba)
    return best
