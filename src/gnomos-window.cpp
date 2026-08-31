// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnomos-window.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <map>
#include <tuple>

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
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/expander.h>
#include <gtkmm/expression.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/linkbutton.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scale.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include "config.h"
#include "widgets/art-cache.h"
#include "widgets/cover-thumbnail.h"
#include "widgets/radio-browser-service.h"

namespace gnomos
{

// Number of nav_list_box_ rows built once in the constructor and never
// rebuilt (Warteschlange/Favoriten/Alarme/Verlauf/Bibliothek) — matches
// kNavPages's own size further down; RebuildLibraryNavEntries() uses this
// to know where the library's own sub-item rows start.
constexpr size_t kStaticNavRowCount = 5;

GnomosWindow::GnomosWindow()
{
  set_title("Gnomos");
  // Section sidebar + page content; player_bar_ is a fixed-height bottom
  // bar, not a factor in width — overridden by LoadWindowState() below if
  // a size was saved from a previous run.
  set_default_size(1000, 760);
  LoadWindowState();
  signal_close_request().connect(sigc::mem_fun(*this, &GnomosWindow::OnCloseRequest), false);

  // --- Header bar (libadwaita, built via the C API — see header comment) ---
  header_bar_ = adw_header_bar_new();
  window_title_.set_text("Gnomos");
  window_title_.add_css_class("title");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(window_title_.gobj()));

  // Sidebar toggle — only ever visible once the AdwOverlaySplitView below
  // has collapsed the section sidebar behind a breakpoint on narrow
  // windows; bound to split_view_'s own "collapsed"/"show-sidebar"
  // properties once split_view_ exists further down this constructor.
  sidebar_toggle_button_.set_icon_name("sidebar-show-symbolic");
  sidebar_toggle_button_.set_tooltip_text("Bereiche ein-/ausblenden");
  sidebar_toggle_button_.set_visible(false);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(sidebar_toggle_button_.gobj()));

  // --- Room/zone picker — see room_button_'s own header comment for why
  // this replaced a second permanent sidebar. Popover content
  // (zones_scroller_/zones_list_box_) is built further down, where the
  // room list used to live; this just packs the button itself.
  // AdwButtonContent gives the button both the speaker icon and a text
  // label showing the current room name (UpdateRoomButtonLabel()) — a
  // plain icon-only button would leave the room invisible without
  // opening the popover. ---
  room_button_content_ = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(room_button_content_), "audio-speakers-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(room_button_content_), "Kein Raum");
  room_button_.set_child(*Glib::wrap(room_button_content_));
  room_button_.set_tooltip_text("Raum wählen");
  room_button_.set_popover(room_popover_);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(room_button_.gobj()));

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

  activity_spinner_.set_margin_start(6);
  activity_spinner_.set_margin_end(6);
  activity_spinner_.set_tooltip_text("Sonos-System antwortet …");
  refresh_button_.set_icon_name("view-refresh-symbolic");
  refresh_button_.set_tooltip_text("Sonos-Geräte suchen");
  refresh_button_.signal_clicked().connect(sigc::mem_fun(*this, &GnomosWindow::OnRefreshClicked));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(refresh_button_.gobj()));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(activity_spinner_.gobj()));

  // --- Grouping popover: which rooms play together with the selected zone ---
  grouping_list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  grouping_list_box_.add_css_class("boxed-list");
  auto* grouping_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  grouping_scroller->set_child(grouping_list_box_);
  grouping_scroller->set_size_request(260, -1);
  // Reported live with a screenshot: 320 forced scrolling for a real
  // 4-room group, its last row cut off mid-slider — each room takes
  // roughly 90-100px (name/switch row plus its own volume slider row),
  // so 320 barely fit 3. Raised generously enough to fit a real household
  // of up to ~6 rooms without scrolling at all; propagate_natural_height
  // below means this is only ever a ceiling, not a fixed height — a
  // smaller group still sizes down to its own actual content, and
  // Gtk::Popover's own screen-edge avoidance still keeps this from ever
  // overflowing off-screen for a household with even more rooms than that.
  grouping_scroller->set_max_content_height(640);
  grouping_scroller->set_propagate_natural_height(true);
  grouping_scroller->set_margin_start(6);
  grouping_scroller->set_margin_end(6);
  grouping_scroller->set_margin_bottom(6);

  // "Group all" — joins every *free* room (not already part of some other
  // group) to the current zone in one go, reusing the exact same
  // JoinRoomToCurrentZone() each per-room switch already calls. Matches
  // noson-app's own "group all zones" action (Zones.qml,
  // onGroupAllZoneClicked -> zoneList.selectAll()), minus the rooms the
  // per-row switches now also refuse to touch directly — see
  // RebuildGroupingPopover()'s own comment.
  auto* group_all_button = Gtk::make_managed<Gtk::Button>("Alle Räume gruppieren");
  group_all_button->add_css_class("flat");
  group_all_button->set_tooltip_text(
      "Fügt jeden freien Raum hinzu — bereits mit einem anderen Raum gruppierte Räume bleiben unverändert");
  group_all_button->set_margin_top(6);
  group_all_button->set_margin_start(6);
  group_all_button->set_margin_end(6);
  group_all_button->set_margin_bottom(6);
  group_all_button->signal_clicked().connect([this] {
    std::vector<RoomInfo> rooms = backend_->Rooms();
    // Same "free rooms only" restriction as each per-row switch — see
    // RebuildGroupingPopover()'s own comment for why a room already
    // merged into some other group is deliberately left alone here too,
    // rather than silently regrouped.
    std::map<std::string, int> group_sizes;
    for (const RoomInfo& room : rooms)
      ++group_sizes[room.group_id];
    for (const RoomInfo& room : rooms)
      if (room.group_id != selected_group_id_ && group_sizes[room.group_id] == 1)
        backend_->JoinRoomToCurrentZone(room.player_uuid);
  });

  // Symmetric counterpart — removes every *other* member of the current
  // group (leaving the coordinator standalone), reusing the exact same
  // RemoveRoomFromGroup() each per-room switch already calls.
  auto* ungroup_all_button = Gtk::make_managed<Gtk::Button>("Gruppe auflösen");
  ungroup_all_button->add_css_class("flat");
  ungroup_all_button->set_margin_start(6);
  ungroup_all_button->set_margin_end(6);
  ungroup_all_button->set_margin_bottom(6);
  ungroup_all_button->signal_clicked().connect([this] {
    for (const RoomInfo& room : backend_->Rooms())
      if (room.group_id == selected_group_id_ && room.player_uuid != room.coordinator_uuid)
        backend_->RemoveRoomFromGroup(room.player_uuid);
  });

  auto* grouping_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  grouping_box->append(*group_all_button);
  grouping_box->append(*ungroup_all_button);
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

  add_sound_label("Sub-Pegel");
  sub_gain_scale_.set_range(-15, 15);
  sub_gain_scale_.set_digits(0);
  sub_gain_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetSubGain(static_cast<int16_t>(sub_gain_scale_.get_value()));
  });
  sound_box->append(sub_gain_scale_);

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

  auto* output_fixed_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* output_fixed_label = Gtk::make_managed<Gtk::Label>("Feste Lautstärke (Line-Out)");
  output_fixed_label->set_halign(Gtk::Align::START);
  output_fixed_label->set_hexpand(true);
  output_fixed_row->append(*output_fixed_label);
  output_fixed_switch_.set_valign(Gtk::Align::CENTER);
  output_fixed_switch_.set_tooltip_text(
      "Ignoriert die eigene Lautstärkeregelung — für den Anschluss an einen Verstärker mit eigener Lautstärke");
  output_fixed_switch_.signal_state_set().connect(
      [this](bool state) -> bool {
        backend_->SetOutputFixed(state);
        return true;
      },
      false);
  output_fixed_row->append(output_fixed_switch_);
  sound_box->append(*output_fixed_row);

  sound_box->append(*Gtk::make_managed<Gtk::Separator>());

  // Autoplay and the status LED are both rarely-touched settings —
  // collapsed behind a toggle by default so the popover's *default*
  // height stays reasonable regardless of how many rooms/settings a given
  // household happens to have. Reported live: with every section always
  // expanded, the popover no longer fit without scrolling.
  //
  // A plain Gtk::Expander was tried first and reported back as looking
  // out of place — it brings its own distinct look (an indented triangle
  // + label) that doesn't match every *other* row in this popover
  // (Loudness/Night Mode/Feste Lautstärke: a label on the left, a control
  // flush right). Rebuilt as a button styled to look like exactly one
  // more of those rows — a label plus a chevron in the same spot a switch
  // would sit — driving a Gtk::Revealer instead, so it reads as "another
  // row that happens to expand" rather than a visually foreign widget.
  auto* advanced_toggle_row = Gtk::make_managed<Gtk::Button>();
  advanced_toggle_row->add_css_class("flat");
  auto* advanced_toggle_content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* advanced_toggle_label = Gtk::make_managed<Gtk::Label>("Erweitert");
  advanced_toggle_label->set_halign(Gtk::Align::START);
  advanced_toggle_label->set_hexpand(true);
  advanced_toggle_content->append(*advanced_toggle_label);
  auto* advanced_chevron = Gtk::make_managed<Gtk::Image>();
  advanced_chevron->set_from_icon_name("pan-end-symbolic");
  advanced_toggle_content->append(*advanced_chevron);
  advanced_toggle_row->set_child(*advanced_toggle_content);
  sound_box->append(*advanced_toggle_row);

  auto* advanced_revealer = Gtk::make_managed<Gtk::Revealer>();
  advanced_revealer->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
  advanced_toggle_row->signal_clicked().connect([advanced_revealer, advanced_chevron] {
    bool reveal = !advanced_revealer->get_reveal_child();
    advanced_revealer->set_reveal_child(reveal);
    advanced_chevron->set_from_icon_name(reveal ? "pan-down-symbolic" : "pan-end-symbolic");
  });
  sound_box->append(*advanced_revealer);

  auto* advanced_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  advanced_box->set_margin_top(6);

  auto* autoplay_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* autoplay_label = Gtk::make_managed<Gtk::Label>("Autoplay (Line-In)");
  autoplay_label->set_halign(Gtk::Align::START);
  autoplay_label->set_hexpand(true);
  autoplay_row->append(*autoplay_label);
  autoplay_switch_.set_valign(Gtk::Align::CENTER);
  autoplay_switch_.set_tooltip_text(
      "Startet automatisch die Wiedergabe hier, sobald ein Line-In-Signal an diesem Gerät anliegt");
  autoplay_switch_.signal_state_set().connect(
      [this](bool state) -> bool {
        backend_->SetAutoplay(state);
        return true;
      },
      false);
  autoplay_row->append(autoplay_switch_);
  advanced_box->append(*autoplay_row);

  auto* autoplay_volume_switch_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* autoplay_volume_switch_label = Gtk::make_managed<Gtk::Label>("Eigene Autoplay-Lautstärke");
  autoplay_volume_switch_label->set_halign(Gtk::Align::START);
  autoplay_volume_switch_label->set_hexpand(true);
  autoplay_volume_switch_row->append(*autoplay_volume_switch_label);
  autoplay_use_volume_switch_.set_valign(Gtk::Align::CENTER);
  autoplay_use_volume_switch_.signal_state_set().connect(
      [this](bool state) -> bool {
        backend_->SetUseAutoplayVolume(state);
        return true;
      },
      false);
  autoplay_volume_switch_row->append(autoplay_use_volume_switch_);
  advanced_box->append(*autoplay_volume_switch_row);

  auto* autoplay_volume_label = Gtk::make_managed<Gtk::Label>("Autoplay-Lautstärke");
  autoplay_volume_label->set_halign(Gtk::Align::START);
  autoplay_volume_label->add_css_class("caption");
  autoplay_volume_label->add_css_class("dim-label");
  advanced_box->append(*autoplay_volume_label);
  autoplay_volume_scale_.set_range(0, 100);
  autoplay_volume_scale_.set_digits(0);
  autoplay_volume_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetAutoplayVolume(static_cast<uint8_t>(autoplay_volume_scale_.get_value()));
  });
  advanced_box->append(autoplay_volume_scale_);

  advanced_box->append(*Gtk::make_managed<Gtk::Separator>());

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
  advanced_box->append(*led_row);

  advanced_revealer->set_child(*advanced_box);

  sound_popover_.set_child(*sound_box);

  sound_button_.set_icon_name("multimedia-volume-control-symbolic");
  sound_button_.set_tooltip_text("Klang");
  sound_button_.set_popover(sound_popover_);
  sound_popover_.signal_show().connect([this] { backend_->RefreshSoundSettingsAsync(); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(sound_button_.gobj()));

  set_titlebar(*Glib::wrap(header_bar_));

  // --- Room/zone list — now room_popover_'s content instead of a
  // permanent sidebar (see room_button_'s own comment). Sized like a
  // popover (fixed width, capped+scrollable height), the same pattern
  // grouping_popover_'s own room list already uses, rather than the
  // set_vexpand(true)/set_min_content_width() sizing appropriate for a
  // permanent panel this used to have. ---
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
  // row-activated (real click/Enter on a row), not row-selected — the
  // latter also fires from OnZonesChanged()'s own select_row() call
  // whenever zones_list_box_'s selection gets (re)applied while the
  // popover's content is still unmapped/hidden, which GTK then re-emits
  // once the popover actually maps. Closing on row-selected instead
  // closed the popover in the very same tick it had just opened in —
  // confirmed live: every click logged "shown" immediately followed by
  // "closed", so the popover was never visibly open at all.
  zones_list_box_.signal_row_activated().connect(
      [this](Gtk::ListBoxRow*) { room_popover_.popdown(); });

  zones_scroller_.set_child(zones_list_box_);
  zones_scroller_.set_size_request(260, -1);
  zones_scroller_.set_max_content_height(400);
  zones_scroller_.set_propagate_natural_height(true);
  room_popover_.set_child(zones_scroller_);

  // --- Section sidebar (Warteschlange/Favoriten/Alarme/Verlauf/
  // Bibliothek) — replaces the AdwViewSwitcher this app used to have as a
  // top tab bar, styled after noson-app's own left-hand navigation and
  // matching zones_list_box_'s own icon-row look (see nav_list_box_'s
  // header comment). Built from the same (page-name, title, icon) tuples
  // view_stack_'s pages themselves use, so the two can never drift apart. ---
  static const std::array<std::tuple<const char*, const char*, const char*>, 5> kNavPages = {{
      {"queue", "Warteschlange", "view-list-symbolic"},
      {"favorites", "Favoriten", "starred-symbolic"},
      {"alarms", "Alarme", "alarm-symbolic"},
      {"history", "Verlauf", "document-open-recent-symbolic"},
      {"library", "Bibliothek", "folder-music-symbolic"},
  }};
  nav_list_box_.set_selection_mode(Gtk::SelectionMode::SINGLE);
  nav_list_box_.add_css_class("navigation-sidebar");
  nav_list_box_.signal_row_selected().connect(sigc::mem_fun(*this, &GnomosWindow::OnNavRowSelected));
  for (const auto& [name, title, icon] : kNavPages)
  {
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(8);
    row_box->set_margin_bottom(8);
    row_box->set_margin_start(8);
    row_box->set_margin_end(8);
    auto* row_icon = Gtk::make_managed<Gtk::Image>();
    row_icon->set_from_icon_name(icon);
    row_icon->add_css_class("dim-label");
    row_box->append(*row_icon);
    auto* row_label = Gtk::make_managed<Gtk::Label>(title);
    row_label->set_halign(Gtk::Align::START);
    row_box->append(*row_label);
    nav_list_box_.append(*row_box);

    std::string page_name = name;
    if (page_name == "library")
    {
      // Unlike every other static row here, "Bibliothek" wasn't just
      // switching to an already-fresh page — library_view_ keeps showing
      // whatever level was last browsed, so clicking this while already
      // deep in a browse (e.g. a specific album's tracks) did nothing
      // visible at all, confirmed live as exactly that "this button
      // doesn't seem to do anything" report. Jumping back to the root
      // overview mirrors what a specific sub-item row already does below
      // (RebuildLibraryNavEntries()'s own append_entry lambda) — just with
      // no category pushed on top, since this one means the overview
      // itself, not a specific category within it.
      nav_row_actions_.push_back([this, page_name] {
        library_stack_.clear();
        library_stack_.push_back({"", "Bibliothek"});
        backend_->BrowseLibraryAsync("");
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), page_name.c_str());
      });
    }
    else
    {
      nav_row_actions_.push_back([this, page_name] {
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), page_name.c_str());
      });
    }
  }
  auto* nav_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  nav_scroller->set_child(nav_list_box_);
  nav_scroller->set_min_content_width(200);
  nav_scroller->set_vexpand(true);

  // --- Content: queue/favorites/alarms/library pages. The Now Playing
  // panel (player_bar_) is docked separately, as a bottom bar spanning
  // the whole window rather than living inside this content area — see
  // root_box further down this constructor. ---
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
  favorites_view_.signal_play_all_requested().connect([this] { backend_->PlayAllFavoritesAsync(); });
  favorites_view_.signal_queue_all_requested().connect([this] {
    backend_->AddAllFavoritesToQueue();
    ShowToast("Zur Warteschlange hinzugefügt");
  });

  alarms_view_.signal_enabled_toggled().connect(
      [this](std::string id, bool enabled) { backend_->SetAlarmEnabled(id, enabled); });
  alarms_view_.signal_include_linked_zones_toggled().connect(
      [this](std::string id, bool include) { backend_->SetAlarmIncludeLinkedZones(id, include); });
  alarms_view_.signal_delete_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowDeleteAlarmConfirmDialog));
  alarms_view_.signal_add_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowAddAlarmDialog));
  alarms_view_.signal_edit_requested().connect(sigc::mem_fun(*this, &GnomosWindow::OnAlarmEditRequested));
  alarms_view_.signal_duplicate_requested().connect(sigc::mem_fun(*this, &GnomosWindow::OnAlarmDuplicateRequested));

  history_view_.signal_clear_requested().connect([this] {
    history_.clear();
    last_history_key_.clear();
    history_view_.SetItems(history_);
    SaveHistory();
  });
  history_view_.signal_search_requested().connect([this](unsigned index) {
    if (index >= history_.size())
      return;
    const HistoryEntry& entry = history_[index];
    ShowLibrarySearchDialog(entry.artist.empty() ? entry.title : entry.artist);
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
  library_view_.signal_add_to_favorites_requested().connect([this](unsigned index) {
    backend_->AddLibraryItemToFavorites(index);
    ShowToast("Zu Favoriten hinzugefügt");
  });
  library_view_.signal_delete_requested().connect(
      sigc::mem_fun(*this, &GnomosWindow::ShowDeleteLibraryEntryConfirmDialog));
  library_view_.signal_radio_settings_requested().connect(
      sigc::mem_fun(*this, &GnomosWindow::ShowRadioMprisSettingsDialog));
  library_view_.signal_add_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowAddRadioStationDialog));
  library_view_.signal_add_to_playlist_requested().connect(
      sigc::mem_fun(*this, &GnomosWindow::ShowAddToPlaylistDialog));
  library_view_.signal_reorder_requested().connect([this](unsigned from, unsigned to) {
    const std::string& current_object_id = library_stack_.back().first;
    backend_->ReorderLibraryPlaylistTrack(current_object_id, from, to);
    // ReorderLibraryPlaylistTrack() is queued first on the same serial
    // tasks_ worker this browse gets queued on — same reasoning as
    // ShowDeleteLibraryEntryConfirmDialog()'s own re-browse.
    backend_->BrowseLibraryAsync(current_object_id);
  });
  library_view_.signal_play_all_requested().connect([this] { backend_->PlayAllLibraryItemsAsync(); });
  library_view_.signal_queue_all_requested().connect([this] {
    backend_->AddAllLibraryItemsToQueue();
    ShowToast("Zur Warteschlange hinzugefügt");
  });
  // Re-renders the already-fetched current_library_entries_ with the
  // flipped preference — no need to ask NosonBackend for anything again,
  // this is purely a local rendering choice.
  library_view_.signal_view_mode_toggled().connect([this] {
    SetPreferGridView(!prefer_grid_view_);
    OnLibraryChanged();
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

  // --- Section sidebar as an AdwOverlaySplitView, not a plain Gtk::Paned —
  // lets it collapse behind sidebar_toggle_button_ on narrow windows (see
  // the AdwBreakpoint below), the adaptive behavior the README used to
  // list as a known gap. min/max width mirror the fixed 240px paned
  // position this replaced. nav_scroller (built above, wrapping
  // nav_list_box_) is the sidebar now; view_stack_ is used directly as the
  // content, since content_box_ no longer exists — it only ever wrapped
  // view_stack_ together with the now-removed AdwViewSwitcher.
  split_view_ = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_view_), GTK_WIDGET(nav_scroller->gobj()));
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_view_), GTK_WIDGET(view_stack_));
  adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_view_), 200);
  adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_view_), 260);
  // sidebar_toggle_button_ only needs to exist (and be shown) once the
  // split view has actually collapsed the sidebar into an overlay —
  // above that width it's docked side-by-side and the button would be
  // redundant. "active" <-> "show-sidebar" is bidirectional so it also
  // stays in sync if the sidebar is dismissed via its own swipe/click-away
  // gesture rather than the button itself.
  //
  // G_BINDING_SYNC_CREATE syncs from the *source* (the button's own
  // "active") to the target on creation, so the button needs to start
  // active first — otherwise this binding would immediately force
  // show-sidebar back to false and hide the sidebar completely at normal
  // window widths (a real bug once, confirmed live: the sidebar was
  // simply missing at startup until the window got narrow enough to
  // collapse it).
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
  // shrink to 0x0. 360x480 is a sane practical floor for this UI — a bit
  // taller than before now that player_bar_ (below) adds a fixed-height
  // bottom bar rather than sharing width with the content area.
  gtk_widget_set_size_request(breakpoint_bin, 360, 480);
  adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(breakpoint_bin), split_view_);

  AdwBreakpoint* sidebar_breakpoint =
      adw_breakpoint_new(adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 900, ADW_LENGTH_UNIT_PX));
  GValue collapsed_value = G_VALUE_INIT;
  g_value_init(&collapsed_value, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed_value, TRUE);
  adw_breakpoint_add_setter(sidebar_breakpoint, G_OBJECT(split_view_), "collapsed", &collapsed_value);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(breakpoint_bin), sidebar_breakpoint);
  g_value_unset(&collapsed_value);

  LoadSplitFractions();

  // --- Root layout: sidebar+content above, player_bar_ docked as a fixed-
  // height bar along the bottom — not a side panel anymore (see
  // PlayerBar's own header comment for why: a bottom bar gives the seek
  // bar far more usable width than a ~300px-wide side column ever could).
  // vexpand on breakpoint_bin only, not player_bar_, is what keeps the
  // bar pinned to its natural height instead of being stretched.
  auto* root_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  Gtk::Widget* wrapped_breakpoint_bin = Glib::wrap(breakpoint_bin);
  wrapped_breakpoint_bin->set_vexpand(true);
  root_box->append(*wrapped_breakpoint_bin);
  root_box->append(player_bar_);

  // --- Toast overlay wraps everything, for error feedback ---
  toast_overlay_ = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay_), GTK_WIDGET(root_box->gobj()));
  set_child(*Glib::wrap(toast_overlay_));

  // --- Backend wiring ---
  backend_ = std::make_unique<NosonBackend>();
  backend_->signal_discovery_done().connect(sigc::mem_fun(*this, &GnomosWindow::OnDiscoveryDone));
  backend_->signal_busy_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnBusyChanged));
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
  radio_history_filter_ = std::make_unique<RadioContentFilter>(*backend_);

  pending_restore_room_uuid_ = LoadLastRoomUuid();

  LoadHistory();
  history_view_.SetItems(history_);

  LoadColorScheme();
  LoadNotificationSetting();
  LoadLibraryViewPreference();
  LoadArtistImagesSetting();
  LoadFallbackIconScaleSetting();
  // CoverThumbnail's own scale is a static, process-wide default — needs
  // syncing explicitly here for a value loaded from a previous run;
  // SetFallbackIconScale() (the Settings row's own handler) keeps it in
  // sync for any *later* change on its own.
  CoverThumbnail::SetFallbackIconScale(fallback_icon_scale_);

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
  discovering_ = true;
  UpdateActivitySpinner();
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
  // Confirmed live: OnZonesChanged() tears down and rebuilds every row in
  // zones_list_box_ from scratch on *every* signal_zones_changed_ (which
  // fires more than once during startup, as the household's topology
  // settles across a few real ZGTopologyChanged events) — re-selecting the
  // still-current room there means calling select_row() on a brand-new
  // Gtk::ListBoxRow object each time, which fires row-selected again even
  // though nothing actually changed. backend_->SelectZone() itself has no
  // dedup (it always opens a fresh player connection and re-fetches
  // now-playing/volume), so without this check, every one of those
  // redundant topology events cascaded into a full RefreshQueueAsync() and
  // QueueView rebuild — the actual cause of the cover art briefly flashing
  // back to the fallback icon a few times right after launch.
  bool room_changed = zone.group_id != selected_group_id_;
  selected_group_id_ = zone.group_id;
  if (room_changed)
    backend_->SelectZone(selected_group_id_);
  SaveLastRoom(zone.coordinator_uuid);
  UpdateRoomButtonLabel();
}

void GnomosWindow::OnNavRowSelected(Gtk::ListBoxRow* row)
{
  if (!row || !view_stack_)
    return;
  int index = row->get_index();
  if (index < 0 || static_cast<size_t>(index) >= nav_row_actions_.size())
    return;
  nav_row_actions_[static_cast<size_t>(index)]();
}

void GnomosWindow::RebuildLibraryNavEntries()
{
  // The five static top-level rows (Warteschlange/Favoriten/Alarme/Verlauf/
  // Bibliothek) are never touched here — only whatever library sub-item
  // rows a previous call appended after them.
  while (nav_row_actions_.size() > kStaticNavRowCount)
  {
    int last_index = static_cast<int>(nav_row_actions_.size()) - 1;
    if (Gtk::ListBoxRow* row = nav_list_box_.get_row_at_index(last_index))
      nav_list_box_.remove(*row);
    nav_row_actions_.pop_back();
  }

  // Splits the flat root-category list into two labeled groups: "Bibliothek"
  // (the locally-indexed-share namespace, object_id prefix "A:" — see
  // BrowseLibraryAsync()'s own comment on that prefix) and "Dienste"
  // (everything else content actually comes from outside that share: Sonos-
  // native saved playlists, the radio directory, and linked third-party
  // services). "Dienst verknüpfen…" lives in "Dienste" too, alongside
  // whatever's already linked — it used to be excluded from the sidebar
  // entirely (it opens a dialog rather than browsing into anything), which
  // meant the *only* way to ever discover it was clicking the top-level
  // "Bibliothek" nav row itself rather than any of its sub-items, landing
  // on the true library root where it's shown as a regular entry — not
  // obvious, confirmed live as a real "how do I even link a service"
  // question once "Dienste" existed as an obvious place to expect it.
  auto append_header = [this](const char* title) {
    auto* header_label = Gtk::make_managed<Gtk::Label>(title);
    header_label->set_halign(Gtk::Align::START);
    header_label->add_css_class("heading");
    header_label->set_margin_top(6);
    header_label->set_margin_bottom(6);
    header_label->set_margin_start(8);
    header_label->set_margin_end(8);
    nav_list_box_.append(*header_label);
    // Every nav_list_box_ row needs a matching nav_row_actions_ slot —
    // OnNavRowSelected() indexes into it by row position — even a header
    // row that does nothing when clicked.
    nav_row_actions_.push_back([] {});
  };
  // Icon + label, same construction kNavPages' own static rows use just
  // above (row_icon/row_label/8px gap) — every row in a GtkListBox styled
  // ".navigation-sidebar" carrying an icon is the actual GNOME convention
  // here, not just the five top-level ones; a sub-item used to be a bare
  // label. The extra margin_start (24 vs. the top-level rows' 8) is the
  // only thing marking this as a nested entry rather than its own size or
  // icon presence — a smaller step than the old plain-label version's 40,
  // now that the icon itself already carries most of that visual weight.
  auto append_entry = [this](const LibraryEntry& entry) {
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(6);
    row_box->set_margin_bottom(6);
    row_box->set_margin_start(24);
    row_box->set_margin_end(8);
    auto* row_icon = Gtk::make_managed<Gtk::Image>();
    // Every root entry populates its own icon_name (BrowseLibraryAsync())
    // except a genuinely unexpected one this list was never meant to
    // handle — a plain folder glyph reads as "some kind of container"
    // regardless, rather than leaving the row with no icon at all.
    row_icon->set_from_icon_name(entry.icon_name.empty() ? "folder-symbolic" : entry.icon_name);
    row_icon->add_css_class("dim-label");
    row_box->append(*row_icon);
    auto* row_label = Gtk::make_managed<Gtk::Label>(entry.title);
    row_label->set_halign(Gtk::Align::START);
    row_label->set_ellipsize(Pango::EllipsizeMode::END);
    row_label->add_css_class("dim-label");
    row_label->add_css_class("caption");
    row_box->append(*row_label);
    nav_list_box_.append(*row_box);

    // Same special-case OnLibraryEntryActivated() already has for clicking
    // this same entry from the actual root level — opens the picker
    // dialog directly rather than trying to "browse into" a sentinel
    // object_id that was never a real container to begin with.
    if (entry.object_id == kLinkServiceSentinel)
    {
      nav_row_actions_.push_back([this] { ShowLinkServiceDialog(); });
      return;
    }

    std::string object_id = entry.object_id;
    std::string title = entry.title;
    nav_row_actions_.push_back([this, object_id, title] {
      // Jump straight to this category, discarding any deeper browse
      // position — matches what clicking it from the actual library root
      // level would do, since that's exactly where this list comes from.
      library_stack_.clear();
      library_stack_.push_back({"", "Bibliothek"});
      library_stack_.push_back({object_id, title.empty() ? "—" : title});
      backend_->BrowseLibraryAsync(object_id);
      adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), "library");
    });
  };

  bool has_library = std::any_of(library_root_entries_.begin(), library_root_entries_.end(),
                                  [](const LibraryEntry& e) { return e.object_id.compare(0, 2, "A:") == 0; });
  bool has_services = std::any_of(library_root_entries_.begin(), library_root_entries_.end(),
                                   [](const LibraryEntry& e) { return e.object_id.compare(0, 2, "A:") != 0; });

  if (has_library)
  {
    append_header("Bibliothek");
    for (const LibraryEntry& entry : library_root_entries_)
      if (entry.object_id.compare(0, 2, "A:") == 0)
        append_entry(entry);
  }
  if (has_services)
  {
    append_header("Dienste");
    for (const LibraryEntry& entry : library_root_entries_)
      if (entry.object_id.compare(0, 2, "A:") != 0)
        append_entry(entry);
  }
}

void GnomosWindow::UpdateRoomButtonLabel()
{
  if (!room_button_content_)
    return;
  for (const ZoneInfo& zone : current_zones_)
  {
    if (zone.group_id == selected_group_id_)
    {
      adw_button_content_set_label(ADW_BUTTON_CONTENT(room_button_content_), zone.display_name.c_str());
      return;
    }
  }
  adw_button_content_set_label(ADW_BUTTON_CONTENT(room_button_content_), "Kein Raum");
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
  discovering_ = false;
  UpdateActivitySpinner();
  if (!ok)
    ShowToast("Kein Sonos-Gerät im Netzwerk gefunden.");
}

void GnomosWindow::OnBusyChanged(bool busy)
{
  backend_busy_ = busy;
  UpdateActivitySpinner();
}

void GnomosWindow::UpdateActivitySpinner()
{
  if (discovering_ || backend_busy_)
    activity_spinner_.start();
  else
    activity_spinner_.stop();
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

    auto* info_button = Gtk::make_managed<Gtk::Button>();
    info_button->set_icon_name("dialog-information-symbolic");
    info_button->add_css_class("flat");
    info_button->set_valign(Gtk::Align::CENTER);
    info_button->set_tooltip_text("Geräteinfo");
    std::string group_id = zone.group_id;
    std::string zone_name = zone.display_name;
    info_button->signal_clicked().connect([this, group_id, zone_name] { ShowDeviceInfoDialog(group_id, zone_name); });
    row_box->append(*info_button);

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
  // Covers the no-zones-at-all case: select_row() above (and the
  // OnZoneRowSelected() it triggers) never runs then, which would
  // otherwise leave room_button_'s label stuck on a stale room name.
  UpdateRoomButtonLabel();

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
  // Skipped while TransportState::Transitioning — np.playing_from_queue is
  // *always* false for that one event (CurrentTrack/AVTransportURI are
  // unreliable mid-transition; see RefreshNowPlayingLocked()'s own
  // comment), not just during a real track change but also, confirmed
  // live, during a plain seek within the current track. Blindly applying
  // that momentary false flashed both the queue highlight and the
  // "Weiter: …" hint off and back on a moment later — a visible layout
  // jump in the player bar. Keeping the previous value until a settled
  // (non-Transitioning) event confirms the real one avoids that without
  // ever risking a wrong value being shown instead.
  if (np.state != TransportState::Transitioning)
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

  // Radio: treat History (and, downstream, the desktop notification) the
  // same way MprisService treats MPRIS Metadata — ad breaks/idents
  // interspersed between song repeats shouldn't spam either one. See
  // RadioContentFilter's own comment; only radio-like sources
  // (duration == 0) with a known stream are affected, a queued track's
  // artist is stable and passes through unchanged.
  NowPlaying filtered = now_playing;
  if (now_playing.duration == 0 && !now_playing.stream_uri.empty())
  {
    filtered.artist = radio_history_filter_->Filter(now_playing.stream_uri, now_playing.artist);
    if (filtered.artist.empty() && !now_playing.artist.empty())
      return;  // filler, a repeat, or this station opted out — not a real change
  }

  std::string key = filtered.title + "\x1f" + filtered.artist;
  if (key == last_history_key_)
    return;  // same track as last time — OnNowPlayingChanged() re-fired without a real change
  last_history_key_ = key;

  HistoryEntry entry;
  entry.title = filtered.title;
  entry.artist = filtered.artist;
  entry.album = filtered.album;
  entry.art_uri = filtered.art_uri;
  history_.insert(history_.begin(), entry);
  if (history_.size() > kMaxHistoryEntries)
    history_.resize(kMaxHistoryEntries);

  history_view_.SetItems(history_);
  SaveHistory();
  SendTrackChangeNotification(filtered);
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

void GnomosWindow::LoadLibraryViewPreference()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    prefer_grid_view_ = keyfile->get_boolean("library", "prefer_grid");
  }
  catch (const Glib::Error&)
  {
    // fine — no preference saved yet, stays at the true default
  }
}

void GnomosWindow::SetPreferGridView(bool prefer_grid)
{
  prefer_grid_view_ = prefer_grid;

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
  keyfile->set_boolean("library", "prefer_grid", prefer_grid);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the preference won't be remembered next launch
  }
}

void GnomosWindow::LoadArtistImagesSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    load_artist_images_ = keyfile->get_boolean("library", "load_artist_images");
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays off (the default)
  }
}

void GnomosWindow::SetLoadArtistImages(bool enabled)
{
  load_artist_images_ = enabled;

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
  keyfile->set_boolean("library", "load_artist_images", enabled);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
  // Takes effect immediately if "Interpreten" (or any other level with
  // artist-typed entries) happens to be the currently displayed one —
  // harmless no-op re-render otherwise, same as toggling prefer_grid_view_
  // already does unconditionally.
  OnLibraryChanged();
}

void GnomosWindow::LoadFallbackIconScaleSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    double scale = keyfile->get_double("library", "fallback_icon_scale");
    if (scale > 0.0 && scale <= 1.0)
      fallback_icon_scale_ = scale;
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays at the full-size default
  }
}

void GnomosWindow::SetFallbackIconScale(double scale)
{
  fallback_icon_scale_ = scale;
  CoverThumbnail::SetFallbackIconScale(scale);

  // Debounced: an AdwSpinRow fires notify::value many times a second while
  // being dragged/scrolled — unlike a switch row (SetLoadArtistImages()'s
  // own OnLibraryChanged() call), which only ever fires once per click.
  // Confirmed live: letting each intermediate value trigger its own full
  // save-to-disk + OnLibraryChanged() rebuild (potentially dozens/hundreds
  // of grid tiles, e.g. a real Albums listing) back-to-back crashed the
  // app. Same reasoning as NosonBackend::SetVolume()'s own debouncing —
  // only the settled value, a short delay after the last change, actually
  // persists and re-renders.
  fallback_icon_scale_debounce_connection_.disconnect();
  fallback_icon_scale_debounce_connection_ = Glib::signal_timeout().connect(
      [this, scale] {
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
        keyfile->set_double("library", "fallback_icon_scale", scale);
        try
        {
          keyfile->save_to_file(StateFilePath());
        }
        catch (const Glib::Error&)
        {
          // non-fatal — just means the setting won't be remembered next launch
        }
        OnLibraryChanged();
        return false;  // one-shot
      },
      200);
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

// Soonest enabled alarm's own summary ("Heute, 07:00 Uhr — Küche" /
// "Morgen, ..." / a weekday name), or empty if there are no enabled
// alarms with parseable recurrence days — an alarm using a recurrence
// Gnomos never itself generates (e.g. a literal "ONCE" from the official
// Sonos app rather than a day list) is silently skipped rather than
// guessed at, since ParseRecurrenceDays() would return no days for it.
std::string NextAlarmSummary(const std::vector<AlarmInfo>& alarms)
{
  std::time_t now_time = std::time(nullptr);
  std::tm now_tm{};
  localtime_r(&now_time, &now_tm);
  int now_wday = now_tm.tm_wday;
  int now_minutes = now_tm.tm_hour * 60 + now_tm.tm_min;

  static const std::array<const char*, 7> kWeekdayNames = {"Sonntag",     "Montag", "Dienstag", "Mittwoch",
                                                             "Donnerstag", "Freitag", "Samstag"};

  int best_day_offset = -1;
  int best_minutes = -1;
  const AlarmInfo* best_alarm = nullptr;
  for (const AlarmInfo& alarm : alarms)
  {
    if (!alarm.enabled)
      continue;
    int hour = 0, minute = 0;
    if (std::sscanf(alarm.start_time.c_str(), "%d:%d", &hour, &minute) != 2)
      continue;
    int alarm_minutes = hour * 60 + minute;
    std::vector<int> days = ParseRecurrenceDays(alarm.recurrence);
    if (days.empty())
      continue;
    for (int offset = 0; offset < 8; ++offset)
    {
      int wday = (now_wday + offset) % 7;
      if (std::find(days.begin(), days.end(), wday) == days.end())
        continue;
      if (offset == 0 && alarm_minutes < now_minutes)
        continue;  // today's own slot already passed — next real match is a week from now
      if (best_day_offset < 0 || offset < best_day_offset ||
          (offset == best_day_offset && alarm_minutes < best_minutes))
      {
        best_day_offset = offset;
        best_minutes = alarm_minutes;
        best_alarm = &alarm;
      }
      break;  // this alarm's own soonest occurrence found — move to the next alarm
    }
  }

  if (!best_alarm)
    return "";

  char time_buf[16];
  std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", best_minutes / 60, best_minutes % 60);
  std::string when = best_day_offset == 0    ? "Heute"
                      : best_day_offset == 1 ? "Morgen"
                                              : kWeekdayNames[(now_wday + best_day_offset) % 7];
  return when + ", " + time_buf + " Uhr — " + best_alarm->room_name;
}
}  // namespace

void GnomosWindow::OnAlarmsChanged()
{
  std::vector<AlarmInfo> alarms = backend_->GetAlarms();
  alarms_view_.SetItems(alarms);
  alarms_view_.SetNextAlarmLabel(NextAlarmSummary(alarms));
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

void GnomosWindow::OnAlarmDuplicateRequested(std::string alarm_id)
{
  for (const AlarmInfo& alarm : backend_->GetAlarms())
  {
    if (alarm.id == alarm_id)
    {
      ShowAlarmDialog(&alarm, /*duplicate=*/true);
      return;
    }
  }
}

void GnomosWindow::OnLibraryChanged()
{
  current_library_entries_ = backend_->GetLibraryEntries();

  // Grid view (cover-art tiles, like Euphonica's own Albums/Artists grid —
  // https://github.com/htkhiem/euphonica) is available whenever any entry
  // at this level says so — LibraryEntry::display_as_grid is populated
  // uniformly by NosonBackend regardless of whether the level came from
  // the local library or a third-party service (see that field's own
  // comment for the two different underlying heuristics), so this no
  // longer needs to branch on where we are the way it used to. Whether to
  // actually *render* one when available is the user's own choice
  // (prefer_grid_view_, toggled via view_mode_button_), not decided here.
  bool grid_available = std::any_of(current_library_entries_.begin(), current_library_entries_.end(),
                                     [](const LibraryEntry& entry) { return entry.display_as_grid; });

  // Favoriting only offered below the true root — "Interpreten"/"Alben"/...
  // are static categories, not real content Sonos has anything to
  // favorite. Deletion only offered browsing "SQ:" or "R:0/0" themselves,
  // where every entry really is a destroyable saved playlist or custom
  // radio station. Add-to-playlist offered below the true root too, but
  // *not* while browsing "R:0/0" — confirmed live: a saved Sonos playlist
  // is conceptually a list of tracks, and adding a live radio stream to
  // one via AddURIToSavedQueue() reads as nonsensical there even though
  // nothing stops the SOAP call itself from accepting it. Reordering only
  // offered while viewing a *specific* playlist's own tracks (an
  // "SQ:<id>" level, not "SQ:" itself).
  const std::string& current_object_id = library_stack_.back().first;
  bool below_root = library_stack_.size() > 1;
  bool is_radio_level = current_object_id == "R:0/0";
  bool viewing_one_playlist =
      current_object_id.compare(0, 3, "SQ:") == 0 && current_object_id != "SQ:";
  // Bulk "play all"/"add all to queue" and the per-row "add to queue"/
  // "play next" buttons are all excluded for "R:0/0", same underlying
  // reason as add-to-playlist above — see show_play_all_action's/
  // show_queue_all_action's/show_queue_actions's own comments in
  // library-view.h (a per-row "play now" button takes the place of the
  // latter two there instead).
  library_view_.SetEntries(current_library_entries_, grid_available, prefer_grid_view_, below_root,
                            current_object_id == "SQ:" || is_radio_level, below_root && !is_radio_level,
                            viewing_one_playlist, !is_radio_level, !is_radio_level, !is_radio_level,
                            load_artist_images_, is_radio_level);
  library_view_.SetLevelTitle(library_stack_.back().second);
  library_view_.SetBackVisible(library_stack_.size() > 1);
  library_view_.SetAddVisible(current_object_id == "R:0/0");

  // Root level specifically — see library_root_entries_'s own comment for
  // why this can't just reuse current_library_entries_ unconditionally
  // (it tracks whatever level is currently browsed, usually not the root).
  if (library_stack_.size() == 1)
  {
    library_root_entries_ = current_library_entries_;
    RebuildLibraryNavEntries();
  }
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

  std::vector<RoomInfo> rooms = backend_->Rooms();

  // A room's own current group size — RoomInfo has no member list of its
  // own, but every room sharing the same group_id is (by definition) a
  // member of the same group, so counting occurrences of each group_id
  // across all rooms gives exactly that. Needed to tell a genuinely
  // "free" room (alone in its own single-member group) apart from one
  // that's already merged into *some other*, non-selected multi-room
  // group — see the switch's own sensitivity comment below for why that
  // distinction matters.
  std::map<std::string, int> group_sizes;
  for (const RoomInfo& room : rooms)
    ++group_sizes[room.group_id];

  for (const RoomInfo& room : rooms)
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
    const bool in_selected_group = room.group_id == selected_group_id_;
    room_switch->set_active(in_selected_group);

    // This room *is* the currently selected zone's own coordinator —
    // always "in the group" trivially, not something to toggle from here.
    const bool is_self = (room.player_uuid == room.coordinator_uuid && in_selected_group);
    // Requested directly: a room already merged into *some other*,
    // non-selected group (group_sizes[room.group_id] > 1 — more than
    // just itself) can't be joined to this one in a single step anymore,
    // even though the underlying Sonos action would technically allow it
    // (moving a device straight from one group to another). Only a
    // genuinely free room (alone in its own single-member group) can be
    // added directly; moving an already-grouped room means removing it
    // from its current group first (switch it off there), then adding it
    // here as a separate action — the explicit two-step the user wants
    // instead of an implicit silent regroup.
    const bool is_free = group_sizes[room.group_id] == 1;
    const bool can_toggle = in_selected_group || is_free;
    room_switch->set_sensitive(!is_self && can_toggle);
    if (!is_self && !can_toggle)
      room_switch->set_tooltip_text(
          "Bereits mit einem anderen Raum gruppiert — dort zuerst entfernen, um ihn hier hinzuzufügen");

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
      // See PlayerBar's own identical call for why — Gtk::Range's built-in
      // scroll-wheel support needs a non-zero step/page increment to
      // actually move the value, not just consume the scroll event.
      volume_scale->set_increments(2, 10);
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

void GnomosWindow::ShowDeviceInfoDialog(std::string group_id, std::string zone_name)
{
  // Every room sharing this group_id is a member of the zone right now —
  // same "no dedicated member-list field, so derive it from RoomInfo"
  // technique RebuildGroupingPopover() already uses for its own free/
  // grouped check.
  std::vector<RoomInfo> members;
  for (const RoomInfo& room : backend_->Rooms())
    if (room.group_id == group_id)
      members.push_back(room);

  auto* dialog = new Gtk::Window();
  dialog->set_title("Geräteinfo");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(320, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* heading = Gtk::make_managed<Gtk::Label>(zone_name);
  heading->set_wrap(true);
  heading->add_css_class("title-2");
  heading->set_halign(Gtk::Align::START);
  content->append(*heading);

  // One section per member room — not just the coordinator's — since
  // zone_name/the heading above already names the whole zone as a unit.
  std::string clipboard_text = zone_name;
  bool first = true;
  for (const RoomInfo& member : members)
  {
    if (!first)
      content->append(*Gtk::make_managed<Gtk::Separator>());
    first = false;

    DeviceInfo info = backend_->GetDeviceInfo(member.player_uuid);

    auto* room_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    room_row->set_margin_top(6);
    auto* room_heading = Gtk::make_managed<Gtk::Label>(member.name);
    room_heading->add_css_class("heading");
    room_heading->set_halign(Gtk::Align::START);
    room_row->append(*room_heading);
    if (info.is_gen1)
    {
      auto* badge = Gtk::make_managed<Gtk::Label>("Gen 1");
      badge->add_css_class("dim-label");
      badge->add_css_class("caption");
      room_row->append(*badge);
    }
    content->append(*room_row);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(12);
    grid->set_margin_top(6);

    // (label, value) — a field libnoson couldn't resolve (e.g. an offline
    // room that dropped out of the topology between opening the popover
    // and clicking its info button) shows as "—" rather than an empty cell.
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"Modell", info.model_number.empty() ? "—" : info.model_number},
        {"IP-Adresse", info.ip.empty() ? "—" : info.ip},
        {"MAC-Adresse", info.mac.empty() ? "—" : info.mac},
        {"Software-Version", info.software_version.empty() ? "—" : info.software_version},
        {"Hardware-Version", info.hardware_version.empty() ? "—" : info.hardware_version},
        {"Seriennummer", info.serial_number.empty() ? "—" : info.serial_number},
    };
    int row = 0;
    for (const auto& [label_text, value_text] : fields)
    {
      auto* label = Gtk::make_managed<Gtk::Label>(label_text);
      label->set_halign(Gtk::Align::START);
      label->add_css_class("dim-label");
      grid->attach(*label, 0, row);

      auto* value = Gtk::make_managed<Gtk::Label>(value_text);
      value->set_halign(Gtk::Align::START);
      value->set_selectable(true);
      grid->attach(*value, 1, row);
      ++row;
    }
    content->append(*grid);

    clipboard_text += "\n\n" + member.name + "\nModell: " + (info.model_number.empty() ? "—" : info.model_number) +
                       "\nIP-Adresse: " + (info.ip.empty() ? "—" : info.ip) +
                       "\nMAC-Adresse: " + (info.mac.empty() ? "—" : info.mac) +
                       "\nSoftware-Version: " + (info.software_version.empty() ? "—" : info.software_version) +
                       "\nHardware-Version: " + (info.hardware_version.empty() ? "—" : info.hardware_version) +
                       "\nSeriennummer: " + (info.serial_number.empty() ? "—" : info.serial_number);
  }

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* copy_button = Gtk::make_managed<Gtk::Button>("Kopieren");
  copy_button->signal_clicked().connect([this, clipboard_text] { get_clipboard()->set_text(clipboard_text); });
  button_box->append(*copy_button);
  auto* close_button = Gtk::make_managed<Gtk::Button>("Schließen");
  close_button->add_css_class("suggested-action");
  close_button->signal_clicked().connect([dialog] { dialog->close(); });
  button_box->append(*close_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->present();
}

void GnomosWindow::ShowAddAlarmDialog()
{
  ShowAlarmDialog(nullptr);
}

// existing == nullptr creates a new alarm; otherwise edits it in place,
// prefilled from its current schedule (enabled state and sound source are
// left untouched either way — see NosonBackend::UpdateAlarmSchedule()).
void GnomosWindow::ShowAlarmDialog(const AlarmInfo* existing, bool duplicate)
{
  std::vector<RoomInfo> rooms = backend_->Rooms();
  if (rooms.empty())
  {
    ShowToast("Kein Raum verfügbar.");
    return;
  }

  // existing (when non-null) supplies default field values either way;
  // editing specifically means "saving updates *existing's own alarm" —
  // false for both a genuinely new alarm (existing == nullptr) and a
  // duplicate (existing != nullptr, but a new one gets created instead).
  bool editing = existing && !duplicate;

  auto* dialog = new Gtk::Window();
  dialog->set_title(editing ? "Alarm bearbeiten" : (duplicate ? "Alarm duplizieren" : "Neuer Alarm"));
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
  if (editing)
    sound_entries.push_back("Aktueller Klang beibehalten");
  for (const std::string& title : sound_titles)
    sound_entries.push_back(title);
  auto sound_model = Gtk::StringList::create(sound_entries);
  auto* sound_dropdown = Gtk::make_managed<Gtk::DropDown>(sound_model);
  // See ShowLinkServiceDialog()'s identical call for why this expression
  // is needed alongside set_enable_search() — without it the search entry
  // shows but never actually filters anything.
  sound_dropdown->set_expression(
      Gtk::PropertyExpression<Glib::ustring>::create(Gtk::StringObject::get_type(), "string"));
  sound_dropdown->set_enable_search(true);
  sound_dropdown->set_selected(0);
  content->append(*sound_dropdown);

  bool has_keep_current_for_test = editing;
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
  auto* confirm_button = Gtk::make_managed<Gtk::Button>(editing ? "Speichern" : "Erstellen");
  confirm_button->add_css_class("suggested-action");
  std::string alarm_id = editing ? existing->id : std::string();
  bool has_keep_current = editing;
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
  sub_gain_scale_.set_sensitive(settings.sub_gain_supported);
  sub_gain_scale_.set_value(settings.sub_gain);
  autoplay_volume_scale_.set_sensitive(settings.autoplay_supported && settings.autoplay_use_volume);
  autoplay_volume_scale_.set_value(settings.autoplay_volume);
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

  output_fixed_switch_.set_sensitive(settings.output_fixed_supported);
  output_fixed_switch_.set_active(settings.output_fixed);
  output_fixed_switch_.set_state(settings.output_fixed);

  autoplay_switch_.set_sensitive(settings.autoplay_supported);
  autoplay_switch_.set_active(settings.autoplay_enabled);
  autoplay_switch_.set_state(settings.autoplay_enabled);
  autoplay_use_volume_switch_.set_sensitive(settings.autoplay_supported);
  autoplay_use_volume_switch_.set_active(settings.autoplay_use_volume);
  autoplay_use_volume_switch_.set_state(settings.autoplay_use_volume);
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

void GnomosWindow::StartLibraryIndexProgressPolling()
{
  // A re-click while a previous poll is still running (or still waiting to
  // ever see ShareIndexInProgress go true) shouldn't stack up a second
  // timer/connection alongside the first.
  library_index_poll_connection_.disconnect();
  library_index_status_connection_.disconnect();

  auto seen_in_progress = std::make_shared<bool>(false);
  auto ticks_without_progress = std::make_shared<int>(0);
  library_index_status_connection_ = backend_->signal_library_index_status_changed().connect(
      [this, seen_in_progress, ticks_without_progress] {
        if (backend_->GetLibraryIndexInProgress())
        {
          *seen_in_progress = true;
          *ticks_without_progress = 0;
          return;
        }
        if (!*seen_in_progress && ++*ticks_without_progress < 5)
          // Sonos may not have flipped ShareIndexInProgress to true yet —
          // give it a few more ticks before giving up silently, rather
          // than risk a false "completed" toast for a scan that hasn't
          // actually registered as running at all yet.
          return;

        library_index_poll_connection_.disconnect();
        library_index_status_connection_.disconnect();
        if (*seen_in_progress)
        {
          std::string error = backend_->GetLibraryIndexLastError();
          ShowToast(error.empty() ? "Bibliotheks-Scan abgeschlossen" : "Bibliotheks-Scan fehlgeschlagen: " + error);
        }
        // Never observed running at all (too fast to catch between polls,
        // or the scan silently did nothing) — RefreshLibraryIndex()'s own
        // "gestartet" toast already set expectations, so this stays quiet
        // rather than risk a misleading message either way.
      });
  library_index_poll_connection_ = Glib::signal_timeout().connect(
      [this] {
        backend_->CheckLibraryIndexProgressAsync();
        return true;  // signal_library_index_status_changed()'s own handler above stops this
      },
      2000);
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
  adw_about_dialog_set_website(about, "https://github.com/linuxundich/gnomos");
  adw_about_dialog_set_issue_url(about, "https://github.com/linuxundich/gnomos/issues");

  // Gnomos links libnoson (GPL-3.0-or-later) statically, so the combined
  // work is bound to those terms — see LICENSE and README.md.
  adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_legal_section(about, "libnoson", "© 2014-2024 Jean-Luc Barriere", GTK_LICENSE_GPL_3_0, nullptr);

  // "Name https://url" is AdwAboutDialog's documented format for a
  // clickable link in a credits/acknowledgement section (there's no link
  // field on add_legal_section itself).
  const char* libraries[] = {"libnoson (Jean-Luc Barriere) https://github.com/janbar/noson", nullptr};
  adw_about_dialog_add_acknowledgement_section(about, "Bibliotheken", libraries);

  // Radio-Browser (see RadioBrowserService's own header) — the public
  // directory the "Radiosender hinzufügen" dialog's search is built on.
  const char* services[] = {"Radio Browser https://www.radio-browser.info", nullptr};
  adw_about_dialog_add_acknowledgement_section(about, "Dienste", services);

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
extern "C" void OnDoubleSpinRowValueChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(double)>*>(user_data);
  (*callback)(adw_spin_row_get_value(ADW_SPIN_ROW(object)));
}
extern "C" void DeleteDoubleCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(double)>*>(data);
}
// AdwEntryRow implements GtkEditable rather than exposing its own text
// property/signal, same "raw GObject + trampoline" approach as the rows
// above.
extern "C" void OnEntryRowTextChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(const std::string&)>*>(user_data);
  const char* text = gtk_editable_get_text(GTK_EDITABLE(object));
  (*callback)(text ? text : "");
}
extern "C" void DeleteStringCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(const std::string&)>*>(data);
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

  // --- Bibliothek ---
  GtkWidget* library_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(library_group), "Bibliothek");
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(library_group),
      "Alles andere in Gnomos bleibt innerhalb deines Sonos-Haushalts im lokalen Netzwerk — die Funktion "
      "unten ist die einzige Ausnahme davon.");

  GtkWidget* icon_scale_row = adw_spin_row_new_with_range(20, 100, 5);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(icon_scale_row), "Symbolgröße");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(icon_scale_row),
      "Größe des Symbols innerhalb einer Kachel, wenn kein Coverbild verfügbar ist");
  adw_spin_row_set_value(ADW_SPIN_ROW(icon_scale_row), fallback_icon_scale_ * 100.0);
  g_signal_connect_data(
      icon_scale_row, "notify::value", G_CALLBACK(OnDoubleSpinRowValueChanged),
      new std::function<void(double)>([this](double percent) { SetFallbackIconScale(percent / 100.0); }),
      DeleteDoubleCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), icon_scale_row);

  GtkWidget* artist_images_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(artist_images_row), "Künstlerbilder laden");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(artist_images_row),
      "Fragt für Interpreten ohne eigenes Coverbild ein Foto bei der öffentlichen Deezer-API "
      "(api.deezer.com) ab — das ist eine echte Abfrage über das Internet, kein lokaler Sonos-Zugriff. "
      "Dabei wird jeweils der Interpretenname an Deezer übertragen. Es gelten Deezers eigene "
      "Nutzungsbedingungen für diese API (siehe Link unten).");
  adw_switch_row_set_active(ADW_SWITCH_ROW(artist_images_row), load_artist_images_);
  g_signal_connect_data(
      artist_images_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { SetLoadArtistImages(active); }), DeleteBoolCallback,
      static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), artist_images_row);

  GtkWidget* deezer_terms_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(deezer_terms_row), "Deezer-API und Nutzungsbedingungen");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(deezer_terms_row), "developers.deezer.com");
  auto* deezer_link_button = Gtk::make_managed<Gtk::LinkButton>("https://developers.deezer.com/api", "Öffnen");
  deezer_link_button->set_valign(Gtk::Align::CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(deezer_terms_row), GTK_WIDGET(deezer_link_button->gobj()));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), deezer_terms_row);

  GtkWidget* refresh_index_row = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(refresh_index_row), "Bibliothek neu einlesen");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(refresh_index_row),
      "Lässt Sonos die eingebundene lokale Freigabe neu einlesen — etwa nach dem Hinzufügen neuer Dateien");
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(refresh_index_row), "view-refresh-symbolic");
  auto* refresh_index_callback = new std::function<void()>([this] {
    backend_->RefreshLibraryIndex();
    ShowToast("Bibliotheks-Scan gestartet");
    StartLibraryIndexProgressPolling();
  });
  g_signal_connect_data(refresh_index_row, "activated", G_CALLBACK(OnButtonRowActivated), refresh_index_callback,
                         DeleteVoidCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), refresh_index_row);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(library_group));

  // --- Genres ---
  GtkWidget* genre_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(genre_group), "Genres");
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(genre_group),
      "Jedes Zeichen hier trennt mehrere in einem Genre-Tag zusammengefasste Genres in der Genre-Ansicht "
      "der Bibliothek auf, z. B. \";\" bei \"Rap; Metal; Hard-Core\" — mehrere Zeichen sind möglich (z. B. "
      "\";/|\").");

  GtkWidget* genre_separators_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(genre_separators_row), "Trennzeichen");
  gtk_editable_set_text(GTK_EDITABLE(genre_separators_row), backend_->GetGenreSeparators().c_str());
  g_signal_connect_data(
      genre_separators_row, "notify::text", G_CALLBACK(OnEntryRowTextChanged),
      new std::function<void(const std::string&)>(
          [this](const std::string& text) { backend_->SetGenreSeparators(text); }),
      DeleteStringCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(genre_group), genre_separators_row);

  GtkWidget* genre_first_only_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(genre_first_only_row), "Nur erstes Genre verwenden");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(genre_first_only_row),
      "Zeigt nur das erste Genre vor dem ersten Trennzeichen an, statt alle aufzuteilen — hilfreich, wenn "
      "Sonos einen langen, zusammengesetzten Genre-Tag selbst schon abschneidet und nachfolgende Genres "
      "dadurch unvollständig ankommen (z. B. „Elec“ statt „Electronic“)");
  adw_switch_row_set_active(ADW_SWITCH_ROW(genre_first_only_row), backend_->GetGenreUseFirstOnly());
  g_signal_connect_data(
      genre_first_only_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { backend_->SetGenreUseFirstOnly(active); }),
      DeleteBoolCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(genre_group), genre_first_only_row);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(genre_group));

  // --- Radio ---
  GtkWidget* radio_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(radio_group), "Radio");
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(radio_group),
      "Gilt zusätzlich zu einem eigenen Muster, das sich pro Sender über dessen Zahnrad-Symbol unter "
      "„Radiosender“ einstellen lässt.");

  GtkWidget* spam_filter_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spam_filter_row), "Werbeinhalte automatisch erkennen");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(spam_filter_row),
      "Behandelt Inhalte mit mehr als zwei aufeinanderfolgenden Leerzeichen als Werbung/Füllinhalt — "
      "betrifft Benachrichtigungen (MPRIS) und den Verlauf gleichermaßen.");
  adw_switch_row_set_active(ADW_SWITCH_ROW(spam_filter_row), backend_->GetRadioSpamWhitespaceFilterEnabled());
  g_signal_connect_data(
      spam_filter_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { backend_->SetRadioSpamWhitespaceFilterEnabled(active); }),
      DeleteBoolCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(radio_group), spam_filter_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(radio_group));

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
  // set_enable_search() alone shows a search entry in the popup but never
  // actually filters anything without this — confirmed live. Despite the
  // gtkmm/GTK docs describing GtkStringList items as auto-detected, the
  // search filter itself still needs an explicit expression telling it
  // how to pull a comparable string out of each item.
  dropdown->set_expression(
      Gtk::PropertyExpression<Glib::ustring>::create(Gtk::StringObject::get_type(), "string"));
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

  if (!np.album.empty())
  {
    auto* search_album_button = Gtk::make_managed<Gtk::Button>();
    search_album_button->set_icon_name("media-optical-cd-audio-symbolic");
    search_album_button->set_tooltip_text("Album in der Bibliothek suchen");
    std::string album = np.album;
    search_album_button->signal_clicked().connect([this, dialog, album] {
      dialog->close();
      ShowLibrarySearchDialog(album);
    });
    button_box->append(*search_album_button);
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

void GnomosWindow::ShowDeleteLibraryEntryConfirmDialog(unsigned index)
{
  bool is_radio = library_stack_.back().first == "R:0/0";
  std::string default_title = is_radio ? "diesen Radiosender" : "diese Playlist";
  std::string title = index < current_library_entries_.size() && !current_library_entries_[index].title.empty()
                           ? current_library_entries_[index].title
                           : default_title;
  std::string heading = is_radio ? "Radiosender löschen?" : "Playlist löschen?";
  ShowConfirmDialog(heading, "„" + title + "“ wirklich löschen?", "Löschen", [this, index, is_radio] {
    if (is_radio)
      backend_->DeleteLibraryRadioStation(index);
    else
      backend_->DeleteLibraryPlaylist(index);
    // Both run on the same serial tasks_ worker this browse gets queued
    // on, so the delete always executes first — see
    // DeleteLibraryPlaylist()'s own comment for why NosonBackend can't
    // just do this itself.
    backend_->BrowseLibraryAsync(library_stack_.back().first);
  });
}

void GnomosWindow::ShowAddToPlaylistDialog(unsigned library_index)
{
  backend_->FetchSavedPlaylistsAsync();
  // One-shot: FetchSavedPlaylistsAsync() is async (a real Browse() round
  // trip), so the dialog can't be built until its result actually arrives
  // — a std::shared_ptr<sigc::connection> captured by value lets the
  // lambda disconnect itself from inside its own body once it's run once,
  // same trick GnomosWindow uses nowhere else yet but sigc++ itself has no
  // simpler built-in for "connect, fire once, auto-disconnect" outside
  // Glib::signal_idle()'s own connect_once() (which is main-loop-idle
  // specific, not applicable to a plain sigc::signal like this one).
  auto connection = std::make_shared<sigc::connection>();
  *connection = backend_->signal_saved_playlists_changed().connect([this, library_index, connection] {
    connection->disconnect();
    std::vector<LibraryEntry> playlists = backend_->GetSavedPlaylists();

    auto* dialog = new Gtk::Window();
    dialog->set_title("Zu Playlist hinzufügen");
    dialog->set_transient_for(*this);
    dialog->set_modal(true);
    dialog->set_default_size(360, -1);

    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    content->set_margin_top(18);
    content->set_margin_bottom(18);
    content->set_margin_start(18);
    content->set_margin_end(18);

    auto* label = Gtk::make_managed<Gtk::Label>("Playlist");
    label->set_halign(Gtk::Align::START);
    content->append(*label);

    // "Neue Playlist…" always sits first — mirrors the "Dienst
    // verknüpfen…" sentinel row pattern (library-view.cpp's own root
    // categories) for the same reason: creating one shouldn't require
    // already having a queue to save first, which was the only way to
    // create a playlist at all before this (confirmed live: an empty
    // playlist list here used to just toast "Keine Playlisten vorhanden"
    // and refuse to open a dialog at all).
    std::vector<Glib::ustring> names;
    names.reserve(playlists.size() + 1);
    names.push_back("Neue Playlist…");
    for (const LibraryEntry& entry : playlists)
      names.push_back(entry.title.empty() ? "Unbenannt" : entry.title);
    auto model = Gtk::StringList::create(names);
    auto* dropdown = Gtk::make_managed<Gtk::DropDown>(model);
    content->append(*dropdown);

    // Only actually used while "Neue Playlist…" (index 0) is selected —
    // kept unconditionally visible rather than shown/hidden reactively as
    // the dropdown selection changes, since it's still perfectly clear
    // from the placeholder alone which choice it belongs to.
    auto* new_playlist_entry = Gtk::make_managed<Gtk::Entry>();
    new_playlist_entry->set_placeholder_text("Name für „Neue Playlist…“");
    new_playlist_entry->set_activates_default(true);
    content->append(*new_playlist_entry);

    auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    button_box->set_halign(Gtk::Align::END);
    button_box->set_margin_top(6);
    auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
    cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
    button_box->append(*cancel_button);
    auto* confirm_button = Gtk::make_managed<Gtk::Button>("Hinzufügen");
    confirm_button->add_css_class("suggested-action");
    confirm_button->signal_clicked().connect(
        [this, dialog, dropdown, new_playlist_entry, playlists, library_index] {
          guint selected = dropdown->get_selected();
          if (selected == GTK_INVALID_LIST_POSITION)
            return;
          if (selected == 0)
          {
            std::string title = new_playlist_entry->get_text().raw();
            if (title.empty())
            {
              new_playlist_entry->grab_focus();
              return;
            }
            backend_->CreatePlaylistAndAddLibraryItem(library_index, title);
            ShowToast("Playlist erstellt und Titel hinzugefügt");
          }
          else
          {
            if (selected - 1 >= playlists.size())
              return;
            backend_->AddLibraryItemToPlaylist(library_index, playlists[selected - 1].object_id);
            ShowToast("Zu Playlist hinzugefügt");
          }
          dialog->close();
        });
    button_box->append(*confirm_button);
    content->append(*button_box);

    dialog->set_child(*content);
    dialog->set_default_widget(*confirm_button);
    dialog->present();
  });
}

void GnomosWindow::ShowAddRadioStationDialog()
{
  auto* dialog = new Gtk::Window();
  dialog->set_title("Radiosender hinzufügen");
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(420, 560);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* disclosure_label = Gtk::make_managed<Gtk::Label>(
      "Durchsucht das öffentliche Senderverzeichnis von radio-browser.info — "
      "eine echte Abfrage über das Internet, kein lokaler Sonos-Zugriff.");
  disclosure_label->set_halign(Gtk::Align::START);
  disclosure_label->set_wrap(true);
  disclosure_label->add_css_class("caption");
  disclosure_label->add_css_class("dim-label");
  content->append(*disclosure_label);

  auto* search_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* search_entry = Gtk::make_managed<Gtk::Entry>();
  search_entry->set_placeholder_text("Sendername…");
  search_entry->set_hexpand(true);
  search_row->append(*search_entry);
  // Populated once FetchCountries() resolves — "Alle Länder" (no filter) is
  // always index 0 regardless, so search can fire before the country list
  // itself has even loaded.
  auto country_model = Gtk::StringList::create({"Alle Länder"});
  auto* country_dropdown = Gtk::make_managed<Gtk::DropDown>(country_model);
  search_row->append(*country_dropdown);
  auto* search_button = Gtk::make_managed<Gtk::Button>();
  search_button->set_icon_name("system-search-symbolic");
  search_row->append(*search_button);
  content->append(*search_row);

  auto* results_list = Gtk::make_managed<Gtk::ListBox>();
  results_list->set_selection_mode(Gtk::SelectionMode::NONE);
  auto* results_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  results_scroller->set_child(*results_list);
  results_scroller->set_vexpand(true);
  results_scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  content->append(*results_scroller);

  // countrycodes[i] index-aligns with country_model — empty string at index
  // 0 ("Alle Länder") means no country filter; shared_ptr since both the
  // FetchCountries() callback and every later search need to read it.
  auto countrycodes = std::make_shared<std::vector<std::string>>();
  countrycodes->push_back("");
  // Index-aligned with whatever's currently in results_list, same
  // real-index convention every other list in this app already uses —
  // rebuilt fresh on every search, so always valid for the results
  // currently on screen.
  auto results = std::make_shared<std::vector<RadioBrowserStation>>();

  // Shared by both ways to add a result — clicking its own per-row "+"
  // button, and activating the row itself (clicking anywhere else on it).
  // Confirmed live these need to actually be the same code path: a
  // Gtk::Button nested inside a Gtk::ListBoxRow consumes its own click
  // rather than letting it propagate into row-activated, so a "+" button
  // that only *looks* like it adds something (relying on the row beneath
  // it to fire instead) silently does nothing when clicked directly.
  auto add_station = [this](const RadioBrowserStation& station) {
    backend_->AddRadioStation(station.name, station.url, station.favicon);
    // Same reasoning as ShowDeleteLibraryEntryConfirmDialog()'s own
    // re-browse — AddRadioStation() is queued first on the same serial
    // worker.
    backend_->BrowseLibraryAsync(library_stack_.back().first);
    ShowToast("„" + station.name + "“ hinzugefügt");
  };

  auto run_search = [this, search_entry, country_dropdown, countrycodes, results, results_list, add_station] {
    while (Gtk::Widget* child = results_list->get_first_child())
      results_list->remove(*child);
    std::string countrycode;
    guint selected = country_dropdown->get_selected();
    if (selected != GTK_INVALID_LIST_POSITION && selected < countrycodes->size())
      countrycode = (*countrycodes)[selected];
    RadioBrowserService::Instance().SearchStations(
        search_entry->get_text(), countrycode,
        [results, results_list, add_station](std::vector<RadioBrowserStation> stations) {
          *results = std::move(stations);
          if (results->empty())
          {
            auto* placeholder = Gtk::make_managed<Gtk::Label>("Keine Sender gefunden.");
            placeholder->add_css_class("dim-label");
            placeholder->set_margin_top(12);
            placeholder->set_margin_bottom(12);
            results_list->append(*placeholder);
            return;
          }
          for (const RadioBrowserStation& station : *results)
          {
            auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row_box->set_margin_top(6);
            row_box->set_margin_bottom(6);
            row_box->set_margin_start(6);
            row_box->set_margin_end(6);

            auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            labels->set_hexpand(true);
            auto* title = Gtk::make_managed<Gtk::Label>(station.name);
            title->set_halign(Gtk::Align::START);
            title->set_ellipsize(Pango::EllipsizeMode::END);
            labels->append(*title);
            std::string subtitle = station.countrycode;
            if (!station.codec.empty())
              subtitle += (subtitle.empty() ? "" : " · ") + station.codec;
            if (station.bitrate > 0)
              subtitle += " · " + std::to_string(station.bitrate) + " kbps";
            if (!subtitle.empty())
            {
              auto* subtitle_label = Gtk::make_managed<Gtk::Label>(subtitle);
              subtitle_label->set_halign(Gtk::Align::START);
              subtitle_label->set_ellipsize(Pango::EllipsizeMode::END);
              subtitle_label->add_css_class("dim-label");
              subtitle_label->add_css_class("caption");
              labels->append(*subtitle_label);
            }
            row_box->append(*labels);

            auto* add_button = Gtk::make_managed<Gtk::Button>();
            add_button->set_icon_name("list-add-symbolic");
            add_button->add_css_class("flat");
            add_button->set_valign(Gtk::Align::CENTER);
            add_button->set_tooltip_text("Hinzufügen");
            add_button->signal_clicked().connect([add_station, station] { add_station(station); });
            row_box->append(*add_button);

            results_list->append(*row_box);
          }
        });
  };
  search_button->signal_clicked().connect(run_search);
  search_entry->signal_activate().connect(run_search);

  // Row-add wiring needs `results` (index-aligned with whatever's on
  // screen right now) resolved at *click* time, not capture time — done via
  // results_list's own row-activated-equivalent below instead of per-row
  // signal_clicked() connections, since Gtk::ListBox already gives a
  // reliable index via get_index() the same way LibraryView's own rows do.
  results_list->signal_row_activated().connect([results, add_station](Gtk::ListBoxRow* row) {
    if (!row)
      return;
    int index = row->get_index();
    if (index < 0 || static_cast<size_t>(index) >= results->size())
      return;
    add_station((*results)[static_cast<size_t>(index)]);
  });
  // Rows aren't buttons themselves, but ListBox rows are activatable by
  // default on click — the per-row add_button above is purely a visual
  // affordance (same "the whole row already does this" pattern the rest of
  // the app uses for is_container rows' chevron).
  results_list->set_activate_on_single_click(true);

  RadioBrowserService::Instance().FetchCountries(
      [country_model, countrycodes](std::vector<RadioBrowserCountry> countries) {
        std::sort(countries.begin(), countries.end(),
                  [](const RadioBrowserCountry& a, const RadioBrowserCountry& b) { return a.name < b.name; });
        for (const RadioBrowserCountry& country : countries)
        {
          if (country.countrycode.empty())
            continue;  // no usable filter value for this entry — skip it
          country_model->append(country.name + " (" + std::to_string(country.station_count) + ")");
          countrycodes->push_back(country.countrycode);
        }
      });

  auto* manual_expander = Gtk::make_managed<Gtk::Expander>("Manuell eingeben (Name und Stream-URL)");
  auto* manual_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  manual_box->set_margin_top(6);

  auto* title_label = Gtk::make_managed<Gtk::Label>("Name");
  title_label->set_halign(Gtk::Align::START);
  manual_box->append(*title_label);
  auto* title_entry = Gtk::make_managed<Gtk::Entry>();
  manual_box->append(*title_entry);

  auto* url_label = Gtk::make_managed<Gtk::Label>("Stream-URL");
  url_label->set_halign(Gtk::Align::START);
  manual_box->append(*url_label);
  auto* url_entry = Gtk::make_managed<Gtk::Entry>();
  url_entry->set_placeholder_text("http://...");
  manual_box->append(*url_entry);

  auto* manual_add_button = Gtk::make_managed<Gtk::Button>("Hinzufügen");
  manual_add_button->set_halign(Gtk::Align::END);
  manual_add_button->signal_clicked().connect([this, dialog, title_entry, url_entry] {
    std::string title = title_entry->get_text();
    std::string url = url_entry->get_text();
    if (title.empty() || url.empty())
      return;
    backend_->AddRadioStation(title, url);
    backend_->BrowseLibraryAsync(library_stack_.back().first);
    dialog->close();
  });
  manual_box->append(*manual_add_button);

  manual_expander->set_child(*manual_box);
  content->append(*manual_expander);

  auto* close_button = Gtk::make_managed<Gtk::Button>("Schließen");
  close_button->set_halign(Gtk::Align::END);
  close_button->set_margin_top(6);
  close_button->signal_clicked().connect([dialog] { dialog->close(); });
  content->append(*close_button);

  dialog->set_child(*content);
  dialog->present();
}

void GnomosWindow::ShowRadioMprisSettingsDialog(unsigned index)
{
  if (index >= current_library_entries_.size())
    return;
  const LibraryEntry& entry = current_library_entries_[index];
  // Shouldn't happen while browsing "R:0/0" (BrowseLibraryAsync() always
  // populates stream_uri there) — guards against an out-of-sync index
  // rather than assuming the caller got it right.
  if (entry.stream_uri.empty())
    return;

  std::string stream_uri = entry.stream_uri;
  RadioMprisSettings settings = backend_->GetRadioMprisSettings(stream_uri);

  auto* dialog = new Gtk::Window();
  dialog->set_title("Benachrichtigungen: " + (entry.title.empty() ? "Radiosender" : entry.title));
  dialog->set_transient_for(*this);
  dialog->set_modal(true);
  dialog->set_default_size(420, -1);

  // A plain Gtk::Window (unlike Adw::Dialog/AdwPreferencesDialog, used
  // everywhere else in this file) has no built-in Escape-to-close — add it
  // explicitly, same as cancel_button below.
  auto escape_controller = Gtk::EventControllerKey::create();
  escape_controller->signal_key_pressed().connect(
      [dialog](guint keyval, guint, Gdk::ModifierType) {
        if (keyval != GDK_KEY_Escape)
          return false;
        dialog->close();
        return true;
      },
      false);
  dialog->add_controller(escape_controller);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* enabled_check = Gtk::make_managed<Gtk::CheckButton>("Titelwechsel für diesen Sender melden");
  enabled_check->set_active(settings.mpris_enabled);
  content->append(*enabled_check);

  auto* regex_label = Gtk::make_managed<Gtk::Label>("Regex-Filter (optional)");
  regex_label->set_halign(Gtk::Align::START);
  regex_label->set_margin_top(6);
  content->append(*regex_label);

  auto* regex_entry = Gtk::make_managed<Gtk::Entry>();
  regex_entry->set_text(settings.regex);
  regex_entry->set_placeholder_text("z. B. .+ / .+");
  regex_entry->set_activates_default(true);
  content->append(*regex_entry);

  // Explains both halves of RadioContentFilter's own filtering in one
  // line: the regex decides what counts as a real song (ad text that
  // doesn't match is ignored), and only a genuinely new match is reported
  // — a repeat of the same song, or an ad in between, doesn't retrigger
  // MPRIS clients like GNOME Shell's media notification, and doesn't add
  // a spurious entry to "Verlauf" either.
  auto* help_label = Gtk::make_managed<Gtk::Label>(
      "Nur Inhalte, die zu diesem Muster passen, werden an MPRIS und den "
      "Verlauf übermittelt — Werbung und Senderkennungen dazwischen werden "
      "ignoriert. Leer = alles wird übermittelt.");
  help_label->set_halign(Gtk::Align::START);
  help_label->set_wrap(true);
  help_label->add_css_class("caption");
  help_label->add_css_class("dim-label");
  content->append(*help_label);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>("Abbrechen");
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* save_button = Gtk::make_managed<Gtk::Button>("Speichern");
  save_button->add_css_class("suggested-action");
  auto do_save = [this, dialog, enabled_check, regex_entry, stream_uri] {
    RadioMprisSettings new_settings;
    new_settings.mpris_enabled = enabled_check->get_active();
    new_settings.regex = regex_entry->get_text().raw();
    backend_->SetRadioMprisSettings(stream_uri, new_settings);
    dialog->close();
  };
  save_button->signal_clicked().connect(do_save);
  regex_entry->signal_activate().connect(do_save);
  button_box->append(*cancel_button);
  button_box->append(*save_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->set_default_widget(*save_button);
  dialog->signal_hide().connect([dialog] { delete dialog; });
  dialog->present();
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
