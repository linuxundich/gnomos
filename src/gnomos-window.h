// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <adwaita.h>
#include <sigc++/connection.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinner.h>
#include <gtkmm/switch.h>
#include <gtkmm/togglebutton.h>

#include "backend/noson-backend.h"
#include "backend/noson-types.h"
#include "mpris-service.h"
#include "radio-content-filter.h"
#include "widgets/alarms-view.h"
#include "widgets/favorites-view.h"
#include "widgets/history-view.h"
#include "widgets/library-view.h"
#include "widgets/player-bar.h"
#include "widgets/queue-view.h"

namespace gnomos
{

class GnomosWindow : public Gtk::ApplicationWindow
{
public:
  GnomosWindow();
  ~GnomosWindow() override = default;

private:
  void OnRefreshClicked();
  void OnZoneRowSelected(Gtk::ListBoxRow* row);
  // Persists ZoneInfo::coordinator_uuid to $XDG_CONFIG_HOME/gnomos/state.ini
  // — NOT group_id, whose own prefix is not reliably per-zone (confirmed
  // live: two independent zones shared the same group_id prefix).
  void SaveLastRoom(const std::string& coordinator_uuid) const;
  std::string LoadLastRoomUuid() const;
  void RebuildGroupingPopover();
  // Read-only IP/MAC/software-version/model dialog for a whole zone (see
  // the room popover's own info button, which is what opens this) — one
  // section per member room, not just the coordinator's own. Confirmed
  // live: a merged zone's info button used to only ever show the
  // coordinator (the room the group was originally opened from), with no
  // way to see a later-added member's own IP/MAC/etc. at all.
  void ShowDeviceInfoDialog(std::string group_id, std::string zone_name);
  // room_button_'s own label — the current room name, so it still reads
  // correctly without opening room_popover_. Called after every zone
  // selection and every zones_list_box_ rebuild (a topology change can
  // rename the selected zone, e.g. joining/leaving a group).
  void UpdateRoomButtonLabel();
  // nav_list_box_ (the section sidebar: Warteschlange/Favoriten/Alarme/
  // Verlauf/Bibliothek) replaced the old top AdwViewSwitcher. Every row —
  // the five static top-level ones and the library's own sub-items alike
  // — carries its own action in nav_row_actions_ (index-matched, see that
  // member's comment); this just runs the one for the row clicked.
  void OnNavRowSelected(Gtk::ListBoxRow* row);
  // Rebuilds the indented library sub-item rows nested under "Bibliothek"
  // (Interpreten/Alben/.../linked services) from library_root_entries_ —
  // called whenever the library root is (re)fetched (see OnLibraryChanged())
  // so a newly linked/unlinked service is reflected without restarting.
  // Only ever touches rows after the five static ones — see
  // nav_row_actions_'s own comment for why those are never rebuilt.
  void RebuildLibraryNavEntries();
  void ShowAddAlarmDialog();
  // duplicate: pre-fills every field from *existing (same as editing), but
  // always creates a brand-new alarm rather than updating *existing's own
  // one — forces alarm_id empty and skips the "Aktueller Klang beibehalten"
  // sound option, which only makes sense for an alarm that already exists
  // server-side. See OnAlarmDuplicateRequested().
  void ShowAlarmDialog(const AlarmInfo* existing, bool duplicate = false);
  void OnAlarmEditRequested(std::string alarm_id);
  void OnAlarmDuplicateRequested(std::string alarm_id);
  void ShowAboutDialog();
  void ShowSettingsDialog();
  // Closes and immediately reopens open_settings_dialog_, if one is
  // currently open, so an async change that happened while it was open
  // (e.g. Last.fm's auth flow completing) shows up without the user
  // having to close and reopen it manually themselves — reported live
  // as confusing before this existed. A no-op otherwise.
  void RefreshOpenSettingsDialog();
  void ShowShortcutsDialog();
  void ShowLinkServiceDialog();
  void ShowSavePlaylistDialog();
  // URL + optional title entry for NosonBackend::PlayStreamAsync() — a
  // one-off, unsaved stream play, reached from the primary menu. Distinct
  // from ShowAddRadioStationDialog(), which always persists a favorite.
  void ShowPlayStreamDialog();
  // Local JSON backup of radio favorites only — see
  // NosonBackend::GetExportableRadioFavorites()'s own comment for why not
  // every favorite type. Reached from the primary menu.
  void ExportRadioFavorites();
  void ImportRadioFavorites();
  // Scenes (grouping presets) — captures every room's current group
  // (which coordinator, if any, it's joined to) and, best-effort, its
  // current volume, as a named preset restorable in one click. Purely
  // client-side, composing JoinRoomToZone()/RemoveRoomFromGroup()/
  // SetRoomVolume(), the same calls the grouping popover's own switches
  // and sliders already make — no new backend/libnoson surface needed.
  void LoadScenes();
  void SaveScenes() const;
  void CaptureCurrentAsScene(const std::string& name);
  void ApplyScene(const std::string& name);
  void DeleteScene(const std::string& name);
  void ShowScenesDialog();
  void ShowSaveSceneDialog();
  void ShowClearQueueConfirmDialog();
  // Same confirm-dialog treatment as ShowClearQueueConfirmDialog() — a
  // multi-item removal is just as unrecoverable as clearing the whole
  // queue. indices are whatever QueueView::signal_remove_selected_requested()
  // last carried (always non-empty — see that signal's own comment).
  void ShowRemoveSelectedQueueItemsConfirmDialog(std::vector<unsigned> indices);
  void ShowDeleteFavoriteConfirmDialog(unsigned index);
  void ShowDeleteAlarmConfirmDialog(std::string alarm_id);
  // Handles both cases LibraryView::signal_delete_requested() can mean —
  // deleting a saved playlist (browsing "SQ:") or a custom radio station
  // (browsing "R:0/0") — branching on library_stack_.back().first, since
  // the signal itself only carries the row index either way.
  void ShowDeleteLibraryEntryConfirmDialog(unsigned index);
  // Title + stream URL entry for NosonBackend::AddRadioStation() — reached
  // from LibraryView's add_button_, only visible while browsing "R:0/0".
  void ShowAddRadioStationDialog();
  // Enabled switch + regex filter entry for one radio station's
  // RadioMprisSettings — governs MPRIS reporting *and* Verlauf recording
  // for that station (both go through their own RadioContentFilter
  // instance, reading the same per-station settings). Reached from
  // LibraryView's per-row gear button, only visible while browsing
  // "R:0/0". No-op if current_library_entries_[index].stream_uri is
  // empty (shouldn't happen while browsing "R:0/0", but guards against an
  // out-of-sync index).
  void ShowRadioMprisSettingsDialog(unsigned index);
  // Fetches the saved-playlist list fresh (NosonBackend::FetchSavedPlaylistsAsync())
  // and shows a picker once it arrives — reached from LibraryView's
  // per-row "add to playlist" button.
  void ShowAddToPlaylistDialog(unsigned library_index);
  // Shared AdwAlertDialog wrapper for all three confirmations above — see
  // its header comment for why this replaced three hand-rolled Gtk::Window
  // dialogs.
  void ShowConfirmDialog(const std::string& heading, const std::string& body, const std::string& confirm_label,
                          std::function<void()> on_confirmed);
  void ShowTrackInfoDialog();
  // prefill is pre-filled into the search entry (e.g. from the "Interpret
  // suchen" button in the Track-Details dialog) but not auto-submitted —
  // the dialog is shown either way, so the user can confirm or adjust the
  // scope (local library vs. the currently linked service) before firing.
  void ShowLibrarySearchDialog(const std::string& prefill = "");
  void OnServiceLinkReady(std::string url, std::string code);

  void OnDiscoveryDone(bool ok);
  // backend_->signal_busy_changed() — a burst of one or more queued
  // backend actions (any button/switch/slider anywhere in the app) counts
  // as one continuous busy period. Combined with discovering_ (both drive
  // the same activity_spinner_, see UpdateActivitySpinner()) rather than
  // each owning it outright, since either one alone stopping shouldn't
  // stop a spinner the other one still wants running.
  void OnBusyChanged(bool busy);
  void UpdateActivitySpinner();
  void OnZonesChanged();
  void OnPlayerReady();
  void OnNowPlayingChanged();
  void OnPositionChanged();
  bool OnPositionTimerTick();
  void OnVolumeChanged();
  void OnQueueChanged();
  // Shared by OnNowPlayingChanged() and OnQueueChanged() — both change one
  // of the two inputs (current_queue_index_, the queue itself) this hint
  // depends on.
  void UpdateNextTrackHint();
  void OnFavoritesChanged();
  void OnAlarmsChanged();
  void OnLibraryChanged();
  void OnLibraryEntryActivated(unsigned index);
  void OnLibraryBackRequested();
  void OnSleepTimerChanged();
  void OnSoundSettingsChanged();
  void OnBackendError(std::string message);

  // Records a HistoryEntry snapshot whenever the track actually changes
  // (not on every OnNowPlayingChanged() re-fire for the same track), and
  // persists it to $XDG_CONFIG_HOME/gnomos/history.ini.
  void RecordHistoryIfTrackChanged(const NowPlaying& now_playing);
  // Surfaces two AVTProperty conditions that otherwise have no visible
  // indication at all: an alarm actively ringing in the selected room
  // (with a "Stoppen" toast action, win.stop-alarm), and the device
  // itself reporting a transport error. Both edge-triggered off a
  // last-known flag, so each condition only toasts once per occurrence,
  // not on every now-playing refresh while it stays true.
  void CheckAlarmAndTransportStatus(const NowPlaying& now_playing);
  void LoadHistory();
  void SaveHistory() const;
  // AdwStyleManager's light/dark override, persisted to state.ini
  // ([appearance] color_scheme) — see ApplyColorScheme()'s header comment.
  void LoadColorScheme();
  void ApplyColorScheme(const std::string& scheme);
  // [notifications] track_change in state.ini — see notify_on_track_change_'s
  // own comment.
  void LoadNotificationSetting();
  void SetNotifyOnTrackChange(bool enabled);
  void SendTrackChangeNotification(const NowPlaying& now_playing);
  // [library] prefer_grid in state.ini — see prefer_grid_view_'s own
  // comment.
  void LoadLibraryViewPreference();
  // [library] load_artist_images in state.ini — see load_artist_images_'s
  // own comment.
  void LoadArtistImagesSetting();
  void SetLoadArtistImages(bool enabled);
  // [player] load_lyrics in state.ini — see load_lyrics_'s own comment.
  void LoadLyricsSetting();
  void SetLoadLyrics(bool enabled);
  // [track_info_dialog] width/height in state.ini — see
  // track_info_dialog_width_'s own comment.
  void LoadTrackInfoDialogSize();
  void SaveTrackInfoDialogSize(int width, int height);
  // [scrobbling] listenbrainz_token in state.ini — see
  // listenbrainz_token_'s own comment.
  void LoadListenBrainzToken();
  void SetListenBrainzToken(const std::string& token);
  // [scrobbling] lastfm_api_key/lastfm_shared_secret/lastfm_session_key/
  // lastfm_username in state.ini — see lastfm_session_key_'s own comment.
  void LoadLastFmSettings();
  void SetLastFmApiCredentials(const std::string& api_key, const std::string& shared_secret);
  void SetLastFmSession(const std::string& session_key, const std::string& username);
  void DisconnectLastFm();
  // Desktop-auth flow (see LastFmScrobbler's own header comment for why
  // this needs 3 steps instead of ListenBrainz's single pasted token):
  // requests a token, then shows ShowLastFmAuthDialog() with the browser
  // link to authorize it.
  void StartLastFmAuth();
  void ShowLastFmAuthDialog(const std::string& token);
  // Schedules (or cancels/reschedules, on a genuine track change) a
  // one-shot scrobble for `np` once it's been "listened to" long enough —
  // see its own definition for what that means and why. A no-op while
  // neither listenbrainz_token_ nor lastfm_session_key_ is set.
  void MaybeScheduleScrobble(const NowPlaying& np);
  void SetPreferGridView(bool prefer_grid);
  // [library] fallback_icon_scale in state.ini — see fallback_icon_scale_'s
  // own comment.
  void LoadFallbackIconScaleSetting();
  void SetFallbackIconScale(double scale);
  // Window width/height/maximized, persisted to state.ini's [window] group
  // alongside last_room_uuid — restores the size the user actually left
  // the app at, instead of always reopening at the fixed default_size().
  void LoadWindowState();
  bool OnCloseRequest();
  // split_view_'s sidebar_width_fraction, same state.ini [window] group
  // as LoadWindowState() — separate function since it must run after
  // split_view_ exists (LoadWindowState() runs early in the constructor,
  // before it does).
  void LoadSplitFractions();
  bool OnKeyPressed(guint keyval, guint keycode, Gdk::ModifierType state);

  void ShowToast(const std::string& message);
  // Polls NosonBackend::CheckLibraryIndexProgressAsync() every 2s after a
  // "Bibliothek neu einlesen" click, since RefreshLibraryIndex() itself only
  // reports a failure to *start* the scan — surfaces a completion/failure
  // toast once ShareIndexInProgress is actually observed dropping back to
  // false after having been true, or gives up silently after a handful of
  // ticks if it's never observed running at all (a scan too fast to catch
  // between polls, or one that never really started).
  void StartLibraryIndexProgressPolling();

  std::unique_ptr<NosonBackend> backend_;
  // Constructed after backend_ (needs it) and destroyed before it
  // (implicitly, via reverse declaration order) — it only ever reads
  // backend_ through its own signal connections and getters, same as every
  // other backend consumer in this class.
  std::unique_ptr<MprisService> mpris_;

  // libadwaita widgets, constructed via the C API and used through gtkmm
  // only as plain Gtk::Widget children — gtkmm has no bindings for
  // libadwaita, so Adwaita-specific behaviour is always driven through the
  // raw ADW_*() pointers kept here rather than through gtkmm method calls.
  GtkWidget* header_bar_ = nullptr;
  GtkWidget* toast_overlay_ = nullptr;

  Gtk::Label window_title_;
  // Spins while either discovering_ (zone discovery in progress) or
  // backend_busy_ (any other action currently waiting on the Sonos
  // system) is true — see UpdateActivitySpinner()'s own comment for why
  // both conditions share the one spinner rather than each getting its
  // own indicator.
  Gtk::Spinner activity_spinner_;
  bool discovering_ = false;
  bool backend_busy_ = false;
  Gtk::Button refresh_button_;
  Gtk::MenuButton primary_menu_button_;

  // Room/zone picker — a header-bar popover now, not a permanent sidebar
  // (see split_view_'s own comment for what replaced it there). Reuses
  // zones_list_box_/zones_scroller_'s exact row-building logic
  // (OnZonesChanged()) and selection handling (OnZoneRowSelected()); only
  // where they're displayed changed. room_button_'s own label always
  // shows the current room name (UpdateRoomButtonLabel()), so the room
  // stays visible without opening the popover.
  Gtk::MenuButton room_button_;
  // AdwButtonContent (raw C API, same reasoning as header_bar_/
  // toast_overlay_ — no gtkmm binding exists) — gives room_button_ an
  // icon *and* a text label together (the current room name), which
  // Gtk::MenuButton's own set_icon_name()/set_label() can't combine on
  // their own. Kept as a member so UpdateRoomButtonLabel() can update the
  // label after construction.
  GtkWidget* room_button_content_ = nullptr;
  Gtk::Popover room_popover_;

  Gtk::MenuButton grouping_button_;
  Gtk::Popover grouping_popover_;
  Gtk::ListBox grouping_list_box_;

  Gtk::MenuButton input_button_;
  Gtk::Popover input_popover_;

  Gtk::MenuButton sleep_timer_button_;
  Gtk::Popover sleep_timer_popover_;
  Gtk::Label sleep_timer_status_label_;

  Gtk::MenuButton sound_button_;
  Gtk::Popover sound_popover_;
  Gtk::Scale bass_scale_;
  Gtk::Scale treble_scale_;
  // Sub gain — same set_sensitive()-when-unsupported treatment as
  // nightmode_switch_/output_fixed_switch_ (no GetSupportsSubGain() exists
  // to check up front; "supported" is inferred from whether GetSubGain()
  // itself succeeds — see SoundSettings::sub_gain_supported).
  Gtk::Scale sub_gain_scale_;
  Gtk::Switch loudness_switch_;
  Gtk::Switch nightmode_switch_;
  // Line-out / fixed-volume mode — for a device feeding a receiver/amp with
  // its own volume control, so the device's own volume slider has no
  // effect. Same set_sensitive()-when-unsupported treatment as
  // nightmode_switch_ above (not set_visible()), for a consistent, always
  // in the same place popover layout regardless of which room is selected.
  Gtk::Switch output_fixed_switch_;
  // Line-in autoplay — see SoundSettings::autoplay_* for what each of
  // these means. autoplay_volume_scale_ is only sensitive while
  // autoplay_use_volume_switch_ is on, same "grey out the control that
  // doesn't apply yet" idea as sub_gain_scale_'s own sensitivity toggle.
  Gtk::Switch autoplay_switch_;
  Gtk::Switch autoplay_use_volume_switch_;
  Gtk::Scale autoplay_volume_scale_;
  bool suppress_sound_signals_ = false;

  // Section sidebar (Warteschlange/Favoriten/Alarme/Verlauf/Bibliothek,
  // noson-app-style — see nav_list_box_'s own comment) | tab content, with
  // player_bar_ docked as its own fixed-height bar along the bottom of
  // the whole window (see the constructor's root_box) — not part of this
  // split at all. split_view_ is an AdwOverlaySplitView (not a plain
  // Gtk::Paned) so the sidebar can collapse behind sidebar_toggle_button_
  // via an AdwBreakpoint on narrow windows. Room/zone selection used to
  // live in this same slot as a second, permanent sidebar; moved to
  // room_button_'s popover instead (see its own comment) — a Sonos
  // household only ever needs occasional room switching, not a
  // permanently-docked panel for it, and it freed this slot for section
  // navigation instead.
  GtkWidget* split_view_ = nullptr;
  Gtk::ToggleButton sidebar_toggle_button_;
  // Icon+label rows for the five view_stack_ pages, replacing the
  // AdwViewSwitcher this app used to have as a top tab bar — styled after
  // noson-app's own left-hand navigation (Meine Dienste/Mein
  // Musikverzeichnis/Meine Radiosender/Favoriten/Wiedergabelisten/Wecker/
  // Dieses Gerät). Below the "Bibliothek" row, RebuildLibraryNavEntries()
  // appends one indented, icon-less row per root library category
  // (Interpreten/Alben/Genres/Titel/Playlisten/Radiosender) and per linked
  // service (Spotify, bonob, ...) — the same list BrowseLibraryAsync("")
  // returns for the library's own root level, so jumping straight to
  // "Interpreten" from the sidebar doesn't need a second source of truth.
  Gtk::ListBox nav_list_box_;
  // One action per nav_list_box_ row, in the same append order — the five
  // static top-level rows first (built once in the constructor and never
  // rebuilt, so their selection state and row identity survive library
  // refreshes), then the library's own sub-items (rebuilt in place by
  // RebuildLibraryNavEntries() whenever the library root changes).
  // OnNavRowSelected() just runs nav_row_actions_[row->get_index()]() —
  // same index-into-a-parallel-vector pattern current_zones_ already uses,
  // but per-row *behavior* rather than per-row *data*, since a sub-item's
  // action (jump to that library category) differs in kind from a
  // top-level row's (switch view_stack_ page).
  std::vector<std::function<void()>> nav_row_actions_;

  // Room/zone list — see room_button_'s own comment on where it's shown
  // now; this pair of widgets is unchanged from when it was the permanent
  // sidebar, just re-parented into room_popover_ instead of split_view_.
  Gtk::ListBox zones_list_box_;
  Gtk::ScrolledWindow zones_scroller_;
  Gtk::Label zones_placeholder_;

  PlayerBar player_bar_;
  QueueView queue_view_;
  FavoritesView favorites_view_;
  AlarmsView alarms_view_;
  HistoryView history_view_;
  LibraryView library_view_;
  std::vector<HistoryEntry> history_;
  // "title\x1fartist" of the last-recorded entry — lets
  // RecordHistoryIfTrackChanged() tell a genuine track change apart from a
  // re-fire of OnNowPlayingChanged() for the same track (e.g. shuffle/repeat
  // toggles, which emit the same signal without changing the track).
  std::string last_history_key_;
  // RecordHistoryIfTrackChanged()'s own instance for radio-content spam
  // filtering — treats History (and, downstream, the track-change
  // notification below) the same way MprisService treats MPRIS Metadata:
  // ad breaks between song repeats shouldn't spam either one. Own
  // instance, not shared with MprisService's — see RadioContentFilter's
  // own comment for why. unique_ptr, not a plain member, since it needs
  // *backend_ — only assigned in the constructor body, same reasoning as
  // mpris_ below.
  std::unique_ptr<RadioContentFilter> radio_history_filter_;
  // Same reasoning as radio_history_filter_ (its own instance, not shared
  // — see RadioContentFilter's own comment), but for ShowTrackInfoDialog()'s
  // lyrics lookup: fed on every OnNowPlayingChanged() tick (not just while
  // the dialog happens to be open) so its sticky effective_content_ already
  // holds the last real song by the time a dialog opens, even if that
  // exact moment lands on an ad break — see OnNowPlayingChanged()'s own
  // comment.
  std::unique_ptr<RadioContentFilter> radio_lyrics_filter_;
  // Whether to send a desktop notification on a genuine track change —
  // reuses RecordHistoryIfTrackChanged()'s own dedup key, so this never
  // fires twice for the same track. Off by default (opt-in via Settings);
  // persisted to state.ini's [notifications] group.
  bool notify_on_track_change_ = false;
  // Edge-trigger state for CheckAlarmAndTransportStatus() — see its own
  // comment.
  bool last_alarm_running_ = false;
  bool last_transport_status_ok_ = true;
  // User override for grid-eligible library levels (Albums/Artists and
  // similar) — OnLibraryChanged() only ever shows a grid at all when
  // LibraryEntry::display_as_grid says the level supports one; this is
  // just whether the user currently *wants* that when it's available, so
  // they can switch back to a plain list for the same levels a grid would
  // otherwise apply to. Global rather than per-level: simpler to reason
  // about and persist, and matches how the toggle button itself reads
  // (one on/off control, not a per-level memory). Persisted to
  // state.ini's [library] group.
  bool prefer_grid_view_ = true;
  // Off by default (opt-in via Settings) — every enabled lookup sends an
  // artist's name to Deezer's public API (see ArtistImageFetcher). One of
  // two opt-in features (alongside load_lyrics_) that talk to anything
  // beyond the local Sonos household. Persisted to state.ini's [library]
  // group, alongside prefer_grid_view_.
  bool load_artist_images_ = false;
  // Off by default (opt-in via Settings) — every enabled lookup sends the
  // current track's artist/title/album to LRCLIB's public API (see
  // LyricsFetcher). Persisted to state.ini's [player] group.
  bool load_lyrics_ = false;
  // ShowTrackInfoDialog()'s own remembered size — 0 means "never saved
  // yet," in which case that method falls back to a one-time default (420
  // wide, this window's own current height) instead of letting the
  // dialog's natural/content-driven size decide, which made it wildly
  // inconsistent between separate opens: short before Songtexte finished
  // loading, then very tall on a later open once LyricsFetcher's cache
  // made the full lyrics available immediately during layout. Updated on
  // every close, in state.ini's own [track_info_dialog] group, so it
  // tracks whatever the user last resized it to.
  int track_info_dialog_width_ = 0;
  int track_info_dialog_height_ = 0;
  // Empty means scrobbling is off — the token itself gates the feature,
  // no separate on/off switch needed (there's nothing useful this could do
  // without one anyway). Every enabled scrobble sends the current track's
  // artist/title/album to api.listenbrainz.org. Persisted to state.ini's
  // own [scrobbling] group.
  std::string listenbrainz_token_;
  // Last.fm's own equivalent — see LastFmScrobbler's own header comment
  // for why this needs three separate pieces of state instead of
  // ListenBrainz's single pasted token: api_key/shared_secret are a
  // registered API application's own credentials (obtained by the user at
  // last.fm/api/account/create, entered in Settings), session_key is the
  // long-lived credential the 3-step desktop-auth flow produces from
  // those (see StartLastFmAuth()), and username is purely cosmetic — shown
  // in Settings as "Verbunden als …" once linked. Scrobbling to Last.fm is
  // active exactly when session_key is non-empty; api_key/shared_secret
  // alone (entered but not yet authorized) don't enable anything on their
  // own. All persisted to state.ini's [scrobbling] group, alongside
  // listenbrainz_token_.
  std::string lastfm_api_key_;
  std::string lastfm_shared_secret_;
  std::string lastfm_session_key_;
  std::string lastfm_username_;
  // The currently open AdwPreferencesDialog from ShowSettingsDialog(), if
  // any — nullptr otherwise. Tracked (set on open, cleared via its own
  // "closed" signal) purely so RefreshOpenSettingsDialog() can close and
  // reopen it after an async change lands while it's open; this dialog is
  // otherwise unmanaged by GnomosWindow (AdwDialog owns its own lifetime,
  // unlike the plain Gtk::Window dialogs elsewhere in this file that need
  // an explicit `delete`).
  AdwDialog* open_settings_dialog_ = nullptr;
  // MaybeScheduleScrobble()'s own dedup key ("title\x1fartist" of the
  // track a scrobble is currently scheduled or already sent for) and
  // one-shot timer — see that method's own comment.
  std::string last_scrobble_scheduled_key_;
  sigc::connection scrobble_timer_connection_;
  // Loaded once at startup (LoadScenes()), kept in memory and rewritten in
  // full on every change — see SaveScenes()'s own comment. Persisted to
  // its own scenes.ini, one Glib::KeyFile group per scene (its name is the
  // group name), separate from state.ini since this can grow unbounded
  // with the household's own room count/scene count, unlike everything
  // else state.ini holds.
  std::vector<RoomScene> scenes_;
  // How large a CoverThumbnail fallback icon's own glyph renders relative
  // to its tile — see CoverThumbnail::SetFallbackIconScale()'s own
  // comment for why this is the user's own call rather than a fixed
  // value: some fallback icons' glyphs fill nearly their whole canvas,
  // unlike more generously-padded ones, so the same box can still read as
  // visually bigger depending which icon a given level happens to show.
  // 1.0 (full size) is the default — deliberately matching what this
  // looked like before a fixed 3/5 shrink was tried as a fix and reported
  // back as solving the wrong problem (the *tile* size was never the
  // complaint). Persisted to state.ini's [library] group, alongside
  // prefer_grid_view_/load_artist_images_.
  double fallback_icon_scale_ = 1.0;
  // Debounces SetFallbackIconScale()'s own save-to-disk + OnLibraryChanged()
  // rebuild — see that method's own comment for why (confirmed live: an
  // AdwSpinRow fires notify::value many times a second while being
  // dragged/scrolled, and each one re-rendering a potentially large grid
  // back-to-back crashed the app).
  sigc::connection fallback_icon_scale_debounce_connection_;
  // (object_id, display title) from root to current level; back() is the
  // level currently shown. Root is {"", "Bibliothek"}.
  std::vector<std::pair<std::string, std::string>> library_stack_;
  std::vector<LibraryEntry> current_library_entries_;
  // The root level's own entries specifically (a copy of
  // current_library_entries_ taken whenever library_stack_.size() == 1) —
  // kept separately since current_library_entries_ tracks whatever level is
  // currently browsed, which is usually *not* the root once the user has
  // navigated in. RebuildLibraryNavEntries() reads this to populate the
  // sidebar's library sub-items, independent of the LibraryView's own
  // current depth.
  std::vector<LibraryEntry> library_root_entries_;
  // Set right before BeginServiceLink(); consumed once the link succeeds to
  // push a matching breadcrumb onto library_stack_ (see OnServiceLinkReady()'s
  // "Fertig" handler).
  std::string pending_link_service_id_;
  std::string pending_link_service_name_;
  // AdwViewStack/AdwViewSwitcher, same C-API-plus-wrap approach as
  // header_bar_/toast_overlay_ — no gtkmm binding for these exists either.
  GtkWidget* view_stack_ = nullptr;

  std::vector<ZoneInfo> current_zones_;
  std::string selected_group_id_;
  // Loaded once at startup from state.ini; consumed (matched against, then
  // cleared regardless of outcome) the first time OnZonesChanged() sees a
  // non-empty zone list, so a later manual room switch is never overridden
  // by a stale restore attempt.
  std::string pending_restore_room_uuid_;

  // Elapsed playback position has no UPnP event, unlike everything else in
  // this window — so, unlike every other On*Changed() handler, it needs a
  // poll loop rather than a pure event-driven refresh. Started once in the
  // constructor and left running for the window's whole lifetime;
  // OnPositionTimerTick() itself skips the network call whenever nothing is
  // actively playing, so there's no need to start/stop it around zone
  // selection.
  sigc::connection position_timer_connection_;

  // See StartLibraryIndexProgressPolling()'s own comment — both
  // disconnected together once a scan is observed finishing (or polling
  // gives up on ever seeing it start), and re-created on each new "Bibliothek
  // neu einlesen" click, so back-to-back clicks don't stack up timers.
  sigc::connection library_index_poll_connection_;
  sigc::connection library_index_status_connection_;

  // Polls per-room now-playing snapshots while room_popover_ is open;
  // started on signal_show(), stopped on signal_closed().
  sigc::connection room_now_playing_poll_connection_;

  // Space = play/pause when no text-entry field has focus — see
  // OnKeyPressed()'s header comment for why an Editable check is needed.
  Glib::RefPtr<Gtk::EventControllerKey> key_controller_;

  // Cached from the last OnNowPlayingChanged() so OnQueueChanged() can
  // re-apply the same highlight after SetItems() rebuilds every row —
  // queue contents and now-playing state arrive as two independent,
  // asynchronous backend signals, each needing the other's latest value.
  int current_queue_index_ = -1;
};

}  // namespace gnomos
