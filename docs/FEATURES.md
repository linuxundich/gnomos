# Gnomos features

The full list of what Gnomos does. For an overview, see the [README](../README.md).

- Discovery of Sonos zones on the local network, with a header-bar
  spinner showing whenever the app is actually waiting on a response
  from the Sonos system
- Playback controls, volume and mute — aware of multi-room groups, not just
  a single speaker
- Shuffle and repeat, including repeat-one, with both greyed out on
  sources that don't support them (radio, line-in)
- Zone grouping and ungrouping (including a one-click "disband group"),
  with a per-room volume slider
- A bottom Now Playing bar with cover art and a wide seek bar, tinted in
  the colors of the current cover
- A Now Playing view that slides up from the bar (click it, or Ctrl+I): a
  large cover on a blurred, cover-colored background, the controls, quick
  links to search the library for the artist or album, the upcoming queue,
  and lyrics from LRCLIB (opt-in) — time-synced where LRCLIB has them, with
  the current line highlighted and clickable lines to jump there. An
  optional record-player look turns the cover into a spinning record
- Little touches: animated "playing" bars for the current track and
  playing rooms, a sleep timer ring around the play button, a sunrise
  greeting for a ringing alarm, rooms sliding into a group
- A poster-style mini player: the cover fills a small square window, the
  controls fade in on hover
- Queue management: reordering, removing tracks, saving as a Sonos playlist
- Favorites, with search, "add to favorites" from anywhere in the library,
  and "play all"/"add all to queue" for the whole list
- Alarms: create, edit, duplicate, enable/disable, delete, with a sound
  preview and a "next alarm" indicator
- A play history tab (tracked locally, since Sonos doesn't keep one), with
  a quick "search the library" action per entry
- Local music library browsing, with a toggle between list and cover art
  grid for Albums/Artists and similar (local and third-party services
  alike), consistent GNOME iconography per category (artist/album/genre/
  playlist) — while a linked service's own root-menu categories (Albums,
  Random, Favourites, Top Rated, ...) keep that service's own icon
  instead — a live filter for narrowing down a long list as you type,
  "play all"/"add all to queue" for a track listing, adding a custom
  internet radio station (searched for by name/country against
  radio-browser.info's public directory, complete with a thumbnail where
  available, with manual name+URL entry as a fallback) and deleting one,
  deleting a saved Sonos playlist, adding a track to an existing saved
  playlist and reordering its tracks, and a cache (art compressed to WebP
  on disk) so revisiting a level doesn't refetch it over the network
  every time
- Album tiles show the artist name beneath the title, in the grid and the
  list view alike
- An ID3 genre tag with several genres packed into one string (e.g.
  "Rap; Metal; Hard-Core") is split into separate entries in the Genres
  view, with the separator character(s) configurable in Settings →
  Genres
- Optional real artist photos in the library, looked up by name against
  Deezer's public API — off by default, since it's the one thing in this
  app that leaves the local Sonos household (Settings → Bibliothek)
- Fixed volume / line-out mode and sub gain, alongside bass, treble,
  loudness, night mode and line-in autoplay (with its own target volume),
  for a device feeding a receiver or amp with its own volume control, or a
  paired Sonos Sub
- A "Bibliothek neu einlesen" action in Settings, for rescanning an
  indexed local music share after adding files to it
- A "Geräteinfo" dialog per room (IP, MAC, software version, model),
  reached from the room picker's popover
- Third-party services (Spotify, bonob and other SMAPI-based services)
  through Sonos's own account-linking flow
- MPRIS2 integration, so GNOME's media keys, the Quick Settings player
  widget and the lock screen all work with Gnomos like any other player,
  including setting shuffle/repeat from there
- An optional companion GNOME Shell extension
  (`gnome-shell-extension/gnomos-volume@linuxundich.de`, installed
  separately) that adds the current zone's volume as its own slider in the
  system volume Quick Settings menu, with an expandable list reaching
  every other room too — an independent volume slider and click-to-mute
  icon per room
- A "Gen 1" badge identifying original first-generation hardware in a room
- Light/dark appearance override, adjustable cover art cache size
- Keyboard shortcuts for play/pause, next/previous, volume, mute, shuffle
  and repeat, plus Ctrl+F (search), Ctrl+J (current track), F9 (sidebar),
  Ctrl+W/Ctrl+Q; the full list is under "Keyboard Shortcuts" (Ctrl+?)
- A fast, virtualized cover grid for large libraries, and generated
  covers (a gradient with the initials) for anything without art
- A section sidebar (Queue, Favorites, Alarms, History, Library)
  in the style of noson-app's own navigation, plus a compact room/zone
  picker in the header bar; the library's own root categories (Artists,
  Albums, Genres, Radio Stations, linked services, ...) are listed right there
  as sub-items, for jumping straight to one without browsing in first
- A responsive window: the section sidebar tucks away behind a toggle
  button once the window gets narrow, and both the window's and the
  sidebar's size are remembered across restarts
- Optional desktop notifications on track change
- Translatable (gettext): English and German included, following the
  desktop language

## Hardware support

Gnomos should work with any Sonos system reachable on your local network,
old or new. The "Gen 1" badge specifically flags ZP80, ZP90, ZP100, ZP120
and CR100 devices, since those are the ones this project is really written
for — everything else is a byproduct of controlling a Sonos system in
general.
