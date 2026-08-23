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
  narrow windows), tabbed Queue/Favorites/Alarms/History/Library content,
  and `PlayerBar` as a wide Now Playing side panel — structurally inspired
  by [Euphonica](https://github.com/htkhiem/euphonica)'s own dedicated Now
  Playing panel (large circular art, centered transport controls below it),
  without adopting its dynamic accent colors, background blur, or lyrics —
  none of those have a matching data source, or are worth the added
  complexity for what Sonos actually exposes.
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
