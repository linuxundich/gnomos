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

### Stronger Deezer disclosure, and an A-Z jump index for long lists

- The "Künstlerbilder laden" setting's subtitle now names the actual
  endpoint (`api.deezer.com`), says explicitly that it's a real internet
  request rather than a local Sonos one, and states that Deezer's own
  terms of use apply. A second row right below it links out to
  `developers.deezer.com/api` (`Gtk::LinkButton`, same widget the
  service-linking dialog already uses) so the terms are one click away
  rather than just asserted in prose. `AdwPreferencesGroup` itself also
  gained a description stating that this is the *only* thing in the app
  that isn't local-network-only.
- **A-Z jump index**: `LibraryView` gained `index_strip_`, a narrow
  (28px), `.card`-styled vertical strip along the content's left edge —
  one row per bucket (`'0'` for anything not starting with a Latin
  letter, then `A`..`Z`), clickable when the current level has at least
  one entry in that bucket, dimmed and inert when it doesn't (kept
  visible either way, so the strip's own layout stays a reliable spatial
  guide to "roughly where" a letter is). Only shown at all once
  `SetEntries()` sees at least 30 entries — a short list doesn't need it,
  and the 27-row strip would dwarf one.
  - Bucketing (`BucketFor()`) folds accented Latin letters to their base
    letter via Unicode NFD decomposition + skipping the resulting
    combining mark, so a German entry starting with Ä/Ö/Ü doesn't get
    stranded under "0" just because of the umlaut.
  - Clicking a letter calls `JumpToIndex()`, which resolves the target
    child from `list_box_`/`flow_box_` directly (`grid_mode_active_`
    tracks which one — set by `SetEntries()`, since entry indices map
    onto different widgets depending on list vs. grid mode), computes its
    position via `Gtk::Widget::compute_bounds()` relative to that same
    container, and sets the scrolled window's own vertical adjustment to
    it directly — no smooth-scroll animation, an instant jump, matching
    what the index is actually for (skipping past hundreds of entries at
    once, not a gentle nudge).
  - Applies uniformly to every library level, local and third-party alike
    — Artists/Albums/Genres/Playlists all qualify once they're long
    enough; nothing here is specific to any one entry type.

### A-Z index redesign: compact and scroll-tracking, not a fixed full alphabet

The first version showed all 27 buckets (`0`, `A`-`Z`) at once, stretched
to fill the available height — reported back live as not actually
solving the problem: a shorter window still couldn't fit all 27 rows
legibly, so "you can't see the whole alphabet at the edge anyway" was
still true. Replaced with a compact, always-fully-visible window that
tracks scroll position instead of trying to show everything at once:

- `bucket_order_` (`std::vector<std::pair<char, int>>`) now holds only
  the buckets actually *present* at this level, in alphabet order —
  computed once by `RebuildIndexStrip()`, same bucketing (`BucketFor()`)
  as before.
- `UpdateIndexWindow()`, connected to `scroller_`'s own
  `vadjustment`'s `signal_value_changed()`, finds the last bucket whose
  first entry's own top edge (via `compute_bounds()`) is at or above the
  current scroll position — i.e. "the bucket currently scrolled into",
  the same convention section-header list UIs (iOS Contacts, for one)
  already use — and re-renders `index_strip_` to show only `kContext` (3)
  buckets above and below it, the current one highlighted
  (`suggested-action`). Only re-renders when the *bucket* actually
  changes, not on every scroll pixel.
- `index_strip_` itself is no longer stretched (`vexpand`/`homogeneous`)
  — just `valign(CENTER)`, sized to whatever its current ~7 rows need —
  so it's never taller than the window regardless of how many buckets
  exist in total.
- `JumpToIndex()` needed no changes: setting the vertical adjustment's
  value itself fires `signal_value_changed()`, so clicking a letter
  re-centers the window on the newly-current bucket as a natural
  consequence of `UpdateIndexWindow()` already being connected to that
  signal, not a separate step.

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
disc), so the *same* pixel size still reads as visually larger. Fixed by
giving `CoverThumbnail` a `fallback_pixel_size_` distinct from
`pixel_size_` — 3/5 of it — used only for `set_from_icon_name()` calls
(a new `ShowFallback()` helper wraps every one of them, so the smaller
size can't accidentally get skipped at one call site); real texture
content (`set(scaled)`) is completely unaffected by `pixel_size`, so
still displays at the *full* `pixel_size_` via `GetScaled()` as before.

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
