// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnomos-application.h"

#include <adwaita.h>

#include <gdkmm/display.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/stylecontext.h>

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

  // data/style.css, compiled in as a GResource. GnomosApplication is a
  // plain Gtk::Application rather than an AdwApplication (no gtkmm binding,
  // see ARCHITECTURE.md), so libadwaita's automatic style.css loading
  // doesn't apply — this does the same thing by hand. APPLICATION priority
  // sits above the Adwaita theme, below user CSS.
  auto css = Gtk::CssProvider::create();
  css->load_from_resource("/de/linuxundich/Gnomos/style.css");
  Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(), css,
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  // Keyboard shortcuts with a modifier are real accelerators on their
  // window actions — they work regardless of which widget has focus and
  // show up next to their menu items. The bare single-key ones (Space, n,
  // p, ...) stay in GnomosWindow::OnKeyPressed(), which can skip them
  // while a text field has focus; an accelerator can't.
  set_accels_for_action("win.settings", {"<Control>comma"});
  set_accels_for_action("win.shortcuts", {"<Control>question"});
  set_accels_for_action("win.quit", {"<Control>q"});
  set_accels_for_action("win.close", {"<Control>w"});
  set_accels_for_action("win.search-library", {"<Control>f"});
  set_accels_for_action("win.jump-to-current", {"<Control>j"});
  set_accels_for_action("win.toggle-sidebar", {"F9"});
  set_accels_for_action("win.toggle-now-playing", {"<Control>i"});
  set_accels_for_action("win.volume-up", {"<Control>Up"});
  set_accels_for_action("win.volume-down", {"<Control>Down"});
  set_accels_for_action("win.seek-forward", {"<Alt>Right"});
  set_accels_for_action("win.seek-backward", {"<Alt>Left"});

  // The track-change notification's buttons/default action (see
  // GnomosWindow::SendTrackChangeNotification()) can only ever reach
  // "app."-scoped actions — a notification click is delivered over D-Bus
  // via the application's own exported action group, which doesn't include
  // a window's "win."-scoped actions even while that window is alive, so
  // these exist purely to forward into the real "win.play-pause"/"win.next"
  // ones GnomosWindow itself already has. window_ is guaranteed non-null by
  // the time any of these can actually fire — a track-change notification
  // is never sent before the window (and thus playback) exists.
  add_action("notification-play-pause", [this] {
    if (window_)
      window_->activate_action("win.play-pause");
  });
  add_action("notification-next", [this] {
    if (window_)
      window_->activate_action("win.next");
  });
  add_action("notification-raise", [this] {
    if (window_)
      window_->present();
  });
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
