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
  Closing the popover after a pick (`room_popover_.popdown()`) is wired to
  `zones_list_box_.signal_row_activated()`, not `signal_row_selected()` —
  see the bug entry below for why that distinction matters.
- A Sonos household only ever needs *occasional* room switching, not a
  permanently-docked panel for it — freeing the sidebar slot for section
  navigation instead directly mirrors how noson-app itself splits these two
  concerns into a left nav list and a separate room switcher.

### Library categories as sidebar sub-items

Following a user request to see the library's own root categories
(Interpreten/Alben/Genres/Titel/Playlisten/Radiosender, plus one entry per
linked third-party service) directly in the section sidebar, not just after
first clicking into "Bibliothek": `RebuildLibraryNavEntries()` appends one
indented, icon-less row per entry in `library_root_entries_` right after the
"Bibliothek" row, and rebuilds them whenever `OnLibraryChanged()` fires for
the true root level (`library_stack_.size() == 1`) — covering both the
initial fetch and any later change (a service gets linked/unlinked).

- `library_root_entries_` is `current_library_entries_`'s root-level
  contents specifically, kept as its own copy since
  `current_library_entries_` tracks whatever level `LibraryView` currently
  has open, which is usually *not* the root once the user has browsed in —
  the sidebar needs the root list independent of that.
- `nav_row_actions_` replaced the old static index-into-a-fixed-array
  lookup `OnNavRowSelected()` used: now every `nav_list_box_` row, static
  or library-derived, carries its own `std::function<void()>` in the same
  append order, and `OnNavRowSelected()` just runs
  `nav_row_actions_[row->get_index()]()`. The five static top-level rows
  are built once in the constructor and never rebuilt — `kStaticNavRowCount`
  marks where `RebuildLibraryNavEntries()`'s own rows start, so a library
  refresh only ever removes/re-adds its own tail, never disturbing the
  static rows' identity or selection state.
- Clicking a library sub-item resets `library_stack_` back to the root and
  pushes just that one category, then browses into it and switches
  `view_stack_` to "library" — a deliberate jump-to-root-then-in, not a
  push relative to wherever `LibraryView` currently was, so the sidebar
  always behaves like "go to this category" regardless of prior browse
  depth. The library's own back button then returns to the true root, not
  to some arbitrary prior level.
- `kLinkServiceSentinel`'s "Dienst verknüpfen…" root entry is deliberately
  excluded from the sidebar — it opens a dialog rather than browsing
  anywhere, which would read oddly as a permanent nav destination.

### Five more additions: history search, library favoriting, fixed volume, alarm duplication, favorites bulk actions

- **History → search**: a `HistoryEntry` carries no `object_id`/URI it
  could be replayed from (client-side only, see its own comment), so
  `HistoryView` gained a per-row search button instead of a play action —
  `GnomosWindow` calls the existing `ShowLibrarySearchDialog(prefill)` with
  the entry's artist (falling back to its title), the same dialog the
  track-details "Interpret suchen"/"Album suchen" buttons already open.
- **"Add to Favorites" on library entries**: `NosonBackend::AddLibraryItemToFavorites(index)`
  mirrors `AddCurrentTrackToFavorites()`, using `library_raw_[index]`
  directly rather than needing a live-playback snapshot. Unlike
  add-to-queue/play-next, this one applies to *containers* too (a whole
  album/playlist/artist is a completely normal thing to favorite in
  Sonos) — `LibraryView::SetEntries()` gained a `show_favorite_action`
  flag that `GnomosWindow` only sets once `library_stack_.size() > 1`, so
  the static root categories ("Interpreten", "Alben", ...) don't get a
  meaningless favorite button of their own. List rows only — a grid tile
  has no room for a third action button; switching to list view offers it
  instead.
- **Fixed volume / line-out toggle**: `SoundSettings` gained
  `output_fixed_supported`/`output_fixed`, populated in
  `RefreshSoundSettingsAsync()` via `GetSupportsOutputFixed()`/
  `GetOutputFixed()` and applied via `SetOutputFixed()` — a new switch in
  the sound popover, `set_sensitive()`-gated the same way
  `nightmode_switch_` already handles a per-model-unsupported case, rather
  than hiding the whole row (keeps the popover's layout stable across room
  switches). For a device wired via line-out to a receiver/amp that has
  its own volume control; `SetGroupVolume()`/`SetMuted()` already knew to
  skip an `OutputFixed` member when scaling a group, so this closes the
  matching write-side gap.
- **Duplicate an alarm**: `ShowAlarmDialog()` gained a `duplicate`
  parameter, separating "pre-fill fields from `*existing`" from "saving
  should update `*existing`'s own alarm" — previously the same
  `existing != nullptr` check drove both, which doesn't fit a duplicate
  (pre-fill yes, update no). A local `editing = existing && !duplicate`
  now drives every edit-specific decision (dialog title, confirm button
  label, `alarm_id` used for create-vs-update, and whether "Aktueller
  Klang beibehalten" is offered — that option only makes sense for an
  alarm that already exists server-side), while every *value* default
  (room/time/days/volume/duration/shuffle) stays keyed off `existing`
  itself, unaffected by which mode it's used in.
- **"Play all"/"add all to queue" for Favorites**: `AddAllFavoritesToQueue()`/
  `PlayAllFavoritesAsync()` mirror the library's own
  `AddAllLibraryItemsToQueue()`/`PlayAllLibraryItemsAsync()`, over
  `favorites_raw_` instead of `library_raw_` — each favorite first needs
  `System::ExtractObjectFromFavorite()` to unwrap its real playable item,
  same as `PlayFavorite()`/`AddFavoriteToQueue()` already do per-row; one
  that doesn't unwrap to a queueable item (most commonly a live radio
  stream) is silently skipped, same tension `AddAllLibraryItemsToQueue()`
  already resolves the same way. Unlike the library's version, there's no
  "all leaf" gate first — every favorite is already individually
  playable/queueable regardless of type. `FavoritesView` only shows the
  two bulk buttons while its search filter is empty, so "play all" can
  never be misread as "play the filtered results".

### Application icon

Replaced the placeholder icon (a plain blue circle-in-a-square) with a
proper one, combining three references the user pointed at directly:

- **Shape**: GNOME Decibels' own icon template — its background path
  turned out to already *be* a rounded play-triangle silhouette (confirmed
  by rendering it in isolation), not a generic squircle, which fits "a
  streaming music player" perfectly on its own. Reused verbatim from the
  provided source rather than freehand-approximated, since exact bezier
  reproduction by hand is exactly the kind of thing that goes subtly wrong
  without visual verification.
- **Color**: solid black shape, white inner glyph, no gradients — the
  Sonos wordmark's own palette, and a deliberate departure from Decibels'
  own multi-stop color gradient and soft bevel/highlight rendering (which
  would have fought with "take the Sonos colors" — Sonos' actual brand
  identity is flat and high-contrast, not soft).
- **Inner glyph**: three nested white arcs (a wifi/broadcast-wave motif),
  clipped to the black shape so they can never bleed past its edges —
  since the outer silhouette already reads as "play", a second nested
  play triangle would have been redundant; the arcs alone complete "play →
  broadcasting/streaming" without repeating the same glyph twice. Checked
  down to 48px (SVG rendered to PNG at multiple sizes and inspected) before
  finalizing — the arcs blur together below that, same trade-off
  Decibels' own multi-bar equalizer glyph makes at small sizes.

The new SVG replaces `data/icons/hicolor/scalable/apps/de.christophlangner.Gnomos.svg`
directly — everywhere it's referenced (the `.desktop` file's `Icon=`,
`AdwAboutDialog`'s `adw_about_dialog_set_application_icon(APPLICATION_ID)`)
already resolved it purely by icon name, so no other code needed to change
for those. One real gap it exposed: running straight from the build tree
(never `meson install`'d) had **never** actually been able to resolve the
icon at all, since `data/icons/` was never on the icon theme's search
path — confirmed live via `Gtk::IconTheme::has_icon()` returning `false`
before the fix (and a first attempt at the fix itself got the path wrong:
`add_search_path()` wants the directory that *contains* `hicolor/`, not
`hicolor/` itself — also confirmed live, the same way). Fixed in
`GnomosApplication::on_startup()` by adding `$SOURCE_ROOT/data/icons` (a
new `meson.build`-injected `config.h` define) as an extra search path,
guarded by `Glib::file_test()` so it's a harmless no-op for an installed
or Flatpak build where that path doesn't exist.

### Five more additions, round two: device info, playlist/radio management, sub gain, next-alarm

- **"Geräteinfo" dialog**: `NosonBackend::GetDeviceInfo(player_uuid)` is
  synchronous, unlike almost every other query in this class — everything
  it returns (IP, software version, model number) is already sitting in
  the cached zone topology (`ZonePlayer::GetHost()`/`GetAttribut(ZP_VERSION)`,
  `model_number_by_uuid_`), no network round trip needed. The MAC address
  needs no query at all: Sonos embeds it directly in the player's own UUID
  (`"RINCON_<12 hex MAC digits><port suffix>"`, confirmed live). Reached
  from a new "i" button on every row in `room_button_`'s popover — deliberately
  per-room there, not gated to just the currently selected zone's coordinator,
  since knowing a *specific* room's IP/software version is exactly the kind
  of thing you'd want when troubleshooting one particular speaker.
- **Delete a saved Sonos playlist**: `NosonBackend::DeleteLibraryPlaylist(index)`
  wraps `System::DestroySavedQueue()`. `LibraryView` only shows the delete
  button while `library_stack_.back().first == "SQ:"` (the "Playlisten"
  root) — every entry there really is a destroyable saved queue, unlike
  any other library level. The backend can't refresh the level itself
  afterward (it has no notion of "the level currently browsed", only
  `GnomosWindow::library_stack_` does), so `ShowDeletePlaylistConfirmDialog()`
  follows the delete with its own `BrowseLibraryAsync(library_stack_.back().first)`
  call — queued on the same serial `tasks_` worker, so it always runs after
  the delete completes.
- **Add a custom radio station**: `NosonBackend::AddRadioStation(title, url)`
  wraps `System::CreateRadio()`, which creates the entry under the exact
  same `"R:0/0"` root `BrowseLibraryAsync("R:0/0")` already browses — new
  stations show up right alongside the built-in TuneIn-backed directory,
  no separate namespace. `LibraryView::add_button_` only shows up browsing
  that root, same re-browse-after-write pattern as the playlist delete above.
- **Sub gain**: `SoundSettings` gained `sub_gain`/`sub_gain_supported`,
  following the exact same pattern `output_fixed_supported` already
  established — except there's no `GetSupportsOutputFixed()`-style
  capability check for sub gain anywhere in the protocol, so "supported"
  is inferred the same way `nightmode_supported` already is instead, from
  whether `GetSubGain()` itself succeeds. `sub_gain_scale_` uses
  `set_sensitive()`, not `set_visible()`, when unsupported — a stable
  popover layout across room switches, not a jumping one.
- **Next-alarm indicator**: a new label in `AlarmsView`'s own toolbar,
  computed by `GnomosWindow`'s `NextAlarmSummary()` (day-of-week +
  time-of-day arithmetic over every *enabled* alarm's own recurrence,
  reusing `ParseRecurrenceDays()` — moved earlier in the file so
  `OnAlarmsChanged()` can call it too, alongside its original caller
  `ShowAlarmDialog()`) and passed down as plain text — `AlarmsView` itself
  stays a dumb renderer, same division of labor every other view already
  follows. An alarm whose recurrence doesn't parse to any days at all (a
  literal `"ONCE"` from the official Sonos app, say — Gnomos's own alarm
  dialog only ever generates day-list recurrences) is silently excluded
  rather than guessed at.

### Consistent GNOME iconography for the library, and real artist photos

Two related fixes, both prompted directly by screenshots: the local
library's "Interpreten" grid showed the exact same generic note icon for
*every* artist (no per-item art existed to fall back to), and a linked
service's own root menu (bonob, specifically) showed its *own*
inconsistent, non-GNOME icon set for category tiles like "Artists"/
"Albums"/"Random" — a small monochrome stock-icon set that visibly
clashed with the rest of a GNOME app.

- **The real signal**: `DigitalItem::subType()` — parsed generically by
  libnoson from the item's own `upnp:class` DIDL property (see its
  constructor in `digitalitem.cpp`) — already distinguishes person/album/
  genre/playlistContainer/storageFolder, identically for local library
  items *and* every SMAPI service alike, without Gnomos needing to guess
  anything from a title string the way a name-based heuristic would have
  to. `LibraryEntry` gained `icon_name`, populated via a new
  `IconNameForSubType()` helper (in all four entry-building loops:
  local browse, local search, SMAPI browse, SMAPI search) mapping to
  verified-installed Adwaita symbolic icon names (`avatar-default-symbolic`
  for an artist, `media-optical-cd-symbolic` for an album,
  `folder-music-symbolic` for a genre or generic folder,
  `media-playlist-consecutive-symbolic` for a playlist) — checked against
  the actual installed icon theme files before picking names, not
  guessed. The static local root categories ("Interpreten", "Alben", ...)
  set it directly in their own brace-init list, since they have no backing
  `DigitalItem` to derive one from at all.
- **`CoverThumbnail::SetFallbackIconName()`**: replaces the single
  hardcoded `"audio-x-generic-symbolic"` every entry used to fall back to
  regardless of type — now the caller decides, defaulting to that same
  string if never called, so every *other* existing caller (Queue/
  Favorites/History, none of which carry an `icon_name`) is unaffected.
- **Overriding a service's own root-menu icon, not just filling gaps**:
  for a SMAPI entry, `SMAPIItem::displayType == Grid` (already relied on
  elsewhere as bonob's own "this is a root category tile, not real
  content" signal — see the grid-eligibility fix earlier in this file)
  now also clears `art_uri` outright rather than only supplying
  `icon_name` as a fallback for an *empty* one. Confirmed live: bonob's
  own root tiles ("Artists", "Albums", "Random", ...) each carry a real,
  distinct, but visually inconsistent icon image of their own — leaving
  `art_uri` alone there would have kept showing those. A real album's or
  artist's own genuine cover art, deeper in the same service (where
  `displayType` isn't `Grid`), is untouched.
- **Real artist photos** (`src/widgets/artist-image-fetcher.h/.cpp`, new):
  local `A:ALBUMARTIST` entries generally have no `albumArtURI` at all in
  Sonos's own ContentDirectory response (confirmed live — this is exactly
  why every local artist showed the same generic icon before this whole
  fix), so there's no *Sonos-side* photo to fall back to no matter how
  it's parsed. `ArtistImageFetcher` is a small, throttled (`kMaxConcurrent`
  = 2 at a time — hundreds of simultaneous connections the moment
  "Interpreten" opens would be poor citizenship against a free public API)
  singleton that looks a name up against Deezer's public
  `api.deezer.com/search/artist` endpoint and caches the result
  in-process, parsed with `json-glib` (a new dependency — this is the
  first place in the app dealing with a real multi-object JSON response,
  unlike the single-tag XML `std::string::find()` extraction
  `RefreshGen1StatusAsync()` already does elsewhere, so a real parser
  earns its keep here). Deezer's own search ranking isn't popularity-aware
  — confirmed live, searching "Adele" ranks an obscure "Adèle & Zalem"
  (1.5k fans) above the real Adele (15.4M fans) and returns several
  *other* unrelated results also literally named "Adele" with negligible
  fan counts — so matching picks the highest `nb_fan` among exact
  (case-insensitive) name matches on the first page of results, falling
  back to Deezer's own first result only when no exact match appears at
  all. `CoverThumbnail::LoadArtistImage()` reuses the exact same
  generation-counter/`alive_`-flag safety machinery `SetArtUri()` already
  has for its own async load, just with an extra resolution step in front
  of it.
- **Explicitly opt-in**: every other network request this app makes stays
  within the local Sonos household; a Deezer lookup is the one exception,
  sending an artist's name to a third-party server. Off by default,
  toggled via a new "Bibliothek" group in Settings with a subtitle
  disclosing exactly that — `GnomosWindow::load_artist_images_`, persisted
  to `state.ini`'s `[library]` group alongside `prefer_grid_view_`.
  `LibraryView` only calls `CoverThumbnail::LoadArtistImage()` instead of
  `SetArtUri()` when the preference is on **and** `icon_name` is exactly
  `"avatar-default-symbolic"` **and** `art_uri` is empty — never for an
  artist a service already supplied real art for.

### Stronger Deezer disclosure

The "Künstlerbilder laden" setting's subtitle now names the actual
endpoint (`api.deezer.com`), says explicitly that it's a real internet
request rather than a local Sonos one, and states that Deezer's own terms
of use apply. A second row right below it links out to
`developers.deezer.com/api` (`Gtk::LinkButton`, same widget the
service-linking dialog already uses) so the terms are one click away
rather than just asserted in prose. `AdwPreferencesGroup` itself also
gained a description stating that this is the *only* thing in the app
that isn't local-network-only.

### Long lists: two rounds of an A-Z jump index, replaced with a live filter

`LibraryView` tried an A-Z jump index twice — first a fixed 27-row strip
(one row per bucket, always all shown), then, after that was reported
back as not actually solving the problem (a shorter window still couldn't
fit 27 rows legibly), a compact ~7-row window tracking scroll position
live via `scroller_`'s `vadjustment`. Asked directly to look for a better
approach rather than a third iteration on the same idea: GNOME apps
generally don't reach for alphabet scrubbers for this at all — Contacts,
Music, and Nautilus all lean on live filtering instead, and this app
already had exactly that pattern proven out in `FavoritesView`
(`search_entry_`, narrows the list instantly per keystroke, no dialog, no
network call). `LibraryView` now uses the same approach:

- `filter_entry_` (a `Gtk::SearchEntry`, its own row below the header) —
  narrows entries already loaded for the *current* level as you type.
  Deliberately distinct from `search_button_`'s existing dialog (which
  searches the whole library/service on the server): one is instant and
  local, the other is thorough but round-trips over the network — kept as
  two separate, complementary tools rather than merging them.
- `all_entries_` now holds the level's full, unfiltered content;
  `ApplyFilter()` (connected to `filter_entry_`'s own
  `signal_search_changed()`) re-renders from it on every keystroke,
  case-insensitive substring matching against title and subtitle. `SetEntries()`
  stores the incoming entries/flags into `all_entries_` and friends, resets
  `filter_entry_`'s text (a filter that made sense for the *previous*
  level shouldn't silently keep hiding entries after navigating somewhere
  unrelated — `set_text()` alone won't fire the change signal when the
  text was already empty, the common case, so `ApplyFilter()` is called
  explicitly regardless), and delegates the actual render to `ApplyFilter()`.
- `BuildList()`/`BuildGrid()` now take the *filtered* index list
  (`std::vector<unsigned>`, positions into `all_entries_`) rather than a
  entries vector directly, and use each real index — not its position in
  the filtered subset — for every signal emission (favorite/delete/
  add-to-queue/play-next/activate), the same "always the real underlying
  index, never the on-screen position" contract `FavoritesView` already
  documents for its own `row_index_map_`.
- `play_all_button_`/`queue_all_button_` now also require the filter to
  be empty, on top of the existing all-leaf-entries check — with a filter
  narrowing the view, "all of them" would be ambiguous between the full
  level and just the filtered subset, so they're hidden entirely rather
  than guessing.

### Fixed: browsing local library after visiting a service came back empty

Reported live: internal library → click a service (e.g. bonob, itself
reached via the section sidebar's own service sub-item, not just the
library's root screen) → click a *different* sidebar sub-item to return
to local browsing (e.g. "Alben") → empty content.

Root cause: `NosonBackend::BrowseLibraryAsync()` only ever reset
`active_smapi_`/`active_service_` in its `object_id == ""` branch (the
true root). `GnomosWindow::RebuildLibraryNavEntries()`'s own sidebar
sub-items (see the "Library categories as sidebar sub-items" section
above) call `BrowseLibraryAsync()` with a *local* object_id directly,
never routing through `""` first — so if `active_smapi_` was still set
from a service visited earlier, `BrowseLibraryAsync()`'s
`if (active_smapi_) { BrowseActiveServiceLocked(object_id); }` branch
wrongly routed the local id (e.g. `"A:ALBUM"`) through the *stale
service session* instead, which of course has no item by that id and
came back empty.

Fixed by recognizing local object_ids on sight — they always carry one of
three reserved prefixes (`"A:"`, `"SQ:"`, `"R:"`, per the root categories
`BrowseLibraryAsync("")` itself builds) that a SMAPI service would never
return as one of its own item ids — and force-resetting
`active_smapi_`/`active_service_` whenever a browse request looks local,
regardless of what was active before. `BrowseLibraryAsync("")`'s own
existing reset is unaffected; this just closes the gap for every *other*
caller that jumps straight to a local id.

### Fixed: real images displayed noticeably larger than fallback icons

Reported live with a screenshot: a bonob category tile showing a real
downloaded icon image came out visibly bigger than neighboring tiles
showing a fallback symbolic icon, despite both being the same
`CoverThumbnail(120)`.

Root cause: `Gtk::Image::set_pixel_size()` only constrains icon-name/GIcon
rendering — a raw `Gdk::Texture` set via `Gtk::Image::set()` (real
downloaded art, an artist photo, a service's own icon image) has no size
cap of its own and displays at its natural decoded resolution. Fixed with
a new `ArtCache::GetScaled(uri, target_size)`, decoding via
`Gdk::Pixbuf::create_from_stream_at_scale()` (a purpose-built scaling
loader) rather than resizing an already-decoded `Gdk::Texture` after the
fact — texture pixel data is only reachable via
`Gdk::Texture::download()`, whose Cairo-ARGB32 layout (premultiplied
alpha, platform-dependent byte order) would need its own careful,
easy-to-get-subtly-wrong conversion back to GdkPixbuf's plain
non-premultiplied RGBA; scaling from the original bytes sidesteps that
conversion entirely. `ArtCache::Entry` gained a `raw_bytes` field
(populated by `Put()` and `Get()`'s own disk-fallback, alongside the
existing `texture` field) so a memory-cache hit can still be re-decoded
at an arbitrary target size without another disk read. `Get()`/`Put()`
themselves are unchanged — `PlayerBar`'s own (single, larger) art image
had no such problem to fix, so only `CoverThumbnail` switched to the new
method.

**Follow-up, reported live against the same screenshot after the above
was already in place**: the *fallback* icon itself (the generic note
glyph shown for entries with neither real art nor a more specific
`icon_name`) was still visibly bigger than neighboring tiles — the fix
above only addressed real downloaded textures, which was a different
class of "too big" from this one. Root cause here is closer to an icon
*design* mismatch than a sizing bug: `set_pixel_size()` was doing exactly
what it's documented to do (constrain the icon to that many pixels), but
`"audio-x-generic-symbolic"`'s own glyph fills nearly its entire square
canvas, unlike a more generously-padded icon (an avatar silhouette, a
disc), so the *same* pixel size still reads as visually larger.

A first attempt fixed this with a hardcoded 3/5 shrink specifically for
fallback icons — reported back as solving the wrong problem: the
complaint was about the *glyph's* size within its tile, not the tile
itself, and a fixed ratio is still just a guess at what looks balanced
across every icon this app uses. Replaced with a user-adjustable setting
instead, defaulting back to the original (pre-shrink) full size:
`CoverThumbnail::s_fallback_icon_scale` (a static, process-wide fraction,
1.0 = full `pixel_size_`) is applied only inside `ShowFallback()`
(`set_pixel_size(pixel_size_ * s_fallback_icon_scale)` right before
`set_from_icon_name()`) — real texture content is completely unaffected,
still displaying at the full `pixel_size_` via `GetScaled()` regardless.
`GnomosWindow::fallback_icon_scale_` persists the user's choice to
`state.ini`'s `[library]` group and calls the static setter both at
startup (syncing a value loaded from a previous run) and from a new
"Symbolgröße" `AdwSpinRow` in Settings (20-100%, mirroring the existing
cover-art-cache-size spin row's own pattern) whenever it changes.

**Second follow-up, reported live**: dragging the new "Symbolgröße" row
while looking at a real, large grid (bonob's Albums listing) crashed the
app outright. `CoverThumbnail`'s async art loading was already checked
and ruled out as the cause — it's guarded by an `alive_`/`generation_`
pair specifically to survive being destroyed mid-load (see
`CoverThumbnail`'s own header). The actual difference between this
setting and every other one that also calls `OnLibraryChanged()`
(`prefer_grid_view_`, `load_artist_images_`) is how it's driven: those
are switch rows, which fire their change signal once per click, while an
`AdwSpinRow` fires `notify::value` many times a second while being
dragged or scrolled — and each firing was rebuilding the entire grid
(`LibraryView::Clear()` tearing down and `BuildGrid()` recreating every
tile) synchronously, back-to-back, with no chance for GTK to finish a
frame in between. Fixed the same way `NosonBackend::SetVolume()` already
handles its own rapid-fire slider input: `GnomosWindow::SetFallbackIconScale()`
now only updates the live rendering scale immediately (cheap, no rebuild)
and debounces the disk-persist + `OnLibraryChanged()` rebuild by 200ms via
a `fallback_icon_scale_debounce_connection_` timeout, so only the
settled value after dragging stops actually triggers a rebuild.

**Third follow-up, reported live against a fresh screenshot**: several
tiles in bonob's own root menu ("Albums", "Random", "Top Rated",
"Internet Radio") still looked like they were showing the plain fallback
icon rather than something fitting. Rather than guess from the
screenshot, this was checked directly against the running app: with no
screen-capture path available in this environment (see the Flatpak
portal note below), a debug build was driven live via `gdb`, calling
`NosonBackend::BrowseLibraryAsync()` directly on the already-running
process (using a `LibraryEntry::object_id` value read straight out of
`GnomosWindow::current_library_entries_` at a breakpoint, avoiding
`gdb`'s own flaky support for constructing a `std::string` argument from
a literal) to browse into bonob's root menu and inspect the resulting
`LibraryEntry` values directly — no UI interaction needed.

That confirmed two distinct things bundled under one complaint. First,
most of bonob's own root tiles ("Albums", "Random", "Favourites", "Top
Rated", "Recently added/played", "Most played") report SMAPI itemType
`albumList` (-> `SubType_storageFolder`), which already resolves to a
real, deliberate icon (`folder-music-symbolic`) via `IconNameForSubType()`
— not literally the generic fallback, just the same undistinguished
folder glyph for every one of them, since SMAPI gives no finer-grained,
service-independent signal to tell "Random" apart from "Top Rated".
Second, "Internet Radio" specifically reports itemType `stream` (->
`SubType_audioItem`, and `IsContainer()` false — a real leaf, not a
folder to browse into, unlike every other root tile) — a subtype
`IconNameForSubType()` rightly leaves unmapped everywhere else in the
app (a real playable track has no better icon than the generic note),
but which was genuinely wrong for a root-level tile: this one really was
falling through to the plain fallback, a real bug.

Fixed at the same call site that builds `LibraryEntry` for a service's
root menu (`id == "root"`, `BrowseActiveServiceLocked()`): when
`IconNameForSubType()` comes back empty *and* this is a root-level tile,
default to `folder-music-symbolic` for a container or
`network-wireless-symbolic` for a non-container leaf — mirroring the
same split the local library's own hardcoded root categories already use
(`BrowseLibraryAsync("")`'s `roots` list gives "Radiosender" that exact
icon for the same reason). Every other browsing level (a real
track/stream found deeper in an actual listing) is untouched, since the
`id == "root"` check only applies to a service's own top-level menu.

*Aside — screen capture in this environment*: taking a screenshot via
`org.freedesktop.portal.Desktop`'s `Screenshot` portal was attempted
again here (`gdbus call` against the session bus) since the user
suggested it directly; the request object is created successfully but
its `Response` signal never arrives, even after 15s — consistent with it
waiting on an interactive confirmation dialog neither `wlr-screencopy`
nor this headless-of-input session can drive. Driving the backend
directly via `gdb` sidestepped that entirely and gave a more precise
answer than a screenshot would have (the actual `SubType_t`/`itemType`
values, not just how they render).

### Switching to a very large grid (1000+ entries) took seconds

Reported live: switching to the local library's or a linked service's
"Alben" showed a multi-second freeze before anything appeared, worse the
bigger the collection. Rather than guess, this was measured directly —
temporary timing instrumentation in `LibraryView::ApplyFilter()`, driven
via the same live-`gdb` technique used for the icon investigation above
(`BrowseLibraryAsync()` called directly on the running process) against
the local library's real 1060-entry "Alben" listing. `Clear()`, the
filter pass, and swapping the scroller's child were all sub-millisecond;
`BuildGrid()` itself took **6.7 seconds**.

`Gtk::FlowBox`/`Gtk::ListBox` aren't virtualized — every tile in a 1060-
entry grid gets a real, fully realized `CoverThumbnail` up front,
regardless of how many are actually visible on screen. That widget count
alone wasn't the bottleneck, though: `CoverThumbnail::SetArtUri()` was
calling `ArtCache::GetScaled()` **synchronously**, inline in the tile-
building loop, for every entry already in cache — and on this system,
decoding a cached image (`Gdk::Pixbuf::create_from_stream_at_scale()`)
costs roughly 6ms each, apparently a sandboxed `glycin` loader round trip
rather than raw pixel work (confirmed by the flood of `glycin::pool`
debug messages coincident with each decode, visible with
`G_MESSAGES_DEBUG=all`). ~1060 of those back-to-back easily accounts for
the full 6.7s. A second, larger cost was hiding in the disk-cache
fallback: `ArtCache`'s in-memory LRU only holds `kMaxMemoryEntries = 300`
decoded entries, so the majority of a 1000+-item grid's tiles — even on
a *repeat* visit to the exact same listing — miss memory and fall
through to `GetRawBytes()`'s disk path, which was *also* eagerly
decoding a full-resolution `Gdk::Texture` there (needed only for
`Get()`/`PlayerBar`'s benefit, never for a grid tile) on top of the
scaled decode a tile actually wanted.

Fixed in two parts:
- **`ArtDecodePool`** (`src/widgets/art-decode-pool.h/.cpp`): a small
  fixed pool of 2–4 worker threads (clamped from
  `std::thread::hardware_concurrency()`), separate from `TaskQueue`
  deliberately — `TaskQueue` is single-threaded by design (libnoson calls
  must be serialized against one connection), but decode jobs are fully
  independent of each other and *should* run concurrently to actually cut
  wall-clock time rather than just relocate it. `ArtCache::DecodeScaledTexture()`
  was promoted from a private, anonymous-namespace helper to a public
  static method (pure — touches only its own arguments) specifically so
  it's safe to call from a pool thread. `CoverThumbnail::SetArtUri()` (the
  cache-hit path) and `OnLoaded()` (the post-download path) both now do
  the cheap lookup (`ArtCache::GetRawBytes()`, a map lookup or a disk
  read — never a decode) inline, then push the actual decode onto
  `ArtDecodePool`, marshaling the resulting texture back to the main
  thread via `Glib::signal_idle()` (documented safe to schedule from any
  thread) before touching `this` or any GTK API — guarded by the same
  `alive_`/`generation_` pair the async network-load path already used,
  so a decode that finishes after its `CoverThumbnail` was torn down (a
  filter keystroke, a second rapid grid rebuild, ...) is safely discarded
  rather than touching freed memory.
- **Lazy full-resolution decode**: `ArtCache::GetRawBytes()`'s disk
  fallback now inserts the memory-cache entry with an empty `texture`
  field (just `raw_bytes`) instead of eagerly decoding one — `Get()`'s
  memory-hit branch decodes the full-resolution texture on demand, the
  one time something (`PlayerBar`) actually asks for it, rather than
  paying for it on every grid tile that happens to need a disk read.

Confirmed live, same 1060-entry listing, same measurement technique:
6.7s → 241ms — driven mostly by the lazy-decode change once the scaled
decode was already off the main thread (6.7s → 2.7s from `ArtDecodePool`
alone, → 241ms once the eager full-res decode was also removed from the
hot path). Stress-tested by triggering five rapid back-to-back rebuilds
of the same 1060-entry grid via `gdb` (queuing several `BrowseLibraryAsync()`
calls before any completed, deliberately racing tile teardown against
in-flight decode jobs) — no crash, no `CRITICAL`/`WARNING` output, each
rebuild consistently 150–230ms.

*Aside — a live-testing mishap*: driving the already-*running*,
user-visible instance's `backend_->BrowseLibraryAsync()` directly via
`gdb` (to get a real 1000+-entry grid without clicking through the UI)
skips `GnomosWindow`'s own navigation-stack bookkeeping
(`library_stack_`), which a real click always goes through. With the
stack still at its startup depth, `OnLibraryChanged()`'s
`library_stack_.size() == 1` check misfired and treated the Albums
listing as the new *root* level, rebuilding the sidebar's nav sub-items
from 1060 album names instead of the real root categories — visible
live, reported directly, and understood immediately as a side effect of
the measurement technique rather than an app bug (a real click into
"Alben" always pushes onto `library_stack_` first, so this path is
unreachable through normal use). Fixed by treating every `gdb`-driven
measurement from then on as a throwaway, disposable process — kill it
immediately after use and relaunch a cleanly, normally-started instance
for the user to actually look at.

### Five more additions: delete a radio station, playlist add/reorder, line-in autoplay, rescan library

Requested as an open-ended "5 new features" — surveyed libnoson (the
`noson/` submodule) for already-linked capability the app didn't expose
yet, rather than guessing at a feature list. Picked five backed by real,
existing methods, no new protocol work needed:

- **Delete a custom radio station** (`System::DestroyRadio()`) —
  `NosonBackend::DeleteLibraryRadioStation()`, a near-exact mirror of the
  already-existing `DeleteLibraryPlaylist()`. `LibraryView`'s
  `show_delete_action` (previously gated on browsing `"SQ:"` only) now
  also gates on `"R:0/0"`; `GnomosWindow::ShowDeleteLibraryEntryConfirmDialog()`
  (renamed from `ShowDeletePlaylistConfirmDialog()`) branches on
  `library_stack_.back().first` to call the right backend method and show
  the right confirmation text either way.
- **Add a library track to an existing saved playlist**
  (`Player::AddURIToSavedQueue()`) — a new per-row "add to playlist"
  button (`bookmark-new-symbolic`) on leaf tracks, gated the same way
  `show_favorite_action` already is (below the true root). Needs the
  target playlist's own current `containerUpdateID`, fetched fresh with a
  single-item `Browse()` right before the call
  (`NosonBackend::AddLibraryItemToPlaylist()`) rather than trusted from
  whenever it was last browsed. The picker itself
  (`GnomosWindow::ShowAddToPlaylistDialog()`) needs the full list of
  saved playlists regardless of where in the library the user currently
  is, which is why `NosonBackend::FetchSavedPlaylistsAsync()`/
  `GetSavedPlaylists()` keep their own `saved_playlists_` storage instead
  of reusing `library_entries_`/`library_raw_` (which track whatever
  level is currently browsed) — same reasoning `favorites_`/`queue_` are
  already kept separate from the library browse state.
- **Reorder tracks within a saved playlist**
  (`Player::ReorderTracksInSavedQueue()`) — per-row "move up"/"move down"
  buttons (`go-up-symbolic`/`go-down-symbolic`), shown only while viewing
  a *specific* playlist's own tracks (`"SQ:<id>"`, not `"SQ:"` itself,
  which lists playlists) and only while unfiltered — a filtered subset's
  on-screen neighbor isn't necessarily the real adjacent track, which
  would make "up/down" silently do something other than what it visually
  shows. Same 1-based UPnP position convention and "moving forward needs
  the target index shifted by one" logic as the existing queue reorder
  (`ReorderQueueItem()`), and the same fresh-`containerUpdateID`-per-call
  reasoning as the playlist-add feature above.
- **Line-in autoplay** (`Player::SetAutoplay()`/`GetAutoplay()`/
  `Set|GetAutoplayVolume()`/`Set|GetUseAutoplayVolume()`) — whether this
  device should start playing (into itself; `Player::SetAutoplay()`'s own
  simplified bool wrapper offers no other target) when a line-in signal
  is detected, at what volume. Unlike bass/treble/loudness/nightmode/
  output_fixed/sub_gain (all keyed to the *selected zone's coordinator*
  uuid), autoplay has no uuid parameter at all — it's inherently a
  property of *this* specific device, same reasoning `PlayLineIn()`/
  `PlayDigitalIn()` already use `SnapshotPlayer()` directly rather than a
  coordinator uuid. Folded into the existing `SoundSettings` struct/
  `RefreshSoundSettingsAsync()` round trip rather than a new dispatcher,
  and a new "Autoplay (Line-In)" section in the sound popover, mirroring
  the existing switch/scale sensitivity-toggle pattern (`sub_gain_scale_`'s
  own). Confirmed live against the real household: `autoplay_supported`
  came back `true` for the currently selected room (it does have a
  line-in), and toggling `SetAutoplay()`/`SetUseAutoplayVolume()` via
  `gdb` and reading the settings back confirmed both round-trip
  correctly — restored to their original values (both off) immediately
  after.
- **Rescan the library share** (`System::RefreshShareIndex()`) — a
  "Bibliothek neu einlesen" button row in Settings → Bibliothek, for
  after adding files to an indexed local NAS share. Fire-and-forget:
  libnoson has no way to observe when the scan itself finishes, only
  that Sonos accepted the request.

*Aside — verifying without touching real data*: `gdb`'s C++ expression
evaluator in this environment cannot construct a `std::string` from a
literal at all (constructor call, C-style cast, and `new`-expression all
failed with a syntax/cast error) — every earlier live test this session
that needed a *new* string value worked around it by referencing an
already-live `std::string` lvalue instead (an existing entry's own
`object_id`/`title`). That workaround doesn't help for genuinely new
content (a disposable test radio station's title/URL, a track to insert
into a playlist), and deliberately running the add/reorder/delete calls
against the household's *real* saved playlists or radio stations to
compensate would mean editing or deleting the user's actual data without
asking — so those three were verified by code review (each mirrors an
already-proven method almost line for line) rather than a live call,
while everything reachable without a fresh string literal — `FetchSavedPlaylistsAsync()`
(returned all 15 real saved playlists correctly), `RefreshLibraryIndex()`,
and the full autoplay read/write round trip — was confirmed directly
against the real household instead.

### Radio-Browser.info search replaces typing in a name and URL by hand

Two follow-ups, reported live right after the batch above shipped.

First: the new per-row "add to playlist" button was showing up on radio
station rows too (browsing "R:0/0"), which doesn't make sense — a saved
Sonos playlist is conceptually a list of tracks, and `AddURIToSavedQueue()`
happily accepting a live stream doesn't make that a sensible thing to
offer. Fixed by excluding "R:0/0" from `show_add_to_playlist_action`'s
gating in `GnomosWindow::OnLibraryChanged()`, alongside the existing
`below_root` check.

Second, a genuine feature request: replace typing in a station's name and
stream URL by hand — the only way to add a custom radio station — with
searching a real directory instead. `RadioBrowserService`
(`src/widgets/radio-browser-service.h/.cpp`) is a thin client for the
public [Radio-Browser API](https://www.radio-browser.info), structured
like `ArtistImageFetcher`'s own Deezer client (same "genuine internet
request, disclosed inline rather than behind a separate opt-in setting"
reasoning — the difference being this one only ever fires from an
explicit user action, never automatically in the background, so it
doesn't need a persistent Settings toggle the way artist photos do; the
"Radiosender hinzufügen" dialog just names the endpoint directly).
`GnomosWindow::ShowAddRadioStationDialog()` was rewritten around a
country dropdown + search field + results list (each result's `url` is
`url_resolved` — the directory's own already-redirect-chased stream URL
— falling back to the raw `url` field for the rare entry missing it); the
original manual name/URL entry survives as a collapsed `Gtk::Expander`
fallback for anything the directory doesn't carry.

Hits `https://all.api.radio-browser.info` directly rather than resolving
a specific mirror via the DNS SRV-based server discovery the project's
own docs recommend for heavier clients — reasonable here given the
usage pattern (occasional, one-off, stateless searches, no pagination
continuity to keep consistent across requests), and considerably simpler
than implementing SRV lookups just for that.

Verified against the real API with a standalone test harness (same
technique `ArtCache`'s decode pipeline was verified with earlier —
compiled directly against `radio-browser-service.cpp` plus a bare
`GMainLoop`, sidestepping `gdb`'s inability to construct a `std::string`
argument *or* a callback for a live in-process test): `FetchCountries()`
returned all 242 real countries with correct names/codes/counts (Germany
→ "DE", 6184 stations), and `SearchStations("swr3", "DE")` returned 7
real, correctly-parsed results with playable URLs, codecs and bitrates —
confirming the JSON field names assumed while writing the parser
(`iso_3166_1`, `url_resolved`, `countrycode`, `bitrate`, ...) actually
match the live API rather than just my best recollection of its docs.

### Fixed: adding a radio station (or editing a playlist) didn't show up

Reported live: neither the new Radio-Browser search nor the original
manual name/URL entry actually added a station — the three pre-existing
ones kept showing, nothing new appeared.

Root cause: `library_cache_` (a TTL-based cache of already-browsed
levels, so revisiting one doesn't hit the network again) is checked
*before* a fresh `ContentDirectory::Browse()` in `BrowseLibraryAsync()`'s
local branch. It's invalidated wholesale on a real
`SVCEvent_ContentDirectoryChanged` — which Sonos does send after
`CreateRadio()`/`DestroySavedQueue()`/etc. actually change something —
but that event arrives asynchronously, over a separate UPnP eventing
channel, with no guaranteed ordering against GnomosWindow's own
follow-up `BrowseLibraryAsync(library_stack_.back().first)` call (queued
immediately after the mutating call, on the same serial `tasks_`
worker). In practice the event reliably loses that race: the follow-up
browse runs first, hits the *still-cached, pre-change* level, and the
UI ends up showing exactly what it showed before — a real station
successfully added server-side, silently invisible client-side for up
to `kLibraryCacheTtl` (5 minutes).

Fixed by having every backend method that mutates something
`library_cache_` might have a stale copy of call the already-existing
`InvalidateLibraryCache()` itself, synchronously, right after its own
SOAP call succeeds — `AddRadioStation()`, `DeleteLibraryRadioStation()`,
`DeleteLibraryPlaylist()`, `AddLibraryItemToPlaylist()`,
`ReorderLibraryPlaylistTrack()`. Since this now happens on the same
serial worker *before* the queued follow-up browse (rather than waiting
on an independent, unordered event), there's no race left to lose —
deterministic instead of "usually loses, occasionally wins if the event
happens to arrive first." `DeleteLibraryPlaylist()`/`DeleteLibraryRadioStation()`
had this same latent bug from the moment they shipped (a deleted entry
staying visible), just not reported until the add-a-station case made it
obvious.

### Fixed: the Radio-Browser result row's own "+" button did nothing

Reported live: a result could be added by clicking the row itself, but
not by clicking its per-row "+" button specifically — that button was
purely decorative, created but never wired to `signal_clicked()` at all.
Root cause is a real GTK behavior, not just a missed connection: a
`Gtk::Button` nested inside a `Gtk::ListBoxRow` consumes its own click
rather than letting it propagate up into the row's `row-activated`, so
even a correctly-styled "+" button silently does nothing unless it has
its own handler. Fixed by extracting the add logic (already used by
`signal_row_activated()`) into a shared `add_station` lambda, wired to
*both* the row activation and the button's own `signal_clicked()`.

### Removed: "add all to queue" for the Radiosender level

Reported live: the bulk "+" button ("Alle zur Warteschlange hinzufügen")
was showing up while browsing "R:0/0", since every radio station entry
is technically a leaf (`LibraryView`'s `all_leaf` check has no way to
tell "leaf" from "leaf that's a live stream"). Bulk-queuing a whole page
of radio stations at once doesn't read as a sensible action the way it
does for a page of real tracks — same judgment call as the earlier fix
excluding "R:0/0" from the per-row "add to playlist" button. `LibraryView::SetEntries()`
gained a `show_queue_all_action` parameter (`GnomosWindow` passes
`!is_radio_level`) gating only `queue_all_button_`'s own visibility —
`play_all_button_` ("Alle abspielen") is untouched, since playing
through a page of stations sequentially is a plausible thing to want
even if bulk-*queuing* them isn't.

### Fixed: the play-next button on a radio row always errored

Reported live: the skip-forward icon on a radio station row (mistaken
for a "play" button — it's actually `NosonBackend::PlayLibraryItemNext()`,
"insert as the next track") failed every time with "Dieser Titel kann
nicht als nächster Titel eingefügt werden." Root cause: it (and the
neighboring "add to queue" button) both go through
`AVTransport::AddURIToQueue()`, which `NSROOT::System::CanQueueItem()`
correctly reports as unsupported for a live radio stream — only a real,
position-addressable track can be inserted into the queue at a specific
spot; a stream can only be played directly. Both buttons were failing
outright every time for every radio row, not just being unhelpful.
Fixed the same way as the earlier per-row/bulk queue-button fixes:
`LibraryView::SetEntries()` gained a `show_queue_actions` parameter
(`GnomosWindow` passes `!is_radio_level`) that hides both buttons
together for "R:0/0" — activating the row itself
(`NosonBackend::PlayLibraryItem()`) is untouched, since its own
non-queueable branch (`SetCurrentURI()` + `Play()`) is exactly the
correct way to start a stream, and already worked correctly.

### Fixed: cover art flickering between real art and the fallback icon at launch

Reported live: thumbnails visibly flashed back and forth between the
real cover art and the generic fallback icon a few times right after
launch. Traced to two independent, stacking causes, both root-caused by
instrumenting `NosonBackend::SelectZone()`/`RefreshQueueAsync()` with a
temporary call counter during a real startup (removed once confirmed) —
every `CoverThumbnail` constructor calls `ShowFallback()` immediately,
so any full teardown-and-rebuild of a list (`LibraryView::Clear()` +
rebuild, same for `QueueView`) makes every one of its tiles flash
fallback-then-real-art again, however briefly; the fix in both cases
below is to stop triggering the *rebuild* redundantly, not to touch
`CoverThumbnail` itself.

1. **`zones_list_box_` rebuilding on every topology-settling event.**
   `GnomosWindow::OnZonesChanged()` (wired to `signal_zones_changed_`,
   which fires on every real `SVCEvent_ZGTopologyChanged` — plural,
   several of these typically arrive in the first few seconds as a
   household's zone players finish responding) tears down and rebuilds
   every row in `zones_list_box_` from scratch, then re-selects the
   still-current room — but as a *brand-new* `Gtk::ListBoxRow` object,
   which fires `row-selected` again even though nothing actually
   changed. `NosonBackend::SelectZone()` itself has no dedup (it always
   opens a fresh player connection), so each of those repeated topology
   events cascaded into a full `RefreshQueueAsync()` and `QueueView`
   rebuild. Confirmed via the call counter: `SelectZone()` went from
   firing multiple times during a single startup to exactly once, just
   by having `GnomosWindow::OnZoneRowSelected()` skip calling
   `backend_->SelectZone()` when the newly selected zone's `group_id`
   already matches `selected_group_id_`.
2. **A second, independent redundant refresh, exposed once #1 was
   fixed.** Even with `SelectZone()` down to one call,
   `RefreshQueueAsync()` still fired twice, ~80ms apart — one from
   `GnomosWindow::OnPlayerReady()` (triggered by `SelectZone()` itself),
   one from `NosonBackend::HandlePlayerEvent()`'s own
   `SVCEvent_ContentDirectoryChanged` branch. The second isn't a bug in
   the usual sense: UPnP GENA eventing reliably delivers an immediate
   "here's the current state" event for every evented service right
   after (re-)subscribing, which is exactly what happens when
   `SelectZone()` opens a fresh player connection — `HandlePlayerEvent()`
   has no way to tell that echo apart from a genuine change using the
   event data alone. Fixed with a 2-second grace window
   (`last_zone_select_at_`, set at the end of `SelectZone()`'s own task):
   `HandlePlayerEvent()` ignores a `ContentDirectoryChanged` event that
   arrives within that window of the most recent zone selection, but
   still reacts normally to one arriving later — a real mid-session
   queue change (from this app or another controller) is unaffected.
   Confirmed via the same call counter: back down to exactly one
   `RefreshQueueAsync()` per launch.

### A dedicated "play now" button replaces two removed bulk/per-row actions on radio rows

Follow-up to the queue-action fixes above: once `show_queue_actions`
hid add-to-queue/play-next for radio rows, a station's *only* way to
play was clicking the row itself — with no dedicated per-row button, the
whole-row-click affordance isn't as discoverable as an explicit icon.
Requested directly: replace the two removed per-row buttons with a
single "play now" (`media-playback-start-symbolic`) button in their
place — `LibraryView::BuildList()`'s leaf branch now has an `else`
alongside the existing `if (show_queue_actions)`, emitting
`signal_entry_activated_` (the exact same signal activating the row
itself already emits) rather than a new signal, since
`NosonBackend::PlayLibraryItem()`'s existing non-queueable branch is
already the correct way to start a stream.

Also requested: `play_all_button_` ("Alle abspielen") — previously left
alone when `queue_all_button_` was excluded from "R:0/0", on the
reasoning that playing through a page of stations sequentially was at
least plausible — turned out not to be wanted either. `LibraryView::SetEntries()`
gained a `show_play_all_action` parameter, `GnomosWindow` passing
`!is_radio_level` same as the others, so both bulk buttons are now
excluded from the Radiosender level symmetrically.

### About dialog: GitHub link, and a Radio Browser acknowledgement

Two small, explicitly requested additions to `ShowAboutDialog()`:
`adw_about_dialog_set_website()`/`set_issue_url()` pointing at the GitHub
repo and its issue tracker (the same two links README.md itself already
points people to for bug reports), and a new "Dienste" acknowledgement
section crediting Radio Browser (radio-browser.info), alongside the
existing "Bibliotheken" section that already credits libnoson.

### Radio station thumbnails for custom-added stations

Requested directly: show a real thumbnail for a station added via the
new Radio-Browser search, the same way a built-in TuneIn station already
does. The built-in ones work already — Sonos's own local radio directory
carries a real `upnp:albumArtURI` for those, picked up by the exact same
`ResolveArtUri(item->GetValue("upnp:albumArtURI"))` call every other
library entry already goes through. A *custom* station never can, though:
`System::CreateRadio()` (sonossystem.cpp) takes no icon parameter at
all — there's nothing to send Sonos in the first place, unlike
`upnp:albumArtURI`, which the device populates itself from its own
TuneIn integration.

Radio-Browser's own station objects do carry a `favicon` URL (now parsed
into `RadioBrowserStation::favicon`), so the fix stores that locally
instead — `NosonBackend::AddRadioStation()` gained an optional
`favicon_url` parameter (the manual-entry fallback has nothing to pass;
the radio-browser dialog's `add_station` lambda passes
`station.favicon`), persisted via `SaveRadioFavicon()` to a small
`radio-favicons.ini` (mirroring `art-cache.ini`'s own load-modify-save
pattern) keyed by a hash of the station's stream URL. `BrowseLibraryAsync()`'s
local branch loads that map once per "R:0/0" browse
(`LoadRadioFavicons()`) and fills in `entry.art_uri` from it whenever
`upnp:albumArtURI` came back empty — CoverThumbnail/ArtCache then fetch,
cache and decode that favicon URL exactly like any other art URI, no new
image-loading code needed at all.

The one real trap here, caught by reading `System::CreateRadio()`'s own
implementation rather than assuming: it does **not** store `streamURL`
verbatim. It strips the `http(s)` scheme and prepends Sonos's own
`x-rincon-mp3radio:` protocol, keeping only the `"://host/path"` suffix
— so a browsed entry's own `res` value never textually matches the
original URL `AddRadioStation()` was called with. Hashing the *full*
original URL (an earlier version of this fix) would have silently
matched nothing, ever. Fixed by extracting and hashing only the common
`"://..."` suffix (`RadioStreamMatchKey()`) on *both* sides — saving and
lookup — so the scheme-prefix difference stops mattering.

### Fixed: switching radio stations showed the generic icon in PlayerBar

Reported live, right after the thumbnail fix above shipped: the library
listing showed the right thumbnail per station, but switching to one
still showed the generic fallback in the bottom Now Playing bar. Same
root cause, second location: `RefreshNowPlayingLocked()` sets
`NowPlaying::art_uri` for a live stream strictly from Sonos's own
`CurrentTrackMetaData`'s `upnp:albumArtURI`, which is empty for any
custom station for the exact same reason the library listing's own copy
was (`System::CreateRadio()` never gave Sonos an icon to report back in
the first place).

Fixed the same way, applied a second time: when that field comes back
empty, fall back to `radio-favicons.ini` via the same
`RadioStreamMatchKey()`, matched against `AVTProperty::AVTransportURI`
this time rather than a browsed item's `res` value — confirmed by
reading `Player::SetCurrentURI()`'s own implementation
(`m_AVTransport->SetCurrentURI(item->GetValue("res"), ...)`) that this
is exactly the same value verbatim, just reflected back once played
rather than read from a directory listing, so the identical scheme-
rewrite reasoning (and the identical matching function) applies
unchanged.

### Fixed: some radio stations permanently stuck on the fallback icon, an actual `ArtCache` bug

Reported live with a screenshot: the library listing showed correct,
distinct thumbnails for *every* station (including SWR1) — but the
bottom Now Playing bar still showed the generic fallback while SWR1 was
actively playing, contradicting the two fixes just above. Live
inspection (`gdb`, reading `backend_->now_playing_` directly on a real
playing session — same generation-mismatch and type-resolution
complications as ever, worked around the same way) showed
`NowPlaying::art_uri` itself was already correctly resolved to SWR1's
favicon URL. So the bug wasn't in resolving the URL at all — it was in
what happened *after*.

Root cause, found by reading `PlayerBar::LoadArt()`: unlike every
library tile (`CoverThumbnail`, which uses `ArtCache::GetRawBytes()` +
`DecodeScaledTexture()`), `PlayerBar` still calls the older
`ArtCache::Get()` directly for a full-resolution texture. `Get()`'s
memory-hit branch just returned `it->second.texture` unconditionally —
but the large-grid performance fix earlier in this document
(`GetRawBytes()`'s disk-fallback) deliberately leaves that field *empty*
or a raw-bytes-only entry, on the explicit promise (written directly in
that method's own comment) that `Get()` would decode it lazily on
demand. That lazy decode was never actually implemented — only promised
in the comment. Once a URI's memory entry existed at all (even with an
empty texture), `Get()`'s `if (it != entries_.end())` check always
matched and returned immediately, *never* falling through to retry via
a fresh network fetch — a real, non-recoverable dead end, not a
transient one, which matches exactly what was reported ("permanently
the generic icon," not "briefly, then corrects itself").

This is reachable any time a URI's memory entry gets populated via
`GetRawBytes()`'s disk-fallback before `PlayerBar::LoadArt()` ever asks
for it directly — in practice, exactly the radio thumbnail scenario:
`CoverThumbnail` (the library tile) loads and caches the art first, that
memory entry later gets evicted by the 300-entry LRU cap as other art
gets browsed, and the *next* time the library tile re-requests it, the
resulting disk-fallback re-populates the entry with an empty texture —
poisoning it for `PlayerBar::Get()` from then on, permanently, even
though the exact same bytes decode successfully every time `GetScaled()`
asks for them.

Fixed by actually implementing the lazy decode `Get()`'s memory-hit
branch was always supposed to do: if the entry's `texture` is empty but
its `raw_bytes` are present, decode a full-resolution texture from them
right there, cache the result back into the entry, and return it —
matching `GetRawBytes()`'s own comment for the first time. Verified live
against the real, already-disk-cached SWR1 favicon (populated by
actually browsing "R:0/0" first, the same way `CoverThumbnail` would):
`ArtCache::Instance().Get(uri)` now returns a real, non-null texture for
exactly the URI that would previously have been silently stuck empty.

### Reworked: joining a room to a group now requires it to be free first

Requested directly, as an explicit rule set: selecting a standalone
device makes it its own (single-member) group; a genuinely free room
(alone in its own group) can be added to the currently selected one
directly; but a room already merged into *some other*, non-selected
group can no longer be joined straight across — it has to be removed
from its current group first (a separate action there), then added to
the target group as its own step. Sonos's own protocol has no problem
moving a device directly from one group to another in a single action —
this is a deliberate app-level UX choice, not a limitation being worked
around.

`RoomInfo` has no member-list field of its own, but every room sharing
the same `group_id` is by definition a member of that group, so
`RebuildGroupingPopover()` now does one pass over `backend_->Rooms()`
building a `group_id -> count` map before the render pass, giving an
O(1) "is this room's own group free (count == 1) or does it have other
members (count > 1)" check per row. A room in the selected group can
still be switched off (leave) same as before; a genuinely free room can
still be switched on (join) same as before; a room merged elsewhere now
gets `set_sensitive(false)` with a tooltip explaining why, instead of a
switch that would have silently regrouped it. The bulk "Alle Räume
gruppieren" button (which reuses the exact same
`JoinRoomToCurrentZone()` each row's switch calls) got the identical
`group_id -> count` check, so it now only ever picks up genuinely free
rooms too, plus an updated tooltip saying so — before this, it would
have silently regrouped an already-grouped room the same way an
individual switch could.

Verified directly against the real household's live topology (`gdb`,
reading `backend_->Rooms()` mid-session): confirmed a genuine 2-member
group currently exists (two rooms sharing one `group_id`, a different
coordinator each), alongside two standalone single-member ones — working
the new logic through by hand against that real data lands exactly where
the rule set says it should (the 2-member group's rooms disabled unless
selected, both standalone rooms freely joinable).

### Fixed: the room picker's device info only ever showed the group's coordinator

Reported live: the room picker's "Geräteinfo" button, for a merged zone,
only ever showed the info of whichever room the group was originally
opened from (`ZoneInfo::coordinator_uuid`) — a room added to that group
*later* had no way to see its own IP/MAC/model/software version at all.

`ShowDeviceInfoDialog()` took a single `player_uuid` (always the
coordinator's, from the room popover's own info button — see
`OnZonesChanged()`); a merged zone has no member-list field of its own
in `ZoneInfo` either. Fixed by changing the dialog to take the zone's
`group_id` instead of one specific `player_uuid`, and — same "derive
membership by matching `RoomInfo::group_id`" technique the grouping
rework above already introduced — collecting every room that currently
shares it. The dialog now renders one section per member (its own name,
Gen 1 badge if applicable, and the same Modell/IP/MAC/Software-Version
grid as before), separated by a `Gtk::Separator`, with "Kopieren"
copying all of them at once rather than just the first.

*Aside — a gdb limitation worth noting*: verifying the new dialog
directly by calling `ShowDeviceInfoDialog()` from `gdb` failed with
"Couldn't find method std::string::std::string" — its two parameters are
`std::string` passed *by value*, and `gdb`'s expression evaluator in
this environment can't synthesize the copy-construction an existing
`std::string` lvalue argument needs for a by-value parameter, a
different flavor of the same string-construction limitation noted
earlier in this document. Verified instead by direct data inspection: the
member-collection loop is textually the same
`if (room.group_id == group_id) members.push_back(room)` pattern already
proven correct for the grouping popover above, over the exact same
`backend_->Rooms()` data already confirmed live to have "Arbeitszimmer"
and "Wohnzimmer" sharing one `group_id` — sufficient to be confident
without a direct call.

### Fixed: a room newly joined to a group had no volume slider

Reported live: the grouping popover's per-room volume sliders (see the
grouping rework above) were missing for some members of a real group.
Root cause: `room_volumes_` (what `GetRoomVolume()` reads, gating
whether a row gets a slider at all) is only ever repopulated by
`RefreshVolumeLocked()`, called from `SelectZone()` once and from
`HandlePlayerEvent()`'s own `SVCEvent_RenderingControlChanged` branch —
but *not* from anywhere reacting to a topology change. Joining a room to
a group is not guaranteed to also fire a `RenderingControlChanged` event
for the selected player, so a room added *after* the zone was first
selected could go the rest of the session with no entry in
`room_volumes_` at all, and therefore no slider — the exact "the
original members have one, something added later doesn't" pattern
reported.

Fixed by also calling `RefreshVolumeLocked()` (and emitting
`signal_volume_changed_`) from `HandleSystemEvent()`'s own
`SVCEvent_ZGTopologyChanged` branch, right alongside the existing
`zones_by_uuid_` refresh — a join/remove already fires that event
reliably (it's what keeps the zone list itself correct), so piggybacking
the volume refresh on it needs no new event subscription.
`RefreshVolumeLocked()` already no-ops safely when `player_` is null
(no zone selected yet), the same guard every other caller already relies
on.

Verified directly against the real household (`gdb`, calling
`backend_->SelectZone()` with an existing zone's own `coordinator_uuid`
— a `const std::string&` parameter, unlike `ShowDeviceInfoDialog()`
above, so no by-value copy-construction problem to work around): with
all four real rooms now merged into one group, `backend_->room_volumes_`
correctly held an entry for every one of them, including the room
specifically reported missing. (One dead end on the way there worth
noting: the household's topology had changed mid-investigation — down
to one 4-member group instead of the earlier 2-member one — and reusing
a stale zone-list index from an earlier check crashed *that specific
gdb-driven test process* outright. Harmless in isolation, since it was
never the user-facing persistent instance, but a reminder that this
technique needs re-reading live state fresh each time, not assuming it
matches an earlier check in the same conversation.)

### A header-bar activity spinner for any backend action in flight

Requested directly: a spinner showing whenever the app is waiting on the
Sonos system, not just during zone discovery (`activity_spinner_`,
renamed from `discovery_spinner_` to match its now-broader purpose —
every reference updated together).

`TaskQueue` (`src/backend/task-queue.h`) serializes every one of
libnoson's ~200 blocking SOAP calls onto its own single worker thread —
already the one choke point every backend action already passes
through, regardless of which of NosonBackend's many public methods
triggered it. Gained an optional `on_busy_changed` constructor callback,
fired `true` right before the first task of a new burst starts and
`false` only once the queue is genuinely empty again — not once per
task — so a rapid sequence of back-to-back actions (a fast volume drag,
a burst of library prefetches) reads as one continuous busy period
rather than flickering the spinner on and off between each one.

The callback fires on `tasks_`'s own worker thread, so `NosonBackend`
marshals it to the main thread the same way every other cross-thread
notification in this class already does — a new `busy_dispatcher_` plus
an `std::atomic<bool> pending_busy_state_` handoff variable (the one
place this class needs that pattern; every *other* dispatcher here only
ever means "go re-read some already-consistent state," not "here's a
value"), surfaced as `signal_busy_changed(bool)`. `GnomosWindow` combines
this with the pre-existing `discovering_` flag (`OnDiscoveryDone()`'s own
state) — either one being true keeps `activity_spinner_` running,
neither being true stops it — since a spinner started by one shouldn't
be stopped by the other finishing first.

### Compacted the sound popover so it no longer needs scrolling

Reported live: with every section (bass/treble/sub-gain, loudness/night
mode/fixed volume, the Autoplay section added earlier this session, and
status LED) always expanded, the popover no longer fit on screen without
scrolling. Folded the two most rarely-touched sections — Autoplay
(Line-In) and Status-LED, five rows between them — behind one collapsed
`Gtk::Expander` ("Erweitert"), the same pattern already proven for the
radio dialog's own manual-entry fallback. Default visible height drops
from every row always shown to just the expander's own collapsed title
row for that whole section, while everything still works identically
once expanded — nothing was removed, just deferred behind an extra
click for the settings most people won't touch every session.

*Aside*: verifying the popover's actual rendered height directly via
`gdb` (`sound_popover_.popup()`, then `get_height()`) didn't pan out —
`gdb` reported "Couldn't find method Gtk::Popover::popup" (and the same
for `Gtk::MenuButton::activate()` as a fallback), consistent with
`gtkmm` methods that are never directly called anywhere in this app's
own compiled code (driven entirely through signals/property bindings
instead) simply having no callable symbol for `gdb` to find, a different
flavor of the `gdb` limitations already noted elsewhere in this
document. Confidence here rests on the row count actually removed from
the default-visible layout, not a measured pixel height.

### Fixed the actual scrolling popover — the grouping one, not the sound one

Follow-up, with a screenshot: the compaction above targeted
`sound_popover_` (bass/treble/loudness/autoplay/...), but the popover the
user actually meant by "Lautstärke Popup" (it has its own per-room
*volume* sliders, hence the name) was `grouping_popover_` — the room
picker's grouping list, showing a real 4-room group with the last row's
slider visibly cut off mid-scroll. That popover's `grouping_scroller_`
already had a hardcoded `set_max_content_height(320)` — with each room
row taking roughly 90-100px (a name/switch row plus its own volume
slider row), 320px barely fit three rooms, not four. Raised to 640 —
`set_propagate_natural_height(true)` was already in place, so this is
only a ceiling (a smaller group still sizes down to its own actual
content), and `Gtk::Popover`'s own screen-edge avoidance still protects
against a household with even more rooms than that overflowing off
screen. The sound popover compaction from the entry above stands on its
own merits regardless — it was a real improvement to a popover that
really had grown tall over the session — just not the one this specific
report was about.

### Replaced the sound popover's plain Gtk::Expander with a proper row

Reported live with two screenshots (collapsed and expanded): the
`Gtk::Expander` from the compaction above "sieht komisch eingepasst
aus" — looks oddly squeezed in. Fair: every *other* row in this popover
(Loudness/Night Mode/Feste Lautstärke) is a label on the left, a control
flush right; `Gtk::Expander` brings its own distinct look instead (an
indented disclosure triangle + label), which reads as a foreign widget
dropped into an otherwise consistent list rather than one more row of
it.

Rebuilt as a flat `Gtk::Button` styled to look exactly like a sibling
row — the same label-left, chevron-right layout as an `Gtk::Image`
(`pan-end-symbolic`/`pan-down-symbolic`, flipped on click) standing in
for where a switch would normally sit — driving a `Gtk::Revealer`
(`SLIDE_DOWN` transition) instead of `Gtk::Expander`'s own default
disclosure behavior. `Gtk::Expander` itself is untouched everywhere else
it's used (the radio dialog's own manual-entry fallback still uses it —
a plain settings-style dialog, not a row-based popover, where the
default look already fits fine) — this was specifically about matching
*this* popover's own established row language, not a blanket "replace
every expander" change.

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
10. **Clicking `room_button_` visibly did nothing.** Root-caused live via
    temporary `g_message()` diagnostics wired to `room_button_`'s "active"
    property and `room_popover_`'s show/closed signals: every click logged
    "shown" immediately followed by "closed" at the same timestamp — the
    popover opened and closed again within the same tick, never actually
    rendering. Cause: `zones_list_box_.select_row()` (called from
    `OnZonesChanged()` to keep the selection in sync) re-fires
    `row-selected` once the popover's content actually maps, and
    `OnZoneRowSelected()` was unconditionally closing the popover on every
    `row-selected` — including that re-fire, closing the very popover that
    had just triggered it. Fixed by moving the `popdown()` call to
    `row-activated` instead, which GTK only emits for genuine user
    interaction (click/Enter), never for a programmatic `select_row()`.

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
