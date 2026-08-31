// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"
#include "cover-thumbnail.h"

namespace gnomos
{

// List display of the current zone's queue, with a per-row delete button
// and a "clear queue" toolbar action. Rebuilds its rows wholesale on
// SetItems() — queues are small enough (Sonos caps them well under a
// thousand tracks) that incremental diffing isn't worth the complexity here.
class QueueView : public Gtk::Box
{
public:
  QueueView();

  void SetItems(const std::vector<QueueItem>& items);
  // -1 = nothing in the queue is currently playing (a non-queue source, or
  // no zone selected). Safe to call before or after SetItems() in any
  // order — GnomosWindow gets the two pieces of state (queue contents,
  // now-playing) from independent, asynchronously-arriving events.
  void SetCurrentIndex(int index);
  void Clear();
  // Same action as jump_to_current_button_'s own click handler — exposed so
  // GnomosWindow's "jump to Now Playing" shortcut can trigger it after
  // switching view_stack_ to this page. No-op while nothing here is playing
  // (current_index_ < 0).
  void ScrollToCurrent();

  sigc::signal<void(unsigned)>& signal_item_activated() { return signal_item_activated_; }
  sigc::signal<void(unsigned)>& signal_item_remove_requested() { return signal_item_remove_requested_; }
  // Only ever emitted with a non-empty list — select_mode_button_'s own
  // "delete selected" action is only sensitive once at least one row is
  // checked. Indices are into the list SetItems() was last called with,
  // same convention as every other index this widget emits; NosonBackend::
  // RemoveQueueItems() handles them in any order.
  sigc::signal<void(std::vector<unsigned>)>& signal_remove_selected_requested()
  {
    return signal_remove_selected_requested_;
  }
  sigc::signal<void()>& signal_clear_requested() { return signal_clear_requested_; }
  sigc::signal<void()>& signal_save_playlist_requested() { return signal_save_playlist_requested_; }
  // Both indices are into the list last passed to SetItems(), same as
  // every other signal here — 0-based, GTK/UPnP index conversion happens
  // in NosonBackend::ReorderQueueItem().
  sigc::signal<void(unsigned, unsigned)>& signal_reorder_requested() { return signal_reorder_requested_; }

private:
  // Shows/hides select_checks_ and remove_selected_button_ — off by
  // default, so a plain single-item removal (the existing per-row trash
  // button, untouched) still needs no extra click for the common case.
  void UpdateSelectModeVisibility();
  // remove_selected_button_'s own sensitivity — on exactly while at least
  // one row is checked, off (rather than emitting with an empty list)
  // otherwise.
  void UpdateRemoveSelectedSensitivity();

  Gtk::Button clear_button_;
  Gtk::Button save_playlist_button_;
  // Scrolls/focuses the currently-playing row — only sensitive while
  // current_index_ >= 0, i.e. something in *this* queue is actually
  // playing (see SetCurrentIndex()'s own header comment).
  Gtk::Button jump_to_current_button_;
  // Reveals select_checks_ (one per row) and remove_selected_button_ when
  // toggled on — see UpdateSelectModeVisibility().
  Gtk::ToggleButton select_mode_button_;
  Gtk::Button remove_selected_button_;
  Gtk::Label count_label_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  // Both index-aligned with the list SetItems() was last called with;
  // rebuilt there. now_playing_icons_' visibility is only ever toggled by
  // SetCurrentIndex(); select_checks_' by select_mode_button_ (all of
  // them at once) and individually by the user.
  std::vector<Gtk::Image*> now_playing_icons_;
  std::vector<Gtk::CheckButton*> select_checks_;
  int current_index_ = -1;
  sigc::signal<void(unsigned)> signal_item_activated_;
  sigc::signal<void(unsigned)> signal_item_remove_requested_;
  sigc::signal<void(std::vector<unsigned>)> signal_remove_selected_requested_;
  sigc::signal<void()> signal_clear_requested_;
  sigc::signal<void()> signal_save_playlist_requested_;
  sigc::signal<void(unsigned, unsigned)> signal_reorder_requested_;
};

}  // namespace gnomos
