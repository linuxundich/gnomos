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

// Sonos's PlayMode_t has no combined "shuffle + repeat one" mode (only
// NORMAL, REPEAT_ALL, REPEAT_ONE, SHUFFLE [= shuffle + repeat all], and
// SHUFFLE_NOREPEAT), so repeat-one is only ever reachable while shuffle is
// off — see NosonBackend::SetRepeatMode()/SetShuffle() for how the two
// dimensions get reconciled into a single device PlayMode_t.
enum class RepeatMode
{
  Off,
  All,
  One,
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
  RepeatMode repeat = RepeatMode::Off;  // AVTProperty::CurrentPlayMode == REPEAT_ALL/REPEAT_ONE/SHUFFLE
  // AVTProperty::CurrentCrossfadeMode ("0"/"1") — read-only here: this
  // fork's AVTransport class has no SetCrossfadeMode SOAP call at all, so
  // there's nothing for a toggle in Gnomos to actually call. Shown purely
  // informationally (PlayerBar).
  bool crossfade_enabled = false;
  // AVTProperty::r_CurrentValidPlayModes ("SHUFFLE,REPEAT,CROSSFADE" or a
  // subset) — not every source supports shuffle/repeat at all (radio,
  // line-in), so the shuffle/repeat buttons are only sensitive when the
  // device itself reports the mode as valid for what's currently playing.
  bool shuffle_supported = true;
  bool repeat_supported = true;
  // AVTProperty::CurrentTransportActions ("Set, Play, Stop, Pause, Seek,
  // Next, Previous" or a subset) — not every source supports every
  // transport action (e.g. some radio stations don't support Next/
  // Previous at all), so the corresponding buttons/MPRIS Can* properties
  // are only true when the device itself reports the action as available.
  bool can_go_next = true;
  bool can_go_previous = true;
  bool can_pause = true;
  // AVTProperty::TransportStatus — "OK" normally; anything else (e.g.
  // "ERROR_OCCURRED") means the device itself is reporting a transport
  // problem, surfaced as a toast rather than silently ignored.
  bool transport_status_ok = true;
  // AVTProperty::r_AlarmRunning — an alarm is actively ringing in this
  // room right now. Doesn't identify *which* alarm; stopping transport in
  // the room (the same action Play/Pause already offers) stops it
  // regardless of which one it was, so nothing further is needed to act
  // on this.
  bool alarm_running = false;
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
  // The raw stream URL (AVTProperty::AVTransportURI) — only ever populated
  // for a radio-like source (duration == 0), mirroring LibraryEntry::
  // stream_uri's own comment. Lets MprisService look up this station's
  // per-station MPRIS settings without needing to reach back into
  // NosonBackend's own internal RadioStreamMatchKey() hashing scheme.
  std::string stream_uri;
};

// Per-station preferences for whether/how a radio station's rotating
// "now playing" content (station-reported song/ad text, NowPlaying::artist)
// is republished to MPRIS clients. Keyed by NowPlaying::stream_uri /
// LibraryEntry::stream_uri via the same RadioStreamMatchKey() hashing
// radio-favicons.ini already uses — see NosonBackend::GetRadioMprisSettings()/
// SetRadioMprisSettings().
struct RadioMprisSettings
{
  bool mpris_enabled = true;
  // Empty means no filtering: every content change is republished as-is
  // (today's default behavior). Non-empty: only content that matches this
  // pattern (std::regex_search) counts as a real song — everything else
  // (ad breaks, station idents, ...) is ignored. See MprisService::
  // BuildMetadata() for how this and mpris_enabled combine.
  std::string regex;
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
  // RenderingControl's own logarithmic dB scale (RCSProperty::VolumeDecibelMaster),
  // alongside the simple 0-100 volume already above — genuinely
  // per-device, not a group concept the way the 0-100 scale's group
  // average is, so this reads just one representative member of the
  // current group (whichever RefreshVolumeLocked() happened to iterate
  // first) rather than trying to average several devices' own dB scales,
  // which can differ by model. Comes back "for free" as part of the same
  // GetRenderingProperty() call volume/muted above are already read
  // from — no extra network round trip. Purely informational (a
  // PlayerBar tooltip); never used for any actual volume logic.
  int16_t volume_db = 0;
};

struct SoundSettings
{
  int8_t bass = 0;     // -10..10
  int8_t treble = 0;   // -10..10
  bool loudness = false;
  bool nightmode = false;
  bool nightmode_supported = false;  // not every model has this (e.g. no subwoofer channel)
  // Fixed volume / line-out mode: with it on, the device ignores its own
  // volume control entirely (meant for feeding a receiver/amp that already
  // has its own volume knob) — same OutputFixed concept
  // NosonBackend::SetGroupVolume()/SetMuted() already skip when scaling a
  // group. Only relevant for a device wired via line-out, so gated behind
  // GetSupportsOutputFixed() rather than always shown.
  bool output_fixed_supported = false;
  bool output_fixed = false;
  // Sub (subwoofer) gain, -15..15 (Sonos's own convention for this
  // parameter, distinct from bass/treble's -10..10). No GetSupportsSubGain()
  // exists anywhere in the protocol the way output_fixed has one — support
  // is inferred the same way nightmode_supported already is, from whether
  // GetSubGain() itself succeeds (a zone with no paired Sub simply fails
  // the request).
  bool sub_gain_supported = false;
  int16_t sub_gain = 0;
  // Line-in autoplay: when a line-in signal is detected on this device,
  // should it start playing (into itself — Player::SetAutoplay()'s own
  // simplified bool wrapper around DeviceProperties::SetAutoplayRoomUUID()
  // only ever sets it to this device's own uuid or clears it, no arbitrary
  // target room). autoplay_supported is inferred the same way
  // nightmode_supported/sub_gain_supported are, from whether GetAutoplay()
  // itself succeeds — not every model has a line-in at all.
  bool autoplay_supported = false;
  bool autoplay_enabled = false;
  bool autoplay_use_volume = false;
  uint8_t autoplay_volume = 0;  // 0..100, meaningful only when autoplay_use_volume
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
  // Whether this entry is well suited to cover-art grid display — the
  // single, uniform signal GnomosWindow::OnLibraryChanged() checks
  // (std::any_of over a level's entries) regardless of where the level
  // came from, so it never needs to branch on local-vs-service itself.
  // The two sources populating it use different underlying signals, since
  // they're genuinely different protocols: third-party services
  // (Spotify, bonob, ...) say so directly via SMAPIItem::displayType ==
  // Grid (see BrowseActiveServiceLocked()); the local library has no such
  // per-item hint, so BrowseLibraryAsync() derives it from the level's
  // object_id prefix ("A:ALBUM"/"A:ALBUMARTIST") combined with
  // is_container instead.
  bool display_as_grid = false;
  // GNOME/Adwaita symbolic icon name shown by CoverThumbnail whenever
  // art_uri is empty (or fails to load) — empty means "just use
  // CoverThumbnail's own generic default", the same "audio-x-generic-
  // symbolic" every entry used to show regardless of type. Populated two
  // ways: the static local root categories set it directly (BrowseLibraryAsync()'s
  // own roots list, e.g. "Interpreten" -> an avatar icon); everything else
  // (real local content, and every SMAPI entry) derives it from the
  // underlying DigitalItem::subType() — person/album/genre/playlistContainer
  // map to a matching icon, since that's a reliable, already-populated,
  // service-and-language-independent signal (parsed generically from the
  // item's own upnp:class DIDL property) rather than something Gnomos
  // needs to guess from a title string. See IconNameForSubType().
  std::string icon_name;
  // The raw stream URL ("res" DIDL value) — only ever populated while
  // browsing "R:0/0" (see BrowseLibraryAsync()'s radio_favicons lookup,
  // which reads the same value). Lets GnomosWindow key a station's
  // per-station MPRIS settings (NosonBackend::GetRadioMprisSettings()/
  // SetRadioMprisSettings()) the same way SaveRadioFavicon() already keys
  // its own per-station favicon.
  std::string stream_uri;
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

// For the "Geräteinfo" dialog — entirely derived from already-cached zone
// topology data (see NosonBackend::GetDeviceInfo()), no network round trip
// needed: ip/software_version come straight from the player's own
// ZoneGroupState attributes, mac is decoded from its RINCON_<mac>...
// UUID directly (Sonos embeds it there), and model_number/is_gen1 reuse
// the same cache RefreshGen1StatusAsync() already populates.
struct DeviceInfo
{
  std::string ip;
  std::string mac;
  std::string software_version;
  std::string model_number;
  bool is_gen1 = false;
  // DeviceProperties::GetZoneInfo()'s own "SerialNumber"/"HardwareVersion"
  // fields — a real UPnP call to the room's own device (see
  // NosonBackend::RefreshZoneInfoAsync()), unlike every other DeviceInfo
  // field above, which comes from data already gathered at discovery time.
  std::string serial_number;
  std::string hardware_version;
};

// A lightweight, per-room snapshot for the room switcher's own list —
// unlike NowPlaying (which only ever tracks the *currently selected*
// zone), this is fetched independently for every zone's coordinator, so
// the switcher can show "what's playing where" without switching into a
// room first. Deliberately not the full NowPlaying: no shuffle/repeat/
// art/etc., nothing the switcher's own compact row actually shows.
struct RoomNowPlaying
{
  bool valid = false;
  TransportState state = TransportState::Unknown;
  std::string title;
  // AVTProperty::CurrentTransportActions listing "Pause" — mirrors
  // NosonBackend::PauseOrStop()'s own per-source check, needed here too
  // since ToggleRoomPlayback() has no NowPlaying::can_pause of its own to
  // read (this struct doesn't carry it — see above).
  bool can_pause = true;
};

}  // namespace gnomos
