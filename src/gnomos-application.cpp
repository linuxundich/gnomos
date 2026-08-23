// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnomos-application.h"

#include <adwaita.h>

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
}

void GnomosApplication::on_activate()
{
  if (!window_)
  {
    // Top-level application windows are not owned by a container, so they
    // are not Gtk::make_managed(); this app has exactly one, and it is
    // freed when it's closed.
    window_ = new GnomosWindow();
    add_window(*window_);
    window_->signal_hide().connect([this] {
      delete window_;
      window_ = nullptr;
    });
  }
  window_->present();
}

}  // namespace gnomos
