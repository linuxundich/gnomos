// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"
#include "cover-thumbnail.h"

namespace gnomos
{

// Hierarchical browser for the music library: a header row with a back
// button + current level title, a live local filter row below it, and
// either a list or a cover-art grid of entries below that. Whether
// activating an entry means "go deeper" or "play it" depends on
// LibraryEntry::is_container. Whether a grid is even an option for the
// current level is a single, uniform signal now — LibraryEntry::display_as_grid,
// which both the local library and third-party services populate (via
// their own, different underlying heuristics — see that field's own
// comment) — so GnomosWindow itself no longer branches on where the level
// came from; it just checks whether *any* entry wants a grid. Whether to
// actually render one *when available* is the user's own choice, via
// view_mode_button_.
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
  // ever true while browsing "SQ:" (the "Playlisten" root, deletes a whole
  // saved playlist) or "R:0/0" ("Radiosender", deletes a custom station) —
  // every entry at either of those two specific levels really is
  // destroyable, unlike any other library level (a local album, an
  // artist, a service listing, ... none of those are things this app can
  // delete).
  // show_add_to_playlist_action: whether to show a per-row "add to
  // playlist" button — GnomosWindow only offers this for a leaf track
  // below the true root, mirroring show_favorite_action's own gating; a
  // container (album/artist/genre/...) has nothing meaningful to add as a
  // single saved-queue entry.
  // show_reorder_action: whether to show per-row "move up"/"move down"
  // buttons — only while browsing a *specific* saved playlist's own track
  // listing (an "SQ:<id>" level, not "SQ:" itself, which lists playlists
  // rather than tracks). Hidden whenever filter_entry_ has text, same as
  // play_all_button_/queue_all_button_ — a filtered subset's on-screen
  // neighbor isn't necessarily the real adjacent track, which would make
  // "move up/down" do something other than what it visually looks like.
  // show_play_all_action/show_queue_all_action: whether play_all_button_
  // ("Alle abspielen") / queue_all_button_ ("+", bulk add to queue) may
  // show at all once every entry at this level is a leaf — GnomosWindow
  // turns both off specifically while browsing "R:0/0" ("Radiosender"):
  // confirmed live, bulk-playing or bulk-queuing a whole page of live
  // radio streams at once doesn't read as a sensible action the way it
  // does for a page of real tracks (an album, a playlist).
  // show_queue_actions: whether a leaf row's own per-row "add to queue"
  // (list-add-symbolic) and "play next" (media-skip-forward-symbolic)
  // buttons show at all, *instead of* a single "play now"
  // (media-playback-start-symbolic) button — GnomosWindow turns this off
  // for "R:0/0" too. Confirmed live: add-to-queue/play-next both back
  // onto AVTransport::AddURIToQueue(), which NSROOT::System::CanQueueItem()
  // reports false for a live radio stream (only a real position-addressable
  // track can be queued, not an internet stream) — the buttons weren't
  // just unhelpful there, they failed outright with an error every time.
  // "Play now" instead emits signal_entry_activated_ — the exact same
  // signal activating the row itself already emits, since
  // NosonBackend::PlayLibraryItem()'s own non-queueable branch
  // (SetCurrentURI() + Play()) is exactly the correct way to start a
  // stream; the button is just a more discoverable, explicit affordance
  // for what the row already does on its own.
  // load_artist_images: the user's own opt-in preference (see
  // GnomosWindow::load_artist_images_) — when true, an artist entry
  // (icon_name == "avatar-default-symbolic", see IconNameForSubType())
  // with no real art_uri gets a genuine photo fetched via
  // ArtistImageFetcher instead of just showing that generic icon forever.
  // Resets filter_entry_'s own text — a search that made sense for the
  // *previous* level (e.g. "adele" while browsing Interpreten) shouldn't
  // silently keep hiding entries after navigating somewhere unrelated.
  // show_radio_settings_action: whether to show a per-row "settings" (gear)
  // button — true under the exact same condition as show_delete_action's
  // "R:0/0" half (not "SQ:", saved playlists have no MPRIS-relevant
  // settings of their own). Opens GnomosWindow's per-station MPRIS
  // settings dialog (see signal_radio_settings_requested()).
  void SetEntries(const std::vector<LibraryEntry>& entries, bool grid_available, bool grid_active,
                  bool show_favorite_action, bool show_delete_action, bool show_add_to_playlist_action,
                  bool show_reorder_action, bool show_play_all_action, bool show_queue_all_action,
                  bool show_queue_actions, bool load_artist_images, bool show_radio_settings_action);
  void SetLevelTitle(const std::string& title);
  void SetBackVisible(bool visible);
  // Whether add_button_ (a custom radio stream, see signal_add_requested())
  // is shown at all — true only while browsing "R:0/0" ("Radiosender").
  void SetAddVisible(bool visible);
  void Clear();

  sigc::signal<void(unsigned)>& signal_entry_activated() { return signal_entry_activated_; }
  sigc::signal<void()>& signal_back_requested() { return signal_back_requested_; }
  // search_button_'s click — opens GnomosWindow's own server-side search
  // dialog (searches the whole library/service, not just this level).
  // Deliberately kept distinct from filter_entry_ below, which only ever
  // narrows entries already loaded for *this* level, with no network
  // round trip at all — the two are complementary, not redundant.
  sigc::signal<void()>& signal_search_requested() { return signal_search_requested_; }
  // Both indices below are the entry's position in the *unfiltered* level
  // SetEntries() was last called with — what GnomosWindow's own
  // current_library_entries_[index]-based handlers expect. Never the
  // row/tile's on-screen position, which shifts once filter_entry_ has
  // hidden earlier entries — see row_index_map_'s own comment.
  //
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
  // (browsing "SQ:" or "R:0/0") — see SetEntries()'s own comment.
  sigc::signal<void(unsigned)>& signal_delete_requested() { return signal_delete_requested_; }
  // Only ever emitted when show_radio_settings_action was true for this
  // level (browsing "R:0/0") — see SetEntries()'s own comment.
  sigc::signal<void(unsigned)>& signal_radio_settings_requested() { return signal_radio_settings_requested_; }
  // Only ever emitted when show_add_to_playlist_action was true — see
  // SetEntries()'s own comment. GnomosWindow follows up with its own
  // playlist-picker dialog; this signal only carries which library entry
  // (not which playlist) was requested.
  sigc::signal<void(unsigned)>& signal_add_to_playlist_requested() { return signal_add_to_playlist_requested_; }
  // Only ever emitted when show_reorder_action was true — see
  // SetEntries()'s own comment. Both indices are 0-based positions in the
  // unfiltered level SetEntries() was last called with, same convention
  // every other index-carrying signal here already uses.
  sigc::signal<void(unsigned, unsigned)>& signal_reorder_requested() { return signal_reorder_requested_; }
  // add_button_'s click — see SetAddVisible()'s own comment. No index:
  // this adds a brand new custom radio stream, not an action on an
  // existing row.
  sigc::signal<void()>& signal_add_requested() { return signal_add_requested_; }
  // play_all_button_/queue_all_button_ only ever show up once the current
  // level is entirely leaf tracks (e.g. an album's contents) *and* no
  // filter is currently narrowing the view — see SetEntries()/ApplyFilter().
  // Both act on the whole (unfiltered) level, not a single row, so neither
  // signal carries an index — while a filter is active there'd be no way
  // to tell "all of them" from "just the filtered ones" apart, so they're
  // hidden entirely rather than being ambiguous.
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
  // Re-renders from all_entries_/the flags SetEntries() last stored,
  // showing only entries whose title/subtitle contains filter_entry_'s
  // current text (case-insensitive substring, empty text = everything) —
  // connected to filter_entry_'s own signal_search_changed(), so this
  // runs on every keystroke. Mirrors FavoritesView's own ApplyFilter()
  // (same "small enough to just rebuild wholesale" reasoning it already
  // documents), extended for LibraryView's own grid/list duality.
  void ApplyFilter();
  void BuildList(const std::vector<unsigned>& indices, bool show_favorite_action, bool show_delete_action,
                 bool show_add_to_playlist_action, bool show_reorder_action, bool show_queue_actions,
                 bool load_artist_images, bool show_radio_settings_action);
  void BuildGrid(const std::vector<unsigned>& indices, bool load_artist_images);

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
  // Live local filter — see ApplyFilter()'s own comment for how this
  // differs from search_button_'s server-side search dialog. Its own row
  // below the header, same layout FavoritesView already uses for the
  // equivalent field.
  Gtk::SearchEntry filter_entry_;
  Gtk::ScrolledWindow scroller_;
  Gtk::ListBox list_box_;
  Gtk::FlowBox flow_box_;
  Gtk::Label placeholder_;
  // The full, unfiltered level, plus the flags it was shown with —
  // ApplyFilter() needs both on every keystroke, without GnomosWindow
  // having to call SetEntries() again just because the filter text changed.
  std::vector<LibraryEntry> all_entries_;
  bool grid_available_ = false;
  bool grid_active_ = false;
  bool show_favorite_action_ = false;
  bool show_delete_action_ = false;
  bool show_add_to_playlist_action_ = false;
  bool show_reorder_action_ = false;
  bool show_play_all_action_ = false;
  bool show_queue_all_action_ = false;
  bool show_queue_actions_ = false;
  bool load_artist_images_ = false;
  bool show_radio_settings_action_ = false;
  sigc::signal<void(unsigned)> signal_entry_activated_;
  sigc::signal<void()> signal_back_requested_;
  sigc::signal<void()> signal_search_requested_;
  sigc::signal<void(unsigned)> signal_add_to_queue_requested_;
  sigc::signal<void(unsigned)> signal_play_next_requested_;
  sigc::signal<void(unsigned)> signal_add_to_favorites_requested_;
  sigc::signal<void(unsigned)> signal_delete_requested_;
  sigc::signal<void(unsigned)> signal_radio_settings_requested_;
  sigc::signal<void(unsigned)> signal_add_to_playlist_requested_;
  sigc::signal<void(unsigned, unsigned)> signal_reorder_requested_;
  sigc::signal<void()> signal_add_requested_;
  sigc::signal<void()> signal_play_all_requested_;
  sigc::signal<void()> signal_queue_all_requested_;
  sigc::signal<void()> signal_view_mode_toggled_;
};

}  // namespace gnomos
