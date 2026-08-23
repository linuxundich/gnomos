// SPDX-License-Identifier: GPL-3.0-or-later

#include "library-view.h"

#include <algorithm>
#include <map>
#include <string>

#include <glib.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/image.h>
#include <pangomm/layout.h>

namespace gnomos
{

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

  // Only ever shown for a fully-leaf level (e.g. an album's track list) —
  // see SetEntries(). Placed before the search button so search stays the
  // rightmost, always-present action.
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
  search_button_.set_tooltip_text("Suchen");
  search_button_.signal_clicked().connect([this] { signal_search_requested_.emit(); });
  header->append(search_button_);

  append(*header);

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

  // A-Z jump index (see RebuildIndexStrip()) — narrow, fixed-width, its
  // own "card" background so it reads as a distinct control rather than
  // part of the scrolled content next to it, matching the user's own
  // "schmale farblich abgesetzte Spalte" request. Only visible once
  // SetEntries() has enough entries to actually be worth jumping around
  // in — see RebuildIndexStrip()'s own threshold.
  index_strip_.add_css_class("card");
  index_strip_.set_homogeneous(true);
  index_strip_.set_vexpand(true);
  index_strip_.set_size_request(28, -1);
  index_strip_.set_visible(false);

  auto* content_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  content_row->set_vexpand(true);
  content_row->set_hexpand(true);
  content_row->append(index_strip_);
  content_row->append(scroller_);
  append(*content_row);
}

void LibraryView::SetEntries(const std::vector<LibraryEntry>& entries, bool grid_available, bool grid_active,
                              bool show_favorite_action, bool show_delete_action, bool load_artist_images)
{
  Clear();
  count_label_.set_text(entries.empty()      ? ""
                         : entries.size() == 1 ? "1 Eintrag"
                                                : std::to_string(entries.size()) + " Einträge");
  bool grid = grid_available && grid_active;
  grid_mode_active_ = grid;
  scroller_.set_child(grid ? static_cast<Gtk::Widget&>(flow_box_) : static_cast<Gtk::Widget&>(list_box_));
  if (grid)
    BuildGrid(entries, load_artist_images);
  else
    BuildList(entries, show_favorite_action, show_delete_action, load_artist_images);
  RebuildIndexStrip(entries);

  view_mode_button_.set_visible(grid_available);
  // Icon/tooltip reflect the mode a click switches *to*, not the current
  // one.
  view_mode_button_.set_icon_name(grid ? "view-list-symbolic" : "view-grid-symbolic");
  view_mode_button_.set_tooltip_text(grid ? "Als Liste anzeigen" : "Als Raster anzeigen");

  // "Play all"/"queue all" only make sense once every entry at this level
  // is a leaf track — never for a grid (always containers to browse into)
  // or a list still mixing in containers.
  bool all_leaf = !grid && !entries.empty() &&
                   std::none_of(entries.begin(), entries.end(), [](const LibraryEntry& e) { return e.is_container; });
  play_all_button_.set_visible(all_leaf);
  queue_all_button_.set_visible(all_leaf);
}

void LibraryView::BuildList(const std::vector<LibraryEntry>& entries, bool show_favorite_action,
                             bool show_delete_action, bool load_artist_images)
{
  for (size_t i = 0; i < entries.size(); ++i)
  {
    const LibraryEntry& entry = entries[i];
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

    unsigned index = static_cast<unsigned>(i);

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

    // Only ever true while browsing "SQ:" — see SetEntries()'s own
    // comment. Every entry there is a container (a saved playlist), so
    // this sits alongside the favorite button rather than inside the
    // leaf-only branch below.
    if (show_delete_action)
    {
      auto* delete_button = Gtk::make_managed<Gtk::Button>();
      delete_button->set_icon_name("user-trash-symbolic");
      delete_button->add_css_class("flat");
      delete_button->set_valign(Gtk::Align::CENTER);
      delete_button->set_tooltip_text("Playlist löschen");
      delete_button->signal_clicked().connect([this, index] { signal_delete_requested_.emit(index); });
      row_box->append(*delete_button);
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

    list_box_.append(*row_box);
  }
}

void LibraryView::BuildGrid(const std::vector<LibraryEntry>& entries, bool load_artist_images)
{
  for (const LibraryEntry& entry : entries)
  {
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

namespace
{
// '0' for anything not starting with a Latin letter, otherwise the
// upper-cased first Latin letter — accented ones (Ä, É, ...) fold to
// their base letter via Unicode NFD decomposition + skipping the
// resulting combining mark, so a German or French entry isn't stranded
// under "0" just because of an umlaut or accent.
char BucketFor(const std::string& title)
{
  if (title.empty())
    return '0';
  gchar* normalized = g_utf8_normalize(title.c_str(), -1, G_NORMALIZE_NFD);
  if (!normalized)
    return '0';
  char result = '0';
  for (const char* p = normalized; *p; p = g_utf8_next_char(p))
  {
    gunichar ch = g_utf8_get_char(p);
    if (g_unichar_type(ch) == G_UNICODE_NON_SPACING_MARK)
      continue;
    gunichar upper = g_unichar_toupper(ch);
    if (upper >= 'A' && upper <= 'Z')
      result = static_cast<char>(upper);
    break;
  }
  g_free(normalized);
  return result;
}
}  // namespace

void LibraryView::RebuildIndexStrip(const std::vector<LibraryEntry>& entries)
{
  while (Gtk::Widget* child = index_strip_.get_first_child())
    index_strip_.remove(*child);

  // Only worth showing once there's enough content to actually need
  // jumping around in — a handful of entries doesn't need this, and the
  // 27-row strip would dwarf a short list.
  static constexpr size_t kMinEntriesForIndex = 30;
  if (entries.size() < kMinEntriesForIndex)
  {
    index_strip_.set_visible(false);
    return;
  }

  std::map<char, int> first_index_for_bucket;
  for (size_t i = 0; i < entries.size(); ++i)
  {
    char bucket = BucketFor(entries[i].title);
    if (first_index_for_bucket.find(bucket) == first_index_for_bucket.end())
      first_index_for_bucket[bucket] = static_cast<int>(i);
  }

  static const std::string kAlphabet = "0ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (char c : kAlphabet)
  {
    auto* label = Gtk::make_managed<Gtk::Label>(std::string(1, c));
    label->add_css_class("caption");

    auto it = first_index_for_bucket.find(c);
    if (it == first_index_for_bucket.end())
    {
      // Still shown (not removed) so the strip's spatial layout stays a
      // reliable, consistent guide to "roughly where" a letter is even
      // when nothing that entries this level happens to start with it.
      label->add_css_class("dim-label");
      label->set_opacity(0.35);
      index_strip_.append(*label);
      continue;
    }

    auto* button = Gtk::make_managed<Gtk::Button>();
    button->set_child(*label);
    button->add_css_class("flat");
    int target_index = it->second;
    button->signal_clicked().connect([this, target_index] { JumpToIndex(target_index); });
    index_strip_.append(*button);
  }
  index_strip_.set_visible(true);
}

void LibraryView::JumpToIndex(int entry_index)
{
  Gtk::Widget* target = grid_mode_active_
                            ? static_cast<Gtk::Widget*>(flow_box_.get_child_at_index(entry_index))
                            : static_cast<Gtk::Widget*>(list_box_.get_row_at_index(entry_index));
  if (!target)
    return;
  Gtk::Widget& container = grid_mode_active_ ? static_cast<Gtk::Widget&>(flow_box_) : static_cast<Gtk::Widget&>(list_box_);
  if (auto bounds = target->compute_bounds(container))
    scroller_.get_vadjustment()->set_value(bounds->get_y());
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
