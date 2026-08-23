// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"

namespace gnomos
{

// List display of existing alarms with an enable/disable switch and a
// delete button per row, plus an "add" button that asks GnomosWindow to
// present the new-alarm dialog (this widget has no room list of its own to
// populate one).
class AlarmsView : public Gtk::Box
{
public:
  AlarmsView();

  void SetItems(const std::vector<AlarmInfo>& items);
  void Clear();

  sigc::signal<void(std::string, bool)>& signal_enabled_toggled() { return signal_enabled_toggled_; }
  sigc::signal<void(std::string, bool)>& signal_include_linked_zones_toggled()
  {
    return signal_include_linked_zones_toggled_;
  }
  sigc::signal<void(std::string)>& signal_delete_requested() { return signal_delete_requested_; }
  sigc::signal<void()>& signal_add_requested() { return signal_add_requested_; }
  sigc::signal<void(std::string)>& signal_edit_requested() { return signal_edit_requested_; }
  // Opens the same add-alarm dialog as signal_add_requested(), but
  // pre-filled from this alarm's own settings — for a similar alarm
  // (e.g. "same time, different room") without re-entering everything.
  sigc::signal<void(std::string)>& signal_duplicate_requested() { return signal_duplicate_requested_; }

private:
  Gtk::Button add_button_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  Gtk::Label placeholder_;
  sigc::signal<void(std::string, bool)> signal_enabled_toggled_;
  sigc::signal<void(std::string, bool)> signal_include_linked_zones_toggled_;
  sigc::signal<void(std::string)> signal_delete_requested_;
  sigc::signal<void()> signal_add_requested_;
  sigc::signal<void(std::string)> signal_edit_requested_;
  sigc::signal<void(std::string)> signal_duplicate_requested_;
};

}  // namespace gnomos
