# Copyright 2026 Velle Sinclair.
#
# Simplified BSD License or GPLv3, like the rest of this tree.

"""What both windows share: messages, the product-key question, choosing a
folder into an entry, and starting an app with its checks."""

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk, Pango  # noqa: E402

from . import launch  # noqa: E402


def message(parent, title, text, kind=Gtk.MessageType.ERROR):
    dialog = Gtk.MessageDialog(transient_for=parent, modal=True,
                               message_type=kind,
                               buttons=Gtk.ButtonsType.CLOSE, text=title)
    dialog.format_secondary_text(text)
    for label in dialog.get_message_area().get_children():
        if isinstance(label, Gtk.Label):
            label.set_selectable(True)
            label.set_max_width_chars(70)
    dialog.run()
    dialog.destroy()


def confirm(parent, title, text, action):
    dialog = Gtk.MessageDialog(transient_for=parent, modal=True,
                               message_type=Gtk.MessageType.QUESTION,
                               text=title)
    dialog.format_secondary_text(text)
    dialog.add_button("Cancel", Gtk.ResponseType.CANCEL)
    dialog.add_button(action, Gtk.ResponseType.OK)
    dialog.set_default_response(Gtk.ResponseType.CANCEL)
    answer = dialog.run() == Gtk.ResponseType.OK
    dialog.destroy()
    return answer


def ask_product_key(parent, app_name):
    """The product key the player types, normalized, or None."""
    dialog = Gtk.Dialog(title="Product key", transient_for=parent, modal=True)
    dialog.add_button("Cancel", Gtk.ResponseType.CANCEL)
    start = dialog.add_button("Start", Gtk.ResponseType.OK)
    start.get_style_context().add_class("suggested-action")
    start.set_sensitive(False)
    dialog.set_default_response(Gtk.ResponseType.OK)
    box = dialog.get_content_area()
    box.set_spacing(12)
    box.set_border_width(18)
    label = Gtk.Label(xalign=0)
    label.set_line_wrap(True)
    label.set_max_width_chars(52)
    label.set_text(
        "%s asks for its product key before it starts for the first time. "
        "It is printed in the game's manual. The game keeps a key it has "
        "accepted; this launcher does not store it." % app_name)
    box.add(label)
    entry = Gtk.Entry(placeholder_text="XXXX-XXXX-XXXX-XXXX", max_length=24,
                      activates_default=True)
    entry.set_width_chars(24)
    entry.override_font(Pango.FontDescription("Monospace 12"))
    box.add(entry)

    def changed(_):
        start.set_sensitive(
            launch.normalize_product_key(entry.get_text()) is not None)

    entry.connect("changed", changed)
    dialog.show_all()
    key = None
    if dialog.run() == Gtk.ResponseType.OK:
        key = launch.normalize_product_key(entry.get_text())
    dialog.destroy()
    return key


def choose_folder(parent, title, entry):
    dialog = Gtk.FileChooserNative.new(title, parent,
                                       Gtk.FileChooserAction.SELECT_FOLDER,
                                       "Choose", "Cancel")
    current = entry.get_text()
    if current:
        dialog.set_current_folder(current)
    if dialog.run() == Gtk.ResponseType.ACCEPT:
        entry.set_text(dialog.get_filename())
    dialog.destroy()


def folder_row(parent, title, entry):
    """An entry with a Browse button beside it."""
    box = Gtk.Box(spacing=6)
    entry.set_hexpand(True)
    box.pack_start(entry, True, True, 0)
    button = Gtk.Button(label="Browse…")
    button.connect("clicked", lambda _: choose_folder(parent, title, entry))
    box.pack_start(button, False, False, 0)
    return box


def start_app(parent, profile, on_exit=None):
    """Starts |profile|'s app after its checks, asking for a product key if
    it needs one. Returns the process, or None."""
    found = launch.problems(profile)
    if found:
        message(parent, "%s cannot start" % profile["name"],
                "\n\n".join(found))
        return None
    key = None
    if launch.product_key_needed(profile):
        key = ask_product_key(parent, profile["name"])
        if not key:
            return None
    try:
        process = launch.start(profile, product_key=key)
    except (launch.LaunchError, OSError) as e:
        message(parent, "%s cannot start" % profile["name"], str(e))
        return None
    if on_exit:
        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, process.pid,
                             lambda pid, status: on_exit(process, status))
    return process
