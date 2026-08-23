// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <giomm/dbusconnection.h>
#include <glibmm/dispatcher.h>
#include <sigc++/sigc++.h>

#include <smapi.h>
#include <sonosplayer.h>
#include <sonossystem.h>
#include <sonoszone.h>

#include "noson-types.h"
#include "task-queue.h"

namespace gnomos
{

// GTK-safe wrapper around libnoson's System/Player.
//
// libnoson spawns its own worker threads and invokes event callbacks on
// them (noson/src/eventhandler.cpp: one thread per subscription), and every
// action it exposes blocks the calling thread with a real HTTP round-trip.
// NosonBackend hides both facts: write actions run on a background
// TaskQueue, and every libnoson event callback does nothing but wake a
// Glib::Dispatcher, which re-enters on the GTK main thread before touching
// any cached state or emitting a signal. Code outside this class should
// never touch NSROOT:: directly.
class NosonBackend
{
public:
  NosonBackend();
  ~NosonBackend();

  NosonBackend(const NosonBackend&) = delete;
  NosonBackend& operator=(const NosonBackend&) = delete;

  // Runs SSDP discovery in the background (libnoson blocks for up to ~5s).
  // signal_discovery_done() fires on the main thread with the result.
  void DiscoverAsync();

  std::vector<ZoneInfo> Zones() const;
  std::vector<RoomInfo> Rooms() const;

  // Makes the room join the currently selected zone's group (as a
  // satellite of that zone's coordinator), or leave whatever group it's
  // currently in to become standalone again. Both are fire-and-forget:
  // the resulting topology change arrives as a normal ZGTopologyChanged
  // event and updates Zones()/Rooms() the same way any other change would.
  void JoinRoomToCurrentZone(const std::string& room_player_uuid);
  void RemoveRoomFromGroup(const std::string& room_player_uuid);

  // Selects the zone to control. Building the Player subscribes to that
  // zone's AVTransport/RenderingControl/ContentDirectory events, which
  // libnoson does synchronously, so this runs on the TaskQueue too;
  // signal_player_ready() fires on the main thread once state is usable.
  void SelectZone(const std::string& coordinator_uuid);

  bool HasPlayer() const;

  NowPlaying GetNowPlaying() const;
  VolumeInfo GetVolume() const;
  std::vector<QueueItem> GetQueue() const;

  void Play();
  void PauseOrStop();
  void Next();
  void Previous();
  // Debounced — see the .cpp. Dragging the volume slider fires this many
  // times a second; without debouncing, every intermediate step queued
  // its own full read-every-member-then-scale-every-member round trip,
  // leaving the device visibly lagging behind the slider for a while
  // after the user had already stopped dragging.
  void SetVolume(uint8_t value);
  void SetMuted(bool muted);
  // Per-member volume within the current group, for the grouping popover's
  // own sliders — unlike SetVolume() above, this targets exactly one room,
  // no proportional scaling of the rest. Only meaningful for a room that's
  // actually a member of the *currently selected* zone (its RenderingControl
  // state is only known from player_'s own subscription — see
  // RefreshVolumeLocked()); returns false for any other uuid. Fixed-output
  // members (line-out to a fixed-volume amp) are never present here, same
  // as they're skipped in SetVolume()'s own scaling.
  bool GetRoomVolume(const std::string& player_uuid, uint8_t& out_volume) const;
  // Also debounced per-room, same reasoning as SetVolume() above — the
  // grouping popover has one of these sliders per room.
  void SetRoomVolume(const std::string& player_uuid, uint8_t value);
  // Both read the current AVTProperty::CurrentPlayMode and cycle it.
  // ToggleRepeat() cycles Off -> All -> One -> Off while not shuffling
  // (noson-app's own Player::toggleRepeat(), player.cpp, only ever
  // toggles Off/All — repeat-one is a Gnomos addition); while shuffling,
  // both still just flip the SHUFFLE/SHUFFLE_NOREPEAT pair, since Sonos
  // has no shuffle+repeat-one combination — see RepeatMode's own comment.
  void ToggleShuffle();
  void ToggleRepeat();
  // Absolute-value counterparts to the two toggles above, for callers that
  // want to set a specific state rather than cycle it (MPRIS's Shuffle/
  // LoopStatus properties are read-write, not "toggle" actions).
  void SetShuffle(bool shuffle);
  void SetRepeatMode(RepeatMode mode);
  // Adds whatever is currently playing to Favorites (System::AddURIToFavorites,
  // which internally attaches a <desc> service token itself, same as
  // Favorites created via the Sonos app — no EnsureServiceDesc() needed
  // here). Uses CurrentTrackMetaData for a queued track, or
  // r_EnqueuedTransportURIMetaData for a live stream (duration == 0) — the
  // same distinction RefreshNowPlayingLocked() makes for the title. Surfaces
  // failure via signal_error(); on success there's no dedicated signal, same
  // as every other write action here — the new favorite shows up via the
  // household's own ContentDirectoryChanged event, like any other Favorites
  // change.
  void AddCurrentTrackToFavorites();

  // Switches the current zone's coordinator to its own local input. Not
  // every first-generation model has both (or either) physical input —
  // there's no capability flag for this in the protocol, so an
  // unsupported call just surfaces as the usual signal_error().
  void PlayLineIn();
  void PlayDigitalIn();

  // 0 cancels an active sleep timer.
  void SetSleepTimer(unsigned seconds);
  void RefreshSleepTimerAsync();
  SleepTimerInfo GetSleepTimerInfo() const;

  void RefreshSoundSettingsAsync();
  SoundSettings GetSoundSettings() const;
  void SetBass(int8_t value);
  void SetTreble(int8_t value);
  void SetLoudness(bool enabled);
  void SetNightmode(bool enabled);
  // Write-only, like PlayLineIn()/PlayDigitalIn() — libnoson has no
  // GetLEDState() to show a current value with.
  void SetLedState(bool enabled);

  void RefreshQueueAsync();
  void PlayQueueItem(unsigned index);
  void RemoveQueueItem(unsigned index);
  void ClearQueue();
  void SaveQueueAsPlaylist(const std::string& title);
  // Both indices are 0-based positions in the last-refreshed queue (same
  // convention as PlayQueueItem()/RemoveQueueItem()).
  void ReorderQueueItem(unsigned from, unsigned to);

  // Favorites are household-wide (served by the originally discovered
  // device), independent of which zone is currently selected — unlike the
  // queue, PlayFavorite() still needs a selected zone to play *into*.
  void RefreshFavoritesAsync();
  std::vector<FavoriteItem> GetFavorites() const;
  void PlayFavorite(unsigned index);
  // Appends without interrupting current playback (AVTransport::AddURIToQueue,
  // position 0 = append at end). Fails gracefully — same non-queueable
  // stream case PlayFavorite() falls back to SetCurrentURI() for — when the
  // favorite is a live stream (radio, line-in, ...), since there's nothing
  // sensible to append there.
  void AddFavoriteToQueue(unsigned index);
  // Inserts right after the currently playing track (AVTransport::AddURIToQueue,
  // position AVTProperty::CurrentTrack + 1) without interrupting playback —
  // same non-queueable-stream limitation as AddFavoriteToQueue().
  void PlayFavoriteNext(unsigned index);
  // System::DestroyFavorite() against favorites_raw_[index]'s own real
  // ObjectID (this is a genuine ContentDirectory browse result, unlike the
  // unwrapped playable item PlayFavorite() extracts from it).
  void DeleteFavorite(unsigned index);

  // object_id == "" browses the fixed, local root categories (no network
  // call); anything else is a real objectID browsed against the household
  // ContentDirectory (works for both the root category ids themselves,
  // e.g. "A:ALBUM", and any deeper objectID a returned LibraryEntry carries).
  void BrowseLibraryAsync(const std::string& object_id);
  std::vector<LibraryEntry> GetLibraryEntries() const;
  void PlayLibraryItem(unsigned index);
  // Same append-without-interrupting semantics as AddFavoriteToQueue().
  void AddLibraryItemToQueue(unsigned index);
  // Same "insert right after the current track" semantics as PlayFavoriteNext().
  void PlayLibraryItemNext(unsigned index);
  // Both operate on every entry GetLibraryEntries() currently returns (a
  // fully-browsed leaf level, e.g. an album's track list) — GnomosWindow
  // only ever offers these when the level has no containers left to
  // navigate into. AddAllLibraryItemsToQueue() appends without disturbing
  // playback, same as AddLibraryItemToQueue(); PlayAllLibraryItemsAsync()
  // replaces the queue outright and starts playing from the first item.
  void AddAllLibraryItemsToQueue();
  void PlayAllLibraryItemsAsync();

  // Third-party service linking (Spotify, bonob, ...). AppLink/DeviceLink
  // services need this before they'll browse — see the "Third-party
  // service linking" section in README for the full protocol story.
  std::vector<LinkableService> GetLinkableServices() const;
  // Starts linking; signal_service_link_ready() fires with a URL to open
  // in a browser once ready.
  void BeginServiceLink(const std::string& service_id);
  // Call once the user has completed the browser step. On success this
  // persists the credentials (survives restarts) and starts browsing the
  // service's root, just like a normal library entry activation would.
  void CompleteServiceLink();
  // Only meaningful while browsing inside a service (see BrowseLibraryAsync()
  // — empty means there's no active service, so nothing to search).
  std::vector<std::string> GetActiveServiceSearchCategories() const;
  void SearchActiveServiceAsync(const std::string& category, const std::string& term);
  // libnoson has no ContentDirectory "Search" action, only "Browse" — this
  // browses object_id (the current level) in full and filters client-side
  // by a case-insensitive substring match on title/subtitle. Only
  // meaningful when NOT inside a service (GetActiveServiceSearchCategories()
  // empty); GnomosWindow picks between the two based on that.
  void SearchLocalLibraryAsync(const std::string& object_id, const std::string& term);

  // Elapsed playback position, in seconds. UPnP's AVTransport only exposes
  // this via the polling GetPositionInfo action — unlike everything else in
  // this class, it is never pushed as an event — so callers must trigger
  // RefreshPositionAsync() themselves; GnomosWindow does so on a 1s timer
  // while a track (NowPlaying::duration > 0) is playing.
  void RefreshPositionAsync();
  unsigned GetPosition() const;
  // Absolute seconds from track start (despite AVTransport::SeekTime()'s
  // "REL_TIME" unit name, which refers to the UPnP time-vs-track-number
  // seek-target kind, not relative-to-current-position).
  void SeekAsync(unsigned seconds);
  sigc::signal<void()>& signal_position_changed() { return signal_position_changed_; }

  void RefreshAlarmsAsync();
  std::vector<AlarmInfo> GetAlarms() const;
  void SetAlarmEnabled(const std::string& alarm_id, bool enabled);
  // Whether the alarm also plays in rooms grouped with this one at wake
  // time (Alarm::SetIncludeLinkedZones()) — a standalone quick toggle in
  // the alarm list, same as SetAlarmEnabled(), rather than a field in the
  // create/edit dialog; mirrors noson-app's own UI (Alarms.qml), which
  // exposes it the same way.
  void SetAlarmIncludeLinkedZones(const std::string& alarm_id, bool include);
  void DeleteAlarm(const std::string& alarm_id);
  // days: NSROOT::Day_t values (0=Sunday..6=Saturday); empty means "once"
  // in the sense of no day repeated — callers should pass at least one day.
  // sound_index: 0 is the Sonos buzzer chime; i>0 is GetFavorites()[i-1] as
  // the wake sound (favorites, not raw library items, since that's the
  // pool noson-app's own alarm dialog draws from — see
  // DialogAlarm.qml::loadPrograms()).
  // duration_minutes: how long the alarm plays before auto-stopping.
  void CreateAlarm(const std::string& room_uuid, int hour, int minute, const std::vector<int>& days, uint8_t volume,
                    unsigned sound_index = 0, unsigned duration_minutes = 120, bool shuffle = false);
  // Updates the room/time/days/volume/sound/duration/shuffle of an existing
  // alarm; only its enabled state and IncludeLinkedZones (see
  // SetAlarmIncludeLinkedZones()) are left untouched.
  void UpdateAlarmSchedule(const std::string& alarm_id, const std::string& room_uuid, int hour, int minute,
                            const std::vector<int>& days, uint8_t volume, unsigned sound_index = 0,
                            unsigned duration_minutes = 120, bool shuffle = false);
  // ["Wecker-Ton", <favorite titles>...] — index into this list is what
  // CreateAlarm()/UpdateAlarmSchedule() want as sound_index.
  std::vector<std::string> GetAlarmSoundTitles() const;
  // Briefly plays the chosen wake sound in room_uuid, outside of and
  // without touching that room's actual queue — so the create/edit dialog
  // can offer a "test" button. Only meaningful for a real favorite
  // (sound_index > 0, and not kKeepExistingAlarmSound); silently does
  // nothing for the buzzer chime (index 0), since libnoson has no plain
  // string-URI playback entry point to feed it its special
  // "x-rincon-buzzer:0" pseudo-URI with — that only ever gets resolved
  // internally by the device itself when a real Alarm object fires.
  void PreviewAlarmSound(const std::string& room_uuid, unsigned sound_index);

  sigc::signal<void(bool)>& signal_discovery_done() { return signal_discovery_done_; }
  sigc::signal<void()>& signal_zones_changed() { return signal_zones_changed_; }
  sigc::signal<void()>& signal_player_ready() { return signal_player_ready_; }
  sigc::signal<void()>& signal_now_playing_changed() { return signal_now_playing_changed_; }
  sigc::signal<void()>& signal_volume_changed() { return signal_volume_changed_; }
  sigc::signal<void()>& signal_queue_changed() { return signal_queue_changed_; }
  sigc::signal<void()>& signal_favorites_changed() { return signal_favorites_changed_; }
  sigc::signal<void()>& signal_library_changed() { return signal_library_changed_; }
  // Carries the URL (and, for DeviceLink services, a short code shown
  // alongside it) the user needs to open to complete linking.
  sigc::signal<void(std::string, std::string)>& signal_service_link_ready() { return signal_service_link_ready_; }
  sigc::signal<void()>& signal_sleep_timer_changed() { return signal_sleep_timer_changed_; }
  sigc::signal<void()>& signal_sound_settings_changed() { return signal_sound_settings_changed_; }
  sigc::signal<void()>& signal_alarms_changed() { return signal_alarms_changed_; }
  sigc::signal<void(std::string)>& signal_error() { return signal_error_; }

private:
  // Trampolines: invoked by libnoson on one of ITS OWN threads. They must
  // do nothing but wake the matching dispatcher.
  static void OnSystemEvent(void* handle);
  static void OnPlayerEvent(void* handle);

  // Dispatcher slots: run on the GTK main thread.
  void HandleSystemEvent();
  void HandlePlayerEvent();
  void HandleDiscoveryDone();
  void HandlePlayerReady();
  void HandleError();

  // Fetches (via a blocking HTTP GET, on the TaskQueue worker thread —
  // this isn't a libnoson/SOAP call at all, just the coordinator's own
  // UPnP device_description.xml) and caches each not-yet-known zone
  // coordinator's real hardware model number, for is_gen1_model(). Only
  // ever fetched once per uuid (a device's hardware doesn't change at
  // runtime) and only cached on a successful fetch+parse, so a transient
  // network hiccup gets retried on the next topology change rather than
  // permanently sticking as "not gen1".
  void RefreshGen1StatusAsync();

  // Callers must hold state_mutex_. Only touch the now_playing_/volume_
  // cache and cheap, non-blocking libnoson getters (GetTransportProperty(),
  // GetRenderingProperty() are locked in-memory reads, not network calls).
  void RefreshNowPlayingLocked();
  void RefreshVolumeLocked();
  std::string ResolveArtUri(const std::string& uri) const;

  // The actual (non-debounced) work SetVolume()/SetRoomVolume() defer to
  // — see those two and their shared debounce members below.
  void ApplyVolumeAsync(uint8_t value);
  void ApplyRoomVolumeAsync(const std::string& player_uuid, uint8_t value);

  NSROOT::PlayerPtr SnapshotPlayer() const;
  NSROOT::ZonePtr SnapshotZone() const;

  // Runs on the TaskQueue worker thread; only called from within
  // BrowseLibraryAsync()'s own worker-thread task.
  void BrowseActiveServiceLocked(const std::string& id);
  // Refreshes active_service_search_categories_ from active_smapi_ (which
  // must already be Init()'d and not AuthTokenExpired()). Worker-thread only.
  void CacheActiveServiceSearchCategories();

  // Resolves a GetAlarmSoundTitles() index to a program URI + metadata and
  // applies it to alarm. Runs on the TaskQueue worker thread (called from
  // CreateAlarm()/UpdateAlarmSchedule()'s own tasks).
  void ApplyAlarmSound(NSROOT::Alarm& alarm, unsigned sound_index);

  // Third-party service credential persistence — a small keyfile under
  // $XDG_CONFIG_HOME/gnomos/, since System::AddServiceOAuth() only
  // registers an account for the current process; nothing in libnoson
  // itself persists it across restarts (noson-app does the same thing
  // itself, at the QML layer — see mediamodel.cpp/noson.qml).
  void LoadLinkedServices();
  void PersistAndRegisterServiceCredentials(const std::string& type, const std::string& serial_num,
                                             const std::string& key, const std::string& token,
                                             const std::string& username);

  // Subscribes to logind's PrepareForSleep signal (system bus) so a
  // suspend/resume cycle doesn't leave the app showing stale state for
  // however long libnoson's own subscription-renewal timers would
  // otherwise take to notice — see the .cpp for what "resume" does.
  // Best-effort: if the system bus or logind aren't reachable (e.g. no
  // systemd), this silently does nothing rather than failing startup.
  void SubscribeToSleepSignal();
  void OnPrepareForSleep(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring& sender_name,
                          const Glib::ustring& object_path, const Glib::ustring& interface_name,
                          const Glib::ustring& signal_name, const Glib::VariantContainerBase& parameters);

  // --- Destruction order matters here and is NOT the declaration order an
  // unrelated reader would expect. Members are destroyed bottom-to-top, and
  // we need, in this exact order:
  //   1. tasks_ stops accepting/joins first, so no queued action can call
  //      into a Player/System that's mid-teardown or gone.
  //   2. system_/player_ are destroyed next, which stops libnoson's own
  //      internal event-handler/subscription threads synchronously — after
  //      this, OnSystemEvent/OnPlayerEvent can no longer fire.
  //   3. Only then are the Glib::Dispatcher members destroyed, since step 2
  //      guarantees nothing will call ::emit() on them anymore.
  // That requires declaring dispatchers and plain state FIRST, and
  // system_/player_ and tasks_ LAST, in that relative order.

  Glib::Dispatcher system_dispatcher_;
  Glib::Dispatcher player_dispatcher_;
  Glib::Dispatcher discovery_dispatcher_;
  Glib::Dispatcher player_ready_dispatcher_;
  Glib::Dispatcher error_dispatcher_;
  Glib::Dispatcher queue_dispatcher_;
  Glib::Dispatcher favorites_dispatcher_;
  Glib::Dispatcher alarms_dispatcher_;
  Glib::Dispatcher library_dispatcher_;
  Glib::Dispatcher sleep_timer_dispatcher_;
  Glib::Dispatcher sound_settings_dispatcher_;
  Glib::Dispatcher service_link_ready_dispatcher_;
  Glib::Dispatcher position_dispatcher_;

  sigc::signal<void(bool)> signal_discovery_done_;
  sigc::signal<void()> signal_zones_changed_;
  sigc::signal<void()> signal_player_ready_;
  sigc::signal<void()> signal_now_playing_changed_;
  sigc::signal<void()> signal_volume_changed_;
  sigc::signal<void()> signal_queue_changed_;
  sigc::signal<void()> signal_favorites_changed_;
  sigc::signal<void()> signal_alarms_changed_;
  sigc::signal<void()> signal_library_changed_;
  sigc::signal<void(std::string, std::string)> signal_service_link_ready_;
  sigc::signal<void()> signal_sleep_timer_changed_;
  sigc::signal<void()> signal_sound_settings_changed_;
  sigc::signal<void(std::string)> signal_error_;
  sigc::signal<void()> signal_position_changed_;

  // Independent of libnoson entirely, and explicitly unsubscribed at the
  // very top of ~NosonBackend()'s body (before any member starts being
  // destroyed) rather than relying on declaration order — see the
  // destructor for why that's simpler to reason about here than fitting
  // this into the ordering rules below.
  Glib::RefPtr<Gio::DBus::Connection> system_bus_connection_;
  guint sleep_signal_subscription_id_ = 0;

  // Debounce state for SetVolume()/SetRoomVolume() — main-thread only
  // (Glib::signal_timeout(), like the D-Bus connection above, needs no
  // locking and is explicitly disconnected in ~NosonBackend()'s body
  // rather than relying on declaration order, same reasoning as above).
  sigc::connection volume_debounce_connection_;
  std::map<std::string, sigc::connection> room_volume_debounce_connections_;

  mutable std::mutex state_mutex_;
  std::map<std::string, NSROOT::ZonePtr> zones_by_uuid_;
  // player uuid -> device_description.xml <modelNumber> — see
  // RefreshGen1StatusAsync(). is_gen1_model() is derived from this on
  // demand in Zones()/Rooms(), rather than cached separately.
  std::map<std::string, std::string> model_number_by_uuid_;
  NowPlaying now_playing_;
  unsigned position_ = 0;
  // "<CurrentTrack>|<CurrentTrackURI>" as of the last
  // RefreshNowPlayingLocked() call — lets it tell an actual track change
  // from a same-track play/pause/stop (both fire SVCEvent_TransportChanged)
  // so position_ only resets to 0 on the former.
  std::string current_track_key_;
  VolumeInfo volume_;
  // uuid -> volume for every non-fixed-output member of the *currently
  // selected* zone — populated alongside volume_ in RefreshVolumeLocked(),
  // from the same player_->GetRenderingProperty() call. Backs
  // GetRoomVolume() for the grouping popover's per-room sliders.
  std::map<std::string, uint8_t> room_volumes_;
  std::vector<QueueItem> queue_;
  unsigned queue_update_id_ = 0;  // needed by Player::RemoveTrackFromQueue()
  std::vector<FavoriteItem> favorites_;
  std::vector<NSROOT::DigitalItemPtr> favorites_raw_;  // index-aligned with favorites_
  std::vector<AlarmInfo> alarms_;
  std::vector<LibraryEntry> library_entries_;
  std::vector<NSROOT::DigitalItemPtr> library_raw_;  // index-aligned with library_entries_
  std::string pending_link_url_;
  std::string pending_link_code_;
  // Cached (not read live from active_smapi_, which is worker-thread-only —
  // see its declaration) so GetActiveServiceSearchCategories() can be a
  // plain, main-thread-safe locked getter like everything else here.
  // Populated when a service's root is entered, cleared on leaving it.
  std::vector<std::string> active_service_search_categories_;
  SleepTimerInfo sleep_timer_;
  SoundSettings sound_settings_;
  bool last_discovery_ok_ = false;
  std::string pending_error_;
  NSROOT::ZonePtr current_zone_;

  std::unique_ptr<NSROOT::System> system_;
  NSROOT::PlayerPtr player_;
  // Set while browsing inside a third-party service (e.g. Spotify via
  // bonob); null means the library view is browsing the local
  // ContentDirectory instead. Only ever touched on the TaskQueue worker
  // thread (see BrowseLibraryAsync()), so — unlike player_ — it needs no
  // mutex: nothing else ever reads or writes it.
  std::unique_ptr<NSROOT::SMAPI> active_smapi_;
  NSROOT::SMServicePtr active_service_;

  TaskQueue tasks_;
};

}  // namespace gnomos
