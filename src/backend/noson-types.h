// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace gnomos
{

// There is no explicit "hardware generation" flag anywhere in the Sonos
// UPnP protocol / libnoson's API. This checks the device's real
// <modelNumber> (e.g. "ZP120"), fetched by NosonBackend from the player's
// own UPnP device_description.xml (ZonePlayer::GetLocation()) — NOT
// ZonePlayer::GetIconName(), which was tried first and is wrong: that
// returns the user-assigned *room* icon (e.g. "living", "masterbedroom"),
// unrelated to hardware at all, confirmed live against real hardware
// reporting exactly those two room-icon strings.
inline bool is_gen1_model(const std::string& model_number)
{
  static const std::vector<std::string> gen1_models = {
    "zp80", "zp90", "zp100", "zp120", "cr100",
  };
  std::string lower = model_number;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return std::find(gen1_models.begin(), gen1_models.end(), lower) != gen1_models.end();
}

struct ZoneInfo
{
  // Zone::GetGroup() — despite the name, this is Sonos's *group* id
  // ("RINCON_xxx:42", coordinator uuid + a topology version counter), NOT
  // the coordinator's own uuid. It's only ever used as an opaque key to
  // look this zone back up (e.g. in SelectZone()), never passed to an
  // action that expects a bare player uuid — see RoomInfo::coordinator_uuid
  // for that.
  std::string group_id;
  std::string display_name;  // Zone::GetZoneShortName(), e.g. "Living Room + 1"
  std::string icon;          // ZonePlayer::GetIconName() of the coordinator
  bool is_gen1 = false;
  // zone->GetCoordinator()->GetUUID() — the coordinator's actual bare
  // uuid, stable across restarts (unlike group_id's version counter).
  // Confirmed live that group_id's own prefix is NOT reliably this: two
  // simultaneously-active, independent zones were observed sharing the
  // exact same group_id prefix, so it cannot be used as a per-zone key
  // for anything that must survive a restart (e.g. "remember last room").
  std::string coordinator_uuid;
};

enum class TransportState
{
  Stopped,
  Playing,
  Paused,
  Transitioning,
  NoMedia,
  Unknown,
};

struct NowPlaying
{
  bool valid = false;
  TransportState state = TransportState::Unknown;
  std::string title;
  std::string artist;
  std::string album;
  std::string art_uri;      // resolved to an absolute http(s) URL, or empty
  bool shuffle = false;     // AVTProperty::CurrentPlayMode == SHUFFLE or SHUFFLE_NOREPEAT
  bool repeat = false;      // AVTProperty::CurrentPlayMode == REPEAT_ALL, REPEAT_ONE, or SHUFFLE
  // Seconds; 0 means a live stream (radio/line-in) or unknown, not "just
  // started" — mirrors noson-app's own postulate (player.cpp,
  // setCurrentMeta()). Pushed via AVTProperty::CurrentTrackDuration, unlike
  // the elapsed position below, which UPnP doesn't push at all.
  unsigned duration = 0;
  // Whether the current source is the zone's own queue at all
  // (AVTProperty::AVTransportURI starting with "x-rincon-queue:" — radio,
  // line-in, and direct-URI service streams are all something else) and,
  // if so, which 0-based QueueItem::index is currently playing
  // (AVTProperty::CurrentTrack is 1-based). Lets the queue view highlight
  // the playing row without ever risking a false-positive highlight while
  // playing from a non-queue source, since CurrentTrack is otherwise not a
  // meaningful queue position at all in that case.
  bool playing_from_queue = false;
  unsigned current_queue_index = 0;
};

struct SleepTimerInfo
{
  bool active = false;
  std::string remaining;  // device-native "H:MM:SS", only meaningful if active
};

struct VolumeInfo
{
  uint8_t volume = 0;
  bool muted = false;
};

struct SoundSettings
{
  int8_t bass = 0;     // -10..10
  int8_t treble = 0;   // -10..10
  bool loudness = false;
  bool nightmode = false;
  bool nightmode_supported = false;  // not every model has this (e.g. no subwoofer channel)
};

struct QueueItem
{
  std::string object_id;
  std::string title;
  std::string artist;
  std::string album;
  std::string art_uri;  // resolved to an absolute http(s) URL, or empty — like NowPlaying::art_uri
  unsigned index = 0;   // 0-based position in the queue
};

struct FavoriteItem
{
  std::string title;
  std::string subtitle;
  std::string art_uri;  // resolved to an absolute http(s) URL, or empty
  unsigned index = 0;    // position in the favorites list, passed back to NosonBackend::PlayFavorite()
};

// A past NowPlaying snapshot, recorded client-side (Sonos/UPnP has no
// native play-history API) purely for display in the "Zuletzt gespielt"
// tab. Deliberately has no object_id/URI to replay from — NowPlaying
// itself doesn't carry one — so this is informational only, unlike
// QueueItem/FavoriteItem.
struct HistoryEntry
{
  std::string title;
  std::string artist;
  std::string album;
  std::string art_uri;
};

// Sentinel LibraryEntry::object_id GnomosWindow recognizes to open the
// service-linking picker instead of trying to browse into it (see
// NosonBackend::BrowseLibraryAsync()'s root-category construction).
inline constexpr const char* kLinkServiceSentinel = "gnomos:link-service";

// Prefix a root-level LibraryEntry::object_id carries for "enter this
// third-party service" (followed by SMService::GetId()) — shared between
// NosonBackend::BrowseLibraryAsync() and GnomosWindow, which needs the same
// prefix to push a matching breadcrumb after a successful service link
// (see ShowServiceLinkDialog()).
inline constexpr const char* kServiceRootPrefix = "smapi:";

// A service from System::GetAvailableServices() that could be linked (i.e.
// its policy is AppLink or DeviceLink — Anonymous ones don't need linking,
// UserId ones need a username/password form not implemented here).
struct LinkableService
{
  std::string id;  // SMService::GetId()
  std::string name;
};

struct LibraryEntry
{
  std::string object_id;
  std::string title;
  std::string subtitle;
  bool is_container = false;  // container: navigate into it; leaf item: play it
  // Appended after the fields above (not inserted earlier) so it doesn't
  // shift positions in the existing brace-init lists that build the static
  // root category entries (BrowseLibraryAsync()).
  std::string art_uri;  // resolved to an absolute http(s) URL, or empty
  // SMAPIItem::displayType == Grid (see BrowseActiveServiceLocked()) —
  // third-party services (Spotify, bonob, ...) say themselves whether an
  // item belongs in a cover-art grid, unlike the local library, which has
  // no such hint and falls back to an object_id-prefix heuristic in
  // GnomosWindow instead. Always false for local-library entries.
  bool display_as_grid = false;
};

// Sentinel for NosonBackend::UpdateAlarmSchedule()'s sound_index — leaves
// the alarm's existing sound untouched instead of resolving an index into
// GetAlarmSoundTitles(). GnomosWindow's alarm dialog offers this as an
// explicit "Aktueller Klang beibehalten" choice when editing, so an edit
// that isn't about the sound at all can't silently reset it to the buzzer.
inline constexpr unsigned kKeepExistingAlarmSound = static_cast<unsigned>(-1);

struct AlarmInfo
{
  std::string id;
  bool enabled = false;
  std::string room_uuid;   // Alarm::GetRoomUUID(), needed to preselect the room when editing
  std::string room_name;   // resolved from room_uuid via the room list; falls back to the raw uuid
  std::string start_time;  // device-native "HH:MM:SS", displayed as-is
  std::string recurrence;  // device-native "MON,TUE,WED,...", displayed as-is
  int volume = 0;
  // Alarm::GetIncludeLinkedZones() — whether the alarm also plays in rooms
  // grouped with this one at wake time, not just this one speaker. Defaults
  // to false in libnoson's own Alarm() constructor.
  bool include_linked_zones = false;
  // Alarm::GetDuration() ("H:MM:SS") parsed to minutes — how long the
  // alarm plays before auto-stopping. 120 matches CreateAlarm()'s own
  // existing default (Alarm() itself leaves this empty).
  unsigned duration_minutes = 120;
  // Alarm::GetPlayMode() == "SHUFFLE" or "SHUFFLE_NOREPEAT" — whether wake
  // playback is shuffled. Repeat doesn't apply the same way to a single
  // wake playback, so this is deliberately shuffle-only, not a full
  // PlayMode_t mirror.
  bool shuffle = false;
};

// One physical Sonos device, independent of the current group topology —
// used for the grouping UI, where every room in the house needs to be
// listed regardless of which group (ZoneInfo) it's currently part of.
struct RoomInfo
{
  std::string player_uuid;  // this device's own (bare) uuid
  std::string name;
  std::string group_id;              // matches ZoneInfo::group_id of the group this room is in right now
  std::string coordinator_uuid;      // bare uuid of that group's coordinator — the argument JoinToGroup() wants
  bool is_gen1 = false;               // ZonePlayer::GetIconName(), same heuristic as ZoneInfo::is_gen1
  std::string model_number;           // device_description.xml <modelNumber> (e.g. "ZP120"), empty if not yet fetched
};

}  // namespace gnomos
