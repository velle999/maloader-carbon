# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""Starting an application under the loader: finding ld-mac, checking what
it needs, the environment a profile makes, and the logs."""

import json
import os
import plistlib
import re
import subprocess
import sys

from . import bundle as bundles
from . import profiles

# The Mac executables' __TEXT segment starts here, and the kernel refuses to
# map below vm.mmap_min_addr.
MMAP_MIN_ADDR_NEEDED = 4096
LOGS_KEPT = 5
FRONTEND_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRODUCT_KEY = re.compile(r"^[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}$")


class LaunchError(Exception):
    pass


def loader_ok(directory):
    return (bool(directory) and
            os.access(os.path.join(directory, "ld-mac"), os.X_OK) and
            os.path.isfile(os.path.join(directory, "libmac.so")))


def loader_candidates():
    """Where ld-mac and libmac.so may be, most deliberate first."""
    candidates = []
    if os.environ.get("MALOADER_CARBON_DIR"):
        candidates.append(os.environ["MALOADER_CARBON_DIR"])
    try:
        with open(os.path.join(profiles.config_dir(), "config.json")) as f:
            configured = json.load(f).get("loader_dir")
        if configured:
            candidates.append(configured)
    except (OSError, ValueError, AttributeError):
        pass
    # Installed: PREFIX/share/maloader-carbon/frontend beside
    # PREFIX/lib/maloader-carbon. In the source tree: the tree's top.
    candidates.append(os.path.normpath(os.path.join(
        FRONTEND_DIR, "..", "..", "..", "lib", "maloader-carbon")))
    candidates.append(os.path.normpath(os.path.join(FRONTEND_DIR, "..")))
    return candidates


def find_loader():
    for directory in loader_candidates():
        if loader_ok(directory):
            return os.path.abspath(directory)
    return None


def mmap_min_addr():
    try:
        with open("/proc/sys/vm/mmap_min_addr") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def mmap_fix():
    return ("Mac OS X executables start at address 4096, below what this "
            "kernel lets a program use. As root:\n\n"
            "  sysctl -w vm.mmap_min_addr=4096\n"
            "  echo vm.mmap_min_addr=4096 > /etc/sysctl.d/60-mmap-min-addr.conf")


def problems(profile=None):
    """What stops apps, or |profile|'s app, from starting, as sentences."""
    found = []
    if not find_loader():
        found.append("The loader (ld-mac and libmac.so) was not found. Build "
                     "it with make, install it with make install, or set "
                     "MALOADER_CARBON_DIR to its folder.")
    limit = mmap_min_addr()
    if limit is not None and limit > MMAP_MIN_ADDR_NEEDED:
        found.append("vm.mmap_min_addr is %d. " % limit + mmap_fix())
    if profile is not None:
        try:
            app = bundles.read_bundle(profile["bundle"])
            if not app.runnable:
                found.append("%s has no 32-bit Intel code (it has: %s)." % (
                    app.name, ", ".join(app.architectures) or "none"))
        except bundles.BundleError as e:
            found.append(str(e))
        if profile.get("disc") and not os.path.isdir(profile["disc"]):
            found.append("The disc folder %s is missing." % profile["disc"])
    return found


def normalize_product_key(text):
    """XXXX-XXXX-XXXX-XXXX from what was typed, or None."""
    letters = re.sub(r"[^A-Za-z0-9]", "", text or "").upper()
    if len(letters) != 16:
        return None
    key = "-".join(letters[i:i + 4] for i in range(0, 16, 4))
    return key if PRODUCT_KEY.match(key) else None


def product_key_needed(profile):
    """Whether the app will ask for a product key: it can, and its
    preferences hold none it accepted."""
    wanted = profile.get("product_key")
    if not wanted:
        return False
    path = os.path.join(profiles.mac_home(profile), wanted["plist"])
    try:
        with open(path, "rb") as f:
            preferences = plistlib.load(f)
    except Exception:
        return True
    return not (isinstance(preferences, dict) and
                preferences.get(wanted["key"]))


def environment(profile, product_key=None, base=None):
    env = dict(os.environ if base is None else base)
    for name in ("HALO_MAC_HOME", "HLE_CD_PATH", "HLE_CD_NAME",
                 "HLE_WINDOWED", "HLE_SOUND", "HLE_TRACE"):
        env.pop(name, None)
    if profile.get("home"):
        env["HALO_MAC_HOME"] = profile["home"]
    if profile.get("disc"):
        env["HLE_CD_PATH"] = profile["disc"]
        if profile.get("disc_name"):
            env["HLE_CD_NAME"] = profile["disc_name"]
    if profile.get("display") == "fullscreen":
        env["HLE_WINDOWED"] = "0"
    elif profile.get("display") == "window":
        env["HLE_WINDOWED"] = "1"
    if not profile.get("sound", True):
        env["HLE_SOUND"] = "0"
    if profile.get("trace"):
        env["HLE_TRACE"] = "1"
    env.update({k: str(v) for k, v in (profile.get("env") or {}).items()})
    if product_key and profile.get("product_key"):
        env["HLE_DIALOG_%d" % profile["product_key"]["dialog"]] = product_key
    return env


def log_path(app_id):
    return os.path.join(profiles.state_dir(), "logs", app_id + ".log")


def rotate_logs(path):
    for n in range(LOGS_KEPT - 1, 0, -1):
        older = "%s.%d" % (path, n)
        newer = path if n == 1 else "%s.%d" % (path, n - 1)
        if os.path.exists(newer):
            os.replace(newer, older)


def command(profile):
    """(argv, working directory) for |profile|'s app."""
    loader = find_loader()
    if not loader:
        raise LaunchError(problems()[0])
    app = bundles.read_bundle(profile["bundle"])
    return ([os.path.join(loader, "ld-mac"),
             "./" + os.path.basename(app.executable)],
            os.path.dirname(app.executable))


def start(profile, product_key=None, extra_env=None, detach=True):
    """Starts the app, its output going to its log. Returns the process."""
    found = problems(profile)
    if found:
        raise LaunchError("\n\n".join(found))
    argv, cwd = command(profile)
    env = environment(profile, product_key)
    env.update(extra_env or {})
    path = log_path(profile["id"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rotate_logs(path)
    log = open(path, "w")
    key_variable = ("HLE_DIALOG_%d" % profile["product_key"]["dialog"]
                    if profile.get("product_key") else None)
    shown = [name for name in sorted(env)
             if (name.startswith("HLE_") or name.startswith("HALO_") or
                 name.startswith("SDL_")) and name != key_variable]
    log.write("carbon-launcher: %s (%s), %s\n" % (
        profile["name"], profile["bundle"],
        " ".join("%s=%s" % (n, env[n]) for n in shown) or "no settings"))
    log.flush()
    try:
        return subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=detach)
    finally:
        log.close()
