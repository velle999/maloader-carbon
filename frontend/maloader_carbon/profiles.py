# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""The applications the launcher knows, a JSON file each under
$XDG_CONFIG_HOME/maloader-carbon/apps, and where the frontend keeps its
other files."""

import json
import os
import re

# How the window is chosen: the app's own saved setting, full screen, or a
# window (HLE_WINDOWED).
DISPLAY_MODES = ("game", "fullscreen", "window")

DEFAULTS = {
    "id": "",
    "name": "",
    "comment": "",
    "category": "",
    "bundle": "",
    "home": "",
    "disc": "",
    "disc_name": "",
    "display": "game",
    "sound": True,
    "trace": False,
    "env": {},
    "icon": "",
    # For an app that asks for a product key the loader answers through a
    # classic dialog: {"dialog": id, "plist": path in the Mac home, "key":
    # the preference that holds an accepted key}. The key itself is never
    # kept here.
    "product_key": None,
}

ENV_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _xdg(variable, fallback):
    value = os.environ.get(variable)
    if value and os.path.isabs(value):
        return value
    return os.path.join(os.path.expanduser("~"), fallback)


def config_dir():
    return os.path.join(_xdg("XDG_CONFIG_HOME", ".config"), "maloader-carbon")


def data_dir():
    return os.path.join(_xdg("XDG_DATA_HOME", ".local/share"),
                        "maloader-carbon")


def state_dir():
    return os.path.join(_xdg("XDG_STATE_HOME", ".local/state"),
                        "maloader-carbon")


def applications_dir():
    return os.path.join(_xdg("XDG_DATA_HOME", ".local/share"), "applications")


def loader_default_home():
    """The Mac home the loader uses when HALO_MAC_HOME is not set."""
    return os.path.join(_xdg("XDG_DATA_HOME", ".local/share"),
                        "halo-mac-loader", "home")


def apps_dir():
    return os.path.join(config_dir(), "apps")


def _path(app_id):
    if not re.match(r"^[a-z0-9][a-z0-9-]*$", app_id or ""):
        raise KeyError("no app %r" % app_id)
    return os.path.join(apps_dir(), app_id + ".json")


def with_defaults(profile):
    merged = dict(DEFAULTS)
    merged["env"] = {}
    merged.update(profile)
    if merged["display"] not in DISPLAY_MODES:
        merged["display"] = "game"
    if not isinstance(merged["env"], dict):
        merged["env"] = {}
    return merged


def load(app_id):
    try:
        with open(_path(app_id)) as f:
            profile = json.load(f)
    except FileNotFoundError:
        raise KeyError("no app %r" % app_id)
    profile["id"] = app_id
    return with_defaults(profile)


def load_all():
    profiles = []
    try:
        names = os.listdir(apps_dir())
    except FileNotFoundError:
        return profiles
    for name in names:
        if name.endswith(".json"):
            try:
                profiles.append(load(name[:-5]))
            except (KeyError, ValueError, OSError):
                continue
    profiles.sort(key=lambda p: p["name"].lower())
    return profiles


def save(profile):
    profile = with_defaults(profile)
    for name in profile["env"]:
        if not ENV_NAME.match(name):
            raise ValueError("%r is not an environment variable name" % name)
    path = _path(profile["id"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".new"
    with open(temporary, "w") as f:
        json.dump(profile, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(temporary, path)
    return profile


def remove(app_id):
    try:
        os.remove(_path(app_id))
    except FileNotFoundError:
        pass


def new_id(name):
    """An id from |name| that no app has yet."""
    base = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "app"
    candidate = base
    n = 2
    while os.path.exists(_path(candidate)):
        candidate = "%s-%d" % (base, n)
        n += 1
    return candidate


def mac_home(profile):
    return profile.get("home") or loader_default_home()
