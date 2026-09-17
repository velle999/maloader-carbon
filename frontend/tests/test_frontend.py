# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Checks the frontend without a display or the game: icons, bundles,
profiles, launching, desktop entries, and installing from a disc made here
as a folder and as an archive 7-Zip reads.

  python3 -m unittest discover -s frontend/tests
"""

import gzip
import io
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))))

from maloader_carbon import bundle as bundles  # noqa: E402
from maloader_carbon import desktop  # noqa: E402
from maloader_carbon import halo  # noqa: E402
from maloader_carbon import icns  # noqa: E402
from maloader_carbon import installer_app  # noqa: E402
from maloader_carbon import launch  # noqa: E402
from maloader_carbon import launcher_app  # noqa: E402
from maloader_carbon import profiles  # noqa: E402

FAKE_KEY = "AB12-CD34-EF56-GH78"


def rle(channel):
    """Run-length encodes one channel as icns does, runs and literals."""
    out = bytearray()
    i = 0
    while i < len(channel):
        run = 1
        while i + run < len(channel) and channel[i + run] == channel[i] and \
                run < 130:
            run += 1
        if run >= 3:
            out += bytes((run + 125, channel[i]))
            i += run
        else:
            literal = channel[i:i + min(128, len(channel) - i)]
            end = 1
            while end < len(literal) and not (
                    end + 2 < len(literal) and
                    literal[end] == literal[end + 1] == literal[end + 2]):
                end += 1
            out += bytes((end - 1,)) + bytes(literal[:end])
            i += end
    return bytes(out)


def make_icns(width=16, png_width=None):
    kind = {16: b"is32", 128: b"it32"}[width]
    mask_kind = {16: b"s8mk", 128: b"t8mk"}[width]
    pixels = width * width
    red = bytes([200] * pixels)
    green = bytes(range(256)) * (pixels // 256) + bytes(range(pixels % 256))
    blue = bytes([7, 7, 9] * (pixels // 3)) + bytes([7] * (pixels % 3))
    body = rle(red) + rle(green) + rle(blue)
    if kind == b"it32":
        body = b"\x00" * 4 + body
    mask = bytes((x * 3) % 256 for x in range(pixels))
    chunks = [(kind, body), (mask_kind, mask)]
    if png_width:
        rgba = bytes([1, 2, 3, 255]) * (png_width * png_width)
        chunks.append((b"ic08", icns.encode_png(png_width, png_width, rgba)))
    data = b"".join(k + struct.pack(">I", len(b) + 8) + b for k, b in chunks)
    return (b"icns" + struct.pack(">I", len(data) + 8) + data,
            red, green, blue, mask)


def png_rows(png):
    """The RGBA rows of a PNG written by encode_png."""
    at = 8
    idat = b""
    width = 0
    while at < len(png):
        size = struct.unpack(">I", png[at:at + 4])[0]
        kind = png[at + 4:at + 8]
        body = png[at + 8:at + 8 + size]
        if kind == b"IHDR":
            width = struct.unpack(">I", body[:4])[0]
        elif kind == b"IDAT":
            idat += body
        at += size + 12
    raw = zlib.decompress(idat)
    stride = width * 4 + 1
    return width, b"".join(raw[i + 1:i + stride]
                           for i in range(0, len(raw), stride))


def mach_o(architectures):
    """A fat Mach-O header for |architectures| (CPU type numbers)."""
    header = struct.pack(">II", 0xCAFEBABE, len(architectures))
    for cpu in architectures:
        header += struct.pack(">iiIII", cpu, 3, 4096, 64, 12)
    return header + b"\x00" * 128


def make_bundle(folder, name="Test", identifier="org.example.test",
                architectures=(7,), icon=True):
    path = os.path.join(folder, name + ".app")
    os.makedirs(os.path.join(path, "Contents", "MacOS"))
    os.makedirs(os.path.join(path, "Contents", "Resources"))
    info = {"CFBundleExecutable": name, "CFBundleName": name,
            "CFBundleIdentifier": identifier,
            "CFBundleShortVersionString": "2.0"}
    if icon:
        info["CFBundleIconFile"] = "AppIcon"
        with open(os.path.join(path, "Contents", "Resources", "AppIcon.icns"),
                  "wb") as f:
            f.write(make_icns(128)[0])
    with open(os.path.join(path, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    executable = os.path.join(path, "Contents", "MacOS", name)
    with open(executable, "wb") as f:
        f.write(mach_o(architectures))
    os.chmod(executable, 0o755)
    return path


class Isolated(unittest.TestCase):
    """Keeps every file the frontend writes in a temporary folder."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="frontend-test-")
        self.saved_env = dict(os.environ)
        for name, sub in (("XDG_CONFIG_HOME", "config"),
                          ("XDG_DATA_HOME", "data"),
                          ("XDG_STATE_HOME", "state")):
            os.environ[name] = os.path.join(self.root, sub)
        self.desktop_folder = os.path.join(self.root, "Desktop")
        os.makedirs(self.desktop_folder)
        self.saved_desktop_folder = desktop.desktop_folder
        desktop.desktop_folder = lambda: self.desktop_folder
        self.saved_mmap = launch.mmap_min_addr
        launch.mmap_min_addr = lambda: 4096
        self.saved_space = halo.SPACE_NEEDED
        halo.SPACE_NEEDED = 0
        self.loader = os.path.join(self.root, "loader")
        os.makedirs(self.loader)
        with open(os.path.join(self.loader, "ld-mac"), "w") as f:
            f.write("#!/bin/sh\nenv > ../env.txt\necho \"ran $1 in $PWD\"\n")
        os.chmod(os.path.join(self.loader, "ld-mac"), 0o755)
        open(os.path.join(self.loader, "libmac.so"), "w").close()
        os.environ["MALOADER_CARBON_DIR"] = self.loader

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved_env)
        desktop.desktop_folder = self.saved_desktop_folder
        launch.mmap_min_addr = self.saved_mmap
        halo.SPACE_NEEDED = self.saved_space
        shutil.rmtree(self.root, ignore_errors=True)


class IcnsTest(unittest.TestCase):
    def test_rle_images_decode_with_their_masks(self):
        for width in (16, 128):
            data, red, green, blue, mask = make_icns(width)
            chunks = icns.read_chunks(data)
            kind = {16: b"is32", 128: b"it32"}[width]
            mask_kind = {16: b"s8mk", 128: b"t8mk"}[width]
            got_width, rgba = icns.decode_rgb(kind, chunks[kind],
                                              chunks[mask_kind])
            self.assertEqual(got_width, width)
            self.assertEqual(rgba[0::4], red)
            self.assertEqual(rgba[1::4], green)
            self.assertEqual(rgba[2::4], blue)
            self.assertEqual(rgba[3::4], mask)

    def test_largest_image_becomes_png(self):
        data, red, green, blue, mask = make_icns(128)
        width, rows = png_rows(icns.largest_png(data))
        self.assertEqual(width, 128)
        self.assertEqual(rows[1::4], green)
        self.assertEqual(rows[3::4], mask)
        # A PNG entry wider than any RGB image is taken as it is.
        data = make_icns(16, png_width=64)[0]
        self.assertEqual(icns.png_width(icns.largest_png(data)), 64)

    def test_damaged_files(self):
        with self.assertRaises(ValueError):
            icns.read_chunks(b"not an icon")
        data = make_icns(16)[0]
        # Cut inside the image: its chunk is dropped, and nothing is found.
        self.assertIsNone(icns.largest_png(data[:30]))


class BundleTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="bundle-test-")

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_universal_bundle(self):
        app = bundles.read_bundle(make_bundle(self.root, architectures=(18, 7)))
        self.assertEqual(app.name, "Test")
        self.assertEqual(app.version, "2.0")
        self.assertEqual(app.architectures, ["ppc", "i386"])
        self.assertTrue(app.runnable)
        self.assertTrue(app.icon.endswith("AppIcon.icns"))

    def test_powerpc_only_and_thin_files(self):
        app = bundles.read_bundle(make_bundle(self.root, architectures=(18,)))
        self.assertFalse(app.runnable)
        thin = os.path.join(self.root, "thin")
        with open(thin, "wb") as f:
            f.write(struct.pack("<Ii", 0xFEEDFACE, 7) + b"\x00" * 32)
        self.assertEqual(bundles.architectures(thin), ["i386"])
        with open(thin, "wb") as f:
            f.write(b"#!/bin/sh\n")
        self.assertEqual(bundles.architectures(thin), [])

    def test_not_a_bundle(self):
        os.makedirs(os.path.join(self.root, "Plain.app"))
        with self.assertRaises(bundles.BundleError):
            bundles.read_bundle(os.path.join(self.root, "Plain.app"))
        path = make_bundle(self.root, name="Missing")
        os.remove(os.path.join(path, "Contents", "MacOS", "Missing"))
        with self.assertRaises(bundles.BundleError):
            bundles.read_bundle(path)


class ProfileAndLaunchTest(Isolated):
    def test_profiles(self):
        self.assertEqual(profiles.load_all(), [])
        first = profiles.save({"id": profiles.new_id("My App!"),
                               "name": "My App!", "bundle": "/x.app"})
        self.assertEqual(first["id"], "my-app")
        self.assertEqual(profiles.new_id("my app"), "my-app-2")
        self.assertEqual(profiles.load("my-app")["display"], "game")
        with self.assertRaises(ValueError):
            profiles.save(dict(first, env={"not valid": "1"}))
        with self.assertRaises(KeyError):
            profiles.load("../etc/passwd")
        profiles.remove("my-app")
        self.assertEqual(profiles.load_all(), [])

    def test_environment(self):
        profile = profiles.with_defaults({
            "id": "a", "name": "A", "home": "/h", "disc": "/d/Disc One",
            "disc_name": "ONE", "display": "window", "sound": False,
            "trace": True, "env": {"HLE_AUDIO_FRAMES": "4096"},
            "product_key": halo.PRODUCT_KEY})
        env = launch.environment(profile, FAKE_KEY,
                                 base={"HLE_WINDOWED": "0", "PATH": "/bin"})
        self.assertEqual(env["HALO_MAC_HOME"], "/h")
        self.assertEqual(env["HLE_CD_PATH"], "/d/Disc One")
        self.assertEqual(env["HLE_CD_NAME"], "ONE")
        self.assertEqual(env["HLE_WINDOWED"], "1")
        self.assertEqual(env["HLE_SOUND"], "0")
        self.assertEqual(env["HLE_TRACE"], "1")
        self.assertEqual(env["HLE_AUDIO_FRAMES"], "4096")
        self.assertEqual(env["HLE_DIALOG_10001"], FAKE_KEY)
        profile["display"] = "game"
        self.assertNotIn("HLE_WINDOWED", launch.environment(
            profile, base={"HLE_WINDOWED": "0"}))

    def test_product_keys(self):
        self.assertEqual(launch.normalize_product_key("ab12 cd34ef56-gh78"),
                         FAKE_KEY)
        self.assertIsNone(launch.normalize_product_key("AB12-CD34"))
        home = os.path.join(self.root, "home")
        profile = profiles.with_defaults({
            "id": "halo-ce", "home": home, "product_key": halo.PRODUCT_KEY})
        self.assertTrue(launch.product_key_needed(profile))
        plist = os.path.join(home, halo.PRODUCT_KEY["plist"])
        os.makedirs(os.path.dirname(plist))
        with open(plist, "wb") as f:
            plistlib.dump({"EULA": True}, f)
        self.assertTrue(launch.product_key_needed(profile))
        with open(plist, "wb") as f:
            plistlib.dump({"GameSpy Key": "stored by the game"}, f)
        self.assertFalse(launch.product_key_needed(profile))
        profile["product_key"] = None
        self.assertFalse(launch.product_key_needed(profile))

    def test_start_writes_a_log_without_the_key(self):
        app = make_bundle(self.root, architectures=(7,))
        profile = profiles.save({
            "id": "test", "name": "Test", "bundle": app,
            "product_key": halo.PRODUCT_KEY, "display": "fullscreen"})
        for _ in range(7):
            process = launch.start(profile, product_key=FAKE_KEY, detach=False)
            self.assertEqual(process.wait(), 0)
        log = launch.log_path("test")
        with open(log) as f:
            text = f.read()
        self.assertIn("ran ./Test in %s" % os.path.join(app, "Contents",
                                                         "MacOS"), text)
        self.assertIn("HLE_WINDOWED=0", text)
        self.assertNotIn(FAKE_KEY, text)
        self.assertTrue(os.path.exists(log + ".4"))
        self.assertFalse(os.path.exists(log + ".5"))
        with open(os.path.join(app, "Contents", "env.txt")) as f:
            self.assertIn("HLE_DIALOG_10001=" + FAKE_KEY, f.read())

    def test_problems(self):
        app = make_bundle(self.root, name="Ppc", architectures=(18,))
        profile = profiles.with_defaults({"id": "p", "name": "Ppc",
                                          "bundle": app, "disc": "/nowhere"})
        found = launch.problems(profile)
        self.assertTrue(any("32-bit Intel" in p for p in found))
        self.assertTrue(any("/nowhere" in p for p in found))
        with self.assertRaises(launch.LaunchError):
            launch.start(profile)
        launch.mmap_min_addr = lambda: 65536
        self.assertTrue(any("mmap_min_addr" in p for p in launch.problems()))
        os.environ["MALOADER_CARBON_DIR"] = self.root
        self.assertTrue(launch.problems())


class DesktopTest(Isolated):
    def test_exec_quoting(self):
        self.assertEqual(desktop.quote_exec("/usr/bin/carbon-launcher"),
                         "/usr/bin/carbon-launcher")
        self.assertEqual(desktop.quote_exec("/home/a b/c"), '"/home/a b/c"')
        self.assertEqual(desktop.quote_exec('/x/$y"z%'), '"/x/\\$y\\"z%%"')

    def test_entries(self):
        app = make_bundle(self.root)
        profile = profiles.save({"id": "test", "name": "Test\nApp",
                                 "bundle": app, "category": "Game"})
        profile["icon"] = desktop.extract_icon(profile)
        self.assertTrue(os.path.isfile(profile["icon"]))
        paths = desktop.install(profile, on_desktop=True)
        self.assertEqual(len(paths), 2)
        with open(paths[0]) as f:
            text = f.read()
        self.assertIn("Name=Test\\nApp\n", text)
        self.assertIn(" run test\n", text)
        self.assertIn("Categories=Game;\n", text)
        self.assertIn("Icon=%s\n" % profile["icon"], text)
        self.assertTrue(os.access(paths[1], os.X_OK))
        self.assertEqual(sorted(desktop.installed("test")), sorted(paths))
        desktop.remove("test")
        self.assertEqual(desktop.installed("test"), [])


def make_disc(root, damaged=False):
    """A Halo Universal disc folder with a small game in its package."""
    staging = os.path.join(root, "package")
    make_bundle(staging, name="Halo", identifier=halo.IDENTIFIER,
                architectures=(18, 7))
    resources = os.path.join(staging, "Halo.app", "Contents", "Resources")
    with open(os.path.join(resources, "._EULA.rsrc"), "wb") as f:
        f.write(b"resource fork")
    listing = subprocess.run(["find", ".", "-print"], cwd=staging,
                             capture_output=True, check=True).stdout
    archive = subprocess.run(["cpio", "-o", "-H", "odc", "--quiet"],
                             cwd=staging, input=listing, capture_output=True,
                             check=True).stdout
    packed = gzip.compress(archive)
    if damaged:
        packed = packed[:len(packed) // 2]
    volume = os.path.join(root, "disc", "Halo Universal")
    contents = os.path.join(volume, "Halo.pkg", "Contents")
    os.makedirs(contents)
    with open(os.path.join(contents, "Archive.pax.gz"), "wb") as f:
        f.write(packed)
    for name in (".DS_Store", "Xiph License.pdf", "EULA.pdf"):
        with open(os.path.join(volume, name), "w") as f:
            f.write(name)
    os.makedirs(os.path.join(volume, ".background"))
    with open(os.path.join(volume, ".background", "picture"), "w") as f:
        f.write("picture")
    os.makedirs(os.path.join(volume, ".Trashes"))
    return volume


class HaloTest(Isolated):
    def check_installed(self, destination):
        app = bundles.read_bundle(os.path.join(destination, "Halo.app"))
        self.assertEqual(app.identifier, halo.IDENTIFIER)
        self.assertTrue(os.path.isfile(os.path.join(
            destination, "Halo.app", "Contents", "Resources", "._EULA.rsrc")))
        disc = os.path.join(destination, "Disc", "Halo Universal")
        for name in (".DS_Store", "Xiph License.pdf",
                     os.path.join(".background", "picture")):
            self.assertTrue(os.path.isfile(os.path.join(disc, name)), name)
        self.assertFalse(os.path.exists(os.path.join(disc, "Halo.pkg")))
        self.assertFalse(os.path.exists(os.path.join(disc, ".Trashes")))
        self.assertEqual(sorted(os.listdir(destination)),
                         ["Disc", "Halo.app", "Home"])

    def test_install_from_a_folder(self):
        volume = make_disc(self.root)
        for given in (volume, os.path.dirname(volume)):
            source = halo.probe(given)
            self.assertEqual(source.kind, "folder")
            self.assertEqual(source.volume, "Halo Universal")
        destination = os.path.join(self.root, "Games", "HaloCE")
        steps = []
        paths = halo.install(source, destination,
                             progress=lambda s, f: steps.append((s, f)))
        self.check_installed(destination)
        self.assertEqual(steps[-2 if steps[-1][1] is None else -1][1], 1.0)
        profile = halo.register(paths, "window", on_desktop=False)
        self.assertEqual(profiles.load(halo.APP_ID)["display"], "window")
        self.assertTrue(os.path.isfile(profile["icon"]))
        self.assertEqual(len(desktop.installed(halo.APP_ID)), 1)
        self.assertTrue(launch.product_key_needed(profile))
        # A reinstall replaces the game and keeps the home's saves.
        save = os.path.join(paths["home"], "Documents", "Halo", "save")
        os.makedirs(os.path.dirname(save))
        open(save, "w").close()
        halo.install(source, destination)
        self.check_installed(destination)
        self.assertTrue(os.path.isfile(save))

    def test_install_from_an_image(self):
        if not halo.seven_zip():
            self.skipTest("7-Zip is not installed")
        volume = make_disc(self.root)
        image = os.path.join(self.root, "HaloMac.zip")
        subprocess.run([halo.seven_zip(), "a", "-tzip", image,
                        "Halo Universal"], cwd=os.path.dirname(volume),
                       capture_output=True, check=True)
        source = halo.probe(image)
        self.assertEqual((source.kind, source.volume),
                         ("image", "Halo Universal"))
        self.assertNotIn("Halo Universal/Halo.pkg", source.top_level)
        destination = os.path.join(self.root, "Games", "HaloCE")
        halo.install(source, destination)
        self.check_installed(destination)

    def test_bringing_over_an_earlier_home(self):
        earlier = os.path.join(self.root, "old-home")
        plist = os.path.join(earlier, halo.PRODUCT_KEY["plist"])
        os.makedirs(os.path.dirname(plist))
        with open(plist, "wb") as f:
            plistlib.dump({"GameSpy Key": "kept by the game"}, f)
        saves = os.path.join(earlier, "Documents", "Halo", "savegames")
        os.makedirs(saves)
        open(os.path.join(saves, "checkpoint"), "w").close()
        source = halo.probe(make_disc(self.root))
        destination = os.path.join(self.root, "HaloCE")
        paths = halo.install(source, destination, import_home=earlier)
        profile = halo.register(paths)
        self.assertFalse(launch.product_key_needed(profile))
        self.assertTrue(os.path.isfile(os.path.join(
            paths["home"], "Documents", "Halo", "savegames", "checkpoint")))

    def test_what_is_refused(self):
        with self.assertRaises(halo.InstallError):
            halo.probe(self.root)
        source = halo.probe(make_disc(self.root))
        problems, _ = halo.check_destination(
            os.path.join(self.root, "my.games", "HaloCE"))
        self.assertTrue(problems)
        with self.assertRaises(halo.InstallError):
            halo.install(source, os.path.join(self.root, "my.games", "Halo"))
        halo.SPACE_NEEDED = 1 << 62
        self.assertTrue(halo.check_destination(
            os.path.join(self.root, "HaloCE"))[0])

    def test_cancel_and_damage_leave_nothing_behind(self):
        source = halo.probe(make_disc(self.root))
        destination = os.path.join(self.root, "cancelled")
        with self.assertRaises(halo.Cancelled):
            halo.install(source, destination, cancelled=lambda: True)
        self.assertEqual(sorted(os.listdir(destination)), ["Disc"])
        damaged = halo.probe(make_disc(os.path.join(self.root, "d"),
                                       damaged=True))
        destination = os.path.join(self.root, "damaged")
        with self.assertRaises(halo.InstallError):
            halo.install(damaged, destination)
        self.assertEqual(sorted(os.listdir(destination)), ["Disc"])


class CommandLineTest(Isolated):
    def run_main(self, main, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            try:
                status = main(list(argv))
            except SystemExit as e:
                status = e.code
        return status, out.getvalue()

    def test_launcher_commands(self):
        app = make_bundle(self.root, name="Tool")
        status, out = self.run_main(launcher_app.main, "add", app, "--display",
                                    "window", "--env", "HLE_VRAM_MB=128",
                                    "--menu-entry")
        self.assertEqual(status, 0, out)
        self.assertIn("Added Tool as tool", out)
        status, out = self.run_main(launcher_app.main, "set", "tool",
                                    "--sound", "off", "--env", "HLE_VRAM_MB=")
        profile = profiles.load("tool")
        self.assertFalse(profile["sound"])
        self.assertEqual(profile["env"], {})
        status, out = self.run_main(launcher_app.main, "show", "tool")
        self.assertIn("In a window", out)
        status, out = self.run_main(launcher_app.main, "list")
        self.assertIn("tool", out)
        status, out = self.run_main(launcher_app.main, "check", "tool")
        self.assertEqual(status, 0, out)
        status, out = self.run_main(launcher_app.main, "remove", "tool")
        self.assertEqual(profiles.load_all(), [])
        self.assertEqual(desktop.installed("tool"), [])
        status, out = self.run_main(launcher_app.main, "run", "tool")
        self.assertNotEqual(status, 0)
        ppc = make_bundle(self.root, name="Old", architectures=(18,))
        status, out = self.run_main(launcher_app.main, "add", ppc)
        self.assertIn("no 32-bit Intel code", str(status))

    def test_installer_command(self):
        volume = make_disc(self.root)
        destination = os.path.join(self.root, "HaloCE")
        status, out = self.run_main(installer_app.main, "--source", volume,
                                    "--destination", destination, "--check")
        self.assertEqual(status, 0, out)
        self.assertFalse(os.path.exists(destination))
        status, out = self.run_main(installer_app.main, "--source", volume,
                                    "--destination", destination, "--window")
        self.assertEqual(status, 0, out)
        self.assertIn("carbon-launcher run halo-ce", out)
        self.assertEqual(profiles.load("halo-ce")["display"], "window")


if __name__ == "__main__":
    unittest.main()
