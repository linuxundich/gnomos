// SPDX-License-Identifier: GPL-3.0-or-later

#include "dialog-shell.h"

#include <glibmm/main.h>

namespace gnomos
{

DialogShell::DialogShell(Gtk::Widget& parent) : parent_(parent)
{
  dialog_ = adw_dialog_new();
  g_object_ref_sink(dialog_);
  toolbar_view_ = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view_), adw_header_bar_new());
  adw_dialog_set_child(dialog_, toolbar_view_);
  g_signal_connect(dialog_, "closed", G_CALLBACK(OnClosed), this);
}

DialogShell::~DialogShell()
{
  g_signal_handlers_disconnect_by_data(dialog_, this);
  g_object_unref(dialog_);
}

void DialogShell::set_title(const Glib::ustring& title)
{
  adw_dialog_set_title(dialog_, title.c_str());
}

void DialogShell::set_default_size(int width, int height)
{
  if (width > 0)
    adw_dialog_set_content_width(dialog_, width);
  if (height > 0)
    adw_dialog_set_content_height(dialog_, height);
}

void DialogShell::set_child(Gtk::Widget& child)
{
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view_), GTK_WIDGET(child.gobj()));
}

void DialogShell::set_default_widget(Gtk::Widget& widget)
{
  adw_dialog_set_default_widget(dialog_, GTK_WIDGET(widget.gobj()));
}

void DialogShell::present()
{
  adw_dialog_present(dialog_, GTK_WIDGET(parent_.gobj()));
}

void DialogShell::close()
{
  adw_dialog_close(dialog_);
}

void DialogShell::OnClosed(AdwDialog*, gpointer user_data)
{
  auto* self = static_cast<DialogShell*>(user_data);
  self->signal_closed_.emit();
  // Deferred: "closed" is still being emitted on dialog_ right now, and
  // handlers queued by the dialog's widgets may still run this iteration.
  Glib::signal_idle().connect_once([self] { delete self; });
}

}  // namespace gnomos
