# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""The installer's window: an assistant from the disc to a menu entry."""

import os
import threading

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, GObject, Gtk, Pango  # noqa: E402

from . import gtk_common  # noqa: E402
from . import halo  # noqa: E402
from . import launch  # noqa: E402
from . import profiles  # noqa: E402
from .installer_app import DEFAULT_DESTINATION, prerequisites  # noqa: E402

TITLE = "Halo: Combat Evolved — 25th Anniversary Linux Port"


def page_box():
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12,
                  border_width=18)
    return box


def paragraph(text, dim=False, markup=False):
    label = Gtk.Label(xalign=0, wrap=True, max_width_chars=72)
    label.set_line_wrap_mode(Pango.WrapMode.WORD_CHAR)
    if markup:
        label.set_markup(text)
    else:
        label.set_text(text)
    if dim:
        label.get_style_context().add_class("dim-label")
    return label


class Installer(Gtk.Assistant):
    def __init__(self, application):
        super().__init__(application=application, title=TITLE,
                         use_header_bar=False)
        self.set_default_size(720, 520)
        self.source = None
        self.cancel_requested = False
        self.installing = False
        self.profile = None
        self.probe_serial = 0

        self.connect("cancel", self.on_cancel)
        self.connect("close", lambda _: self.destroy())
        self.connect("apply", self.on_apply)
        self.connect("prepare", self.on_prepare)

        self.add_welcome()
        self.add_source()
        self.add_destination()
        self.add_confirm()
        self.add_progress()
        self.add_done()
        self.show_all()

    # Pages ------------------------------------------------------------

    def add_welcome(self):
        box = page_box()
        box.pack_start(paragraph(
            "<big><b>Halo: Combat Evolved</b></big>\n"
            "The Mac release, Halo Universal 2.0, running as a native 32-bit "
            "Linux program with its own OpenGL renderer.", markup=True),
            False, False, 0)
        box.pack_start(paragraph(
            "You need the Halo Universal disc or its disc image and the "
            "product key printed in the manual. The installer copies the game "
            "from the disc, keeps the disc's files beside it for the game's "
            "disc check, and adds Halo to the applications menu."),
            False, False, 0)
        grid = Gtk.Grid(row_spacing=8, column_spacing=12, margin_top=6)
        ready = True
        for row, (name, ok, detail) in enumerate(prerequisites()):
            mark = Gtk.Label()
            mark.set_markup('<span foreground="%s"><b>%s</b></span>' % (
                ("#26a269", "✓") if ok else ("#c64600", "!")))
            grid.attach(mark, 0, row, 1, 1)
            grid.attach(Gtk.Label(label=name, xalign=0), 1, row, 1, 1)
            text = paragraph(detail, dim=True)
            text.set_selectable(True)
            grid.attach(text, 2, row, 1, 1)
            if not ok and name in ("maloader-carbon", "cpio"):
                ready = False
        box.pack_start(grid, False, False, 0)
        limit = launch.mmap_min_addr()
        if limit is not None and limit > launch.MMAP_MIN_ADDR_NEEDED:
            fix = paragraph(launch.mmap_fix(), dim=True)
            fix.set_selectable(True)
            box.pack_start(fix, False, False, 0)
        self.append_page(box)
        self.set_page_title(box, "Welcome")
        self.set_page_type(box, Gtk.AssistantPageType.INTRO)
        self.set_page_complete(box, ready)

    def add_source(self):
        box = page_box()
        self.source_page = box
        box.pack_start(paragraph(
            "Choose the disc image, or a folder with the disc's files, such "
            "as the disc itself where it is mounted."), False, False, 0)

        grid = Gtk.Grid(row_spacing=10, column_spacing=12)
        self.image_button = Gtk.FileChooserButton(
            title="Choose the disc image", action=Gtk.FileChooserAction.OPEN,
            hexpand=True)
        images = Gtk.FileFilter()
        images.set_name("Disc images")
        for suffix in halo.IMAGE_SUFFIXES:
            images.add_pattern("*" + suffix)
            images.add_pattern("*" + suffix.upper())
        self.image_button.add_filter(images)
        everything = Gtk.FileFilter()
        everything.set_name("All files")
        everything.add_pattern("*")
        self.image_button.add_filter(everything)
        downloads = os.path.join(os.path.expanduser("~"), "Downloads")
        if os.path.isdir(downloads):
            self.image_button.set_current_folder(downloads)
        self.image_button.connect("file-set", lambda b: self.probe(
            b.get_filename(), self.folder_button))
        grid.attach(Gtk.Label(label="Disc image", xalign=1), 0, 0, 1, 1)
        grid.attach(self.image_button, 1, 0, 1, 1)

        self.folder_button = Gtk.FileChooserButton(
            title="Choose the disc's folder",
            action=Gtk.FileChooserAction.SELECT_FOLDER, hexpand=True)
        self.folder_button.connect("file-set", lambda b: self.probe(
            b.get_filename(), self.image_button))
        grid.attach(Gtk.Label(label="Or a folder", xalign=1), 0, 1, 1, 1)
        grid.attach(self.folder_button, 1, 1, 1, 1)
        box.pack_start(grid, False, False, 0)

        status = Gtk.Box(spacing=8)
        self.source_spinner = Gtk.Spinner()
        status.pack_start(self.source_spinner, False, False, 0)
        self.source_status = paragraph("")
        self.source_status.set_selectable(True)
        status.pack_start(self.source_status, True, True, 0)
        box.pack_start(status, False, False, 0)

        self.append_page(box)
        self.set_page_title(box, "Disc")

    def add_destination(self):
        box = page_box()
        self.destination_page = box
        grid = Gtk.Grid(row_spacing=10, column_spacing=12)
        self.destination = Gtk.Entry(text=DEFAULT_DESTINATION, hexpand=True)
        self.destination.connect("changed", lambda _: self.check_destination())
        grid.attach(Gtk.Label(label="Install into", xalign=1), 0, 0, 1, 1)
        grid.attach(gtk_common.folder_row(self, "Install into",
                                          self.destination), 1, 0, 1, 1)
        box.pack_start(grid, False, False, 0)
        self.destination_status = paragraph("")
        box.pack_start(self.destination_status, False, False, 0)

        self.fullscreen = Gtk.CheckButton(label="Play full screen",
                                          active=True)
        box.pack_start(self.fullscreen, False, False, 0)
        self.on_desktop = Gtk.CheckButton(
            label="Put a launcher on the desktop too")
        box.pack_start(self.on_desktop, False, False, 0)
        box.pack_start(paragraph("Halo gets an entry in the applications menu "
                                 "either way.", dim=True), False, False, 0)

        earlier = halo.earlier_homes()
        self.import_check = Gtk.CheckButton(
            label="Bring over saved games and settings from an earlier "
            "install", active=bool(earlier))
        box.pack_start(self.import_check, False, False, 0)
        self.import_home = Gtk.Entry(
            text=earlier[0] if earlier else "",
            placeholder_text="Its Mac home folder (holding Library and "
            "Documents)")
        row = gtk_common.folder_row(self, "The earlier install's Mac home",
                                    self.import_home)
        row.set_margin_start(24)
        self.import_check.bind_property(
            "active", row, "sensitive",
            GObject.BindingFlags.SYNC_CREATE)
        self.import_check.connect("toggled",
                                  lambda _: self.check_destination())
        self.import_home.connect("changed", lambda _: self.check_destination())
        box.pack_start(row, False, False, 0)

        self.append_page(box)
        self.set_page_title(box, "Install")

    def add_confirm(self):
        box = page_box()
        self.confirm_text = paragraph("", markup=True)
        box.pack_start(self.confirm_text, False, False, 0)
        self.append_page(box)
        self.set_page_title(box, "Ready")
        self.set_page_type(box, Gtk.AssistantPageType.CONFIRM)
        self.set_page_complete(box, True)
        self.confirm_page = box

    def add_progress(self):
        box = page_box()
        box.set_valign(Gtk.Align.CENTER)
        self.step = paragraph("")
        box.pack_start(self.step, False, False, 0)
        self.bar = Gtk.ProgressBar(show_text=True)
        box.pack_start(self.bar, False, False, 0)
        box.pack_start(paragraph(
            "Unpacking takes several minutes on an older machine.",
            dim=True), False, False, 0)
        self.append_page(box)
        self.set_page_title(box, "Installing")
        self.set_page_type(box, Gtk.AssistantPageType.PROGRESS)
        self.progress_page = box

    def add_done(self):
        box = page_box()
        self.done_text = paragraph("", markup=True)
        box.pack_start(self.done_text, False, False, 0)
        self.play = Gtk.Button(label="Play Halo", halign=Gtk.Align.START)
        self.play.get_style_context().add_class("suggested-action")
        self.play.connect("clicked", self.on_play)
        box.pack_start(self.play, False, False, 0)
        self.append_page(box)
        self.set_page_title(box, "Done")
        self.set_page_type(box, Gtk.AssistantPageType.SUMMARY)
        self.done_page = box

    # Behaviour ------------------------------------------------------

    def probe(self, path, other_button):
        if not path:
            return
        other_button.unselect_all()
        self.source = None
        self.set_page_complete(self.source_page, False)
        self.source_spinner.start()
        self.source_status.set_text("Looking for the game in %s…" % path)
        self.probe_serial += 1
        serial = self.probe_serial

        def work():
            try:
                result, error = halo.probe(path), None
            except (halo.InstallError, OSError) as e:
                result, error = None, str(e)
            GLib.idle_add(self.probed, serial, result, error)

        threading.Thread(target=work, daemon=True).start()

    def probed(self, serial, source, error):
        if serial != self.probe_serial:
            return False
        self.source_spinner.stop()
        if error:
            self.source_status.set_text(error)
        else:
            self.source = source
            self.source_status.set_text("Found %s." % source.describe())
            self.set_page_complete(self.source_page, True)
        return False

    def destination_path(self):
        return os.path.abspath(os.path.expanduser(
            self.destination.get_text().strip() or DEFAULT_DESTINATION))

    def check_destination(self):
        problems, warnings = halo.check_destination(self.destination_path())
        if self.import_check.get_active():
            earlier = self.import_home.get_text().strip()
            if not earlier or not os.path.isdir(earlier):
                problems.append("Choose the earlier install's Mac home "
                                "folder, or untick bringing its saves over.")
        lines = [GLib.markup_escape_text(p) for p in problems]
        lines += ["<i>%s</i>" % GLib.markup_escape_text(w) for w in warnings]
        self.destination_status.set_markup("\n\n".join(lines))
        self.set_page_complete(self.destination_page, not problems)

    def on_prepare(self, _, page):
        if page is self.destination_page:
            self.check_destination()
        elif page is self.confirm_page:
            lines = [
                "<b>From</b>  %s" % GLib.markup_escape_text(
                    self.source.describe()),
                "<b>Into</b>  %s" % GLib.markup_escape_text(
                    self.destination_path()),
                "<b>Plays</b>  %s" % ("full screen" if
                                      self.fullscreen.get_active()
                                      else "in a window"),
                "<b>Menu entry</b>  yes%s" % (
                    ", and a launcher on the desktop"
                    if self.on_desktop.get_active() else ""),
            ]
            if self.import_check.get_active():
                lines.append("<b>Saves and settings from</b>  %s" %
                             GLib.markup_escape_text(
                                 self.import_home.get_text().strip()))
            self.confirm_text.set_markup("\n".join(lines))
            self.set_page_complete(page, True)
        elif page is self.progress_page:
            self.commit()

    def on_apply(self, _):
        self.installing = True
        self.cancel_requested = False
        destination = self.destination_path()
        earlier = (self.import_home.get_text().strip()
                   if self.import_check.get_active() else None)
        display = "fullscreen" if self.fullscreen.get_active() else "window"
        on_desktop = self.on_desktop.get_active()
        source = self.source

        def progress(step, fraction):
            GLib.idle_add(self.show_progress, step, fraction)

        def work():
            try:
                paths = halo.install(source, destination, earlier, progress,
                                     lambda: self.cancel_requested)
                profile = halo.register(paths, display, on_desktop)
                GLib.idle_add(self.finished, profile, None)
            except (halo.InstallError, OSError) as e:
                GLib.idle_add(self.finished, None, e)

        threading.Thread(target=work, daemon=True).start()

    def show_progress(self, step, fraction):
        self.step.set_text(step)
        if fraction is None:
            self.bar.pulse()
            self.bar.set_text("")
        else:
            self.bar.set_fraction(fraction)
            self.bar.set_text("%d%%" % int(fraction * 100))
        return False

    def finished(self, profile, error):
        self.installing = False
        self.profile = profile
        if error is None:
            key = launch.product_key_needed(profile)
            self.done_text.set_markup(
                "<big><b>Halo is installed.</b></big>\n\n"
                "It is in the applications menu as %s%s.%s\n\n"
                "Its log, saves and settings: the launcher's Log button, and "
                "%s." % (
                    GLib.markup_escape_text(profile["name"]),
                    " and on the desktop" if self.on_desktop.get_active()
                    else "",
                    "\n\nThe first time it starts it asks for the product key "
                    "printed in the manual." if key else "",
                    GLib.markup_escape_text(profile["home"])))
            self.play.set_visible(True)
        else:
            cancelled = isinstance(error, halo.Cancelled)
            self.done_text.set_markup(
                "<big><b>%s</b></big>\n\n%s" % (
                    "The installation was cancelled." if cancelled
                    else "Halo could not be installed.",
                    "" if cancelled else GLib.markup_escape_text(str(error))))
            self.play.set_visible(False)
        self.set_page_complete(self.progress_page, True)
        self.next_page()
        return False

    def on_play(self, _):
        if self.profile and gtk_common.start_app(self, self.profile):
            self.destroy()

    def on_cancel(self, _):
        if self.installing:
            if gtk_common.confirm(self, "Stop installing?",
                                  "What was unpacked so far is removed.",
                                  "Stop"):
                self.cancel_requested = True
            return
        self.destroy()


def main():
    application = Gtk.Application(
        application_id="org.maloader_carbon.halo_installer")
    application.connect("activate", lambda app: (
        app.get_windows()[0].present() if app.get_windows()
        else Installer(app)))
    return application.run([])
