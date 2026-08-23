# Architecture and implementation notes

This document covers how Gnomos is built internally, why some things are
done the way they are, and a log of real bugs found while testing against
actual first-generation Sonos hardware. It's aimed at anyone reading or
extending the code; if you just want to use the app, see [README.md](README.md).

## Overview

- `noson/` — [libnoson](https://github.com/janbar/noson) as a git
  submodule, an unmodified checkout of upstream (which only ships a CMake
  build). The meson build for it lives in `noson-meson/meson.build`
  instead, kept outside the submodule on purpose so `noson/` never needs
  its own commits — see that file's own header comment. It mirrors
  `noson/noson/CMakeLists.txt` and builds an internal static library; the
  local-audio-streaming feature (FLAC/PulseAudio source) is deliberately
  not built, since a pure remote control never needs to originate audio
  itself.
- `src/backend/noson-backend.{h,cpp}` — the only place that touches
  `NSROOT::` (libnoson) directly. Everything else in `src/` only sees
  plain-data types from `noson-types.h` and sigc++ signals.
- `src/widgets/` — the view widgets (`PlayerBar`, `QueueView`,
  `FavoritesView`, `AlarmsView`, `LibraryView`, `HistoryView`, ...). These
  are pure views: they emit signals for user actions and expose
  `Update()`/`SetItems()`-style setters, with no knowledge of libnoson.
- `src/gnomos-window.{h,cpp}` — wires the backend to the widgets and owns
  the window layout: a room sidebar (`AdwOverlaySplitView`, collapsible on
  narrow windows) beside tabbed Queue/Favorites/Alarms/History/Library
  content, with `PlayerBar` docked as a fixed-height bar along the bottom
  of the whole window (`root_box` in the constructor). Two stacked rows:
  a full-width seek bar (wrapped in an `AdwClamp`, capped at 1000px so it
  doesn't stretch absurdly long on an ultrawide monitor) with elapsed/
  duration labels flanking its ends, then cover art + title/artist on the
  left, transport controls centered, favorite/mute/volume on the right.
  This replaced an earlier design with `PlayerBar` as a wide side panel
  (structurally inspired by [Euphonica](https://github.com/htkhiem/euphonica)'s
  own Now Playing panel); moved to a bottom bar on request, mainly to give
  the seek bar real usable width instead of the ~300px a side column
  could ever offer it — the current two-row structure (seek bar on its
  own row above everything else, not squeezed into a center column)
  mirrors Euphonica's own bottom bar screenshot more directly than the
  first bottom-bar attempt did. `AdwClamp`-capping the seek row instead of
  letting it grow unbounded mirrors GNOME Music's own `PlayerToolbar`
  (`data/ui/PlayerToolbar.ui`), which wraps its equivalent row the same
  way. Every round icon button in `PlayerBar` gets an explicit *equal*
  width/height `set_size_request()` plus `valign(CENTER)` (never the
  `Gtk::Box` default of `FILL`) — without both, the bar's own fixed
  height stretches a button into an oval instead of a circle.
- No `.ui` templates: everything is built in C++. That was a deliberate
  call to reduce risk in an early, largely-unverified first pass — an
  XML/C++ ID mismatch is a class of bug this sidesteps entirely. Worth
  revisiting as the UI grows.

### Why there's no `Adw::` C++ namespace

gtkmm has no official libadwaita bindings (libadwaita isn't part of core
GTK). The approach used here is to construct Adwaita widgets via their C
API (`adw_header_bar_new()`, `adw_toast_overlay_new()`, `adw_alert_dialog_new()`,
...) and use `Glib::wrap()` only to get a `Gtk::Widget*` handle for packing
into gtkmm containers; Adwaita-specific behavior is driven through the raw
`ADW_*(...)` pointers kept as members on `GnomosWindow`. `AdwApplicationWindow`
itself is intentionally skipped in favor of a plain `Gtk::ApplicationWindow`
— `adw_init()` installs Adwaita's styling globally regardless of the
window's concrete type, and a window built entirely through gtkmm's own
API is less likely to have gotten something subtly wrong than one reached
only through the C-API-plus-wrap fallback.

### Threading model (why `TaskQueue` and `Glib::Dispatcher` exist)

libnoson's actions (`Play()`, `SetVolume()`, `Discover()`, `Browse()`, ...)
are synchronous, blocking HTTP/SOAP calls, and it invokes event callbacks
(track changed, volume changed, topology changed, ...) on its own internal
threads (one thread per subscribed service). Neither can happen on the GTK
main thread:

- Every write action is pushed onto `TaskQueue`, a single background worker
  thread that runs tasks one at a time, so overlapping user actions (e.g.
  dragging the volume slider) can't race each other on the same connection.
- Every libnoson event callback only wakes a `Glib::Dispatcher`; the actual
  state refresh and signal emission happen in the dispatcher's connected
  slot, which `Glib::Dispatcher` guarantees runs on the GTK main thread.

See the destruction-order comment in `noson-backend.h` for how this affects
member ordering in that class — it's the one place a "reasonable-looking"
reordering would introduce a use-after-free.

### Backend reliability

- **Pagination.** `NSROOT::ContentBrowser`'s constructor only fetches a
  single page (its own `BROWSE_COUNT` default is 100; the call sites in
  `noson-backend.cpp` pass a larger but still fixed count). A queue,
  playlist, or library folder bigger than that silently lost its tail end
  without further work — `ExhaustBrowser()` (an internal helper in
  `noson-backend.cpp`) grows the browser's window to the real reported
  total in a bounded loop. `SMAPI::GetMetadata()`/`Search()` have the same
  single-page limitation but a different shape (each call *replaces* the
  out-param rather than a browser accumulating internally), so
  `BrowseActiveServiceLocked()`/`SearchActiveServiceAsync()` page through
  them by hand, accumulating into their own `entries`/`raw` vectors. All of
  this is capped at `kMaxBrowseItems` (5000) so a pathological folder can't
  make a refresh hang or eat unbounded memory.
- **False "failed to load" on an empty container.** `ContentBrowser`'s
  public API can't tell "the fetch genuinely failed" apart from "the fetch
  succeeded and the container is just empty" — browsing index 0 of a
  totally empty container looks identical to an out-of-range/failed
  browse. `RefreshQueueAsync()` already worked around this (see bug #7
  below); the same fix — trusting the constructor's own fetch instead of a
  redundant `Browse()` call that would return `false` for an empty result
  — was also applied to `RefreshFavoritesAsync()`, `BrowseLibraryAsync()`,
  and `SearchLocalLibraryAsync()`, which had the identical bug (a
  household with zero favorites, or browsing into an empty library level,
  showed a spurious error toast).
- **Suspend/resume.** libnoston's own UPnP-eventing subscriptions already
  self-renew on their own timers (see `subscription.cpp`'s background
  thread), but that can leave the app showing stale state for however long
  is left on the current renewal cycle after a laptop wakes from suspend.
  `NosonBackend` subscribes to logind's `PrepareForSleep` D-Bus signal
  (system bus) and, on resume, calls `System::RenewSubscriptions()` plus
  the same `DiscoverAsync()` path startup itself uses — including its
  existing "restore the previously selected room" handling. Best-effort:
  if the system bus or logind aren't reachable, this silently does
  nothing rather than failing startup.
- **Repeat-one.** `PlayMode_t` has a `PlayMode_REPEAT_ONE` value that
  nothing previously ever produced or read — `ToggleRepeat()` only cycled
  Off/All. `NowPlaying::repeat` is now a tri-state `RepeatMode` (Off/All/
  One) instead of a bool, `ToggleRepeat()` cycles all three, and
  `SetRepeatMode()`/`SetShuffle()` (absolute-value counterparts, for
  MPRIS) reconcile the two independent dimensions into Sonos's single
  combined `PlayMode_t` — there's no shuffle+repeat-one combination on the
  device, so requesting one falls back to shuffle+repeat-all rather than
  silently dropping the request.
- **Shuffle/repeat button sensitivity.** Not every source supports
  shuffle/repeat (radio, line-in) — `AVTProperty::r_CurrentValidPlayModes`
  reports which modes are actually valid for what's currently playing, and
  `PlayerBar` now disables the corresponding button when the device says a
  mode isn't supported, instead of leaving it clickable and silently
  failing.
- **MPRIS `Shuffle`/`LoopStatus`.** Previously not exposed on the MPRIS
  interface at all, not even read-only. Both are now full read-write
  properties, backed by `SetShuffle()`/`SetRepeatMode()` — confirmed live
  via `gdbus call ... org.freedesktop.DBus.Properties.GetAll` showing real
  `Shuffle`/`LoopStatus` values alongside the rest of the playing track's
  metadata.
- **Transport action capability.** Same idea as shuffle/repeat above, for
  `AVTProperty::CurrentTransportActions` ("Set, Play, Stop, Pause, Seek,
  Next, Previous" or a subset) — some radio stations don't support Next/
  Previous/Pause at all. `NowPlaying::can_go_next`/`can_go_previous`/
  `can_pause` now gate both `PlayerBar`'s buttons and MPRIS's
  `CanGoNext`/`CanGoPrevious`/`CanPause`, which previously just mirrored
  `np.valid` regardless of what the source actually supported.
- **Ringing-alarm detection.** `AVTProperty::r_AlarmRunning` was read by
  nothing — an alarm going off was only ever noticeable by actually
  hearing it. `GnomosWindow` now edge-triggers a toast ("Wecker klingelt")
  with a "Stoppen" action (`win.stop-alarm`, wired to the same
  `PauseOrStop()` the play/pause button uses) the moment it starts, and
  separately toasts once if `AVTProperty::TransportStatus` ever reports
  anything other than `"OK"`.
- **Volume debounce.** Dragging the volume slider fired `SetVolume()`
  many times a second, and each call was a full read-every-member-then-
  scale-every-member round trip — every intermediate drag step queued its
  own trip on `TaskQueue`, leaving the device visibly lagging behind the
  slider for a while after the user had already stopped dragging.
  `SetVolume()`/`SetRoomVolume()` now debounce through a short
  (150ms) `Glib::signal_timeout()`, collapsing a drag into the single
  final value.
- **Alarm sound preview never stopped itself.** `PreviewAlarmSound()`
  started playback and just left it running indefinitely — the "Wecker-Ton
  testen" button's own name implies a brief preview, but nothing actually
  made it brief. It now sleeps 8 seconds on the same `TaskQueue` task
  before stopping the room itself, rather than a separate timer that could
  fire after the room moved on to something else — the task ordering
  guarantee `TaskQueue` already provides means any other action on that
  room queues up behind it and only runs after, so this can't race a
  meanwhile-started unrelated playback.
- **Library browse cache.** Every navigation into or back out of a level
  — local library or a third-party service alike — was a fresh network
  round trip, even revisiting a level browsed moments ago; this was the
  single biggest contributor to browsing feeling sluggish.
  `BrowseLibraryAsync()`/`BrowseActiveServiceLocked()` now serve an
  already-fetched level straight from `library_cache_` (keyed by
  object_id, or `"<service id>\x1f<object_id>"` inside a service — a bare
  object_id isn't necessarily unique across services) instead of
  re-fetching. The local-library side is invalidated outright on a real
  `SVCEvent_ContentDirectoryChanged` (new music scanned, a playlist
  edited elsewhere); third-party services have no equivalent
  change-notification Gnomos can act on, so those entries instead expire
  on a 5-minute TTL. Capped at 300 cached levels (dropped outright past
  that, rather than real LRU bookkeeping) so a very long session can't
  grow this unbounded.

### Player bar and grouping additions

- **"Previous" restarts the current track once a few seconds in**,
  instead of always skipping to the actual previous track — the same
  convention most media players use. Implemented as a plain `SeekTime(0)`
  in `NosonBackend::Previous()` when `position_` is past a 3-second
  threshold, falling back to a real `Player::Previous()` otherwise.
- **"Gruppe auflösen"** in the grouping popover, symmetric to the existing
  "Alle Räume gruppieren" — removes every other member of the current
  group via the same `RemoveRoomFromGroup()` each room's own switch
  already calls, leaving just the coordinator.
- The mute button icon now reflects the actual volume level
  (low/medium/high/muted thresholds, matching the system volume icon
  convention) instead of always showing "high" for any unmuted level: a
  volume slider tooltip now also shows the percentage.
- The track details dialog's existing "Interpret suchen" button now has an
  "Album suchen" counterpart alongside it.
- The `bar` row (info / transport / volume) is now a `Gtk::CenterBox`
  instead of a plain hexpand()'d `Gtk::Box` — a plain Box only centers
  `transport_row` within whatever space is left between `info_box` and
  `secondary_row`, which visibly drifted off the bar's true center
  whenever those two differed in width (`info_box`, with cover art +
  title/artist, is almost always the wider one). `Gtk::CenterBox` keeps
  its center child at the actual visual center of the whole bar
  regardless, the same guarantee GNOME Music's own `PlayerToolbar` gets
  from `GtkActionBar`.
- Fixed a layout jump in the player bar while seeking: a seek (not just a
  real track change) briefly puts the transport into
  `TransportState::Transitioning`, during which
  `AVTProperty::CurrentTrack`/`AVTransportURI` are unreliable — so
  `RefreshNowPlayingLocked()` already forces `playing_from_queue = false`
  for that one event (see the bug list below). `OnNowPlayingChanged()`
  was applying that momentary `false` directly to `current_queue_index_`,
  which hid the "Weiter: …" hint and the queue highlight for one event
  and then restored them once the next, settled event arrived — a visible
  jump confirmed live while dragging the seek bar. Fixed by skipping the
  `current_queue_index_` update entirely while `Transitioning`, keeping
  the previous (correct) value on screen until a settled event confirms
  the real one, rather than ever showing a known-wrong intermediate value.

### Library grid/list: one signal, user-togglable

Album/Artist grid display used to be decided by two different code paths
in `GnomosWindow::OnLibraryChanged()`, branching on whether the current
level was inside a third-party service or the local library, with no way
for the user to override either. Unified into one signal and a real
toggle:

- `LibraryEntry::display_as_grid` is now populated for **every** entry,
  regardless of source, not just SMAPI ones. Third-party services set it
  from `SMAPIItem::displayType == Grid` **or** simply `is_container` with
  real cover art (`BrowseActiveServiceLocked()`/`SearchActiveServiceAsync()`)
  — not displayType alone: confirmed live against a real bonob server,
  its own "Albums" listing carries real per-album art but never sets
  displayType to Grid at all (only its root menu does), which left an
  obviously grid-worthy listing with no way to switch it at all. The
  local library still has no per-item hint the way SMAPI's displayType
  gives services, so `BrowseLibraryAsync()` derives it from the level's
  own object_id prefix (`"A:ALBUM"`/`"A:ALBUMARTIST"`) combined with
  `is_container`, computed once per level and applied to every entry in
  it — deliberately *not* switched to the same "has art" rule the SMAPI
  side uses, since plenty of real local libraries have artist entries
  with no photo at all and would otherwise lose the Artists grid
  entirely. Different underlying heuristics, because they're genuinely
  different data sources with different failure modes — but one uniform
  field to consume either way.
- `OnLibraryChanged()` no longer branches on where the level came from at
  all: it just checks `std::any_of(entries, [](e){ return
  e.display_as_grid; })` to decide whether a grid is *available* for this
  level, unconditionally.
- Whether to actually *render* one when available is now the user's own
  choice: `LibraryView` gained `view_mode_button_` (only shown when
  `grid_available` is true), and `GnomosWindow::prefer_grid_view_` (a
  single global preference, not per-level — simpler to reason about and
  persist, and matches how a single on/off toggle button reads) persisted
  to `state.ini`'s `[library]` group. Toggling it re-renders the
  already-fetched `current_library_entries_` locally
  (`LibraryView::SetEntries()` again) — no new network fetch, since this
  is purely a rendering choice, not a data one.

### Section sidebar replaces the room sidebar; room picking moved to a popover

The permanent left sidebar used to be a room/zone list — one `AdwOverlaySplitView`
slot, occupied by `zones_scroller_`, with page navigation (Warteschlange/
Favoriten/Alarme/Verlauf/Bibliothek) handled separately by a top
`AdwViewSwitcher` tab bar. Reorganized after the user pointed at noson-app's
own left-hand navigation (Meine Dienste/Mein Musikverzeichnis/Meine
Radiosender/Favoriten/Wiedergabelisten/Wecker/Dieses Gerät) as the model to
follow instead:

- The sidebar slot in `split_view_` is now `nav_list_box_`, a
  `navigation-sidebar`-styled `Gtk::ListBox` with one icon+label row per
  `view_stack_` page, built from the same five `(name, title, icon)` tuples
  `adw_view_stack_add_titled_with_icon()` already uses — so the two can't
  drift apart. Selecting a row (`OnNavRowSelected()`) just calls
  `adw_view_stack_set_visible_child_name()`, the same job the removed
  `AdwViewSwitcher` used to do internally. `view_stack_` is now
  `split_view_`'s content directly — the `content_box_` wrapper that used to
  hold `view_switcher` + `view_stack_` together no longer has a reason to
  exist and was removed from `GnomosWindow`'s member list entirely.
- Room/zone selection moved out of the permanent sidebar into a header-bar
  `room_button_` (an `AdwButtonContent`-based `Gtk::MenuButton`, same
  reasoning as `grouping_button_`: no gtkmm binding for `AdwButtonContent`
  exists, so it's built via the raw C API and wrapped) with a `room_popover_`
  popover. The popover's content is the *exact same* `zones_scroller_` /
  `zones_list_box_` pair the permanent sidebar used to own — same
  `OnZoneRowSelected()`/`OnZonesChanged()` row-building logic, just
  re-parented and re-sized for a popover (`set_size_request(260, -1)` +
  `set_max_content_height(400)` + `set_propagate_natural_height(true)`,
  mirroring `grouping_popover_`'s own room list, instead of the
  `set_vexpand(true)`/`set_min_content_width(220)` sizing appropriate for a
  permanent docked panel).
- `room_button_`'s own label always shows the current room name — a
  button that only carried an icon would leave the room invisible without
  opening the popover. `UpdateRoomButtonLabel()` looks up
  `current_zones_` for the entry matching `selected_group_id_` and calls
  `adw_button_content_set_label()`, falling back to "Kein Raum" when
  nothing is selected; called from both `OnZoneRowSelected()` (after a
  manual pick) and `OnZonesChanged()` (after a topology refresh, including
  the empty-zone-list case where `OnZoneRowSelected()` is never triggered).
  `OnZoneRowSelected()` also closes the popover (`room_popover_.popdown()`)
  after a pick, matching how a `Gtk::DropDown` or menu item closes itself
  on selection.
- A Sonos household only ever needs *occasional* room switching, not a
  permanently-docked panel for it — freeing the sidebar slot for section
  navigation instead directly mirrors how noson-app itself splits these two
  concerns into a left nav list and a separate room switcher.

## Bugs found during hardware testing

All of the following were found by running Gnomos against a real
first-generation Sonos household, not just by reading the code, and are
fixed as of the version noted:

1. **Playing a directly-browsed item (e.g. a TuneIn radio station) failed
   with an HTTP 500.** Items recovered from a favorite always carry a
   `<desc>` service token (`System::AddURIToFavorites` attaches one at
   creation time); items straight from `ContentDirectory::Browse()` (e.g.
   the local radio directory `"R:0/0"`) don't, and Sonos rejects
   `SetAVTransportURI` without one. Fixed by attaching the generic
   `RINCON_AssociatedZPUDN` token when an item has none, mirroring
   `AddURIToFavorites`'s own fallback.
2. **The now-playing title showed the raw stream URI** (e.g.
   `x-sonosapi-stream:s24896?...`) instead of the station name for radio.
   `CurrentTrackMetaData`'s `dc:title` is unreliable for a live stream; the
   correct source is `AVTProperty::r_EnqueuedTransportURIMetaData` (what was
   set when starting playback), with `CurrentTrackDuration == 0` as the
   signal to use it instead of the regular per-track fields.
3. **The input-source header bar icon showed a missing-icon placeholder.**
   `audio-input-line-in-symbolic` doesn't exist in the Adwaita icon theme.
   Switched to `audio-input-microphone-symbolic`.
4. **Grouping and sound-setting switches could get stuck showing the wrong
   state after a failed action** (e.g. toggling night mode on a model that
   doesn't support it): they optimistically confirmed the new state right
   after firing the action, before knowing whether it actually succeeded.
   Fixed by only confirming state once a real refresh comes back.
5. **Relinking a service (Spotify, bonob) after it was already linked
   listed it twice.** The credential store only deduped on an exact
   `(type, serial number)` match, and relinking mints a fresh serial number
   every time. Fixed by purging any existing accounts for that service type
   before storing the new one, and always registering under a fixed serial
   number.
6. **"Remember last room" always restored the first room in the list,
   never whichever one was actually last active.** The persistence code
   keyed on a prefix of the zone's group id, which — confirmed with two
   simultaneously active, independent zones — is not reliably unique per
   room. Fixed by keying on the zone coordinator's own stable UUID instead.
7. **After "clear queue", the queue view still showed the old contents,
   and deleting one of those leftover rows failed**, and separately, an
   actually-empty queue showed a "queue could not be loaded" error on
   every refresh, including at plain app startup with no clear involved.
   Both trace back to a libnoson quirk: browsing index 0 of a genuinely
   empty container is treated the same as a real fetch failure. Fixed by
   trusting the already-fetched result instead of re-browsing, and by
   reflecting a known-empty queue immediately after clearing rather than
   asking the device again.
8. **The "Gen 1" badge never showed up on any zone.** It was matching the
   device's room icon (e.g. `"living"`, `"masterbedroom"`) against a
   hand-maintained token list — but the room icon is user-assigned and has
   nothing to do with the hardware model. Fixed by fetching each room's own
   UPnP device description and reading the real model number.
9. **A real segfault** crashed the app soon after cover art thumbnails were
   added to list rows. Row thumbnail widgets get destroyed and rebuilt
   wholesale on every list refresh; if an async art load was still in
   flight when its row was torn down, the completion callback fired later
   on a widget that no longer existed — cancelling the download doesn't
   prevent an already-scheduled callback from still being invoked. Fixed
   with a shared "is this widget still alive" flag, allocated independently
   of the widget itself and checked before the callback touches anything.

`GNOMOS_DEBUG=<0-6>` (see `main.cpp`) turns on libnoson's own SOAP
request/response logging to stderr — level 4 is what found bug #1 above (an
otherwise-silent `HTTP/1.1 500` on the `AVTransport` SOAP call).

## Verified vs. still-unverified

Confirmed against real first-generation Sonos hardware on the author's own
network, in addition to a clean native build (Arch Linux, GTK4 4.22 /
libadwaita 1.9 / gtkmm-4.0 4.22):
- Discovery, zone list (including a real grouped zone), transport
  controls, volume, and cover art all work end to end.
- DIDL metadata keys (`dc:title`, `dc:creator`, `upnp:album`,
  `upnp:albumArtURI`) are correct against real device XML.
- Zone grouping/ungrouping works.
- Favorites list and play-favorite work.
- Alarms: list/enable/disable/delete/create all confirmed working live.

Still unverified:
- **Flatpak manifest tags/module list** (`build-aux/flatpak/`) — written
  without a full build attempt; expect to fix version pins.
- Queue reordering via drag and drop is the one feature that's genuinely
  hard to test without a human hand on a mouse, so it's the least-verified
  piece of UI in the app.
