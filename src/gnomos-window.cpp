// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnomos-window.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/texture.h>
#include <giomm/application.h>
#include <giomm/asyncresult.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <giomm/menu.h>
#include <giomm/notification.h>
#include <glib.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>
#include <glibmm/keyfile.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/box.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/image.h>
#include <gtkmm/linkbutton.h>
#include <gtkmm/scale.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include "config.h"
#include "widgets/art-cache.h"

namespace gnomos
{

GnomosWindow::GnomosWindow()
{
  set_title("Gnomos");
  set_default_size(1280, 700);  // room sidebar + tab content + the wider Now Playing panel — overridden by LoadWindowState() below if a size was saved from a previous run
  LoadWindowState();
  signal_close_request().connect(sigc::mem_fun(*this, &GnomosWindow::OnCloseRequest), false);

  // --- Header bar (libadwaita, built via the C API — see header comment) ---
  header_bar_ = adw_header_bar_new();
  window_title_.set_text("Gnomos");
  window_title_.add_css_class("title");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(window_title_.gobj()));

  // Sidebar toggle — only ever visible once the AdwOverlaySplitView below
  // has collapsed the room sidebar behind a breakpoint on narrow windows;
  // bound to split_view_'s own "collapsed"/"show-sidebar" properties once
  // split_view_ exists further down this constructor.
  sidebar_toggle_button_.set_icon_name("sidebar-show-symbolic");
  sidebar_toggle_button_.set_tooltip_text("Räume ein-/ausblenden");
  sidebar_toggle_button_.set_visible(false);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(sidebar_toggle_button_.gobj()));

  // --- Primary menu (Help / About) — packed first so it ends up as the
  // rightmost header bar item; pack_end() adds each new widget to the left
  // of the previous ones. ---
  add_action("about", sigc::mem_fun(*this, &GnomosWindow::ShowAboutDialog));
  add_action("settings", sigc::mem_fun(*this, &GnomosWindow::ShowSettingsDialog));
  add_action("shortcuts", sigc::mem_fun(*this, &GnomosWindow::ShowShortcutsDialog));
  // Wired to the "Stoppen" button on the ringing-alarm toast — see
  // CheckAlarmAndTransportStatus(). Stopping transport in the room stops
  // the alarm regardless of which one it was, same call the play/pause
  // button already uses.
  add_action("stop-alarm", [this] { backend_->PauseOrStop(); });
  auto primary_menu = Gio::Menu::create();
  primary_menu->append("Einstellungen", "win.settings");
  primary_menu->append("Tastenkürzel", "win.shortcuts");
  primary_menu->append("Über Gnomos", "win.about");
  primary_menu_button_.set_icon_name("open-menu-symbolic");
  primary_menu_button_.set_tooltip_text("Hauptmenü");
  primary_menu_button_.set_menu_model(primary_menu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(primary_menu_button_.gobj()));

  // Now Playing panel toggle — only ever visible once content_split_view_
  // has collapsed it behind a breakpoint on narrow windows, same
  // "collapsed"-bound-to-"visible" pattern as sidebar_toggle_button_ (see
  // the constructor further down for where content_split_view_ exists).
  now_playing_toggle_button_.set_icon_name("sidebar-show-right-symbolic");
  now_playing_toggle_button_.set_tooltip_text("Wiedergabe ein-/ausblenden");
  now_playing_toggle_button_.set_visible(false);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(now_playing_toggle_button_.gobj()));

  discovery_spinner_.set_margin_start(6);
  discovery_spinner_.set_margin_end(6);
  refresh_button_.set_icon_name("view-refresh-symbolic");
  refresh_button_.set_tooltip_text("Sonos-Geräte suchen");
  refresh_button_.signal_clicked().connect(sigc::mem_fun(*this, &GnomosWindow::OnRefreshClicked));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(refresh_button_.gobj()));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(discovery_spinner_.gobj()));

  // --- Grouping popover: which rooms play together with the selected zone ---
  grouping_list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  grouping_list_box_.add_css_class("boxed-list");
  auto* grouping_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  grouping_scroller->set_child(grouping_list_box_);
  grouping_scroller->set_size_request(260, -1);
  grouping_scroller->set_max_content_height(320);
  grouping_scroller->set_propagate_natural_height(true);
  grouping_scroller->set_margin_start(6);
  grouping_scroller->set_margin_end(6);
  grouping_scroller->set_margin_bottom(6);

  // "Group all" — joins every room not already part of the current zone to
  // it in one go, reusing the exact same JoinRoomToCurrentZone() each
  // per-room switch already calls. Matches noson-app's own "group all
  // zones" action (Zones.qml, onGroupAllZoneClicked -> zoneList.selectAll()).
  auto* group_all_button = Gtk::make_managed<Gtk::Button>("Alle Räume gruppieren");
  group_all_button->add_css_class("flat");
  group_all_button->set_margin_top(6);
  group_all_button->set_margin_start(6);
  group_all_button->set_margin_end(6);
  group_all_button->set_margin_bottom(6);
  group_all_button->signal_clicked().connect([this] {
    for (const RoomInfo& room : backend_->Rooms())
      if (room.group_id != selected_group_id_)
        backend_->JoinRoomToCurrentZone(room.player_uuid);
  });

  auto* grouping_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  grouping_box->append(*group_all_button);
  grouping_box->append(*Gtk::make_managed<Gtk::Separator>());
  grouping_box->append(*grouping_scroller);
  grouping_popover_.set_child(*grouping_box);
  grouping_popover_.signal_show().connect(sigc::mem_fun(*this, &GnomosWindow::RebuildGroupingPopover));

  grouping_button_.set_icon_name("audio-speakers-symbolic");
  grouping_button_.set_tooltip_text("Räume gruppieren");
  grouping_button_.set_popover(grouping_popover_);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(grouping_button_.gobj()));

  // --- Input source popover (line-in / digital-in on the current zone) ---
  auto* input_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  input_box->set_margin_top(6);
  input_box->set_margin_bottom(6);
  input_box->set_margin_start(6);
  input_box->set_margin_end(6);
  auto* line_in_button = Gtk::make_managed<Gtk::Button>("Line-In");
  line_in_button->add_css_class("flat");
  line_in_button->signal_clicked().connect([this] {
    backend_->PlayLineIn();
    input_popover_.popdown();
  });
  auto* digital_in_button = Gtk::make_managed<Gtk::Button>("Digital-In");
  digital_in_button->add_css_class("flat");
  digital_in_button->signal_clicked().connect([this] {
    backend_->PlayDigitalIn();
    input_popover_.popdown();
  });
  input_box->append(*line_in_button);
  input_box->append(*digital_in_button);
  input_popover_.set_child(*input_box);

  // No dedicated line-in/aux icon exists in the Adwaita icon theme;
  // audio-input-microphone-symbolic is the closest generic "audio input"
  // icon (confirmed present: /usr/share/icons/Adwaita/symbolic/devices/).
  input_button_.set_icon_name("audio-input-microphone-symbolic");
  input_button_.set_tooltip_text("Eingang");
  input_button_.set_popover(input_popover_);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(input_button_.gobj()));

  // --- Sleep timer popover ---
  auto* sleep_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  sleep_box->set_margin_top(6);
  sleep_box->set_margin_bottom(6);
  sleep_box->set_margin_start(6);
  sleep_box->set_margin_end(6);
  sleep_timer_status_label_.add_css_class("dim-label");
  sleep_timer_status_label_.add_css_class("caption");
  sleep_box->append(sleep_timer_status_label_);
  sleep_box->append(*Gtk::make_managed<Gtk::Separator>());
  // (label, minutes) — 0 cancels an active timer.
  static const std::array<std::pair<const char*, unsigned>, 6> kSleepPresets = {{
      {"Aus", 0},
      {"15 Minuten", 15},
      {"30 Minuten", 30},
      {"45 Minuten", 45},
      {"60 Minuten", 60},
      {"90 Minuten", 90},
  }};
  for (const auto& [label, minutes] : kSleepPresets)
  {
    auto* preset_button = Gtk::make_managed<Gtk::Button>(label);
    preset_button->add_css_class("flat");
    preset_button->signal_clicked().connect([this, minutes] {
      backend_->SetSleepTimer(minutes * 60);
      sleep_timer_popover_.popdown();
    });
    sleep_box->append(*preset_button);
  }
  sleep_timer_popover_.set_child(*sleep_box);

  sleep_timer_button_.set_icon_name("weather-clear-night-symbolic");
  sleep_timer_button_.set_tooltip_text("Sleep-Timer");
  sleep_timer_button_.set_popover(sleep_timer_popover_);
  sleep_timer_popover_.signal_show().connect([this] { backend_->RefreshSleepTimerAsync(); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(sleep_timer_button_.gobj()));

  // --- Sound settings popover (bass/treble/loudness/night mode) ---
  auto* sound_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  sound_box->set_margin_top(6);
  sound_box->set_margin_bottom(6);
  sound_box->set_margin_start(6);
  sound_box->set_margin_end(6);
  sound_box->set_size_request(220, -1);

  auto add_sound_label = [&](const char* text) {
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_halign(Gtk::Align::START);
    label->add_css_class("caption");
    label->add_css_class("dim-label");
    sound_box->append(*label);
  };

  add_sound_label("Bässe");
  bass_scale_.set_range(-10, 10);
  bass_scale_.set_digits(0);
  bass_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetBass(static_cast<int8_t>(bass_scale_.get_value()));
  });
  sound_box->append(bass_scale_);

  add_sound_label("Höhen");
  treble_scale_.set_range(-10, 10);
  treble_scale_.set_digits(0);
  treble_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetTreble(static_cast<int8_t>(treble_scale_.get_value()));
  });
  sound_box->append(treble_scale_);

  auto* reset_eq_button = Gtk::make_managed<Gtk::Button>("Bässe/Höhen zurücksetzen");
  reset_eq_button->add_css_class("flat");
  reset_eq_button->signal_clicked().connect([this] {
    suppress_sound_signals_ = true;
    bass_scale_.set_value(0);
    treble_scale_.set_value(0);
    suppress_sound_signals_ = false;
    backend_->SetBass(0);
    backend_->SetTreble(0);
  });
  sound_box->append(*reset_eq_button);

  sound_box->append(*Gtk::make_managed<Gtk::Separator>());

  auto* loudness_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* loudness_label = Gtk::make_managed<Gtk::Label>("Loudness");
  loudness_label->set_halign(Gtk::Align::START);
  loudness_label->set_hexpand(true);
  loudness_row->append(*loudness_label);
  loudness_switch_.set_valign(Gtk::Align::CENTER);
  // Deliberately not calling set_state() here — NosonBackend::SetLoudness()
  // always follows up with a refresh, whether the action succeeded or not,
  // which is what corrects the switch (see its comment).
  loudness_switch_.signal_state_set().connect(
      [this](bool state) -> bool {
        backend_->SetLoudness(state);
        return true;
      },
      false);
  loudness_row->append(loudness_switch_);
  sound_box->append(*loudness_row);

  auto* nightmode_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* nightmode_label = Gtk::make_managed<Gtk::Label>("Night Mode");
  nightmode_label->set_halign(Gtk::Align::START);
  nightmode_label->set_hexpand(true);
  nightmode_row->append(*nightmode_label);
  nightmode_switch_.set_valign(Gtk::Align::CENTER);
  nightmode_switch_.signal_state_set().connect(
      [this](bool state) -> bool {
        backend_->SetNightmode(state);
        return true;
      },
      false);
  nightmode_row->append(nightmode_switch_);
  sound_box->append(*nightmode_row);

  sound_box->append(*Gtk::make_managed<Gtk::Separator>());

  // Plain buttons, not a switch: libnoson has no GetLEDState() to show a
  // true current value with (unlike loudness/night mode above, which get
  // corrected via RefreshSoundSettingsAsync() after every use) — a switch
  // here would just be guessing at a state we don't actually know.
  auto* led_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* led_label = Gtk::make_managed<Gtk::Label>("Status-LED");
  led_label->set_halign(Gtk::Align::START);
  led_label->set_hexpand(true);
  led_row->append(*led_label);
  auto* led_on_button = Gtk::make_managed<Gtk::Button>("An");
  led_on_button->signal_clicked().connect([this] { backend_->SetLedState(true); });
  led_row->append(*led_on_button);
  auto* led_off_button = Gtk::make_managed<Gtk::Button>("Aus");
  led_off_button->signal_clicked().connect([this] { backend_->SetLedState(false); });
  led_row->append(*led_off_button);
  sound_box->append(*led_row);

  sound_popover_.set_child(*sound_box);

  sound_button_.set_icon_name("multimedia-volume-control-symbolic");
  sound_button_.set_tooltip_text("Klang");
  sound_button_.set_popover(sound_popover_);
  sound_popover_.signal_show().connect([this] { backend_->RefreshSoundSettingsAsync(); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(sound_button_.gobj()));

  set_titlebar(*Glib::wrap(header_bar_));

  // --- Sidebar: zone/room list ---
  zones_placeholder_.set_text("Keine Sonos-Geräte gefunden.\nKlicke auf Aktualisieren.");
  zones_placeholder_.set_wrap(true);
  zones_placeholder_.set_justify(Gtk::Justification::CENTER);
  zones_placeholder_.add_css_class("dim-label");
  zones_placeholder_.set_margin_top(24);
  zones_placeholder_.set_margin_bottom(24);
  zones_placeholder_.set_margin_start(12);
  zones_placeholder_.set_margin_end(12);
  zones_list_box_.set_placeholder(zones_placeholder_);
  zones_list_box_.set_selection_mode(Gtk::SelectionMode::SINGLE);
  zones_list_box_.add_css_class("navigation-sidebar");
  zones_list_box_.signal_row_selected().connect(sigc::mem_fun(*this, &GnomosWindow::OnZoneRowSelected));

  zones_scroller_.set_child(zones_list_box_);
  zones_scroller_.set_min_content_width(220);
  zones_scroller_.set_vexpand(true);

  // --- Content: queue/favorites/alarms/library tabs. The Now Playing
  // panel (player_bar_) is no longer docked here — see content_split_view_
  // below, which gives it its own wide side pane instead. ---
  view_stack_ = adw_view_stack_new();
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(queue_view_.gobj()), "queue",
                                       "Warteschlange", "view-list-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(favorites_view_.gobj()), "favorites",
                                       "Favoriten", "starred-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(alarms_view_.gobj()), "alarms",
                                       "Alarme", "alarm-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(history_view_.gobj()), "history",
                                       "Verlauf", "document-open-recent-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(library_view_.gobj()), "library",
                                       "Bibliothek", "folder-music-symbolic");

  GtkWidget* view_switcher = adw_view_switcher_new();
  adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(view_switcher), ADW_VIEW_STACK(view_stack_));
  adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(view_switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  Gtk::Widget* wrapped_switcher = Glib::wrap(view_switcher);
  wrapped_switcher->set_margin_top(6);
  wrapped_switcher->set_margin_bottom(6);
  wrapped_switcher->set_halign(Gtk::Align::CENTER);

  content_box_.append(*wrapped_switcher);
  content_box_.append(*Glib::wrap(view_stack_));

  favorites_view_.signal_item_activated().connect([this](unsigned index) { backend_->PlayFavorite(index); });
  favorites_view_.signal_add_to_queue_requested().connect([this](unsigned index) {
    backend_->AddFavoriteToQueue(index);
    ShowToast("Zur Warteschlange hinzugefügt");
  });
  favorites_view_.signal_play_next_requested().connect([this](unsigned index) {
    backend_->PlayFavoriteNext(index);
    ShowToast("Als Nächstes hinzugefügt");
  });
  favorites_view_.signal_delete_requested().connect(
      sigc::mem_fun(*this, &GnomosWindow::ShowDeleteFavoriteConfirmDialog));

  alarms_view_.signal_enabled_toggled().connect(
      [this](std::string id, bool enabled) { backend_->SetAlarmEnabled(id, enabled); });
  alarms_view_.signal_include_linked_zones_toggled().connect(
      [this](std::string id, bool include) { backend_->SetAlarmIncludeLinkedZones(id, include); });
  alarms_view_.signal_delete_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowDeleteAlarmConfirmDialog));
  alarms_view_.signal_add_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowAddAlarmDialog));
  alarms_view_.signal_edit_requested().connect(sigc::mem_fun(*this, &GnomosWindow::OnAlarmEditRequested));

  history_view_.signal_clear_requested().connect([this] {
    history_.clear();
    last_history_key_.clear();
    history_view_.SetItems(history_);
    SaveHistory();
  });

  library_stack_.push_back({"", "Bibliothek"});
  library_view_.SetLevelTitle("Bibliothek");
  library_view_.SetBackVisible(false);
  library_view_.signal_entry_activated().connect(sigc::mem_fun(*this, &GnomosWindow::OnLibraryEntryActivated));
  library_view_.signal_back_requested().connect(sigc::mem_fun(*this, &GnomosWindow::OnLibraryBackRequested));
  library_view_.signal_search_requested().connect([this] { ShowLibrarySearchDialog(); });
  library_view_.signal_add_to_queue_requested().connect([this](unsigned index) {
    backend_->AddLibraryItemToQueue(index);
    ShowToast("Zur Warteschlange hinzugefügt");
  });
  library_view_.signal_play_next_requested().connect([this](unsigned index) {
    backend_->PlayLibraryItemNext(index);
    ShowToast("Als Nächstes hinzugefügt");
  });
  library_view_.signal_play_all_requested().connect([this] { backend_->PlayAllLibraryItemsAsync(); });
  library_view_.signal_queue_all_requested().connect([this] {
    backend_->AddAllLibraryItemsToQueue();
    ShowToast("Zur Warteschlange hinzugefügt");
  });

  player_bar_.signal_play_pause().connect([this] {
    NowPlaying np = backend_->GetNowPlaying();
    if (np.valid && np.state == TransportState::Playing)
      backend_->PauseOrStop();
    else
      backend_->Play();
  });
  player_bar_.signal_next().connect([this] { backend_->Next(); });
  player_bar_.signal_shuffle_clicked().connect([this] { backend_->ToggleShuffle(); });
  player_bar_.signal_repeat_clicked().connect([this] { backend_->ToggleRepeat(); });
  player_bar_.signal_add_to_favorites_clicked().connect([this] { backend_->AddCurrentTrackToFavorites(); });
  player_bar_.signal_art_clicked().connect(sigc::mem_fun(*this, &GnomosWindow::ShowTrackInfoDialog));
  player_bar_.signal_previous().connect([this] { backend_->Previous(); });
  player_bar_.signal_volume_changed().connect(
      [this](double value) { backend_->SetVolume(static_cast<uint8_t>(value)); });
  player_bar_.signal_mute_toggled().connect([this](bool muted) { backend_->SetMuted(muted); });
  player_bar_.signal_seek_requested().connect([this](unsigned seconds) { backend_->SeekAsync(seconds); });

  queue_view_.signal_item_activated().connect([this](unsigned index) { backend_->PlayQueueItem(index); });
  queue_view_.signal_item_remove_requested().connect([this](unsigned index) { backend_->RemoveQueueItem(index); });
  queue_view_.signal_clear_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowClearQueueConfirmDialog));
  queue_view_.signal_save_playlist_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowSavePlaylistDialog));
  queue_view_.signal_reorder_requested().connect(
      [this](unsigned from, unsigned to) { backend_->ReorderQueueItem(from, to); });

  // --- Now Playing panel as a second AdwOverlaySplitView, not a plain
  // Gtk::Paned — lets it collapse behind now_playing_toggle_button_ on
  // narrow windows too (see the AdwBreakpoint below), the same treatment
  // split_view_ (the room sidebar) already gets. player_bar_ is the
  // "sidebar" here, pinned to the END so it keeps sitting on the right
  // like before; its min/max width mirror its own set_size_request(280, -1).
  content_split_view_ = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(content_split_view_), GTK_PACK_END);
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(content_split_view_), GTK_WIDGET(player_bar_.gobj()));
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(content_split_view_), GTK_WIDGET(content_box_.gobj()));
  adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(content_split_view_), 280);
  adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(content_split_view_), 360);
  // G_BINDING_SYNC_CREATE syncs from the *source* (the toggle button's own
  // "active") to the target on creation — so the button needs to start
  // active itself, or this binding would immediately force show-sidebar
  // back to false and hide the panel completely at normal window widths.
  // Confirmed live: without this, both side panels were simply missing at
  // startup, not just uncollapsed.
  now_playing_toggle_button_.set_active(true);
  g_object_bind_property(now_playing_toggle_button_.gobj(), "active", content_split_view_, "show-sidebar",
                          static_cast<GBindingFlags>(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
  g_object_bind_property(content_split_view_, "collapsed", now_playing_toggle_button_.gobj(), "visible",
                          G_BINDING_SYNC_CREATE);

  // --- Room sidebar as an AdwOverlaySplitView, not a plain Gtk::Paned —
  // lets it collapse behind sidebar_toggle_button_ on narrow windows (see
  // the AdwBreakpoint below), the adaptive behavior the README used to
  // list as a known gap. min/max width mirror the fixed 240px paned
  // position this replaced.
  split_view_ = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_view_), GTK_WIDGET(zones_scroller_.gobj()));
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_view_), content_split_view_);
  adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_view_), 220);
  adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_view_), 320);
  // sidebar_toggle_button_ only needs to exist (and be shown) once the
  // split view has actually collapsed the sidebar into an overlay —
  // above that width it's docked side-by-side and the button would be
  // redundant. "active" <-> "show-sidebar" is bidirectional so it also
  // stays in sync if the sidebar is dismissed via its own swipe/click-away
  // gesture rather than the button itself.
  //
  // G_BINDING_SYNC_CREATE syncs from the *source* (the button's own
  // "active") to the target on creation, so the button needs to start
  // active — see the identical comment on now_playing_toggle_button_ above
  // for why (this was a real bug: both side panels were missing at
  // startup until the window got narrow enough to collapse them).
  sidebar_toggle_button_.set_active(true);
  g_object_bind_property(sidebar_toggle_button_.gobj(), "active", split_view_, "show-sidebar",
                          static_cast<GBindingFlags>(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
  g_object_bind_property(split_view_, "collapsed", sidebar_toggle_button_.gobj(), "visible",
                          G_BINDING_SYNC_CREATE);

  // AdwBreakpoint needs an AdwBreakpointBin ancestor to evaluate against —
  // GnomosWindow is a plain Gtk::ApplicationWindow (see the class header
  // comment on why there's no Adw::ApplicationWindow binding), so that bin
  // is added explicitly here rather than coming from the window type
  // itself, wrapping split_view_ directly.
  GtkWidget* breakpoint_bin = adw_breakpoint_bin_new();
  // AdwBreakpointBin has no natural minimum size of its own (unlike a
  // regular container, which would size itself from its child) — without
  // this, GTK warns at every allocation and the window could in principle
  // shrink to 0x0. 360x400 is a sane practical floor for this UI.
  gtk_widget_set_size_request(breakpoint_bin, 360, 400);
  adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(breakpoint_bin), split_view_);

  // Two independent breakpoints on the same bin: below 900px the room
  // sidebar collapses; below 700px the Now Playing panel *also* collapses
  // (both conditions are true at once for anything under 700px, so both
  // setters apply together there) — a progressive collapse from three
  // panes down to one, rather than an all-or-nothing jump.
  AdwBreakpoint* sidebar_breakpoint =
      adw_breakpoint_new(adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 900, ADW_LENGTH_UNIT_PX));
  GValue collapsed_value = G_VALUE_INIT;
  g_value_init(&collapsed_value, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed_value, TRUE);
  adw_breakpoint_add_setter(sidebar_breakpoint, G_OBJECT(split_view_), "collapsed", &collapsed_value);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(breakpoint_bin), sidebar_breakpoint);

  AdwBreakpoint* now_playing_breakpoint =
      adw_breakpoint_new(adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 700, ADW_LENGTH_UNIT_PX));
  adw_breakpoint_add_setter(now_playing_breakpoint, G_OBJECT(content_split_view_), "collapsed", &collapsed_value);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(breakpoint_bin), now_playing_breakpoint);
  g_value_unset(&collapsed_value);

  LoadSplitFractions();

  // --- Toast overlay wraps everything, for error feedback ---
  toast_overlay_ = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay_), breakpoint_bin);
  set_child(*Glib::wrap(toast_overlay_));

  // --- Backend wiring ---
  backend_ = std::make_unique<NosonBackend>();
  backend_->signal_discovery_done().connect(sigc::mem_fun(*this, &GnomosWindow::OnDiscoveryDone));
  backend_->signal_zones_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnZonesChanged));
  backend_->signal_player_ready().connect(sigc::mem_fun(*this, &GnomosWindow::OnPlayerReady));
  backend_->signal_now_playing_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnNowPlayingChanged));
  backend_->signal_position_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnPositionChanged));
  backend_->signal_volume_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnVolumeChanged));
  backend_->signal_queue_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnQueueChanged));
  backend_->signal_favorites_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnFavoritesChanged));
  backend_->signal_alarms_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnAlarmsChanged));
  backend_->signal_library_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnLibraryChanged));
  backend_->signal_service_link_ready().connect(sigc::mem_fun(*this, &GnomosWindow::OnServiceLinkReady));
  backend_->signal_sleep_timer_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnSleepTimerChanged));
  backend_->signal_sound_settings_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnSoundSettingsChanged));
  backend_->signal_error().connect(sigc::mem_fun(*this, &GnomosWindow::OnBackendError));

  mpris_ = std::make_unique<MprisService>(*backend_, *this);

  pending_restore_room_uuid_ = LoadLastRoomUuid();

  LoadHistory();
  history_view_.SetItems(history_);

  LoadColorScheme();
  LoadNotificationSetting();

  // Space = play/pause — see OnKeyPressed()'s header comment for why a
  // focused-Editable check is needed alongside the keyval check.
  key_controller_ = Gtk::EventControllerKey::create();
  key_controller_->signal_key_pressed().connect(sigc::mem_fun(*this, &GnomosWindow::OnKeyPressed), false);
  add_controller(key_controller_);

  OnRefreshClicked();
  backend_->BrowseLibraryAsync("");  // root categories are static/local, no need to wait for discovery

  position_timer_connection_ =
      Glib::signal_timeout().connect(sigc::mem_fun(*this, &GnomosWindow::OnPositionTimerTick), 1000);
}

void GnomosWindow::OnRefreshClicked()
{
  discovery_spinner_.start();
  backend_->DiscoverAsync();
}

void GnomosWindow::OnZoneRowSelected(Gtk::ListBoxRow* row)
{
  if (!row)
    return;
  int index = row->get_index();
  if (index < 0 || static_cast<size_t>(index) >= current_zones_.size())
    return;
  const ZoneInfo& zone = current_zones_[static_cast<size_t>(index)];
  selected_group_id_ = zone.group_id;
  backend_->SelectZone(selected_group_id_);
  SaveLastRoom(zone.coordinator_uuid);
}

namespace
{
std::string StateFilePath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "state.ini");
}
}  // namespace

void GnomosWindow::SaveLastRoom(const std::string& uuid) const
{
  if (uuid.empty())
    return;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("window", "last_room_uuid", uuid);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the room won't be remembered next launch
  }
}

std::string GnomosWindow::LoadLastRoomUuid() const
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return "";
    return keyfile->get_string("window", "last_room_uuid").raw();
  }
  catch (const Glib::Error&)
  {
    return "";
  }
}

void GnomosWindow::LoadWindowState()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    int width = keyfile->get_integer("window", "width");
    int height = keyfile->get_integer("window", "height");
    if (width > 0 && height > 0)
      set_default_size(width, height);
    if (keyfile->get_boolean("window", "maximized"))
      maximize();
  }
  catch (const Glib::Error&)
  {
    // fine — no saved size yet, the fixed default from the constructor applies
  }
}

void GnomosWindow::LoadSplitFractions()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    double sidebar_fraction = keyfile->get_double("window", "sidebar_fraction");
    if (sidebar_fraction > 0.0 && sidebar_fraction < 1.0)
      adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(split_view_), sidebar_fraction);
    double now_playing_fraction = keyfile->get_double("window", "now_playing_fraction");
    if (now_playing_fraction > 0.0 && now_playing_fraction < 1.0)
      adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(content_split_view_),
                                                          now_playing_fraction);
  }
  catch (const Glib::Error&)
  {
    // fine — no saved fraction yet, min/max_sidebar_width's own defaults apply
  }
}

bool GnomosWindow::OnCloseRequest()
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  bool maximized = is_maximized();
  keyfile->set_boolean("window", "maximized", maximized);
  if (!maximized)
  {
    int width = 0, height = 0;
    get_default_size(width, height);
    keyfile->set_integer("window", "width", width);
    keyfile->set_integer("window", "height", height);
  }
  // Saved even while collapsed — AdwOverlaySplitView keeps tracking a
  // sidebar-width-fraction internally either way, it just isn't visible
  // until show-sidebar is true again.
  keyfile->set_double("window", "sidebar_fraction",
                       adw_overlay_split_view_get_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(split_view_)));
  keyfile->set_double(
      "window", "now_playing_fraction",
      adw_overlay_split_view_get_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(content_split_view_)));
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the window size won't be remembered next launch
  }
  return false;  // don't block the close
}

void GnomosWindow::OnDiscoveryDone(bool ok)
{
  discovery_spinner_.stop();
  if (!ok)
    ShowToast("Kein Sonos-Gerät im Netzwerk gefunden.");
}

void GnomosWindow::OnZonesChanged()
{
  current_zones_ = backend_->Zones();

  while (Gtk::Widget* child = zones_list_box_.get_first_child())
    zones_list_box_.remove(*child);

  int select_index = -1;
  for (size_t i = 0; i < current_zones_.size(); ++i)
  {
    const ZoneInfo& zone = current_zones_[i];

    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(8);
    row_box->set_margin_bottom(8);
    row_box->set_margin_start(8);
    row_box->set_margin_end(8);

    // Icon-prefixed row, matching Euphonica's nav-list style
    // (https://github.com/htkhiem/euphonica) — its sidebar entries (Albums,
    // Artists, ...) are icon+label the same way.
    auto* room_icon = Gtk::make_managed<Gtk::Image>();
    room_icon->set_from_icon_name("audio-speakers-symbolic");
    room_icon->add_css_class("dim-label");
    row_box->append(*room_icon);

    auto* name_label = Gtk::make_managed<Gtk::Label>(zone.display_name);
    name_label->set_halign(Gtk::Align::START);
    name_label->set_hexpand(true);
    name_label->set_ellipsize(Pango::EllipsizeMode::END);
    row_box->append(*name_label);

    if (zone.is_gen1)
    {
      auto* badge = Gtk::make_managed<Gtk::Label>("Gen 1");
      badge->add_css_class("dim-label");
      badge->add_css_class("caption");
      row_box->append(*badge);
    }

    zones_list_box_.append(*row_box);

    if (zone.group_id == selected_group_id_)
      select_index = static_cast<int>(i);
  }

  if (!current_zones_.empty() && select_index < 0 && !pending_restore_room_uuid_.empty())
  {
    for (size_t i = 0; i < current_zones_.size(); ++i)
    {
      if (current_zones_[i].coordinator_uuid == pending_restore_room_uuid_)
      {
        select_index = static_cast<int>(i);
        break;
      }
    }
    // Only ever tried once we actually had a zone list to search: if the
    // remembered room isn't in it (renamed, offline), silently fall
    // through to the index-0 default below rather than re-attempting on
    // every future refresh, which could yank a later manual room switch
    // back to this one. An empty zone list (discovery still pending or
    // failed) leaves this untouched so a later successful discovery still
    // gets to try.
    pending_restore_room_uuid_.clear();
  }

  if (select_index < 0 && !current_zones_.empty())
    select_index = 0;

  if (select_index >= 0)
  {
    if (Gtk::ListBoxRow* row = zones_list_box_.get_row_at_index(select_index))
      zones_list_box_.select_row(*row);
  }

  // A topology change (e.g. our own join/remove action taking effect) may
  // have happened while the grouping popover was open; keep it truthful.
  if (grouping_popover_.get_visible())
    RebuildGroupingPopover();
}

void GnomosWindow::OnPlayerReady()
{
  player_bar_.SetEnabled(true);
  backend_->RefreshQueueAsync();
}

void GnomosWindow::OnNowPlayingChanged()
{
  NowPlaying np = backend_->GetNowPlaying();
  player_bar_.Update(np);
  current_queue_index_ = (np.valid && np.playing_from_queue) ? static_cast<int>(np.current_queue_index) : -1;
  queue_view_.SetCurrentIndex(current_queue_index_);
  UpdateNextTrackHint();
  RecordHistoryIfTrackChanged(np);
  CheckAlarmAndTransportStatus(np);
}

void GnomosWindow::OnPositionChanged()
{
  player_bar_.UpdatePosition(backend_->GetPosition(), backend_->GetNowPlaying().duration);
}

bool GnomosWindow::OnPositionTimerTick()
{
  NowPlaying np = backend_->GetNowPlaying();
  if (np.valid && np.state == TransportState::Playing && np.duration > 0)
    backend_->RefreshPositionAsync();
  return true;  // keep the timer running
}

void GnomosWindow::OnVolumeChanged()
{
  player_bar_.UpdateVolume(backend_->GetVolume());
}

void GnomosWindow::OnQueueChanged()
{
  queue_view_.SetItems(backend_->GetQueue());
  queue_view_.SetCurrentIndex(current_queue_index_);
  UpdateNextTrackHint();
}

void GnomosWindow::UpdateNextTrackHint()
{
  std::vector<QueueItem> queue = backend_->GetQueue();
  if (current_queue_index_ >= 0 && static_cast<size_t>(current_queue_index_) + 1 < queue.size())
  {
    const QueueItem& next = queue[static_cast<size_t>(current_queue_index_) + 1];
    player_bar_.UpdateNextTrack(next.title.empty() ? "Unbekannter Titel" : next.title);
  }
  else
  {
    player_bar_.UpdateNextTrack("");
  }
}

namespace
{
std::string HistoryFilePath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "history.ini");
}
constexpr size_t kMaxHistoryEntries = 50;
}  // namespace

void GnomosWindow::RecordHistoryIfTrackChanged(const NowPlaying& now_playing)
{
  if (!now_playing.valid || now_playing.title.empty())
    return;

  std::string key = now_playing.title + "\x1f" + now_playing.artist;
  if (key == last_history_key_)
    return;  // same track as last time — OnNowPlayingChanged() re-fired without a real change
  last_history_key_ = key;

  HistoryEntry entry;
  entry.title = now_playing.title;
  entry.artist = now_playing.artist;
  entry.album = now_playing.album;
  entry.art_uri = now_playing.art_uri;
  history_.insert(history_.begin(), entry);
  if (history_.size() > kMaxHistoryEntries)
    history_.resize(kMaxHistoryEntries);

  history_view_.SetItems(history_);
  SaveHistory();
  SendTrackChangeNotification(now_playing);
}

void GnomosWindow::CheckAlarmAndTransportStatus(const NowPlaying& now_playing)
{
  if (now_playing.alarm_running && !last_alarm_running_)
  {
    AdwToast* toast = adw_toast_new("Wecker klingelt");
    adw_toast_set_button_label(toast, "Stoppen");
    adw_toast_set_action_name(toast, "win.stop-alarm");
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toast_overlay_), toast);
  }
  last_alarm_running_ = now_playing.alarm_running;

  if (now_playing.valid && !now_playing.transport_status_ok && last_transport_status_ok_)
    ShowToast("Gerät meldet einen Wiedergabefehler.");
  last_transport_status_ok_ = now_playing.transport_status_ok;
}

void GnomosWindow::LoadHistory()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(HistoryFilePath()))
      return;
    std::vector<Glib::ustring> titles = keyfile->get_string_list("history", "titles");
    std::vector<Glib::ustring> artists = keyfile->get_string_list("history", "artists");
    std::vector<Glib::ustring> albums = keyfile->get_string_list("history", "albums");
    std::vector<Glib::ustring> art_uris = keyfile->get_string_list("history", "art_uris");
    // Parallel arrays, all written together by SaveHistory() — a size
    // mismatch means a corrupt/foreign file, safer to ignore than guess.
    if (titles.size() != artists.size() || titles.size() != albums.size() || titles.size() != art_uris.size())
      return;
    for (size_t i = 0; i < titles.size(); ++i)
    {
      HistoryEntry entry;
      entry.title = titles[i].raw();
      entry.artist = artists[i].raw();
      entry.album = albums[i].raw();
      entry.art_uri = art_uris[i].raw();
      history_.push_back(entry);
    }
    if (!history_.empty())
      last_history_key_ = history_.front().title + "\x1f" + history_.front().artist;
  }
  catch (const Glib::Error&)
  {
    history_.clear();
  }
}

void GnomosWindow::SaveHistory() const
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  std::vector<Glib::ustring> titles, artists, albums, art_uris;
  for (const HistoryEntry& entry : history_)
  {
    titles.push_back(entry.title);
    artists.push_back(entry.artist);
    albums.push_back(entry.album);
    art_uris.push_back(entry.art_uri);
  }

  auto keyfile = Glib::KeyFile::create();
  keyfile->set_string_list("history", "titles", titles);
  keyfile->set_string_list("history", "artists", artists);
  keyfile->set_string_list("history", "albums", albums);
  keyfile->set_string_list("history", "art_uris", art_uris);
  try
  {
    keyfile->save_to_file(HistoryFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the history won't be remembered next launch
  }
}

void GnomosWindow::LoadColorScheme()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    ApplyColorScheme(keyfile->get_string("appearance", "color_scheme").raw());
  }
  catch (const Glib::Error&)
  {
    // fine — no override saved yet, AdwStyleManager's own default applies
  }
}

void GnomosWindow::ApplyColorScheme(const std::string& scheme)
{
  AdwStyleManager* style_manager = adw_style_manager_get_default();
  if (scheme == "light")
    adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
  else if (scheme == "dark")
    adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
  else
    adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_DEFAULT);

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("appearance", "color_scheme", scheme);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the override won't be remembered next launch
  }
}

void GnomosWindow::LoadNotificationSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    notify_on_track_change_ = keyfile->get_boolean("notifications", "track_change");
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays off (the default)
  }
}

void GnomosWindow::SetNotifyOnTrackChange(bool enabled)
{
  notify_on_track_change_ = enabled;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("notifications", "track_change", enabled);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
}

void GnomosWindow::SendTrackChangeNotification(const NowPlaying& now_playing)
{
  if (!notify_on_track_change_)
    return;

  auto notification = Gio::Notification::create(now_playing.title.empty() ? "Unbekannter Titel" : now_playing.title);
  std::string body = now_playing.artist;
  if (!now_playing.album.empty())
    body += (body.empty() ? "" : " — ") + now_playing.album;
  if (!body.empty())
    notification->set_body(body);

  // Only ever the already-cached texture (memory or disk) — never a fresh
  // network fetch here, so a notification is never held up waiting on one.
  // Gdk::Texture doesn't implement Gio::Icon in this gtkmm version (see its
  // own header), even though the underlying GdkTexture does at the C/GObject
  // level, so the icon is set through the raw API instead.
  if (!now_playing.art_uri.empty())
  {
    if (auto texture = ArtCache::Instance().Get(now_playing.art_uri))
      g_notification_set_icon(notification->gobj(), G_ICON(texture->gobj()));
  }

  if (auto app = get_application())
    app->send_notification("now-playing", notification);
}

bool GnomosWindow::OnKeyPressed(guint keyval, guint /*keycode*/, Gdk::ModifierType state)
{
  bool ctrl = (state & Gdk::ModifierType::CONTROL_MASK) == Gdk::ModifierType::CONTROL_MASK;

  // Ctrl+, (Preferences) is a GNOME-wide convention that works everywhere,
  // including while a text field has focus — same as every native GNOME
  // app's own accelerator for it, so this one is checked before the
  // Editable exclusion below.
  if (ctrl && keyval == GDK_KEY_comma)
  {
    ShowSettingsDialog();
    return true;
  }

  // Don't hijack the rest of these while the user is typing into a search
  // box, the library search dialog's entry, a spin button, etc. —
  // Gtk::Editable is the interface every text-entry-like widget
  // implements, so this covers all of them without listing each widget
  // type.
  Gtk::Widget* focus = get_focus();
  if (dynamic_cast<Gtk::Editable*>(focus) != nullptr)
    return false;

  if (ctrl && keyval == GDK_KEY_f)
  {
    ShowLibrarySearchDialog();
    return true;
  }

  switch (keyval)
  {
    case GDK_KEY_space:
    {
      NowPlaying np = backend_->GetNowPlaying();
      if (np.valid && np.state == TransportState::Playing)
        backend_->PauseOrStop();
      else
        backend_->Play();
      return true;
    }
    case GDK_KEY_n: backend_->Next(); return true;
    case GDK_KEY_p: backend_->Previous(); return true;
    case GDK_KEY_Up:
    {
      VolumeInfo volume = backend_->GetVolume();
      backend_->SetVolume(static_cast<uint8_t>(std::min(100, static_cast<int>(volume.volume) + 5)));
      return true;
    }
    case GDK_KEY_Down:
    {
      VolumeInfo volume = backend_->GetVolume();
      backend_->SetVolume(static_cast<uint8_t>(std::max(0, static_cast<int>(volume.volume) - 5)));
      return true;
    }
    case GDK_KEY_m:
    {
      VolumeInfo volume = backend_->GetVolume();
      backend_->SetMuted(!volume.muted);
      return true;
    }
    case GDK_KEY_s: backend_->ToggleShuffle(); return true;
    case GDK_KEY_r: backend_->ToggleRepeat(); return true;
    default: return false;
  }
}

void GnomosWindow::OnFavoritesChanged()
{
  favorites_view_.SetItems(backend_->GetFavorites());
}

void GnomosWindow::OnAlarmsChanged()
{
  alarms_view_.SetItems(backend_->GetAlarms());
}

void GnomosWindow::OnAlarmEditRequested(std::string alarm_id)
{
  for (const AlarmInfo& alarm : backend_->GetAlarms())
  {
    if (alarm.id == alarm_id)
    {
      ShowAlarmDialog(&alarm);
      return;
    }
  }
}

void GnomosWindow::OnLibraryChanged()
{
  current_library_entries_ = backend_->GetLibraryEntries();

  // Grid view (cover-art tiles, like Euphonica's own Albums/Artists grid —
  // https://github.com/htkhiem/euphonica). Two different signals depending
  // on where we are, since they're two fairly different systems:
  //
  // - Inside a third-party service (Spotify, bonob, ...): SMAPIItem itself
  //   carries a displayType (GRID/LIST/HERO/EDITORIAL) straight from the
  //   service's own SMAPI response — trust that directly rather than
  //   guessing. "Inside a service" is checked via library_stack_[1] (the
  //   breadcrumb entry right after the true root), which is always that
  //   service's own root object_id (kServiceRootPrefix-prefixed) for as
  //   long as we're anywhere under it — NOT the current level's own
  //   object_id, which only carries that prefix at the service's own root
  //   and is the service's internal (unprefixed) id at any deeper level.
  //   Deliberately not backend_->GetActiveServiceSearchCategories().empty():
  //   that's empty for services with no search support (bonob, confirmed
  //   live) even while genuinely inside one, so it can't tell "inside a
  //   service" apart from "in the local library" here.
  // - Local library: no such hint exists, so this falls back to an
  //   object_id-prefix heuristic instead — both "A:ALBUM" (the root album
  //   list) and "A:ALBUMARTIST" (the root artist list, and an artist's own
  //   album list once you browse into one) use this prefix. Gated on the
  //   entries actually being containers too, since browsing all the way
  //   down into a real album swaps the entries for that album's individual
  //   tracks — a grid of identical repeated cover art for every track
  //   would be worse than the plain list those get instead.
  bool in_service = library_stack_.size() > 1 &&
                     library_stack_[1].first.compare(0, std::string(kServiceRootPrefix).size(), kServiceRootPrefix) ==
                         0;
  bool grid;
  if (in_service)
  {
    grid = std::any_of(current_library_entries_.begin(), current_library_entries_.end(),
                        [](const LibraryEntry& entry) { return entry.display_as_grid; });
  }
  else
  {
    const std::string& object_id = library_stack_.back().first;
    grid = object_id.compare(0, 7, "A:ALBUM") == 0 &&
           std::any_of(current_library_entries_.begin(), current_library_entries_.end(),
                       [](const LibraryEntry& entry) { return entry.is_container; });
  }

  library_view_.SetEntries(current_library_entries_, grid);
  library_view_.SetLevelTitle(library_stack_.back().second);
  library_view_.SetBackVisible(library_stack_.size() > 1);
}

void GnomosWindow::OnLibraryEntryActivated(unsigned index)
{
  if (index >= current_library_entries_.size())
    return;
  const LibraryEntry& entry = current_library_entries_[index];
  if (entry.object_id == kLinkServiceSentinel)
  {
    ShowLinkServiceDialog();
  }
  else if (entry.is_container)
  {
    library_stack_.push_back({entry.object_id, entry.title.empty() ? "—" : entry.title});
    backend_->BrowseLibraryAsync(entry.object_id);
  }
  else
  {
    backend_->PlayLibraryItem(index);
  }
}

void GnomosWindow::OnLibraryBackRequested()
{
  if (library_stack_.size() <= 1)
    return;
  library_stack_.pop_back();
  backend_->BrowseLibraryAsync(library_stack_.back().first);
}

void GnomosWindow::RebuildGroupingPopover()
{
  while (Gtk::Widget* child = grouping_list_box_.get_first_child())
    grouping_list_box_.remove(*child);

  if (selected_group_id_.empty())
    return;

  for (const RoomInfo& room : backend_->Rooms())
  {
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    row_box->set_margin_top(6);
    row_box->set_margin_bottom(6);
    row_box->set_margin_start(6);
    row_box->set_margin_end(6);

    auto* top_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    if (!room.model_number.empty())
      top_row->set_tooltip_text(room.model_number);
    row_box->append(*top_row);

    auto* name_label = Gtk::make_managed<Gtk::Label>(room.name);
    name_label->set_halign(Gtk::Align::START);
    name_label->set_hexpand(true);
    top_row->append(*name_label);

    if (room.is_gen1)
    {
      auto* badge = Gtk::make_managed<Gtk::Label>("Gen 1");
      badge->add_css_class("dim-label");
      badge->add_css_class("caption");
      top_row->append(*badge);
    }

    auto* room_switch = Gtk::make_managed<Gtk::Switch>();
    room_switch->set_valign(Gtk::Align::CENTER);
    room_switch->set_active(room.group_id == selected_group_id_);

    // This room *is* the currently selected zone's own coordinator —
    // always "in the group" trivially, not something to toggle from here.
    const bool is_self = (room.player_uuid == room.coordinator_uuid && room.group_id == selected_group_id_);
    room_switch->set_sensitive(!is_self);

    // signal_state_set() (unlike notify::active) only fires for user
    // interaction, never for the set_active() call above. Deliberately not
    // calling set_state() here: the actual join/remove action is async and
    // can fail (see OnBackendError), so the switch's confirmed state is
    // left alone until the real topology change (or lack thereof) comes
    // back around through signal_zones_changed() and rebuilds this popover
    // with the true state.
    std::string uuid = room.player_uuid;
    room_switch->signal_state_set().connect(
        [this, uuid](bool state) -> bool {
          if (state)
            backend_->JoinRoomToCurrentZone(uuid);
          else
            backend_->RemoveRoomFromGroup(uuid);
          return true;
        },
        false);

    top_row->append(*room_switch);

    // Per-room volume within the group — only meaningful (and only known,
    // see GetRoomVolume()'s comment) for a room that's actually a member
    // of the currently selected zone right now.
    uint8_t room_volume = 0;
    if (room.group_id == selected_group_id_ && backend_->GetRoomVolume(room.player_uuid, room_volume))
    {
      auto* volume_scale = Gtk::make_managed<Gtk::Scale>();
      volume_scale->set_range(0, 100);
      volume_scale->set_value(room_volume);
      volume_scale->set_draw_value(false);
      volume_scale->signal_value_changed().connect([this, volume_scale, uuid] {
        backend_->SetRoomVolume(uuid, static_cast<uint8_t>(volume_scale->get_value()));
      });
      row_box->append(*volume_scale);
    }

    grouping_list_box_.append(*row_box);
  }
}

namespace
{
// Recurrence strings are comma-separated 3-letter day abbreviations (see
// NSROOT::DayTable in alarm.h) — substring search is safe here since none
// of the seven tokens is a substring of another.
std::vector<int> ParseRecurrenceDays(const std::string& recurrence)
{
  static const std::array<std::pair<const char*, int>, 7> kDayTokens = {{
      {"SUN", 0},
      {"MON", 1},
      {"TUE", 2},
      {"WED", 3},
      {"THU", 4},
      {"FRI", 5},
      {"SAT", 6},
  }};
  std::vector<int> days;
  for (const auto& [token, value] : kDayTokens)
    if (recurrence.find(token) != std::string::npos)
      days.push_back(value);
  return days;
}
}  // namespace

void GnomosWindow::ShowAddAlarmDialog()
{
  ShowAlarmDialog(nullptr);
}

// existing == nullptr creates a new alarm; otherwise edits it in place,
// prefilled from its current schedule (enabled state and sound source are
// left untouched either way — see NosonBackend::UpdateAlarmSchedule()).
void GnomosWindow::ShowAlarmDialog(const AlarmInfo* existing)
{
  std::vector<RoomInfo> rooms = backend_->Rooms();
  if (rooms.empty())
  {
    ShowToast("Kein Raum verfügbar.");
    return;
  }

  auto* dialog = new Gtk::Window();
  dialog->set_title(existing ? "Alarm bearbeiten" : "Neuer Alarm");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto add_section_label = [&](const char* text) {
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_halign(Gtk::Align::START);
    content->append(*label);
  };

  add_section_label("Raum");
  std::vector<Glib::ustring> room_names;
  room_names.reserve(rooms.size());
  guint preselected_room = 0;
  for (size_t i = 0; i < rooms.size(); ++i)
  {
    room_names.push_back(rooms[i].name);
    if (existing && rooms[i].player_uuid == existing->room_uuid)
      preselected_room = static_cast<guint>(i);
  }
  auto room_model = Gtk::StringList::create(room_names);
  auto* room_dropdown = Gtk::make_managed<Gtk::DropDown>(room_model);
  room_dropdown->set_selected(preselected_room);
  content->append(*room_dropdown);

  add_section_label("Uhrzeit");
  auto* time_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* hour_spin = Gtk::make_managed<Gtk::SpinButton>();
  hour_spin->set_range(0, 23);
  hour_spin->set_increments(1, 1);
  hour_spin->set_wrap(true);
  auto* minute_spin = Gtk::make_managed<Gtk::SpinButton>();
  minute_spin->set_range(0, 59);
  minute_spin->set_increments(5, 5);
  minute_spin->set_wrap(true);
  int existing_hour = 7, existing_minute = 0;
  if (existing)
    std::sscanf(existing->start_time.c_str(), "%d:%d", &existing_hour, &existing_minute);
  hour_spin->set_value(existing_hour);
  minute_spin->set_value(existing_minute);
  time_box->append(*hour_spin);
  time_box->append(*Gtk::make_managed<Gtk::Label>(":"));
  time_box->append(*minute_spin);
  content->append(*time_box);

  add_section_label("Wiederholung");
  auto* days_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  days_box->add_css_class("linked");
  // label, NSROOT::Day_t value, default-checked (only used for a new
  // alarm) — Monday-first display order (common convention), Sonos day
  // values (0=Sunday..6=Saturday).
  static const std::array<std::tuple<const char*, int, bool>, 7> kDays = {{
      {"Mo", 1, true},
      {"Di", 2, true},
      {"Mi", 3, true},
      {"Do", 4, true},
      {"Fr", 5, true},
      {"Sa", 6, false},
      {"So", 0, false},
  }};
  std::vector<int> existing_days = existing ? ParseRecurrenceDays(existing->recurrence) : std::vector<int>();
  auto day_buttons = std::make_shared<std::vector<std::pair<Gtk::ToggleButton*, int>>>();
  for (const auto& [label, value, default_on] : kDays)
  {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>(label);
    bool active = existing ? (std::find(existing_days.begin(), existing_days.end(), value) != existing_days.end())
                            : default_on;
    btn->set_active(active);
    days_box->append(*btn);
    day_buttons->push_back({btn, value});
  }
  content->append(*days_box);

  add_section_label("Lautstärke");
  auto* volume_scale = Gtk::make_managed<Gtk::Scale>();
  volume_scale->set_range(0, 100);
  volume_scale->set_value(existing ? existing->volume : 30);
  content->append(*volume_scale);

  add_section_label("Dauer");
  static const std::array<std::pair<const char*, unsigned>, 5> kDurations = {{
      {"15 Minuten", 15},
      {"30 Minuten", 30},
      {"1 Stunde", 60},
      {"2 Stunden", 120},
      {"3 Stunden", 180},
  }};
  std::vector<Glib::ustring> duration_labels;
  duration_labels.reserve(kDurations.size());
  guint preselected_duration = 3;  // "2 Stunden" — CreateAlarm()'s own existing default
  for (size_t i = 0; i < kDurations.size(); ++i)
  {
    duration_labels.push_back(kDurations[i].first);
    if (existing && existing->duration_minutes == kDurations[i].second)
      preselected_duration = static_cast<guint>(i);
  }
  auto duration_model = Gtk::StringList::create(duration_labels);
  auto* duration_dropdown = Gtk::make_managed<Gtk::DropDown>(duration_model);
  duration_dropdown->set_selected(preselected_duration);
  content->append(*duration_dropdown);

  auto* shuffle_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* shuffle_label = Gtk::make_managed<Gtk::Label>("Zufallswiedergabe beim Wecken");
  shuffle_label->set_halign(Gtk::Align::START);
  shuffle_label->set_hexpand(true);
  shuffle_row->append(*shuffle_label);
  auto* shuffle_switch = Gtk::make_managed<Gtk::Switch>();
  shuffle_switch->set_valign(Gtk::Align::CENTER);
  shuffle_switch->set_active(existing && existing->shuffle);
  shuffle_row->append(*shuffle_switch);
  content->append(*shuffle_row);

  add_section_label("Klang");
  // Index 0 in sound_titles is always "Wecker-Ton" (the buzzer); when
  // editing, an extra "Aktueller Klang beibehalten" entry is prepended so
  // that editing time/room/etc. can't silently reset a custom alarm sound
  // back to the buzzer just because the user didn't touch this dropdown —
  // see NosonBackend::kKeepExistingAlarmSound.
  std::vector<std::string> sound_titles = backend_->GetAlarmSoundTitles();
  std::vector<Glib::ustring> sound_entries;
  if (existing)
    sound_entries.push_back("Aktueller Klang beibehalten");
  for (const std::string& title : sound_titles)
    sound_entries.push_back(title);
  auto sound_model = Gtk::StringList::create(sound_entries);
  auto* sound_dropdown = Gtk::make_managed<Gtk::DropDown>(sound_model);
  sound_dropdown->set_enable_search(true);
  sound_dropdown->set_selected(0);
  content->append(*sound_dropdown);

  bool has_keep_current_for_test = existing != nullptr;
  auto* test_sound_button = Gtk::make_managed<Gtk::Button>("Wecker-Ton testen");
  test_sound_button->add_css_class("flat");
  test_sound_button->set_halign(Gtk::Align::START);
  test_sound_button->signal_clicked().connect([this, room_dropdown, sound_dropdown, rooms, has_keep_current_for_test] {
    guint selected_room = room_dropdown->get_selected();
    if (selected_room >= rooms.size())
      return;
    guint sound_selection = sound_dropdown->get_selected();
    // Exactly mirrors confirm_button's own sound_index resolution below —
    // "Wecker-Ton" (the buzzer, sound_index 0) and "Aktueller Klang
    // beibehalten" (only present when editing, kKeepExistingAlarmSound)
    // can't be previewed this way (see PreviewAlarmSound()'s comment), but
    // PreviewAlarmSound() already no-ops for both, so no special-casing is
    // needed here beyond getting the same index confirm_button would use.
    unsigned sound_index;
    if (has_keep_current_for_test && sound_selection == 0)
      sound_index = kKeepExistingAlarmSound;
    else
      sound_index = has_keep_current_for_test ? sound_selection - 1 : sound_selection;
    backend_->PreviewAlarmSound(rooms[selected_room].player_uuid, sound_index);
  });
  content->append(*test_sound_button);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* confirm_button = Gtk::make_managed<Gtk::Button>(existing ? "Speichern" : "Erstellen");
  confirm_button->add_css_class("suggested-action");
  std::string alarm_id = existing ? existing->id : std::string();
  bool has_keep_current = existing != nullptr;
  confirm_button->signal_clicked().connect(
      [this, dialog, room_dropdown, hour_spin, minute_spin, volume_scale, day_buttons, rooms, alarm_id,
       sound_dropdown, has_keep_current, duration_dropdown, shuffle_switch] {
        guint selected = room_dropdown->get_selected();
        if (selected >= rooms.size())
        {
          dialog->close();
          return;
        }
        std::vector<int> days;
        for (const auto& [btn, value] : *day_buttons)
          if (btn->get_active())
            days.push_back(value);
        int hour = static_cast<int>(hour_spin->get_value());
        int minute = static_cast<int>(minute_spin->get_value());
        auto volume = static_cast<uint8_t>(volume_scale->get_value());
        guint sound_selection = sound_dropdown->get_selected();
        unsigned sound_index;
        if (has_keep_current && sound_selection == 0)
          sound_index = kKeepExistingAlarmSound;
        else
          sound_index = has_keep_current ? sound_selection - 1 : sound_selection;
        guint duration_selection = duration_dropdown->get_selected();
        unsigned duration_minutes =
            duration_selection < kDurations.size() ? kDurations[duration_selection].second : 120;
        bool shuffle = shuffle_switch->get_active();
        if (alarm_id.empty())
          backend_->CreateAlarm(rooms[selected].player_uuid, hour, minute, days, volume, sound_index,
                                 duration_minutes, shuffle);
        else
          backend_->UpdateAlarmSchedule(alarm_id, rooms[selected].player_uuid, hour, minute, days, volume,
                                         sound_index, duration_minutes, shuffle);
        dialog->close();
      });
  button_box->append(*cancel_button);
  button_box->append(*confirm_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
}

void GnomosWindow::OnSleepTimerChanged()
{
  SleepTimerInfo info = backend_->GetSleepTimerInfo();
  sleep_timer_status_label_.set_text(info.active ? "Aktiv, verbleibend: " + info.remaining : "Kein Sleep-Timer aktiv");
}

void GnomosWindow::OnSoundSettingsChanged()
{
  SoundSettings settings = backend_->GetSoundSettings();

  suppress_sound_signals_ = true;
  bass_scale_.set_value(settings.bass);
  treble_scale_.set_value(settings.treble);
  suppress_sound_signals_ = false;

  // set_active()/set_state() called programmatically don't trigger
  // signal_state_set() (that only fires for user interaction — see the
  // grouping popover's switches for the same reasoning), so no suppress
  // flag is needed here the way the sliders above need one.
  loudness_switch_.set_active(settings.loudness);
  loudness_switch_.set_state(settings.loudness);

  nightmode_switch_.set_sensitive(settings.nightmode_supported);
  nightmode_switch_.set_active(settings.nightmode);
  nightmode_switch_.set_state(settings.nightmode);
}

void GnomosWindow::OnBackendError(std::string message)
{
  ShowToast(message);
  // A failed join/remove leaves no topology change to correct the switch
  // that optimistically flipped on click, so undo that here too.
  if (grouping_popover_.get_visible())
    RebuildGroupingPopover();
}

void GnomosWindow::ShowToast(const std::string& message)
{
  AdwToast* toast = adw_toast_new(message.c_str());
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toast_overlay_), toast);
}

void GnomosWindow::ShowAboutDialog()
{
  AdwDialog* dialog = adw_about_dialog_new();
  AdwAboutDialog* about = ADW_ABOUT_DIALOG(dialog);

  adw_about_dialog_set_application_name(about, "Gnomos");
  adw_about_dialog_set_application_icon(about, APPLICATION_ID);
  adw_about_dialog_set_version(about, PACKAGE_VERSION);
  adw_about_dialog_set_developer_name(about, "Christoph Langner");
  adw_about_dialog_set_comments(
      about, "Ein GTK4/libadwaita-Client für Sonos-Lautsprecher, mit besonderem Fokus auf Geräte der ersten "
             "Generation (ZP80, ZP90, ZP100, ZP120, CR100), die von Sonos' eigenen aktuellen Apps nicht mehr "
             "unterstützt werden.");
  adw_about_dialog_set_copyright(about, "© 2026 Christoph Langner");

  // Gnomos links libnoson (GPL-3.0-or-later) statically, so the combined
  // work is bound to those terms — see LICENSE and README.md.
  adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_legal_section(about, "libnoson", "© 2014-2024 Jean-Luc Barriere", GTK_LICENSE_GPL_3_0, nullptr);

  // "Name https://url" is AdwAboutDialog's documented format for a
  // clickable link in a credits/acknowledgement section (there's no link
  // field on add_legal_section itself).
  const char* libraries[] = {"libnoson (Jean-Luc Barriere) https://github.com/janbar/noson", nullptr};
  adw_about_dialog_add_acknowledgement_section(about, "Bibliotheken", libraries);

  const char* developers[] = {"Christoph Langner", nullptr};
  adw_about_dialog_set_developers(about, developers);

  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::ShowShortcutsDialog()
{
  AdwDialog* dialog = adw_shortcuts_dialog_new();

  AdwShortcutsSection* section = adw_shortcuts_section_new("Wiedergabe");
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Play/Pause", "space"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Nächster Titel", "n"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Vorheriger Titel", "p"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Lauter", "Up"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Leiser", "Down"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Stumm schalten", "m"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Zufallswiedergabe", "s"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new("Wiederholen", "r"));
  adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), section);

  AdwShortcutsSection* general = adw_shortcuts_section_new("Allgemein");
  adw_shortcuts_section_add(general, adw_shortcuts_item_new("Bibliothek durchsuchen", "<Control>f"));
  adw_shortcuts_section_add(general, adw_shortcuts_item_new("Einstellungen", "<Control>comma"));
  adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), general);

  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

namespace
{
// notify::selected has no gtkmm binding on AdwComboRow (an Adw-only
// widget) — same "raw GObject signal + trampoline" approach as
// OnConfirmDialogResponse above.
extern "C" void OnComboRowSelectedChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(guint)>*>(user_data);
  (*callback)(adw_combo_row_get_selected(ADW_COMBO_ROW(object)));
}
extern "C" void DeleteGuintCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(guint)>*>(data);
}
extern "C" void OnButtonRowActivated(AdwButtonRow*, gpointer user_data)
{
  (*static_cast<std::function<void()>*>(user_data))();
}
// Cache size row: pushes the new value into ArtCache and refreshes the
// "N MB belegt" subtitle, since SetMaxDiskMb() may have just evicted files.
extern "C" void OnSpinRowValueChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  ArtCache::Instance().SetMaxDiskMb(static_cast<unsigned>(adw_spin_row_get_value(ADW_SPIN_ROW(object))));
  (*static_cast<std::function<void()>*>(user_data))();
}
extern "C" void DeleteVoidCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void()>*>(data);
}
extern "C" void OnSwitchRowActiveChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(bool)>*>(user_data);
  (*callback)(adw_switch_row_get_active(ADW_SWITCH_ROW(object)));
}
extern "C" void DeleteBoolCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(bool)>*>(data);
}
}  // namespace

void GnomosWindow::ShowSettingsDialog()
{
  AdwDialog* dialog = adw_preferences_dialog_new();
  adw_preferences_dialog_set_search_enabled(ADW_PREFERENCES_DIALOG(dialog), false);

  GtkWidget* page = adw_preferences_page_new();
  adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Einstellungen");

  // --- Erscheinungsbild ---
  GtkWidget* appearance_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(appearance_group), "Erscheinungsbild");

  GtkWidget* scheme_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scheme_row), "Farbschema");
  std::vector<Glib::ustring> scheme_labels = {"Systemeinstellung", "Hell", "Dunkel"};
  auto scheme_model = Gtk::StringList::create(scheme_labels);
  adw_combo_row_set_model(ADW_COMBO_ROW(scheme_row), G_LIST_MODEL(scheme_model->gobj()));
  // AdwStyleManager itself is the source of truth for the current scheme
  // (ApplyColorScheme() sets it directly), so read it back rather than
  // tracking a separate member here.
  AdwColorScheme current_scheme = adw_style_manager_get_color_scheme(adw_style_manager_get_default());
  adw_combo_row_set_selected(ADW_COMBO_ROW(scheme_row), current_scheme == ADW_COLOR_SCHEME_FORCE_LIGHT  ? 1
                                                         : current_scheme == ADW_COLOR_SCHEME_FORCE_DARK ? 2
                                                                                                          : 0);
  auto* scheme_callback = new std::function<void(guint)>([this](guint selected) {
    switch (selected)
    {
      case 1: ApplyColorScheme("light"); break;
      case 2: ApplyColorScheme("dark"); break;
      default: ApplyColorScheme("default"); break;
    }
  });
  g_signal_connect_data(scheme_row, "notify::selected", G_CALLBACK(OnComboRowSelectedChanged), scheme_callback,
                         DeleteGuintCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(appearance_group), scheme_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(appearance_group));

  // --- Benachrichtigungen ---
  GtkWidget* notifications_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(notifications_group), "Benachrichtigungen");

  GtkWidget* notify_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(notify_row), "Bei Titelwechsel benachrichtigen");
  adw_switch_row_set_active(ADW_SWITCH_ROW(notify_row), notify_on_track_change_);
  g_signal_connect_data(
      notify_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { SetNotifyOnTrackChange(active); }), DeleteBoolCallback,
      static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(notifications_group), notify_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(notifications_group));

  // --- Cover-Art-Cache ---
  GtkWidget* cache_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(cache_group), "Cover-Art-Cache");

  GtkWidget* size_row = adw_spin_row_new_with_range(10, 2000, 10);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(size_row), "Maximale Größe");
  adw_spin_row_set_value(ADW_SPIN_ROW(size_row), ArtCache::Instance().GetMaxDiskMb());
  auto update_subtitle = [size_row] {
    double mb = static_cast<double>(ArtCache::Instance().GetDiskUsageBytes()) / (1024.0 * 1024.0);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f MB belegt", mb);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(size_row), buf);
  };
  update_subtitle();
  g_signal_connect_data(size_row, "notify::value", G_CALLBACK(OnSpinRowValueChanged),
                         new std::function<void()>(update_subtitle), DeleteVoidCallback,
                         static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(cache_group), size_row);

  GtkWidget* clear_row = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(clear_row), "Cache jetzt leeren");
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(clear_row), "user-trash-symbolic");
  gtk_widget_add_css_class(clear_row, "destructive-action");
  auto* clear_callback = new std::function<void()>([update_subtitle] {
    ArtCache::Instance().Clear();
    update_subtitle();
  });
  g_signal_connect_data(clear_row, "activated", G_CALLBACK(OnButtonRowActivated), clear_callback,
                         DeleteVoidCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(cache_group), clear_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(cache_group));

  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::ShowLinkServiceDialog()
{
  std::vector<LinkableService> services = backend_->GetLinkableServices();
  if (services.empty())
  {
    ShowToast("Keine verknüpfbaren Dienste gefunden.");
    return;
  }

  auto* dialog = new Gtk::Window();
  dialog->set_title("Dienst verknüpfen");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* label = Gtk::make_managed<Gtk::Label>("Dienst");
  label->set_halign(Gtk::Align::START);
  content->append(*label);

  std::vector<Glib::ustring> names;
  names.reserve(services.size());
  for (const LinkableService& svc : services)
    names.push_back(svc.name);
  auto model = Gtk::StringList::create(names);
  auto* dropdown = Gtk::make_managed<Gtk::DropDown>(model);
  dropdown->set_enable_search(true);
  content->append(*dropdown);

  auto* info_label =
      Gtk::make_managed<Gtk::Label>("Danach öffnet sich ein Link, den du in einem Browser abschließen musst.");
  info_label->set_wrap(true);
  info_label->set_halign(Gtk::Align::START);
  info_label->add_css_class("dim-label");
  info_label->add_css_class("caption");
  content->append(*info_label);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* start_button = Gtk::make_managed<Gtk::Button>("Verknüpfung starten");
  start_button->add_css_class("suggested-action");
  start_button->signal_clicked().connect([this, dialog, dropdown, services] {
    guint selected = dropdown->get_selected();
    if (selected < services.size())
    {
      pending_link_service_id_ = services[selected].id;
      pending_link_service_name_ = services[selected].name;
      backend_->BeginServiceLink(services[selected].id);
    }
    dialog->close();
  });
  button_box->append(*cancel_button);
  button_box->append(*start_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
}

void GnomosWindow::ShowSavePlaylistDialog()
{
  auto* dialog = new Gtk::Window();
  dialog->set_title("Als Playlist speichern");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* label = Gtk::make_managed<Gtk::Label>("Name der Playlist");
  label->set_halign(Gtk::Align::START);
  content->append(*label);

  auto* entry = Gtk::make_managed<Gtk::Entry>();
  entry->set_activates_default(true);
  content->append(*entry);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* save_button = Gtk::make_managed<Gtk::Button>("Speichern");
  save_button->add_css_class("suggested-action");
  auto do_save = [this, dialog, entry] {
    Glib::ustring title = entry->get_text();
    if (!title.empty())
      backend_->SaveQueueAsPlaylist(title.raw());
    dialog->close();
  };
  save_button->signal_clicked().connect(do_save);
  entry->signal_activate().connect(do_save);
  button_box->append(*cancel_button);
  button_box->append(*save_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*save_button);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
  entry->grab_focus();
}

void GnomosWindow::ShowTrackInfoDialog()
{
  NowPlaying np = backend_->GetNowPlaying();
  if (!np.valid)
    return;

  auto* dialog = new Gtk::Window();
  dialog->set_title("Titel-Details");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(320, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* art = Gtk::make_managed<Gtk::Image>();
  art->set_from_icon_name("audio-x-generic-symbolic");
  art->set_pixel_size(256);
  art->add_css_class("card");
  art->set_halign(Gtk::Align::CENTER);
  content->append(*art);

  auto* title = Gtk::make_managed<Gtk::Label>(np.title.empty() ? "Unbekannter Titel" : np.title);
  title->set_wrap(true);
  title->add_css_class("title-2");
  content->append(*title);

  if (!np.artist.empty())
  {
    auto* artist = Gtk::make_managed<Gtk::Label>(np.artist);
    artist->set_wrap(true);
    artist->add_css_class("dim-label");
    content->append(*artist);
  }

  if (!np.album.empty())
  {
    auto* album = Gtk::make_managed<Gtk::Label>(np.album);
    album->set_wrap(true);
    album->add_css_class("dim-label");
    album->add_css_class("caption");
    content->append(*album);
  }

  // One-shot load, same mechanics as PlayerBar::LoadArt() but scoped to
  // this dialog's own lifetime — cancelled on close so a slow response
  // arriving after the dialog is already gone can't touch a freed widget.
  if (!np.art_uri.empty())
  {
    auto cancellable = Gio::Cancellable::create();
    dialog->signal_hide().connect([cancellable] { cancellable->cancel(); });
    auto file = Gio::File::create_for_uri(np.art_uri);
    file->load_contents_async(
        [art, file](Glib::RefPtr<Gio::AsyncResult>& result) {
          try
          {
            char* contents = nullptr;
            gsize length = 0;
            if (file->load_contents_finish(result, contents, length) && contents)
            {
              auto bytes = Glib::Bytes::create(contents, length);
              g_free(contents);
              art->set(Gdk::Texture::create_from_bytes(bytes));
            }
          }
          catch (const Glib::Error&)
          {
            // dialog (and so `art`) may already be gone, or the load
            // simply failed — either way, the placeholder icon stays.
          }
        },
        cancellable);
  }

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);

  std::string clipboard_text = np.title;
  if (!np.artist.empty())
    clipboard_text += " — " + np.artist;
  if (!np.album.empty())
    clipboard_text += " — " + np.album;
  auto* copy_button = Gtk::make_managed<Gtk::Button>();
  copy_button->set_icon_name("edit-copy-symbolic");
  copy_button->set_tooltip_text("Titel-Infos kopieren");
  copy_button->signal_clicked().connect([this, clipboard_text] { get_clipboard()->set_text(clipboard_text); });
  button_box->append(*copy_button);

  if (!np.artist.empty())
  {
    auto* search_artist_button = Gtk::make_managed<Gtk::Button>();
    search_artist_button->set_icon_name("system-search-symbolic");
    search_artist_button->set_tooltip_text("Interpret in der Bibliothek suchen");
    std::string artist = np.artist;
    search_artist_button->signal_clicked().connect([this, dialog, artist] {
      dialog->close();
      ShowLibrarySearchDialog(artist);
    });
    button_box->append(*search_artist_button);
  }

  auto* close_button = Gtk::make_managed<Gtk::Button>("Schließen");
  close_button->signal_clicked().connect([dialog] { dialog->close(); });
  button_box->append(*close_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
}

namespace
{
// AdwAlertDialog's "response" signal has no gtkmm binding (it's a plain
// GObject signal, not one of the widget signals gtkmm wraps) — this
// trampoline bridges it to a heap-allocated std::function, freed here once
// the single response has been delivered. AdwAlertDialog only ever emits
// "response" once per dialog (any response, including the close one),
// so there's no risk of a dangling callback firing twice.
extern "C" void OnConfirmDialogResponse(AdwAlertDialog*, const char* response, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(std::string)>*>(user_data);
  (*callback)(response != nullptr ? response : "");
  delete callback;
}
}  // namespace

void GnomosWindow::ShowConfirmDialog(const std::string& heading, const std::string& body,
                                      const std::string& confirm_label, std::function<void()> on_confirmed)
{
  AdwDialog* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", "Abbrechen", "confirm", confirm_label.c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(alert, "confirm", ADW_RESPONSE_DESTRUCTIVE);
  // Cancel, not the destructive action, is both the Enter-key default and
  // what a plain Escape/close counts as — matches GNOME HIG for
  // irreversible confirmations.
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");

  auto* callback = new std::function<void(std::string)>(
      [on_confirmed = std::move(on_confirmed)](std::string response) {
        if (response == "confirm")
          on_confirmed();
      });
  g_signal_connect(dialog, "response", G_CALLBACK(OnConfirmDialogResponse), callback);

  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::ShowClearQueueConfirmDialog()
{
  ShowConfirmDialog("Warteschlange leeren?",
                     "Alle Titel aus der Warteschlange entfernen? Das kann nicht rückgängig gemacht werden.",
                     "Leeren", [this] { backend_->ClearQueue(); });
}

void GnomosWindow::ShowDeleteFavoriteConfirmDialog(unsigned index)
{
  std::vector<FavoriteItem> favorites = backend_->GetFavorites();
  std::string title = index < favorites.size() && !favorites[index].title.empty() ? favorites[index].title
                                                                                    : "diesen Favoriten";
  ShowConfirmDialog("Favorit löschen?", "„" + title + "“ wirklich aus den Favoriten löschen?", "Löschen",
                     [this, index] { backend_->DeleteFavorite(index); });
}

void GnomosWindow::ShowDeleteAlarmConfirmDialog(std::string alarm_id)
{
  ShowConfirmDialog("Alarm löschen?", "Diesen Alarm wirklich löschen?", "Löschen",
                     [this, alarm_id] { backend_->DeleteAlarm(alarm_id); });
}

void GnomosWindow::ShowLibrarySearchDialog(const std::string& prefill)
{
  std::vector<std::string> categories = backend_->GetActiveServiceSearchCategories();
  // Empty categories means we're not inside a linked service (SMAPI) — fall
  // back to a client-side substring search within the current local-library
  // level instead of refusing outright.
  bool local_search = categories.empty();
  std::string local_object_id = library_stack_.back().first;

  auto* dialog = new Gtk::Window();
  dialog->set_title("Suchen");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  Gtk::DropDown* category_dropdown = nullptr;
  if (!local_search && categories.size() > 1)
  {
    auto* category_label = Gtk::make_managed<Gtk::Label>("Kategorie");
    category_label->set_halign(Gtk::Align::START);
    content->append(*category_label);

    std::vector<Glib::ustring> category_names;
    category_names.reserve(categories.size());
    for (const std::string& category : categories)
      category_names.push_back(category);
    auto category_model = Gtk::StringList::create(category_names);
    category_dropdown = Gtk::make_managed<Gtk::DropDown>(category_model);
    content->append(*category_dropdown);
  }

  auto* term_label = Gtk::make_managed<Gtk::Label>("Suchbegriff");
  term_label->set_halign(Gtk::Align::START);
  content->append(*term_label);

  auto* entry = Gtk::make_managed<Gtk::Entry>();
  entry->set_activates_default(true);
  if (!prefill.empty())
    entry->set_text(prefill);
  content->append(*entry);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* search_button = Gtk::make_managed<Gtk::Button>("Suchen");
  search_button->add_css_class("suggested-action");
  auto do_search = [this, dialog, entry, category_dropdown, categories, local_search, local_object_id] {
    Glib::ustring term = entry->get_text();
    if (!term.empty())
    {
      if (local_search)
      {
        backend_->SearchLocalLibraryAsync(local_object_id, term.raw());
      }
      else
      {
        size_t index = category_dropdown ? category_dropdown->get_selected() : 0;
        if (index < categories.size())
          backend_->SearchActiveServiceAsync(categories[index], term.raw());
      }
    }
    dialog->close();
  };
  search_button->signal_clicked().connect(do_search);
  entry->signal_activate().connect(do_search);
  button_box->append(*cancel_button);
  button_box->append(*search_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*search_button);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
  entry->grab_focus();
}

void GnomosWindow::OnServiceLinkReady(std::string url, std::string code)
{
  auto* dialog = new Gtk::Window();
  dialog->set_title("Verknüpfung: " + pending_link_service_name_);
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(420, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* instructions = Gtk::make_managed<Gtk::Label>(
      "Öffne den folgenden Link in einem Browser und schließe die Verknüpfung dort ab. Komm danach hierher "
      "zurück und klick auf \"Fertig\".");
  instructions->set_wrap(true);
  instructions->set_halign(Gtk::Align::START);
  content->append(*instructions);

  auto* link_button = Gtk::make_managed<Gtk::LinkButton>(url, url);
  link_button->set_halign(Gtk::Align::START);
  content->append(*link_button);

  if (!code.empty())
  {
    auto* code_label = Gtk::make_managed<Gtk::Label>("Code: " + code);
    code_label->set_halign(Gtk::Align::START);
    code_label->add_css_class("heading");
    content->append(*code_label);
  }

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* done_button = Gtk::make_managed<Gtk::Button>("Fertig");
  done_button->add_css_class("suggested-action");
  done_button->signal_clicked().connect([this, dialog] {
    if (!pending_link_service_id_.empty())
      library_stack_.push_back({std::string(kServiceRootPrefix) + pending_link_service_id_, pending_link_service_name_});
    backend_->CompleteServiceLink();
    dialog->close();
  });
  button_box->append(*cancel_button);
  button_box->append(*done_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
}

}  // namespace gnomos
