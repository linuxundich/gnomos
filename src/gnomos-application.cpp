// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnomos-application.h"

#include <adwaita.h>

#include <gdkmm/display.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <gtkmm/icontheme.h>

#include "config.h"

namespace gnomos
{

GnomosApplication::GnomosApplication() : Gtk::Application(APPLICATION_ID)
{
}

Glib::RefPtr<GnomosApplication> GnomosApplication::create()
{
  return Glib::RefPtr<GnomosApplication>(new GnomosApplication());
}

void GnomosApplication::on_startup()
{
  Gtk::Application::on_startup();
  // Must run after GTK itself is initialized (i.e. after the base
  // on_startup()) and before any Adw widget is constructed.
  adw_init();

  // Lets the icon theme resolve APPLICATION_ID by name (used by
  // AdwAboutDialog's application-icon and, once installed, the .desktop
  // file's own Icon=) when running straight from the build tree, where
  // data/icons/ was never installed to a standard icon theme path. A real
  // installed/Flatpak build already finds it there instead, so this is a
  // harmless no-op — file_test() guards against SOURCE_ROOT not existing
  // at all in that case. add_search_path() wants the directory that
  // *contains* hicolor/, not hicolor/ itself (confirmed live — pointing at
  // hicolor/ directly made has_icon() return false).
  std::string icon_dir = Glib::build_filename(SOURCE_ROOT, "data", "icons");
  if (Glib::file_test(icon_dir, Glib::FileTest::IS_DIR))
    Gtk::IconTheme::get_for_display(Gdk::Display::get_default())->add_search_path(icon_dir);
}

void GnomosApplication::on_activate()
{
  if (!window_)
  {
    // Held for the life of the process — without this, the GApplication
    // would quit the instant its one window closes/hides, which is exactly
    // what run_in_background_ (see GnomosWindow's own comment) needs to not
    // happen. Released exactly once, directly by
    // GnomosWindow::OnCloseRequest() itself when the window is really
    // closing (not just backgrounding) — that, not a signal_hide handler
    // here, is the reliable hook: confirmed live that GTK's own
    // close-request handling destroys the window without ever emitting a
    // "hide" signal, only OnCloseRequest()'s own explicit set_visible(false)
    // for backgrounding does.
    hold();
    // Top-level application windows are not owned by a container, so they
    // are not Gtk::make_managed(); this app has exactly one, and it is
    // freed when it's closed.
    window_ = new GnomosWindow();
    add_window(*window_);
  }
  window_->present();
}

}  // namespace gnomos
