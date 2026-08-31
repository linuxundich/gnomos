// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
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
  // Synchronous, unlike almost every other query here — see DeviceInfo's
  // own comment for why no network round trip is needed. Returns a
  // default-constructed (all-empty) DeviceInfo if player_uuid isn't a
  // currently known room.
  DeviceInfo GetDeviceInfo(const std::string& player_uuid) const;
  // System::GetHouseholdID() — set once, during Discover(), and
  // essentially fixed for the household's lifetime; useful mainly for
  // troubleshooting a multi-household setup (About → Debug-Informationen).
  // Synchronous, same reasoning as GetDeviceInfo() above. Empty before
  // the first successful discovery.
  std::string GetHouseholdID() const;

  // Fetches a fresh RoomNowPlaying snapshot for every currently known
  // zone's coordinator, since Gnomos otherwise only ever tracks live
  // transport state for the *currently selected* zone. One throwaway
  // NSROOT::AVTransport per zone, queried directly via its own
  // GetTransportInfo()/GetPositionInfo() SOAP calls — deliberately NOT a
  // throwaway NSROOT::Player's GetTransportProperty(), which only reflects
  // whatever that Player's own AVTransport has accumulated from
  // *subscribed* LastChange events; confirmed live, an unsubscribed
  // Player's GetTransportProperty() came back permanently empty
  // (TransportState::Unknown, no title) for every room, no matter what was
  // actually playing. Meant to be called while the room switcher popover
  // is open (once on open, then on a short repeating timer — see
  // room_popover_'s own signal_show() handler, in the GnomosWindow
  // constructor), not continuously in the background. Results land in
  // signal_room_now_playing_changed(), read back per room via
  // GetRoomNowPlaying().
  void RefreshAllRoomNowPlayingAsync();
  RoomNowPlaying GetRoomNowPlaying(const std::string& coordinator_uuid) const;
  // Same Pause()-if-supported-else-Stop() decision PauseOrStop() already
  // makes for the currently selected zone, applied to an arbitrary one —
  // lets the room switcher's own per-row button toggle playback in a room
  // without switching into it first.
  void ToggleRoomPlayback(const std::string& coordinator_uuid);
  sigc::signal<void()>& signal_room_now_playing_changed() { return signal_room_now_playing_changed_; }

  // Makes the room join the currently selected zone's group (as a
  // satellite of that zone's coordinator), or leave whatever group it's
  // currently in to become standalone again. Both are fire-and-forget:
  // the resulting topology change arrives as a normal ZGTopologyChanged
  // event and updates Zones()/Rooms() the same way any other change would.
  void JoinRoomToCurrentZone(const std::string& room_player_uuid);
  void RemoveRoomFromGroup(const std::string& room_player_uuid);
  // Same JoinToGroup() action as JoinRoomToCurrentZone() above, but against
  // an arbitrary target coordinator rather than always the currently
  // selected zone — backs the room switcher's drag-and-drop grouping
  // (dragging one zone's row onto another's).
  void JoinRoomToZone(const std::string& room_player_uuid, const std::string& target_coordinator_uuid);

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
  // One-off, unsaved playback of an arbitrary stream URL — distinct from
  // AddRadioStation(), which always persists a favorite. title shows up
  // wherever the currently playing track's own title normally would
  // (player bar, MPRIS, ...); pass the URL itself if the caller has
  // nothing better.
  void PlayStreamAsync(const std::string& url, const std::string& title);
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
  // Mutes (or un-mutes) every physical player in the household in one go,
  // regardless of which group it's currently in — unlike SetMuted() above,
  // which only ever reaches the *currently selected* zone's own group.
  // GnomosWindow's own "Überall stummschalten" menu action only ever calls
  // this with true — a quick "silence everything now" convenience, not a
  // toggle exposed to the user (un-muting a given room again is a
  // deliberate per-room call, same as everywhere else in this app) — but
  // the underlying call is symmetric regardless.
  void MuteAllRoomsAsync(bool muted);
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
  // Same idea as AddCurrentTrackToFavorites(), but for an arbitrary already-
  // browsed library entry rather than whatever's currently playing — index
  // into current_library_entries_/library_raw_ (both index-aligned, same
  // pattern PlayLibraryItem() already uses). Works for a container (e.g. a
  // whole album or playlist) as well as a leaf track, since Sonos favorites
  // support both — GnomosWindow only offers the action below the true root
  // level, where entries are real content rather than static categories.
  void AddLibraryItemToFavorites(unsigned index);

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
  void SetOutputFixed(bool enabled);
  void SetSubGain(int16_t value);
  // Line-in autoplay — see SoundSettings::autoplay_* for what each of
  // these actually means; SetAutoplay(true) targets this same device (the
  // only option Player::SetAutoplay() itself exposes), SetAutoplay(false)
  // clears it.
  void SetAutoplay(bool enabled);
  void SetAutoplayVolume(uint8_t value);
  void SetUseAutoplayVolume(bool enabled);
  // Write-only, like PlayLineIn()/PlayDigitalIn() — libnoson has no
  // GetLEDState() to show a current value with.
  void SetLedState(bool enabled);

  void RefreshQueueAsync();
  void PlayQueueItem(unsigned index);
  void RemoveQueueItem(unsigned index);
  // Same 0-based queue indices as RemoveQueueItem(), any number of them,
  // in any order — collapsed into the fewest possible contiguous ranges
  // and removed via AVTransport::RemoveTrackRangeFromQueue() (one SOAP
  // call per range, not per track), highest-index range first so an
  // earlier removal in the same batch never shifts the position of a
  // range still waiting to be removed. See its own .cpp comment for why
  // this needs a fresh containerUpdateID between ranges rather than
  // reusing queue_update_id_ for all of them.
  void RemoveQueueItems(std::vector<unsigned> indices);
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
  // Same semantics as AddAllLibraryItemsToQueue()/PlayAllLibraryItemsAsync()
  // below, over favorites_raw_ instead of library_raw_ — unlike a library
  // level, a favorites list is never gated on "all leaf" first, since every
  // favorite (track, album, playlist, or radio station alike) is already
  // individually playable/queueable the same way PlayFavorite()/
  // AddFavoriteToQueue() handle it per-row.
  void AddAllFavoritesToQueue();
  void PlayAllFavoritesAsync();

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
  // System::DestroySavedQueue() against library_entries_[index]'s own
  // object_id — GnomosWindow only offers this while browsing "SQ:" (the
  // "Playlisten" root), where every entry's object_id really is a
  // destroyable saved-queue id, unlike any other library level.
  void DeleteLibraryPlaylist(unsigned index);
  // System::DestroyRadio() — same idea as DeleteLibraryPlaylist(), but
  // against library_entries_[index]'s object_id while browsing "R:0/0"
  // ("Radiosender") instead of "SQ:". GnomosWindow gates the delete button
  // on being at either of those two specific levels.
  void DeleteLibraryRadioStation(unsigned index);
  // Saved playlists, fetched independently of whatever level is currently
  // browsed (library_entries_/library_raw_) — a picker dialog for "add
  // this track to a playlist" needs the *list of playlists* regardless of
  // where in the library the user actually is when they click that
  // button, so this can't just reuse BrowseLibraryAsync("SQ:") without
  // clobbering whatever the user is currently looking at.
  void FetchSavedPlaylistsAsync();
  std::vector<LibraryEntry> GetSavedPlaylists() const;
  sigc::signal<void()>& signal_saved_playlists_changed() { return signal_saved_playlists_changed_; }
  // library_raw_[library_index] into playlist_object_id (an SQ:<id> from
  // GetSavedPlaylists()), via Player::AddURIToSavedQueue(). Fetches a
  // fresh containerUpdateID for the target playlist itself right before
  // the call — see its own comment for why that can't be cached from
  // whenever the playlist was last browsed.
  void AddLibraryItemToPlaylist(unsigned library_index, const std::string& playlist_object_id);
  // Player::CreateSavedQueue(title), then the same AddURIToSavedQueue()
  // call AddLibraryItemToPlaylist() makes — CreateSavedQueue() itself has
  // no way to hand back the new playlist's own object_id, so this
  // re-browses "SQ:" and matches on the title just given it to find it,
  // the same way every other saved playlist is discovered. Also updates
  // GetSavedPlaylists()' own cache and fires signal_saved_playlists_changed()
  // with the fresh list, so a picker dialog still open shows the new
  // playlist without a separate re-fetch.
  void CreatePlaylistAndAddLibraryItem(unsigned library_index, const std::string& title);
  // 0-based positions within playlist_object_id's own current track
  // listing (same convention as ReorderQueueItem()'s from/to) — only
  // meaningful while GnomosWindow is showing that exact playlist's
  // contents, not the "SQ:" listing of playlists itself.
  void ReorderLibraryPlaylistTrack(const std::string& playlist_object_id, unsigned from, unsigned to);
  // System::RefreshShareIndex() — asks Sonos to rescan whatever indexed
  // local music share(s) it's configured with (e.g. after adding files to
  // a NAS share). Fire-and-forget: libnoson has no way to observe when the
  // scan itself finishes, only that the request was accepted.
  void RefreshLibraryIndex();
  // System::GetContentProperty()'s ShareIndexInProgress/ShareIndexLastError
  // — RefreshLibraryIndex() above only reports a failure to *start* the
  // scan; this is how GnomosWindow finds out once a scan that did start
  // actually finishes (or fails partway through), by calling this once per
  // tick of its own polling timer (StartLibraryIndexProgressPolling())
  // after triggering RefreshLibraryIndex(). GetContentProperty() itself is
  // a locked in-memory read of the last event ContentDirectory pushed, not
  // a fresh network round trip — same category as GetTransportProperty()/
  // GetRenderingProperty() — but still routed through tasks_ like every
  // other libnoson call here, not read directly from the GTK main thread.
  void CheckLibraryIndexProgressAsync();
  bool GetLibraryIndexInProgress() const;
  std::string GetLibraryIndexLastError() const;
  sigc::signal<void()>& signal_library_index_status_changed() { return signal_library_index_status_changed_; }
  // System::CreateRadio() — adds a custom internet radio stream, which
  // then shows up browsing "R:0/0" ("Radiosender") alongside the built-in
  // directory, same namespace either way (confirmed in CreateRadio()'s own
  // implementation — it creates the object under the same ContentSearch
  // root "R:0/0" already browses). streamURL must be an http(s) URL —
  // System::CreateRadio() itself validates and rejects anything else.
  // favicon_url is never sent to Sonos at all — CreateRadio() has no icon
  // parameter, so a custom station never gets real cover art the way a
  // built-in TuneIn one does (Sonos's own directory already carries a
  // upnp:albumArtURI for those). When non-empty (radio-browser.info's own
  // search results carry one; the manual name/URL fallback has nothing to
  // offer here), it's persisted locally instead — see SaveRadioFavicon()'s
  // own comment — and looked back up the next time "R:0/0" is browsed.
  void AddRadioStation(const std::string& title, const std::string& stream_url,
                        const std::string& favicon_url = "");

  // Per-station preferences for whether/how a radio station's rotating
  // "now playing" content reaches MPRIS clients *and* the History tab
  // (see RadioMprisSettings' own comment for why, and RadioContentFilter,
  // which both consumers apply this through — one instance each). Keyed
  // the same way SaveRadioFavicon()/LoadRadioFavicons() key a station's
  // favicon — the SHA-256 of its stream URL — in a separate
  // radio-mpris-settings.ini, so a station with no saved preferences yet
  // just returns RadioMprisSettings' own defaults (enabled, unfiltered).
  // Called from both MprisService and GnomosWindow (read-only, every
  // NowPlaying update, via their own RadioContentFilter) and GnomosWindow's
  // settings dialog (read on open, write on save), so this needs to be
  // public unlike the favicon pair, which only NosonBackend itself ever
  // calls.
  RadioMprisSettings GetRadioMprisSettings(const std::string& stream_uri) const;
  void SetRadioMprisSettings(const std::string& stream_uri, const RadioMprisSettings& settings);

  // Global (not per-station) companion to the regex above — RadioContentFilter
  // treats content with more than two consecutive spaces as filler
  // whenever this is on, regardless of whether the current station has
  // its own regex configured. Defaults to on; stored alongside the
  // per-station settings in the same radio-mpris-settings.ini, under a
  // "global" group rather than a per-station one.
  bool GetRadioSpamWhitespaceFilterEnabled() const;
  void SetRadioSpamWhitespaceFilterEnabled(bool enabled);

  // Every character in this string is treated as its own genre-tag
  // separator when BrowseLibraryAsync() builds the "Genres" level — e.g.
  // ";" splits an ID3 genre tag of "Rap; Metal; Hard-Core" into three
  // separate entries ("New Metal", with no separator character present,
  // stays a single entry). User-configurable since taggers disagree on
  // convention (";", "/", "|", ...); defaults to ";" alone, the most
  // common one, so nobody sees their genres unexpectedly split until they
  // opt in to more. Stored in library-settings.ini, not the per-station
  // radio-mpris-settings.ini — this has nothing to do with radio.
  std::string GetGenreSeparators() const;
  void SetGenreSeparators(const std::string& separators);

  // Sonos's own local-library indexer has been confirmed live to truncate
  // a multi-genre ID3 tag at a fixed total length before ever handing it
  // to noson — a genuinely long tag (several genres joined by the
  // configured separator) can come back with its later genre(s) cut off
  // mid-word ("Elec" instead of "Electronic"), nothing Gnomos/noson can
  // recover after the fact. Since whatever comes first in the tag is the
  // part least likely to have been cut, this restricts SplitGenreEntries()
  // to just the first resulting token per entry instead of every one —
  // trading "see every genre, including truncated fragments" for "see
  // only the reliable one." Off by default (today's split-everything
  // behavior); stored alongside GetGenreSeparators() in the same
  // library-settings.ini.
  bool GetGenreUseFirstOnly() const;
  void SetGenreUseFirstOnly(bool enabled);

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

  // Fires whenever tasks_ transitions between idle and busy — a burst of
  // one or more queued actions (any of the near-200 blocking libnoson
  // calls this class wraps) counts as one continuous busy period, not one
  // event per action — see TaskQueue's own constructor comment. Backs a
  // header-bar spinner showing "waiting on the Sonos system right now",
  // distinct from signal_discovery_done()'s own spinner-worthy state
  // (zone discovery specifically) — GnomosWindow combines both.
  sigc::signal<void(bool)>& signal_busy_changed() { return signal_busy_changed_; }
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

  // A real UPnP call (DeviceProperties::GetZoneInfo(), unlike
  // RefreshGen1StatusAsync()'s plain XML fetch above) to each not-yet-known
  // zone player's own device, for its SerialNumber/HardwareVersion —
  // shown in GnomosWindow's "Geräteinfo" dialog alongside the fields
  // GetDeviceInfo() already resolves from data gathered at discovery time.
  // Same "only ever fetched once per uuid, retried on the next topology
  // change if it failed" caching as RefreshGen1StatusAsync().
  void RefreshZoneInfoAsync();

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

  // Clears library_cache_ — called when the household's ContentDirectory
  // actually changes (SVCEvent_ContentDirectoryChanged in
  // HandleSystemEvent()), so a real change (new music scanned, a playlist
  // edited elsewhere) is reflected on the next visit to an affected level
  // instead of serving a stale cached copy for up to the TTL. Worker-
  // thread only, like library_cache_ itself.
  void InvalidateLibraryCache();

  // Rewrites the just-built "A:GENRE" level in place, splitting each
  // entry's title on GetGenreSeparators() into one entry per resulting
  // token (a title with no configured separator present comes back
  // unchanged). When the same token results from more than one original
  // entry (e.g. "Rap; Metal" and a separately tagged plain "Metal" both
  // produce "Metal"), the duplicates collapse into a single entry whose
  // object_id is a synthetic kMergedGenrePrefix id — BrowseLibraryAsync()
  // recognizes that prefix and browses every underlying id it packs in,
  // concatenating their children, so activating the merged entry shows
  // the union rather than just one of the originals.
  void SplitGenreEntries(std::vector<LibraryEntry>& entries, std::vector<NSROOT::DigitalItemPtr>& raw) const;

  // Local-only persistence for a custom radio station's favicon (see
  // AddRadioStation()'s own comment for why this can't just be sent to
  // Sonos and read back via upnp:albumArtURI like every other entry's
  // art). Keyed by the SHA-256 of the station's stream URL (same "hash an
  // arbitrary URI into something safe to use as an identifier" idea
  // ArtCache::PathFor() already uses, just as a KeyFile key here instead
  // of a filename) — matched against a browsed item's own res value
  // (item->GetValue("res"), the DIDL resource URI) in BrowseLibraryAsync()'s
  // local branch. Loaded fresh (not cached across calls) since it's only
  // ever read once per "R:0/0" browse, a rare, non-performance-sensitive
  // path.
  void SaveRadioFavicon(const std::string& stream_url, const std::string& favicon_url);
  std::map<std::string, std::string> LoadRadioFavicons() const;

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
  Glib::Dispatcher saved_playlists_dispatcher_;
  Glib::Dispatcher sleep_timer_dispatcher_;
  Glib::Dispatcher sound_settings_dispatcher_;
  Glib::Dispatcher service_link_ready_dispatcher_;
  Glib::Dispatcher position_dispatcher_;
  Glib::Dispatcher busy_dispatcher_;
  Glib::Dispatcher library_index_status_dispatcher_;
  Glib::Dispatcher room_now_playing_dispatcher_;
  // Written on tasks_'s own worker thread (TaskQueue's on_busy_changed
  // callback, passed in the constructor), read on the main thread once
  // busy_dispatcher_ wakes it up — the same "atomic handoff variable
  // alongside a Dispatcher" pattern this class has no other use for
  // elsewhere, since every other dispatcher here only ever signals "go
  // re-read some already-locked, already-consistent state" rather than
  // carrying a value of its own.
  std::atomic<bool> pending_busy_state_{false};

  sigc::signal<void(bool)> signal_busy_changed_;
  sigc::signal<void(bool)> signal_discovery_done_;
  sigc::signal<void()> signal_zones_changed_;
  sigc::signal<void()> signal_player_ready_;
  sigc::signal<void()> signal_now_playing_changed_;
  sigc::signal<void()> signal_volume_changed_;
  sigc::signal<void()> signal_queue_changed_;
  sigc::signal<void()> signal_favorites_changed_;
  sigc::signal<void()> signal_alarms_changed_;
  sigc::signal<void()> signal_library_changed_;
  sigc::signal<void()> signal_saved_playlists_changed_;
  sigc::signal<void(std::string, std::string)> signal_service_link_ready_;
  sigc::signal<void()> signal_sleep_timer_changed_;
  sigc::signal<void()> signal_sound_settings_changed_;
  sigc::signal<void(std::string)> signal_error_;
  sigc::signal<void()> signal_position_changed_;
  sigc::signal<void()> signal_library_index_status_changed_;
  sigc::signal<void()> signal_room_now_playing_changed_;

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

  // RefreshGen1StatusAsync()'s HttpFetch() callbacks capture this by value
  // and check it before touching `this` — unlike every other async
  // completion here (a Glib::Dispatcher, or a lambda pushed onto tasks_,
  // both of which this destructor already accounts for above), HttpFetch()
  // holds these callbacks in its own module-global queue, entirely outside
  // NosonBackend's control, so one can still fire after this object is
  // gone. Same "captured shared_ptr<bool>, flipped in the destructor"
  // pattern CoverThumbnail already uses for its own HttpFetch() calls.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  mutable std::mutex state_mutex_;
  std::map<std::string, NSROOT::ZonePtr> zones_by_uuid_;
  // player uuid -> device_description.xml <modelNumber> — see
  // RefreshGen1StatusAsync(). is_gen1_model() is derived from this on
  // demand in Zones()/Rooms(), rather than cached separately.
  std::map<std::string, std::string> model_number_by_uuid_;
  // player uuid -> (SerialNumber, HardwareVersion) — see
  // RefreshZoneInfoAsync(). GetDeviceInfo() reads from this the same way
  // it already reads model_number_by_uuid_.
  std::map<std::string, std::pair<std::string, std::string>> zone_info_by_uuid_;
  // coordinator uuid -> its own RoomNowPlaying snapshot — see
  // RefreshAllRoomNowPlayingAsync()/GetRoomNowPlaying().
  std::map<std::string, RoomNowPlaying> room_now_playing_by_uuid_;
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
  // Set at the end of SelectZone()'s own task, right when player_ is
  // (re)assigned. Subscribing to a freshly selected player's own eventing
  // channel reliably delivers an immediate "initial state" event for each
  // evented service (standard UPnP GENA behavior — the first event after
  // SUBSCRIBE echoes the current state, not an actual change) — without
  // this, HandlePlayerEvent()'s own SVCEvent_ContentDirectoryChanged
  // branch treated that echo as a genuine queue change and fired a second,
  // fully redundant RefreshQueueAsync() a few dozen ms after the one
  // SelectZone() → OnPlayerReady() already triggered, each one rebuilding
  // every queue row's CoverThumbnail from scratch — confirmed live as (the
  // last remaining bit of) a brief fallback-icon flicker right after
  // launch. See HandlePlayerEvent()'s own use of this for the grace
  // window — long enough to swallow that one echo, short enough that any
  // real mid-session queue change (from this app or another controller)
  // still refreshes normally.
  std::chrono::steady_clock::time_point last_zone_select_at_;
  std::vector<FavoriteItem> favorites_;
  std::vector<NSROOT::DigitalItemPtr> favorites_raw_;  // index-aligned with favorites_
  std::vector<AlarmInfo> alarms_;
  std::vector<LibraryEntry> library_entries_;
  std::vector<NSROOT::DigitalItemPtr> library_raw_;  // index-aligned with library_entries_
  // See FetchSavedPlaylistsAsync()'s own comment for why this is kept
  // separate from library_entries_ rather than reusing it.
  std::vector<LibraryEntry> saved_playlists_;
  // Last CheckLibraryIndexProgressAsync() result — see its own comment.
  bool library_index_in_progress_ = false;
  std::string library_index_last_error_;
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

  // Browse-result cache, keyed by object_id (or, inside a third-party
  // service, "<service id>\x1f<object_id>" — a bare object_id isn't
  // necessarily unique across services). Every level BrowseLibraryAsync()/
  // BrowseActiveServiceLocked() has already fetched once is served
  // straight from here on a repeat visit instead of a fresh network round
  // trip — the local browse tree rarely changes mid-session, and
  // navigating back into a level you just left (or revisiting a sibling)
  // is by far the most common pattern, not the exception. TTL-bounded
  // rather than purely event-invalidated: SVCEvent_ContentDirectoryChanged
  // (see HandleSystemEvent()) clears it outright for the local-library
  // case, but nothing plays that role for a third-party service's own
  // catalog, so a bounded staleness window covers that gap too. Only ever
  // touched on the TaskQueue worker thread, same as active_smapi_ above —
  // no mutex needed for the same reason.
  struct CachedLibraryLevel
  {
    std::vector<LibraryEntry> entries;
    std::vector<NSROOT::DigitalItemPtr> raw;
    std::chrono::steady_clock::time_point fetched_at;
  };
  std::map<std::string, CachedLibraryLevel> library_cache_;
  // Prepends the active service's id, if any — see library_cache_'s own
  // comment. Worker-thread only, same as active_smapi_/active_service_.
  std::string LibraryCacheKey(const std::string& object_id) const;
  bool GetCachedLibraryLevel(const std::string& object_id, std::vector<LibraryEntry>& out_entries,
                              std::vector<NSROOT::DigitalItemPtr>& out_raw) const;
  void StoreLibraryCacheLevel(const std::string& object_id, const std::vector<LibraryEntry>& entries,
                               const std::vector<NSROOT::DigitalItemPtr>& raw);

  TaskQueue tasks_;
};

}  // namespace gnomos
