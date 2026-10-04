# Changelog

All notable changes to Gnomos are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project does not yet follow strict Semantic Versioning guarantees
(pre-1.0, so the minor version can still carry breaking changes), but the
general idea holds: a new minor version (0.x.0) marks a significant chunk of
work, patch versions (0.x.y) are smaller additions and fixes on top of it.

## [Unreleased]

### Changed
- The README is now a short product page: what Gnomos looks like, what it
  does, how it works and how to install it. The full feature list moved to
  `docs/FEATURES.md`, build instructions to `docs/BUILDING.md` and the
  translation guide to `docs/TRANSLATING.md`.

## [0.28.4] - 2026-10-02

### Fixed
- After "Play All" replaced a room's queue, the player bar, Now Playing
  and the lyrics could show a track of the old queue for several seconds
  (and again after the next skip), although the new track was already
  playing. Sonos sometimes reports stale track details in its change
  notifications; Gnomos now checks every track change against the
  speaker's current position info and uses that when they disagree.

## [0.28.3] - 2026-10-02

### Fixed
- Lyrics sometimes showed "No lyrics found." although LRCLIB has them:
  Gnomos sent its web requests without a User-Agent, which LRCLIB
  answers with an occasional 503. Requests now identify as Gnomos, and a
  failed request is no longer remembered as "no lyrics" for the session.
- When a track restarts (or you seek back into its intro), the lyrics
  scroll back to the top instead of staying where the last play left off.
- Dragging a room in the room picker showed its internal Sonos group id
  next to the pointer. Room and queue rows now drag as a picture of the
  row itself, held where you grabbed it.

## [0.28.2] - 2026-10-02

### Fixed
- Tracks without art of their own no longer show a large black note in
  the library: they show the cover of the album or playlist they were
  opened from, and anything still without art gets a generated cover
  (tracks of one album share its color).

### Changed
- New README screenshots: Now Playing, queue, album grid, room picker,
  preferences, dark mode with the record player, mini player.
- The first group on the Appearance preferences page no longer repeats
  the page title.

## [0.28.1] - 2026-10-02

### Fixed
- On smaller windows the end of the sidebar (e.g. linked services and
  "Dienst verknüpfen…") and of every page sat behind the player bar and
  couldn't be scrolled into view. The content now ends above the bar.

## [0.28.0] - 2026-10-02

### Changed
- Dialogs open inside the main window again instead of as windows of
  their own, and adapt to its size: the main window is a real
  libadwaita application window now.
- Type to search in the preferences: typing anywhere in the dialog goes
  to its search field.

### Fixed
- Synced lyrics no longer jump back a line and forward again. Sonos
  reports the position only in whole seconds, and every other report
  repeats the previous one; following each report literally made the
  clock fall back. It now runs on smoothly, steers gently toward the
  reports, ignores repeats, and only resets on a real jump such as a
  seek. Measured live: deviation from the reports within ±0.01 s, no
  backward line changes.

## [0.27.0] - 2026-10-02

### Changed
- Preferences in two columns: a sidebar with eight categories on the
  left — General, Appearance, Library, Radio, Storage, and under
  "Online Services" Lyrics, Artist Images and Scrobbling — and only the
  selected category on the right, scrolling by itself. The old dialog
  stacked up to five groups on one page and grew far too tall; now every
  category fits without scrolling except Scrobbling, barely. On narrow
  windows it turns into a category list that opens each page with a back
  button. The search field filters categories by name and by what's in
  them ("token" finds Scrobbling, "cache" finds Storage). The three
  online pages say once, at the top, that they send data to another
  service.
- The library's list mode is virtualized too (`GtkListView`), like the
  grid: levels with thousands of entries, such as all tracks, no longer
  build a row for every entry up front.

## [0.26.0] - 2026-10-02

### Added
- Gnomos is translatable: the interface is English in the source, with
  German as the first translation (`po/de.po`, all 420 strings). It
  follows the desktop language, so German desktops see no difference.
  Messages with numbers use proper plural forms, and sentences with names
  in them are whole sentences for translators instead of glued-together
  fragments.
- The desktop entry's description is translated too.

### Changed
- `gnomos-window.cpp` (over 6000 lines) is split by topic into several
  files; no change in behavior.
- The Flatpak keeps its translations inside the app instead of a separate
  locale extension, so the downloadable bundle includes them.

## [0.25.0] - 2026-10-02

### Changed
- The remaining hand-built input windows — Stream abspielen, Als Playlist
  speichern, Szenen and Szene speichern, Zu Playlist hinzufügen,
  Radiosender hinzufügen, Benachrichtigungen per Sender, Bibliothek
  durchsuchen, Dienst verknüpfen, Last.fm-Anmeldung, Geräteinfo, Alarm —
  are libadwaita dialogs now: a header bar with a close button, Escape
  closes them, and on narrow windows they come up as a bottom sheet.

### Fixed
- Three of those windows (Geräteinfo, Zu Playlist hinzufügen, Radiosender
  hinzufügen) were never freed after closing.

## [0.24.0] - 2026-10-02

### Added
- A small "playing" indicator — three bobbing bars — marks the current
  queue entry and every playing room in the room popover; on GNOME 51
  (libadwaita 1.10) also the queue's sidebar entry. Paused, the bars rest;
  with reduced motion they don't move. They show the state only: Gnomos
  never hears the audio, so they don't pretend to be a level meter.
- Sleep timer ring: while a sleep timer runs, a thin ring around the play
  button shows the share of time left, and the moon button in the header
  lights up. Timers set elsewhere (the Sonos app) are picked up too.
- A ringing alarm's notice greets you with a slowly drifting
  night-to-dawn gradient and stays until stopped or dismissed.
- In the grouping popover, a room that just joined the group slides into
  place with a short glow.

### Changed
- The mini player is a "poster" now: the cover fills the square, freely
  resizable window, and title, seek bar and controls fade in over it while
  the pointer is on it (or while nothing plays). No title bar — drag the
  cover to move it, the corner button brings the main window back. The
  play button takes the cover's color.

## [0.23.0] - 2026-10-02

### Added
- Now Playing view: clicking the player bar (or its cover, or Ctrl+I)
  slides up a sheet with a large cover, title, the playback controls and
  volume, and on wide windows a second column with the lyrics and the
  upcoming queue (click an entry to play it). It replaces the
  "Titel-Details" dialog, whose actions (copy, search artist/album, about
  the artist) moved under the title.
- Colors from the cover: the player bar gets a soft gradient and the play
  button the cover's main color, adjusted so it stays readable in light
  and dark mode. The Now Playing view sits on a blurred copy of the cover
  washed in those colors. Both crossfade on a track change.
- Time-synced lyrics: when LRCLIB has them, the current line is
  highlighted and the view follows the song; click a line to jump there.
  Scrolling yourself pauses the following for a few seconds.
- "Plattenteller" (off by default): the cover becomes a record with
  grooves and a label that turns while the music plays.
- New "Erscheinungsbild" settings for the cover colors, the blurred
  background and the record player.

### Changed
- The lyrics lookup only runs while the Now Playing view is open, as it
  did with the old dialog.

### Removed
- The "Titel-Details" dialog (see above) and its remembered size.

## [0.22.0] - 2026-10-02

### Added
- Generated covers: albums, playlists, favorites, queue and history
  entries without art of their own show a colored gradient with their
  initials instead of a large symbolic icon. The colors follow from the
  name, so the same album always looks the same.
- The library shows a spinner while a level loads, instead of the previous
  level's entry count above an empty list.

### Changed
- The library grid is a virtualized `GtkGridView`: only the tiles on
  screen exist, so the local library's 1060 albums appear at once instead
  of after several seconds. Tiles are real grid items now, reachable and
  activatable with the keyboard, and lift slightly on hover.
- The library's entry count sits under the level title instead of on a
  row of its own.
- Built against the GNOME 51 runtime (GTK 4.24, libadwaita 1.10). The
  build now asks for libadwaita 1.9 or newer, which it has silently needed
  since the sidebar moved to `AdwSidebar`.
- Keyboard shortcuts with a modifier are real accelerators now and work
  wherever the focus is. New: Ctrl+Q quits, Ctrl+W closes the window, F9
  toggles the sidebar on narrow windows, Ctrl+? opens the shortcut list,
  Ctrl+I shows the current track. Volume moved to Ctrl+Up/Down; plain
  Up/Down still change the volume, but no longer while a list, grid or
  slider has the focus, where they used to steal the arrow keys from
  keyboard navigation.
- The room popover opens towards the window instead of centered on the
  room button, which pushed it past the window's left edge.
- An open room popover no longer rebuilds its whole list every four
  seconds; only each room's now-playing line and play button update.
- Replaced the deprecated `.dim-label` style class with `.dimmed`.
- Gnomos now ships its own stylesheet (`data/style.css`, compiled in as a
  GResource), the base for the visual work in the next releases.

### Fixed
- Previous, play/pause, next and mute in the player bar and the mini
  player had no tooltip and nothing a screen reader could announce. The
  position and volume sliders are labeled now too.

## [0.21.0] - 2026-10-01

### Changed
- New application icon following the GNOME HIG: a speaker standing on a
  round plate whose rings show sound spreading through the room, in GNOME
  palette colors, plus a matching symbolic icon (there was none before) and
  PNG renderings in 48/64/128/256 px. Generated by
  `build-aux/icons/generate_icons.py`; the drafts and the reasoning are in
  `docs/icon.md`.
- New application ID `de.linuxundich.Gnomos` (was `de.christophlangner.Gnomos`,
  which was based on a domain that doesn't exist). The D-Bus name and path of
  the zone volume service follow suit (`de.linuxundich.Gnomos.Zone`), and so
  does the GNOME Shell extension, now `gnomos-volume@linuxundich.de`.
  Upgrading from a 0.20.x Flatpak installs Gnomos as a separate app: copy
  `~/.var/app/de.christophlangner.Gnomos/` to
  `~/.var/app/de.linuxundich.Gnomos/` to keep linked services, scenes and
  settings, then uninstall the old one; reinstall the Shell extension under
  its new UUID (see "Upgrading" in the README).
- The metainfo now names the developer (`de.linuxundich`), the homepage,
  and the old ID it replaces.

## [0.20.2] - 2026-09-28

### Fixed
- Navigating the local library (via the sidebar shortcuts, an in-view
  category row, or the back button) could briefly flash the previous
  level's content before the new one loaded — the view switched before
  the async browse response arrived. Now cleared and retitled
  synchronously first.

## [0.20.1] - 2026-09-28

### Fixed
- Local library and favorites album covers could silently fall back to
  the generic icon: their art URLs were resolved against the currently
  selected room's device instead of the device that actually served
  the library listing, which differ in a multi-device household.

### Added
- "Bibliothek aktualisieren" in the primary menu, triggering the same
  music library rescan the official Sonos app's own "Update Music
  Library" uses — the fix for a device's browse index and its own art
  cache having drifted out of sync (e.g. after a NAS share reconnects).

## [0.20.0] - 2026-09-24

### Added
- A compact "Mini-Player…" window, reachable from the primary menu:
  cover art, title/artist, seek bar, and previous/play-pause/next
  controls in a small standalone window. Opening it hides the full
  window (they substitute for each other rather than coexisting), and
  closing it brings the full window back. Not "always on top" — GTK4
  and Wayland give an ordinary window no way to request that.

## [0.19.1] - 2026-09-24

### Fixed
- The section sidebar showed "Bibliothek" twice in a row — once as the
  clickable top-level item, once as an unclickable section header
  directly beneath it for the library's own sub-categories.

### Changed
- The artist info and track details dialogs are now real `AdwDialog`s
  instead of plain windows, with the dialog's own header-bar close
  button replacing a separate bottom "Schließen" button in both, and
  the track details dialog's action icons consistently `.flat`-styled.

## [0.19.0] - 2026-09-22

### Added
- The zone volume slider in GNOME's Quick Settings menu now has an
  expandable "Andere Räume" list, reaching every other room in the
  household, not just the currently selected zone: an independent
  volume slider per room (uniformly sized regardless of room name
  length) and a click-to-mute icon that greys out the slider while
  muted.
- A prebuilt Flatpak bundle is now published on the GitHub Releases
  page, for installing without building from source.

### Fixed
- `SetRoomVolume()` silently did nothing when targeting a room outside
  the currently selected zone's own group (used by the grouping
  popover's per-room sliders, and now also by the Quick Settings room
  list above).

## [0.18.0] - 2026-09-15

### Added
- The currently selected Sonos zone's volume is now available as its own
  slider in GNOME's system volume Quick Settings menu, directly under the
  regular output volume slider — via a new companion GNOME Shell extension
  (`gnome-shell-extension/gnomos-volume@christophlangner.de/`, installed
  separately) talking to a small new D-Bus interface Gnomos itself now
  exposes (`de.christophlangner.Gnomos.Zone`).

## [0.17.1] - 2026-09-11

### Fixed
- Scrolling over the volume slider jumped by roughly 10% per notch
  instead of a small, controlled step.
- The volume tooltip always showed "(0 dB)" next to the percentage;
  this hardware doesn't support the dB scale at all, so it never
  showed a real reading. Removed.

## [0.17.0] - 2026-09-11

### Added
- Global keyboard shortcuts (Wiedergabe/Pause, Nächster/Vorheriger Titel,
  Überall stummschalten) via the xdg-desktop-portal GlobalShortcuts
  portal, configurable through GNOME's own "Add Keyboard Shortcuts"
  dialog and working even while Gnomos isn't focused.

### Fixed
- Running in the background (closing via the window's close button) and
  then relaunching Gnomos left every entry in the hamburger menu greyed
  out and unusable.

## [0.16.0] - 2026-09-10

### Added
- The track-change desktop notification now has Play/Pause and (when
  available) Next buttons, and clicking it anywhere brings Gnomos to
  the front.

### Fixed
- The track-change notification never actually showed cover art,
  despite being wired up to — it silently failed to serialize over
  D-Bus regardless of whether a cover was cached.

## [0.15.0] - 2026-09-10

### Changed
- The section sidebar (Warteschlange/Favoriten/Alarme/Verlauf/
  Bibliothek), the library grid/list toggle, Settings' color-scheme
  picker, and various empty-state placeholders now use current GNOME/
  libadwaita widgets (AdwSidebar, AdwToggleGroup, AdwStatusPage,
  AdwSpinner) instead of hand-built equivalents, for a more native
  look. Requires the Flatpak's GNOME runtime moving to 50.
- Grid view (Alben/Interpreten) tiles now align into a clean, uniform
  grid at any window size — previously could develop uneven padding,
  mismatched column counts, or uneven row heights depending on title
  length and window width.
- Grid view cover art is now always square, even for non-square
  source photos.

### Fixed
- Track cover art could get permanently stuck missing after one
  failed fetch (reported with bonob/PMEDIA-tagged albums).
- Lyrics search failed for tracks or albums with a release-group tag
  like "(PMEDIA)" anywhere in the title or album name, not just the
  album.
- Switching between grid and list view in Bibliothek could crash.

## [0.14.1] - 2026-09-01

### Fixed
- Searching for a specific artist or album (Titel-Details' search
  buttons, a related artist, a history entry) only found anything if
  you'd separately already browsed into the matching library section
  beforehand — it now always searches the right one.
- The "Sonos-System antwortet" spinner flickered on and off during
  ordinary playback with nothing pending — it now only appears once
  something has genuinely stayed busy for a moment, and its tooltip
  only shows while it's actually spinning.
- Geräteinfo always showed the very first room's model number
  highlighted in blue, regardless of which model it actually was.

## [0.14.0] - 2026-09-01

### Added
- Last.fm scrobbling alongside the existing ListenBrainz support: enter
  your own API key/shared secret in Settings → Allgemein → Scrobbling
  and sign in to start submitting listens. Either, both, or neither
  service can be configured independently.
- "Über den Interpreten" in Titel-Details shows the artist's Last.fm bio
  (reuses the API key already configured for Last.fm scrobbling) and
  related artists from Deezer, each searchable in the library with one
  click.
- "M3U/PLS-Playlist importieren…" in the main menu parses a picked
  playlist file, matches its entries against your library by title/
  artist, and adds whatever matched to the queue.
- Gnomos now keeps running in the background when its window is closed
  (new "Im Hintergrund weiterlaufen" toggle in Settings, on by
  default), so Last.fm/ListenBrainz scrobbling and MPRIS media-key/
  lock-screen control keep working without the window open. "Gnomos
  beenden" in the main menu fully quits.

### Fixed
- Clicking a filtered result in the artist/album grid or list (typing
  into "Filtern…") could open the wrong entry entirely — always
  whatever sat at that same screen position in the *unfiltered* list,
  rather than the filtered item actually clicked.
- Settings now reflects a Last.fm connect/disconnect immediately
  instead of only after closing and reopening the dialog.

## [0.13.0] - 2026-08-31

### Added
- ListenBrainz scrobbling: paste a personal user token into Settings →
  Allgemein → Scrobbling to start submitting listens (empty token = off).
  A track counts once played for half its length or 4 minutes, matching
  ListenBrainz's own guideline; radio and other live streams are never
  scrobbled.
- "Radiosender-Favoriten exportieren…"/"…importieren…" in the main menu
  back up and restore your radio favorites as a local JSON file.
- "Szenen…" in the main menu: save the current room grouping (and each
  room's volume) as a named preset, restorable with one click.

## [0.12.0] - 2026-08-31

### Added
- The Bibliothek gained a "Komponisten" category, browsing the local
  library's composer index alongside the existing Interpreten/Alben/
  Genres/Titel ones.
- "Stream abspielen…" in the main menu plays an arbitrary stream address
  once, without saving it as a favorite (the existing "Radiosender
  hinzufügen" always persists one).
- "Überall stummschalten" in the main menu mutes every room in the
  household at once, regardless of current grouping.
- "Radiosender hinzufügen" gained a genre filter alongside the existing
  country one, and now opens already showing the directory's most
  popular stations.
- The grouping popover's per-room volume sliders now have an "Alle
  Räume" master fader above them, scaling every room proportionally
  while preserving each room's relative balance.
- Rooms can now be regrouped by dragging one room switcher row onto
  another, in addition to the existing per-room switches in the
  grouping popover.

### Fixed
- The grouping popover's per-room volume sliders (and the new master
  fader) could end up missing a room entirely after regrouping — its
  volume was read from a list that stopped updating once a zone was
  selected, so a room that joined the group afterward was never
  reflected. Volumes are now fetched fresh whenever the popover opens,
  independent of grouping history.
- Removed two extra divider lines in the grouping popover that made it
  look cluttered, reported live after the master fader was added.

## [0.11.2] - 2026-08-31

### Fixed
- "Titel-Details" now opens at a stable, remembered size instead of one
  that swung between too short and too tall depending on whether its
  lyrics happened to already be cached — it defaults to this window's
  own height (visibly inset from its top/bottom edges, not flush with
  them) the first time, then remembers whatever size it's actually
  closed at from then on.
- The lyrics text no longer appears pre-selected/highlighted the moment
  the dialog opens.

## [0.11.1] - 2026-08-31

### Fixed
- The room switcher's per-room live status and play/pause button (new in
  0.11.0) never actually worked: reading a throwaway, unsubscribed
  player's cached transport state came back permanently empty, so every
  room looked idle regardless of what was really playing, and the
  play/pause button always just called Play() — pausing or stopping a
  room that was already playing did nothing. Both now query live
  transport state directly instead.

## [0.11.0] - 2026-08-31

### Added
- The System-Informationen for a household's own Sonos ID (System::
  GetHouseholdID()) now show up in About's own "Debug-Informationen"
  section — useful for troubleshooting a multi-household setup.
- The player bar shows a small "Crossfade aktiv" label whenever the
  current zone has crossfade turned on (read-only — this fork's
  AVTransport class has no SOAP call to toggle it, same limitation
  noson-app itself has).
- The room switcher popover now shows each room's own live now-playing
  status (title, or "Pausiert") with an inline play/pause button, so you
  can see and control what's playing in another room without switching
  into it first.
- New keyboard shortcuts: Alt+Left/Right seeks 10 seconds back/forward,
  Ctrl+J jumps the Queue view to the currently playing track.
- "Titel-Details" can show the current track's lyrics, fetched from the
  public LRCLIB API — off by default, opt-in via Einstellungen →
  Allgemein → Songtexte, with LRCLIB's own attribution link. Works for
  radio stations too: the station's rotating "now playing" text is run
  through the same spam filter already used for MPRIS/History, so ad
  breaks don't feed garbage into the lyrics search.

### Fixed
- "Titel-Details" wasn't showing cover art at all in some cases — it
  loaded art directly via GVfs, which silently fails whenever GVfs's
  http backend isn't available, the same class of problem already fixed
  for library art. Now uses the same cache-first HTTP fetch as
  everywhere else in the app.
- The room switcher popover was too narrow to show a grouped zone's own
  name in full (e.g. "Arbeitszimmer + 1", the only visual indication of
  which rooms are currently grouped) once the new live-status line and
  play/pause button also had to fit in each row.

## [0.10.0] - 2026-08-31

### Added
- Adding every favorite or every library item to the queue now batches
  up to 16 tracks per request instead of one request per track — a
  large "add all" is noticeably faster.
- The Queue view has a new "Mehrere auswählen" toggle: check any number
  of tracks and remove them all at once (with the same confirmation
  dialog "Warteschlange leeren" already has).
- The volume slider's tooltip now also shows the decibel value
  alongside the percentage.

## [0.9.0] - 2026-08-31

### Added
- "Bibliothek neu einlesen" now reports whether the scan actually
  completed or failed, instead of only ever confirming that it started.
- Geräteinfo now also shows a room's serial number and hardware
  version.
- A new "Nur erstes Genre verwenden" setting works around Sonos's own
  indexer truncating a long multi-genre ID3 tag before ever handing it
  to Gnomos, which could otherwise cut a later genre off mid-word.
- "Zu Playlist hinzufügen" can create a brand new playlist inline
  ("Neue Playlist…") instead of requiring an existing one to add to.
- Settings is now split across three tabs (Allgemein, Bibliothek,
  Radio) instead of one long stacked page.

### Fixed
- The "Bibliothek" sidebar row now jumps back to the library root when
  clicked, even from deep inside a browse — previously did nothing
  visible if you were already several levels in.
- Gen1 hardware detection (device model lookup) could silently never
  resolve under the Flatpak build — the same GVfs-unavailable-under-
  Flatpak issue already fixed elsewhere for cover art and radio search,
  missed in that pass since this fetch didn't go through either of
  those code paths.
- "Bibliothek neu einlesen" in Einstellungen → Bibliothek threw a GTK
  warning on every open (an internal row built with the wrong widget
  type for its subtitle).

## [0.8.0] - 2026-08-28

### Added
- Album tiles (local library and third-party services alike) now show the
  artist name in a smaller, dimmed line beneath the title.
- An ID3 genre tag with more than one genre packed into a single string
  (e.g. "Rap; Metal; Hard-Core") is now split into separate entries in the
  Genres view. The separator character(s) are configurable in a new
  Settings → Genres section (defaults to ";"); genres that only differ
  before splitting are merged back into one entry covering both.
- The cover art disk cache is now stored as WebP instead of whatever
  format the source served (typically JPEG/PNG), shrinking its footprint
  on disk.

### Fixed
- A linked service's (bonob, Spotify, ...) own category icons in its root
  menu (Albums, Random, Favourites, Top Rated, Recently added, Recently
  played, Most played, ...) were being discarded in favor of Gnomos's own
  fallback icon, which doesn't distinguish between most of those
  categories at all and showed the same generic note glyph for each. The
  service's own icon is used instead now.
- The per-station radio notification settings dialog couldn't be closed
  with Escape.

### Changed
- Trimmed and unified the grid spacing shared by the Albums and Artists
  views (and any other grid-eligible listing).

## [0.7.1] - 2026-08-27

### Added
- Volume sliders (the Now Playing bar and the per-room sliders in the
  grouping popover) now respond to scrolling the mouse wheel while
  hovered.

### Fixed
- In the Flatpak build specifically: radio-browser search and cover art/
  artist-photo thumbnails never loaded at all — `Gio::File`'s http(s)
  support depends on GVfs's own backend, which isn't part of the Flatpak
  runtime. Replaced with a small HTTP client backed directly by libsoup
  for all three.
- A large cover-art grid (Albums/Artists, 1000+ entries) fired every
  tile's fetch essentially at once, overwhelming the local Sonos
  device's own tiny HTTP server — most requests failed outright and
  never populated the cache, leaving tiles blank even on a later
  revisit. Fetches are now capped to a handful concurrent, and whatever
  actually scrolls into view gets bumped to the front of the queue
  (covering both a direct art fetch and, separately, the Deezer
  name-lookup stage an artist photo goes through first).
- Artist photos could get permanently stuck on the fallback icon once
  Deezer's API rate limit was hit — it signals the limit with a 200 OK
  carrying an error body rather than an HTTP 429, which looked
  indistinguishable from "no photo found" and got cached as such
  forever. The rate limit is now detected and backed off instead.
- Local album/artist cover art needed re-fetching after almost every
  restart — the on-disk cache was keyed by the full art URL, which
  bakes in the Sonos speaker's *current* IP address, so a DHCP lease
  change silently orphaned the entire cache. The cache key now ignores
  the device's address for local art specifically.
- "Dienst verknüpfen…" (linking Spotify, bonob, …) had no way to be
  found in the sidebar at all unless you happened to click the
  top-level "Bibliothek" row itself rather than any of its sub-items —
  it now shows as a regular entry under "Dienste".
- The search field in both of the app's searchable dropdowns (the
  service-link picker, the alarm sound picker) never actually filtered
  anything typed into it.

## [0.7.0] - 2026-08-24

### Added
- Per-station MPRIS control for radio: a switch to opt a station out of
  MPRIS reporting entirely, and an optional regex filter so only content
  matching a station's own "song / artist" format counts as a real song —
  ad breaks and station idents in between no longer retrigger MPRIS
  clients like GNOME Shell's media notification. Reached from a new gear
  icon on each row while browsing "Radiosender".
- The library sidebar now splits its sub-items into "Bibliothek" (local
  network-share content) and "Dienste" (Sonos playlists, Radiosender,
  linked third-party services).

### Fixed
- A linked service's name (e.g. bonob) is now capitalized everywhere it's
  shown, instead of exactly as that service reports it.
- The Flatpak build was missing the D-Bus permission to own its own MPRIS
  bus name, so MPRIS silently never worked in the packaged build even
  though it worked in a native build.

## [0.6.0] - 2026-08-24

### Added
- A header-bar spinner that shows whenever the app is waiting on a
  response from the Sonos system, not just during zone discovery.
- The room picker's "Geräteinfo" dialog now shows every device in a
  merged zone, not just the one it was originally opened from.
- A verified Flatpak manifest (`build-aux/flatpak/`) — not published, but
  built and installed sporadically by the author for local testing.

### Changed
- Joining a room to a group now requires it to be free first — a room
  already merged into another group can no longer be joined directly;
  it has to be removed from its current group first, then added to the
  new one as a separate step. Applies to both the per-room switches and
  "Alle Räume gruppieren".

### Fixed
- A room newly joined to a group could have no volume slider at all in
  the grouping popover until something unrelated happened to refresh
  it.
- The grouping popover's room list forced scrolling for as few as 4
  rooms.
- The sound popover's "Erweitert" section used a disclosure widget that
  didn't visually match the rest of the popover.

## [0.5.2] - 2026-08-24

### Fixed
- A radio station's icon in the bottom Now Playing bar could get
  permanently stuck on the generic fallback, even for a station whose
  thumbnail displayed correctly everywhere else in the library.

## [0.5.1] - 2026-08-24

### Added
- Real thumbnails for custom radio stations added via the Radio-Browser
  search — both in the library listing and the bottom Now Playing bar
  while one is playing.
- A link to the GitHub project (and its issue tracker) in the About
  dialog, plus a credit for the Radio Browser project.

## [0.5.0] - 2026-08-24

### Added
- Deleting a custom internet radio station, alongside the existing
  ability to add one.
- Adding a library track to an existing saved playlist, and reordering
  tracks within one.
- Line-in autoplay settings (enable, target volume) in the sound
  popover.
- A "Bibliothek neu einlesen" action in Settings, for rescanning an
  indexed local music share after adding files to it.
- Radio station search against radio-browser.info's public directory
  (country filter, name search, results list), replacing manual name/
  URL entry as the primary way to add a station — manual entry is still
  available as a fallback.
- A "play now" button on each radio station row.

### Fixed
- A freshly added or deleted radio station, a deleted playlist, or a
  playlist edited via the two features above didn't show up until the
  library cache happened to expire on its own (up to 5 minutes).
- Several actions that don't make sense for a live radio stream (add to
  queue, play next, add to playlist, bulk add-all-to-queue, bulk play
  all) were shown on radio station rows anyway and failed on click —
  removed for the Radiosender section.
- Cover art briefly flickering between real art and the fallback icon a
  few times right after launch.

## [0.4.1] - 2026-08-23

### Fixed
- Switching to a very large grid (1000+ entries, e.g. the local library's
  or a linked service's full "Alben" listing) took several seconds.
  Cover art decoding now happens off the main thread instead of blocking
  it once per tile.

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
