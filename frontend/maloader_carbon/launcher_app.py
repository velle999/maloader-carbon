# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""carbon-launcher: runs Mac OS X Carbon applications under the loader.

Without arguments it opens its window; with a command it works on the
command line."""

import argparse
import os
import sys

from . import bundle as bundles
from . import desktop
from . import launch
from . import profiles

DISPLAY_NAMES = {
    "game": "The app's own setting",
    "fullscreen": "Full screen",
    "window": "In a window",
}


def add_settings_arguments(parser):
    parser.add_argument("--name", help="the name shown for the app")
    parser.add_argument("--home", help="the app's Mac home folder (its "
                        "Library and Documents); the loader's default when "
                        "not given")
    parser.add_argument("--disc", help="a folder that stands in for the "
                        "app's CD")
    parser.add_argument("--disc-name", help="the CD's volume name, when it "
                        "is not the folder's name")
    parser.add_argument("--display", choices=profiles.DISPLAY_MODES)
    parser.add_argument("--sound", choices=("on", "off"))
    parser.add_argument("--trace", choices=("on", "off"),
                        help="a detailed log")
    parser.add_argument("--env", action="append", default=[],
                        metavar="NAME=VALUE",
                        help="an environment variable for the app")


def apply_settings(profile, args):
    if args.name:
        profile["name"] = args.name
    for name in ("home", "disc"):
        value = getattr(args, name)
        if value is not None:
            profile[name] = (os.path.abspath(os.path.expanduser(value))
                             if value else "")
    if args.disc_name is not None:
        profile["disc_name"] = args.disc_name
    if args.display:
        profile["display"] = args.display
    if args.sound:
        profile["sound"] = args.sound == "on"
    if args.trace:
        profile["trace"] = args.trace == "on"
    for item in args.env:
        name, sep, value = item.partition("=")
        if not sep:
            raise SystemExit("--env takes NAME=VALUE, not %r" % item)
        if value:
            profile["env"][name] = value
        else:
            profile["env"].pop(name, None)


def load_or_exit(app_id):
    try:
        return profiles.load(app_id)
    except KeyError:
        raise SystemExit("carbon-launcher: no app %r (see carbon-launcher "
                         "list)" % app_id)


def command_list(args):
    apps = profiles.load_all()
    if not apps:
        print("No apps yet: carbon-launcher add /path/to/Some.app")
    for profile in apps:
        print("%-20s %s\n%-20s %s" % (profile["id"], profile["name"], "",
                                       profile["bundle"]))
    return 0


def command_add(args):
    try:
        app = bundles.read_bundle(args.bundle)
    except bundles.BundleError as e:
        raise SystemExit("carbon-launcher: %s" % e)
    if not app.runnable:
        raise SystemExit("carbon-launcher: %s has no 32-bit Intel code (it "
                         "has: %s)" % (app.name, ", ".join(
                             app.architectures) or "none"))
    profile = profiles.with_defaults({
        "id": profiles.new_id(args.name or app.name),
        "name": app.name,
        "bundle": app.path,
    })
    apply_settings(profile, args)
    profile["icon"] = desktop.extract_icon(profile)
    profiles.save(profile)
    print("Added %s as %s" % (profile["name"], profile["id"]))
    if args.menu_entry or args.desktop_shortcut:
        for path in desktop.install(profile, args.desktop_shortcut):
            print("Wrote %s" % path)
    return 0


def command_set(args):
    profile = load_or_exit(args.id)
    apply_settings(profile, args)
    profiles.save(profile)
    if desktop.installed(profile["id"]):
        desktop.install(profile, any(
            os.path.dirname(p) != profiles.applications_dir()
            for p in desktop.installed(profile["id"])))
    print("Saved %s" % profile["id"])
    return 0


def command_show(args):
    profile = load_or_exit(args.id)
    for name in ("name", "bundle", "home", "disc", "disc_name"):
        print("%-10s %s" % (name, profile[name] or "-"))
    print("%-10s %s" % ("display", DISPLAY_NAMES[profile["display"]]))
    print("%-10s %s" % ("sound", "on" if profile["sound"] else "off"))
    print("%-10s %s" % ("trace", "on" if profile["trace"] else "off"))
    for name, value in sorted(profile["env"].items()):
        print("%-10s %s=%s" % ("env", name, value))
    print("%-10s %s" % ("log", launch.log_path(profile["id"])))
    for problem in launch.problems(profile):
        print("problem:   %s" % problem)
    return 0


def gui_available():
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        return False
    try:
        import gi
        gi.require_version("Gtk", "3.0")
        from gi.repository import Gtk  # noqa: F401
        return True
    except (ImportError, ValueError):
        return False


def command_run(args):
    """Runs the app and waits for it. Started from a desktop entry, with no
    terminal, it asks and reports through dialogs."""
    profile = load_or_exit(args.id)
    extra = {}
    for item in args.settings:
        name, sep, value = item.partition("=")
        if not sep or not profiles.ENV_NAME.match(name):
            raise SystemExit("carbon-launcher: %r is not NAME=VALUE" % item)
        extra[name] = value
    interactive = sys.stdin.isatty()
    dialogs = not interactive and gui_available()

    def fail(text):
        if dialogs:
            from . import gtk_common
            gtk_common.message(None, "%s cannot start" % profile["name"], text)
        raise SystemExit("carbon-launcher: %s" % text)

    found = launch.problems(profile)
    if found:
        fail("\n\n".join(found))
    key = None
    if launch.product_key_needed(profile) and not any(
            n.startswith("HLE_DIALOG_") for n in extra):
        if interactive:
            print("%s asks for its product key, printed in its manual. The "
                  "game keeps a key it accepts." % profile["name"])
            key = launch.normalize_product_key(input("Product key: "))
            if not key:
                raise SystemExit("carbon-launcher: a product key is four "
                                 "groups of four letters and digits")
        elif dialogs:
            from . import gtk_common
            key = gtk_common.ask_product_key(None, profile["name"])
            if not key:
                return 1
        else:
            fail("it needs its product key, and there is no terminal or "
                 "window to ask for it in")
    try:
        process = launch.start(profile, product_key=key, extra_env=extra,
                               detach=False)
    except (launch.LaunchError, OSError) as e:
        fail(str(e))
    if interactive:
        print("Started %s; its output goes to %s" % (
            profile["name"], launch.log_path(profile["id"])))
    try:
        status = process.wait()
    except KeyboardInterrupt:
        process.terminate()
        status = process.wait()
    if interactive and status:
        print("%s exited with status %d" % (profile["name"], status))
    return status


def command_desktop(args):
    profile = load_or_exit(args.id)
    if args.remove:
        desktop.remove(profile["id"])
        print("Removed the desktop entries of %s" % profile["id"])
        return 0
    if not profile["icon"] or not os.path.isfile(profile["icon"]):
        profile["icon"] = desktop.extract_icon(profile)
        profiles.save(profile)
    for path in desktop.install(profile, args.desktop_shortcut):
        print("Wrote %s" % path)
    return 0


def command_remove(args):
    profile = load_or_exit(args.id)
    desktop.remove(profile["id"])
    profiles.remove(profile["id"])
    print("Removed %s from the launcher; its files stay where they are" %
          profile["id"])
    return 0


def command_log(args):
    profile = load_or_exit(args.id)
    path = launch.log_path(profile["id"])
    if args.path:
        print(path)
        return 0
    try:
        with open(path, errors="replace") as f:
            lines = f.readlines()
    except FileNotFoundError:
        raise SystemExit("carbon-launcher: %s has no log yet" % profile["id"])
    sys.stdout.writelines(lines[-args.lines:])
    return 0


def command_check(args):
    found = launch.problems(load_or_exit(args.id) if args.id else None)
    loader = launch.find_loader()
    print("loader: %s" % (loader or "not found"))
    for problem in found:
        print("problem: %s" % problem)
    return 1 if found else 0


def parser():
    top = argparse.ArgumentParser(
        prog="carbon-launcher",
        description="Runs Mac OS X Carbon applications for Intel under "
        "maloader-carbon. Without a command, opens the launcher's window.")
    commands = top.add_subparsers(dest="command")
    commands.add_parser("list", help="list the apps").set_defaults(
        run=command_list)
    add = commands.add_parser("add", help="add an application bundle")
    add.add_argument("bundle", help="the .app folder")
    add_settings_arguments(add)
    add.add_argument("--menu-entry", action="store_true",
                     help="add it to the applications menu")
    add.add_argument("--desktop-shortcut", action="store_true",
                     help="and put a launcher on the desktop")
    add.set_defaults(run=command_add)
    change = commands.add_parser("set", help="change an app's settings")
    change.add_argument("id")
    add_settings_arguments(change)
    change.set_defaults(run=command_set)
    show = commands.add_parser("show", help="an app's settings and problems")
    show.add_argument("id")
    show.set_defaults(run=command_show)
    run = commands.add_parser("run", help="run an app and wait for it")
    run.add_argument("id")
    run.add_argument("settings", nargs="*", metavar="NAME=VALUE",
                     help="environment variables for this run only")
    run.set_defaults(run=command_run)
    entry = commands.add_parser("desktop", help="menu and desktop entries")
    entry.add_argument("id")
    entry.add_argument("--desktop-shortcut", action="store_true",
                       help="also put a launcher on the desktop")
    entry.add_argument("--remove", action="store_true",
                       help="remove the app's entries")
    entry.set_defaults(run=command_desktop)
    remove = commands.add_parser("remove", help="forget an app")
    remove.add_argument("id")
    remove.set_defaults(run=command_remove)
    log = commands.add_parser("log", help="the app's last log")
    log.add_argument("id")
    log.add_argument("--lines", type=int, default=40)
    log.add_argument("--path", action="store_true", help="only its path")
    log.set_defaults(run=command_log)
    check = commands.add_parser("check", help="what stops apps from running")
    check.add_argument("id", nargs="?")
    check.set_defaults(run=command_check)
    return top


def main(argv):
    args = parser().parse_args(argv)
    if not args.command:
        if not gui_available():
            parser().print_help()
            return 2
        from . import gtk_launcher
        return gtk_launcher.main()
    return args.run(args)
