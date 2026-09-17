# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""halo-ce-installer: installs Halo: Combat Evolved from its Mac release
(Halo Universal 2.0) to run on 32-bit Linux under maloader-carbon.

Without arguments it opens its window; with --source it installs from the
command line."""

import argparse
import os
import shutil
import sys

from . import halo
from . import launch

DEFAULT_DESTINATION = os.path.join(os.path.expanduser("~"), "Games", "HaloCE")


def prerequisites():
    """(name, ok, detail) for what installing and playing need."""
    loader = launch.find_loader()
    limit = launch.mmap_min_addr()
    return [
        ("maloader-carbon", bool(loader),
         loader or "ld-mac and libmac.so were not found; build them with "
         "make, or install with make install"),
        ("7-Zip", bool(halo.seven_zip()),
         halo.seven_zip() or "7z is needed to read disc images (a folder "
         "with the disc's files works without it)"),
        ("cpio", bool(shutil.which("cpio")),
         shutil.which("cpio") or "cpio is needed to unpack the game"),
        ("vm.mmap_min_addr", limit is None or
         limit <= launch.MMAP_MIN_ADDR_NEEDED,
         "%s (%d or less is needed to play)" % (
             "unknown" if limit is None else limit,
             launch.MMAP_MIN_ADDR_NEEDED)),
    ]


def parser():
    p = argparse.ArgumentParser(
        prog="halo-ce-installer",
        description="Installs Halo: Combat Evolved from its Mac release, "
        "Halo Universal 2.0, to run on 32-bit Linux under maloader-carbon. "
        "Without --source, opens the installer's window.")
    p.add_argument("--source", help="the disc image (.dmg, .img, .iso, .cdr, "
                   ".toast) or a folder with the disc's files")
    p.add_argument("--destination", default=DEFAULT_DESTINATION,
                   help="where to install (default: %(default)s)")
    p.add_argument("--window", action="store_true",
                   help="play in a window rather than full screen")
    p.add_argument("--desktop-shortcut", action="store_true",
                   help="also put a launcher on the desktop")
    p.add_argument("--import-home", metavar="FOLDER",
                   help="bring over saved games and settings from an earlier "
                   "install's Mac home folder")
    p.add_argument("--check", action="store_true",
                   help="only check the source and destination")
    return p


def main(argv):
    args = parser().parse_args(argv)
    if not args.source:
        from .launcher_app import gui_available
        if gui_available():
            from . import gtk_installer
            return gtk_installer.main()
        parser().print_help()
        return 2
    ready = True
    for name, ok, detail in prerequisites():
        print("%-18s %s  %s" % (name, "ok" if ok else "MISSING", detail))
        ready = ready and (ok or name == "vm.mmap_min_addr")
    try:
        source = halo.probe(args.source)
    except halo.InstallError as e:
        raise SystemExit("halo-ce-installer: %s" % e)
    print("Found %s" % source.describe())
    destination = os.path.abspath(os.path.expanduser(args.destination))
    problems, warnings = halo.check_destination(destination)
    for warning in warnings:
        print("Note: %s" % warning)
    for problem in problems:
        print("Problem: %s" % problem)
    if problems or not ready:
        return 1
    if args.check:
        return 0

    last = [None]

    def progress(step, fraction):
        if fraction is None:
            if step != last[0]:
                print(step)
            last[0] = step
            return
        percent = int(fraction * 100)
        if (step, percent) != last[0]:
            last[0] = (step, percent)
            if sys.stdout.isatty():
                print("\r%s: %d%%" % (step, percent), end="", flush=True)
            elif percent % 10 == 0:
                print("%s: %d%%" % (step, percent), flush=True)
            if percent == 100 and sys.stdout.isatty():
                print()

    try:
        paths = halo.install(source, destination, args.import_home, progress)
    except KeyboardInterrupt:
        raise SystemExit("\nhalo-ce-installer: cancelled")
    except (halo.InstallError, OSError) as e:
        raise SystemExit("halo-ce-installer: %s" % e)
    profile = halo.register(paths, "window" if args.window else "fullscreen",
                            args.desktop_shortcut)
    print("Installed to %s.\nStart it from the applications menu, or with: "
          "carbon-launcher run %s" % (destination, profile["id"]))
    if launch.product_key_needed(profile):
        print("It asks for its product key, printed in its manual, the first "
              "time it starts.")
    return 0
