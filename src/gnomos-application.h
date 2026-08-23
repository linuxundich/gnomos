// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm/application.h>

#include "gnomos-window.h"

namespace gnomos
{

class GnomosApplication : public Gtk::Application
{
public:
  static Glib::RefPtr<GnomosApplication> create();

protected:
  GnomosApplication();

  void on_startup() override;
  void on_activate() override;

private:
  GnomosWindow* window_ = nullptr;
};

}  // namespace gnomos
