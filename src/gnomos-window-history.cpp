// SPDX-License-Identifier: GPL-3.0-or-later
//
// GnomosWindow, continued: Play history, scenes, scrobbling and track-change notifications.
// The class itself and its constructor live in gnomos-window.cpp.

#include "gnomos-window.h"

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

void GnomosWindow::RecordHistoryIfTrackChanged(const NowPlaying& now_playing)
{
  if (!now_playing.valid || now_playing.title.empty())
    return;

  // Radio: treat History (and, downstream, the desktop notification) the
  // same way MprisService treats MPRIS Metadata — ad breaks/idents
  // interspersed between song repeats shouldn't spam either one. See
  // RadioContentFilter's own comment; only radio-like sources
  // (duration == 0) with a known stream are affected, a queued track's
  // artist is stable and passes through unchanged.
  NowPlaying filtered = now_playing;
  if (now_playing.duration == 0 && !now_playing.stream_uri.empty())
  {
    filtered.artist = radio_history_filter_->Filter(now_playing.stream_uri, now_playing.artist);
    if (filtered.artist.empty() && !now_playing.artist.empty())
      return;  // filler, a repeat, or this station opted out — not a real change
  }

  std::string key = filtered.title + "\x1f" + filtered.artist;
  if (key == last_history_key_)
    return;  // same track as last time — OnNowPlayingChanged() re-fired without a real change
  last_history_key_ = key;

  HistoryEntry entry;
  entry.title = filtered.title;
  entry.artist = filtered.artist;
  entry.album = filtered.album;
  entry.art_uri = filtered.art_uri;
  history_.insert(history_.begin(), entry);
  if (history_.size() > kMaxHistoryEntries)
    history_.resize(kMaxHistoryEntries);

  history_view_.SetItems(history_);
  SaveHistory();
  SendTrackChangeNotification(filtered);
}

void GnomosWindow::MaybeScheduleScrobble(const NowPlaying& np)
{
  bool listenbrainz_enabled = !listenbrainz_token_.empty();
  bool lastfm_enabled = !lastfm_session_key_.empty();
  if (!listenbrainz_enabled && !lastfm_enabled)
    return;
  // Radio/live streams (duration == 0) have no fixed length to gauge
  // "listened long enough" against, and scrobbling an endless stream makes
  // little sense anyway — same convention other scrobbling clients follow.
  if (!np.valid || np.title.empty() || np.artist.empty() || np.duration == 0)
  {
    scrobble_timer_connection_.disconnect();
    last_scrobble_scheduled_key_.clear();
    return;
  }

  std::string key = np.title + "\x1f" + np.artist;
  if (key == last_scrobble_scheduled_key_)
    return;  // already scheduled (or already sent) for this exact track
  last_scrobble_scheduled_key_ = key;
  scrobble_timer_connection_.disconnect();

  // ListenBrainz's own submission guideline (Last.fm's is the same in
  // spirit): a track counts as "listened" once played for half its length
  // or 4 minutes, whichever is lower. Measured from wall-clock time since
  // this track was first detected, not actual accumulated playback time —
  // a simplification: pausing for a long stretch mid-track can make the
  // real scrobble land later than that guideline's own intent (or,
  // rarely, never, if the track changes again before this fires) rather
  // than tracking true accumulated play time across pause/resume, which
  // would need meaningfully more state for a background convenience
  // feature.
  unsigned threshold_seconds = std::min(np.duration / 2, 240u);
  if (threshold_seconds == 0)
    return;  // pathologically short track — nothing meaningful to wait for

  std::string artist = np.artist;
  std::string title = np.title;
  std::string album = np.album;
  scrobble_timer_connection_ = Glib::signal_timeout().connect_seconds(
      [this, key, artist, title, album, listenbrainz_enabled, lastfm_enabled] {
        // Re-check: only scrobble if this exact track is still the one
        // actually playing right now, not paused and not superseded by a
        // skip that happened to leave the same dedup key stale.
        NowPlaying current = backend_->GetNowPlaying();
        if (current.valid && current.state == TransportState::Playing &&
            (current.title + "\x1f" + current.artist) == key)
        {
          auto now = std::chrono::system_clock::now();
          if (listenbrainz_enabled)
            ListenBrainzScrobbler::Instance().Scrobble(listenbrainz_token_, artist, title, album, now);
          if (lastfm_enabled)
            LastFmScrobbler::Instance().Scrobble(lastfm_api_key_, lastfm_shared_secret_, lastfm_session_key_, artist,
                                                  title, album, now);
        }
        return false;  // one-shot
      },
      threshold_seconds);
}

void GnomosWindow::LoadScenes()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(ScenesFilePath()))
      return;
    for (const Glib::ustring& group : keyfile->get_groups())
    {
      RoomScene scene;
      scene.name = group.raw();
      std::vector<Glib::ustring> room_uuids = keyfile->get_string_list(group, "room_uuids");
      std::vector<Glib::ustring> coordinator_uuids = keyfile->get_string_list(group, "coordinator_uuids");
      std::vector<bool> has_volumes = keyfile->get_boolean_list(group, "has_volumes");
      std::vector<int> volumes = keyfile->get_integer_list(group, "volumes");
      // Parallel arrays, all written together by SaveScenes() — a size
      // mismatch means a corrupt/foreign file, safer to skip this one
      // scene than guess.
      if (room_uuids.size() != coordinator_uuids.size() || room_uuids.size() != has_volumes.size() ||
          room_uuids.size() != volumes.size())
        continue;
      for (size_t i = 0; i < room_uuids.size(); ++i)
      {
        RoomSceneEntry entry;
        entry.room_uuid = room_uuids[i].raw();
        entry.coordinator_uuid = coordinator_uuids[i].raw();
        entry.has_volume = has_volumes[i];
        entry.volume = static_cast<uint8_t>(std::clamp(volumes[i], 0, 100));
        scene.rooms.push_back(std::move(entry));
      }
      scenes_.push_back(std::move(scene));
    }
  }
  catch (const Glib::Error&)
  {
    scenes_.clear();
  }
}

void GnomosWindow::SaveScenes() const
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  // Rewritten in full rather than incrementally patched — same reasoning
  // as SaveHistory(): scenes_ already holds the complete, authoritative
  // list in memory, and a handful of small groups is cheap to serialize
  // from scratch every time.
  auto keyfile = Glib::KeyFile::create();
  for (const RoomScene& scene : scenes_)
  {
    std::vector<Glib::ustring> room_uuids, coordinator_uuids;
    std::vector<bool> has_volumes;
    std::vector<int> volumes;
    for (const RoomSceneEntry& entry : scene.rooms)
    {
      room_uuids.push_back(entry.room_uuid);
      coordinator_uuids.push_back(entry.coordinator_uuid);
      has_volumes.push_back(entry.has_volume);
      volumes.push_back(entry.volume);
    }
    keyfile->set_string_list(scene.name, "room_uuids", room_uuids);
    keyfile->set_string_list(scene.name, "coordinator_uuids", coordinator_uuids);
    keyfile->set_boolean_list(scene.name, "has_volumes", has_volumes);
    keyfile->set_integer_list(scene.name, "volumes", volumes);
  }
  try
  {
    keyfile->save_to_file(ScenesFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means this change won't be remembered next launch
  }
}

void GnomosWindow::CaptureCurrentAsScene(const std::string& name)
{
  if (name.empty())
    return;

  RoomScene scene;
  scene.name = name;
  for (const RoomInfo& room : backend_->Rooms())
  {
    RoomSceneEntry entry;
    entry.room_uuid = room.player_uuid;
    entry.coordinator_uuid = room.coordinator_uuid;
    entry.has_volume = backend_->GetRoomVolume(room.player_uuid, entry.volume);
    scene.rooms.push_back(std::move(entry));
  }

  // Same name overwrites in place (matches how a saved playlist or a
  // radio station's own settings already behave when re-saved) rather
  // than accumulating duplicates.
  auto it = std::find_if(scenes_.begin(), scenes_.end(), [&name](const RoomScene& s) { return s.name == name; });
  if (it != scenes_.end())
    *it = std::move(scene);
  else
    scenes_.push_back(std::move(scene));
  SaveScenes();
  // Best-effort, opportunistic refresh for next time — see
  // RefreshGroupVolumesAsync()'s own comment for why GetRoomVolume() above
  // can come back empty for a room that joined its group only recently;
  // this doesn't block the capture itself on it landing.
  backend_->RefreshGroupVolumesAsync();
}

void GnomosWindow::ApplyScene(const std::string& name)
{
  auto it = std::find_if(scenes_.begin(), scenes_.end(), [&name](const RoomScene& s) { return s.name == name; });
  if (it == scenes_.end())
    return;

  // Grouping first, then volumes — both go through the same serial
  // backend task queue either way, so this ordering is really just for
  // readability; the actual sequencing guarantee comes from that queue,
  // not from waiting here.
  for (const RoomSceneEntry& entry : it->rooms)
  {
    if (entry.coordinator_uuid == entry.room_uuid)
      backend_->RemoveRoomFromGroup(entry.room_uuid);
    else
      backend_->JoinRoomToZone(entry.room_uuid, entry.coordinator_uuid);
  }
  for (const RoomSceneEntry& entry : it->rooms)
    if (entry.has_volume)
      backend_->SetRoomVolume(entry.room_uuid, entry.volume);
}

void GnomosWindow::DeleteScene(const std::string& name)
{
  scenes_.erase(std::remove_if(scenes_.begin(), scenes_.end(),
                                [&name](const RoomScene& s) { return s.name == name; }),
                scenes_.end());
  SaveScenes();
}

void GnomosWindow::LoadHistory()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(HistoryFilePath()))
      return;
    std::vector<Glib::ustring> titles = keyfile->get_string_list("history", "titles");
    std::vector<Glib::ustring> artists = keyfile->get_string_list("history", "artists");
    std::vector<Glib::ustring> albums = keyfile->get_string_list("history", "albums");
    std::vector<Glib::ustring> art_uris = keyfile->get_string_list("history", "art_uris");
    // Parallel arrays, all written together by SaveHistory() — a size
    // mismatch means a corrupt/foreign file, safer to ignore than guess.
    if (titles.size() != artists.size() || titles.size() != albums.size() || titles.size() != art_uris.size())
      return;
    for (size_t i = 0; i < titles.size(); ++i)
    {
      HistoryEntry entry;
      entry.title = titles[i].raw();
      entry.artist = artists[i].raw();
      entry.album = albums[i].raw();
      entry.art_uri = art_uris[i].raw();
      history_.push_back(entry);
    }
    if (!history_.empty())
      last_history_key_ = history_.front().title + "\x1f" + history_.front().artist;
  }
  catch (const Glib::Error&)
  {
    history_.clear();
  }
}

void GnomosWindow::SaveHistory() const
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  std::vector<Glib::ustring> titles, artists, albums, art_uris;
  for (const HistoryEntry& entry : history_)
  {
    titles.push_back(entry.title);
    artists.push_back(entry.artist);
    albums.push_back(entry.album);
    art_uris.push_back(entry.art_uri);
  }

  auto keyfile = Glib::KeyFile::create();
  keyfile->set_string_list("history", "titles", titles);
  keyfile->set_string_list("history", "artists", artists);
  keyfile->set_string_list("history", "albums", albums);
  keyfile->set_string_list("history", "art_uris", art_uris);
  try
  {
    keyfile->save_to_file(HistoryFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the history won't be remembered next launch
  }
}

void GnomosWindow::SendTrackChangeNotification(const NowPlaying& now_playing)
{
  if (!notify_on_track_change_)
    return;

  auto notification = Gio::Notification::create(now_playing.title.empty() ? "Unbekannter Titel" : now_playing.title);
  std::string body = now_playing.artist;
  if (!now_playing.album.empty())
    body += (body.empty() ? "" : " — ") + now_playing.album;
  if (!body.empty())
    notification->set_body(body);

  // Only ever the already-cached bytes (memory or disk) — never a fresh
  // network fetch here, so a notification is never held up waiting on one.
  // Two earlier attempts both failed to actually show a cover: a
  // Gdk::Texture-backed GIcon isn't one of the types g_notification_set_icon()
  // can serialize over D-Bus at all (confirmed live: silently dropped).
  // GBytesIcon (tried next) DOES serialize — confirmed live by inspecting
  // the raw org.gtk.Notifications AddNotification call, which carried
  // correct JPEG bytes — but still never rendered, meaning this Shell's
  // own icon deserialization doesn't handle the "bytes" variant. A plain
  // file path is the one icon mechanism every notification daemon
  // (including this one) has always had to support, so the bytes get
  // written to a cache file and referenced by path instead.
  //
  // That file used to be a single fixed name, reused (overwritten) for
  // every track — confirmed live that this made the cover show up
  // unreliably: this "now-playing" notification is updated in place on
  // every track change rather than re-created, and Shell doesn't always
  // reload an already-displayed notification's icon texture from disk
  // just because the file's *content* changed while its *path* didn't.
  // Hashing art_uri into the filename gives every distinct cover its own
  // path, so an update always points the notification at a path Shell
  // hasn't already loaded — the previous track's file is removed right
  // after, so this still never accumulates more than one file on disk.
  // Whether GetRawBytes() below actually found something — art_uri not
  // being cached *yet* (a first-time fetch still in flight; see
  // OnNotificationArtReady()) is the common case, not a failure, so it's
  // tracked separately from "no art at all" rather than treated the same.
  bool icon_set = false;
  if (!now_playing.art_uri.empty())
  {
    if (auto raw_bytes = ArtCache::Instance().GetRawBytes(now_playing.art_uri))
    {
      std::string icon_dir = Glib::build_filename(Glib::get_user_cache_dir(), "gnomos");
      g_mkdir_with_parents(icon_dir.c_str(), 0700);
      std::string icon_path = Glib::build_filename(
          icon_dir, "notification-icon-" +
                        Glib::Checksum::compute_checksum(Glib::Checksum::Type::SHA256, now_playing.art_uri));
      gsize icon_size = 0;
      auto icon_data = static_cast<const char*>(raw_bytes->get_data(icon_size));
      if (icon_path == last_notification_icon_path_ ||
          g_file_set_contents(icon_path.c_str(), icon_data, static_cast<gssize>(icon_size), nullptr))
      {
        auto icon_file = Gio::File::create_for_path(icon_path);
        g_notification_set_icon(notification->gobj(), G_ICON(g_file_icon_new(icon_file->gobj())));
        if (!last_notification_icon_path_.empty() && last_notification_icon_path_ != icon_path)
          g_remove(last_notification_icon_path_.c_str());
        last_notification_icon_path_ = icon_path;
        icon_set = true;
      }
    }
  }
  // PlayerBar::LoadArt() triggers the same fetch (called from Update(),
  // just before this function's own caller runs) but it's async — for a
  // cover never seen before, GetRawBytes() above runs well before that
  // fetch lands, so the notification would otherwise permanently go out
  // iconless for exactly the tracks that most need it (previously unseen
  // ones). Remembering this one and reacting to signal_art_ready() lets
  // OnNotificationArtReady() retry once the same fetch actually finishes.
  notification_icon_pending_ = !icon_set && !now_playing.art_uri.empty();
  if (notification_icon_pending_)
    pending_notification_track_ = now_playing;

  // Clicking the notification body itself raises the window; the buttons
  // are app-scoped forwards to the window's own real actions — see
  // GnomosApplication::on_startup()'s own comment for why they have to be.
  notification->set_default_action("app.notification-raise");
  notification->add_button(now_playing.state == TransportState::Playing ? "Pause" : "Wiedergabe",
                            "app.notification-play-pause");
  if (now_playing.can_go_next)
    notification->add_button("Weiter", "app.notification-next");

  if (auto app = get_application())
    app->send_notification("now-playing", notification);
}

void GnomosWindow::OnNotificationArtReady(const std::string& uri)
{
  if (notification_icon_pending_ && uri == pending_notification_track_.art_uri)
    SendTrackChangeNotification(pending_notification_track_);
}

void GnomosWindow::ShowScenesDialog()
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title("Szenen");
  dialog->set_default_size(380, 480);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* disclosure_label = Gtk::make_managed<Gtk::Label>(
      "Speichert, welche Räume gerade miteinander gruppiert sind (und deren Lautstärke) als benannte Szene, "
      "mit einem Klick wiederherstellbar.");
  disclosure_label->set_halign(Gtk::Align::START);
  disclosure_label->set_wrap(true);
  disclosure_label->add_css_class("caption");
  disclosure_label->add_css_class("dimmed");
  content->append(*disclosure_label);

  auto* save_button = Gtk::make_managed<Gtk::Button>("Aktuelle Gruppierung speichern…");
  save_button->signal_clicked().connect([this, dialog] {
    dialog->close();
    ShowSaveSceneDialog();
  });
  content->append(*save_button);

  auto* list_box = Gtk::make_managed<Gtk::ListBox>();
  list_box->set_selection_mode(Gtk::SelectionMode::NONE);
  list_box->add_css_class("boxed-list");
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_child(*list_box);
  scroller->set_vexpand(true);
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_margin_top(6);
  content->append(*scroller);

  if (scenes_.empty())
  {
    auto* placeholder = Gtk::make_managed<Gtk::Label>("Noch keine Szenen gespeichert.");
    placeholder->add_css_class("dimmed");
    placeholder->set_margin_top(12);
    placeholder->set_margin_bottom(12);
    list_box->append(*placeholder);
  }
  else
  {
    for (const RoomScene& scene : scenes_)
    {
      auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      row_box->set_margin_top(6);
      row_box->set_margin_bottom(6);
      row_box->set_margin_start(6);
      row_box->set_margin_end(6);

      auto* name_label = Gtk::make_managed<Gtk::Label>(scene.name);
      name_label->set_halign(Gtk::Align::START);
      name_label->set_hexpand(true);
      name_label->set_ellipsize(Pango::EllipsizeMode::END);
      row_box->append(*name_label);

      std::string name = scene.name;

      auto* apply_button = Gtk::make_managed<Gtk::Button>();
      apply_button->set_icon_name("media-playback-start-symbolic");
      apply_button->add_css_class("flat");
      apply_button->set_valign(Gtk::Align::CENTER);
      apply_button->set_tooltip_text("Anwenden");
      apply_button->signal_clicked().connect([this, dialog, name] {
        ApplyScene(name);
        ShowToast("Szene „" + name + "“ angewendet");
        dialog->close();
      });
      row_box->append(*apply_button);

      auto* delete_button = Gtk::make_managed<Gtk::Button>();
      delete_button->set_icon_name("user-trash-symbolic");
      delete_button->add_css_class("flat");
      delete_button->set_valign(Gtk::Align::CENTER);
      delete_button->set_tooltip_text("Löschen");
      delete_button->signal_clicked().connect([this, dialog, name] {
        DeleteScene(name);
        dialog->close();
        ShowScenesDialog();
      });
      row_box->append(*delete_button);

      list_box->append(*row_box);
    }
  }

  auto* close_button = Gtk::make_managed<Gtk::Button>("Schließen");
  close_button->set_halign(Gtk::Align::END);
  close_button->set_margin_top(6);
  close_button->signal_clicked().connect([dialog] { dialog->close(); });
  content->append(*close_button);

  dialog->set_child(*content);
  dialog->present();
}

void GnomosWindow::ShowSaveSceneDialog()
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title("Szene speichern");
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* label = Gtk::make_managed<Gtk::Label>("Name der Szene");
  label->set_halign(Gtk::Align::START);
  content->append(*label);

  auto* entry = Gtk::make_managed<Gtk::Entry>();
  entry->set_activates_default(true);
  content->append(*entry);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([this, dialog] {
    dialog->close();
    ShowScenesDialog();
  });
  auto* save_button = Gtk::make_managed<Gtk::Button>("Speichern");
  save_button->add_css_class("suggested-action");
  auto do_save = [this, dialog, entry] {
    Glib::ustring name = entry->get_text();
    if (!name.empty())
    {
      CaptureCurrentAsScene(name.raw());
      ShowToast("Szene „" + name.raw() + "“ gespeichert");
    }
    dialog->close();
    ShowScenesDialog();
  };
  save_button->signal_clicked().connect(do_save);
  entry->signal_activate().connect(do_save);
  button_box->append(*cancel_button);
  button_box->append(*save_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*save_button);
  dialog->present();
  entry->grab_focus();
}

}  // namespace gnomos
