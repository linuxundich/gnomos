// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
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

  sigc::signal<void(unsigned)>& signal_item_activated() { return signal_item_activated_; }
  sigc::signal<void(unsigned)>& signal_item_remove_requested() { return signal_item_remove_requested_; }
  sigc::signal<void()>& signal_clear_requested() { return signal_clear_requested_; }
  sigc::signal<void()>& signal_save_playlist_requested() { return signal_save_playlist_requested_; }
  // Both indices are into the list last passed to SetItems(), same as
  // every other signal here — 0-based, GTK/UPnP index conversion happens
  // in NosonBackend::ReorderQueueItem().
  sigc::signal<void(unsigned, unsigned)>& signal_reorder_requested() { return signal_reorder_requested_; }

private:
  Gtk::Button clear_button_;
  Gtk::Button save_playlist_button_;
  // Scrolls/focuses the currently-playing row — only sensitive while
  // current_index_ >= 0, i.e. something in *this* queue is actually
  // playing (see SetCurrentIndex()'s own header comment).
  Gtk::Button jump_to_current_button_;
  Gtk::Label count_label_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  // Index-aligned with the list SetItems() was last called with; rebuilt
  // there, only visibility toggled by SetCurrentIndex().
  std::vector<Gtk::Image*> now_playing_icons_;
  int current_index_ = -1;
  sigc::signal<void(unsigned)> signal_item_activated_;
  sigc::signal<void(unsigned)> signal_item_remove_requested_;
  sigc::signal<void()> signal_clear_requested_;
  sigc::signal<void()> signal_save_playlist_requested_;
  sigc::signal<void(unsigned, unsigned)> signal_reorder_requested_;
};

}  // namespace gnomos
