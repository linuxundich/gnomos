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
  void ShowShortcutsDialog();
  void ShowLinkServiceDialog();
  void ShowSavePlaylistDialog();
  void ShowClearQueueConfirmDialog();
  void ShowDeleteFavoriteConfirmDialog(unsigned index);
  void ShowDeleteAlarmConfirmDialog(std::string alarm_id);
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
  void SetPreferGridView(bool prefer_grid);
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
  Gtk::Spinner discovery_spinner_;
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
  Gtk::Switch loudness_switch_;
  Gtk::Switch nightmode_switch_;
  // Line-out / fixed-volume mode — for a device feeding a receiver/amp with
  // its own volume control, so the device's own volume slider has no
  // effect. Same set_sensitive()-when-unsupported treatment as
  // nightmode_switch_ above (not set_visible()), for a consistent, always
  // in the same place popover layout regardless of which room is selected.
  Gtk::Switch output_fixed_switch_;
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
