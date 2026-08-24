// SPDX-License-Identifier: GPL-3.0-or-later

#include "noson-backend.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include <giomm/file.h>
#include <glib.h>
#include <glibmm/checksum.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>

#include <alarm.h>
#include <contentdirectory.h>
#include <didlparser.h>
#include <digitalitem.h>
#include <musicservices.h>
#include <smaccount.h>
#include <sonostypes.h>

namespace gnomos
{

namespace
{

TransportState ParseTransportState(const std::string& s)
{
  if (s == "PLAYING")
    return TransportState::Playing;
  if (s == "PAUSED_PLAYBACK")
    return TransportState::Paused;
  if (s == "STOPPED")
    return TransportState::Stopped;
  if (s == "TRANSITIONING")
    return TransportState::Transitioning;
  if (s == "NO_MEDIA_PRESENT")
    return TransportState::NoMedia;
  return TransportState::Unknown;
}

// Items recovered from a favorite already carry a <desc> service token
// (System::AddURIToFavorites always attaches one when the favorite is
// created). Items straight from a generic ContentDirectory::Browse (e.g.
// the local radio directory "R:0/0") don't — confirmed live: Sonos replies
// to SetAVTransportURI with an HTTP 500 for such an item, and starts
// playing fine once this fallback <desc> is attached. Mirrors the same
// fallback System::AddURIToFavorites itself uses when a favorite has none.
void EnsureServiceDesc(NSROOT::System& system, const NSROOT::DigitalItemPtr& item)
{
  if (!item || !item->GetValue("desc").empty())
    return;
  NSROOT::ElementPtr desc(new NSROOT::Element("desc"));
  NSROOT::SMServicePtr svc = system.GetServiceForMedia(item->GetValue("res"));
  desc->assign(svc ? svc->GetServiceDesc() : NSROOT::ServiceDescTable[NSROOT::ServiceDesc_default]);
  desc->SetAttribut("id", "cdudn");
  desc->SetAttribut("nameSpace", DIDL_XMLNS_RINC);
  item->SetProperty(desc);
}

// ContentBrowser's constructor only fetches a single page (its own
// BROWSE_COUNT default is 100; call sites here pass a larger but still
// fixed count) — a queue, playlist, or library folder bigger than that
// silently lost its tail end without this. Browse(0, total()) asks the
// browser to grow its window to the *full* reported total in one more
// SOAP call; looped defensively in case the device caps a single response
// below what was asked for. Capped at kMaxBrowseItems so a pathological
// folder (tens of thousands of tracks) can't make a refresh hang or eat
// unbounded memory — matches this app's general "the local library isn't
// a substitute for a real client's crate-digging tools" scope.
constexpr unsigned kMaxBrowseItems = 5000;
void ExhaustBrowser(NSROOT::ContentBrowser& browser)
{
  unsigned target = std::min(browser.total(), kMaxBrowseItems);
  while (browser.count() < target)
  {
    unsigned before = browser.count();
    if (!browser.Browse(0, target))
      break;
    if (browser.count() <= before)
      break;  // no progress — avoid spinning on an unexpected response
  }
}

// Maps a DigitalItem's own subType() — parsed generically from its
// upnp:class DIDL property (see DigitalItem's constructor), so this works
// identically for local library items and every SMAPI service alike,
// without Gnomos needing to guess anything from a title string — to a
// GNOME/Adwaita symbolic icon name. Only icon-worthy container types get a
// specific one; everything else (tracks, unknown types) returns empty,
// which just means "CoverThumbnail's own generic default applies" — same
// as before this existed.
std::string IconNameForSubType(NSROOT::DigitalItem::SubType_t subtype)
{
  switch (subtype)
  {
    case NSROOT::DigitalItem::SubType_person:
      return "avatar-default-symbolic";
    case NSROOT::DigitalItem::SubType_album:
      return "media-optical-cd-symbolic";
    case NSROOT::DigitalItem::SubType_genre:
      return "folder-music-symbolic";
    case NSROOT::DigitalItem::SubType_playlistContainer:
      return "media-playlist-consecutive-symbolic";
    case NSROOT::DigitalItem::SubType_storageFolder:
    case NSROOT::DigitalItem::SubType_storageSystem:
    case NSROOT::DigitalItem::SubType_storageVolume:
    case NSROOT::DigitalItem::SubType_bookmarkFolder:
      return "folder-music-symbolic";
    default:
      return "";
  }
}

std::string RadioFaviconsPath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "radio-favicons.ini");
}

// System::CreateRadio() (sonossystem.cpp) does NOT store streamURL
// verbatim — it strips the "http(s)" scheme and prepends Sonos's own
// "x-rincon-mp3radio:" protocol instead, keeping only the "://host/path"
// suffix (streamURL.substr(streamURL.find("://"))). A browsed entry's own
// res value (item->GetValue("res")) therefore never matches the original
// stream_url passed to AddRadioStation() — only that common suffix does,
// which is what both SaveRadioFavicon() and the "R:0/0" browse loop below
// hash instead of the full URL, so the two sides actually agree on a key.
// Confirmed by reading CreateRadio()'s own implementation directly, not
// assumed — an earlier version of this hashed the full original URL,
// which would have silently never matched anything.
std::string RadioStreamMatchKey(const std::string& url)
{
  size_t p = url.find("://");
  return Glib::Checksum::compute_checksum(Glib::Checksum::Type::SHA256, p != std::string::npos ? url.substr(p) : url);
}

}  // namespace

NosonBackend::NosonBackend()
// TaskQueue's own busy callback fires on tasks_'s worker thread — see its
// constructor comment — so it only ever hands the new state off through
// pending_busy_state_ and wakes busy_dispatcher_, exactly like every
// other cross-thread notification in this class already does.
: tasks_([this](bool busy) {
    pending_busy_state_ = busy;
    busy_dispatcher_.emit();
  })
{
  busy_dispatcher_.connect([this] { signal_busy_changed_.emit(pending_busy_state_.load()); });
  system_dispatcher_.connect([this] { HandleSystemEvent(); });
  player_dispatcher_.connect([this] { HandlePlayerEvent(); });
  discovery_dispatcher_.connect([this] { HandleDiscoveryDone(); });
  player_ready_dispatcher_.connect([this] { HandlePlayerReady(); });
  error_dispatcher_.connect([this] { HandleError(); });
  queue_dispatcher_.connect([this] { signal_queue_changed_.emit(); });
  favorites_dispatcher_.connect([this] { signal_favorites_changed_.emit(); });
  alarms_dispatcher_.connect([this] { signal_alarms_changed_.emit(); });
  library_dispatcher_.connect([this] { signal_library_changed_.emit(); });
  saved_playlists_dispatcher_.connect([this] { signal_saved_playlists_changed_.emit(); });
  sleep_timer_dispatcher_.connect([this] { signal_sleep_timer_changed_.emit(); });
  sound_settings_dispatcher_.connect([this] { signal_sound_settings_changed_.emit(); });
  service_link_ready_dispatcher_.connect([this] {
    std::string url, code;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      url = pending_link_url_;
      code = pending_link_code_;
    }
    signal_service_link_ready_.emit(url, code);
  });
  position_dispatcher_.connect([this] { signal_position_changed_.emit(); });

  // Constructing System already starts its internal UPnP-eventing listener
  // thread (noson/src/sonossystem.cpp), independent of Discover().
  system_ = std::make_unique<NSROOT::System>(this, &NosonBackend::OnSystemEvent);

  // AddServiceOAuth() only registers an account for the current process —
  // reload whatever was linked in a previous run so GetEnabledServices()
  // (and hence the library root categories) already includes it.
  LoadLinkedServices();

  SubscribeToSleepSignal();
}

NosonBackend::~NosonBackend()
{
  // Explicitly unsubscribed here, before any member starts being
  // destroyed (including tasks_/system_ below) — OnPrepareForSleep()
  // calls DiscoverAsync() (pushes onto tasks_) and
  // system_->RenewSubscriptions(), so it must never fire once those are
  // mid-teardown or gone.
  if (system_bus_connection_ && sleep_signal_subscription_id_ != 0)
    system_bus_connection_->signal_unsubscribe(sleep_signal_subscription_id_);

  // Same reasoning: a pending debounce timer's slot captures [this] and
  // calls ApplyVolumeAsync()/ApplyRoomVolumeAsync() (which push onto
  // tasks_), so it must never fire once tasks_/system_ are mid-teardown.
  volume_debounce_connection_.disconnect();
  for (auto& [uuid, connection] : room_volume_debounce_connections_)
    connection.disconnect();
}

void NosonBackend::SubscribeToSleepSignal()
{
  try
  {
    system_bus_connection_ = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SYSTEM);
    sleep_signal_subscription_id_ = system_bus_connection_->signal_subscribe(
        sigc::mem_fun(*this, &NosonBackend::OnPrepareForSleep), "org.freedesktop.login1",
        "org.freedesktop.login1.Manager", "PrepareForSleep", "/org/freedesktop/login1");
  }
  catch (const Glib::Error&)
  {
    // No system bus, or no logind (e.g. a non-systemd system) — fine,
    // this is a best-effort resilience improvement, not a requirement.
    system_bus_connection_.reset();
  }
}

void NosonBackend::OnPrepareForSleep(const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&,
                                      const Glib::ustring&, const Glib::ustring&, const Glib::ustring&,
                                      const Glib::VariantContainerBase& parameters)
{
  // PrepareForSleep(b sleeping) fires twice per cycle: once with true right
  // before the system actually suspends, once with false right after it
  // resumes — only the latter is interesting here. UPnP eventing
  // subscriptions and the underlying TCP connections don't survive a
  // suspend, and while libnoson's own subscription threads do already
  // self-renew on their own timers (see subscription.cpp), that can leave
  // the app showing stale now-playing/volume/topology state for however
  // long is left on the previous renewal cycle. Forcing a renewal plus a
  // fresh discovery immediately on resume — the same DiscoverAsync() path
  // startup itself uses, including its existing "restore the previously
  // selected room" handling — clears that gap instead of waiting it out.
  if (parameters.get_n_children() < 1)
    return;
  Glib::Variant<bool> sleeping;
  parameters.get_child(sleeping, 0);
  if (sleeping.get())
    return;

  if (system_)
    system_->RenewSubscriptions();
  DiscoverAsync();
}

namespace
{
std::string LinkedServicesPath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "linked-services.ini");
}
}  // namespace

void NosonBackend::LoadLinkedServices()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(LinkedServicesPath()))
      return;
  }
  catch (const Glib::Error&)
  {
    return;  // no saved services yet (or the file is otherwise unreadable) — nothing to do
  }

  for (const Glib::ustring& group : keyfile->get_groups())
  {
    try
    {
      NSROOT::System::AddServiceOAuth(group.raw(), keyfile->get_string(group, "serial_num").raw(),
                                       keyfile->get_string(group, "key").raw(),
                                       keyfile->get_string(group, "token").raw(),
                                       keyfile->get_string(group, "username").raw());
    }
    catch (const Glib::Error&)
    {
      continue;
    }
  }
}

void NosonBackend::PersistAndRegisterServiceCredentials(const std::string& type, const std::string& serial_num,
                                                          const std::string& key, const std::string& token,
                                                          const std::string& username)
{
  // SMOAKeyring::Store() (which AddServiceOAuth() below calls into) only
  // dedupes on an exact (type, serialNum) match — confirmed in
  // smaccount.cpp. The AppLink flow mints a fresh serialNum on every
  // relink, so without purging whatever was there before, re-linking the
  // same service leaves stale duplicate accounts behind, each making
  // GetEnabledServices() list the service again (this is what produced
  // "2x Spotify, 2x bonob" after relinking). Gnomos only ever wants one
  // account per service, so this always fully replaces, not adds to, the
  // existing one.
  for (const NSROOT::SMAccountPtr& stale : NSROOT::SMAccount::CreateAccounts(type))
    if (stale)
      NSROOT::System::DeleteServiceOAuth(type, stale->GetSerialNum());

  NSROOT::System::AddServiceOAuth(type, serial_num, key, token, username);

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(LinkedServicesPath());
  }
  catch (const Glib::Error&)
  {
    // fine — this is the first service ever linked, nothing to preserve
  }
  keyfile->set_string(type, "serial_num", serial_num);
  keyfile->set_string(type, "key", key);
  keyfile->set_string(type, "token", token);
  keyfile->set_string(type, "username", username);
  try
  {
    keyfile->save_to_file(LinkedServicesPath());
  }
  catch (const Glib::Error&)
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pending_error_ = "Verknüpfung wurde aktiviert, konnte aber nicht dauerhaft gespeichert werden.";
    error_dispatcher_.emit();
  }
}

NSROOT::PlayerPtr NosonBackend::SnapshotPlayer() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return player_;
}

NSROOT::ZonePtr NosonBackend::SnapshotZone() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return current_zone_;
}

bool NosonBackend::HasPlayer() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return static_cast<bool>(player_);
}

void NosonBackend::DiscoverAsync()
{
  tasks_.Push([this] {
    bool ok = system_->Discover();
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      last_discovery_ok_ = ok;
      if (ok)
      {
        zones_by_uuid_.clear();
        for (const auto& kv : system_->GetZoneList())
          zones_by_uuid_[kv.first] = kv.second;
      }
    }
    // TEMPORARY (GNOMOS_DEBUG only): GetEnabledServices() silently drops any
    // service this app hasn't locally linked an account for (see
    // BrowseLibraryAsync()/System::GetEnabledServices() in sonossystem.cpp)
    // — this dumps the *full* catalog with each service's auth policy, to
    // find out what a given service (e.g. bonob) actually needs without
    // grepping megabytes of raw SOAP XML by hand.
    if (ok && std::getenv("GNOMOS_DEBUG"))
    {
      for (const NSROOT::SMServicePtr& svc : system_->GetAvailableServices())
      {
        if (!svc)
          continue;
        std::fprintf(stderr, "(gnomos) available service: name='%s' id='%s' type='%s' auth='%s'\n",
                      svc->GetName().c_str(), svc->GetId().c_str(), svc->GetServiceType().c_str(),
                      svc->GetPolicy() ? svc->GetPolicy()->GetAttribut("Auth").c_str() : "?");
      }
    }
    discovery_dispatcher_.emit();
  });
}

std::vector<ZoneInfo> NosonBackend::Zones() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::vector<ZoneInfo> result;
  result.reserve(zones_by_uuid_.size());
  for (const auto& kv : zones_by_uuid_)
  {
    const NSROOT::ZonePtr& zone = kv.second;
    ZoneInfo info;
    info.group_id = kv.first;
    info.display_name = zone->GetZoneShortName();
    NSROOT::ZonePlayerPtr coord = zone->GetCoordinator();
    if (coord)
    {
      info.icon = coord->GetIconName();
      info.coordinator_uuid = coord->GetUUID();
      auto it = model_number_by_uuid_.find(info.coordinator_uuid);
      info.is_gen1 = it != model_number_by_uuid_.end() && is_gen1_model(it->second);
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::vector<RoomInfo> NosonBackend::Rooms() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::vector<RoomInfo> result;
  for (const auto& kv : zones_by_uuid_)
  {
    const std::string& group_id = kv.first;
    const NSROOT::ZonePtr& zone = kv.second;
    NSROOT::ZonePlayerPtr coord = zone->GetCoordinator();
    if (!coord)
      continue;
    const std::string coordinator_uuid = coord->GetUUID();
    for (const NSROOT::ZonePlayerPtr& zp : *zone)
    {
      RoomInfo info;
      info.player_uuid = zp->GetUUID();
      info.name = *zp;  // ZonePlayer : Element : std::string — the object IS the room name
      info.group_id = group_id;
      info.coordinator_uuid = coordinator_uuid;
      auto it = model_number_by_uuid_.find(info.player_uuid);
      if (it != model_number_by_uuid_.end())
      {
        info.model_number = it->second;
        info.is_gen1 = is_gen1_model(it->second);
      }
      result.push_back(std::move(info));
    }
  }
  return result;
}

DeviceInfo NosonBackend::GetDeviceInfo(const std::string& player_uuid) const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  DeviceInfo info;
  for (const auto& kv : zones_by_uuid_)
  {
    for (const NSROOT::ZonePlayerPtr& zp : *kv.second)
    {
      if (zp->GetUUID() != player_uuid)
        continue;

      info.ip = zp->GetHost();
      info.software_version = zp->GetAttribut(ZP_VERSION);

      // Sonos embeds the MAC address directly in the player's own UUID:
      // "RINCON_<12 hex MAC digits><5-digit port suffix>" — confirmed live
      // against real hardware, avoids a separate query for something
      // that's already sitting in data already fetched at discovery time.
      static const std::string kUuidPrefix = "RINCON_";
      if (player_uuid.compare(0, kUuidPrefix.size(), kUuidPrefix) == 0 &&
          player_uuid.size() >= kUuidPrefix.size() + 12)
      {
        std::string raw_mac = player_uuid.substr(kUuidPrefix.size(), 12);
        for (size_t i = 0; i < raw_mac.size(); i += 2)
        {
          if (i > 0)
            info.mac += ':';
          info.mac += raw_mac.substr(i, 2);
        }
      }

      auto model_it = model_number_by_uuid_.find(player_uuid);
      if (model_it != model_number_by_uuid_.end())
      {
        info.model_number = model_it->second;
        info.is_gen1 = is_gen1_model(model_it->second);
      }
      return info;
    }
  }
  return info;
}

namespace
{

// Both JoinRoomToCurrentZone() and RemoveRoomFromGroup() need a handle on
// an arbitrary room's ZonePlayer, not just the currently selected one, so
// they build a lightweight, unsubscribed NSROOT::Player around it directly
// (see sonosplayer.h: the ZonePlayerPtr constructor is for exactly this —
// one-off actions with no cached state or event subscription needed).
NSROOT::ZonePlayerPtr FindZonePlayer(const std::map<std::string, NSROOT::ZonePtr>& zones_by_uuid,
                                      const std::string& player_uuid)
{
  for (const auto& kv : zones_by_uuid)
    for (const NSROOT::ZonePlayerPtr& zp : *kv.second)
      if (zp->GetUUID() == player_uuid)
        return zp;
  return NSROOT::ZonePlayerPtr();
}

}  // namespace

void NosonBackend::JoinRoomToCurrentZone(const std::string& room_player_uuid)
{
  tasks_.Push([this, room_player_uuid] {
    NSROOT::ZonePlayerPtr target;
    std::string coordinator_uuid;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!current_zone_)
        return;
      // JoinToGroup() builds an "x-rincon:<uuid>" URI and needs the
      // coordinator's own bare uuid — current_zone_->GetGroup() is the
      // compound Sonos group id (uuid + topology version), which produces
      // a malformed URI the target device silently rejects.
      NSROOT::ZonePlayerPtr coord = current_zone_->GetCoordinator();
      if (!coord)
        return;
      coordinator_uuid = coord->GetUUID();
      target = FindZonePlayer(zones_by_uuid_, room_player_uuid);
    }
    if (!target)
      return;

    NSROOT::Player roomPlayer(target);
    if (!roomPlayer.JoinToGroup(coordinator_uuid))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Raum konnte nicht zur Gruppe hinzugefügt werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::RemoveRoomFromGroup(const std::string& room_player_uuid)
{
  tasks_.Push([this, room_player_uuid] {
    NSROOT::ZonePlayerPtr target;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      target = FindZonePlayer(zones_by_uuid_, room_player_uuid);
    }
    if (!target)
      return;

    NSROOT::Player roomPlayer(target);
    if (!roomPlayer.BecomeStandalone())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Raum konnte nicht aus der Gruppe entfernt werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::SelectZone(const std::string& coordinator_uuid)
{
  tasks_.Push([this, coordinator_uuid] {
    NSROOT::ZonePtr zone;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto it = zones_by_uuid_.find(coordinator_uuid);
      if (it == zones_by_uuid_.end())
        return;
      zone = it->second;
    }

    NSROOT::PlayerPtr player = system_->GetPlayer(zone, this, &NosonBackend::OnPlayerEvent);
    if (!player || !player->IsValid())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Verbindung zum Sonos-Player fehlgeschlagen.";
      error_dispatcher_.emit();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      player_ = player;
      current_zone_ = zone;
      last_zone_select_at_ = std::chrono::steady_clock::now();
      RefreshNowPlayingLocked();
      RefreshVolumeLocked();
    }
    player_ready_dispatcher_.emit();
  });
}

NowPlaying NosonBackend::GetNowPlaying() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return now_playing_;
}

VolumeInfo NosonBackend::GetVolume() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return volume_;
}

std::vector<QueueItem> NosonBackend::GetQueue() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return queue_;
}

void NosonBackend::Play()
{
  tasks_.Push([this] {
    if (auto player = SnapshotPlayer())
      player->Play();
  });
}

void NosonBackend::PauseOrStop()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    // Not every source (line-in, some radio streams) supports Pause;
    // CurrentTransportActions lists what the active stream allows.
    NSROOT::AVTProperty prop = player->GetTransportProperty();
    if (prop.CurrentTransportActions.find("Pause") != std::string::npos)
      player->Pause();
    else
      player->Stop();
  });
}

void NosonBackend::Next()
{
  tasks_.Push([this] {
    if (auto player = SnapshotPlayer())
      player->Next();
  });
}

void NosonBackend::Previous()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;

    // Restart the current track instead of skipping to the actual
    // previous one once a few seconds into it — the same convention most
    // media players use (Spotify, YouTube Music, ...): right at the start
    // of a track, "Previous" means the previous track; partway through,
    // it more usefully means "start this one over". Mirrors SeekAsync()'s
    // own success handling below, since a restart really is just a seek
    // to 0.
    unsigned current_position;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      current_position = position_;
    }
    constexpr unsigned kRestartThresholdSeconds = 3;
    if (current_position > kRestartThresholdSeconds && player->SeekTime(0))
    {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        position_ = 0;
      }
      position_dispatcher_.emit();
      return;
    }

    player->Previous();
  });
}

void NosonBackend::SetVolume(uint8_t value)
{
  // Debounced: dragging the slider can fire this many times a second, and
  // each call is a full read-every-member-then-scale-every-member round
  // trip (see ApplyVolumeAsync() below) — without this, every intermediate
  // step queued its own trip, leaving the device visibly lagging behind
  // the slider for a while after the user had already stopped dragging.
  // A short constant delay (not tied to drag speed) is enough: it only
  // needs to collapse a burst of same-frame updates into one.
  volume_debounce_connection_.disconnect();
  volume_debounce_connection_ = Glib::signal_timeout().connect(
      [this, value] {
        ApplyVolumeAsync(value);
        return false;  // one-shot
      },
      150);
}

void NosonBackend::ApplyVolumeAsync(uint8_t value)
{
  tasks_.Push([this, value] {
    auto player = SnapshotPlayer();
    if (!player)
      return;

    // Scales every (non-fixed-output) member's volume by the same ratio
    // rather than flattening them all to the identical value, so a
    // multi-room group keeps its relative room-to-room balance as the
    // group slider moves — mirrors noson-app's own group-volume path
    // (Player::handleRenderingControlChange()/setVolume(), player.cpp),
    // simplified: that version also tracks a fractional "volumeFake" per
    // member across calls so ratio precision survives repeated scaling
    // through 0; this reads live device volumes fresh on every call
    // instead, which is simpler but loses a member's relative position
    // once it's been scaled down to 0 and back up — acceptable for how
    // rarely that exact sequence happens in practice. OutputFixed members
    // (line-out to a fixed-volume amp) are skipped entirely, same as
    // upstream — their own volume control is meaningless.
    NSROOT::SRPList props = player->GetRenderingProperty();
    unsigned sum = 0, count = 0;
    for (const NSROOT::SRProperty& srp : props)
    {
      if (srp.property.OutputFixed)
        continue;
      sum += static_cast<unsigned>(srp.property.VolumeMaster);
      ++count;
    }
    double ratio = (count > 0 && sum > 0) ? static_cast<double>(value) * count / sum : 1.0;

    for (const NSROOT::SRProperty& srp : props)
    {
      if (srp.property.OutputFixed)
        continue;
      uint8_t target = sum == 0 ? value
                                 : static_cast<uint8_t>(std::clamp(
                                       static_cast<int>(std::lround(srp.property.VolumeMaster * ratio)), 0, 100));
      player->SetVolume(srp.uuid, target);
    }
  });
}

void NosonBackend::SetMuted(bool muted)
{
  tasks_.Push([this, muted] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    for (const NSROOT::SRProperty& srp : player->GetRenderingProperty())
      if (!srp.property.OutputFixed)
        player->SetMute(srp.uuid, muted ? 1 : 0);
  });
}

void NosonBackend::ToggleRepeat()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    const std::string mode = player->GetTransportProperty().CurrentPlayMode;
    NSROOT::PlayMode_t target;
    // Off -> All -> One -> Off while not shuffling — REPEAT_ONE has no
    // Sonos-side combination with shuffle (see RepeatMode's own comment in
    // noson-types.h), so while shuffling this still only toggles the
    // SHUFFLE/SHUFFLE_NOREPEAT pair, same as before.
    if (mode == "NORMAL")
      target = NSROOT::PlayMode_REPEAT_ALL;
    else if (mode == "REPEAT_ALL")
      target = NSROOT::PlayMode_REPEAT_ONE;
    else if (mode == "REPEAT_ONE")
      target = NSROOT::PlayMode_NORMAL;
    else if (mode == "SHUFFLE")
      target = NSROOT::PlayMode_SHUFFLE_NOREPEAT;
    else if (mode == "SHUFFLE_NOREPEAT")
      target = NSROOT::PlayMode_SHUFFLE;
    else
      return;
    player->SetPlayMode(target);
  });
}

void NosonBackend::SetRepeatMode(RepeatMode mode)
{
  tasks_.Push([this, mode] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    const std::string current = player->GetTransportProperty().CurrentPlayMode;
    bool shuffling = (current == "SHUFFLE" || current == "SHUFFLE_NOREPEAT");
    NSROOT::PlayMode_t target;
    if (shuffling)
      // REPEAT_ONE has no shuffle-combined Sonos mode — RepeatMode::One is
      // treated the same as RepeatMode::All here rather than silently
      // failing or dropping shuffle, since "some repeat" is a closer match
      // to the caller's intent than ignoring the request outright.
      target = mode == RepeatMode::Off ? NSROOT::PlayMode_SHUFFLE_NOREPEAT : NSROOT::PlayMode_SHUFFLE;
    else
      target = mode == RepeatMode::Off   ? NSROOT::PlayMode_NORMAL
                : mode == RepeatMode::All ? NSROOT::PlayMode_REPEAT_ALL
                                           : NSROOT::PlayMode_REPEAT_ONE;
    player->SetPlayMode(target);
  });
}

void NosonBackend::SetShuffle(bool shuffle)
{
  tasks_.Push([this, shuffle] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    const std::string current = player->GetTransportProperty().CurrentPlayMode;
    bool repeat_all = (current == "REPEAT_ALL" || current == "SHUFFLE");
    bool repeat_one = (current == "REPEAT_ONE");
    NSROOT::PlayMode_t target;
    if (shuffle)
      // REPEAT_ONE has no shuffle-combined mode — dropped in favor of
      // shuffle itself when turning shuffle on, same reasoning as
      // SetRepeatMode()'s shuffling branch above.
      target = repeat_all || repeat_one ? NSROOT::PlayMode_SHUFFLE : NSROOT::PlayMode_SHUFFLE_NOREPEAT;
    else
      target = repeat_one ? NSROOT::PlayMode_REPEAT_ONE : repeat_all ? NSROOT::PlayMode_REPEAT_ALL : NSROOT::PlayMode_NORMAL;
    player->SetPlayMode(target);
  });
}

void NosonBackend::ToggleShuffle()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    const std::string mode = player->GetTransportProperty().CurrentPlayMode;
    NSROOT::PlayMode_t target;
    if (mode == "NORMAL")
      target = NSROOT::PlayMode_SHUFFLE_NOREPEAT;
    else if (mode == "REPEAT_ALL" || mode == "REPEAT_ONE")
      target = NSROOT::PlayMode_SHUFFLE;
    else if (mode == "SHUFFLE")
      target = NSROOT::PlayMode_REPEAT_ALL;
    else if (mode == "SHUFFLE_NOREPEAT")
      target = NSROOT::PlayMode_NORMAL;
    else
      return;
    player->SetPlayMode(target);
  });
}

void NosonBackend::AddCurrentTrackToFavorites()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;

    NSROOT::AVTProperty prop = player->GetTransportProperty();
    unsigned hh = 0, hm = 0, hs = 0;
    bool has_duration =
        std::sscanf(prop.CurrentTrackDuration.c_str(), "%u:%u:%u", &hh, &hm, &hs) == 3 && (hh || hm || hs);
    NSROOT::DigitalItemPtr item = has_duration ? prop.CurrentTrackMetaData : prop.r_EnqueuedTransportURIMetaData;

    std::string art_uri;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      art_uri = now_playing_.art_uri;
    }

    if (!item || !system_->AddURIToFavorites(item, "", art_uri))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Titel konnte nicht zu Favoriten hinzugefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshFavoritesAsync();
  });
}

void NosonBackend::AddLibraryItemToFavorites(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr item;
    std::string art_uri;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= library_raw_.size())
        return;
      item = library_raw_[index];
      art_uri = index < library_entries_.size() ? library_entries_[index].art_uri : std::string();
    }

    if (!item || !system_->AddURIToFavorites(item, "", art_uri))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Eintrag konnte nicht zu Favoriten hinzugefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshFavoritesAsync();
  });
}

void NosonBackend::RefreshPositionAsync()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    NSROOT::ElementList vars;
    if (!player->GetPositionInfo(vars))
      return;
    unsigned hh = 0, hm = 0, hs = 0, pos = 0;
    if (std::sscanf(vars.GetValue("RelTime").c_str(), "%u:%u:%u", &hh, &hm, &hs) == 3)
      pos = hh * 3600 + hm * 60 + hs;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      position_ = pos;
    }
    position_dispatcher_.emit();
  });
}

unsigned NosonBackend::GetPosition() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return position_;
}

void NosonBackend::SeekAsync(unsigned seconds)
{
  tasks_.Push([this, seconds] {
    auto player = SnapshotPlayer();
    if (!player || !player->SeekTime(static_cast<uint16_t>(seconds)))
      return;
    // We just told the device exactly where to go, so this is the real
    // position, not a guess — unlike the optimistic-then-wrong switch state
    // bug in the README, which was about confirming an outcome that wasn't
    // actually known yet. We only get here on success.
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      position_ = seconds;
    }
    position_dispatcher_.emit();
  });
}

void NosonBackend::PlayLineIn()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player || !player->PlayLineIN())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Line-In wird von diesem Gerät nicht unterstützt oder konnte nicht gestartet werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::PlayDigitalIn()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player || !player->PlayDigitalIN())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Digital-In wird von diesem Gerät nicht unterstützt oder konnte nicht gestartet werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::SetSleepTimer(unsigned seconds)
{
  tasks_.Push([this, seconds] {
    auto player = SnapshotPlayer();
    if (!player || !player->ConfigureSleepTimer(seconds))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Sleep-Timer konnte nicht gesetzt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshSleepTimerAsync();
  });
}

void NosonBackend::RefreshSleepTimerAsync()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    SleepTimerInfo info;
    if (player)
    {
      NSROOT::ElementList vars;
      if (player->GetRemainingSleepTimerDuration(vars))
      {
        info.remaining = vars.GetValue("RemainingSleepTimerDuration");
        info.active = !info.remaining.empty();
      }
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      sleep_timer_ = info;
    }
    sleep_timer_dispatcher_.emit();
  });
}

SleepTimerInfo NosonBackend::GetSleepTimerInfo() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return sleep_timer_;
}

void NosonBackend::RefreshSoundSettingsAsync()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (!coord)
      return;
    const std::string uuid = coord->GetUUID();

    SoundSettings settings;
    int8_t bass = 0, treble = 0;
    uint8_t loudness = 0;
    int16_t nightmode = 0;
    if (player->GetBass(uuid, &bass))
      settings.bass = bass;
    if (player->GetTreble(uuid, &treble))
      settings.treble = treble;
    if (player->GetLoudness(uuid, &loudness))
      settings.loudness = loudness != 0;
    settings.nightmode_supported = player->GetNightmode(uuid, &nightmode);
    if (settings.nightmode_supported)
      settings.nightmode = nightmode != 0;

    uint8_t supports_fixed = 0, fixed = 0;
    settings.output_fixed_supported = player->GetSupportsOutputFixed(uuid, &supports_fixed) && supports_fixed != 0;
    if (settings.output_fixed_supported && player->GetOutputFixed(uuid, &fixed))
      settings.output_fixed = fixed != 0;

    int16_t sub_gain = 0;
    settings.sub_gain_supported = player->GetSubGain(uuid, &sub_gain);
    if (settings.sub_gain_supported)
      settings.sub_gain = sub_gain;

    // Autoplay is a property of *this* device (Player::GetAutoplay() takes
    // no uuid — see SoundSettings::autoplay_* comment), unlike bass/treble/
    // loudness/nightmode/output_fixed/sub_gain above, which all apply to
    // uuid (the selected zone's coordinator) — same reasoning
    // PlayLineIn()/PlayDigitalIn() already use `player` directly rather
    // than a coordinator uuid.
    std::string autoplay_roomuuid;
    settings.autoplay_supported = player->GetAutoplay(autoplay_roomuuid);
    if (settings.autoplay_supported)
      settings.autoplay_enabled = !autoplay_roomuuid.empty();
    uint8_t autoplay_use = 0;
    if (player->GetUseAutoplayVolume(&autoplay_use))
      settings.autoplay_use_volume = autoplay_use != 0;
    uint8_t autoplay_vol = 0;
    if (player->GetAutoplayVolume(&autoplay_vol))
      settings.autoplay_volume = autoplay_vol;

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      sound_settings_ = settings;
    }
    sound_settings_dispatcher_.emit();
  });
}

SoundSettings NosonBackend::GetSoundSettings() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return sound_settings_;
}

void NosonBackend::SetBass(int8_t value)
{
  tasks_.Push([this, value] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (coord)
      player->SetBass(coord->GetUUID(), value);
  });
}

void NosonBackend::SetTreble(int8_t value)
{
  tasks_.Push([this, value] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (coord)
      player->SetTreble(coord->GetUUID(), value);
  });
}

void NosonBackend::SetLoudness(bool enabled)
{
  tasks_.Push([this, enabled] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (coord)
      player->SetLoudness(coord->GetUUID(), enabled ? 1 : 0);
    // Unconditional, success or not: this is what lets the switch in the UI
    // snap back to the true device state on failure, instead of getting
    // stuck showing whatever the user last dragged it to (the same class of
    // bug the grouping popover's switches had — see its comments).
    RefreshSoundSettingsAsync();
  });
}

void NosonBackend::SetNightmode(bool enabled)
{
  tasks_.Push([this, enabled] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (!coord)
      return;
    if (!player->SetNightmode(coord->GetUUID(), enabled ? 1 : 0))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Night Mode wird von diesem Gerät nicht unterstützt.";
      error_dispatcher_.emit();
    }
    RefreshSoundSettingsAsync();
  });
}

void NosonBackend::SetOutputFixed(bool enabled)
{
  tasks_.Push([this, enabled] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (!coord)
      return;
    if (!player->SetOutputFixed(coord->GetUUID(), enabled ? 1 : 0))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Feste Lautstärke konnte nicht geändert werden.";
      error_dispatcher_.emit();
    }
    // Same reasoning as SetLoudness()/SetNightmode(): refresh unconditionally
    // so the switch snaps back to the true device state on failure.
    RefreshSoundSettingsAsync();
  });
}

void NosonBackend::SetSubGain(int16_t value)
{
  tasks_.Push([this, value] {
    auto player = SnapshotPlayer();
    auto zone = SnapshotZone();
    if (!player || !zone)
      return;
    auto coord = zone->GetCoordinator();
    if (!coord)
      return;
    if (!player->SetSubGain(coord->GetUUID(), value))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Sub-Pegel konnte nicht geändert werden.";
      error_dispatcher_.emit();
    }
    RefreshSoundSettingsAsync();
  });
}

void NosonBackend::SetAutoplay(bool enabled)
{
  tasks_.Push([this, enabled] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    if (!player->SetAutoplay(enabled))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Automatische Wiedergabe konnte nicht geändert werden.";
      error_dispatcher_.emit();
    }
    RefreshSoundSettingsAsync();
  });
}

void NosonBackend::SetAutoplayVolume(uint8_t value)
{
  tasks_.Push([this, value] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    player->SetAutoplayVolume(value);
  });
}

void NosonBackend::SetUseAutoplayVolume(bool enabled)
{
  tasks_.Push([this, enabled] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    if (!player->SetUseAutoplayVolume(enabled ? 1 : 0))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Einstellung konnte nicht geändert werden.";
      error_dispatcher_.emit();
    }
    RefreshSoundSettingsAsync();
  });
}

void NosonBackend::SetLedState(bool enabled)
{
  tasks_.Push([this, enabled] {
    auto player = SnapshotPlayer();
    if (!player || !player->SetLEDState(enabled))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Status-LED konnte nicht geändert werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::RefreshQueueAsync()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;

    // Each zone coordinator serves its own queue via its local
    // ContentDirectory service under the fixed root object id "Q:0"
    // (see noson/src/contentdirectory.cpp: SearchTable[SearchQueue]).
    // Player doesn't expose its internal ContentDirectory, so a short-lived
    // instance targeting the same host/port is used here instead.
    NSROOT::ContentDirectory queueDirectory(player->GetHost(), player->GetPort());
    // Deliberately not calling browser.Browse(0, 200) again after
    // constructing it below: the constructor already performs the
    // equivalent fetch with the same parameters, and — confirmed live —
    // Browse()'s own "index >= total" bounds check treats index 0 of a
    // *genuinely empty* queue as out of range and returns false,
    // indistinguishable from a real failure (this is what caused "Warteschlange
    // konnte nicht geladen werden" to appear on an actually-empty queue).
    // ContentBrowser's public API gives no way to tell "fetch failed" apart
    // from "fetch succeeded, zero items" in that case, and an empty queue
    // is an entirely normal, common state — unlike a real failure — so
    // this just trusts the constructor's result directly rather than
    // risking a chronic false "failed to load" toast every time the queue
    // is empty.
    NSROOT::ContentBrowser browser(queueDirectory, NSROOT::ContentSearch(NSROOT::SearchQueue, ""), 200);
    // Safe to call even for a genuinely empty queue (see the comment
    // above): ExhaustBrowser() only touches Browse() when total() > 0.
    ExhaustBrowser(browser);

    std::vector<QueueItem> items;
    items.reserve(browser.table().size());
    unsigned idx = 0;
    for (const auto& item : browser.table())
    {
      QueueItem qi;
      qi.object_id = item->GetObjectID();
      qi.title = item->GetValue("dc:title");
      qi.artist = item->GetValue("dc:creator");
      qi.album = item->GetValue("upnp:album");
      qi.art_uri = ResolveArtUri(item->GetValue("upnp:albumArtURI"));
      qi.index = idx++;
      items.push_back(std::move(qi));
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      queue_ = std::move(items);
      queue_update_id_ = browser.GetUpdateID();
    }
    queue_dispatcher_.emit();
  });
}

void NosonBackend::PlayQueueItem(unsigned index)
{
  tasks_.Push([this, index] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    if (player->PlayQueue(false))
    {
      player->SeekTrack(index + 1);  // SeekTrack is 1-based
      player->Play();
    }
  });
}

void NosonBackend::RemoveQueueItem(unsigned index)
{
  tasks_.Push([this, index] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    std::string object_id;
    unsigned update_id;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= queue_.size())
        return;
      object_id = queue_[index].object_id;
      update_id = queue_update_id_;
    }
    if (!player->RemoveTrackFromQueue(object_id, update_id))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Titel konnte nicht aus der Warteschlange entfernt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::ClearQueue()
{
  tasks_.Push([this] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    if (!player->RemoveAllTracksFromQueue())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Warteschlange konnte nicht geleert werden.";
      error_dispatcher_.emit();
      return;
    }

    // We just told the device to remove every track, so the queue *is*
    // empty now — no need to ask the device to confirm what we already
    // know. Confirmed live this matters: RemoveAllTracksFromQueue()
    // succeeding doesn't mean the device's own ContentDirectory index has
    // caught up yet, so a Browse() fired immediately afterward (whether a
    // manual retry loop here, or the one RefreshQueueAsync() naturally
    // runs when the resulting ContentDirectoryChanged event arrives) could
    // still error out outright while the device is still processing the
    // bulk removal — which is what RefreshQueueAsync()'s own retry (below)
    // is for; ClearQueue() itself doesn't need to duplicate that here.
    // queue_update_id_ is deliberately left untouched: it'll be corrected
    // by that same natural refresh once the device actually settles, and
    // nothing here needs a fresh one before then.
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      queue_.clear();
    }
    queue_dispatcher_.emit();
  });
}

void NosonBackend::SaveQueueAsPlaylist(const std::string& title)
{
  tasks_.Push([this, title] {
    auto player = SnapshotPlayer();
    if (!player || !player->SaveQueue(title))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Warteschlange konnte nicht als Playlist gespeichert werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::ReorderQueueItem(unsigned from, unsigned to)
{
  tasks_.Push([this, from, to] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    unsigned update_id;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      update_id = queue_update_id_;
    }
    // ReorderTracksInQueue's positions are 1-based (UPnP convention, same
    // as SeekTrack); moving a track *forward* also needs the target
    // shifted by one, since InsertBefore is evaluated after the moved
    // track is notionally removed from its original slot — mirrors
    // noson-app's own reorderTrackInQueue() (noson.qml).
    unsigned insert_before = to;
    if (from < to)
      ++insert_before;
    if (!player->ReorderTracksInQueue(from + 1, 1, insert_before + 1, update_id))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Warteschlange konnte nicht umsortiert werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::RefreshFavoritesAsync()
{
  tasks_.Push([this] {
    // Favorites are household-wide, served by the originally discovered
    // device's ContentDirectory under the fixed root object id "FV:2" (see
    // contentdirectory.cpp: SearchTable[SearchFavorite]) — independent of
    // system_/GetHost()+GetPort() never change after construction, so this
    // needs no lock to read.
    NSROOT::ContentDirectory favoritesDirectory(system_->GetHost(), system_->GetPort());
    // Not calling browser.Browse(0, 200) again here — same "index 0 of a
    // genuinely empty container looks identical to a real failure" issue
    // documented in RefreshQueueAsync() (this had the same false "Favoriten
    // konnten nicht geladen werden" bug on a household with zero
    // favorites, just never previously caught live). ExhaustBrowser()
    // below only calls Browse() again when there's real additional data
    // waiting (total() > count()), so it can't reintroduce this.
    NSROOT::ContentBrowser browser(favoritesDirectory, NSROOT::ContentSearch(NSROOT::SearchFavorite, ""), 200);
    ExhaustBrowser(browser);

    std::vector<FavoriteItem> items;
    std::vector<NSROOT::DigitalItemPtr> raw;
    items.reserve(browser.table().size());
    raw.reserve(browser.table().size());
    unsigned idx = 0;
    for (const auto& favorite : browser.table())
    {
      FavoriteItem fi;
      fi.title = favorite->GetValue("dc:title");
      fi.subtitle = favorite->GetValue("r:description");
      fi.art_uri = ResolveArtUri(favorite->GetValue("upnp:albumArtURI"));
      fi.index = idx++;
      items.push_back(std::move(fi));
      raw.push_back(favorite);
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      favorites_ = std::move(items);
      favorites_raw_ = std::move(raw);
    }
    favorites_dispatcher_.emit();
  });
}

std::vector<FavoriteItem> NosonBackend::GetFavorites() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return favorites_;
}

void NosonBackend::PlayFavorite(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr favorite;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= favorites_raw_.size())
        return;
      favorite = favorites_raw_[index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    // A favorite wraps its actual playable item in metadata; it has to be
    // unwrapped before it can be queued or set as the current source (see
    // noson-app/backend/NosonApp/player.cpp, playFavorite(), which this
    // mirrors).
    NSROOT::DigitalItemPtr item;
    bool ok = false;
    if (NSROOT::System::ExtractObjectFromFavorite(favorite, item))
    {
      if (NSROOT::System::CanQueueItem(item))
      {
        if (player->PlayQueue(false))
        {
          unsigned pos = player->AddURIToQueue(item, 1);
          ok = pos > 0 && player->SeekTrack(pos) && player->Play();
        }
      }
      else
      {
        EnsureServiceDesc(*system_, item);
        ok = player->SetCurrentURI(item) && player->Play();
      }
    }

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Favorit konnte nicht abgespielt werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::AddFavoriteToQueue(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr favorite;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= favorites_raw_.size())
        return;
      favorite = favorites_raw_[index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    NSROOT::DigitalItemPtr item;
    bool ok = false;
    bool queueable = false;
    if (NSROOT::System::ExtractObjectFromFavorite(favorite, item) && NSROOT::System::CanQueueItem(item))
    {
      queueable = true;
      ok = player->AddURIToQueue(item, 0) > 0;
    }

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = queueable ? "Favorit konnte nicht zur Warteschlange hinzugefügt werden."
                                  : "Dieser Favorit kann nicht zur Warteschlange hinzugefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::PlayFavoriteNext(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr favorite;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= favorites_raw_.size())
        return;
      favorite = favorites_raw_[index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    NSROOT::DigitalItemPtr item;
    bool ok = false;
    bool queueable = false;
    if (NSROOT::System::ExtractObjectFromFavorite(favorite, item) && NSROOT::System::CanQueueItem(item))
    {
      queueable = true;
      unsigned insert_pos = player->GetTransportProperty().CurrentTrack + 1;
      ok = player->AddURIToQueue(item, insert_pos) > 0;
    }

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = queueable ? "Favorit konnte nicht als nächster Titel eingefügt werden."
                                  : "Dieser Favorit kann nicht als nächster Titel eingefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::DeleteFavorite(unsigned index)
{
  tasks_.Push([this, index] {
    std::string object_id;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= favorites_raw_.size())
        return;
      object_id = favorites_raw_[index]->GetObjectID();
    }
    if (!system_->DestroyFavorite(object_id))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Favorit konnte nicht gelöscht werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshFavoritesAsync();
  });
}

void NosonBackend::AddAllFavoritesToQueue()
{
  tasks_.Push([this] {
    std::vector<NSROOT::DigitalItemPtr> favorites;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      favorites = favorites_raw_;
    }
    auto player = SnapshotPlayer();
    if (!player || favorites.empty())
      return;

    // Each favorite wraps its real playable item, same unwrapping
    // PlayFavorite()/AddFavoriteToQueue() do per-item — see
    // ExtractObjectFromFavorite()'s own comment there. A favorite that
    // doesn't unwrap to a queueable item (a live radio stream, most
    // commonly) is silently skipped, same as AddAllLibraryItemsToQueue()
    // already does for its own non-queueable entries — there's nothing
    // sensible a bulk queue action could do with a stream anyway.
    unsigned added = 0;
    for (const NSROOT::DigitalItemPtr& favorite : favorites)
    {
      NSROOT::DigitalItemPtr item;
      if (NSROOT::System::ExtractObjectFromFavorite(favorite, item) && NSROOT::System::CanQueueItem(item) &&
          player->AddURIToQueue(item, 0) > 0)
        ++added;
    }

    if (added == 0)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Favoriten konnten nicht zur Warteschlange hinzugefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::PlayAllFavoritesAsync()
{
  tasks_.Push([this] {
    std::vector<NSROOT::DigitalItemPtr> favorites;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      favorites = favorites_raw_;
    }
    auto player = SnapshotPlayer();
    if (!player || favorites.empty())
      return;

    player->PlayQueue(false);
    if (!player->RemoveAllTracksFromQueue())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Warteschlange konnte nicht geleert werden.";
      error_dispatcher_.emit();
      return;
    }

    unsigned added = 0;
    for (const NSROOT::DigitalItemPtr& favorite : favorites)
    {
      NSROOT::DigitalItemPtr item;
      if (NSROOT::System::ExtractObjectFromFavorite(favorite, item) && NSROOT::System::CanQueueItem(item) &&
          player->AddURIToQueue(item, 0) > 0)
        ++added;
    }

    if (added == 0 || !player->SeekTrack(1) || !player->Play())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Favoriten konnten nicht abgespielt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

// kServiceRootPrefix is declared in noson-types.h (shared with GnomosWindow,
// which needs the same prefix to push a matching breadcrumb after a
// successful service link).

namespace
{
// Cached levels older than this are treated as a miss and re-fetched —
// covers third-party services, which have no change-notification Gnomos
// can act on (unlike the local library, invalidated outright by
// InvalidateLibraryCache() on a real SVCEvent_ContentDirectoryChanged).
constexpr auto kLibraryCacheTtl = std::chrono::minutes(5);
// Simple cap: if exceeded, the whole cache is dropped rather than
// maintaining real LRU bookkeeping — browsing this many distinct levels
// in one session is already an edge case, not a pattern worth optimizing
// for.
constexpr size_t kMaxLibraryCacheEntries = 300;
}  // namespace

std::string NosonBackend::LibraryCacheKey(const std::string& object_id) const
{
  if (active_smapi_ && active_service_)
    return active_service_->GetId() + "\x1f" + object_id;
  return object_id;
}

bool NosonBackend::GetCachedLibraryLevel(const std::string& object_id, std::vector<LibraryEntry>& out_entries,
                                          std::vector<NSROOT::DigitalItemPtr>& out_raw) const
{
  auto it = library_cache_.find(LibraryCacheKey(object_id));
  if (it == library_cache_.end())
    return false;
  if (std::chrono::steady_clock::now() - it->second.fetched_at > kLibraryCacheTtl)
    return false;
  out_entries = it->second.entries;
  out_raw = it->second.raw;
  return true;
}

void NosonBackend::StoreLibraryCacheLevel(const std::string& object_id, const std::vector<LibraryEntry>& entries,
                                           const std::vector<NSROOT::DigitalItemPtr>& raw)
{
  if (library_cache_.size() >= kMaxLibraryCacheEntries)
    library_cache_.clear();
  library_cache_[LibraryCacheKey(object_id)] = {entries, raw, std::chrono::steady_clock::now()};
}

void NosonBackend::InvalidateLibraryCache()
{
  library_cache_.clear();
}

void NosonBackend::BrowseLibraryAsync(const std::string& object_id)
{
  if (object_id.empty())
  {
    tasks_.Push([this] {
      // Leaving any service's browse tree back to the true top level.
      active_smapi_.reset();
      active_service_.reset();
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active_service_search_categories_.clear();
      }

      // Fixed local categories — see contentdirectory.cpp's SearchTable for
      // these object id strings; no network call needed for these. Radio
      // (TuneIn) browses/plays through the exact same ContentDirectory
      // mechanism as the local library — it's just another object id.
      //
      // "A:PLAYLISTS" and "SQ:" are two different things confirmed live:
      // "A:" is the locally-indexed-share namespace (Artist/Album/Genre/
      // Track/Playlist all live there), so "A:PLAYLISTS" is playlist files
      // found on an indexed local music share — it 404s outright with
      // nothing indexed, which is the common case for anyone not running a
      // local NAS share. "SQ:" is the separate, always-present root for
      // actual Sonos-native saved playlists, i.e. exactly what
      // Player::SaveQueue() (the "als Playlist speichern" queue action)
      // creates — that's the one most people mean by "my playlists".
      std::vector<LibraryEntry> roots = {
          {"A:ALBUMARTIST", "Interpreten", "", true, "", false, "avatar-default-symbolic"},
          {"A:ALBUM", "Alben", "", true, "", false, "media-optical-cd-symbolic"},
          {"A:GENRE", "Genres", "", true, "", false, "folder-music-symbolic"},
          {"A:TRACKS", "Titel", "", true, "", false, ""},
          {"SQ:", "Playlisten", "", true, "", false, "media-playlist-consecutive-symbolic"},
          {"A:PLAYLISTS", "Playlisten (lokale Freigabe)", "", true, "", false, "media-playlist-consecutive-symbolic"},
          {"R:0/0", "Radiosender", "", true, "", false, "network-wireless-symbolic"},
      };
      // One root entry per service already linked on this household (e.g.
      // Spotify via bonob) — SMAPI::Init() pulls that service's existing
      // account/session automatically, no separate auth flow needed here.
      for (const NSROOT::SMServicePtr& svc : system_->GetEnabledServices())
      {
        if (!svc)
          continue;
        LibraryEntry entry;
        entry.object_id = std::string(kServiceRootPrefix) + svc->GetId();
        entry.title = svc->GetName();
        entry.is_container = true;
        roots.push_back(std::move(entry));
      }
      // GnomosWindow recognizes this exact object_id and opens the service
      // picker/link dialog instead of trying to browse into it.
      roots.push_back({kLinkServiceSentinel, "Dienst verknüpfen…", "", true, "", false, ""});

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        library_entries_ = std::move(roots);
        library_raw_.clear();
      }
      library_dispatcher_.emit();
    });
    return;
  }

  tasks_.Push([this, object_id] {
    if (object_id.compare(0, strlen(kServiceRootPrefix), kServiceRootPrefix) == 0)
    {
      const std::string service_id = object_id.substr(strlen(kServiceRootPrefix));
      NSROOT::SMServicePtr svc;
      for (const NSROOT::SMServicePtr& candidate : system_->GetEnabledServices())
        if (candidate && candidate->GetId() == service_id)
          svc = candidate;

      auto smapi = std::make_unique<NSROOT::SMAPI>(*system_);
      if (!svc || !smapi->Init(svc, "en-US"))
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_error_ = "Dienst konnte nicht initialisiert werden.";
        error_dispatcher_.emit();
        return;
      }
      active_smapi_ = std::move(smapi);
      active_service_ = svc;

      // GetEnabledServices() only lists services with *some* locally
      // registered account (see its comment), but that account can still
      // have no real session yet — happens right after AddServiceOAuth()
      // registered an empty placeholder, or if a previously saved token
      // expired.
      if (active_smapi_->AuthTokenExpired())
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_error_ = "Anmeldung bei diesem Dienst ist abgelaufen oder unvollständig — bitte über "
                          "\"Dienst verknüpfen\" erneut verknüpfen.";
        error_dispatcher_.emit();
        return;
      }
      CacheActiveServiceSearchCategories();
      BrowseActiveServiceLocked("root");
      return;
    }

    // A local library object_id always carries one of these three reserved
    // prefixes (see the root categories BrowseLibraryAsync("") builds) — a
    // SMAPI service would never itself return one of these as an item id,
    // so seeing one here means we're browsing local content regardless of
    // whether active_smapi_ is still set from a service visited earlier.
    // Confirmed live: jumping from a service straight to a local library
    // sidebar shortcut (GnomosWindow::RebuildLibraryNavEntries()) — which
    // calls BrowseLibraryAsync() with the local id directly, not via the ""
    // root first — silently routed that local id through the still-active
    // service session instead, coming back empty rather than showing the
    // local level at all.
    bool looks_local = object_id.compare(0, 2, "A:") == 0 || object_id.compare(0, 3, "SQ:") == 0 ||
                        object_id.compare(0, 2, "R:") == 0;
    if (looks_local && active_smapi_)
    {
      active_smapi_.reset();
      active_service_.reset();
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active_service_search_categories_.clear();
      }
    }

    if (active_smapi_)
    {
      BrowseActiveServiceLocked(object_id);
      return;
    }

    {
      std::vector<LibraryEntry> cached_entries;
      std::vector<NSROOT::DigitalItemPtr> cached_raw;
      if (GetCachedLibraryLevel(object_id, cached_entries, cached_raw))
      {
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          library_entries_ = std::move(cached_entries);
          library_raw_ = std::move(cached_raw);
        }
        library_dispatcher_.emit();
        return;
      }
    }

    NSROOT::ContentDirectory libraryDirectory(system_->GetHost(), system_->GetPort());
    // Not calling browser.Browse(0, 200) again here — same false-failure-
    // on-empty issue documented in RefreshQueueAsync() (an empty library
    // level, e.g. a genre with no albums yet, showed a bogus "Bibliothek
    // konnte nicht geladen werden"). ExhaustBrowser() only calls Browse()
    // again when there's real additional data waiting.
    NSROOT::ContentBrowser browser(libraryDirectory, object_id, 200);
    ExhaustBrowser(browser);

    // Local ContentDirectory has no per-item hint like SMAPI's own
    // displayType (see BrowseActiveServiceLocked()'s identical field) —
    // both "A:ALBUM" (the root album list) and "A:ALBUMARTIST" (the root
    // artist list, and an artist's own album list once you browse into
    // one) use this object_id prefix, gated on the entry actually being a
    // container too: browsing all the way down into a real album swaps
    // the entries for that album's individual tracks, which should fall
    // back to the plain list rather than a grid of identical repeated
    // cover art. Computed once per level (not per entry) since it only
    // depends on the level's own object_id.
    bool grid_eligible_prefix = object_id.compare(0, 7, "A:ALBUM") == 0;

    // Only ever populated for "R:0/0" — see SaveRadioFavicon()'s own
    // comment for why a custom station's thumbnail can't come from Sonos
    // itself the way a built-in TuneIn one's upnp:albumArtURI does.
    std::map<std::string, std::string> radio_favicons;
    if (object_id == "R:0/0")
      radio_favicons = LoadRadioFavicons();

    std::vector<LibraryEntry> entries;
    std::vector<NSROOT::DigitalItemPtr> raw;
    entries.reserve(browser.table().size());
    raw.reserve(browser.table().size());
    for (const auto& item : browser.table())
    {
      LibraryEntry entry;
      entry.object_id = item->GetObjectID();
      entry.title = item->GetValue("dc:title");
      entry.is_container = item->IsContainer();
      if (!entry.is_container)
        entry.subtitle = item->GetValue("dc:creator");
      entry.art_uri = ResolveArtUri(item->GetValue("upnp:albumArtURI"));
      if (entry.art_uri.empty() && !radio_favicons.empty())
      {
        const std::string res = item->GetValue("res");
        if (!res.empty())
        {
          auto it = radio_favicons.find(RadioStreamMatchKey(res));
          if (it != radio_favicons.end())
            entry.art_uri = it->second;
        }
      }
      entry.icon_name = IconNameForSubType(item->subType());
      entry.display_as_grid = grid_eligible_prefix && entry.is_container;
      entries.push_back(std::move(entry));
      raw.push_back(item);
    }

    StoreLibraryCacheLevel(object_id, entries, raw);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      library_entries_ = std::move(entries);
      library_raw_ = std::move(raw);
    }
    library_dispatcher_.emit();
  });
}

// Runs on the TaskQueue worker thread, same as BrowseLibraryAsync()'s own
// body — active_smapi_ is only ever touched there (see its declaration).
void NosonBackend::CacheActiveServiceSearchCategories()
{
  std::vector<std::string> categories;
  for (const NSROOT::ElementPtr& el : active_smapi_->AvailableSearchCategories())
    if (el)
      categories.push_back(el->GetKey());
  std::lock_guard<std::mutex> lock(state_mutex_);
  active_service_search_categories_ = std::move(categories);
}

std::vector<std::string> NosonBackend::GetActiveServiceSearchCategories() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return active_service_search_categories_;
}

void NosonBackend::SearchActiveServiceAsync(const std::string& category, const std::string& term)
{
  tasks_.Push([this, category, term] {
    if (!active_smapi_)
      return;

    // Unlike ContentBrowser (which accumulates pages into its own internal
    // table across repeated Browse() calls), SMAPI::Search()'s SMAPIMetadata
    // out-param is *replaced* on every call — see SMAPIMetadata::Reset(),
    // called fresh from inside Search() each time — so pagination here
    // means accumulating into entries/raw ourselves across calls, capped at
    // kMaxBrowseItems for the same reason ExhaustBrowser() caps local
    // ContentDirectory browsing.
    std::vector<LibraryEntry> entries;
    std::vector<NSROOT::DigitalItemPtr> raw;
    constexpr unsigned kPageSize = 200;
    unsigned index = 0;
    unsigned total = kPageSize;  // corrected to the real total after the first page
    bool any_page_ok = false;
    while (index < total && index < kMaxBrowseItems)
    {
      NSROOT::SMAPIMetadata meta;
      if (!active_smapi_->Search(category, term, static_cast<int>(index), static_cast<int>(kPageSize), meta))
        break;
      any_page_ok = true;
      total = std::min(meta.TotalCount(), kMaxBrowseItems);

      for (const NSROOT::SMAPIItem& smapi_item : meta.GetItems())
      {
        if (!smapi_item.item)
          continue;
        LibraryEntry entry;
        entry.object_id = smapi_item.item->GetObjectID();
        entry.title = smapi_item.item->GetValue("dc:title");
        entry.is_container = smapi_item.item->IsContainer();
        if (!entry.is_container)
          entry.subtitle = smapi_item.item->GetValue("dc:creator");
        entry.art_uri = ResolveArtUri(smapi_item.item->GetValue("upnp:albumArtURI"));
        entry.icon_name = IconNameForSubType(smapi_item.item->subType());
        // See BrowseActiveServiceLocked()'s identical check for why —
        // Grid marks a category tile, not real content, so its own
        // service-branded icon image gives way to icon_name here.
        if (smapi_item.displayType == NSROOT::SMAPIItem::Grid)
          entry.art_uri.clear();
        // Trust the service's own displayType when it says Grid — but
        // don't require it: confirmed live against a real bonob server,
        // its "Albums" listing carries real per-album cover art but
        // doesn't set displayType to Grid at all (only its root menu
        // does), so relying on displayType alone left an obviously
        // grid-worthy listing stuck in list mode. A container with real
        // art is exactly what a grid is for, regardless of whether the
        // service bothered to say so explicitly — same rule a reader
        // would apply by eye.
        entry.display_as_grid =
            smapi_item.displayType == NSROOT::SMAPIItem::Grid || (entry.is_container && !entry.art_uri.empty());
        entries.push_back(std::move(entry));
        raw.push_back(smapi_item.uriMetadata);
      }

      unsigned fetched = meta.ItemCount();
      if (fetched == 0)
        break;  // no progress — avoid spinning on an unexpected response
      index += fetched;
    }

    if (!any_page_ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Suche fehlgeschlagen.";
      error_dispatcher_.emit();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      library_entries_ = std::move(entries);
      library_raw_ = std::move(raw);
    }
    library_dispatcher_.emit();
  });
}

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

void NosonBackend::SearchLocalLibraryAsync(const std::string& object_id, const std::string& term)
{
  tasks_.Push([this, object_id, term] {
    NSROOT::ContentDirectory libraryDirectory(system_->GetHost(), system_->GetPort());
    // Not calling browser.Browse(0, 500) again here — same false-failure-
    // on-empty issue documented in RefreshQueueAsync() (browsing an
    // already-empty level to search within it showed a bogus "Suche
    // fehlgeschlagen"). ExhaustBrowser() only calls Browse() again when
    // there's real additional data waiting.
    NSROOT::ContentBrowser browser(libraryDirectory, object_id, 500);
    ExhaustBrowser(browser);

    std::string term_lower = term;
    std::transform(term_lower.begin(), term_lower.end(), term_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<LibraryEntry> entries;
    std::vector<NSROOT::DigitalItemPtr> raw;
    for (const auto& item : browser.table())
    {
      LibraryEntry entry;
      entry.object_id = item->GetObjectID();
      entry.title = item->GetValue("dc:title");
      entry.is_container = item->IsContainer();
      if (!entry.is_container)
        entry.subtitle = item->GetValue("dc:creator");
      entry.art_uri = ResolveArtUri(item->GetValue("upnp:albumArtURI"));
      entry.icon_name = IconNameForSubType(item->subType());
      if (!ContainsCaseInsensitive(entry.title, term_lower) && !ContainsCaseInsensitive(entry.subtitle, term_lower))
        continue;
      entries.push_back(std::move(entry));
      raw.push_back(item);
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      library_entries_ = std::move(entries);
      library_raw_ = std::move(raw);
    }
    library_dispatcher_.emit();
  });
}

void NosonBackend::BrowseActiveServiceLocked(const std::string& id)
{
  {
    std::vector<LibraryEntry> cached_entries;
    std::vector<NSROOT::DigitalItemPtr> cached_raw;
    if (GetCachedLibraryLevel(id, cached_entries, cached_raw))
    {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        library_entries_ = std::move(cached_entries);
        library_raw_ = std::move(cached_raw);
      }
      library_dispatcher_.emit();
      return;
    }
  }

  // Same per-call-replace vs. accumulate-ourselves pagination as
  // SearchActiveServiceAsync() — see its comment for why.
  std::vector<LibraryEntry> entries;
  std::vector<NSROOT::DigitalItemPtr> raw;
  constexpr unsigned kPageSize = 200;
  unsigned index = 0;
  unsigned total = kPageSize;
  bool any_page_ok = false;
  while (index < total && index < kMaxBrowseItems)
  {
    NSROOT::SMAPIMetadata meta;
    if (!active_smapi_->GetMetadata(id, static_cast<int>(index), static_cast<int>(kPageSize), false, meta))
      break;
    any_page_ok = true;
    total = std::min(meta.TotalCount(), kMaxBrowseItems);

    for (const NSROOT::SMAPIItem& smapi_item : meta.GetItems())
    {
      if (!smapi_item.item)
        continue;
      LibraryEntry entry;
      entry.object_id = smapi_item.item->GetObjectID();
      entry.title = smapi_item.item->GetValue("dc:title");
      entry.is_container = smapi_item.item->IsContainer();
      if (!entry.is_container)
        entry.subtitle = smapi_item.item->GetValue("dc:creator");
      entry.art_uri = ResolveArtUri(smapi_item.item->GetValue("upnp:albumArtURI"));
      entry.icon_name = IconNameForSubType(smapi_item.item->subType());
      // A service's root menu ("Albums"/"Random"/"Internet Radio"/... —
      // id == "root") is always a list of top-level categories, regardless
      // of what SubType_t the service happens to report for each one.
      // Confirmed live against bonob: most of its own root tiles report
      // itemType "albumList" (-> SubType_storageFolder, already covered by
      // IconNameForSubType() below), but "Internet Radio" comes back as
      // itemType "stream" (-> SubType_audioItem, and IsContainer() false —
      // a real leaf, not a folder to browse into, unlike every other root
      // tile) — a leaf-item subtype IconNameForSubType() rightly leaves
      // unmapped everywhere else in the app (a real playable track has no
      // better icon than the generic note), but which is simply wrong for
      // a root-level tile. Defaulting only *here*, only when the lookup
      // came back empty, keeps every other browsing level (a real
      // track/stream deeper in a listing) exactly as before. The
      // container/non-container split mirrors local library's own root
      // categories (BrowseLibraryAsync("")'s hardcoded `roots` list) —
      // "network-wireless-symbolic" is exactly what its own "Radiosender"
      // entry uses for the same reason: a stream to play, not a folder.
      if (id == "root" && entry.icon_name.empty())
        entry.icon_name = entry.is_container ? "folder-music-symbolic" : "network-wireless-symbolic";
      // displayType == Grid is bonob's (and, per this same field's use
      // elsewhere, apparently every SMAPI service's) way of marking a
      // *category* tile — its own root menu ("Artists"/"Albums"/"Random"/
      // ...), not a specific piece of real content — confirmed live: every
      // one of those tiles carries its own distinct service-branded icon
      // image, visually inconsistent with the rest of a GNOME app (a
      // microphone, a target/circle, shuffle arrows, ...). Discarding that
      // image in favor of icon_name here, rather than only falling back to
      // it when art_uri is empty, is what actually fixes that — a real
      // album's/artist's own genuine cover art (deeper levels, where
      // displayType isn't Grid) is untouched.
      if (smapi_item.displayType == NSROOT::SMAPIItem::Grid)
        entry.art_uri.clear();
      // Trust the service's own displayType when it says Grid — but don't
      // require it: confirmed live against a real bonob server, its own
      // "Albums" listing carries real per-album cover art but doesn't set
      // displayType to Grid at all (only its root menu does), so relying
      // on displayType alone left an obviously grid-worthy listing stuck
      // in list mode with no way to switch it. A container with real art
      // is exactly what a grid is for, regardless of whether the service
      // bothered to say so explicitly.
      entry.display_as_grid =
          smapi_item.displayType == NSROOT::SMAPIItem::Grid || (entry.is_container && !entry.art_uri.empty());
      entries.push_back(std::move(entry));
      // Containers have no uriMetadata (they're never played, only browsed
      // into) — null is fine, PlayLibraryItem() only reaches this index for
      // non-container entries.
      raw.push_back(smapi_item.uriMetadata);
    }

    unsigned fetched = meta.ItemCount();
    if (fetched == 0)
      break;  // no progress — avoid spinning on an unexpected response
    index += fetched;
  }

  if (!any_page_ok)
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pending_error_ = active_smapi_->AuthTokenExpired()
                          ? "Anmeldung bei diesem Dienst ist abgelaufen — bitte über \"Dienst verknüpfen\" erneut verknüpfen."
                          : "Dienst konnte nicht durchsucht werden.";
    error_dispatcher_.emit();
    return;
  }

  StoreLibraryCacheLevel(id, entries, raw);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    library_entries_ = std::move(entries);
    library_raw_ = std::move(raw);
  }
  library_dispatcher_.emit();
}

std::vector<LinkableService> NosonBackend::GetLinkableServices() const
{
  std::vector<LinkableService> result;
  // GetAvailableServices() is an in-memory filter over data System::Discover()
  // already fetched — no network call, safe to call directly (system_ is
  // set once at construction and never reassigned, like elsewhere in this
  // class — see HandleSystemEvent()'s comment).
  for (const NSROOT::SMServicePtr& svc : system_->GetAvailableServices())
  {
    if (!svc)
      continue;
    // Anonymous/UserId-policy services either need no linking at all or a
    // username+password form this pass doesn't implement — only offer the
    // ones this flow (GetAppLink/GetDeviceLinkCode + polling) actually
    // handles.
    const std::string auth = svc->GetPolicy() ? std::string(svc->GetPolicy()->GetAttribut("Auth")) : std::string();
    if (auth != "AppLink" && auth != "DeviceLink")
      continue;
    LinkableService info;
    info.id = svc->GetId();
    info.name = svc->GetName();
    result.push_back(std::move(info));
  }
  return result;
}

void NosonBackend::BeginServiceLink(const std::string& service_id)
{
  tasks_.Push([this, service_id] {
    NSROOT::SMServicePtr svc;
    for (const NSROOT::SMServicePtr& candidate : system_->GetAvailableServices())
      if (candidate && candidate->GetId() == service_id)
        svc = candidate;
    if (!svc)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Dienst nicht gefunden.";
      error_dispatcher_.emit();
      return;
    }

    auto smapi = std::make_unique<NSROOT::SMAPI>(*system_);
    if (!smapi->Init(svc, "en-US"))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Dienst konnte nicht initialisiert werden.";
      error_dispatcher_.emit();
      return;
    }

    std::string reg_url, link_code;
    bool ok = false;
    if (smapi->GetPolicyAuth() == NSROOT::SMAPI::Auth_AppLink)
      ok = smapi->GetAppLink(reg_url, link_code);
    else if (smapi->GetPolicyAuth() == NSROOT::SMAPI::Auth_DeviceLink)
      ok = smapi->GetDeviceLinkCode(reg_url, link_code);

    if (!ok || reg_url.empty())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Verknüpfung konnte nicht gestartet werden.";
      error_dispatcher_.emit();
      return;
    }

    // Kept as the pending link target for CompleteServiceLink() to use.
    active_smapi_ = std::move(smapi);
    active_service_ = svc;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_link_url_ = reg_url;
      pending_link_code_ = link_code;
    }
    service_link_ready_dispatcher_.emit();
  });
}

void NosonBackend::CompleteServiceLink()
{
  tasks_.Push([this] {
    if (!active_smapi_ || !active_service_)
      return;

    NSROOT::SMOAKeyring::Data auth;
    bool retry = active_smapi_->GetDeviceAuthToken(auth);
    if (retry)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Verknüpfung noch nicht abgeschlossen — bitte im Browser bestätigen und erneut versuchen.";
      error_dispatcher_.emit();
      return;
    }
    if (auth.token.empty() && auth.key.empty())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Verknüpfung fehlgeschlagen.";
      error_dispatcher_.emit();
      active_smapi_.reset();
      active_service_.reset();
      return;
    }

    const std::string type = active_service_->GetServiceType();
    // A fixed serial number, not auth.serialNum (which the AppLink flow
    // mints fresh on every relink) — Gnomos only ever keeps one account per
    // service, and PersistAndRegisterServiceCredentials() purging by type
    // already makes relinking idempotent, but keeping this stable too
    // avoids relying on that purge being the only thing preventing a
    // duplicate.
    PersistAndRegisterServiceCredentials(type, "1", auth.key, auth.token, auth.username);

    // Re-fetch as a properly credentialed clone (GetEnabledServices()
    // attaches the account via Clone()+SetCredentials(), not something the
    // bare catalog entry Init() was originally called with already has —
    // see System::GetEnabledServices() in sonossystem.cpp) and re-init
    // against that before browsing.
    NSROOT::SMServicePtr linked;
    for (const NSROOT::SMServicePtr& candidate : system_->GetEnabledServices())
      if (candidate && candidate->GetServiceType() == type)
        linked = candidate;

    auto smapi = std::make_unique<NSROOT::SMAPI>(*system_);
    if (!linked || !smapi->Init(linked, "en-US") || smapi->AuthTokenExpired())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Verknüpfung gespeichert, aber Dienst konnte nicht gestartet werden.";
      error_dispatcher_.emit();
      return;
    }

    active_smapi_ = std::move(smapi);
    active_service_ = linked;
    CacheActiveServiceSearchCategories();
    BrowseActiveServiceLocked("root");
  });
}

std::vector<LibraryEntry> NosonBackend::GetLibraryEntries() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return library_entries_;
}

void NosonBackend::PlayLibraryItem(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr item;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= library_raw_.size())
        return;
      item = library_raw_[index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    bool ok = false;
    if (NSROOT::System::CanQueueItem(item))
    {
      if (player->PlayQueue(false))
      {
        unsigned pos = player->AddURIToQueue(item, 1);
        ok = pos > 0 && player->SeekTrack(pos) && player->Play();
      }
    }
    else
    {
      EnsureServiceDesc(*system_, item);
      ok = player->SetCurrentURI(item) && player->Play();
    }

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Titel konnte nicht abgespielt werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::AddLibraryItemToQueue(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr item;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= library_raw_.size())
        return;
      item = library_raw_[index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    bool queueable = NSROOT::System::CanQueueItem(item);
    bool ok = queueable && player->AddURIToQueue(item, 0) > 0;

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = queueable ? "Titel konnte nicht zur Warteschlange hinzugefügt werden."
                                  : "Dieser Titel kann nicht zur Warteschlange hinzugefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::PlayLibraryItemNext(unsigned index)
{
  tasks_.Push([this, index] {
    NSROOT::DigitalItemPtr item;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= library_raw_.size())
        return;
      item = library_raw_[index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    bool queueable = NSROOT::System::CanQueueItem(item);
    unsigned insert_pos = player->GetTransportProperty().CurrentTrack + 1;
    bool ok = queueable && player->AddURIToQueue(item, insert_pos) > 0;

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = queueable ? "Titel konnte nicht als nächster Titel eingefügt werden."
                                  : "Dieser Titel kann nicht als nächster Titel eingefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::AddAllLibraryItemsToQueue()
{
  tasks_.Push([this] {
    std::vector<NSROOT::DigitalItemPtr> items;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      items = library_raw_;
    }
    auto player = SnapshotPlayer();
    if (!player || items.empty())
      return;

    unsigned added = 0;
    for (const NSROOT::DigitalItemPtr& item : items)
    {
      if (NSROOT::System::CanQueueItem(item) && player->AddURIToQueue(item, 0) > 0)
        ++added;
    }

    if (added == 0)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Titel konnten nicht zur Warteschlange hinzugefügt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::PlayAllLibraryItemsAsync()
{
  tasks_.Push([this] {
    std::vector<NSROOT::DigitalItemPtr> items;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      items = library_raw_;
    }
    auto player = SnapshotPlayer();
    if (!player || items.empty())
      return;

    // Same PlayQueue(false)-then-populate-then-seek-then-Play() sequence
    // PlayLibraryItem() already uses for a single track — mirrored here
    // rather than reordered, since that sequence is the one actually
    // exercised live against real hardware.
    player->PlayQueue(false);
    if (!player->RemoveAllTracksFromQueue())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Warteschlange konnte nicht geleert werden.";
      error_dispatcher_.emit();
      return;
    }

    unsigned added = 0;
    for (const NSROOT::DigitalItemPtr& item : items)
    {
      if (NSROOT::System::CanQueueItem(item) && player->AddURIToQueue(item, 0) > 0)
        ++added;
    }

    if (added == 0 || !player->SeekTrack(1) || !player->Play())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Titel konnten nicht abgespielt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshQueueAsync();
  });
}

void NosonBackend::DeleteLibraryPlaylist(unsigned index)
{
  tasks_.Push([this, index] {
    std::string object_id;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= library_entries_.size())
        return;
      object_id = library_entries_[index].object_id;
    }
    if (!system_->DestroySavedQueue(object_id))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Playlist konnte nicht gelöscht werden.";
      error_dispatcher_.emit();
    }
    // Unconditional, success or not — same reasoning as the invalidation
    // comment on InvalidateLibraryCache() itself: the real
    // ContentDirectoryChanged event this change also triggers arrives
    // asynchronously and reliably loses the race against GnomosWindow's
    // own immediate BrowseLibraryAsync(library_stack_.back().first) right
    // after this call (queued on the same tasks_ serial worker, so it
    // always runs next) — without this, that re-browse hits the *old*
    // cached "SQ:" listing (up to kLibraryCacheTtl old) instead of
    // fetching fresh, and the deleted entry keeps showing. Confirmed live:
    // this exact race is what made a freshly-added radio station never
    // appear (see DeleteLibraryRadioStation()/AddRadioStation() below).
    InvalidateLibraryCache();
    // GnomosWindow follows up with its own
    // BrowseLibraryAsync(library_stack_.back().first) call right after
    // this one (queued on the same tasks_ serial worker, so this delete
    // always runs first) — see its own comment for why the backend can't
    // do that itself (it has no notion of "the level currently browsed",
    // only GnomosWindow's library_stack_ does).
  });
}

void NosonBackend::DeleteLibraryRadioStation(unsigned index)
{
  tasks_.Push([this, index] {
    std::string object_id;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (index >= library_entries_.size())
        return;
      object_id = library_entries_[index].object_id;
    }
    if (!system_->DestroyRadio(object_id))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Radiosender konnte nicht gelöscht werden.";
      error_dispatcher_.emit();
    }
    // Same "refresh either way" reasoning as DeleteLibraryPlaylist() — see
    // its own comment.
    InvalidateLibraryCache();
  });
}

void NosonBackend::FetchSavedPlaylistsAsync()
{
  tasks_.Push([this] {
    NSROOT::ContentDirectory libraryDirectory(system_->GetHost(), system_->GetPort());
    NSROOT::ContentBrowser browser(libraryDirectory, "SQ:", 200);
    ExhaustBrowser(browser);

    std::vector<LibraryEntry> entries;
    entries.reserve(browser.table().size());
    for (const auto& item : browser.table())
    {
      LibraryEntry entry;
      entry.object_id = item->GetObjectID();
      entry.title = item->GetValue("dc:title");
      entry.is_container = item->IsContainer();
      entries.push_back(std::move(entry));
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      saved_playlists_ = std::move(entries);
    }
    saved_playlists_dispatcher_.emit();
  });
}

std::vector<LibraryEntry> NosonBackend::GetSavedPlaylists() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return saved_playlists_;
}

void NosonBackend::AddLibraryItemToPlaylist(unsigned library_index, const std::string& playlist_object_id)
{
  tasks_.Push([this, library_index, playlist_object_id] {
    NSROOT::DigitalItemPtr item;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (library_index >= library_raw_.size())
        return;
      item = library_raw_[library_index];
    }
    auto player = SnapshotPlayer();
    if (!player)
      return;

    // AddURIToSavedQueue() needs the target playlist's own current
    // containerUpdateID — a single-item Browse() just to read it back
    // fresh, rather than trusting a value from whenever the playlist was
    // last browsed (which might not even be this session, or might be
    // stale if it changed since). Same reasoning
    // ReorderLibraryPlaylistTrack() below applies for its own call.
    NSROOT::ContentDirectory libraryDirectory(system_->GetHost(), system_->GetPort());
    NSROOT::ContentBrowser browser(libraryDirectory, playlist_object_id, 1);
    bool queueable = NSROOT::System::CanQueueItem(item);
    bool ok = queueable && browser.Browse(0, 1) &&
              player->AddURIToSavedQueue(playlist_object_id, item, browser.GetUpdateID()) > 0;
    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = queueable ? "Titel konnte nicht zur Playlist hinzugefügt werden."
                                  : "Dieser Titel kann nicht zu einer Playlist hinzugefügt werden.";
      error_dispatcher_.emit();
    }
    else
    {
      // See DeleteLibraryPlaylist()'s own comment for why this can't wait
      // for the real ContentDirectoryChanged event — a later visit to
      // playlist_object_id within the cache TTL would otherwise miss the
      // track just added.
      InvalidateLibraryCache();
    }
  });
}

void NosonBackend::ReorderLibraryPlaylistTrack(const std::string& playlist_object_id, unsigned from, unsigned to)
{
  tasks_.Push([this, playlist_object_id, from, to] {
    auto player = SnapshotPlayer();
    if (!player)
      return;

    NSROOT::ContentDirectory libraryDirectory(system_->GetHost(), system_->GetPort());
    NSROOT::ContentBrowser browser(libraryDirectory, playlist_object_id, 1);
    // Same 1-based UPnP position convention as ReorderTracksInQueue(), and
    // the same "moving forward needs the target shifted by one" reasoning
    // — see ReorderQueueItem()'s own comment.
    unsigned insert_before = to;
    if (from < to)
      ++insert_before;
    bool ok = browser.Browse(0, 1) &&
              player->ReorderTracksInSavedQueue(playlist_object_id, std::to_string(from + 1),
                                                 std::to_string(insert_before + 1), browser.GetUpdateID());
    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Playlist konnte nicht umsortiert werden.";
      error_dispatcher_.emit();
    }
    // Same reasoning as DeleteLibraryPlaylist()'s own comment — without
    // this, GnomosWindow's own immediate follow-up
    // BrowseLibraryAsync(playlist_object_id) (queued right after this call
    // on the same serial tasks_ worker) would hit the pre-reorder cached
    // listing instead of fetching the new order.
    InvalidateLibraryCache();
  });
}

void NosonBackend::RefreshLibraryIndex()
{
  tasks_.Push([this] {
    if (!system_->RefreshShareIndex())
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Bibliotheks-Scan konnte nicht gestartet werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::AddRadioStation(const std::string& title, const std::string& stream_url,
                                    const std::string& favicon_url)
{
  tasks_.Push([this, title, stream_url, favicon_url] {
    if (!system_->CreateRadio(stream_url, title))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Radiosender konnte nicht hinzugefügt werden.";
      error_dispatcher_.emit();
    }
    else
    {
      // See DeleteLibraryPlaylist()'s own comment — reported live: without
      // this, a freshly-added station never showed up, since
      // GnomosWindow's own immediate follow-up BrowseLibraryAsync("R:0/0")
      // (queued right after this call on the same serial tasks_ worker)
      // hit the cached pre-add "R:0/0" listing instead of fetching fresh.
      InvalidateLibraryCache();
      // See AddRadioStation()'s own header comment for why this can't go
      // to Sonos itself — only worth persisting once the station actually
      // exists, and only if the caller (the radio-browser search results;
      // the manual entry fallback has no favicon to offer) actually has one.
      if (!favicon_url.empty())
        SaveRadioFavicon(stream_url, favicon_url);
    }
  });
}

void NosonBackend::SaveRadioFavicon(const std::string& stream_url, const std::string& favicon_url)
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(RadioFaviconsPath());
  }
  catch (const Glib::Error&)
  {
    // fine — first custom station with a favicon this install has ever added
  }
  keyfile->set_string("favicons", RadioStreamMatchKey(stream_url), favicon_url);
  try
  {
    keyfile->save_to_file(RadioFaviconsPath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — the station still got added, it just won't have a
    // thumbnail next time "R:0/0" is browsed
  }
}

std::map<std::string, std::string> NosonBackend::LoadRadioFavicons() const
{
  std::map<std::string, std::string> result;
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (keyfile->load_from_file(RadioFaviconsPath()))
      for (const Glib::ustring& key : keyfile->get_keys("favicons"))
        result[key] = keyfile->get_string("favicons", key);
  }
  catch (const Glib::Error&)
  {
    // no favicons saved yet, or the file is otherwise unreadable — same
    // as "none saved" either way
  }
  return result;
}

void NosonBackend::RefreshAlarmsAsync()
{
  tasks_.Push([this] {
    NSROOT::AlarmList alarms = system_->GetAlarmList();

    // Alarm::GetRoomUUID() is a bare player uuid; resolve it to a display
    // name via the current topology rather than showing raw uuids.
    std::map<std::string, std::string> room_names;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (const auto& kv : zones_by_uuid_)
        for (const NSROOT::ZonePlayerPtr& zp : *kv.second)
          room_names[zp->GetUUID()] = *zp;
    }

    std::vector<AlarmInfo> items;
    items.reserve(alarms.size());
    for (const NSROOT::AlarmPtr& alarm : alarms)
    {
      if (!alarm)
        continue;
      AlarmInfo info;
      info.id = alarm->GetId();
      info.enabled = alarm->GetEnabled();
      info.start_time = alarm->GetStartLocalTime();
      info.recurrence = alarm->GetRecurrence();
      info.volume = alarm->GetVolume();
      info.include_linked_zones = alarm->GetIncludeLinkedZones();
      unsigned hh = 0, hm = 0, hs = 0;
      if (std::sscanf(alarm->GetDuration().c_str(), "%u:%u:%u", &hh, &hm, &hs) == 3)
        info.duration_minutes = hh * 60 + hm;
      info.shuffle = alarm->GetPlayMode() == "SHUFFLE" || alarm->GetPlayMode() == "SHUFFLE_NOREPEAT";
      info.room_uuid = alarm->GetRoomUUID();
      auto it = room_names.find(info.room_uuid);
      info.room_name = (it != room_names.end()) ? it->second : info.room_uuid;
      items.push_back(std::move(info));
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      alarms_ = std::move(items);
    }
    alarms_dispatcher_.emit();
  });
}

std::vector<AlarmInfo> NosonBackend::GetAlarms() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  return alarms_;
}

std::vector<std::string> NosonBackend::GetAlarmSoundTitles() const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::vector<std::string> titles;
  titles.reserve(favorites_.size() + 1);
  titles.push_back("Wecker-Ton");
  for (const FavoriteItem& fav : favorites_)
    titles.push_back(fav.title.empty() ? "Unbenannter Favorit" : fav.title);
  return titles;
}

void NosonBackend::SetAlarmEnabled(const std::string& alarm_id, bool enabled)
{
  tasks_.Push([this, alarm_id, enabled] {
    NSROOT::AlarmList alarms = system_->GetAlarmList();
    for (const NSROOT::AlarmPtr& alarm : alarms)
    {
      if (alarm && alarm->GetId() == alarm_id)
      {
        alarm->SetEnabled(enabled);
        if (system_->UpdateAlarm(*alarm))
        {
          RefreshAlarmsAsync();
        }
        else
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          pending_error_ = "Alarm konnte nicht aktualisiert werden.";
          error_dispatcher_.emit();
        }
        return;
      }
    }
  });
}

void NosonBackend::SetAlarmIncludeLinkedZones(const std::string& alarm_id, bool include)
{
  tasks_.Push([this, alarm_id, include] {
    NSROOT::AlarmList alarms = system_->GetAlarmList();
    for (const NSROOT::AlarmPtr& alarm : alarms)
    {
      if (alarm && alarm->GetId() == alarm_id)
      {
        alarm->SetIncludeLinkedZones(include);
        if (system_->UpdateAlarm(*alarm))
        {
          RefreshAlarmsAsync();
        }
        else
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          pending_error_ = "Alarm konnte nicht aktualisiert werden.";
          error_dispatcher_.emit();
        }
        return;
      }
    }
  });
}

void NosonBackend::DeleteAlarm(const std::string& alarm_id)
{
  tasks_.Push([this, alarm_id] {
    if (system_->DestroyAlarm(alarm_id))
    {
      RefreshAlarmsAsync();
    }
    else
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Alarm konnte nicht gelöscht werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::PreviewAlarmSound(const std::string& room_uuid, unsigned sound_index)
{
  tasks_.Push([this, room_uuid, sound_index] {
    constexpr int kAlarmPreviewSeconds = 8;
    if (sound_index == 0 || sound_index == kKeepExistingAlarmSound)
      return;

    NSROOT::DigitalItemPtr favorite;
    NSROOT::ZonePlayerPtr target;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      size_t index = sound_index - 1;
      if (index < favorites_raw_.size())
        favorite = favorites_raw_[index];
      target = FindZonePlayer(zones_by_uuid_, room_uuid);
    }
    if (!favorite || !target)
      return;

    NSROOT::DigitalItemPtr item;
    bool ok = false;
    if (NSROOT::System::ExtractObjectFromFavorite(favorite, item) && item)
    {
      EnsureServiceDesc(*system_, item);
      NSROOT::Player roomPlayer(target);
      ok = roomPlayer.SetCurrentURI(item) && roomPlayer.Play();
      if (ok)
      {
        // A genuine preview, not something left playing until the user
        // manually stops it — sleeping right here on this same
        // TaskQueue task (rather than a separate timer) means any other
        // action the user takes on this room meanwhile simply queues up
        // behind this one and runs after, the same ordering guarantee
        // every other TaskQueue task already relies on, instead of a
        // cross-thread timer that could fire after the room moved on to
        // playing something else entirely.
        std::this_thread::sleep_for(std::chrono::seconds(kAlarmPreviewSeconds));
        roomPlayer.Stop();
      }
    }

    if (!ok)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Wecker-Ton konnte nicht getestet werden.";
      error_dispatcher_.emit();
    }
  });
}

void NosonBackend::ApplyAlarmSound(NSROOT::Alarm& alarm, unsigned sound_index)
{
  if (sound_index == kKeepExistingAlarmSound)
    return;  // alarm already has whatever ProgramURI/ProgramMetadata it came in with

  if (sound_index == 0)
  {
    // Alarm()'s own default — kept explicit since UpdateAlarmSchedule()
    // reuses this on an alarm that might currently have something else set.
    alarm.SetProgramURI(ALARM_BUZZER_URI);
    alarm.SetProgramMetadata(NSROOT::DigitalItemPtr());
    return;
  }

  NSROOT::DigitalItemPtr favorite;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    size_t index = sound_index - 1;
    if (index < favorites_raw_.size())
      favorite = favorites_raw_[index];
  }

  // A favorite wraps its actual playable item in metadata — same unwrap
  // PlayFavorite() does (see its comment), needed here for the same reason:
  // the wrapper's own "res"/metadata isn't directly playable.
  NSROOT::DigitalItemPtr item;
  if (favorite && NSROOT::System::ExtractObjectFromFavorite(favorite, item) && item)
  {
    alarm.SetProgramURI(item->GetValue("res"));
    alarm.SetProgramMetadata(item);
  }
  else
  {
    alarm.SetProgramURI(ALARM_BUZZER_URI);
    alarm.SetProgramMetadata(NSROOT::DigitalItemPtr());
  }
}

void NosonBackend::CreateAlarm(const std::string& room_uuid, int hour, int minute, const std::vector<int>& days,
                                uint8_t volume, unsigned sound_index, unsigned duration_minutes, bool shuffle)
{
  tasks_.Push([this, room_uuid, hour, minute, days, volume, sound_index, duration_minutes, shuffle] {
    NSROOT::Alarm alarm;
    alarm.SetEnabled(true);  // Alarm()'s own default is disabled
    alarm.SetRoomUUID(room_uuid);
    ApplyAlarmSound(alarm, sound_index);

    char start_time[9];
    std::snprintf(start_time, sizeof(start_time), "%02d:%02d:00", hour, minute);
    alarm.SetStartLocalTime(start_time);
    char duration[16];
    std::snprintf(duration, sizeof(duration), "%02u:%02u:00", duration_minutes / 60, duration_minutes % 60);
    alarm.SetDuration(duration);  // Alarm() leaves this empty
    alarm.SetPlayMode(shuffle ? NSROOT::PlayMode_SHUFFLE_NOREPEAT : NSROOT::PlayMode_NORMAL);

    std::string recurrence;
    for (int day : days)
    {
      if (day < 0 || day > 6)
        continue;
      if (!recurrence.empty())
        recurrence += ",";
      recurrence += NSROOT::DayTable[day];
    }
    alarm.SetRecurrence(recurrence);
    alarm.SetVolume(volume);

    if (!system_->CreateAlarm(alarm))
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pending_error_ = "Alarm konnte nicht erstellt werden.";
      error_dispatcher_.emit();
      return;
    }
    RefreshAlarmsAsync();
  });
}

void NosonBackend::UpdateAlarmSchedule(const std::string& alarm_id, const std::string& room_uuid, int hour,
                                        int minute, const std::vector<int>& days, uint8_t volume,
                                        unsigned sound_index, unsigned duration_minutes, bool shuffle)
{
  tasks_.Push([this, alarm_id, room_uuid, hour, minute, days, volume, sound_index, duration_minutes, shuffle] {
    NSROOT::AlarmList alarms = system_->GetAlarmList();
    for (const NSROOT::AlarmPtr& alarm : alarms)
    {
      if (!alarm || alarm->GetId() != alarm_id)
        continue;

      alarm->SetRoomUUID(room_uuid);
      ApplyAlarmSound(*alarm, sound_index);
      char start_time[9];
      std::snprintf(start_time, sizeof(start_time), "%02d:%02d:00", hour, minute);
      alarm->SetStartLocalTime(start_time);
      char duration[16];
      std::snprintf(duration, sizeof(duration), "%02u:%02u:00", duration_minutes / 60, duration_minutes % 60);
      alarm->SetDuration(duration);
      alarm->SetPlayMode(shuffle ? NSROOT::PlayMode_SHUFFLE_NOREPEAT : NSROOT::PlayMode_NORMAL);

      std::string recurrence;
      for (int day : days)
      {
        if (day < 0 || day > 6)
          continue;
        if (!recurrence.empty())
          recurrence += ",";
        recurrence += NSROOT::DayTable[day];
      }
      alarm->SetRecurrence(recurrence);
      alarm->SetVolume(volume);

      if (system_->UpdateAlarm(*alarm))
      {
        RefreshAlarmsAsync();
      }
      else
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_error_ = "Alarm konnte nicht aktualisiert werden.";
        error_dispatcher_.emit();
      }
      return;
    }
  });
}

std::string NosonBackend::ResolveArtUri(const std::string& uri) const
{
  if (uri.empty() || uri.compare(0, 4, "http") == 0)
    return uri;
  if (!player_)
    return uri;
  std::string base = "http://" + player_->GetHost() + ":" + std::to_string(player_->GetPort());
  if (uri.front() != '/')
    base += "/";
  return base + uri;
}

void NosonBackend::RefreshNowPlayingLocked()
{
  NSROOT::AVTProperty prop = player_->GetTransportProperty();

  // Identifies the playing item and stays stable across a play/pause/stop
  // of the *same* track (which also fires this refresh, via
  // SVCEvent_TransportChanged) — unlike TransportState. Only an actual
  // track change should snap the position display back to 0; a bare
  // pause/resume must leave it where it was. CurrentTrack (the queue
  // index) is included alongside CurrentTrackURI: a manual skip (Next())
  // was observed live not resetting position, which points at
  // CurrentTrackURI alone sometimes staying identical across consecutive
  // queue entries (plausible for streaming-service tracks whose URI
  // encodes little beyond the service/session, not the specific track) —
  // CurrentTrack always advances by definition, so combining both is the
  // safer identity check.
  std::string track_key = std::to_string(prop.CurrentTrack) + "|" + prop.CurrentTrackURI;
  if (track_key != current_track_key_)
  {
    current_track_key_ = track_key;
    position_ = 0;
  }

  NowPlaying np;
  np.valid = true;
  np.state = ParseTransportState(prop.TransportState);
  np.shuffle = (prop.CurrentPlayMode == "SHUFFLE" || prop.CurrentPlayMode == "SHUFFLE_NOREPEAT");
  np.repeat = prop.CurrentPlayMode == "REPEAT_ONE" ? RepeatMode::One
              : (prop.CurrentPlayMode == "REPEAT_ALL" || prop.CurrentPlayMode == "SHUFFLE") ? RepeatMode::All
                                                                                             : RepeatMode::Off;
  // Comma-separated capability list (e.g. "SHUFFLE,REPEAT,CROSSFADE"), empty
  // for a source that supports neither (radio, line-in). Absence of the
  // field entirely (defaults true) would wrongly enable both, so this is an
  // explicit substring check, not just "field is non-empty".
  np.shuffle_supported = prop.r_CurrentValidPlayModes.find("SHUFFLE") != std::string::npos;
  np.repeat_supported = prop.r_CurrentValidPlayModes.find("REPEAT") != std::string::npos;
  np.can_go_next = prop.CurrentTransportActions.find("Next") != std::string::npos;
  np.can_go_previous = prop.CurrentTransportActions.find("Previous") != std::string::npos;
  np.can_pause = prop.CurrentTransportActions.find("Pause") != std::string::npos;
  np.transport_status_ok = prop.TransportStatus.empty() || prop.TransportStatus == "OK";
  np.alarm_running = prop.r_AlarmRunning == "1";
  // CurrentTrack/AVTransportURI are unreliable for a brief moment during a
  // track change — TransportState == TRANSITIONING is UPnP's own signal
  // that this snapshot is mid-update and nothing derived from it should be
  // trusted yet. Confirmed live: even after guarding against a literal
  // CurrentTrack == 0, the queue highlight still flashed onto track 1
  // during a skip, meaning the device can report some other transient
  // (non-zero, still wrong) value while TRANSITIONING — so gate on the
  // transport state itself instead of guessing at every value it might
  // transiently report.
  np.playing_from_queue = np.state != TransportState::Transitioning && prop.CurrentTrack > 0 &&
                           prop.AVTransportURI.compare(0, 15, "x-rincon-queue:") == 0;
  np.current_queue_index = prop.CurrentTrack > 0 ? prop.CurrentTrack - 1 : 0;

  // Sonos duration format is "H:MM:SS"; zero (or unparseable) means a live
  // stream (radio, line-in, ...) rather than a queued track — mirrors
  // noson-app's own postulate in player.cpp, updateAVTransport(). For a
  // stream, CurrentTrackMetaData's dc:title is unreliable (confirmed live:
  // it showed the raw stream URI for internet radio); the URI's own title
  // — what was set when starting playback — is what noson-app itself
  // relies on instead.
  unsigned hh = 0, hm = 0, hs = 0;
  bool has_duration =
      std::sscanf(prop.CurrentTrackDuration.c_str(), "%u:%u:%u", &hh, &hm, &hs) == 3 && (hh || hm || hs);
  np.duration = has_duration ? hh * 3600 + hm * 60 + hs : 0;

  if (!has_duration)
  {
    std::string uri_title;
    if (prop.r_EnqueuedTransportURIMetaData)
      uri_title = prop.r_EnqueuedTransportURIMetaData->GetValue("dc:title");

    if (prop.TransportState == "TRANSITIONING")
    {
      np.title = uri_title;
    }
    else
    {
      np.title = (uri_title.empty() && prop.CurrentTrackMetaData) ? prop.CurrentTrackMetaData->GetValue("dc:title")
                                                                    : uri_title;
      if (prop.CurrentTrackMetaData)
      {
        // "Now playing" info a station reports for its current song/show,
        // when available (often empty).
        std::string content = prop.CurrentTrackMetaData->GetValue("r:streamContent");
        if (content.empty())
        {
          std::string show = prop.CurrentTrackMetaData->GetValue("r:radioShowMd");
          content = show.substr(0, show.find_last_of(","));
        }
        np.artist = content;
      }
    }
    if (prop.CurrentTrackMetaData)
      np.art_uri = ResolveArtUri(prop.CurrentTrackMetaData->GetValue("upnp:albumArtURI"));
    if (np.art_uri.empty())
    {
      // A custom radio station has no upnp:albumArtURI from Sonos itself
      // at all (System::CreateRadio() has no icon parameter — see
      // SaveRadioFavicon()'s own comment) — confirmed live: switching to
      // one showed the generic fallback in PlayerBar instead of its own
      // thumbnail, the same gap BrowseLibraryAsync()'s "R:0/0" branch
      // already closes for the library listing itself. Player::SetCurrentURI()
      // passes item->GetValue("res") straight through to
      // AVTransport::SetCurrentURI(), so AVTransportURI read back here is
      // exactly that same value — matched against radio-favicons.ini the
      // same way, via the same RadioStreamMatchKey() (the x-rincon-mp3radio:
      // scheme-prefix rewrite CreateRadio() does applies here too).
      std::map<std::string, std::string> radio_favicons = LoadRadioFavicons();
      auto it = radio_favicons.find(RadioStreamMatchKey(prop.AVTransportURI));
      if (it != radio_favicons.end())
        np.art_uri = it->second;
    }
  }
  else if (prop.CurrentTrackMetaData)
  {
    np.title = prop.CurrentTrackMetaData->GetValue("dc:title");
    np.artist = prop.CurrentTrackMetaData->GetValue("dc:creator");
    np.album = prop.CurrentTrackMetaData->GetValue("upnp:album");
    np.art_uri = ResolveArtUri(prop.CurrentTrackMetaData->GetValue("upnp:albumArtURI"));
  }

  now_playing_ = std::move(np);
}

void NosonBackend::RefreshVolumeLocked()
{
  if (!player_)
    return;
  // Group average, not just the coordinator's own volume — see SetVolume()'s
  // comment. A group counts as muted only once *every* (non-fixed-output)
  // member is muted, matching noson-app's own "exists active audio in
  // group" check.
  unsigned sum = 0, count = 0;
  bool any_unmuted = false;
  room_volumes_.clear();
  for (const NSROOT::SRProperty& srp : player_->GetRenderingProperty())
  {
    if (srp.property.OutputFixed)
      continue;
    sum += static_cast<unsigned>(srp.property.VolumeMaster);
    ++count;
    if (!srp.property.MuteMaster)
      any_unmuted = true;
    room_volumes_[srp.uuid] = static_cast<uint8_t>(srp.property.VolumeMaster);
  }
  if (count > 0)
  {
    volume_.volume = static_cast<uint8_t>(sum / count);
    volume_.muted = !any_unmuted;
  }
}

bool NosonBackend::GetRoomVolume(const std::string& player_uuid, uint8_t& out_volume) const
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = room_volumes_.find(player_uuid);
  if (it == room_volumes_.end())
    return false;
  out_volume = it->second;
  return true;
}

void NosonBackend::SetRoomVolume(const std::string& player_uuid, uint8_t value)
{
  // Debounced per-room, same reasoning as SetVolume() above — the
  // grouping popover has one of these sliders per room, each needing its
  // own independent debounce timer.
  room_volume_debounce_connections_[player_uuid].disconnect();
  room_volume_debounce_connections_[player_uuid] = Glib::signal_timeout().connect(
      [this, player_uuid, value] {
        ApplyRoomVolumeAsync(player_uuid, value);
        return false;  // one-shot
      },
      150);
}

void NosonBackend::ApplyRoomVolumeAsync(const std::string& player_uuid, uint8_t value)
{
  tasks_.Push([this, player_uuid, value] {
    auto player = SnapshotPlayer();
    if (!player)
      return;
    player->SetVolume(player_uuid, value);
  });
}

void NosonBackend::OnSystemEvent(void* handle)
{
  static_cast<NosonBackend*>(handle)->system_dispatcher_.emit();
}

void NosonBackend::OnPlayerEvent(void* handle)
{
  static_cast<NosonBackend*>(handle)->player_dispatcher_.emit();
}

void NosonBackend::RefreshGen1StatusAsync()
{
  tasks_.Push([this] {
    std::vector<std::pair<std::string, std::string>> to_fetch;  // (uuid, device_description.xml URL)
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (const auto& kv : zones_by_uuid_)
        for (const NSROOT::ZonePlayerPtr& zp : *kv.second)
          if (model_number_by_uuid_.find(zp->GetUUID()) == model_number_by_uuid_.end())
            to_fetch.push_back({zp->GetUUID(), zp->GetLocation()});
    }
    if (to_fetch.empty())
      return;

    bool any_resolved = false;
    for (const auto& [uuid, location] : to_fetch)
    {
      if (location.empty())
        continue;
      try
      {
        auto file = Gio::File::create_for_uri(location);
        char* contents = nullptr;
        gsize length = 0;
        if (!file->load_contents(contents, length) || !contents)
          continue;
        std::string xml(contents, length);
        g_free(contents);

        static const std::string kOpenTag = "<modelNumber>";
        size_t start = xml.find(kOpenTag);
        if (start == std::string::npos)
          continue;
        start += kOpenTag.size();
        size_t end = xml.find("</modelNumber>", start);
        if (end == std::string::npos)
          continue;

        std::lock_guard<std::mutex> lock(state_mutex_);
        model_number_by_uuid_[uuid] = xml.substr(start, end - start);
        any_resolved = true;
      }
      catch (const Glib::Error&)
      {
        // Left unset — retried on the next topology change rather than
        // permanently cached as "not gen1" from a transient network hiccup.
      }
    }
    if (any_resolved)
      signal_zones_changed_.emit();
  });
}

void NosonBackend::HandleSystemEvent()
{
  unsigned char mask = system_->LastEvents();
  if (mask & NSROOT::SVCEvent_ZGTopologyChanged)
  {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      zones_by_uuid_.clear();
      for (const auto& kv : system_->GetZoneList())
        zones_by_uuid_[kv.first] = kv.second;
      // room_volumes_ (backing the grouping popover's own per-room
      // sliders) is otherwise only refreshed by a real
      // SVCEvent_RenderingControlChanged — not guaranteed to fire just
      // because group membership changed, and reported live as not
      // reliably doing so: a room newly joined to the selected zone had
      // no volume slider at all until something else happened to also
      // trigger a volume refresh. player_ may be null this early (no
      // zone selected yet) — RefreshVolumeLocked() already handles that
      // itself, same guard every other caller relies on.
      RefreshVolumeLocked();
    }
    signal_zones_changed_.emit();
    signal_volume_changed_.emit();
    RefreshGen1StatusAsync();
  }
  if (mask & NSROOT::SVCEvent_ContentDirectoryChanged)
  {
    RefreshFavoritesAsync();
    // Runs on the worker thread (tasks_.Push()), not directly here —
    // library_cache_ is worker-thread-only, same as active_smapi_ (see
    // its own declaration comment). A real household library change
    // (new music scanned, a playlist edited elsewhere) shouldn't keep
    // showing a stale cached level until the TTL happens to expire.
    tasks_.Push([this] { InvalidateLibraryCache(); });
  }
  if (mask & NSROOT::SVCEvent_AlarmClockChanged)
    RefreshAlarmsAsync();
  // (This ContentDirectoryChanged is the household-wide one, e.g. a
  // favorite added/removed elsewhere; distinct from the per-player one
  // handled in HandlePlayerEvent(), which is the current zone's queue.)
}

void NosonBackend::HandlePlayerEvent()
{
  // player_ can be reassigned by SelectZone() on the TaskQueue worker
  // thread at any time, so it (and LastEvents(), which depends on which
  // player_ is current) must only ever be touched under state_mutex_ here
  // — unlike system_, which is set once in the constructor and never
  // reassigned, so HandleSystemEvent doesn't need the same care.
  bool now_playing_dirty = false;
  bool volume_dirty = false;
  bool content_dirty = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!player_)
      return;
    unsigned char mask = player_->LastEvents();
    if (mask & NSROOT::SVCEvent_TransportChanged)
    {
      RefreshNowPlayingLocked();
      now_playing_dirty = true;
    }
    if (mask & NSROOT::SVCEvent_RenderingControlChanged)
    {
      RefreshVolumeLocked();
      volume_dirty = true;
    }
    // See last_zone_select_at_'s own comment — swallows the redundant
    // "initial state" ContentDirectoryChanged echo GENA always delivers
    // right after (re)subscribing to a freshly selected player, without
    // suppressing a genuine mid-session queue change.
    constexpr auto kZoneSelectGracePeriod = std::chrono::seconds(2);
    content_dirty = (mask & NSROOT::SVCEvent_ContentDirectoryChanged) &&
                     (std::chrono::steady_clock::now() - last_zone_select_at_ > kZoneSelectGracePeriod);
  }

  if (now_playing_dirty)
  {
    signal_now_playing_changed_.emit();
    // Cheap even when the track didn't actually change (RefreshNowPlayingLocked
    // only resets position_ on a real track change) — just re-delivers the
    // same value to the UI in that case.
    signal_position_changed_.emit();
  }
  if (volume_dirty)
    signal_volume_changed_.emit();
  if (content_dirty)
    RefreshQueueAsync();
}

void NosonBackend::HandleDiscoveryDone()
{
  bool ok;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ok = last_discovery_ok_;
  }
  signal_zones_changed_.emit();
  signal_discovery_done_.emit(ok);
  if (ok)
  {
    RefreshFavoritesAsync();
    RefreshAlarmsAsync();
  }
}

void NosonBackend::HandlePlayerReady()
{
  signal_player_ready_.emit();
  signal_now_playing_changed_.emit();
  signal_volume_changed_.emit();
  RefreshSleepTimerAsync();
  RefreshSoundSettingsAsync();
}

void NosonBackend::HandleError()
{
  std::string message;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    message = pending_error_;
  }
  signal_error_.emit(message);
}

}  // namespace gnomos
