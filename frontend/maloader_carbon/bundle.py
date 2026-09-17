# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Mac OS X application bundles (.app): their name, version, executable and
icon, and which processors the executable has code for."""

import os
import plistlib
import struct

CPU_NAMES = {7: "i386", 0x01000007: "x86_64", 18: "ppc", 0x01000012: "ppc64"}


class BundleError(Exception):
    pass


class Bundle:
    def __init__(self, path, info):
        self.path = path
        self.info = info
        contents = os.path.join(path, "Contents")
        stem = os.path.splitext(os.path.basename(path))[0]
        executable = info.get("CFBundleExecutable") or stem
        self.executable = os.path.join(contents, "MacOS", executable)
        self.name = (info.get("CFBundleDisplayName") or
                     info.get("CFBundleName") or stem)
        self.identifier = info.get("CFBundleIdentifier") or ""
        self.version = (info.get("CFBundleShortVersionString") or
                        info.get("CFBundleVersion") or "")
        icon = info.get("CFBundleIconFile")
        if icon:
            if not os.path.splitext(icon)[1]:
                icon += ".icns"
            icon = os.path.join(contents, "Resources", icon)
        self.icon = icon if icon and os.path.isfile(icon) else None
        self.architectures = architectures(self.executable)

    @property
    def runnable(self):
        """Whether the loader, which runs 32-bit Intel code, can run it."""
        return "i386" in self.architectures


def architectures(path):
    """The processors a Mach-O file has code for, empty for anything else."""
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
    except OSError:
        return []
    if len(head) < 8:
        return []
    if struct.unpack(">I", head[:4])[0] == 0xCAFEBABE:
        count = struct.unpack(">I", head[4:8])[0]
        found = []
        for i in range(min(count, 32)):
            at = 8 + i * 20
            if at + 4 > len(head):
                break
            cpu = struct.unpack(">i", head[at:at + 4])[0]
            found.append(CPU_NAMES.get(cpu & 0xFFFFFFFF, "cpu %d" % cpu))
        return found
    magic_le = struct.unpack("<I", head[:4])[0]
    magic_be = struct.unpack(">I", head[:4])[0]
    for magic, order in ((magic_le, "<"), (magic_be, ">")):
        if magic in (0xFEEDFACE, 0xFEEDFACF):
            cpu = struct.unpack(order + "i", head[4:8])[0]
            return [CPU_NAMES.get(cpu & 0xFFFFFFFF, "cpu %d" % cpu)]
    return []


def read_bundle(path):
    """The bundle at |path|, or BundleError saying why it is not one."""
    path = os.path.abspath(path.rstrip("/") or "/")
    info_path = os.path.join(path, "Contents", "Info.plist")
    if not os.path.isdir(path):
        raise BundleError("%s is not a folder" % path)
    try:
        with open(info_path, "rb") as f:
            info = plistlib.load(f)
    except FileNotFoundError:
        raise BundleError("%s has no Contents/Info.plist: it is not an "
                          "application bundle" % path)
    except Exception as e:
        raise BundleError("%s: Contents/Info.plist cannot be read: %s" %
                          (path, e))
    if not isinstance(info, dict):
        raise BundleError("%s: Contents/Info.plist is not a dictionary" %
                          path)
    bundle = Bundle(path, info)
    if not os.path.isfile(bundle.executable):
        raise BundleError("%s has no executable at %s" %
                          (path, bundle.executable))
    return bundle
