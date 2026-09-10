// SPDX-License-Identifier: GPL-3.0-or-later

#include "library-view.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

#include <adwaita.h>
#include <gdkmm/graphene_rect.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/image.h>
#include <pangomm/layout.h>

namespace gnomos
{

namespace
{
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle_lower)
{
  std::string haystack_lower = haystack;
  std::transform(haystack_lower.begin(), haystack_lower.end(), haystack_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return haystack_lower.find(needle_lower) != std::string::npos;
}

// notify::active has no gtkmm binding on AdwToggleGroup (an Adw-only
// widget) — raw GObject signal + trampoline, same "heap-allocated
// std::function + matching GClosureNotify" pattern gnomos-window.cpp uses
// for its own Adw widgets. Named distinctly (not reused from there) since
// extern "C" functions keep C language linkage across an anonymous
// namespace, so an identically-named one in another translation unit
// would collide at link time.
extern "C" void OnLibraryViewModeToggleChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(guint)>*>(user_data);
  (*callback)(adw_toggle_group_get_active(ADW_TOGGLE_GROUP(object)));
}
extern "C" void DeleteLibraryGuintCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(guint)>*>(data);
}
}  // namespace

LibraryView::LibraryView() : Gtk::Box(Gtk::Orientation::VERTICAL, 0)
{
  set_vexpand(true);
  set_hexpand(true);

  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  header->set_margin_top(6);
  header->set_margin_bottom(6);
  header->set_margin_start(6);
  header->set_margin_end(6);

  back_button_.set_icon_name("go-previous-symbolic");
  back_button_.add_css_class("flat");
  back_button_.set_tooltip_text("Zurück");
  back_button_.signal_clicked().connect([this] { signal_back_requested_.emit(); });
  header->append(back_button_);

  level_title_.set_halign(Gtk::Align::START);
  level_title_.set_hexpand(true);
  level_title_.set_ellipsize(Pango::EllipsizeMode::END);
  level_title_.add_css_class("heading");
  header->append(level_title_);

  // Only ever shown for a fully-leaf level (e.g. an album's track list),
  // and only while no filter is active — see ApplyFilter(). Placed before
  // the search button so search stays the rightmost, always-present action.
  play_all_button_.set_icon_name("media-playback-start-symbolic");
  play_all_button_.add_css_class("flat");
  play_all_button_.set_tooltip_text("Alle abspielen");
  play_all_button_.set_visible(false);
  play_all_button_.signal_clicked().connect([this] { signal_play_all_requested_.emit(); });
  header->append(play_all_button_);

  queue_all_button_.set_icon_name("list-add-symbolic");
  queue_all_button_.add_css_class("flat");
  queue_all_button_.set_tooltip_text("Alle zur Warteschlange hinzufügen");
  queue_all_button_.set_visible(false);
  queue_all_button_.signal_clicked().connect([this] { signal_queue_all_requested_.emit(); });
  header->append(queue_all_button_);

  // Only ever shown when the current level has at least one grid-eligible
  // entry (LibraryEntry::display_as_grid) — see SetEntries(). A 2-way
  // AdwToggleGroup showing both options at once, rather than a single
  // button that flips its own icon to name whichever mode it would switch
  // *to* — the same "show the actual choices" convention GNOME Settings'
  // own Appearance panel moved to for light/dark/auto.
  view_mode_toggle_group_ = adw_toggle_group_new();
  AdwToggle* grid_toggle = adw_toggle_new();
  adw_toggle_set_icon_name(grid_toggle, "view-grid-symbolic");
  adw_toggle_set_tooltip(grid_toggle, "Als Raster anzeigen");
  adw_toggle_group_add(ADW_TOGGLE_GROUP(view_mode_toggle_group_), grid_toggle);
  AdwToggle* list_toggle = adw_toggle_new();
  adw_toggle_set_icon_name(list_toggle, "view-list-symbolic");
  adw_toggle_set_tooltip(list_toggle, "Als Liste anzeigen");
  adw_toggle_group_add(ADW_TOGGLE_GROUP(view_mode_toggle_group_), list_toggle);
  gtk_widget_set_visible(view_mode_toggle_group_, false);
  // Guards against the programmatic adw_toggle_group_set_active() call in
  // SetEntries() (keeping the group in sync with prefer_grid_view_) itself
  // triggering this same "notify::active" handler right back — same
  // "caller decides the new state, this widget just reports a genuine
  // user action" split the header comment on signal_view_mode_toggled()
  // already documents, adapted for AdwToggleGroup having no separate
  // "clicked" signal to filter on the way Gtk::ToggleButton did.
  g_signal_connect_data(
      view_mode_toggle_group_, "notify::active", G_CALLBACK(OnLibraryViewModeToggleChanged),
      new std::function<void(guint)>([this](guint active) {
        if (!updating_view_mode_toggle_)
          signal_view_mode_toggled_.emit(active == 0);
      }),
      DeleteLibraryGuintCallback, static_cast<GConnectFlags>(0));
  header->append(*Glib::wrap(view_mode_toggle_group_));

  // Only ever shown while browsing "R:0/0" ("Radiosender") — see
  // SetAddVisible(). Placed right before search, same "actions before the
  // always-present rightmost search button" convention as play_all_button_/
  // queue_all_button_ above.
  add_button_.set_icon_name("list-add-symbolic");
  add_button_.add_css_class("flat");
  add_button_.set_tooltip_text("Radiosender hinzufügen");
  add_button_.set_visible(false);
  add_button_.signal_clicked().connect([this] { signal_add_requested_.emit(); });
  header->append(add_button_);

  search_button_.set_icon_name("system-search-symbolic");
  search_button_.add_css_class("flat");
  search_button_.set_tooltip_text("Im ganzen Dienst suchen");
  search_button_.signal_clicked().connect([this] { signal_search_requested_.emit(); });
  header->append(search_button_);

  append(*header);

  // Live local filter — narrows entries already loaded for *this* level as
  // you type, no network round trip (unlike search_button_'s dialog, which
  // searches the whole library/service on the server). Replaces an earlier
  // A-Z jump index that was tried here first and reported back as not a
  // good fit — this mirrors FavoritesView's own proven filter field instead.
  filter_entry_.set_placeholder_text("Filtern…");
  filter_entry_.set_margin_start(12);
  filter_entry_.set_margin_end(12);
  filter_entry_.set_margin_bottom(6);
  filter_entry_.signal_search_changed().connect(sigc::mem_fun(*this, &LibraryView::ApplyFilter));
  append(filter_entry_);

  count_label_.add_css_class("dim-label");
  count_label_.add_css_class("caption");
  count_label_.set_halign(Gtk::Align::START);
  count_label_.set_margin_start(12);
  count_label_.set_margin_bottom(4);
  append(count_label_);

  placeholder_ = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(placeholder_), "folder-music-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(placeholder_), "Keine Einträge gefunden");

  list_box_.set_placeholder(*Glib::wrap(placeholder_));
  list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  list_box_.add_css_class("boxed-list");
  list_box_.set_margin_top(6);
  list_box_.set_margin_bottom(12);
  list_box_.set_margin_start(12);
  list_box_.set_margin_end(12);

  // row->get_index() would be the row's position among the currently
  // *displayed* (possibly filter_entry_-narrowed) rows, not its real
  // position in all_entries_ — wrong the moment a filter is active, since
  // then they diverge (confirmed live: filtering down to one match and
  // activating it opened all_entries_[0] instead). BuildList() stashes the
  // real index as GObject data on each row instead, read back here.
  list_box_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
    if (row)
      signal_entry_activated_.emit(
          static_cast<unsigned>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row->gobj()), "entry-index"))));
  });

  // Grid mode (Albums/Artists — see the header comment on SetEntries()),
  // styled after Euphonica's own Albums/Artists grid
  // (https://github.com/htkhiem/euphonica): square tiles that reflow with
  // the available width, rather than a plain list. AdwWrapBox instead of
  // GtkFlowBox — confirmed live that FlowBox's homogeneous mode, even with
  // max-children-per-line raised, stretches every cell in a row to fill
  // whatever width is left over once it's decided how many columns fit,
  // ballooning tiles at wide window sizes; AdwWrapBox lays children out at
  // their own natural size with no such hidden stretch, exactly what a
  // "square tiles, more of them as the window widens" grid needs.
  wrap_box_ = adw_wrap_box_new();
  // Takes ownership independent of whichever container currently parents
  // it — see wrap_box_'s own header comment for why this matters.
  g_object_ref_sink(wrap_box_);
  // Same value on every side and between rows/columns — Albums and
  // Artists (and any other grid-eligible level, e.g. a bonob category)
  // all render through this one wrap_box_, so there's nothing left to
  // unify between them; trimmed down from 12 across the board, which
  // read as more air than the tiles themselves needed.
  adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(wrap_box_), 8);
  adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(wrap_box_), 8);
  // Unlike GtkFlowBox's own "homogeneous" (coupled to stretch-to-fill —
  // see this block's header comment for why that was wrong here),
  // AdwWrapBox keeps line_homogeneous and justify independent: this only
  // equalizes tile sizes *within* each line to that line's own widest
  // tile, with no leftover-space stretching (justify stays NONE below).
  // Needed because a tile's own natural width isn't reliably uniform on
  // its own — a title label with one long unbreakable word (e.g.
  // "AnnenMayKantereit") reports a wider natural size than a short one —
  // and without this, that row alone fit one fewer tile than the rows
  // above/below it (reported live).
  adw_wrap_box_set_line_homogeneous(ADW_WRAP_BOX(wrap_box_), true);
  adw_wrap_box_set_justify(ADW_WRAP_BOX(wrap_box_), ADW_JUSTIFY_NONE);
  adw_wrap_box_set_wrap_policy(ADW_WRAP_BOX(wrap_box_), ADW_WRAP_NATURAL);
  gtk_widget_set_margin_top(wrap_box_, 8);
  gtk_widget_set_margin_bottom(wrap_box_, 8);
  gtk_widget_set_margin_start(wrap_box_, 8);
  gtk_widget_set_margin_end(wrap_box_, 8);
  gtk_widget_set_valign(wrap_box_, GTK_ALIGN_START);

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);

  // See OnScrollSettled()'s own comment. 150ms after the last scroll event
  // — long enough that a still-moving scroll (a drag, a momentum fling)
  // doesn't trigger a bounds check per tick, short enough that a genuine
  // stop reprioritizes promptly rather than visibly lagging behind.
  scroller_.get_vadjustment()->signal_value_changed().connect([this] {
    scroll_settle_connection_.disconnect();
    scroll_settle_connection_ = Glib::signal_timeout().connect(
        [this] {
          OnScrollSettled();
          return false;  // one-shot
        },
        150);
  });
}

LibraryView::~LibraryView()
{
  g_object_unref(wrap_box_);
}

void LibraryView::SetEntries(const std::vector<LibraryEntry>& entries, bool grid_available, bool grid_active,
                              bool show_favorite_action, bool show_delete_action, bool show_add_to_playlist_action,
                              bool show_reorder_action, bool show_play_all_action, bool show_queue_all_action,
                              bool show_queue_actions, bool load_artist_images, bool show_radio_settings_action)
{
  all_entries_ = entries;
  grid_available_ = grid_available;
  grid_active_ = grid_active;
  show_favorite_action_ = show_favorite_action;
  show_delete_action_ = show_delete_action;
  show_add_to_playlist_action_ = show_add_to_playlist_action;
  show_reorder_action_ = show_reorder_action;
  show_play_all_action_ = show_play_all_action;
  show_queue_all_action_ = show_queue_all_action;
  show_queue_actions_ = show_queue_actions;
  load_artist_images_ = load_artist_images;
  show_radio_settings_action_ = show_radio_settings_action;

  // A filter that made sense for the *previous* level shouldn't silently
  // keep hiding entries after navigating somewhere unrelated — set_text()
  // alone won't fire signal_search_changed() when the text was already
  // empty (the common case), so ApplyFilter() is called explicitly below
  // regardless.
  filter_entry_.set_text("");

  bool grid = grid_available && grid_active;
  gtk_widget_set_visible(view_mode_toggle_group_, grid_available);
  // See OnLibraryViewModeToggleChanged's registration comment for why this
  // guard is needed — index 0 is the grid toggle, 1 is list, matching the
  // order they were added in the constructor.
  updating_view_mode_toggle_ = true;
  adw_toggle_group_set_active(ADW_TOGGLE_GROUP(view_mode_toggle_group_), grid ? 0 : 1);
  updating_view_mode_toggle_ = false;

  ApplyFilter();
}

void LibraryView::ApplyFilter()
{
  Clear();

  std::string term = filter_entry_.get_text();
  std::transform(term.begin(), term.end(), term.begin(), [](unsigned char c) { return std::tolower(c); });

  std::vector<unsigned> indices;
  indices.reserve(all_entries_.size());
  for (unsigned i = 0; i < all_entries_.size(); ++i)
  {
    const LibraryEntry& entry = all_entries_[i];
    if (term.empty() || ContainsCaseInsensitive(entry.title, term) || ContainsCaseInsensitive(entry.subtitle, term))
      indices.push_back(i);
  }

  count_label_.set_text(indices.empty()      ? ""
                         : indices.size() == 1 ? "1 Eintrag"
                                                : std::to_string(indices.size()) + " Einträge");

  bool grid = grid_available_ && grid_active_;
  scroller_.set_child(grid ? *Glib::wrap(wrap_box_) : static_cast<Gtk::Widget&>(list_box_));
  if (grid)
    BuildGrid(indices, load_artist_images_);
  else
    BuildList(indices, show_favorite_action_, show_delete_action_, show_add_to_playlist_action_,
              show_reorder_action_ && term.empty(), show_queue_actions_, load_artist_images_,
              show_radio_settings_action_);

  // "Play all"/"queue all" only make sense once every entry at the (full,
  // unfiltered) level is a leaf track, and only while no filter is
  // narrowing the view — see their own signal comments in the header for
  // why a filtered subset can't unambiguously mean "all of them" too.
  bool all_leaf =
      !grid && !all_entries_.empty() &&
      std::none_of(all_entries_.begin(), all_entries_.end(), [](const LibraryEntry& e) { return e.is_container; });
  play_all_button_.set_visible(all_leaf && term.empty() && show_play_all_action_);
  queue_all_button_.set_visible(all_leaf && term.empty() && show_queue_all_action_);
}

void LibraryView::BuildList(const std::vector<unsigned>& indices, bool show_favorite_action, bool show_delete_action,
                             bool show_add_to_playlist_action, bool show_reorder_action, bool show_queue_actions,
                             bool load_artist_images, bool show_radio_settings_action)
{
  for (unsigned index : indices)
  {
    const LibraryEntry& entry = all_entries_[index];
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(6);
    row_box->set_margin_bottom(6);
    row_box->set_margin_start(6);
    row_box->set_margin_end(6);

    auto* thumbnail = Gtk::make_managed<CoverThumbnail>();
    thumbnail->SetFallbackIconName(entry.icon_name);
    // "avatar-default-symbolic" is exactly the icon IconNameForSubType()
    // (noson-backend.cpp) assigns for an artist (DigitalItem::SubType_person)
    // — the one entry type with no real art of its own to fall back to.
    if (load_artist_images && entry.icon_name == "avatar-default-symbolic" && entry.art_uri.empty())
      thumbnail->LoadArtistImage(entry.title);
    else
      thumbnail->SetArtUri(entry.art_uri);
    row_box->append(*thumbnail);
    thumbnails_.push_back(thumbnail);

    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_hexpand(true);
    auto* title = Gtk::make_managed<Gtk::Label>(entry.title.empty() ? "Unbenannt" : entry.title);
    title->set_halign(Gtk::Align::START);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    labels->append(*title);
    if (!entry.subtitle.empty())
    {
      auto* subtitle = Gtk::make_managed<Gtk::Label>(entry.subtitle);
      subtitle->set_halign(Gtk::Align::START);
      subtitle->set_ellipsize(Pango::EllipsizeMode::END);
      subtitle->add_css_class("dim-label");
      subtitle->add_css_class("caption");
      labels->append(*subtitle);
    }
    row_box->append(*labels);

    // Both a container (a whole album/playlist/artist) and a leaf track can
    // be favorited in Sonos, unlike add-to-queue/play-next which only make
    // sense for a leaf — so this button sits outside the container/leaf
    // split below, common to both branches.
    if (show_favorite_action)
    {
      auto* favorite_button = Gtk::make_managed<Gtk::Button>();
      favorite_button->set_icon_name("non-starred-symbolic");
      favorite_button->add_css_class("flat");
      favorite_button->set_valign(Gtk::Align::CENTER);
      favorite_button->set_tooltip_text("Zu Favoriten hinzufügen");
      favorite_button->signal_clicked().connect([this, index] { signal_add_to_favorites_requested_.emit(index); });
      row_box->append(*favorite_button);
    }

    // Only ever true while browsing "SQ:" or "R:0/0" — see SetEntries()'s
    // own comment. Every entry at either level is a container (a saved
    // playlist or a radio station), so this sits alongside the favorite
    // button rather than inside the leaf-only branch below.
    if (show_delete_action)
    {
      auto* delete_button = Gtk::make_managed<Gtk::Button>();
      delete_button->set_icon_name("user-trash-symbolic");
      delete_button->add_css_class("flat");
      delete_button->set_valign(Gtk::Align::CENTER);
      delete_button->set_tooltip_text("Löschen");
      delete_button->signal_clicked().connect([this, index] { signal_delete_requested_.emit(index); });
      row_box->append(*delete_button);
    }

    // Only ever true while browsing "R:0/0" — not "SQ:", saved playlists
    // have no notification-relevant settings. Opens GnomosWindow's
    // per-station settings dialog (mpris_enabled + regex, see
    // RadioMprisSettings) — governs both MPRIS reporting and Verlauf
    // recording for this station now, not just MPRIS.
    if (show_radio_settings_action)
    {
      auto* radio_settings_button = Gtk::make_managed<Gtk::Button>();
      radio_settings_button->set_icon_name("emblem-system-symbolic");
      radio_settings_button->add_css_class("flat");
      radio_settings_button->set_valign(Gtk::Align::CENTER);
      radio_settings_button->set_tooltip_text("Benachrichtigungen");
      radio_settings_button->signal_clicked().connect(
          [this, index] { signal_radio_settings_requested_.emit(index); });
      row_box->append(*radio_settings_button);
    }

    // Only while viewing a specific saved playlist's own track listing
    // (and unfiltered — see SetEntries()'s own comment) — indices is then
    // exactly 0..all_entries_.size()-1 in order, so index doubles as this
    // row's real position for the edge checks below.
    if (show_reorder_action)
    {
      auto* up_button = Gtk::make_managed<Gtk::Button>();
      up_button->set_icon_name("go-up-symbolic");
      up_button->add_css_class("flat");
      up_button->set_valign(Gtk::Align::CENTER);
      up_button->set_tooltip_text("Nach oben verschieben");
      up_button->set_sensitive(index > 0);
      up_button->signal_clicked().connect([this, index] { signal_reorder_requested_.emit(index, index - 1); });
      row_box->append(*up_button);

      auto* down_button = Gtk::make_managed<Gtk::Button>();
      down_button->set_icon_name("go-down-symbolic");
      down_button->add_css_class("flat");
      down_button->set_valign(Gtk::Align::CENTER);
      down_button->set_tooltip_text("Nach unten verschieben");
      down_button->set_sensitive(index + 1 < all_entries_.size());
      down_button->signal_clicked().connect([this, index] { signal_reorder_requested_.emit(index, index + 1); });
      row_box->append(*down_button);
    }

    if (entry.is_container)
    {
      auto* chevron = Gtk::make_managed<Gtk::Image>();
      chevron->set_from_icon_name("go-next-symbolic");
      chevron->add_css_class("dim-label");
      row_box->append(*chevron);
    }
    else
    {
      // See SetEntries()'s own comment — both buttons back onto
      // AVTransport::AddURIToQueue(), which fails outright for an entry
      // System::CanQueueItem() reports as not queueable (e.g. a live
      // radio stream); GnomosWindow turns this off for exactly those
      // levels rather than showing two buttons guaranteed to error.
      if (show_queue_actions)
      {
        auto* add_button = Gtk::make_managed<Gtk::Button>();
        add_button->set_icon_name("list-add-symbolic");
        add_button->add_css_class("flat");
        add_button->set_valign(Gtk::Align::CENTER);
        add_button->set_tooltip_text("Zur Warteschlange hinzufügen");
        add_button->signal_clicked().connect([this, index] { signal_add_to_queue_requested_.emit(index); });
        row_box->append(*add_button);

        auto* play_next_button = Gtk::make_managed<Gtk::Button>();
        play_next_button->set_icon_name("media-skip-forward-symbolic");
        play_next_button->add_css_class("flat");
        play_next_button->set_valign(Gtk::Align::CENTER);
        play_next_button->set_tooltip_text("Als nächstes abspielen");
        play_next_button->signal_clicked().connect([this, index] { signal_play_next_requested_.emit(index); });
        row_box->append(*play_next_button);
      }
      else
      {
        // See SetEntries()'s own comment — the entry that would otherwise
        // get add-to-queue/play-next gets a single, explicit "play now"
        // instead, emitting exactly what activating the row itself
        // already would.
        auto* play_now_button = Gtk::make_managed<Gtk::Button>();
        play_now_button->set_icon_name("media-playback-start-symbolic");
        play_now_button->add_css_class("flat");
        play_now_button->set_valign(Gtk::Align::CENTER);
        play_now_button->set_tooltip_text("Jetzt abspielen");
        play_now_button->signal_clicked().connect([this, index] { signal_entry_activated_.emit(index); });
        row_box->append(*play_now_button);
      }

      if (show_add_to_playlist_action)
      {
        auto* add_to_playlist_button = Gtk::make_managed<Gtk::Button>();
        add_to_playlist_button->set_icon_name("bookmark-new-symbolic");
        add_to_playlist_button->add_css_class("flat");
        add_to_playlist_button->set_valign(Gtk::Align::CENTER);
        add_to_playlist_button->set_tooltip_text("Zu Playlist hinzufügen");
        add_to_playlist_button->signal_clicked().connect(
            [this, index] { signal_add_to_playlist_requested_.emit(index); });
        row_box->append(*add_to_playlist_button);
      }
    }

    // Explicit Gtk::ListBoxRow (rather than letting list_box_.append() auto-
    // wrap row_box in an anonymous one) so the real all_entries_ index can
    // be stashed on it — see signal_row_activated()'s own comment.
    auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
    g_object_set_data(G_OBJECT(row->gobj()), "entry-index", GUINT_TO_POINTER(index));
    row->set_child(*row_box);
    list_box_.append(*row);
  }
}

void LibraryView::BuildGrid(const std::vector<unsigned>& indices, bool load_artist_images)
{
  for (unsigned index : indices)
  {
    const LibraryEntry& entry = all_entries_[index];
    auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    tile->set_size_request(120, -1);
    // Uniform padding on every side — without it, only the *bottom* of a
    // tile had breathing room (the gap before the next row), since nothing
    // ever margined the top of the thumbnail itself (reported live: "kein
    // padding oberhalb des Covers").
    tile->set_margin_top(6);
    tile->set_margin_bottom(6);
    tile->set_margin_start(6);
    tile->set_margin_end(6);

    auto* thumbnail = Gtk::make_managed<CoverThumbnail>(120);
    thumbnail->set_halign(Gtk::Align::CENTER);
    thumbnail->SetFallbackIconName(entry.icon_name);
    // See BuildList()'s identical check for why "avatar-default-symbolic"
    // specifically is the signal to use here.
    if (load_artist_images && entry.icon_name == "avatar-default-symbolic" && entry.art_uri.empty())
      thumbnail->LoadArtistImage(entry.title);
    else
      thumbnail->SetArtUri(entry.art_uri);
    tile->append(*thumbnail);
    thumbnails_.push_back(thumbnail);

    auto* title = Gtk::make_managed<Gtk::Label>(entry.title.empty() ? "Unbenannt" : entry.title);
    title->set_halign(Gtk::Align::CENTER);
    title->set_justify(Gtk::Justification::CENTER);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    title->set_lines(2);
    title->set_wrap(true);
    // Plain word-wrap (the default wrap mode) can't break a single long
    // unbroken word (e.g. "AnnenMayKantereit") at all, so a label like
    // that requests more natural width than max_width_chars implies —
    // and since AdwWrapBox lays out each line at its tiles' own natural
    // size (not a shared homogeneous cell), that one wider tile shrinks
    // how many fit in its row, breaking column alignment with the rows
    // above/below it (reported live: a whole row falling short by one).
    // Allowing a mid-word break when necessary keeps every tile's natural
    // width capped the same regardless of what its title says.
    title->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    title->set_max_width_chars(16);
    // set_lines(2) only CAPS the rendered height at two lines — it doesn't
    // RESERVE that height, so a one-line title (most of them) naturally
    // requests a shorter box than a neighbouring two-line one. Since
    // AdwWrapBox's line_homogeneous only equalizes tile height *within* a
    // line, that left whole rows visibly shorter or taller than the rows
    // above/below depending on which titles happened to wrap that line
    // (reported live: "sollen sauber in einem Raster angeordnet sein").
    // Measured once against the title label's own resolved font/theme
    // (not a hardcoded pixel guess, so it still holds under a different
    // font size or accessibility setting) and reused for every tile, so
    // every row ends up the same height regardless of content.
    // Forcing the height directly on the wrapping/ellipsizing label itself
    // (tried first) fed back into its own width negotiation — Pango ended
    // up choosing a much narrower layout width to make the text tall
    // enough to fill that forced minimum, breaking even short one-word
    // titles like "Adele" mid-word. A plain, non-wrapping wrapper box
    // around the label keeps the label's own width/wrap solving entirely
    // unaffected — only the wrapper's own allocation grows, with the
    // label staying top-anchored inside it and the leftover space simply
    // unused below a one-line title.
    static int two_line_title_height = 0;
    if (two_line_title_height == 0)
    {
      int width_unused = 0;
      title->create_pango_layout("Ay\nAy")->get_pixel_size(width_unused, two_line_title_height);
    }
    auto* title_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    title_box->set_valign(Gtk::Align::START);
    title_box->set_size_request(-1, two_line_title_height);
    title->set_valign(Gtk::Align::START);
    title_box->append(*title);
    tile->append(*title_box);

    if (!entry.subtitle.empty())
    {
      auto* subtitle = Gtk::make_managed<Gtk::Label>(entry.subtitle);
      subtitle->set_halign(Gtk::Align::CENTER);
      subtitle->set_justify(Gtk::Justification::CENTER);
      subtitle->set_ellipsize(Pango::EllipsizeMode::END);
      subtitle->set_max_width_chars(16);
      subtitle->add_css_class("dim-label");
      subtitle->add_css_class("caption");
      tile->append(*subtitle);
    }

    // AdwWrapBox takes plain widgets directly (no GtkFlowBoxChild-style
    // wrapper needed) and has no activation signal of its own, so each
    // tile gets its own click gesture — capturing `index` directly here
    // sidesteps the whole "real index vs. display position" bug class
    // BuildList()'s row-level GObject-data workaround exists for.
    auto click = Gtk::GestureClick::create();
    click->signal_released().connect([this, index](int, double, double) { signal_entry_activated_.emit(index); });
    tile->add_controller(click);
    adw_wrap_box_append(ADW_WRAP_BOX(wrap_box_), GTK_WIDGET(tile->gobj()));
  }
}

void LibraryView::SetLevelTitle(const std::string& title)
{
  level_title_.set_text(title);
}

void LibraryView::SetBackVisible(bool visible)
{
  back_button_.set_visible(visible);
}

void LibraryView::SetAddVisible(bool visible)
{
  add_button_.set_visible(visible);
}

void LibraryView::Clear()
{
  while (Gtk::Widget* child = list_box_.get_first_child())
    list_box_.remove(*child);
  adw_wrap_box_remove_all(ADW_WRAP_BOX(wrap_box_));
  thumbnails_.clear();
}

void LibraryView::OnScrollSettled()
{
  double viewport_height = scroller_.get_height();
  for (CoverThumbnail* thumbnail : thumbnails_)
  {
    auto bounds = thumbnail->compute_bounds(scroller_);
    // No bounds (unmapped, not yet laid out) is never itself "visible" —
    // skip rather than treat a missing value as in-viewport.
    if (bounds && bounds->get_y() + bounds->get_height() >= 0 && bounds->get_y() <= viewport_height)
      thumbnail->PrioritizeLoad();
  }
}

}  // namespace gnomos
