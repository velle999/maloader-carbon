# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Desktop entries that start an app through the launcher, in the
applications menu and, if asked, on the desktop."""

import hashlib
import os
import shutil
import subprocess

from . import bundle as bundles
from . import icns
from . import profiles

LAUNCHER = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "carbon-launcher")


def launcher_command():
    """The launcher as desktop entries should run it: the installed command
    when it is on PATH and is this one, else this script."""
    on_path = shutil.which("carbon-launcher")
    if on_path and os.path.realpath(on_path) == os.path.realpath(LAUNCHER):
        return on_path
    return LAUNCHER


def quote_exec(argument):
    """An Exec argument, quoted as the Desktop Entry Specification says."""
    reserved = set(' \t\n"\'\\><~|&;$*?#()`')
    if argument and not (set(argument) & reserved):
        return argument.replace("%", "%%")
    escaped = "".join("\\" + c if c in '"`$\\' else c for c in argument)
    return '"%s"' % escaped.replace("%", "%%")


def entry_escape(text):
    return (text or "").replace("\\", "\\\\").replace("\n", "\\n")


def entry_name(app_id):
    return "maloader-carbon-%s.desktop" % app_id


def extract_icon(profile):
    """Writes the app's icon as PNG where the frontend keeps its files and
    returns the path, or "" when there is none."""
    try:
        app = bundles.read_bundle(profile["bundle"])
        if not app.icon:
            return ""
        with open(app.icon, "rb") as f:
            png = icns.largest_png(f.read())
    except (bundles.BundleError, OSError, ValueError):
        return ""
    if not png:
        return ""
    path = os.path.join(profiles.data_dir(), "icons", profile["id"] + ".png")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    return path


def entry_text(profile):
    categories = profile.get("category") or "Utility"
    lines = [
        "[Desktop Entry]",
        "Type=Application",
        "Name=%s" % entry_escape(profile["name"]),
        "Comment=%s" % entry_escape(
            profile.get("comment") or "A Mac OS X application"),
        "Exec=%s run %s" % (quote_exec(launcher_command()), profile["id"]),
        "Icon=%s" % (profile.get("icon") or "application-x-executable"),
        "Terminal=false",
        # SDL never completes startup notification.
        "StartupNotify=false",
        "Categories=%s;" % categories.strip(";"),
    ]
    return "\n".join(lines) + "\n"


def desktop_folder():
    try:
        out = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True,
                             text=True, timeout=10).stdout.strip()
        if out and os.path.isdir(out):
            return out
    except (OSError, subprocess.SubprocessError):
        pass
    fallback = os.path.join(os.path.expanduser("~"), "Desktop")
    return fallback if os.path.isdir(fallback) else None


def trust(path):
    """Marks a launcher on the desktop as trusted, as XFCE's and GNOME's
    desktops ask before running an untrusted one."""
    with open(path, "rb") as f:
        checksum = hashlib.sha256(f.read()).hexdigest()
    for args in (["gio", "set", "-t", "string", path,
                  "metadata::xfce-exe-checksum", checksum],
                 ["gio", "set", path, "metadata::trusted", "true"]):
        try:
            subprocess.run(args, capture_output=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            pass


def install(profile, on_desktop=False):
    """Writes the app's desktop entries and returns their paths."""
    text = entry_text(profile)
    written = []
    menu = profiles.applications_dir()
    os.makedirs(menu, exist_ok=True)
    path = os.path.join(menu, entry_name(profile["id"]))
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o644)
    written.append(path)
    try:
        subprocess.run(["update-desktop-database", menu], capture_output=True,
                       timeout=30)
    except (OSError, subprocess.SubprocessError):
        pass
    if on_desktop:
        folder = desktop_folder()
        if folder:
            path = os.path.join(folder, entry_name(profile["id"]))
            with open(path, "w") as f:
                f.write(text)
            os.chmod(path, 0o755)
            trust(path)
            written.append(path)
    return written


def installed(app_id):
    """The app's desktop entries that exist."""
    paths = [os.path.join(profiles.applications_dir(), entry_name(app_id))]
    folder = desktop_folder()
    if folder:
        paths.append(os.path.join(folder, entry_name(app_id)))
    return [p for p in paths if os.path.exists(p)]


def remove(app_id):
    for path in installed(app_id):
        os.remove(path)
