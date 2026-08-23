// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"
#include "cover-thumbnail.h"

namespace gnomos
{

// Hierarchical browser for the music library: a header row with a back
// button + current level title, and either a list or a cover-art grid of
// entries below. Whether activating an entry means "go deeper" or "play
// it" depends on LibraryEntry::is_container. Whether a grid is even an
// option for the current level is a single, uniform signal now —
// LibraryEntry::display_as_grid, which both the local library and
// third-party services populate (via their own, different underlying
// heuristics — see that field's own comment) — so GnomosWindow itself no
// longer branches on where the level came from; it just checks whether
// *any* entry wants a grid. Whether to actually render one *when
// available* is the user's own choice, via view_mode_button_.
class LibraryView : public Gtk::Box
{
public:
  LibraryView();

  // grid_available: whether this level has any grid-eligible entries at
  // all (LibraryEntry::display_as_grid) — controls whether
  // view_mode_button_ is shown; a level of plain leaf tracks or local
  // Genres/Playlists never offers the choice in the first place.
  // grid_active: whether to actually render as a grid right now (only
  // meaningful when grid_available is true) — GnomosWindow decides this
  // by combining grid_available with the user's own persisted preference.
  // Cover-art tiles are styled after Euphonica's own Albums/Artists grid
  // (https://github.com/htkhiem/euphonica). Grid tiles never get the
  // add-to-queue/play-next/favorite buttons list rows do (see those
  // signals' own comments) — a grid is only ever offered for containers
  // (albums/artists/playlists to browse into), never leaf tracks those
  // buttons would apply to; switching to list view offers them instead.
  // show_favorite_action: whether to show a per-row "add to favorites"
  // star at all — GnomosWindow only passes true below the true library
  // root, where entries are real content rather than static categories
  // ("Interpreten", "Alben", ...) that Sonos has nothing to favorite.
  // show_delete_action: whether to show a per-row "delete" button — only
  // ever true while browsing "SQ:" (the "Playlisten" root), where every
  // entry really is a destroyable saved Sonos playlist, unlike any other
  // library level (a local album, an artist, a service listing, ... none
  // of those are things this app can delete).
  // load_artist_images: the user's own opt-in preference (see
  // GnomosWindow::load_artist_images_) — when true, an artist entry
  // (icon_name == "avatar-default-symbolic", see IconNameForSubType())
  // with no real art_uri gets a genuine photo fetched via
  // ArtistImageFetcher instead of just showing that generic icon forever.
  void SetEntries(const std::vector<LibraryEntry>& entries, bool grid_available, bool grid_active,
                  bool show_favorite_action, bool show_delete_action, bool load_artist_images);
  void SetLevelTitle(const std::string& title);
  void SetBackVisible(bool visible);
  // Whether add_button_ (a custom radio stream, see signal_add_requested())
  // is shown at all — true only while browsing "R:0/0" ("Radiosender").
  void SetAddVisible(bool visible);
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
  // Unlike add-to-queue/play-next, this one *is* emitted for containers too
  // (a whole album/playlist/artist is a perfectly normal thing to
  // favorite in Sonos) — only ever in list mode, and only when
  // show_favorite_action was true for this level (see SetEntries()).
  sigc::signal<void(unsigned)>& signal_add_to_favorites_requested() { return signal_add_to_favorites_requested_; }
  // Only ever emitted when show_delete_action was true for this level
  // (browsing "SQ:") — see SetEntries()'s own comment.
  sigc::signal<void(unsigned)>& signal_delete_requested() { return signal_delete_requested_; }
  // add_button_'s click — see SetAddVisible()'s own comment. No index:
  // this adds a brand new custom radio stream, not an action on an
  // existing row.
  sigc::signal<void()>& signal_add_requested() { return signal_add_requested_; }
  // play_all_button_/queue_all_button_ only ever show up once the current
  // level is entirely leaf tracks (e.g. an album's contents) — see
  // SetEntries(). Both act on the whole level, not a single row, so
  // neither signal carries an index.
  sigc::signal<void()>& signal_play_all_requested() { return signal_play_all_requested_; }
  sigc::signal<void()>& signal_queue_all_requested() { return signal_queue_all_requested_; }
  // Fires on user click only (Gtk::Button::signal_clicked(), not
  // ToggleButton's own signal_toggled(), which also fires for the
  // programmatic set_active() SetEntries() itself makes) — GnomosWindow
  // decides the new grid/list state itself and calls SetEntries() again
  // with it, the same "caller decides, this widget just reports the
  // click" split every other toggle in this app already uses.
  sigc::signal<void()>& signal_view_mode_toggled() { return signal_view_mode_toggled_; }

private:
  void BuildList(const std::vector<LibraryEntry>& entries, bool show_favorite_action, bool show_delete_action,
                 bool load_artist_images);
  void BuildGrid(const std::vector<LibraryEntry>& entries, bool load_artist_images);

  Gtk::Button back_button_;
  Gtk::Label level_title_;
  Gtk::Label count_label_;
  Gtk::Button play_all_button_;
  Gtk::Button queue_all_button_;
  Gtk::ToggleButton view_mode_button_;
  // Custom radio stream — see SetAddVisible()/signal_add_requested()'s own
  // comments. Placed before search_button_, same reasoning as
  // play_all_button_/queue_all_button_.
  Gtk::Button add_button_;
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
  sigc::signal<void(unsigned)> signal_add_to_favorites_requested_;
  sigc::signal<void(unsigned)> signal_delete_requested_;
  sigc::signal<void()> signal_add_requested_;
  sigc::signal<void()> signal_play_all_requested_;
  sigc::signal<void()> signal_queue_all_requested_;
  sigc::signal<void()> signal_view_mode_toggled_;
};

}  // namespace gnomos
