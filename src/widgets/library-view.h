// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"
#include "cover-thumbnail.h"

namespace gnomos
{

// Hierarchical browser for the music library: a header row with a back
// button + current level title, and either a list or a cover-art grid of
// entries below. Whether activating an entry means "go deeper" or "play
// it" depends on LibraryEntry::is_container, and whether to show a grid
// at all is decided by GnomosWindow (it's the one that knows whether the
// current object_id is under the Albums/Artists namespace — see
// SetEntries()) — this widget only reports which index was activated.
class LibraryView : public Gtk::Box
{
public:
  LibraryView();

  // grid: cover-art tiles (Euphonica-style, https://github.com/htkhiem/euphonica)
  // instead of the plain list — meant for Albums/Artists levels, where
  // every entry has its own distinct, meaningful cover. Grid tiles never
  // get the add-to-queue/play-next buttons list rows do (see those
  // signals' own comments): GnomosWindow only ever asks for a grid when
  // entries are containers (albums/artists to browse into), never leaf
  // tracks those buttons would apply to.
  void SetEntries(const std::vector<LibraryEntry>& entries, bool grid);
  void SetLevelTitle(const std::string& title);
  void SetBackVisible(bool visible);
  void Clear();

  sigc::signal<void(unsigned)>& signal_entry_activated() { return signal_entry_activated_; }
  sigc::signal<void()>& signal_back_requested() { return signal_back_requested_; }
  sigc::signal<void()>& signal_search_requested() { return signal_search_requested_; }
  // Only ever emitted for a leaf entry in list mode — SetEntries() doesn't
  // add the triggering button to container rows, since there's nothing to
  // append to the queue until you've browsed into one.
  sigc::signal<void(unsigned)>& signal_add_to_queue_requested() { return signal_add_to_queue_requested_; }
  // Also only ever emitted for a leaf entry in list mode — see
  // signal_add_to_queue_requested()'s comment.
  sigc::signal<void(unsigned)>& signal_play_next_requested() { return signal_play_next_requested_; }
  // play_all_button_/queue_all_button_ only ever show up once the current
  // level is entirely leaf tracks (e.g. an album's contents) — see
  // SetEntries(). Both act on the whole level, not a single row, so
  // neither signal carries an index.
  sigc::signal<void()>& signal_play_all_requested() { return signal_play_all_requested_; }
  sigc::signal<void()>& signal_queue_all_requested() { return signal_queue_all_requested_; }

private:
  void BuildList(const std::vector<LibraryEntry>& entries);
  void BuildGrid(const std::vector<LibraryEntry>& entries);

  Gtk::Button back_button_;
  Gtk::Label level_title_;
  Gtk::Label count_label_;
  Gtk::Button play_all_button_;
  Gtk::Button queue_all_button_;
  Gtk::Button search_button_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  Gtk::FlowBox flow_box_;
  Gtk::Label placeholder_;
  sigc::signal<void(unsigned)> signal_entry_activated_;
  sigc::signal<void()> signal_back_requested_;
  sigc::signal<void()> signal_search_requested_;
  sigc::signal<void(unsigned)> signal_add_to_queue_requested_;
  sigc::signal<void(unsigned)> signal_play_next_requested_;
  sigc::signal<void()> signal_play_all_requested_;
  sigc::signal<void()> signal_queue_all_requested_;
};

}  // namespace gnomos
