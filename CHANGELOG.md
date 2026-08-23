# Changelog

All notable changes to Gnomos are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project does not yet follow strict Semantic Versioning guarantees
(pre-1.0, so the minor version can still carry breaking changes), but the
general idea holds: a new minor version (0.x.0) marks a significant chunk of
work, patch versions (0.x.y) are smaller additions and fixes on top of it.

## [0.4.0] - 2026-08-23

### Added
- Consistent GNOME iconography for library entries by type (artist/album/
  genre/playlist/folder), replacing the single generic icon every entry
  used to show regardless of what it actually was — including linked
  third-party services, whose own category tiles (e.g. bonob's "Albums",
  "Internet Radio") previously showed a visually inconsistent
  service-branded icon or, in one case, fell through to the plain
  fallback glyph entirely.
- Optional real artist photos in the library, looked up by name against
  Deezer's public API — off by default, since it's the one thing in this
  app that leaves the local Sonos household, with a Settings entry
  disclosing exactly that (endpoint, terms, opt-in).
- A live filter for narrowing down a long library list as you type
  (Artists/Albums/Genres/Playlists and similar, local and third-party
  services alike), plus a user-adjustable fallback icon size ("Symbolgröße"
  in Settings → Bibliothek).
- A "Geräteinfo" dialog per room (IP, MAC, software version, model),
  reached from the room picker's popover.
- Deleting a saved Sonos playlist, and adding a custom internet radio
  station, both from their respective library root levels.
- Sub gain in the sound popover, alongside the existing bass/treble/
  loudness/night mode/fixed volume.
- A "next alarm" indicator in the Alarms tab, and alarm duplication.
- A "search the library" action on history entries (search by artist,
  since a history entry carries no URI to replay directly).
- "Add to favorites" for library entries (tracks and containers alike),
  and "play all"/"add all to queue" for Favorites, matching actions the
  library already had.
- A fixed volume / line-out toggle in the sound popover, for a device
  feeding a receiver/amp with its own volume control.
- A proper app icon, replacing the placeholder — Sonos' black/white
  palette with a wifi/broadcast-wave motif clipped inside a play-triangle
  silhouette.

### Changed
- Real downloaded images (album art, service icons, artist photos) no
  longer display larger than fallback icons in the same grid or list.

### Fixed
- Local library browsing coming back empty after visiting a third-party
  service and then jumping to a local category from the sidebar.
- A crash when adjusting the new icon-size setting while viewing a large
  grid (e.g. a linked service's full Albums listing).

## [0.3.6] - 2026-08-23

### Added
- The Now Playing panel moved from a side panel to a full-width bottom bar,
  with the seek bar getting its own dedicated, much wider row (capped at a
  sane maximum width on ultrawide monitors, matching GNOME Music's own
  approach).
- "Zurück" now restarts the current track if a few seconds in, instead of
  always skipping to the actual previous track.
- "Gruppe auflösen" in the grouping popover, symmetric to the existing
  "Alle Räume gruppieren".
- The mute button icon now reflects the actual volume level (low/medium/
  high/muted); the volume slider shows a percentage tooltip.
- An "Album suchen" button alongside "Interpret suchen" in the track
  details dialog.
- A grid/list toggle for the library (Albums/Artists and similar levels),
  available for the local library and third-party services alike, with a
  single persisted preference.
- A section sidebar (Warteschlange/Favoriten/Alarme/Verlauf/Bibliothek)
  replacing the previous top tab switcher, styled after noson-app's own
  left-hand navigation. The library's own root categories (Interpreten,
  Alben, Genres, Titel, Playlisten, Radiosender, and any linked
  third-party services) are listed directly underneath "Bibliothek" as
  sub-items, so you can jump straight to one.
- Room/zone selection moved out of a permanent sidebar into a compact
  picker in the header bar, always showing the current room's name.

### Changed
- Every round transport/icon button (play/pause, shuffle, repeat, prev/
  next, favorite, mute) now gets an explicit equal width/height and center
  alignment, so a fixed-height bar can no longer stretch one into an oval.
- The player bar's info/transport/volume row is now truly centered
  regardless of the two side groups' relative widths.
- Grid eligibility for third-party service listings (e.g. bonob's Albums)
  no longer depends solely on the service's own, often-missing
  `displayType` hint — a container entry with real cover art now also
  qualifies.

### Fixed
- A layout jump in the player bar while seeking, caused by a momentary
  `Transitioning` transport state briefly hiding the "Weiter: …" hint.
- The room picker popover closing itself in the same instant it opened,
  making the header-bar room button appear completely unresponsive.

## [0.3.5] - 2026-08-23

### Added
- Repeat-one, alongside the existing off/repeat-all toggle.
- MPRIS `Shuffle`/`LoopStatus` are now exposed as real, read-write
  properties (previously not exposed at all), so external media controls
  can read and set them too.
- Suspend/resume resilience: a subscription renewal and a fresh discovery
  are forced right after the system wakes up, instead of waiting out
  libnoson's own renewal timers.
- A ringing alarm now shows a toast with a "Stoppen" action, and a
  device-reported transport error shows one too — both previously had no
  visible indication at all.
- A cache for library browsing: revisiting an already-opened level (local
  library or a third-party service) is now served instantly instead of
  refetching over the network every time — by far the biggest fix for
  navigation feeling sluggish.

### Changed
- The shuffle/repeat/next/previous buttons, and their MPRIS counterparts,
  are now only enabled when the device itself reports the source supports
  them (some radio stations don't support Next/Previous at all, for
  example).
- Volume changes are now debounced, instead of sending a full round trip
  for every intermediate step while dragging the slider.
- "Wecker-Ton testen" now actually stops itself after a few seconds
  instead of playing indefinitely until manually stopped.

### Fixed
- Local library/queue/favorites browsing and search only ever fetched a
  single page — a queue, playlist, or library folder larger than that
  silently lost its tail end. Now paginated properly, local and
  third-party services alike.
- Three more places had the same false "failed to load" error already
  fixed for the queue in 0.2.2: an empty favorites list, an empty library
  level, and searching an empty level all previously showed a bogus error
  instead of just being empty.

## [0.3.4] - 2026-08-23

### Added
- "Play all"/"add all to queue" in the library view, once you've browsed
  into a track listing (e.g. an album).
- Success toasts ("added to queue", "added as next") for the existing
  add-to-queue/play-next actions on favorites and library items.
- An optional desktop notification on track change (off by default,
  Settings toggle).
- Window size and maximized state are now remembered across restarts.

### Changed
- libnoson is now vendored as a proper git submodule instead of a plain
  copy of its source — cloning the repository now needs
  `--recurse-submodules` (see README).

### Fixed
- The room sidebar and Now Playing panel could be completely invisible at
  a normal window width, only reappearing once the window was narrow
  enough to trigger their collapse — a regression from 0.3.3's adaptive
  layout work.

## [0.3.3] - 2026-08-23

### Added
- Keyboard shortcuts for volume up/down and mute.
- A "jump to current track" button in the queue toolbar.
- A button in the track details dialog to search the library for the
  current artist.
- The Now Playing panel now also collapses behind a toggle button on
  narrow windows, alongside the room sidebar.

## [0.3.2] - 2026-08-23

### Changed
- Confirmation dialogs (clear queue, delete favorite, delete alarm) now use
  `AdwAlertDialog` instead of a custom window.
- The Settings window is now built with `AdwPreferencesDialog`, matching the
  rest of GNOME's own preferences dialogs.
- The room sidebar collapses behind a toggle button on narrow windows
  instead of always taking up fixed space.

### Added
- A "Keyboard Shortcuts" dialog listing all available shortcuts.
- More keyboard shortcuts: next/previous track, focus library search,
  open Settings.
- A copy-to-clipboard button in the track details dialog.

## [0.3.1] - 2026-08-23

### Added
- A "Recently Played" tab with local playback history (Sonos has no
  built-in history of its own).
- A light/dark appearance override in Settings.
- An entry count in the library view header.
- Space bar toggles play/pause.

### Changed
- Alarms are now sorted by time instead of creation order.

## [0.3.0] - 2026-08-22

### Added
- Album and Artist views now render as a cover art grid instead of a plain
  list, for both the local library and linked services.
- Cover art is cached in memory and on disk, with a configurable size limit
  and a manual "clear cache" action in Settings.

## [0.2.5] - 2026-08-22

### Changed
- Reworked the main window around a three-pane layout (room sidebar, tabbed
  content, a dedicated Now Playing panel), replacing the earlier thin player
  bar.
- Cover art thumbnails now appear throughout the queue, favorites and
  library lists, and rooms show an icon in the sidebar and grouping popover.

### Fixed
- A crash that could occur while loading cover art thumbnails in list rows.

## [0.2.4] - 2026-08-22

### Fixed
- The "Gen 1" badge, meant to flag original first-generation hardware, never
  appeared. It was reading the wrong device property; it now checks each
  room's actual hardware model.

## [0.2.3] - 2026-08-22

### Added
- "Add to queue" and "play next" actions on favorites and library tracks.
- Alarm duration and shuffle-on-wake options; alarms can also play in rooms
  grouped with the alarm's room.
- Alarms can use a saved favorite as their wake-up sound, with a preview
  button.
- Confirmation prompts before clearing the queue or deleting a favorite or
  alarm.
- A "next track" hint in the player, and a quick "add to favorites" button
  for whatever is currently playing.
- Search within the local library.
- A per-room volume slider in the grouping popover.

## [0.2.2] - 2026-08-22

### Fixed
- A false "queue could not be loaded" error shown for a genuinely empty
  queue, and leftover rows staying visible after clearing the queue.

## [0.2.1] - 2026-08-22

### Fixed
- Relinking an already-linked service (Spotify, bonob, ...) created a
  duplicate entry instead of replacing it.
- "Remember last room" sometimes restored the wrong room after a restart.

## [0.2.0] - 2026-08-22

### Added
- Support for third-party music services (Spotify, bonob and other
  SMAPI-based services) through Sonos's own account-linking flow, including
  search within a linked service.

## [0.1.2] - 2026-08-22

### Added
- A track details dialog with larger cover art and full metadata.
- A seek/position bar for the current track.
- Drag-and-drop queue reordering, and saving the current queue as a Sonos
  playlist.
- An About dialog.

### Fixed
- Playing an item browsed directly from a room (rather than from favorites)
  could fail outright.
- The now-playing title for radio and other live streams showed a raw
  stream address instead of the station name.
- A missing icon in the header bar for line-in/digital-in sources.
- Grouping and sound-setting switches could get stuck showing the wrong
  state after a failed change.

## [0.1.1] - 2026-08-22

### Added
- MPRIS2 support, so media keys, the GNOME Quick Settings player widget and
  the lock screen can all control Gnomos.
- Sound settings: bass, treble, loudness, night mode, and the status LED.
- Sleep timer with a live remaining-time display.
- Line-in and digital-in source switching.
- The last-used room is remembered across restarts.

## [0.1.0] - 2026-08-22

### Added
- Initial release.
- Discovery of Sonos zones on the network, with a room list sidebar.
- Playback controls, volume and mute, aware of multi-room groups.
- Now playing display with cover art.
- Queue and Favorites views.
- Alarms: list, enable/disable, delete, create.
- Zone grouping and ungrouping.
