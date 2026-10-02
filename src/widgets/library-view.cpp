// SPDX-License-Identifier: GPL-3.0-or-later

#include "library-view.h"
#include "../i18n.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

#include <adwaita.h>
#include <gdkmm/graphene_rect.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/image.h>
#include <gtkmm/listitem.h>
#include <gtkmm/noselection.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/stringobject.h>
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

// Albums and playlists get a generated cover when they have no art of
// their own — see CoverThumbnail::SetGeneratedFallback() — and so do
// plain tracks (no specific icon of their own), which would otherwise show
// a large note glyph. Artists keep their silhouette (or a real photo),
// categories their own symbol.
bool WantsGeneratedCover(const LibraryEntry& entry)
{
  return entry.icon_name == "media-optical-cd-symbolic" || entry.icon_name == "media-playlist-consecutive-symbolic" ||
         (!entry.is_container && entry.icon_name.empty());
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
  back_button_.set_tooltip_text(_("Back"));
  back_button_.signal_clicked().connect([this] { signal_back_requested_.emit(); });
  header->append(back_button_);

  // Title with the entry count right below it, instead of the count on a
  // row of its own under the filter field — one row less above the
  // content, nothing hidden behind an extra click.
  auto* title_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  title_box->set_hexpand(true);
  title_box->set_valign(Gtk::Align::CENTER);
  level_title_.set_halign(Gtk::Align::START);
  level_title_.set_ellipsize(Pango::EllipsizeMode::END);
  level_title_.add_css_class("heading");
  title_box->append(level_title_);
  count_label_.add_css_class("dimmed");
  count_label_.add_css_class("caption");
  count_label_.set_halign(Gtk::Align::START);
  title_box->append(count_label_);
  header->append(*title_box);

  // Only ever shown for a fully-leaf level (e.g. an album's track list),
  // and only while no filter is active — see ApplyFilter(). Placed before
  // the search button so search stays the rightmost, always-present action.
  play_all_button_.set_icon_name("media-playback-start-symbolic");
  play_all_button_.add_css_class("flat");
  play_all_button_.set_tooltip_text(_("Play All"));
  play_all_button_.set_visible(false);
  play_all_button_.signal_clicked().connect([this] { signal_play_all_requested_.emit(); });
  header->append(play_all_button_);

  queue_all_button_.set_icon_name("list-add-symbolic");
  queue_all_button_.add_css_class("flat");
  queue_all_button_.set_tooltip_text(_("Add All to Queue"));
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
  adw_toggle_set_tooltip(grid_toggle, _("Show as Grid"));
  adw_toggle_group_add(ADW_TOGGLE_GROUP(view_mode_toggle_group_), grid_toggle);
  AdwToggle* list_toggle = adw_toggle_new();
  adw_toggle_set_icon_name(list_toggle, "view-list-symbolic");
  adw_toggle_set_tooltip(list_toggle, _("Show as List"));
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
  add_button_.set_tooltip_text(_("Add Radio Station"));
  add_button_.set_visible(false);
  add_button_.signal_clicked().connect([this] { signal_add_requested_.emit(); });
  header->append(add_button_);

  search_button_.set_icon_name("system-search-symbolic");
  search_button_.add_css_class("flat");
  search_button_.set_tooltip_text(_("Search Whole Service"));
  search_button_.signal_clicked().connect([this] { signal_search_requested_.emit(); });
  header->append(search_button_);

  append(*header);

  // Live local filter — narrows entries already loaded for *this* level as
  // you type, no network round trip (unlike search_button_'s dialog, which
  // searches the whole library/service on the server). Replaces an earlier
  // A-Z jump index that was tried here first and reported back as not a
  // good fit — this mirrors FavoritesView's own proven filter field instead.
  filter_entry_.set_placeholder_text(_("Filter…"));
  filter_entry_.set_margin_start(12);
  filter_entry_.set_margin_end(12);
  filter_entry_.set_margin_bottom(6);
  filter_entry_.signal_search_changed().connect(sigc::mem_fun(*this, &LibraryView::ApplyFilter));
  append(filter_entry_);


  placeholder_ = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(placeholder_), "folder-music-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(placeholder_), _("No Entries Found"));

  // Both placeholders are swapped in and out of list_box_, so each keeps
  // its own reference rather than living only as long as it's attached.
  g_object_ref_sink(placeholder_);
  loading_placeholder_ = adw_spinner_new();
  g_object_ref_sink(loading_placeholder_);
  gtk_widget_set_size_request(loading_placeholder_, 32, 32);
  gtk_widget_set_halign(loading_placeholder_, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(loading_placeholder_, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(loading_placeholder_, 48);
  gtk_widget_set_margin_bottom(loading_placeholder_, 48);
  list_box_.set_placeholder(*Glib::wrap(placeholder_));
  list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  list_box_.add_css_class("boxed-list");
  list_box_.set_margin_top(6);
  list_box_.set_margin_bottom(12);
  list_box_.set_margin_start(12);
  list_box_.set_margin_end(12);

  // list_box_ only ever shows a placeholder now (empty level, loading);
  // entries go into list_view_/grid_view_ below.

  // Grid mode (Albums/Artists — see the header comment on SetEntries()),
  // styled after Euphonica's own Albums/Artists grid
  // (https://github.com/htkhiem/euphonica): square tiles that reflow with
  // the available width. A Gtk::GridView rather than the AdwWrapBox it
  // replaced: only the tiles on screen exist as widgets, recycled while
  // scrolling, so a 1000-album level appears instantly — and each tile is
  // a real, keyboard-focusable grid item rather than a Box with a click
  // gesture. GridView also gives every column the same width by itself,
  // which the wrap box needed pixel-measured, pre-truncated labels for.
  entries_model_ = Gtk::StringList::create();
  grid_view_.set_model(Gtk::NoSelection::create(entries_model_));
  auto factory = Gtk::SignalListItemFactory::create();
  factory->signal_setup().connect(sigc::mem_fun(*this, &LibraryView::SetupGridTile));
  factory->signal_bind().connect(sigc::mem_fun(*this, &LibraryView::BindGridTile));
  grid_view_.set_factory(factory);
  grid_view_.set_min_columns(2);
  grid_view_.set_max_columns(24);
  grid_view_.set_single_click_activate(true);
  grid_view_.add_css_class("library-grid");
  grid_view_.signal_activate().connect([this](guint position) {
    signal_entry_activated_.emit(static_cast<unsigned>(std::stoul(entries_model_->get_string(position).raw())));
  });

  // List mode: a virtualized Gtk::ListView over the same model, for levels
  // like "Tracks" with thousands of entries. Rows are recycled while
  // scrolling (SetupListRow()/BindListRow()); a row's real index into
  // all_entries_ comes from the model, never from its on-screen position,
  // which shifts as soon as the filter hides entries.
  auto list_factory = Gtk::SignalListItemFactory::create();
  list_factory->signal_setup().connect(sigc::mem_fun(*this, &LibraryView::SetupListRow));
  list_factory->signal_bind().connect(sigc::mem_fun(*this, &LibraryView::BindListRow));
  list_view_.set_model(Gtk::NoSelection::create(entries_model_));
  list_view_.set_factory(list_factory);
  list_view_.set_single_click_activate(true);
  list_view_.set_show_separators(true);
  list_view_.add_css_class("library-list");
  list_view_.signal_activate().connect([this](guint position) {
    signal_entry_activated_.emit(static_cast<unsigned>(std::stoul(entries_model_->get_string(position).raw())));
  });

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);
}

LibraryView::~LibraryView()
{
  g_object_unref(placeholder_);
  g_object_unref(loading_placeholder_);
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

void LibraryView::ShowLoading()
{
  Clear();
  count_label_.set_visible(false);
  play_all_button_.set_visible(false);
  queue_all_button_.set_visible(false);
  gtk_widget_set_visible(view_mode_toggle_group_, false);
  scroller_.set_child(list_box_);
  list_box_.set_placeholder(*Glib::wrap(loading_placeholder_));
}

void LibraryView::ApplyFilter()
{
  Clear();
  list_box_.set_placeholder(*Glib::wrap(placeholder_));

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
                                                : Format(ngettext("%zu entry", "%zu entries", indices.size()),
                                                         indices.size()));
  count_label_.set_visible(!indices.empty());

  bool grid = grid_available_ && grid_active_;
  // Reordering only makes sense on the unfiltered list — see SetEntries().
  list_reorder_active_ = show_reorder_action_ && term.empty();
  // An empty level shows list_box_, which carries the "nothing found"
  // placeholder; otherwise one of the two virtualized views.
  if (indices.empty())
    scroller_.set_child(list_box_);
  else
    scroller_.set_child(grid ? static_cast<Gtk::Widget&>(grid_view_) : static_cast<Gtk::Widget&>(list_view_));
  std::vector<Glib::ustring> items;
  items.reserve(indices.size());
  for (unsigned index : indices)
    items.push_back(std::to_string(index));
  entries_model_->splice(0, entries_model_->get_n_items(), items);

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

namespace
{
// The widgets of one recycled list row, kept on the row's box as GObject
// data — BindListRow() fills them for whatever entry the row shows now, and
// the buttons read `index` when clicked.
struct LibraryListRow
{
  unsigned index = 0;
  CoverThumbnail* thumbnail = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* subtitle = nullptr;
  Gtk::Button* favorite = nullptr;
  Gtk::Button* remove = nullptr;
  Gtk::Button* radio_settings = nullptr;
  Gtk::Button* move_up = nullptr;
  Gtk::Button* move_down = nullptr;
  Gtk::Image* chevron = nullptr;
  Gtk::Button* add_to_queue = nullptr;
  Gtk::Button* play_next = nullptr;
  Gtk::Button* play_now = nullptr;
  Gtk::Button* add_to_playlist = nullptr;
};
constexpr const char* kLibraryListRowKey = "gnomos-library-row";

Gtk::Button* MakeRowButton(Gtk::Box& row, const char* icon, const char* tooltip)
{
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->set_icon_name(icon);
  button->add_css_class("flat");
  button->set_valign(Gtk::Align::CENTER);
  button->set_tooltip_text(tooltip);
  row.append(*button);
  return button;
}
}  // namespace

// Containers are colored by their own name; tracks by their subtitle
// (artist, or artist and album), so the tracks of one album share a color
// while a list of all tracks still varies.
std::string LibraryView::GeneratedSeed(const LibraryEntry& entry) const
{
  if (!entry.is_container && !entry.subtitle.empty())
    return entry.subtitle;
  if (!entry.is_container && !level_title_.get_text().empty())
    return level_title_.get_text();
  return entry.title.empty() ? "?" : entry.title;
}

void LibraryView::SetupListRow(const Glib::RefPtr<Gtk::ListItem>& item)
{
  auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  row_box->set_margin_top(6);
  row_box->set_margin_bottom(6);
  row_box->set_margin_start(6);
  row_box->set_margin_end(6);
  auto* row = new LibraryListRow;
  g_object_set_data_full(G_OBJECT(row_box->gobj()), kLibraryListRowKey, row,
                         [](gpointer data) { delete static_cast<LibraryListRow*>(data); });

  row->thumbnail = Gtk::make_managed<CoverThumbnail>();
  row_box->append(*row->thumbnail);

  auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  labels->set_hexpand(true);
  labels->set_valign(Gtk::Align::CENTER);
  row->title = Gtk::make_managed<Gtk::Label>();
  row->title->set_halign(Gtk::Align::START);
  row->title->set_ellipsize(Pango::EllipsizeMode::END);
  labels->append(*row->title);
  row->subtitle = Gtk::make_managed<Gtk::Label>();
  row->subtitle->set_halign(Gtk::Align::START);
  row->subtitle->set_ellipsize(Pango::EllipsizeMode::END);
  row->subtitle->add_css_class("dimmed");
  row->subtitle->add_css_class("caption");
  labels->append(*row->subtitle);
  row_box->append(*labels);

  // Every possible action button exists on every row; BindListRow() shows
  // the ones that apply to the entry and level — see SetEntries() for
  // which flag gates which.
  row->favorite = MakeRowButton(*row_box, "non-starred-symbolic", _("Add to Favorites"));
  row->favorite->signal_clicked().connect([this, row] { signal_add_to_favorites_requested_.emit(row->index); });
  row->remove = MakeRowButton(*row_box, "user-trash-symbolic", _("Delete"));
  row->remove->signal_clicked().connect([this, row] { signal_delete_requested_.emit(row->index); });
  row->radio_settings = MakeRowButton(*row_box, "emblem-system-symbolic", _("Notifications"));
  row->radio_settings->signal_clicked().connect([this, row] { signal_radio_settings_requested_.emit(row->index); });
  row->move_up = MakeRowButton(*row_box, "go-up-symbolic", _("Move Up"));
  row->move_up->signal_clicked().connect([this, row] { signal_reorder_requested_.emit(row->index, row->index - 1); });
  row->move_down = MakeRowButton(*row_box, "go-down-symbolic", _("Move Down"));
  row->move_down->signal_clicked().connect([this, row] { signal_reorder_requested_.emit(row->index, row->index + 1); });
  row->chevron = Gtk::make_managed<Gtk::Image>();
  row->chevron->set_from_icon_name("go-next-symbolic");
  row->chevron->add_css_class("dimmed");
  row_box->append(*row->chevron);
  row->add_to_queue = MakeRowButton(*row_box, "list-add-symbolic", _("Add to Queue"));
  row->add_to_queue->signal_clicked().connect([this, row] { signal_add_to_queue_requested_.emit(row->index); });
  row->play_next = MakeRowButton(*row_box, "media-skip-forward-symbolic", _("Play Next"));
  row->play_next->signal_clicked().connect([this, row] { signal_play_next_requested_.emit(row->index); });
  // See SetEntries() — "play now" emits exactly what activating the row does.
  row->play_now = MakeRowButton(*row_box, "media-playback-start-symbolic", _("Play Now"));
  row->play_now->signal_clicked().connect([this, row] { signal_entry_activated_.emit(row->index); });
  row->add_to_playlist = MakeRowButton(*row_box, "bookmark-new-symbolic", _("Add to Playlist"));
  row->add_to_playlist->signal_clicked().connect([this, row] { signal_add_to_playlist_requested_.emit(row->index); });

  item->set_child(*row_box);
}

void LibraryView::BindListRow(const Glib::RefPtr<Gtk::ListItem>& item)
{
  auto string_object = std::dynamic_pointer_cast<Gtk::StringObject>(item->get_item());
  auto* row_box = item->get_child();
  if (!string_object || !row_box)
    return;
  unsigned index = static_cast<unsigned>(std::stoul(string_object->get_string().raw()));
  if (index >= all_entries_.size())
    return;
  const LibraryEntry& entry = all_entries_[index];
  auto* row = static_cast<LibraryListRow*>(g_object_get_data(G_OBJECT(row_box->gobj()), kLibraryListRowKey));
  row->index = index;

  row->title->set_text(entry.title.empty() ? _("Untitled") : entry.title);
  row->subtitle->set_text(entry.subtitle);
  row->subtitle->set_visible(!entry.subtitle.empty());

  row->thumbnail->SetFallbackIconName(entry.icon_name);
  row->thumbnail->SetGeneratedFallback(WantsGeneratedCover(entry) ? GeneratedSeed(entry) : "");
  // "avatar-default-symbolic" is exactly the icon IconNameForSubType()
  // (noson-backend.cpp) assigns for an artist — the one entry type with no
  // real art of its own to fall back to. A recycled row may still show the
  // previous entry's art, so it's reset to the fallback first.
  if (load_artist_images_ && entry.icon_name == "avatar-default-symbolic" && entry.art_uri.empty())
  {
    row->thumbnail->SetArtUri("");
    row->thumbnail->LoadArtistImage(entry.title);
  }
  else
  {
    row->thumbnail->SetArtUri(entry.art_uri);
  }

  bool leaf = !entry.is_container;
  row->favorite->set_visible(show_favorite_action_);
  row->remove->set_visible(show_delete_action_);
  row->radio_settings->set_visible(show_radio_settings_action_);
  // Only for a saved playlist's own, unfiltered tracks — then the index is
  // the track's real position, so it doubles for the edge checks.
  row->move_up->set_visible(list_reorder_active_);
  row->move_up->set_sensitive(index > 0);
  row->move_down->set_visible(list_reorder_active_);
  row->move_down->set_sensitive(index + 1 < all_entries_.size());
  row->chevron->set_visible(!leaf);
  // Add-to-queue/play-next back onto AVTransport::AddURIToQueue(), which
  // fails for anything System::CanQueueItem() rejects (a live radio
  // stream) — those levels get a single "play now" instead.
  row->add_to_queue->set_visible(leaf && show_queue_actions_);
  row->play_next->set_visible(leaf && show_queue_actions_);
  row->play_now->set_visible(leaf && !show_queue_actions_);
  row->add_to_playlist->set_visible(leaf && show_add_to_playlist_action_);
}

void LibraryView::SetupGridTile(const Glib::RefPtr<Gtk::ListItem>& item)
{
  auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  tile->add_css_class("library-tile");

  auto* thumbnail = Gtk::make_managed<CoverThumbnail>(120);
  thumbnail->set_halign(Gtk::Align::CENTER);
  tile->append(*thumbnail);

  // Two lines at most, both reserved (set_lines() alone only caps the
  // height), so a row of one-line titles lines up with a row of two-line
  // ones. width/max_width_chars keep a long title from widening its column.
  auto* title = Gtk::make_managed<Gtk::Label>();
  title->set_wrap(true);
  title->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  title->set_lines(2);
  title->set_ellipsize(Pango::EllipsizeMode::END);
  title->set_justify(Gtk::Justification::CENTER);
  title->set_width_chars(14);
  title->set_max_width_chars(14);
  title->set_yalign(0);
  title->add_css_class("library-tile-title");
  tile->append(*title);

  auto* subtitle = Gtk::make_managed<Gtk::Label>();
  subtitle->set_ellipsize(Pango::EllipsizeMode::END);
  subtitle->set_width_chars(14);
  subtitle->set_max_width_chars(14);
  subtitle->add_css_class("dimmed");
  subtitle->add_css_class("caption");
  tile->append(*subtitle);

  item->set_child(*tile);
}

void LibraryView::BindGridTile(const Glib::RefPtr<Gtk::ListItem>& item)
{
  auto string_object = std::dynamic_pointer_cast<Gtk::StringObject>(item->get_item());
  auto* tile = item->get_child();
  if (!string_object || !tile)
    return;
  unsigned index = static_cast<unsigned>(std::stoul(string_object->get_string().raw()));
  if (index >= all_entries_.size())
    return;
  const LibraryEntry& entry = all_entries_[index];

  auto* thumbnail = static_cast<CoverThumbnail*>(tile->get_first_child());
  auto* title = static_cast<Gtk::Label*>(thumbnail->get_next_sibling());
  auto* subtitle = static_cast<Gtk::Label*>(title->get_next_sibling());

  std::string title_text = entry.title.empty() ? _("Untitled") : entry.title;
  title->set_text(title_text);
  subtitle->set_text(entry.subtitle);
  subtitle->set_visible(!entry.subtitle.empty());
  tile->set_tooltip_text(entry.subtitle.empty() ? title_text : title_text + "\n" + entry.subtitle);

  thumbnail->SetFallbackIconName(entry.icon_name);
  thumbnail->SetGeneratedFallback(WantsGeneratedCover(entry) ? GeneratedSeed(entry) : "");
  // See BindListRow()'s identical check for why "avatar-default-symbolic"
  // specifically is the signal to use here. A recycled tile may still show
  // the previous entry's art, so it's reset to the fallback first.
  if (load_artist_images_ && entry.icon_name == "avatar-default-symbolic" && entry.art_uri.empty())
  {
    thumbnail->SetArtUri("");
    thumbnail->LoadArtistImage(entry.title);
  }
  else
  {
    thumbnail->SetArtUri(entry.art_uri);
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
  entries_model_->splice(0, entries_model_->get_n_items(), std::vector<Glib::ustring>{});
}

}  // namespace gnomos
