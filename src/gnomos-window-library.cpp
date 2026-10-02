// SPDX-License-Identifier: GPL-3.0-or-later
//
// GnomosWindow, continued: Library browsing, linked services, radio stations, playlists and their dialogs.
// The class itself and its constructor live in gnomos-window.cpp.

#include "gnomos-window.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/texture.h>
#include <giomm/application.h>
#include <giomm/asyncresult.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/notification.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/bytes.h>
#include <glibmm/checksum.h>
#include <glibmm/error.h>
#include <glibmm/keyfile.h>
#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/expander.h>
#include <gtkmm/expression.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/gridview.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/linkbutton.h>
#include <gtkmm/listview.h>
#include <gtkmm/range.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scale.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include "config.h"
#include "window-helpers.h"
#include "widgets/art-cache.h"
#include "widgets/artist-info-fetcher.h"
#include "widgets/cover-thumbnail.h"
#include "widgets/dialog-shell.h"
#include "widgets/http-fetch.h"
#include "widgets/lastfm-scrobbler.h"
#include "widgets/listenbrainz-scrobbler.h"
#include "widgets/lyrics-fetcher.h"
#include "widgets/radio-browser-service.h"

namespace gnomos
{

void GnomosWindow::RebuildLibraryNavEntries()
{
  // The static first section (Warteschlange/Favoriten/Alarme/Verlauf/
  // Bibliothek) is never touched here — only library_nav_section_/
  // services_nav_section_, cleared and repopulated in place below.
  adw_sidebar_section_remove_all(library_nav_section_);
  adw_sidebar_section_remove_all(services_nav_section_);
  if (adw_sidebar_section_get_sidebar(library_nav_section_))
    adw_sidebar_remove(ADW_SIDEBAR(nav_sidebar_), library_nav_section_);
  if (adw_sidebar_section_get_sidebar(services_nav_section_))
    adw_sidebar_remove(ADW_SIDEBAR(nav_sidebar_), services_nav_section_);

  // Splits the flat root-category list into two labeled groups: "Bibliothek"
  // (the locally-indexed-share namespace, object_id prefix "A:" — see
  // BrowseLibraryAsync()'s own comment on that prefix) and "Dienste"
  // (everything else content actually comes from outside that share: Sonos-
  // native saved playlists, the radio directory, and linked third-party
  // services). "Dienst verknüpfen…" lives in "Dienste" too, alongside
  // whatever's already linked — it used to be excluded from the sidebar
  // entirely (it opens a dialog rather than browsing into anything), which
  // meant the *only* way to ever discover it was clicking the top-level
  // "Bibliothek" nav row itself rather than any of its sub-items, landing
  // on the true library root where it's shown as a regular entry — not
  // obvious, confirmed live as a real "how do I even link a service"
  // question once "Dienste" existed as an obvious place to expect it.
  auto append_entry = [this](AdwSidebarSection* section, const LibraryEntry& entry) {
    AdwSidebarItem* item = adw_sidebar_item_new(entry.title.c_str());
    // Every root entry populates its own icon_name (BrowseLibraryAsync())
    // except a genuinely unexpected one this list was never meant to
    // handle — a plain folder glyph reads as "some kind of container"
    // regardless, rather than leaving the item with no icon at all.
    adw_sidebar_item_set_icon_name(item, entry.icon_name.empty() ? "folder-symbolic" : entry.icon_name.c_str());
    adw_sidebar_section_append(section, item);

    // Same special-case OnLibraryEntryActivated() already has for clicking
    // this same entry from the actual root level — opens the picker
    // dialog directly rather than trying to "browse into" a sentinel
    // object_id that was never a real container to begin with.
    if (entry.object_id == kLinkServiceSentinel)
    {
      SetSidebarItemAction(item, [this] { ShowLinkServiceDialog(); });
      return;
    }

    std::string object_id = entry.object_id;
    std::string title = entry.title;
    SetSidebarItemAction(item, [this, object_id, title] {
      // Jump straight to this category, discarding any deeper browse
      // position — matches what clicking it from the actual library root
      // level would do, since that's exactly where this list comes from.
      library_stack_.clear();
      library_stack_.push_back({"", _("Library")});
      library_stack_.push_back({object_id, title.empty() ? "—" : title});
      // Cleared and retitled synchronously, before switching the page
      // into view — see the plain "Bibliothek" row's own action above for
      // why: BrowseLibraryAsync() below is a real network round trip, and
      // library_view_ otherwise keeps showing whatever it last held (e.g.
      // the root category list itself, still populated from the startup
      // fetch that built this very sidebar) for that whole round trip
      // once the page is already visible, flashing stale content.
      // Confirmed live: clicking "Interpreten" as the very first library
      // navigation of a session briefly showed the root "Bibliothek"
      // list, not the artist grid.
      library_view_.ShowLoading();
      library_view_.SetLevelTitle(title.empty() ? "—" : title);
      backend_->BrowseLibraryAsync(object_id);
      adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), "library");
    });
  };

  bool has_library = std::any_of(library_root_entries_.begin(), library_root_entries_.end(),
                                  [](const LibraryEntry& e) { return e.object_id.compare(0, 2, "A:") == 0; });
  bool has_services = std::any_of(library_root_entries_.begin(), library_root_entries_.end(),
                                   [](const LibraryEntry& e) { return e.object_id.compare(0, 2, "A:") != 0; });

  if (has_library)
  {
    for (const LibraryEntry& entry : library_root_entries_)
      if (entry.object_id.compare(0, 2, "A:") == 0)
        append_entry(library_nav_section_, entry);
    adw_sidebar_append(ADW_SIDEBAR(nav_sidebar_), library_nav_section_);
  }
  if (has_services)
  {
    for (const LibraryEntry& entry : library_root_entries_)
      if (entry.object_id.compare(0, 2, "A:") != 0)
        append_entry(services_nav_section_, entry);
    adw_sidebar_append(ADW_SIDEBAR(nav_sidebar_), services_nav_section_);
  }
}

void GnomosWindow::OnFavoritesChanged()
{
  favorites_view_.SetItems(backend_->GetFavorites());
}

void GnomosWindow::OnLibraryChanged()
{
  current_library_entries_ = backend_->GetLibraryEntries();
  // Display only: a track without art of its own shows the cover of the
  // album (or playlist) it was opened from. Indices stay as the backend
  // has them, so nothing that acts on an entry is affected.
  if (auto art = library_container_art_.find(library_stack_.back().first);
      art != library_container_art_.end() && !art->second.empty())
  {
    for (LibraryEntry& entry : current_library_entries_)
      if (!entry.is_container && entry.art_uri.empty())
        entry.art_uri = art->second;
  }

  // Grid view (cover-art tiles, like Euphonica's own Albums/Artists grid —
  // https://github.com/htkhiem/euphonica) is available whenever any entry
  // at this level says so — LibraryEntry::display_as_grid is populated
  // uniformly by NosonBackend regardless of whether the level came from
  // the local library or a third-party service (see that field's own
  // comment for the two different underlying heuristics), so this no
  // longer needs to branch on where we are the way it used to. Whether to
  // actually *render* one when available is the user's own choice
  // (prefer_grid_view_, toggled via view_mode_button_), not decided here.
  bool grid_available = std::any_of(current_library_entries_.begin(), current_library_entries_.end(),
                                     [](const LibraryEntry& entry) { return entry.display_as_grid; });

  // Favoriting only offered below the true root — "Interpreten"/"Alben"/...
  // are static categories, not real content Sonos has anything to
  // favorite. Deletion only offered browsing "SQ:" or "R:0/0" themselves,
  // where every entry really is a destroyable saved playlist or custom
  // radio station. Add-to-playlist offered below the true root too, but
  // *not* while browsing "R:0/0" — confirmed live: a saved Sonos playlist
  // is conceptually a list of tracks, and adding a live radio stream to
  // one via AddURIToSavedQueue() reads as nonsensical there even though
  // nothing stops the SOAP call itself from accepting it. Reordering only
  // offered while viewing a *specific* playlist's own tracks (an
  // "SQ:<id>" level, not "SQ:" itself).
  const std::string& current_object_id = library_stack_.back().first;
  bool below_root = library_stack_.size() > 1;
  bool is_radio_level = current_object_id == "R:0/0";
  bool viewing_one_playlist =
      current_object_id.compare(0, 3, "SQ:") == 0 && current_object_id != "SQ:";
  // Bulk "play all"/"add all to queue" and the per-row "add to queue"/
  // "play next" buttons are all excluded for "R:0/0", same underlying
  // reason as add-to-playlist above — see show_play_all_action's/
  // show_queue_all_action's/show_queue_actions's own comments in
  // library-view.h (a per-row "play now" button takes the place of the
  // latter two there instead).
  library_view_.SetEntries(current_library_entries_, grid_available, prefer_grid_view_, below_root,
                            current_object_id == "SQ:" || is_radio_level, below_root && !is_radio_level,
                            viewing_one_playlist, !is_radio_level, !is_radio_level, !is_radio_level,
                            load_artist_images_, is_radio_level);
  library_view_.SetLevelTitle(library_stack_.back().second);
  library_view_.SetBackVisible(library_stack_.size() > 1);
  library_view_.SetAddVisible(current_object_id == "R:0/0");

  // Root level specifically — see library_root_entries_'s own comment for
  // why this can't just reuse current_library_entries_ unconditionally
  // (it tracks whatever level is currently browsed, usually not the root).
  if (library_stack_.size() == 1)
  {
    library_root_entries_ = current_library_entries_;
    RebuildLibraryNavEntries();
  }
}

void GnomosWindow::OnLibraryEntryActivated(unsigned index)
{
  if (index >= current_library_entries_.size())
    return;
  const LibraryEntry& entry = current_library_entries_[index];
  if (entry.object_id == kLinkServiceSentinel)
  {
    ShowLinkServiceDialog();
  }
  else if (entry.is_container)
  {
    library_stack_.push_back({entry.object_id, entry.title.empty() ? "—" : entry.title});
    // Tracks inside an album often come without art of their own while
    // the album has some — remembered here and lent to them in
    // OnLibraryChanged().
    library_container_art_[entry.object_id] = entry.art_uri;
    // Cleared and retitled synchronously, before the (async, real
    // network round-trip) browse — same fix and reasoning as the nav
    // sidebar's own library shortcuts: library_view_ otherwise keeps
    // showing this level's *previous* content (e.g. the root category
    // list while "Alben" itself is still loading) for the whole round
    // trip, flashing stale content. Confirmed live for this same
    // in-view navigation, not just the sidebar shortcuts.
    library_view_.ShowLoading();
    library_view_.SetLevelTitle(library_stack_.back().second);
    backend_->BrowseLibraryAsync(entry.object_id);
  }
  else
  {
    backend_->PlayLibraryItem(index);
  }
}

void GnomosWindow::OnLibraryBackRequested()
{
  if (library_stack_.size() <= 1)
    return;
  library_stack_.pop_back();
  // See OnLibraryEntryActivated()'s identical fix and comment — going
  // back is exactly the same "genuinely different level, async browse"
  // case, just in the other direction.
  library_view_.ShowLoading();
  library_view_.SetLevelTitle(library_stack_.back().second);
  backend_->BrowseLibraryAsync(library_stack_.back().first);
}

void GnomosWindow::StartLibraryIndexProgressPolling()
{
  // A re-click while a previous poll is still running (or still waiting to
  // ever see ShareIndexInProgress go true) shouldn't stack up a second
  // timer/connection alongside the first.
  library_index_poll_connection_.disconnect();
  library_index_status_connection_.disconnect();

  auto seen_in_progress = std::make_shared<bool>(false);
  auto ticks_without_progress = std::make_shared<int>(0);
  library_index_status_connection_ = backend_->signal_library_index_status_changed().connect(
      [this, seen_in_progress, ticks_without_progress] {
        if (backend_->GetLibraryIndexInProgress())
        {
          *seen_in_progress = true;
          *ticks_without_progress = 0;
          return;
        }
        if (!*seen_in_progress && ++*ticks_without_progress < 5)
          // Sonos may not have flipped ShareIndexInProgress to true yet —
          // give it a few more ticks before giving up silently, rather
          // than risk a false "completed" toast for a scan that hasn't
          // actually registered as running at all yet.
          return;

        library_index_poll_connection_.disconnect();
        library_index_status_connection_.disconnect();
        if (*seen_in_progress)
        {
          std::string error = backend_->GetLibraryIndexLastError();
          ShowToast(error.empty() ? std::string(_("Library scan finished"))
                                  : Format(_("Library scan failed: %s"), error.c_str()));
        }
        // Never observed running at all (too fast to catch between polls,
        // or the scan silently did nothing) — RefreshLibraryIndex()'s own
        // "gestartet" toast already set expectations, so this stays quiet
        // rather than risk a misleading message either way.
      });
  library_index_poll_connection_ = Glib::signal_timeout().connect(
      [this] {
        backend_->CheckLibraryIndexProgressAsync();
        return true;  // signal_library_index_status_changed()'s own handler above stops this
      },
      2000);
}

void GnomosWindow::ShowLinkServiceDialog()
{
  std::vector<LinkableService> services = backend_->GetLinkableServices();
  if (services.empty())
  {
    ShowToast(_("No services available to link."));
    return;
  }

  auto* dialog = new DialogShell(*this);
  dialog->set_title(_("Link Service"));
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* label = Gtk::make_managed<Gtk::Label>(_("Service"));
  label->set_halign(Gtk::Align::START);
  content->append(*label);

  std::vector<Glib::ustring> names;
  names.reserve(services.size());
  for (const LinkableService& svc : services)
    names.push_back(svc.name);
  auto model = Gtk::StringList::create(names);
  auto* dropdown = Gtk::make_managed<Gtk::DropDown>(model);
  // set_enable_search() alone shows a search entry in the popup but never
  // actually filters anything without this — confirmed live. Despite the
  // gtkmm/GTK docs describing GtkStringList items as auto-detected, the
  // search filter itself still needs an explicit expression telling it
  // how to pull a comparable string out of each item.
  dropdown->set_expression(
      Gtk::PropertyExpression<Glib::ustring>::create(Gtk::StringObject::get_type(), "string"));
  dropdown->set_enable_search(true);
  content->append(*dropdown);

  auto* info_label =
      Gtk::make_managed<Gtk::Label>(_("A link will open next, which you finish in a browser."));
  info_label->set_wrap(true);
  info_label->set_halign(Gtk::Align::START);
  info_label->add_css_class("dimmed");
  info_label->add_css_class("caption");
  content->append(*info_label);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* start_button = Gtk::make_managed<Gtk::Button>(_("Start Linking"));
  start_button->add_css_class("suggested-action");
  start_button->signal_clicked().connect([this, dialog, dropdown, services] {
    guint selected = dropdown->get_selected();
    if (selected < services.size())
    {
      pending_link_service_id_ = services[selected].id;
      pending_link_service_name_ = services[selected].name;
      backend_->BeginServiceLink(services[selected].id);
    }
    dialog->close();
  });
  button_box->append(*cancel_button);
  button_box->append(*start_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->present();
}

void GnomosWindow::ShowPlayStreamDialog()
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title(_("Play Stream"));
  dialog->set_default_size(380, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* disclosure_label = Gtk::make_managed<Gtk::Label>(
      _("Plays a stream address once without saving it — for a permanent station, see “Add Radio Station”."));
  disclosure_label->set_halign(Gtk::Align::START);
  disclosure_label->set_wrap(true);
  disclosure_label->add_css_class("caption");
  disclosure_label->add_css_class("dimmed");
  content->append(*disclosure_label);

  auto* url_label = Gtk::make_managed<Gtk::Label>(_("Stream address"));
  url_label->set_halign(Gtk::Align::START);
  content->append(*url_label);
  auto* url_entry = Gtk::make_managed<Gtk::Entry>();
  url_entry->set_placeholder_text("https://…");
  url_entry->set_activates_default(true);
  content->append(*url_entry);

  auto* title_label = Gtk::make_managed<Gtk::Label>(_("Title (optional)"));
  title_label->set_halign(Gtk::Align::START);
  content->append(*title_label);
  auto* title_entry = Gtk::make_managed<Gtk::Entry>();
  title_entry->set_placeholder_text(_("Shown while playing"));
  title_entry->set_activates_default(true);
  content->append(*title_entry);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* play_button = Gtk::make_managed<Gtk::Button>(_("Play"));
  play_button->add_css_class("suggested-action");
  auto do_play = [this, dialog, url_entry, title_entry] {
    Glib::ustring url = url_entry->get_text();
    if (url.empty())
      return;
    Glib::ustring title = title_entry->get_text();
    backend_->PlayStreamAsync(url.raw(), title.empty() ? url.raw() : title.raw());
    dialog->close();
  };
  play_button->signal_clicked().connect(do_play);
  url_entry->signal_activate().connect(do_play);
  title_entry->signal_activate().connect(do_play);
  button_box->append(*cancel_button);
  button_box->append(*play_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*play_button);
  dialog->present();
  url_entry->grab_focus();
}

void GnomosWindow::ExportRadioFavorites()
{
  std::vector<ExportableRadioFavorite> favorites = backend_->GetExportableRadioFavorites();
  if (favorites.empty())
  {
    ShowToast(_("No radio station favorites to export"));
    return;
  }

  auto file_dialog = Gtk::FileDialog::create();
  file_dialog->set_title(_("Export Radio Station Favorites"));
  file_dialog->set_initial_name("gnomos-radiosender.json");
  file_dialog->save(*this, [this, file_dialog, favorites](Glib::RefPtr<Gio::AsyncResult>& result) {
    Glib::RefPtr<Gio::File> file;
    try
    {
      file = file_dialog->save_finish(result);
    }
    catch (const Glib::Error&)
    {
      return;  // user cancelled — nothing to report
    }
    if (!file)
      return;

    JsonBuilder* builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "gnomos_radio_favorites");
    json_builder_begin_array(builder);
    for (const ExportableRadioFavorite& favorite : favorites)
    {
      json_builder_begin_object(builder);
      json_builder_set_member_name(builder, "title");
      json_builder_add_string_value(builder, favorite.title.c_str());
      json_builder_set_member_name(builder, "url");
      json_builder_add_string_value(builder, favorite.stream_url.c_str());
      json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonGenerator* generator = json_generator_new();
    JsonNode* root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    gsize length = 0;
    gchar* data = json_generator_to_data(generator, &length);

    try
    {
      std::string new_etag;
      file->replace_contents(std::string(data, length), "", new_etag);
      ShowToast(Format(ngettext("%zu radio station exported", "%zu radio stations exported", favorites.size()),
                       favorites.size()));
    }
    catch (const Glib::Error&)
    {
      ShowToast(_("Export failed"));
    }

    g_free(data);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
  });
}

void GnomosWindow::ImportRadioFavorites()
{
  auto file_dialog = Gtk::FileDialog::create();
  file_dialog->set_title(_("Import Radio Station Favorites"));
  auto json_filter = Gtk::FileFilter::create();
  json_filter->set_name(_("JSON files"));
  json_filter->add_pattern("*.json");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(json_filter);
  file_dialog->set_filters(filters);
  file_dialog->open(*this, [this, file_dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
    Glib::RefPtr<Gio::File> file;
    try
    {
      file = file_dialog->open_finish(result);
    }
    catch (const Glib::Error&)
    {
      return;  // user cancelled — nothing to report
    }
    if (!file)
      return;

    char* contents = nullptr;
    gsize length = 0;
    if (!file->load_contents(contents, length))
    {
      ShowToast(_("Couldn't read the file"));
      return;
    }
    std::string body(contents, length);
    g_free(contents);

    JsonParser* parser = json_parser_new();
    GError* error = nullptr;
    if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
    {
      if (error)
        g_error_free(error);
      g_object_unref(parser);
      ShowToast(_("The file isn't a valid backup JSON"));
      return;
    }

    unsigned imported = 0;
    JsonNode* root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_OBJECT(root))
    {
      JsonObject* root_obj = json_node_get_object(root);
      if (json_object_has_member(root_obj, "gnomos_radio_favorites") &&
          JSON_NODE_HOLDS_ARRAY(json_object_get_member(root_obj, "gnomos_radio_favorites")))
      {
        JsonArray* array = json_object_get_array_member(root_obj, "gnomos_radio_favorites");
        guint array_length = json_array_get_length(array);
        for (guint i = 0; i < array_length; ++i)
        {
          JsonObject* entry = json_array_get_object_element(array, i);
          if (!entry)
            continue;
          std::string title =
              json_object_has_member(entry, "title") ? json_object_get_string_member(entry, "title") : "";
          std::string url = json_object_has_member(entry, "url") ? json_object_get_string_member(entry, "url") : "";
          if (title.empty() || url.empty())
            continue;
          backend_->AddRadioStation(title, url);
          ++imported;
        }
      }
    }
    g_object_unref(parser);

    ShowToast(imported > 0 ? Format(ngettext("%zu radio station imported", "%zu radio stations imported", imported),
                                    static_cast<size_t>(imported))
                           : std::string(_("No radio stations found in this file")));
  });
}

namespace
{
// One parsed M3U/PLS entry — best-effort, client-side-only guesses at
// what a matching library track's own title/artist would be. artist may
// be empty (no reliable way to split it out of that particular line).
struct M3uEntry
{
  std::string title;
  std::string artist;
};

// A plain file path/URL's own filename, minus directory and extension —
// the fallback title guess for an M3U entry with no #EXTINF line, or a
// PLS entry with no TitleN= (both real cases, confirmed by the format
// specs themselves: EXTINF/Title are optional metadata, File/the bare
// path is the only thing either format actually requires).
std::string GuessTitleFromPath(const std::string& path)
{
  std::string name = path;
  size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0)
    name = name.substr(0, dot);
  return name;
}

// "Artist - Title", the convention every common M3U/PLS generator uses
// for its own free-text title metadata — split on the first " - ", or
// treat the whole string as title-only if that separator isn't there.
void SplitArtistTitle(const std::string& info, std::string& artist, std::string& title)
{
  size_t dash = info.find(" - ");
  if (dash != std::string::npos)
  {
    artist = info.substr(0, dash);
    title = info.substr(dash + 3);
  }
  else
  {
    artist.clear();
    title = info;
  }
}

std::string StripLineEnding(std::string line)
{
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.pop_back();
  return line;
}

std::vector<M3uEntry> ParseM3u(const std::string& contents)
{
  std::vector<M3uEntry> entries;
  std::istringstream stream(contents);
  std::string line;
  std::string pending_artist, pending_title;
  bool have_pending = false;
  while (std::getline(stream, line))
  {
    line = StripLineEnding(line);
    if (line.empty())
      continue;
    if (line.compare(0, 8, "#EXTINF:") == 0)
    {
      size_t comma = line.find(',');
      SplitArtistTitle(comma != std::string::npos ? line.substr(comma + 1) : "", pending_artist, pending_title);
      have_pending = !pending_title.empty();
      continue;
    }
    if (line[0] == '#')
      continue;  // #EXTM3U and any other directive/comment line
    M3uEntry entry;
    if (have_pending)
    {
      entry.title = pending_title;
      entry.artist = pending_artist;
    }
    else
    {
      entry.title = GuessTitleFromPath(line);
    }
    if (!entry.title.empty())
      entries.push_back(std::move(entry));
    have_pending = false;
  }
  return entries;
}

std::vector<M3uEntry> ParsePls(const std::string& contents)
{
  // PLS's own File.../Title... keys are numbered independently and in no
  // guaranteed order in the file, so both need collecting into per-index
  // maps first, then paired up afterward — unlike M3U's own strictly
  // sequential #EXTINF-then-path structure.
  std::map<int, std::string> titles;
  std::map<int, std::string> files;
  std::istringstream stream(contents);
  std::string line;
  while (std::getline(stream, line))
  {
    line = StripLineEnding(line);
    size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string value = line.substr(eq + 1);
    if (key.compare(0, 5, "Title") == 0)
      titles[std::atoi(key.c_str() + 5)] = value;
    else if (key.compare(0, 4, "File") == 0)
      files[std::atoi(key.c_str() + 4)] = value;
  }

  std::vector<M3uEntry> entries;
  for (const auto& [index, file] : files)
  {
    M3uEntry entry;
    auto title_it = titles.find(index);
    if (title_it != titles.end() && !title_it->second.empty())
      SplitArtistTitle(title_it->second, entry.artist, entry.title);
    else
      entry.title = GuessTitleFromPath(file);
    if (!entry.title.empty())
      entries.push_back(std::move(entry));
  }
  return entries;
}

std::string ToLowerCopy(const std::string& s)
{
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  return lower;
}

// For each parsed entry, a substring match (either direction — a parsed
// title that's a truncated/expanded variant of the real one still counts)
// against every candidate's own title, refined by *also* requiring the
// candidate's own artist (subtitle) to contain the parsed artist guess
// when one was available — falling back to a title-only match if no
// combined match exists, rather than dropping the entry outright, since a
// title-only match is still usually right and formatting differences
// between an M3U's own artist string and Sonos's own tag data are common.
std::vector<unsigned> MatchM3uEntries(const std::vector<M3uEntry>& parsed, const std::vector<LibraryEntry>& tracks)
{
  std::vector<unsigned> matched;
  for (const M3uEntry& entry : parsed)
  {
    std::string wanted_title = ToLowerCopy(entry.title);
    std::string wanted_artist = ToLowerCopy(entry.artist);
    if (wanted_title.empty())
      continue;

    int best_combined = -1;
    int best_title_only = -1;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
      std::string track_title = ToLowerCopy(tracks[i].title);
      bool title_matches = track_title.find(wanted_title) != std::string::npos ||
                            wanted_title.find(track_title) != std::string::npos;
      if (!title_matches)
        continue;
      if (best_title_only < 0)
        best_title_only = static_cast<int>(i);
      if (!wanted_artist.empty() && ToLowerCopy(tracks[i].subtitle).find(wanted_artist) != std::string::npos)
      {
        best_combined = static_cast<int>(i);
        break;
      }
    }
    int chosen = best_combined >= 0 ? best_combined : best_title_only;
    if (chosen >= 0)
      matched.push_back(static_cast<unsigned>(chosen));
  }
  return matched;
}
}  // namespace

void GnomosWindow::ImportM3uPlaylist()
{
  auto file_dialog = Gtk::FileDialog::create();
  file_dialog->set_title(_("Import M3U/PLS Playlist"));
  auto playlist_filter = Gtk::FileFilter::create();
  playlist_filter->set_name(_("Playlist files"));
  playlist_filter->add_pattern("*.m3u");
  playlist_filter->add_pattern("*.m3u8");
  playlist_filter->add_pattern("*.pls");
  auto filters = Gio::ListStore<Gtk::FileFilter>::create();
  filters->append(playlist_filter);
  file_dialog->set_filters(filters);
  file_dialog->open(*this, [this, file_dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
    Glib::RefPtr<Gio::File> file;
    try
    {
      file = file_dialog->open_finish(result);
    }
    catch (const Glib::Error&)
    {
      return;  // user cancelled — nothing to report
    }
    if (!file)
      return;

    char* contents = nullptr;
    gsize length = 0;
    if (!file->load_contents(contents, length))
    {
      ShowToast(_("Couldn't read the file"));
      return;
    }
    std::string body(contents, length);
    g_free(contents);

    std::string lower_path = ToLowerCopy(file->get_basename());
    bool is_pls = lower_path.size() >= 4 && lower_path.compare(lower_path.size() - 4, 4, ".pls") == 0;
    std::vector<M3uEntry> parsed = is_pls ? ParsePls(body) : ParseM3u(body);
    if (parsed.empty())
    {
      ShowToast(_("No entries found in this playlist file"));
      return;
    }

    ShowToast(_("Matching playlist…"));
    // signal_tracks_for_matching_ready() is self-disconnecting — this
    // import is the only thing that ever triggers
    // FetchAllTracksForMatchingAsync(), so nothing else should react to a
    // *later* fetch this same connection might otherwise still be alive
    // for.
    auto connection = std::make_shared<sigc::connection>();
    *connection = backend_->signal_tracks_for_matching_ready().connect([this, parsed, connection] {
      connection->disconnect();
      std::vector<LibraryEntry> tracks = backend_->GetTracksForMatching();
      std::vector<unsigned> matched_indices = MatchM3uEntries(parsed, tracks);
      if (matched_indices.empty())
      {
        ShowToast(Format(ngettext("The track wasn't found in the library", "None of the %zu tracks were found in the library",
                                  parsed.size()),
                         parsed.size()));
        return;
      }
      backend_->AddTrackMatchesToQueue(matched_indices);
      ShowToast(Format(ngettext("%zu of %zu track added to the queue — save it as a playlist from there",
                                "%zu of %zu tracks added to the queue — save them as a playlist from there", parsed.size()),
                       matched_indices.size(), parsed.size()));
    });
    backend_->FetchAllTracksForMatchingAsync();
  });
}

void GnomosWindow::ShowDeleteFavoriteConfirmDialog(unsigned index)
{
  std::vector<FavoriteItem> favorites = backend_->GetFavorites();
  std::string body = index < favorites.size() && !favorites[index].title.empty()
                         ? Format(_("Really remove “%s” from your favorites?"), favorites[index].title.c_str())
                         : std::string(_("Really remove this favorite?"));
  ShowConfirmDialog(_("Remove Favorite?"), body, _("Remove"),
                     [this, index] { backend_->DeleteFavorite(index); });
}

void GnomosWindow::ShowDeleteLibraryEntryConfirmDialog(unsigned index)
{
  bool is_radio = library_stack_.back().first == "R:0/0";
  bool has_title = index < current_library_entries_.size() && !current_library_entries_[index].title.empty();
  std::string body = has_title ? Format(_("Really delete “%s”?"), current_library_entries_[index].title.c_str())
                     : is_radio ? std::string(_("Really delete this radio station?"))
                                : std::string(_("Really delete this playlist?"));
  std::string heading = is_radio ? _("Delete Radio Station?") : _("Delete Playlist?");
  ShowConfirmDialog(heading, body, _("Delete"), [this, index, is_radio] {
    if (is_radio)
      backend_->DeleteLibraryRadioStation(index);
    else
      backend_->DeleteLibraryPlaylist(index);
    // Both run on the same serial tasks_ worker this browse gets queued
    // on, so the delete always executes first — see
    // DeleteLibraryPlaylist()'s own comment for why NosonBackend can't
    // just do this itself.
    backend_->BrowseLibraryAsync(library_stack_.back().first);
  });
}

void GnomosWindow::ShowAddToPlaylistDialog(unsigned library_index)
{
  backend_->FetchSavedPlaylistsAsync();
  // One-shot: FetchSavedPlaylistsAsync() is async (a real Browse() round
  // trip), so the dialog can't be built until its result actually arrives
  // — a std::shared_ptr<sigc::connection> captured by value lets the
  // lambda disconnect itself from inside its own body once it's run once,
  // same trick GnomosWindow uses nowhere else yet but sigc++ itself has no
  // simpler built-in for "connect, fire once, auto-disconnect" outside
  // Glib::signal_idle()'s own connect_once() (which is main-loop-idle
  // specific, not applicable to a plain sigc::signal like this one).
  auto connection = std::make_shared<sigc::connection>();
  *connection = backend_->signal_saved_playlists_changed().connect([this, library_index, connection] {
    connection->disconnect();
    std::vector<LibraryEntry> playlists = backend_->GetSavedPlaylists();

    auto* dialog = new DialogShell(*this);
    dialog->set_title(_("Add to Playlist"));
        dialog->set_default_size(360, -1);

    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    content->set_margin_top(18);
    content->set_margin_bottom(18);
    content->set_margin_start(18);
    content->set_margin_end(18);

    auto* label = Gtk::make_managed<Gtk::Label>(_("Playlist"));
    label->set_halign(Gtk::Align::START);
    content->append(*label);

    // "Neue Playlist…" always sits first — mirrors the "Dienst
    // verknüpfen…" sentinel row pattern (library-view.cpp's own root
    // categories) for the same reason: creating one shouldn't require
    // already having a queue to save first, which was the only way to
    // create a playlist at all before this (confirmed live: an empty
    // playlist list here used to just toast "Keine Playlisten vorhanden"
    // and refuse to open a dialog at all).
    std::vector<Glib::ustring> names;
    names.reserve(playlists.size() + 1);
    names.push_back(_("New Playlist…"));
    for (const LibraryEntry& entry : playlists)
      names.push_back(entry.title.empty() ? _("Untitled") : entry.title);
    auto model = Gtk::StringList::create(names);
    auto* dropdown = Gtk::make_managed<Gtk::DropDown>(model);
    content->append(*dropdown);

    // Only actually used while "Neue Playlist…" (index 0) is selected —
    // kept unconditionally visible rather than shown/hidden reactively as
    // the dropdown selection changes, since it's still perfectly clear
    // from the placeholder alone which choice it belongs to.
    auto* new_playlist_entry = Gtk::make_managed<Gtk::Entry>();
    new_playlist_entry->set_placeholder_text(_("Name for “New Playlist…”"));
    new_playlist_entry->set_activates_default(true);
    content->append(*new_playlist_entry);

    auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    button_box->set_halign(Gtk::Align::END);
    button_box->set_margin_top(6);
    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
    button_box->append(*cancel_button);
    auto* confirm_button = Gtk::make_managed<Gtk::Button>(_("Add"));
    confirm_button->add_css_class("suggested-action");
    confirm_button->signal_clicked().connect(
        [this, dialog, dropdown, new_playlist_entry, playlists, library_index] {
          guint selected = dropdown->get_selected();
          if (selected == GTK_INVALID_LIST_POSITION)
            return;
          if (selected == 0)
          {
            std::string title = new_playlist_entry->get_text().raw();
            if (title.empty())
            {
              new_playlist_entry->grab_focus();
              return;
            }
            backend_->CreatePlaylistAndAddLibraryItem(library_index, title);
            ShowToast(_("Playlist created and track added"));
          }
          else
          {
            if (selected - 1 >= playlists.size())
              return;
            backend_->AddLibraryItemToPlaylist(library_index, playlists[selected - 1].object_id);
            ShowToast(_("Added to playlist"));
          }
          dialog->close();
        });
    button_box->append(*confirm_button);
    content->append(*button_box);

    dialog->set_child(*content);
    dialog->set_default_widget(*confirm_button);
    dialog->present();
  });
}

void GnomosWindow::ShowAddRadioStationDialog()
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title(_("Add Radio Station"));
  dialog->set_default_size(420, 560);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* disclosure_label = Gtk::make_managed<Gtk::Label>(
      _("Searches the public station directory at radio-browser.info — a real request over the internet, not a local Sonos query."));
  disclosure_label->set_halign(Gtk::Align::START);
  disclosure_label->set_wrap(true);
  disclosure_label->add_css_class("caption");
  disclosure_label->add_css_class("dimmed");
  content->append(*disclosure_label);

  auto* search_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* search_entry = Gtk::make_managed<Gtk::Entry>();
  search_entry->set_placeholder_text(_("Station name… (empty = most popular)"));
  search_entry->set_hexpand(true);
  search_row->append(*search_entry);
  // Populated once FetchCountries() resolves — "Alle Länder" (no filter) is
  // always index 0 regardless, so search can fire before the country list
  // itself has even loaded.
  auto country_model = Gtk::StringList::create({_("All Countries")});
  auto* country_dropdown = Gtk::make_managed<Gtk::DropDown>(country_model);
  search_row->append(*country_dropdown);
  // Same index-0-is-"no filter" convention as country_dropdown, populated
  // once FetchTags() resolves — see genres' own comment below.
  auto genre_model = Gtk::StringList::create({_("All Genres")});
  auto* genre_dropdown = Gtk::make_managed<Gtk::DropDown>(genre_model);
  search_row->append(*genre_dropdown);
  auto* search_button = Gtk::make_managed<Gtk::Button>();
  search_button->set_icon_name("system-search-symbolic");
  search_row->append(*search_button);
  content->append(*search_row);

  auto* results_list = Gtk::make_managed<Gtk::ListBox>();
  results_list->set_selection_mode(Gtk::SelectionMode::NONE);
  auto* results_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  results_scroller->set_child(*results_list);
  results_scroller->set_vexpand(true);
  results_scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  content->append(*results_scroller);

  // countrycodes[i] index-aligns with country_model — empty string at index
  // 0 ("Alle Länder") means no country filter; shared_ptr since both the
  // FetchCountries() callback and every later search need to read it.
  auto countrycodes = std::make_shared<std::vector<std::string>>();
  countrycodes->push_back("");
  // Same convention, index-aligned with genre_model — see FetchTags()'s own
  // comment for why this is user-submitted free text, not a curated list.
  auto genre_tags = std::make_shared<std::vector<std::string>>();
  genre_tags->push_back("");
  // Index-aligned with whatever's currently in results_list, same
  // real-index convention every other list in this app already uses —
  // rebuilt fresh on every search, so always valid for the results
  // currently on screen.
  auto results = std::make_shared<std::vector<RadioBrowserStation>>();

  // Shared by both ways to add a result — clicking its own per-row "+"
  // button, and activating the row itself (clicking anywhere else on it).
  // Confirmed live these need to actually be the same code path: a
  // Gtk::Button nested inside a Gtk::ListBoxRow consumes its own click
  // rather than letting it propagate into row-activated, so a "+" button
  // that only *looks* like it adds something (relying on the row beneath
  // it to fire instead) silently does nothing when clicked directly.
  auto add_station = [this](const RadioBrowserStation& station) {
    backend_->AddRadioStation(station.name, station.url, station.favicon);
    // Same reasoning as ShowDeleteLibraryEntryConfirmDialog()'s own
    // re-browse — AddRadioStation() is queued first on the same serial
    // worker.
    backend_->BrowseLibraryAsync(library_stack_.back().first);
    ShowToast(Format(_("“%s” added"), station.name.c_str()));
  };

  auto run_search = [this, search_entry, country_dropdown, countrycodes, genre_dropdown, genre_tags, results,
                      results_list, add_station] {
    while (Gtk::Widget* child = results_list->get_first_child())
      results_list->remove(*child);
    std::string countrycode;
    guint selected = country_dropdown->get_selected();
    if (selected != GTK_INVALID_LIST_POSITION && selected < countrycodes->size())
      countrycode = (*countrycodes)[selected];
    std::string tag;
    guint genre_selected = genre_dropdown->get_selected();
    if (genre_selected != GTK_INVALID_LIST_POSITION && genre_selected < genre_tags->size())
      tag = (*genre_tags)[genre_selected];
    RadioBrowserService::Instance().SearchStations(
        search_entry->get_text(), countrycode, tag,
        [results, results_list, add_station](std::vector<RadioBrowserStation> stations) {
          *results = std::move(stations);
          if (results->empty())
          {
            auto* placeholder = Gtk::make_managed<Gtk::Label>(_("No stations found."));
            placeholder->add_css_class("dimmed");
            placeholder->set_margin_top(12);
            placeholder->set_margin_bottom(12);
            results_list->append(*placeholder);
            return;
          }
          for (const RadioBrowserStation& station : *results)
          {
            auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row_box->set_margin_top(6);
            row_box->set_margin_bottom(6);
            row_box->set_margin_start(6);
            row_box->set_margin_end(6);

            auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            labels->set_hexpand(true);
            auto* title = Gtk::make_managed<Gtk::Label>(station.name);
            title->set_halign(Gtk::Align::START);
            title->set_ellipsize(Pango::EllipsizeMode::END);
            labels->append(*title);
            std::string subtitle = station.countrycode;
            if (!station.codec.empty())
              subtitle += (subtitle.empty() ? "" : " · ") + station.codec;
            if (station.bitrate > 0)
              subtitle += " · " + Format(_("%u kbps"), static_cast<unsigned>(station.bitrate));
            if (!subtitle.empty())
            {
              auto* subtitle_label = Gtk::make_managed<Gtk::Label>(subtitle);
              subtitle_label->set_halign(Gtk::Align::START);
              subtitle_label->set_ellipsize(Pango::EllipsizeMode::END);
              subtitle_label->add_css_class("dimmed");
              subtitle_label->add_css_class("caption");
              labels->append(*subtitle_label);
            }
            row_box->append(*labels);

            auto* add_button = Gtk::make_managed<Gtk::Button>();
            add_button->set_icon_name("list-add-symbolic");
            add_button->add_css_class("flat");
            add_button->set_valign(Gtk::Align::CENTER);
            add_button->set_tooltip_text(_("Add"));
            add_button->signal_clicked().connect([add_station, station] { add_station(station); });
            row_box->append(*add_button);

            results_list->append(*row_box);
          }
        });
  };
  search_button->signal_clicked().connect(run_search);
  search_entry->signal_activate().connect(run_search);

  // Row-add wiring needs `results` (index-aligned with whatever's on
  // screen right now) resolved at *click* time, not capture time — done via
  // results_list's own row-activated-equivalent below instead of per-row
  // signal_clicked() connections, since Gtk::ListBox already gives a
  // reliable index via get_index() the same way LibraryView's own rows do.
  results_list->signal_row_activated().connect([results, add_station](Gtk::ListBoxRow* row) {
    if (!row)
      return;
    int index = row->get_index();
    if (index < 0 || static_cast<size_t>(index) >= results->size())
      return;
    add_station((*results)[static_cast<size_t>(index)]);
  });
  // Rows aren't buttons themselves, but ListBox rows are activatable by
  // default on click — the per-row add_button above is purely a visual
  // affordance (same "the whole row already does this" pattern the rest of
  // the app uses for is_container rows' chevron).
  results_list->set_activate_on_single_click(true);

  RadioBrowserService::Instance().FetchCountries(
      [country_model, countrycodes](std::vector<RadioBrowserCountry> countries) {
        std::sort(countries.begin(), countries.end(),
                  [](const RadioBrowserCountry& a, const RadioBrowserCountry& b) { return a.name < b.name; });
        for (const RadioBrowserCountry& country : countries)
        {
          if (country.countrycode.empty())
            continue;  // no usable filter value for this entry — skip it
          country_model->append(country.name + " (" + std::to_string(country.station_count) + ")");
          countrycodes->push_back(country.countrycode);
        }
      });

  RadioBrowserService::Instance().FetchTags([genre_model, genre_tags](std::vector<RadioBrowserTag> tags) {
    // Already sorted by station_count descending (FetchTags()' own query)
    // — kept in that order rather than alphabetized like countries above,
    // so the most useful genres land near the top of a long, uncurated
    // list instead of wherever their name happens to sort to.
    for (const RadioBrowserTag& tag : tags)
    {
      genre_model->append(tag.name + " (" + std::to_string(tag.station_count) + ")");
      genre_tags->push_back(tag.name);
    }
  });

  auto* manual_expander = Gtk::make_managed<Gtk::Expander>(_("Enter Manually (Name and Stream URL)"));
  auto* manual_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  manual_box->set_margin_top(6);

  auto* title_label = Gtk::make_managed<Gtk::Label>(_("Name"));
  title_label->set_halign(Gtk::Align::START);
  manual_box->append(*title_label);
  auto* title_entry = Gtk::make_managed<Gtk::Entry>();
  manual_box->append(*title_entry);

  auto* url_label = Gtk::make_managed<Gtk::Label>(_("Stream URL"));
  url_label->set_halign(Gtk::Align::START);
  manual_box->append(*url_label);
  auto* url_entry = Gtk::make_managed<Gtk::Entry>();
  url_entry->set_placeholder_text("http://...");
  manual_box->append(*url_entry);

  auto* manual_add_button = Gtk::make_managed<Gtk::Button>(_("Add"));
  manual_add_button->set_halign(Gtk::Align::END);
  manual_add_button->signal_clicked().connect([this, dialog, title_entry, url_entry] {
    std::string title = title_entry->get_text();
    std::string url = url_entry->get_text();
    if (title.empty() || url.empty())
      return;
    backend_->AddRadioStation(title, url);
    backend_->BrowseLibraryAsync(library_stack_.back().first);
    dialog->close();
  });
  manual_box->append(*manual_add_button);

  manual_expander->set_child(*manual_box);
  content->append(*manual_expander);

  auto* close_button = Gtk::make_managed<Gtk::Button>(_("Close"));
  close_button->set_halign(Gtk::Align::END);
  close_button->set_margin_top(6);
  close_button->signal_clicked().connect([dialog] { dialog->close(); });
  content->append(*close_button);

  dialog->set_child(*content);
  dialog->present();
  // SearchStations() with every filter empty is itself "globally popular
  // stations" (see its own comment on the always-applied clickcount
  // ordering) — running it immediately means opening this dialog already
  // shows something browsable, rather than an empty list until the user
  // types a name or picks a filter first.
  run_search();
}

void GnomosWindow::ShowRadioMprisSettingsDialog(unsigned index)
{
  if (index >= current_library_entries_.size())
    return;
  const LibraryEntry& entry = current_library_entries_[index];
  // Shouldn't happen while browsing "R:0/0" (BrowseLibraryAsync() always
  // populates stream_uri there) — guards against an out-of-sync index
  // rather than assuming the caller got it right.
  if (entry.stream_uri.empty())
    return;

  std::string stream_uri = entry.stream_uri;
  RadioMprisSettings settings = backend_->GetRadioMprisSettings(stream_uri);

  auto* dialog = new DialogShell(*this);
  dialog->set_title(entry.title.empty() ? std::string(_("Notifications")) : Format(_("Notifications: %s"), entry.title.c_str()));
  dialog->set_default_size(420, -1);


  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* enabled_check = Gtk::make_managed<Gtk::CheckButton>(_("Report track changes for this station"));
  enabled_check->set_active(settings.mpris_enabled);
  content->append(*enabled_check);

  auto* regex_label = Gtk::make_managed<Gtk::Label>(_("Regex filter (optional)"));
  regex_label->set_halign(Gtk::Align::START);
  regex_label->set_margin_top(6);
  content->append(*regex_label);

  auto* regex_entry = Gtk::make_managed<Gtk::Entry>();
  regex_entry->set_text(settings.regex);
  regex_entry->set_placeholder_text("z. B. .+ / .+");
  regex_entry->set_activates_default(true);
  content->append(*regex_entry);

  // Explains both halves of RadioContentFilter's own filtering in one
  // line: the regex decides what counts as a real song (ad text that
  // doesn't match is ignored), and only a genuinely new match is reported
  // — a repeat of the same song, or an ad in between, doesn't retrigger
  // MPRIS clients like GNOME Shell's media notification, and doesn't add
  // a spurious entry to "Verlauf" either.
  auto* help_label = Gtk::make_managed<Gtk::Label>(
      _("Only content matching this pattern is passed on to MPRIS and the history — ads and station idents in between are ignored. Empty = everything is passed on."));
  help_label->set_halign(Gtk::Align::START);
  help_label->set_wrap(true);
  help_label->add_css_class("caption");
  help_label->add_css_class("dimmed");
  content->append(*help_label);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* save_button = Gtk::make_managed<Gtk::Button>(_("Save"));
  save_button->add_css_class("suggested-action");
  auto do_save = [this, dialog, enabled_check, regex_entry, stream_uri] {
    RadioMprisSettings new_settings;
    new_settings.mpris_enabled = enabled_check->get_active();
    new_settings.regex = regex_entry->get_text().raw();
    backend_->SetRadioMprisSettings(stream_uri, new_settings);
    dialog->close();
  };
  save_button->signal_clicked().connect(do_save);
  regex_entry->signal_activate().connect(do_save);
  button_box->append(*cancel_button);
  button_box->append(*save_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*save_button);
  dialog->present();
}

void GnomosWindow::ShowLibrarySearchDialog(const std::string& prefill, const std::string& search_scope_object_id)
{
  std::vector<std::string> categories = backend_->GetActiveServiceSearchCategories();
  // Empty categories means we're not inside a linked service (SMAPI) — fall
  // back to a client-side substring search within the current local-library
  // level instead of refusing outright.
  bool local_search = categories.empty();
  // A caller prefilling a specific artist/album name (Titel-Details' own
  // search buttons, a related artist, a history entry) means "find this in
  // the library" — not "find this within whatever level I last happened to
  // be browsing", which is what library_stack_.back().first would give,
  // and is wrong more often than not (confirmed live: searching a related
  // artist only ever found anything if the user had separately already
  // browsed into Interpreten beforehand). Only the library view's own
  // "search this level" button — no override, prefill empty — genuinely
  // means the current level.
  std::string local_object_id = search_scope_object_id.empty() ? library_stack_.back().first : search_scope_object_id;

  auto* dialog = new DialogShell(*this);
  dialog->set_title(_("Search"));
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  Gtk::DropDown* category_dropdown = nullptr;
  if (!local_search && categories.size() > 1)
  {
    auto* category_label = Gtk::make_managed<Gtk::Label>(_("Category"));
    category_label->set_halign(Gtk::Align::START);
    content->append(*category_label);

    std::vector<Glib::ustring> category_names;
    category_names.reserve(categories.size());
    for (const std::string& category : categories)
      category_names.push_back(category);
    auto category_model = Gtk::StringList::create(category_names);
    category_dropdown = Gtk::make_managed<Gtk::DropDown>(category_model);
    content->append(*category_dropdown);
  }

  auto* term_label = Gtk::make_managed<Gtk::Label>(_("Search term"));
  term_label->set_halign(Gtk::Align::START);
  content->append(*term_label);

  auto* entry = Gtk::make_managed<Gtk::Entry>();
  entry->set_activates_default(true);
  if (!prefill.empty())
    entry->set_text(prefill);
  content->append(*entry);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* search_button = Gtk::make_managed<Gtk::Button>(_("Search"));
  search_button->add_css_class("suggested-action");
  auto do_search = [this, dialog, entry, category_dropdown, categories, local_search, local_object_id] {
    Glib::ustring term = entry->get_text();
    if (!term.empty())
    {
      if (local_search)
      {
        backend_->SearchLocalLibraryAsync(local_object_id, term.raw());
      }
      else
      {
        size_t index = category_dropdown ? category_dropdown->get_selected() : 0;
        if (index < categories.size())
          backend_->SearchActiveServiceAsync(categories[index], term.raw());
      }
    }
    dialog->close();
  };
  search_button->signal_clicked().connect(do_search);
  entry->signal_activate().connect(do_search);
  button_box->append(*cancel_button);
  button_box->append(*search_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*search_button);
  dialog->present();
  entry->grab_focus();
}

void GnomosWindow::OnServiceLinkReady(std::string url, std::string code)
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title(Format(_("Link %s"), pending_link_service_name_.c_str()));
  dialog->set_default_size(420, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* instructions = Gtk::make_managed<Gtk::Label>(
      _("Open the following link in a browser and finish linking there. Then come back here and click “Done”."));
  instructions->set_wrap(true);
  instructions->set_halign(Gtk::Align::START);
  content->append(*instructions);

  auto* link_button = Gtk::make_managed<Gtk::LinkButton>(url, url);
  link_button->set_halign(Gtk::Align::START);
  content->append(*link_button);

  if (!code.empty())
  {
    auto* code_label = Gtk::make_managed<Gtk::Label>(Format(_("Code: %s"), code.c_str()));
    code_label->set_halign(Gtk::Align::START);
    code_label->add_css_class("heading");
    content->append(*code_label);
  }

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* done_button = Gtk::make_managed<Gtk::Button>(_("Done"));
  done_button->add_css_class("suggested-action");
  done_button->signal_clicked().connect([this, dialog] {
    if (!pending_link_service_id_.empty())
      library_stack_.push_back({std::string(kServiceRootPrefix) + pending_link_service_id_, pending_link_service_name_});
    backend_->CompleteServiceLink();
    dialog->close();
  });
  button_box->append(*cancel_button);
  button_box->append(*done_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->present();
}

}  // namespace gnomos
