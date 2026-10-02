// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <adwaita.h>
#include <glibmm/ustring.h>
#include <gtkmm/widget.h>
#include <sigc++/sigc++.h>

namespace gnomos
{

// An AdwDialog (header bar with close button, Escape to close, sheet on
// narrow windows) behind the small slice of Gtk::Window's API that
// GnomosWindow's hand-built input dialogs use — those were plain modal
// Gtk::Windows, and this let them move over without rewriting their
// contents.
//
// Create with `new`; it deletes itself once the dialog has closed (and
// signal_closed() has run). Never delete it yourself.
class DialogShell
{
public:
  explicit DialogShell(Gtk::Widget& parent);
  DialogShell(const DialogShell&) = delete;
  DialogShell& operator=(const DialogShell&) = delete;

  void set_title(const Glib::ustring& title);
  // Width, and height if > 0 — otherwise the content decides.
  void set_default_size(int width, int height);
  void set_child(Gtk::Widget& child);
  void set_default_widget(Gtk::Widget& widget);
  void present();
  void close();

  sigc::signal<void()>& signal_closed() { return signal_closed_; }
  AdwDialog* gobj() { return dialog_; }

private:
  ~DialogShell();
  static void OnClosed(AdwDialog* dialog, gpointer user_data);

  Gtk::Widget& parent_;
  AdwDialog* dialog_ = nullptr;
  GtkWidget* toolbar_view_ = nullptr;
  sigc::signal<void()> signal_closed_;
};

}  // namespace gnomos
