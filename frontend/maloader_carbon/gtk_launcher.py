# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""The launcher's window: the apps it knows, to start, set up, add to the
menu, read the logs of, or forget."""

import os

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GdkPixbuf, GLib, Gtk, Pango  # noqa: E402

from . import bundle as bundles  # noqa: E402
from . import desktop  # noqa: E402
from . import gtk_common  # noqa: E402
from . import launch  # noqa: E402
from . import profiles  # noqa: E402
from .launcher_app import DISPLAY_NAMES  # noqa: E402

ICON_SIZE = 48


def app_icon(profile):
    path = profile.get("icon")
    if path and os.path.isfile(path):
        try:
            pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(
                path, ICON_SIZE, ICON_SIZE, True)
            return Gtk.Image.new_from_pixbuf(pixbuf)
        except GLib.Error:
            pass
    image = Gtk.Image.new_from_icon_name("application-x-executable",
                                         Gtk.IconSize.DIALOG)
    image.set_pixel_size(ICON_SIZE)
    return image


class AppRow(Gtk.ListBoxRow):
    def __init__(self, profile, running):
        super().__init__()
        self.profile = profile
        grid = Gtk.Grid(column_spacing=12, margin=8)
        grid.attach(app_icon(profile), 0, 0, 1, 2)
        name = Gtk.Label(xalign=0, hexpand=True)
        name.set_markup("<b>%s</b>" % GLib.markup_escape_text(
            profile["name"]))
        name.set_ellipsize(Pango.EllipsizeMode.END)
        grid.attach(name, 1, 0, 1, 1)
        detail = Gtk.Label(xalign=0, hexpand=True)
        detail.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        detail.get_style_context().add_class("dim-label")
        detail.set_text(profile["bundle"])
        grid.attach(detail, 1, 1, 1, 1)
        status = Gtk.Label(label="Running" if running else "")
        status.get_style_context().add_class("dim-label")
        grid.attach(status, 2, 0, 1, 2)
        self.add(grid)


class SettingsDialog(Gtk.Dialog):
    def __init__(self, parent, profile):
        super().__init__(title="%s settings" % profile["name"],
                         transient_for=parent, modal=True)
        self.profile = dict(profile)
        self.add_button("Cancel", Gtk.ResponseType.CANCEL)
        save = self.add_button("Save", Gtk.ResponseType.OK)
        save.get_style_context().add_class("suggested-action")
        self.set_default_response(Gtk.ResponseType.OK)
        self.set_default_size(560, -1)

        grid = Gtk.Grid(row_spacing=10, column_spacing=12, border_width=18)
        self.get_content_area().add(grid)
        row = 0

        def label(text):
            nonlocal row
            widget = Gtk.Label(label=text, xalign=1)
            grid.attach(widget, 0, row, 1, 1)
            return widget

        label("Name")
        self.name = Gtk.Entry(text=profile["name"], activates_default=True)
        grid.attach(self.name, 1, row, 1, 1)
        row += 1

        label("Application")
        bundle = Gtk.Label(label=profile["bundle"], xalign=0, selectable=True)
        bundle.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        grid.attach(bundle, 1, row, 1, 1)
        row += 1

        label("Mac home folder")
        self.home = Gtk.Entry(text=profile["home"],
                              placeholder_text="The loader's default")
        grid.attach(gtk_common.folder_row(self, "Mac home folder", self.home),
                    1, row, 1, 1)
        row += 1
        note = Gtk.Label(xalign=0, wrap=True, max_width_chars=60)
        note.get_style_context().add_class("dim-label")
        note.set_text("Where the app keeps its preferences (Library) and "
                      "documents (Documents).")
        grid.attach(note, 1, row, 1, 1)
        row += 1

        label("Disc folder")
        self.disc = Gtk.Entry(text=profile["disc"], placeholder_text="None")
        grid.attach(gtk_common.folder_row(self, "Disc folder", self.disc),
                    1, row, 1, 1)
        row += 1

        label("Disc name")
        self.disc_name = Gtk.Entry(text=profile["disc_name"],
                                   placeholder_text="The folder's name")
        grid.attach(self.disc_name, 1, row, 1, 1)
        row += 1

        label("Display")
        self.display = Gtk.ComboBoxText()
        for mode in profiles.DISPLAY_MODES:
            self.display.append(mode, DISPLAY_NAMES[mode])
        self.display.set_active_id(profile["display"])
        grid.attach(self.display, 1, row, 1, 1)
        row += 1

        label("Sound")
        self.sound = Gtk.Switch(active=profile["sound"], halign=Gtk.Align.START)
        grid.attach(self.sound, 1, row, 1, 1)
        row += 1

        label("Detailed log")
        self.trace = Gtk.Switch(active=profile["trace"], halign=Gtk.Align.START)
        grid.attach(self.trace, 1, row, 1, 1)
        row += 1

        env_label = label("Environment")
        env_label.set_valign(Gtk.Align.START)
        scroller = Gtk.ScrolledWindow(min_content_height=90, hexpand=True)
        scroller.set_shadow_type(Gtk.ShadowType.IN)
        scroller.set_overlay_scrolling(False)
        self.env = Gtk.TextView(monospace=True)
        self.env.get_buffer().set_text("\n".join(
            "%s=%s" % item for item in sorted(profile["env"].items())))
        scroller.add(self.env)
        grid.attach(scroller, 1, row, 1, 1)
        row += 1
        hint = Gtk.Label(xalign=0, wrap=True, max_width_chars=60)
        hint.get_style_context().add_class("dim-label")
        hint.set_text("One NAME=VALUE a line, such as HLE_AUDIO_FRAMES=4096. "
                      "The loader's README lists them.")
        grid.attach(hint, 1, row, 1, 1)
        self.show_all()

    def result(self):
        """The profile as set, or raises ValueError saying what is wrong."""
        profile = dict(self.profile)
        profile["name"] = self.name.get_text().strip() or profile["name"]
        for name in ("home", "disc"):
            text = getattr(self, name).get_text().strip()
            profile[name] = os.path.abspath(os.path.expanduser(text)) \
                if text else ""
        if profile["disc"] and not os.path.isdir(profile["disc"]):
            raise ValueError("The disc folder %s does not exist." %
                             profile["disc"])
        profile["disc_name"] = self.disc_name.get_text().strip()
        profile["display"] = self.display.get_active_id()
        profile["sound"] = self.sound.get_active()
        profile["trace"] = self.trace.get_active()
        buffer = self.env.get_buffer()
        text = buffer.get_text(buffer.get_start_iter(), buffer.get_end_iter(),
                               False)
        env = {}
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, sep, value = line.partition("=")
            name = name.strip()
            if not sep or not profiles.ENV_NAME.match(name):
                raise ValueError("%r is not NAME=VALUE." % line)
            env[name] = value
        profile["env"] = env
        return profile


class LogDialog(Gtk.Dialog):
    def __init__(self, parent, profile):
        super().__init__(title="%s log" % profile["name"],
                         transient_for=parent, modal=False)
        self.add_button("Close", Gtk.ResponseType.CLOSE)
        self.connect("response", lambda d, r: d.destroy())
        self.set_default_size(820, 520)
        path = launch.log_path(profile["id"])
        try:
            with open(path, errors="replace") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - 256 * 1024))
                text = f.read()
        except FileNotFoundError:
            text = "No log yet: %s has not been started." % profile["name"]
        scroller = Gtk.ScrolledWindow(vexpand=True, hexpand=True)
        scroller.set_overlay_scrolling(False)
        view = Gtk.TextView(editable=False, monospace=True,
                            wrap_mode=Gtk.WrapMode.CHAR)
        view.get_buffer().set_text(text)
        scroller.add(view)
        area = self.get_content_area()
        caption = Gtk.Label(label=path, xalign=0, selectable=True, margin=6)
        area.pack_start(caption, False, False, 0)
        area.pack_start(scroller, True, True, 0)
        self.show_all()
        GLib.idle_add(lambda: view.scroll_to_iter(
            view.get_buffer().get_end_iter(), 0, False, 0, 1) and False)


class LauncherWindow(Gtk.ApplicationWindow):
    def __init__(self, application):
        super().__init__(application=application, title="Carbon Launcher")
        self.set_default_size(640, 460)
        self.running = {}  # app id: process

        header = Gtk.HeaderBar(show_close_button=True, title="Carbon Launcher",
                               subtitle="Mac OS X apps for Intel, on Linux")
        add = Gtk.Button(label="Add…")
        add.set_tooltip_text("Add a Mac application (.app)")
        add.connect("clicked", self.on_add)
        header.pack_start(add)
        self.set_titlebar(header)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.add(box)
        self.problems = Gtk.InfoBar(message_type=Gtk.MessageType.WARNING,
                                    no_show_all=True)
        self.problem_text = Gtk.Label(xalign=0, wrap=True, selectable=True)
        self.problems.get_content_area().add(self.problem_text)
        self.problem_text.show()
        box.pack_start(self.problems, False, False, 0)

        scroller = Gtk.ScrolledWindow(vexpand=True)
        scroller.set_overlay_scrolling(False)
        self.list = Gtk.ListBox()
        self.list.connect("row-activated", lambda l, r: self.on_launch())
        self.list.connect("row-selected", lambda l, r: self.update_buttons())
        empty = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER,
                          margin=24)
        empty.set_markup(
            "<big>No apps yet</big>\n\nAdd a Mac OS X application bundle "
            "(.app) with code for Intel processors.")
        empty.get_style_context().add_class("dim-label")
        empty.show()
        self.list.set_placeholder(empty)
        scroller.add(self.list)
        box.pack_start(scroller, True, True, 0)

        bar = Gtk.ActionBar()
        self.launch_button = Gtk.Button(label="Launch")
        self.launch_button.get_style_context().add_class("suggested-action")
        self.launch_button.connect("clicked", lambda _: self.on_launch())
        bar.pack_start(self.launch_button)
        self.settings_button = Gtk.Button(label="Settings…")
        self.settings_button.connect("clicked", lambda _: self.on_settings())
        bar.pack_start(self.settings_button)
        self.entry_button = Gtk.Button(label="Menu entry…")
        self.entry_button.connect("clicked", lambda _: self.on_entry())
        bar.pack_start(self.entry_button)
        self.log_button = Gtk.Button(label="Log")
        self.log_button.connect("clicked", lambda _: self.on_log())
        bar.pack_start(self.log_button)
        self.remove_button = Gtk.Button(label="Remove")
        self.remove_button.get_style_context().add_class("destructive-action")
        self.remove_button.connect("clicked", lambda _: self.on_remove())
        bar.pack_end(self.remove_button)
        box.pack_start(bar, False, False, 0)

        self.show_all()
        self.refresh()

    def selected(self):
        row = self.list.get_selected_row()
        return row.profile if row else None

    def refresh(self, select=None):
        chosen = select or (self.selected() or {}).get("id")
        for row in self.list.get_children():
            self.list.remove(row)
        for profile in profiles.load_all():
            row = AppRow(profile, profile["id"] in self.running)
            self.list.add(row)
            if profile["id"] == chosen:
                self.list.select_row(row)
        if not self.list.get_selected_row() and self.list.get_children():
            self.list.select_row(self.list.get_children()[0])
        self.list.show_all()
        found = launch.problems()
        self.problem_text.set_text("\n\n".join(found))
        self.problems.set_visible(bool(found))
        self.update_buttons()

    def update_buttons(self):
        profile = self.selected()
        for button in (self.settings_button, self.entry_button,
                       self.log_button, self.remove_button):
            button.set_sensitive(profile is not None)
        self.launch_button.set_sensitive(
            profile is not None and profile["id"] not in self.running)

    def on_add(self, _):
        dialog = Gtk.FileChooserNative.new(
            "Choose a Mac application (.app)", self,
            Gtk.FileChooserAction.SELECT_FOLDER, "Add", "Cancel")
        path = dialog.get_filename() if dialog.run() == \
            Gtk.ResponseType.ACCEPT else None
        dialog.destroy()
        if not path:
            return
        try:
            app = bundles.read_bundle(path)
        except bundles.BundleError as e:
            gtk_common.message(self, "Not an application", str(e))
            return
        if not app.runnable:
            gtk_common.message(
                self, "%s cannot run here" % app.name,
                "The loader runs 32-bit Intel code, and %s has code for: %s." %
                (app.name, ", ".join(app.architectures) or "no processor"))
            return
        profile = profiles.with_defaults({
            "id": profiles.new_id(app.name),
            "name": app.name,
            "bundle": app.path,
        })
        profile["icon"] = desktop.extract_icon(profile)
        profiles.save(profile)
        self.refresh(select=profile["id"])
        self.on_settings()

    def on_launch(self):
        profile = self.selected()
        if not profile or profile["id"] in self.running:
            return
        process = gtk_common.start_app(self, profile, self.on_exit)
        if process:
            self.running[profile["id"]] = process
            self.refresh()

    def on_exit(self, process, status):
        for app_id, running in list(self.running.items()):
            if running is process:
                del self.running[app_id]
                code = os.waitstatus_to_exitcode(status) if hasattr(
                    os, "waitstatus_to_exitcode") else status
                if code not in (0,):
                    self.problem_text.set_text(
                        "%s ended with status %d. Its log says why: "
                        "select it and press Log." % (
                            profiles.load(app_id)["name"], code))
                    self.problems.set_visible(True)
        self.refresh()

    def on_settings(self):
        profile = self.selected()
        if not profile:
            return
        dialog = SettingsDialog(self, profile)
        while dialog.run() == Gtk.ResponseType.OK:
            try:
                changed = dialog.result()
            except ValueError as e:
                gtk_common.message(dialog, "That setting cannot be saved",
                                   str(e))
                continue
            profiles.save(changed)
            entries = desktop.installed(changed["id"])
            if entries:
                desktop.install(changed, any(
                    os.path.dirname(p) != profiles.applications_dir()
                    for p in entries))
            break
        dialog.destroy()
        self.refresh()

    def on_entry(self):
        profile = self.selected()
        if not profile:
            return
        existing = desktop.installed(profile["id"])
        dialog = Gtk.MessageDialog(
            transient_for=self, modal=True,
            message_type=Gtk.MessageType.QUESTION,
            text="Start %s from the applications menu" % profile["name"])
        dialog.format_secondary_text(
            "The entry runs it through this launcher, with its settings."
            if not existing else "It has entries already:\n" +
            "\n".join(existing))
        on_desktop = Gtk.CheckButton(label="Also put a launcher on the desktop")
        on_desktop.set_active(any(os.path.dirname(p) !=
                                  profiles.applications_dir()
                                  for p in existing))
        dialog.get_message_area().pack_start(on_desktop, False, False, 0)
        on_desktop.show()
        if existing:
            dialog.add_button("Remove entries", 1)
        dialog.add_button("Cancel", Gtk.ResponseType.CANCEL)
        dialog.add_button("Save" if existing else "Add", Gtk.ResponseType.OK)
        answer = dialog.run()
        wanted = on_desktop.get_active()
        dialog.destroy()
        if answer == 1:
            desktop.remove(profile["id"])
        elif answer == Gtk.ResponseType.OK:
            if not profile["icon"] or not os.path.isfile(profile["icon"]):
                profile["icon"] = desktop.extract_icon(profile)
                profiles.save(profile)
            desktop.remove(profile["id"])
            desktop.install(profile, wanted)

    def on_log(self):
        profile = self.selected()
        if profile:
            LogDialog(self, profile)

    def on_remove(self):
        profile = self.selected()
        if not profile:
            return
        if gtk_common.confirm(
                self, "Remove %s from the launcher?" % profile["name"],
                "Its menu entries go too. The application, its settings and "
                "its saved files stay where they are.", "Remove"):
            desktop.remove(profile["id"])
            profiles.remove(profile["id"])
            self.refresh()


def main():
    application = Gtk.Application(application_id="org.maloader_carbon.launcher")
    application.connect("activate", lambda app: (
        app.get_windows()[0].present() if app.get_windows()
        else LauncherWindow(app)))
    return application.run([])
