// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"

namespace gnomos
{

// Read-only list of recently played tracks (client-side history — see
// HistoryEntry). No per-row actions: unlike QueueItem/FavoriteItem, a
// HistoryEntry carries no object_id/URI it could be replayed from.
class HistoryView : public Gtk::Box
{
public:
  HistoryView();

  void SetItems(const std::vector<HistoryEntry>& items);
  void Clear();

  sigc::signal<void()>& signal_clear_requested() { return signal_clear_requested_; }

private:
  Gtk::Button clear_button_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  Gtk::Label placeholder_;
  sigc::signal<void()> signal_clear_requested_;
};

}  // namespace gnomos
