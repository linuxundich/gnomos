// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"
#include "cover-thumbnail.h"

namespace gnomos
{

// List display of the household's favorites (radio stations, saved
// playlists, ...) with a live client-side search filter. Mirrors QueueView
// in row shape; kept as a separate small class rather than a shared base
// since the two item shapes (QueueItem vs FavoriteItem) and their intended
// actions differ enough that sharing would add an abstraction with only
// one real use each.
class FavoritesView : public Gtk::Box
{
public:
  FavoritesView();

  void SetItems(const std::vector<FavoriteItem>& items);
  void Clear();

  // Both indices are FavoriteItem::index — the item's position in the
  // *unfiltered* list SetItems() was last called with, which is what
  // NosonBackend::PlayFavorite()/AddFavoriteToQueue() expect. Never the
  // row's on-screen position, which shifts under an active filter.
  sigc::signal<void(unsigned)>& signal_item_activated() { return signal_item_activated_; }
  sigc::signal<void(unsigned)>& signal_add_to_queue_requested() { return signal_add_to_queue_requested_; }
  sigc::signal<void(unsigned)>& signal_play_next_requested() { return signal_play_next_requested_; }
  sigc::signal<void(unsigned)>& signal_delete_requested() { return signal_delete_requested_; }
  // Both act on the household's whole favorites list (NosonBackend's own
  // favorites_raw_), not just what's currently visible under a search
  // filter — ApplyFilter() only shows play_all_button_/queue_all_button_
  // at all while the filter is empty, so there's no ambiguity between
  // "all favorites" and "all filtered results".
  sigc::signal<void()>& signal_play_all_requested() { return signal_play_all_requested_; }
  sigc::signal<void()>& signal_queue_all_requested() { return signal_queue_all_requested_; }

private:
  void ApplyFilter();

  Gtk::Button play_all_button_;
  Gtk::Button queue_all_button_;
  Gtk::SearchEntry search_entry_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  Gtk::Label placeholder_;
  // The full, unfiltered list — ApplyFilter() re-renders from this on every
  // keystroke rather than mutating the list_box_ incrementally, same
  // "queues are small enough" reasoning QueueView documents for its own
  // wholesale-rebuild approach.
  std::vector<FavoriteItem> all_items_;
  // Index-aligned with list_box_'s rows as last built by ApplyFilter() —
  // row N's real FavoriteItem::index, needed because the on-screen row
  // position list_box_'s own signal_row_activated() reports no longer
  // matches item.index once a filter has removed earlier rows.
  std::vector<unsigned> row_index_map_;
  sigc::signal<void(unsigned)> signal_item_activated_;
  sigc::signal<void(unsigned)> signal_add_to_queue_requested_;
  sigc::signal<void(unsigned)> signal_play_next_requested_;
  sigc::signal<void(unsigned)> signal_delete_requested_;
  sigc::signal<void()> signal_play_all_requested_;
  sigc::signal<void()> signal_queue_all_requested_;
};

}  // namespace gnomos
