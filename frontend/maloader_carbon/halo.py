# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Installing Halo: Combat Evolved from its Mac release, Halo Universal 2.0.

The disc (or its disc image) holds an installer package whose
Contents/Archive.pax.gz is a gzipped cpio archive of Halo.app, AppleDouble
files and all: the game's product-key dialog lives in the resource fork of
EULA.rsrc, which is ._EULA.rsrc here. The disc's other files stay beside the
game as its disc, as the game checks for them before it starts.
"""

import os
import shutil
import subprocess
import tempfile
import zlib

from . import bundle as bundles
from . import desktop
from . import profiles

APP_ID = "halo-ce"
APP_NAME = "Halo: Combat Evolved"
IDENTIFIER = "com.macsoft.halo"
ARCHIVE = "Halo.pkg/Contents/Archive.pax.gz"
DEFAULT_VOLUME = "Halo Universal"
IMAGE_SUFFIXES = (".dmg", ".img", ".iso", ".cdr", ".toast")
# The disc's top level, less the package and what Mac OS keeps for itself.
SKIPPED = {"Halo.pkg", "[HFS+ Private Data]", ".Trashes", ".fseventsd",
           ".Spotlight-V100", ".TemporaryItems"}
# Halo.app unpacks to 1.5 GB.
SPACE_NEEDED = 1700 * 1000 * 1000
PRODUCT_KEY = {
    "dialog": 10001,
    "plist": "Library/Preferences/com.macsoft.halo.plist",
    "key": "GameSpy Key",
}
# What an earlier install's Mac home holds that is worth keeping.
KEPT_FROM_HOME = ("Library/Preferences/com.macsoft.halo.plist",
                  "Documents/Halo")


class InstallError(Exception):
    pass


class Cancelled(InstallError):
    pass


class Source:
    """Where the installer package is: in a disc image, which 7-Zip reads,
    or in a folder, such as a mounted disc."""

    def __init__(self, kind, path, volume, archive, archive_size, top_level):
        self.kind = kind            # "image" or "folder"
        self.path = path            # the image, or the volume's folder
        self.volume = volume        # the disc's name
        self.archive = archive      # path in the image, or of the file
        self.archive_size = archive_size
        self.top_level = top_level  # the disc's other files and folders

    def describe(self):
        size = self.archive_size / 1e6
        return "%s, from %s %s (%s to unpack)" % (
            self.volume, "the disc image" if self.kind == "image"
            else "the folder", self.path,
            "%.0f MB" % size if size >= 1 else "under 1 MB")


def seven_zip():
    return shutil.which("7z") or shutil.which("7za")


def list_image(path):
    """(path, size, is folder) for each entry 7-Zip finds in an image."""
    tool = seven_zip()
    if not tool:
        raise InstallError("7-Zip (7z) is needed to read disc images.")
    result = subprocess.run([tool, "l", "-slt", "--", path],
                            capture_output=True, text=True,
                            errors="replace", timeout=600)
    if result.returncode != 0:
        raise InstallError("7-Zip cannot read %s: %s" % (
            path, (result.stderr or result.stdout).strip()[-300:]))
    entries = []
    listing = result.stdout.split("\n----------\n", 1)
    if len(listing) < 2:
        return entries
    for block in listing[1].split("\n\n"):
        fields = {}
        for line in block.splitlines():
            name, sep, value = line.partition(" = ")
            if sep:
                fields[name] = value
        if "Path" in fields:
            size = fields.get("Size", "")
            entries.append((fields["Path"], int(size) if size.isdigit() else 0,
                            fields.get("Folder") == "+"))
    return entries


def probe(path):
    """The installer package at |path|, or InstallError saying why not."""
    path = os.path.abspath(os.path.expanduser(path))
    if os.path.isdir(path):
        for volume_dir in [path] + sorted(
                os.path.join(path, name) for name in os.listdir(path)
                if os.path.isdir(os.path.join(path, name))):
            archive = os.path.join(volume_dir, ARCHIVE)
            if os.path.isfile(archive):
                top_level = sorted(os.path.join(volume_dir, name)
                                   for name in os.listdir(volume_dir)
                                   if name not in SKIPPED)
                return Source("folder", volume_dir,
                              os.path.basename(volume_dir), archive,
                              os.path.getsize(archive), top_level)
        raise InstallError("%s holds no %s: it is not the Halo Universal "
                           "disc." % (path, ARCHIVE))
    if not os.path.isfile(path):
        raise InstallError("%s does not exist." % path)
    entries = list_image(path)
    for name, size, _ in entries:
        if name == ARCHIVE or name.endswith("/" + ARCHIVE):
            volume = name[:-len(ARCHIVE)].rstrip("/")
            prefix = volume + "/" if volume else ""
            top_level = sorted(
                entry for entry, _, _ in entries
                if entry.startswith(prefix) and entry != prefix and
                "/" not in entry[len(prefix):] and
                entry[len(prefix):] not in SKIPPED)
            return Source("image", path, volume or DEFAULT_VOLUME, name, size,
                          top_level)
    raise InstallError("%s holds no %s: it is not the Halo Universal disc "
                       "image." % (path, ARCHIVE))


def home_path(destination):
    return os.path.join(destination, "Home")


def check_destination(destination):
    """(problems, warnings) for installing into |destination|."""
    problems, warnings = [], []
    destination = os.path.abspath(os.path.expanduser(destination))
    home = home_path(destination)
    # The game builds its folders' Mac paths from every folder's name and
    # quits on one with a period, slash or backslash in it.
    bad = [part for part in home.split("/") if any(c in part for c in ".\\")]
    if bad:
        problems.append("The game refuses folders whose names hold a period "
                        "or backslash, and %s is in this path. Choose "
                        "another folder." % ", ".join(repr(b) for b in bad))
    existing = destination
    while not os.path.exists(existing):
        existing = os.path.dirname(existing)
    if not os.access(existing, os.W_OK):
        problems.append("%s cannot be written to." % existing)
    else:
        stat = os.statvfs(existing)
        free = stat.f_bavail * stat.f_frsize
        # A reinstall needs the room too: the old copy goes only once the
        # new one is unpacked.
        if free < SPACE_NEEDED:
            problems.append("The game needs %.1f GB and %s has %.1f GB free." %
                            (SPACE_NEEDED / 1e9, existing, free / 1e9))
    if os.path.isdir(os.path.join(destination, "Halo.app")):
        warnings.append("%s already holds Halo.app, which will be replaced. "
                        "Saved games and settings in its Home folder stay." %
                        destination)
    elif os.path.isdir(destination) and os.listdir(destination):
        warnings.append("%s is not empty." % destination)
    return problems, warnings


def earlier_homes():
    """Mac homes that hold Halo settings: a registered install's and the
    loader's default."""
    found = []
    try:
        found.append(profiles.load(APP_ID)["home"])
    except (KeyError, ValueError, OSError):
        pass
    found.append(profiles.loader_default_home())
    return [h for h in dict.fromkeys(found)
            if h and os.path.isfile(os.path.join(h, KEPT_FROM_HOME[0]))]


def _copy_disc(source, disc, progress):
    progress("Copying the disc's files", None)
    os.makedirs(disc, exist_ok=True)
    if source.kind == "folder":
        for path in source.top_level:
            target = os.path.join(disc, os.path.basename(path))
            if os.path.isdir(path):
                shutil.copytree(path, target, dirs_exist_ok=True)
            else:
                shutil.copy2(path, target)
        return
    with tempfile.TemporaryDirectory(dir=os.path.dirname(disc)) as scratch:
        args = [seven_zip(), "x", "-y", "-o" + scratch, source.path]
        args += ["-i!" + entry for entry in source.top_level]
        result = subprocess.run(args, capture_output=True, text=True,
                                errors="replace")
        if result.returncode != 0:
            raise InstallError("7-Zip could not copy the disc's files: %s" %
                               (result.stderr or result.stdout)[-300:])
        extracted = os.path.join(scratch, source.volume) if (
            source.volume and os.path.isdir(
                os.path.join(scratch, source.volume))) else scratch
        for name in os.listdir(extracted):
            target = os.path.join(disc, name)
            if os.path.isdir(target):
                shutil.rmtree(target)
            shutil.move(os.path.join(extracted, name), target)


def _unpack(source, staging, progress, cancelled):
    """Unpacks the package into |staging|, piping gzip's output to cpio."""
    if not shutil.which("cpio"):
        raise InstallError("cpio is needed to unpack the game.")
    # Their messages go to files: a pipe nobody reads until the end could
    # fill and stop them.
    reader_errors = tempfile.TemporaryFile()
    cpio_errors = tempfile.TemporaryFile()
    reader = None
    if source.kind == "image":
        reader = subprocess.Popen(
            [seven_zip(), "x", "-so", source.path, source.archive],
            stdout=subprocess.PIPE, stderr=reader_errors)
        stream = reader.stdout
    else:
        stream = open(source.archive, "rb")
    cpio = subprocess.Popen(
        ["cpio", "-idm", "--no-absolute-filenames", "--quiet"], cwd=staging,
        stdin=subprocess.PIPE, stderr=cpio_errors)

    def errors(f):
        f.seek(0)
        return f.read().decode(errors="replace").strip()[-300:]

    inflate = zlib.decompressobj(16 + zlib.MAX_WBITS)
    done = 0
    try:
        while True:
            if cancelled():
                raise Cancelled("The installation was cancelled.")
            chunk = stream.read(1 << 20)
            if not chunk:
                break
            done += len(chunk)
            cpio.stdin.write(inflate.decompress(chunk))
            progress("Unpacking the game",
                     min(done / max(source.archive_size, 1), 1.0))
        cpio.stdin.write(inflate.flush())
        cpio.stdin.close()
        if cpio.wait() != 0:
            raise InstallError("cpio could not unpack the game: %s" %
                               errors(cpio_errors))
        if reader and reader.wait() != 0:
            raise InstallError("7-Zip could not read the game from the "
                               "image: %s" % errors(reader_errors))
        if not inflate.eof:
            raise InstallError("The game's archive ends early: the image "
                               "may be damaged or incomplete.")
    except (BrokenPipeError, zlib.error) as e:
        raise InstallError("The game could not be unpacked: %s" % e)
    finally:
        if not cpio.stdin.closed:
            try:
                cpio.stdin.close()
            except BrokenPipeError:
                pass
        for process in (cpio, reader):
            if process and process.poll() is None:
                process.kill()
                process.wait()
        stream.close()
        reader_errors.close()
        cpio_errors.close()


def _import_home(earlier, home, progress):
    progress("Bringing over saved games and settings", None)
    for relative in KEPT_FROM_HOME:
        source = os.path.join(earlier, relative)
        target = os.path.join(home, relative)
        if os.path.isdir(source):
            shutil.copytree(source, target, dirs_exist_ok=True)
        elif os.path.isfile(source):
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(source, target)


def install(source, destination, import_home=None, progress=None,
            cancelled=None):
    """Installs the game into |destination|: Halo.app, the disc's files in
    Disc/<volume>, and the Mac home in Home. Returns their paths."""
    progress = progress or (lambda step, fraction: None)
    cancelled = cancelled or (lambda: False)
    destination = os.path.abspath(os.path.expanduser(destination))
    problems, _ = check_destination(destination)
    if problems:
        raise InstallError("\n".join(problems))
    os.makedirs(destination, exist_ok=True)
    disc = os.path.join(destination, "Disc", source.volume)
    app = os.path.join(destination, "Halo.app")
    home = home_path(destination)

    _copy_disc(source, disc, progress)
    staging = tempfile.mkdtemp(prefix="unpacking-", dir=destination)
    try:
        _unpack(source, staging, progress, cancelled)
        progress("Checking the game", None)
        unpacked = os.path.join(staging, "Halo.app")
        try:
            game = bundles.read_bundle(unpacked)
        except bundles.BundleError as e:
            raise InstallError("The package did not hold the game: %s" % e)
        if game.identifier != IDENTIFIER or not game.runnable:
            raise InstallError(
                "The package holds %s %s for %s, not Halo for Intel Macs." % (
                    game.name, game.version,
                    ", ".join(game.architectures) or "no processor"))
        old = None
        if os.path.isdir(app):
            old = tempfile.mkdtemp(prefix="replaced-", dir=destination)
            os.rename(app, os.path.join(old, "Halo.app"))
        os.rename(unpacked, app)
        if old:
            shutil.rmtree(old, ignore_errors=True)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    os.makedirs(home, exist_ok=True)
    if import_home and os.path.abspath(import_home) != home:
        _import_home(import_home, home, progress)
    return {"app": app, "home": home, "disc": disc}


def register(paths, display="fullscreen", on_desktop=False):
    """Makes the launcher's profile for the installed game and its desktop
    entries. Returns the profile."""
    profile = profiles.with_defaults({
        "id": APP_ID,
        "name": APP_NAME,
        "comment": "Halo: Combat Evolved for Mac, on Linux",
        "category": "Game",
        "bundle": paths["app"],
        "home": paths["home"],
        "disc": paths["disc"],
        "display": display,
        "trace": True,
        "product_key": PRODUCT_KEY,
    })
    try:
        earlier = profiles.load(APP_ID)
        # Keep what the player set before, for a reinstall.
        for name in ("sound", "trace", "env"):
            profile[name] = earlier[name]
    except (KeyError, ValueError, OSError):
        pass
    profile["icon"] = desktop.extract_icon(profile)
    profiles.save(profile)
    desktop.install(profile, on_desktop=on_desktop)
    return profile
