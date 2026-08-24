// SPDX-License-Identifier: GPL-3.0-or-later

#include "library-view.h"

#include <algorithm>
#include <cctype>
#include <string>

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
}  // namespace

LibraryView::LibraryView()
: Gtk::Box(Gtk::Orientation::VERTICAL, 0), placeholder_("Keine Einträge gefunden.")
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
  // entry (LibraryEntry::display_as_grid) — see SetEntries(). Icon
  // reflects the mode switching *to*, matching the convention every other
  // view-mode toggle in GNOME uses (e.g. Nautilus's own grid/list button).
  view_mode_button_.set_icon_name("view-grid-symbolic");
  view_mode_button_.add_css_class("flat");
  view_mode_button_.set_tooltip_text("Als Raster/Liste anzeigen");
  view_mode_button_.set_visible(false);
  view_mode_button_.signal_clicked().connect([this] { signal_view_mode_toggled_.emit(); });
  header->append(view_mode_button_);

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

  placeholder_.set_wrap(true);
  placeholder_.add_css_class("dim-label");
  placeholder_.set_margin_top(24);
  placeholder_.set_margin_bottom(24);

  list_box_.set_placeholder(placeholder_);
  list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  list_box_.add_css_class("boxed-list");
  list_box_.set_margin_top(6);
  list_box_.set_margin_bottom(12);
  list_box_.set_margin_start(12);
  list_box_.set_margin_end(12);

  list_box_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
    if (row)
      signal_entry_activated_.emit(static_cast<unsigned>(row->get_index()));
  });

  // Grid mode (Albums/Artists — see the header comment on SetEntries()),
  // styled after Euphonica's own Albums/Artists grid
  // (https://github.com/htkhiem/euphonica): square tiles that reflow with
  // the available width, rather than a plain list.
  flow_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  flow_box_.set_homogeneous(true);
  flow_box_.set_row_spacing(12);
  flow_box_.set_column_spacing(12);
  flow_box_.set_margin_top(12);
  flow_box_.set_margin_bottom(12);
  flow_box_.set_margin_start(12);
  flow_box_.set_margin_end(12);
  flow_box_.set_valign(Gtk::Align::START);
  flow_box_.set_activate_on_single_click(true);
  flow_box_.signal_child_activated().connect([this](Gtk::FlowBoxChild* child) {
    if (child)
      signal_entry_activated_.emit(static_cast<unsigned>(child->get_index()));
  });

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);
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
  view_mode_button_.set_visible(grid_available);
  // Icon/tooltip reflect the mode a click switches *to*, not the current
  // one.
  view_mode_button_.set_icon_name(grid ? "view-list-symbolic" : "view-grid-symbolic");
  view_mode_button_.set_tooltip_text(grid ? "Als Liste anzeigen" : "Als Raster anzeigen");

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
  scroller_.set_child(grid ? static_cast<Gtk::Widget&>(flow_box_) : static_cast<Gtk::Widget&>(list_box_));
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
    // have no MPRIS-relevant settings. Opens GnomosWindow's per-station
    // MPRIS settings dialog (mpris_enabled + regex, see RadioMprisSettings).
    if (show_radio_settings_action)
    {
      auto* radio_settings_button = Gtk::make_managed<Gtk::Button>();
      radio_settings_button->set_icon_name("emblem-system-symbolic");
      radio_settings_button->add_css_class("flat");
      radio_settings_button->set_valign(Gtk::Align::CENTER);
      radio_settings_button->set_tooltip_text("MPRIS-Einstellungen");
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

    list_box_.append(*row_box);
  }
}

void LibraryView::BuildGrid(const std::vector<unsigned>& indices, bool load_artist_images)
{
  for (unsigned index : indices)
  {
    const LibraryEntry& entry = all_entries_[index];
    auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    tile->set_size_request(120, -1);

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

    auto* title = Gtk::make_managed<Gtk::Label>(entry.title.empty() ? "Unbenannt" : entry.title);
    title->set_halign(Gtk::Align::CENTER);
    title->set_justify(Gtk::Justification::CENTER);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    title->set_lines(2);
    title->set_wrap(true);
    title->set_max_width_chars(16);
    tile->append(*title);

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

    flow_box_.append(*tile);
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
  while (Gtk::Widget* child = flow_box_.get_first_child())
    flow_box_.remove(*child);
}

}  // namespace gnomos
