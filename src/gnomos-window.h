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
  void ShowAddAlarmDialog();
  void ShowAlarmDialog(const AlarmInfo* existing);
  void OnAlarmEditRequested(std::string alarm_id);
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
  bool suppress_sound_signals_ = false;

  // Room sidebar | tab content, with player_bar_ docked as its own
  // fixed-height bar along the bottom of the whole window (see the
  // constructor's root_box) — not part of this split at all. split_view_
  // is an AdwOverlaySplitView (not a plain Gtk::Paned) so the sidebar can
  // collapse behind sidebar_toggle_button_ via an AdwBreakpoint on narrow
  // windows.
  GtkWidget* split_view_ = nullptr;
  Gtk::ToggleButton sidebar_toggle_button_;
  Gtk::ListBox zones_list_box_;
  Gtk::ScrolledWindow zones_scroller_;
  Gtk::Label zones_placeholder_;

  Gtk::Box content_box_{Gtk::Orientation::VERTICAL, 0};
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
