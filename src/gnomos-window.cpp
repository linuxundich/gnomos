// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnomos-window.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/texture.h>
#include <giomm/application.h>
#include <giomm/asyncresult.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/notification.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/bytes.h>
#include <glibmm/checksum.h>
#include <glibmm/error.h>
#include <glibmm/keyfile.h>
#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/expander.h>
#include <gtkmm/expression.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/gridview.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/linkbutton.h>
#include <gtkmm/listview.h>
#include <gtkmm/range.h>
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
#include "window-helpers.h"
#include "widgets/art-cache.h"
#include "widgets/artist-info-fetcher.h"
#include "widgets/cover-thumbnail.h"
#include "widgets/dialog-shell.h"
#include "widgets/http-fetch.h"
#include "widgets/lastfm-scrobbler.h"
#include "widgets/listenbrainz-scrobbler.h"
#include "widgets/lyrics-fetcher.h"
#include "widgets/radio-browser-service.h"

namespace gnomos
{

// The C++ object wraps an AdwApplicationWindow created by libadwaita —
// gtkmm has no Adw:: classes (see ARCHITECTURE.md), but an
// AdwApplicationWindow *is* a GtkApplicationWindow, so the protected
// "wrap this instance" constructor accepts it. It's what makes every
// AdwDialog open inside this window instead of as a window of its own.
GnomosWindow::GnomosWindow(Gtk::Application& app)
: Gtk::ApplicationWindow(GTK_APPLICATION_WINDOW(adw_application_window_new(app.gobj())))
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
  sidebar_toggle_button_.set_tooltip_text(_("Show/Hide Sections"));
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
  adw_button_content_set_label(ADW_BUTTON_CONTENT(room_button_content_), _("No Room"));
  room_button_.set_child(*Glib::wrap(room_button_content_));
  room_button_.set_tooltip_text(_("Choose Room"));
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
  // Reached two ways that both need real actions to forward to rather than
  // wiring up their own playback calls directly: GnomosApplication's own
  // "app."-scoped notification-* actions (see its own comment for why
  // those have to be app-level, not win-level, to be reachable from a
  // notification button at all), and GlobalShortcutsService's Activated
  // handler (a global shortcut fires even while the window is hidden, so
  // it needs a real action to activate rather than a UI signal only
  // player_bar_'s own buttons emit). play-pause mirrors the same toggle
  // logic player_bar_'s own play/pause button already uses.
  add_action("play-pause", sigc::mem_fun(*this, &GnomosWindow::TogglePlayPause));
  add_action("next", [this] { backend_->Next(); });
  add_action("previous", [this] { backend_->Previous(); });
  add_action("play-stream", sigc::mem_fun(*this, &GnomosWindow::ShowPlayStreamDialog));
  add_action("mute-everywhere", [this] {
    backend_->MuteAllRoomsAsync(true);
    ShowToast(_("All rooms muted"));
  });
  add_action("export-radio-favorites", sigc::mem_fun(*this, &GnomosWindow::ExportRadioFavorites));
  add_action("import-radio-favorites", sigc::mem_fun(*this, &GnomosWindow::ImportRadioFavorites));
  add_action("scenes", sigc::mem_fun(*this, &GnomosWindow::ShowScenesDialog));
  add_action("import-m3u-playlist", sigc::mem_fun(*this, &GnomosWindow::ImportM3uPlaylist));
  add_action("mini-player", sigc::mem_fun(*this, &GnomosWindow::ShowMiniPlayerWindow));
  add_action("refresh-library-index", [this] {
    backend_->RefreshLibraryIndexAsync();
    ShowToast(_("Updating library…"));
  });
  // The window's own close button just hides it when run_in_background_ is
  // on (see OnCloseRequest()) — this is the one reachable way to actually
  // terminate Gnomos in that case.
  add_action("quit", sigc::mem_fun(*this, &GnomosWindow::QuitApplication));
  // Targets of the accelerators GnomosApplication::on_startup() registers
  // (Ctrl+W, Ctrl+F, F9, ...) — see its comment there.
  add_action("close", [this] { close(); });
  add_action("search-library", [this] { ShowLibrarySearchDialog(); });
  add_action("jump-to-current", [this] {
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), "queue");
    queue_view_.ScrollToCurrent();
  });
  add_action("toggle-sidebar", [this] {
    if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(split_view_)))
      sidebar_toggle_button_.set_active(!sidebar_toggle_button_.get_active());
  });
  add_action("toggle-now-playing",
             [this] { SetNowPlayingOpen(!adw_bottom_sheet_get_open(ADW_BOTTOM_SHEET(bottom_sheet_))); });
  add_action("volume-up", [this] { StepVolume(+5); });
  add_action("volume-down", [this] { StepVolume(-5); });
  add_action("seek-forward", [this] { SeekRelative(+10); });
  add_action("seek-backward", [this] { SeekRelative(-10); });
  primary_menu_ = Gio::Menu::create();
  primary_menu_->append(_("Play Stream…"), "win.play-stream");
  primary_menu_->append(_("Mute Everywhere"), "win.mute-everywhere");
  primary_menu_->append(_("Scenes…"), "win.scenes");
  primary_menu_->append(_("Export Radio Station Favorites…"), "win.export-radio-favorites");
  primary_menu_->append(_("Import Radio Station Favorites…"), "win.import-radio-favorites");
  primary_menu_->append(_("Import M3U/PLS Playlist…"), "win.import-m3u-playlist");
  primary_menu_->append(_("Update Library"), "win.refresh-library-index");
  primary_menu_->append(_("Mini Player…"), "win.mini-player");
  primary_menu_->append(_("Preferences"), "win.settings");
  primary_menu_->append(_("Keyboard Shortcuts"), "win.shortcuts");
  primary_menu_->append(_("About Gnomos"), "win.about");
  primary_menu_->append(_("Quit Gnomos"), "win.quit");
  primary_menu_button_.set_icon_name("open-menu-symbolic");
  primary_menu_button_.set_tooltip_text(_("Main Menu"));
  primary_menu_button_.set_menu_model(primary_menu_);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(primary_menu_button_.gobj()));
  // Backgrounding (see OnCloseRequest()) hides this same window rather than
  // destroying it, so re-presenting it later remaps the very GtkPopoverMenu
  // built above instead of a fresh one — confirmed live that this popover's
  // items all come back permanently insensitive after such a remap (every
  // "win.*" item greyed out and unclickable, while plain signal-connected
  // buttons elsewhere in the window keep working fine), even though the
  // "win" actions themselves are untouched. Reassigning the same menu model
  // forces GtkMenuButton to rebuild its popover from scratch, which
  // re-establishes the action binding.
  signal_map().connect([this] { primary_menu_button_.set_menu_model(primary_menu_); });

  activity_spinner_ = adw_spinner_new();
  gtk_widget_set_margin_start(activity_spinner_, 6);
  gtk_widget_set_margin_end(activity_spinner_, 6);
  gtk_widget_set_tooltip_text(activity_spinner_, _("Waiting for the Sonos system…"));
  // Starts hidden — UpdateActivitySpinner() is the only thing that ever
  // shows it, and a hidden widget can't be hovered, so (unlike the old
  // Gtk::Spinner, which stayed hoverable even while stopped) the tooltip
  // above never needs its own separate show/hide dance.
  gtk_widget_set_visible(activity_spinner_, false);
  refresh_button_.set_icon_name("view-refresh-symbolic");
  refresh_button_.set_tooltip_text(_("Search for Sonos Devices"));
  refresh_button_.signal_clicked().connect(sigc::mem_fun(*this, &GnomosWindow::OnRefreshClicked));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(refresh_button_.gobj()));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), activity_spinner_);

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
  grouping_scroller->set_margin_top(6);
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
  auto* group_all_button = Gtk::make_managed<Gtk::Button>(_("Group All Rooms"));
  group_all_button->add_css_class("flat");
  group_all_button->set_tooltip_text(
      _("Adds every free room — rooms already grouped with another room stay as they are"));
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
  auto* ungroup_all_button = Gtk::make_managed<Gtk::Button>(_("Ungroup"));
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
  // No separator here — grouping_scroller's own "boxed-list" content
  // (grouping_list_box_) already reads as its own distinct rounded card
  // right below these two buttons; an explicit line on top of that just
  // looked like clutter (reported live). grouping_scroller's own
  // margin_top gives it breathing room instead.
  grouping_box->append(*grouping_scroller);
  grouping_popover_.set_child(*grouping_box);
  grouping_popover_.signal_show().connect([this] {
    // Immediate rebuild with whatever's already cached, then a live
    // refresh — see RefreshGroupVolumesAsync()'s own comment for why the
    // sliders/master fader need this rather than player_'s own (fixed at
    // zone-selection-time) subscribed volume state.
    RebuildGroupingPopover();
    backend_->RefreshGroupVolumesAsync();
  });

  grouping_button_.set_icon_name("audio-speakers-symbolic");
  grouping_button_.set_tooltip_text(_("Group Rooms"));
  grouping_button_.set_popover(grouping_popover_);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(grouping_button_.gobj()));

  // --- Input source popover (line-in / digital-in on the current zone) ---
  auto* input_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  input_box->set_margin_top(6);
  input_box->set_margin_bottom(6);
  input_box->set_margin_start(6);
  input_box->set_margin_end(6);
  auto* line_in_button = Gtk::make_managed<Gtk::Button>(_("Line In"));
  line_in_button->add_css_class("flat");
  line_in_button->signal_clicked().connect([this] {
    backend_->PlayLineIn();
    input_popover_.popdown();
  });
  auto* digital_in_button = Gtk::make_managed<Gtk::Button>(_("Digital In"));
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
  input_button_.set_tooltip_text(_("Input"));
  input_button_.set_popover(input_popover_);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(input_button_.gobj()));

  // --- Sleep timer popover ---
  auto* sleep_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  sleep_box->set_margin_top(6);
  sleep_box->set_margin_bottom(6);
  sleep_box->set_margin_start(6);
  sleep_box->set_margin_end(6);
  sleep_timer_status_label_.add_css_class("dimmed");
  sleep_timer_status_label_.add_css_class("caption");
  sleep_box->append(sleep_timer_status_label_);
  sleep_box->append(*Gtk::make_managed<Gtk::Separator>());
  // (label, minutes) — 0 cancels an active timer.
  static const std::array<std::pair<const char*, unsigned>, 6> kSleepPresets = {{
      {_("Off"), 0},
      {_("15 Minutes"), 15},
      {_("30 Minutes"), 30},
      {_("45 Minutes"), 45},
      {_("60 Minutes"), 60},
      {_("90 Minutes"), 90},
  }};
  for (const auto& [label, minutes] : kSleepPresets)
  {
    auto* preset_button = Gtk::make_managed<Gtk::Button>(label);
    preset_button->add_css_class("flat");
    preset_button->signal_clicked().connect([this, minutes] {
      // The device only ever reports the time remaining; remember the
      // full length so the ring can show the share that's left.
      sleep_total_seconds_ = minutes * 60;
      backend_->SetSleepTimer(minutes * 60);
      sleep_timer_popover_.popdown();
    });
    sleep_box->append(*preset_button);
  }
  sleep_timer_popover_.set_child(*sleep_box);

  sleep_timer_button_.set_icon_name("weather-clear-night-symbolic");
  sleep_timer_button_.set_tooltip_text(_("Sleep Timer"));
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
    label->add_css_class("dimmed");
    sound_box->append(*label);
  };

  add_sound_label(_("Bass"));
  bass_scale_.set_range(-10, 10);
  bass_scale_.set_digits(0);
  bass_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetBass(static_cast<int8_t>(bass_scale_.get_value()));
  });
  sound_box->append(bass_scale_);

  add_sound_label(_("Treble"));
  treble_scale_.set_range(-10, 10);
  treble_scale_.set_digits(0);
  treble_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetTreble(static_cast<int8_t>(treble_scale_.get_value()));
  });
  sound_box->append(treble_scale_);

  auto* reset_eq_button = Gtk::make_managed<Gtk::Button>(_("Reset Bass/Treble"));
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

  add_sound_label(_("Sub Level"));
  sub_gain_scale_.set_range(-15, 15);
  sub_gain_scale_.set_digits(0);
  sub_gain_scale_.signal_value_changed().connect([this] {
    if (!suppress_sound_signals_)
      backend_->SetSubGain(static_cast<int16_t>(sub_gain_scale_.get_value()));
  });
  sound_box->append(sub_gain_scale_);

  sound_box->append(*Gtk::make_managed<Gtk::Separator>());

  auto* loudness_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* loudness_label = Gtk::make_managed<Gtk::Label>(_("Loudness"));
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
  auto* nightmode_label = Gtk::make_managed<Gtk::Label>(_("Night Mode"));
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
  auto* output_fixed_label = Gtk::make_managed<Gtk::Label>(_("Fixed Volume (Line Out)"));
  output_fixed_label->set_halign(Gtk::Align::START);
  output_fixed_label->set_hexpand(true);
  output_fixed_row->append(*output_fixed_label);
  output_fixed_switch_.set_valign(Gtk::Align::CENTER);
  output_fixed_switch_.set_tooltip_text(
      _("Ignores its own volume control — for connecting to an amplifier with its own volume"));
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
  auto* advanced_toggle_label = Gtk::make_managed<Gtk::Label>(_("Advanced"));
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
  auto* autoplay_label = Gtk::make_managed<Gtk::Label>(_("Autoplay (Line In)"));
  autoplay_label->set_halign(Gtk::Align::START);
  autoplay_label->set_hexpand(true);
  autoplay_row->append(*autoplay_label);
  autoplay_switch_.set_valign(Gtk::Align::CENTER);
  autoplay_switch_.set_tooltip_text(
      _("Starts playing here automatically as soon as a line-in signal reaches this device"));
  autoplay_switch_.signal_state_set().connect(
      [this](bool state) -> bool {
        backend_->SetAutoplay(state);
        return true;
      },
      false);
  autoplay_row->append(autoplay_switch_);
  advanced_box->append(*autoplay_row);

  auto* autoplay_volume_switch_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* autoplay_volume_switch_label = Gtk::make_managed<Gtk::Label>(_("Custom Autoplay Volume"));
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

  auto* autoplay_volume_label = Gtk::make_managed<Gtk::Label>(_("Autoplay Volume"));
  autoplay_volume_label->set_halign(Gtk::Align::START);
  autoplay_volume_label->add_css_class("caption");
  autoplay_volume_label->add_css_class("dimmed");
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
  auto* led_label = Gtk::make_managed<Gtk::Label>(_("Status Light"));
  led_label->set_halign(Gtk::Align::START);
  led_label->set_hexpand(true);
  led_row->append(*led_label);
  auto* led_on_button = Gtk::make_managed<Gtk::Button>(_("On"));
  led_on_button->signal_clicked().connect([this] { backend_->SetLedState(true); });
  led_row->append(*led_on_button);
  auto* led_off_button = Gtk::make_managed<Gtk::Button>(_("Off"));
  led_off_button->signal_clicked().connect([this] { backend_->SetLedState(false); });
  led_row->append(*led_off_button);
  advanced_box->append(*led_row);

  advanced_revealer->set_child(*advanced_box);

  sound_popover_.set_child(*sound_box);

  sound_button_.set_icon_name("multimedia-volume-control-symbolic");
  sound_button_.set_tooltip_text(_("Sound"));
  sound_button_.set_popover(sound_popover_);
  sound_popover_.signal_show().connect([this] { backend_->RefreshSoundSettingsAsync(); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(sound_button_.gobj()));


  // --- Room/zone list — now room_popover_'s content instead of a
  // permanent sidebar (see room_button_'s own comment). Sized like a
  // popover (fixed width, capped+scrollable height), the same pattern
  // grouping_popover_'s own room list already uses, rather than the
  // set_vexpand(true)/set_min_content_width() sizing appropriate for a
  // permanent panel this used to have. ---
  zones_placeholder_.set_text(_("No Sonos devices found.\nClick Refresh."));
  zones_placeholder_.set_wrap(true);
  zones_placeholder_.set_justify(Gtk::Justification::CENTER);
  zones_placeholder_.add_css_class("dimmed");
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
  // Widened from the original 260 (a single-line name + Gen1 badge + one
  // info button) to fit the extra play/pause button and, more importantly,
  // the now-playing subtitle line added below — at 260, a grouped zone's
  // own display_name (e.g. "Arbeitszimmer + 1", the only visual indication
  // this app has for which rooms are currently grouped) was getting
  // ellipsized away entirely, confirmed live. Height raised to match —
  // each row is now two lines tall, so the same 400px ceiling fit
  // noticeably fewer rooms before scrolling.
  zones_scroller_.set_size_request(320, -1);
  zones_scroller_.set_max_content_height(480);
  zones_scroller_.set_propagate_natural_height(true);
  room_popover_.set_child(zones_scroller_);
  // Opens towards the window's interior: room_button_ sits at the far left
  // of the header bar, and a popover centered on it reached past the
  // window's left edge.
  room_popover_.set_halign(Gtk::Align::START);
  zones_list_box_.add_css_class("room-list");
  // Live per-room now-playing status (see OnZonesChanged()'s own comment
  // on room_np) is only ever refreshed while this popover is actually
  // open — an immediate fetch on open, then a light repeating poll so it
  // stays current for as long as the user leaves it open, stopped the
  // moment it closes. No point tracking every room's live state
  // continuously in the background when nothing's showing it.
  room_popover_.signal_show().connect([this] {
    backend_->RefreshAllRoomNowPlayingAsync();
    room_now_playing_poll_connection_.disconnect();
    room_now_playing_poll_connection_ = Glib::signal_timeout().connect(
        [this] {
          backend_->RefreshAllRoomNowPlayingAsync();
          return true;
        },
        4000);
  });
  room_popover_.signal_closed().connect([this] { room_now_playing_poll_connection_.disconnect(); });

  // --- Section sidebar (Warteschlange/Favoriten/Alarme/Verlauf/
  // Bibliothek) — replaces the AdwViewSwitcher this app used to have as a
  // top tab bar, styled after noson-app's own left-hand navigation (see
  // nav_sidebar_'s own comment for why AdwSidebar replaced a hand-rolled
  // Gtk::ListBox). Built from the same (page-name, title, icon) tuples
  // view_stack_'s pages themselves use, so the two can never drift apart. ---
  static const std::array<std::tuple<const char*, const char*, const char*>, 5> kNavPages = {{
      {"queue", _("Queue"), "view-list-symbolic"},
      {"favorites", _("Favorites"), "starred-symbolic"},
      {"alarms", _("Alarms"), "alarm-symbolic"},
      {"history", _("History"), "document-open-recent-symbolic"},
      {"library", _("Library"), "folder-music-symbolic"},
  }};
  nav_sidebar_ = adw_sidebar_new();
  g_signal_connect_data(nav_sidebar_, "notify::selected", G_CALLBACK(OnNavSidebarSelectedChanged), nullptr, nullptr,
                         GConnectFlags(0));
  AdwSidebarSection* static_nav_section = adw_sidebar_section_new();
  adw_sidebar_append(ADW_SIDEBAR(nav_sidebar_), static_nav_section);
  for (const auto& [name, title, icon] : kNavPages)
  {
    AdwSidebarItem* item = adw_sidebar_item_new(title);
    adw_sidebar_item_set_icon_name(item, icon);
    adw_sidebar_section_append(static_nav_section, item);
#if ADW_CHECK_VERSION(1, 10, 0)
    // libadwaita 1.10 (GNOME 51) lets a sidebar item carry a suffix widget:
    // the queue shows the playing indicator there, visible from any page.
    if (std::string(name) == "queue")
    {
      queue_nav_indicator_ = Gtk::make_managed<PlayingIndicator>();
      queue_nav_indicator_->add_css_class("accent");
      queue_nav_indicator_->set_visible(false);
      adw_sidebar_item_set_suffix(item, GTK_WIDGET(queue_nav_indicator_->gobj()));
    }
#endif

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
      SetSidebarItemAction(item, [this, page_name] {
        library_stack_.clear();
        library_stack_.push_back({"", _("Library")});
        // Cleared and retitled synchronously, before switching the page
        // into view — BrowseLibraryAsync() below is a real network round
        // trip, and library_view_ otherwise keeps showing whatever level
        // it last held (e.g. a deeply browsed album's own tracks) for
        // that entire round trip once the page is already visible,
        // flashing stale content. Confirmed live.
        library_view_.ShowLoading();
        library_view_.SetLevelTitle(_("Library"));
        backend_->BrowseLibraryAsync("");
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), page_name.c_str());
      });
    }
    else
    {
      SetSidebarItemAction(item, [this, page_name] {
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), page_name.c_str());
      });
    }
  }
  library_nav_section_ = adw_sidebar_section_new();
  g_object_ref(library_nav_section_);
  // Deliberately untitled — the static "Bibliothek" row directly above
  // already establishes this section's context; titling it "Bibliothek"
  // again read as the label appearing twice in a row (a clickable item
  // immediately followed by an unclickable section header repeating the
  // same word).
  services_nav_section_ = adw_sidebar_section_new();
  g_object_ref(services_nav_section_);
  adw_sidebar_section_set_title(services_nav_section_, _("Services"));

  // --- Content: queue/favorites/alarms/library pages. The Now Playing
  // panel (player_bar_) is docked separately, as a bottom bar spanning
  // the whole window rather than living inside this content area — see
  // root_box further down this constructor. ---
  view_stack_ = adw_view_stack_new();
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(queue_view_.gobj()), "queue",
                                       _("Queue"), "view-list-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(favorites_view_.gobj()), "favorites",
                                       _("Favorites"), "starred-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(alarms_view_.gobj()), "alarms",
                                       _("Alarms"), "alarm-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(history_view_.gobj()), "history",
                                       _("History"), "document-open-recent-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_), GTK_WIDGET(library_view_.gobj()), "library",
                                       _("Library"), "folder-music-symbolic");

  favorites_view_.signal_item_activated().connect([this](unsigned index) { backend_->PlayFavorite(index); });
  favorites_view_.signal_add_to_queue_requested().connect([this](unsigned index) {
    backend_->AddFavoriteToQueue(index);
    ShowToast(_("Added to queue"));
  });
  favorites_view_.signal_play_next_requested().connect([this](unsigned index) {
    backend_->PlayFavoriteNext(index);
    ShowToast(_("Added to play next"));
  });
  favorites_view_.signal_delete_requested().connect(
      sigc::mem_fun(*this, &GnomosWindow::ShowDeleteFavoriteConfirmDialog));
  favorites_view_.signal_play_all_requested().connect([this] { backend_->PlayAllFavoritesAsync(); });
  favorites_view_.signal_queue_all_requested().connect([this] {
    backend_->AddAllFavoritesToQueue();
    ShowToast(_("Added to queue"));
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
    // Only the artist branch has an unambiguous scope to search in — a
    // fallback title could be anything (e.g. a radio station name), so
    // that branch keeps the old "search the current level" behavior.
    ShowLibrarySearchDialog(entry.artist.empty() ? entry.title : entry.artist,
                             entry.artist.empty() ? "" : "A:ALBUMARTIST");
  });

  library_stack_.push_back({"", _("Library")});
  library_view_.SetLevelTitle(_("Library"));
  library_view_.SetBackVisible(false);
  library_view_.signal_entry_activated().connect(sigc::mem_fun(*this, &GnomosWindow::OnLibraryEntryActivated));
  library_view_.signal_back_requested().connect(sigc::mem_fun(*this, &GnomosWindow::OnLibraryBackRequested));
  library_view_.signal_search_requested().connect([this] { ShowLibrarySearchDialog(); });
  library_view_.signal_add_to_queue_requested().connect([this](unsigned index) {
    backend_->AddLibraryItemToQueue(index);
    ShowToast(_("Added to queue"));
  });
  library_view_.signal_play_next_requested().connect([this](unsigned index) {
    backend_->PlayLibraryItemNext(index);
    ShowToast(_("Added to play next"));
  });
  library_view_.signal_add_to_favorites_requested().connect([this](unsigned index) {
    backend_->AddLibraryItemToFavorites(index);
    ShowToast(_("Added to favorites"));
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
    ShowToast(_("Added to queue"));
  });
  // Re-renders the already-fetched current_library_entries_ with the new
  // preference — no need to ask NosonBackend for anything again, this is
  // purely a local rendering choice.
  library_view_.signal_view_mode_toggled().connect([this](bool grid) {
    SetPreferGridView(grid);
    OnLibraryChanged();
  });

  player_bar_.signal_play_pause().connect(sigc::mem_fun(*this, &GnomosWindow::TogglePlayPause));
  player_bar_.signal_next().connect([this] { backend_->Next(); });
  player_bar_.signal_shuffle_clicked().connect([this] { backend_->ToggleShuffle(); });
  player_bar_.signal_repeat_clicked().connect([this] { backend_->ToggleRepeat(); });
  player_bar_.signal_add_to_favorites_clicked().connect([this] { backend_->AddCurrentTrackToFavorites(); });
  player_bar_.signal_art_clicked().connect([this] { SetNowPlayingOpen(true); });
  player_bar_.signal_previous().connect([this] { backend_->Previous(); });
  player_bar_.signal_volume_changed().connect(
      [this](double value) { backend_->SetVolume(static_cast<uint8_t>(value)); });
  player_bar_.signal_mute_toggled().connect([this](bool muted) { backend_->SetMuted(muted); });
  player_bar_.signal_seek_requested().connect([this](unsigned seconds) { backend_->SeekAsync(seconds); });
  player_bar_.signal_art_ready().connect(sigc::mem_fun(*this, &GnomosWindow::OnNotificationArtReady));

  now_playing_view_.signal_play_pause().connect(sigc::mem_fun(*this, &GnomosWindow::TogglePlayPause));
  now_playing_view_.signal_next().connect([this] { backend_->Next(); });
  now_playing_view_.signal_previous().connect([this] { backend_->Previous(); });
  now_playing_view_.signal_shuffle_clicked().connect([this] { backend_->ToggleShuffle(); });
  now_playing_view_.signal_repeat_clicked().connect([this] { backend_->ToggleRepeat(); });
  now_playing_view_.signal_seek_requested().connect([this](unsigned seconds) { backend_->SeekAsync(seconds); });
  now_playing_view_.signal_volume_changed().connect(
      [this](double value) { backend_->SetVolume(static_cast<uint8_t>(value)); });
  now_playing_view_.signal_mute_toggled().connect([this](bool muted) { backend_->SetMuted(muted); });
  now_playing_view_.signal_close_requested().connect([this] { SetNowPlayingOpen(false); });
  now_playing_view_.signal_queue_item_activated().connect([this](unsigned index) { backend_->PlayQueueItem(index); });
  now_playing_view_.signal_search_artist().connect([this](std::string artist) {
    SetNowPlayingOpen(false);
    ShowLibrarySearchDialog(artist, "A:ALBUMARTIST");
  });
  now_playing_view_.signal_search_album().connect([this](std::string album) {
    SetNowPlayingOpen(false);
    ShowLibrarySearchDialog(album, "A:ALBUM");
  });
  now_playing_view_.signal_artist_info().connect([this](std::string artist) { ShowArtistInfoDialog(artist); });
  now_playing_view_.signal_palette_changed().connect(sigc::mem_fun(*this, &GnomosWindow::ApplyCoverTint));
  now_playing_view_.signal_cover_changed().connect([this](const Glib::RefPtr<Gdk::Texture>& texture) {
    if (mini_player_window_)
      mini_player_window_->SetCover(texture, now_playing_view_.palette());
  });
  // The tinted button's lightness depends on light/dark — recompute.
  g_signal_connect_swapped(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(+[](GnomosWindow* self) {
                             self->ApplyCoverTint(self->now_playing_view_.palette());
                           }),
                           this);

  queue_view_.signal_item_activated().connect([this](unsigned index) { backend_->PlayQueueItem(index); });
  queue_view_.signal_item_remove_requested().connect([this](unsigned index) { backend_->RemoveQueueItem(index); });
  queue_view_.signal_remove_selected_requested().connect(
      sigc::mem_fun(*this, &GnomosWindow::ShowRemoveSelectedQueueItemsConfirmDialog));
  queue_view_.signal_clear_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowClearQueueConfirmDialog));
  queue_view_.signal_save_playlist_requested().connect(sigc::mem_fun(*this, &GnomosWindow::ShowSavePlaylistDialog));
  queue_view_.signal_reorder_requested().connect(
      [this](unsigned from, unsigned to) { backend_->ReorderQueueItem(from, to); });

  // --- Section sidebar as an AdwOverlaySplitView, not a plain Gtk::Paned —
  // lets it collapse behind sidebar_toggle_button_ on narrow windows (see
  // the AdwBreakpoint below), the adaptive behavior the README used to
  // list as a known gap. min/max width mirror the fixed 240px paned
  // position this replaced. nav_sidebar_ (built above) is the sidebar now;
  // view_stack_ is used directly as the content, since content_box_ no
  // longer exists — it only ever wrapped view_stack_ together with the
  // now-removed AdwViewSwitcher.
  split_view_ = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_view_), nav_sidebar_);
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
  //
  // The bar is an AdwBottomSheet's bottom bar: clicking it (anywhere but
  // on one of its own controls) or its cover slides up the Now Playing
  // sheet, which takes the bar's place until it's closed again.
  bottom_sheet_ = adw_bottom_sheet_new();
  Gtk::Widget* wrapped_breakpoint_bin = Glib::wrap(breakpoint_bin);
  wrapped_breakpoint_bin->set_vexpand(true);
  adw_bottom_sheet_set_content(ADW_BOTTOM_SHEET(bottom_sheet_), breakpoint_bin);
  adw_bottom_sheet_set_bottom_bar(ADW_BOTTOM_SHEET(bottom_sheet_), GTK_WIDGET(player_bar_.gobj()));
  adw_bottom_sheet_set_sheet(ADW_BOTTOM_SHEET(bottom_sheet_), GTK_WIDGET(now_playing_view_.gobj()));
  adw_bottom_sheet_set_show_drag_handle(ADW_BOTTOM_SHEET(bottom_sheet_), TRUE);
  adw_bottom_sheet_set_modal(ADW_BOTTOM_SHEET(bottom_sheet_), TRUE);
  // The lyrics lookup only runs while the sheet is open (it sends the
  // track to LRCLIB) — catch up on opening.
  g_signal_connect_swapped(bottom_sheet_, "notify::open", G_CALLBACK(+[](GnomosWindow* self) {
                             self->RequestLyricsForCurrentTrack();
                           }),
                           this);

  // --- Toast overlay wraps everything, for error feedback ---
  toast_overlay_ = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay_), bottom_sheet_);
  // AdwApplicationWindow takes a single content widget (no set_titlebar()/
  // set_child()); the header bar sits on top of everything via a toolbar
  // view.
  GtkWidget* toolbar_view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar_);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), toast_overlay_);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(gobj()), toolbar_view);

  // --- Backend wiring ---
  backend_ = std::make_unique<NosonBackend>();
  backend_->signal_discovery_done().connect(sigc::mem_fun(*this, &GnomosWindow::OnDiscoveryDone));
  backend_->signal_busy_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnBusyChanged));
  backend_->signal_zones_changed().connect(sigc::mem_fun(*this, &GnomosWindow::OnZonesChanged));
  backend_->signal_room_now_playing_changed().connect(
      sigc::mem_fun(*this, &GnomosWindow::OnRoomNowPlayingChanged));
  backend_->signal_group_volumes_changed().connect([this] {
    if (grouping_popover_.get_visible())
      RebuildGroupingPopover();
  });
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
  zone_volume_ = std::make_unique<ZoneVolumeService>(*backend_);
  global_shortcuts_ = std::make_unique<GlobalShortcutsService>(*this);
  radio_history_filter_ = std::make_unique<RadioContentFilter>(*backend_);
  radio_lyrics_filter_ = std::make_unique<RadioContentFilter>(*backend_);

  pending_restore_room_uuid_ = LoadLastRoomUuid();

  LoadHistory();
  history_view_.SetItems(history_);

  LoadColorScheme();
  LoadNotificationSetting();
  LoadRunInBackgroundSetting();
  LoadLibraryViewPreference();
  LoadArtistImagesSetting();
  LoadLyricsSetting();
  LoadAppearanceSettings();
  LoadListenBrainzToken();
  LoadLastFmSettings();
  LoadScenes();
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

GnomosWindow::~GnomosWindow()
{
  g_object_unref(library_nav_section_);
  g_object_unref(services_nav_section_);
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

void GnomosWindow::UpdateRoomButtonLabel()
{
  if (!room_button_content_)
    return;
  for (const ZoneInfo& zone : current_zones_)
  {
    if (zone.group_id == selected_group_id_)
    {
      adw_button_content_set_label(ADW_BUTTON_CONTENT(room_button_content_), zone.display_name.c_str());
      now_playing_view_.SetRoomName(zone.display_name);
      return;
    }
  }
  adw_button_content_set_label(ADW_BUTTON_CONTENT(room_button_content_), _("No Room"));
  now_playing_view_.SetRoomName("");
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
  if (!ShouldReallyClose())
  {
    // Hide rather than close, so the process (and with it MPRIS control /
    // scrobbling) keeps running — GnomosApplication's signal_hide handler
    // checks ShouldReallyClose() itself before deleting the window, so this
    // intentional hide doesn't get mistaken for the window actually
    // closing. Re-launching Gnomos (or "Gnomos beenden" from the menu)
    // reaches it via GnomosApplication::on_activate()'s present().
    set_visible(false);
    return true;  // block the close, we handled it ourselves
  }
  // Really closing — GTK's own default close-request handling destroys the
  // window from here without emitting a "hide" signal first (confirmed
  // live: a signal_hide handler on GnomosApplication's side never fired for
  // this path, only for the backgrounding set_visible(false) above), so
  // this is the one reliable place to release the hold() GnomosApplication
  // took at startup — otherwise, now that anything holds the application
  // open at all, it would never quit on a real close.
  if (auto app = get_application())
    app->release();
  return false;  // don't block the close
}

void GnomosWindow::OnDiscoveryDone(bool ok)
{
  discovering_ = false;
  UpdateActivitySpinner();
  if (!ok)
    ShowToast(_("No Sonos device found on the network."));
}

void GnomosWindow::OnBusyChanged(bool busy)
{
  backend_busy_ = busy;
  UpdateActivitySpinner();
}

void GnomosWindow::UpdateActivitySpinner()
{
  if (discovering_ || backend_busy_)
  {
    // Debounced — TaskQueue's busy signal fires around *every* task with
    // nothing else queued at that moment, including routine background
    // polling nobody is actually waiting on (e.g. OnPositionTimerTick()'s
    // once-a-second RefreshPositionAsync() while a track plays), which
    // finishes near-instantly on a local network. Starting the spinner
    // immediately for every one of those made it flicker on and off
    // constantly even with nothing genuinely pending (reported live).
    // Only a task still running after this delay counts as worth showing.
    if (!gtk_widget_get_visible(activity_spinner_) && !spinner_show_delay_connection_.connected())
      spinner_show_delay_connection_ = Glib::signal_timeout().connect(
          [this] {
            gtk_widget_set_visible(activity_spinner_, true);
            return false;  // one-shot
          },
          300);
  }
  else
  {
    spinner_show_delay_connection_.disconnect();
    gtk_widget_set_visible(activity_spinner_, false);
  }
}

void GnomosWindow::OnZonesChanged()
{
  current_zones_ = backend_->Zones();

  zone_rows_.clear();
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

    // Same drag-handle convention QueueView's own rows use — dragging one
    // zone's row onto another merges every room currently in the dragged
    // zone into the target zone's group, an alternative to opening the
    // grouping popover and toggling each room's own switch there.
    auto* drag_handle = Gtk::make_managed<Gtk::Image>();
    drag_handle->set_from_icon_name("list-drag-handle-symbolic");
    drag_handle->add_css_class("dimmed");
    row_box->append(*drag_handle);

    // Icon-prefixed row, matching Euphonica's nav-list style
    // (https://github.com/htkhiem/euphonica) — its sidebar entries (Albums,
    // Artists, ...) are icon+label the same way.
    auto* room_icon = Gtk::make_managed<Gtk::Image>();
    room_icon->set_from_icon_name("audio-speakers-symbolic");
    room_icon->add_css_class("dimmed");
    row_box->append(*room_icon);
    // Takes the speaker icon's place while the room plays — see
    // UpdateZoneRowsNowPlaying().
    auto* room_indicator = Gtk::make_managed<PlayingIndicator>();
    room_indicator->add_css_class("accent");
    room_indicator->set_visible(false);
    row_box->append(*room_indicator);

    // Name + a live now-playing subtitle stacked vertically — lets the
    // switcher show "what's playing where" at a glance, matching
    // noson-app's own room list (Zones.qml), without needing to switch
    // into a room first. room_now_playing comes from NosonBackend's own
    // independent per-room polling (see room_popover_.signal_show()'s own
    // handler, in the constructor) — Gnomos otherwise only ever tracks
    // live transport state for the *selected* zone.
    auto* text_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    text_box->set_hexpand(true);
    text_box->set_valign(Gtk::Align::CENTER);

    auto* name_label = Gtk::make_managed<Gtk::Label>(zone.display_name);
    name_label->set_halign(Gtk::Align::START);
    name_label->set_ellipsize(Pango::EllipsizeMode::END);
    text_box->append(*name_label);

    // Always created, shown only once there's something to say — keeps
    // the widget around for UpdateZoneRowsNowPlaying() to update in place.
    auto* subtitle_label = Gtk::make_managed<Gtk::Label>();
    subtitle_label->set_halign(Gtk::Align::START);
    subtitle_label->set_ellipsize(Pango::EllipsizeMode::END);
    subtitle_label->add_css_class("dimmed");
    subtitle_label->add_css_class("caption");
    subtitle_label->set_visible(false);
    text_box->append(*subtitle_label);
    row_box->append(*text_box);

    if (zone.is_gen1)
    {
      auto* badge = Gtk::make_managed<Gtk::Label>(_("Gen 1"));
      badge->add_css_class("dimmed");
      badge->add_css_class("caption");
      row_box->append(*badge);
    }

    // Hidden until a live snapshot has actually arrived — no misleading
    // "nothing's playing" icon before the first RefreshAllRoomNowPlayingAsync()
    // tick has had a chance to complete.
    auto* play_pause_button = Gtk::make_managed<Gtk::Button>();
    play_pause_button->add_css_class("flat");
    play_pause_button->set_valign(Gtk::Align::CENTER);
    play_pause_button->set_visible(false);
    std::string coordinator_uuid = zone.coordinator_uuid;
    play_pause_button->signal_clicked().connect(
        [this, coordinator_uuid] { backend_->ToggleRoomPlayback(coordinator_uuid); });
    row_box->append(*play_pause_button);
    zone_rows_.push_back({zone.coordinator_uuid, subtitle_label, play_pause_button, room_icon, room_indicator});

    auto* info_button = Gtk::make_managed<Gtk::Button>();
    info_button->set_icon_name("dialog-information-symbolic");
    info_button->add_css_class("flat");
    info_button->set_valign(Gtk::Align::CENTER);
    info_button->set_tooltip_text(_("Device Info"));
    std::string group_id = zone.group_id;
    std::string zone_name = zone.display_name;
    info_button->signal_clicked().connect([this, group_id, zone_name] { ShowDeviceInfoDialog(group_id, zone_name); });
    row_box->append(*info_button);

    // Drag payload is the dragged zone's own group_id — the drop handler
    // below looks up every room currently sharing it (Rooms() has no
    // "give me this whole zone's members" call of its own, but filtering
    // by group_id is exactly that) and joins each one to the row it was
    // dropped on, same JoinRoomToZone() call the grouping popover's own
    // per-room switches use, just against an arbitrary target instead of
    // always the currently selected zone.
    auto drag_source = Gtk::DragSource::create();
    drag_source->set_actions(Gdk::DragAction::MOVE);
    drag_source->signal_prepare().connect(
        [group_id](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
          Glib::Value<Glib::ustring> value;
          value.init(Glib::Value<Glib::ustring>::value_type());
          value.set(group_id);
          return Gdk::ContentProvider::create(value);
        },
        false);
    row_box->add_controller(drag_source);

    std::string drop_coordinator_uuid = zone.coordinator_uuid;
    auto drop_target = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);
    drop_target->signal_drop().connect(
        [this, drop_coordinator_uuid, group_id](const Glib::ValueBase& value, double, double) -> bool {
          if (value.gobj()->g_type != Glib::Value<Glib::ustring>::value_type())
            return false;
          Glib::Value<Glib::ustring> str_value;
          str_value.init(value.gobj());
          std::string source_group_id = str_value.get().raw();
          if (source_group_id == group_id)
            return false;  // dropped on its own row — nothing to do
          for (const RoomInfo& room : backend_->Rooms())
            if (room.group_id == source_group_id)
              backend_->JoinRoomToZone(room.player_uuid, drop_coordinator_uuid);
          return true;
        },
        false);
    row_box->add_controller(drop_target);

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
  UpdateZoneRowsNowPlaying();

  // Covers the no-zones-at-all case: select_row() above (and the
  // OnZoneRowSelected() it triggers) never runs then, which would
  // otherwise leave room_button_'s label stuck on a stale room name.
  UpdateRoomButtonLabel();

  // A topology change (e.g. our own join/remove action taking effect) may
  // have happened while the grouping popover was open; keep it truthful.
  if (grouping_popover_.get_visible())
    RebuildGroupingPopover();
}

// Per-room now-playing snapshots arrive every 4 s while room_popover_ is
// open. Rebuilding the whole list for each of them (as this used to) reset
// hover and keyboard focus inside the open popover; now only the subtitle
// and play/pause button of each row change, and a real topology change
// still goes through the full OnZonesChanged() rebuild.
void GnomosWindow::OnRoomNowPlayingChanged()
{
  std::vector<ZoneInfo> zones = backend_->Zones();
  bool same_topology = zones.size() == current_zones_.size();
  for (size_t i = 0; same_topology && i < zones.size(); ++i)
    same_topology = zones[i].group_id == current_zones_[i].group_id &&
                    zones[i].coordinator_uuid == current_zones_[i].coordinator_uuid &&
                    zones[i].display_name == current_zones_[i].display_name;
  if (same_topology)
    UpdateZoneRowsNowPlaying();
  else
    OnZonesChanged();
}

void GnomosWindow::UpdateZoneRowsNowPlaying()
{
  for (const ZoneRowWidgets& row : zone_rows_)
  {
    RoomNowPlaying room_np = backend_->GetRoomNowPlaying(row.coordinator_uuid);
    std::string subtitle;
    if (room_np.valid)
      subtitle = room_np.state == TransportState::Playing   ? room_np.title
                 : room_np.state == TransportState::Paused ? _("Paused")
                                                            : "";
    row.subtitle->set_text(subtitle);
    row.subtitle->set_visible(!subtitle.empty());

    bool playing = room_np.valid && room_np.state == TransportState::Playing;
    row.icon->set_visible(!playing);
    row.indicator->set_visible(playing);
    row.indicator->SetPlaying(playing);
    row.play_pause->set_visible(room_np.valid);
    row.play_pause->set_icon_name(playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
    row.play_pause->set_tooltip_text(playing ? _("Pause") : _("Play"));
  }
}

void GnomosWindow::OnPlayerReady()
{
  player_bar_.SetEnabled(true);
  // Picks up a sleep timer already running in this room (set from the
  // Sonos app, say), for the ring around the play button.
  backend_->RefreshSleepTimerAsync();
  if (mini_player_window_)
    mini_player_window_->SetEnabled(true);
  backend_->RefreshQueueAsync();
}

void GnomosWindow::OnNowPlayingChanged()
{
  NowPlaying np = backend_->GetNowPlaying();
  player_bar_.Update(np);
  now_playing_view_.Update(np);
  if (mini_player_window_)
    mini_player_window_->Update(np);
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
  bool playing = np.valid && np.state == TransportState::Playing;
  queue_view_.SetPlaying(playing);
  if (queue_nav_indicator_)
  {
    queue_nav_indicator_->SetPlaying(playing);
    queue_nav_indicator_->set_visible(np.valid && (np.state == TransportState::Playing ||
                                                    np.state == TransportState::Paused));
  }
  UpdateNextTrackHint();
  RecordHistoryIfTrackChanged(np);
  MaybeScheduleScrobble(np);
  CheckAlarmAndTransportStatus(np);
  // Keeps radio_lyrics_filter_'s sticky effective_content_ current even
  // while the Now Playing sheet is closed — see that member's own
  // comment. Side effect only; nothing here reads the return value.
  if (np.duration == 0 && !np.stream_uri.empty())
    radio_lyrics_filter_->Filter(np.stream_uri, np.artist);
  RequestLyricsForCurrentTrack();
}

void GnomosWindow::OnPositionChanged()
{
  unsigned position = backend_->GetPosition();
  unsigned duration = backend_->GetNowPlaying().duration;
  player_bar_.UpdatePosition(position, duration);
  now_playing_view_.UpdatePosition(position, duration);
  if (mini_player_window_)
    mini_player_window_->UpdatePosition(position, duration);
}

bool GnomosWindow::OnPositionTimerTick()
{
  UpdateSleepRing();
  NowPlaying np = backend_->GetNowPlaying();
  if (np.valid && np.state == TransportState::Playing && np.duration > 0)
    backend_->RefreshPositionAsync();
  return true;  // keep the timer running
}

void GnomosWindow::OnVolumeChanged()
{
  player_bar_.UpdateVolume(backend_->GetVolume());
  now_playing_view_.UpdateVolume(backend_->GetVolume());
}

void GnomosWindow::OnQueueChanged()
{
  std::vector<QueueItem> queue = backend_->GetQueue();
  queue_view_.SetItems(queue);
  queue_view_.SetCurrentIndex(current_queue_index_);
  now_playing_view_.SetUpNext(queue, current_queue_index_);
  UpdateNextTrackHint();
}

void GnomosWindow::UpdateNextTrackHint()
{
  std::vector<QueueItem> queue = backend_->GetQueue();
  if (current_queue_index_ >= 0 && static_cast<size_t>(current_queue_index_) + 1 < queue.size())
  {
    const QueueItem& next = queue[static_cast<size_t>(current_queue_index_) + 1];
    player_bar_.UpdateNextTrack(next.title.empty() ? _("Unknown Track") : next.title);
  }
  else
  {
    player_bar_.UpdateNextTrack("");
  }
}

void GnomosWindow::CheckAlarmAndTransportStatus(const NowPlaying& now_playing)
{
  if (now_playing.alarm_running && !last_alarm_running_)
  {
    AdwToast* toast = adw_toast_new(_("Alarm ringing"));
    // A little sunrise: the title sits on a night-to-dawn gradient that
    // slowly drifts (style.css, .alarm-sunrise). Stays until stopped or
    // dismissed rather than timing out like an ordinary toast.
    GtkWidget* title = gtk_label_new(_("Good morning – your alarm is ringing"));
    gtk_widget_add_css_class(title, "alarm-sunrise");
    adw_toast_set_custom_title(toast, title);
    adw_toast_set_timeout(toast, 0);
    adw_toast_set_button_label(toast, _("Stop"));
    adw_toast_set_action_name(toast, "win.stop-alarm");
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toast_overlay_), toast);
  }
  last_alarm_running_ = now_playing.alarm_running;

  if (now_playing.valid && !now_playing.transport_status_ok && last_transport_status_ok_)
    ShowToast(_("The device reports a playback error."));
  last_transport_status_ok_ = now_playing.transport_status_ok;
}

void GnomosWindow::SetNowPlayingOpen(bool open)
{
  adw_bottom_sheet_set_open(ADW_BOTTOM_SHEET(bottom_sheet_), open);
}

void GnomosWindow::ApplyCoverTint(const CoverPalette& palette)
{
  if (!cover_css_)
  {
    cover_css_ = Gtk::CssProvider::create();
    // One above the app's own style.css, so these rules win over its
    // defaults for the same selectors.
    Gtk::StyleContext::add_provider_for_display(get_display(), cover_css_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
  bool tint = cover_tint_ && palette.valid;
  if (!tint)
  {
    cover_css_->load_from_string("");
    player_bar_.remove_css_class("cover-tinted");
    now_playing_view_.remove_css_class("cover-tinted");
    return;
  }

  // The button color has to read on both the light and the dark window
  // background, hence the clamped lightness; its icon is black or white,
  // whichever contrasts more.
  bool dark = adw_style_manager_get_dark(adw_style_manager_get_default());
  Gdk::RGBA button = ClampLightness(palette.base, dark ? 0.45 : 0.32, dark ? 0.62 : 0.50);
  std::string button_hex = ToCssHex(button);
  std::string button_fg = PrefersLightText(button) ? "#ffffff" : "#000000";
  std::string base_hex = ToCssHex(palette.base);
  std::string second_hex = ToCssHex(palette.second);
  std::string css =
      ".player-bar.cover-tinted { background-image: linear-gradient(100deg, alpha(" + base_hex + ", " +
      (dark ? "0.30" : "0.22") + "), alpha(" + second_hex + ", " + (dark ? "0.18" : "0.12") +
      ") 55%, transparent 90%); }\n"
      ".cover-tinted .play-button { background-color: " + button_hex + "; color: " + button_fg + "; }\n"
      ".cover-tinted .play-button:hover { background-color: mix(" + button_hex + ", " + button_fg +
      ", 0.1); }\n"
      ".cover-tinted scale.position-scale > trough > highlight, .now-playing.cover-tinted scale > trough > highlight "
      "{ background-color: " + button_hex + "; }\n"
      ".cover-tinted .lyrics-line.current { color: mix(" + button_hex + ", currentColor, 0.35); }\n";
  cover_css_->load_from_string(css);
  player_bar_.add_css_class("cover-tinted");
  now_playing_view_.add_css_class("cover-tinted");
}

void GnomosWindow::RequestLyricsForCurrentTrack()
{
  now_playing_view_.SetLyricsAvailable(load_lyrics_);
  if (!load_lyrics_ || !adw_bottom_sheet_get_open(ADW_BOTTOM_SHEET(bottom_sheet_)))
    return;
  NowPlaying np = backend_->GetNowPlaying();
  if (!np.valid)
    return;

  // For a radio-like source (duration == 0, stream_uri populated — see
  // NowPlaying::stream_uri's own comment), np.title is the *station* name
  // and np.artist its raw rotating content, which flips between the
  // actual song and interstitial ad/ident text. radio_lyrics_filter_
  // (continuously fed from OnNowPlayingChanged()) gives back the last
  // genuinely accepted song content instead, sticky through ad breaks —
  // see its own comment. That content follows the "<title> / <artist>"
  // convention and is reparsed accordingly (ParseRadioSongContent()).
  std::string lyrics_title = np.title;
  std::string lyrics_artist = np.artist;
  bool is_radio = np.duration == 0 && !np.stream_uri.empty();
  if (is_radio)
  {
    std::string accepted_content = radio_lyrics_filter_->Filter(np.stream_uri, np.artist);
    if (accepted_content.empty() || !ParseRadioSongContent(accepted_content, lyrics_title, lyrics_artist))
      lyrics_title.clear();
  }
  // np.album is the station's own metadata for radio, not a real album —
  // never a useful hint for the query.
  std::string album = is_radio ? "" : np.album;
  std::string key = lyrics_artist + '\x1f' + lyrics_title + '\x1f' + album;
  if (key == lyrics_track_key_)
    return;
  lyrics_track_key_ = key;

  if (lyrics_cancellable_)
    lyrics_cancellable_->cancel();
  if (lyrics_title.empty())
  {
    now_playing_view_.SetLyrics({});
    return;
  }
  now_playing_view_.SetLyricsLoading();
  lyrics_cancellable_ = Gio::Cancellable::create();
  auto cancellable = lyrics_cancellable_;
  LyricsFetcher::Instance().RequestLyrics(
      lyrics_artist, lyrics_title, album,
      [this, cancellable](Lyrics lyrics) {
        // HttpFetch() resolves a cancelled request with an empty result
        // rather than dropping it — a newer track has taken over then.
        if (cancellable->is_cancelled())
          return;
        now_playing_view_.SetLyrics(lyrics);
      },
      cancellable);
}

void GnomosWindow::QuitApplication()
{
  quitting_ = true;
  close();
}

void GnomosWindow::TogglePlayPause()
{
  NowPlaying np = backend_->GetNowPlaying();
  if (np.valid && np.state == TransportState::Playing)
    backend_->PauseOrStop();
  else
    backend_->Play();
}

void GnomosWindow::StepVolume(int delta)
{
  VolumeInfo volume = backend_->GetVolume();
  backend_->SetVolume(static_cast<uint8_t>(std::clamp(static_cast<int>(volume.volume) + delta, 0, 100)));
}

// No-op for radio (duration 0, nothing to seek within) and for anything not
// currently playing.
void GnomosWindow::SeekRelative(int seconds)
{
  NowPlaying np = backend_->GetNowPlaying();
  if (!np.valid || np.duration == 0)
    return;
  long long new_position = static_cast<long long>(backend_->GetPosition()) + seconds;
  new_position = std::clamp<long long>(new_position, 0, np.duration);
  backend_->SeekAsync(static_cast<unsigned>(new_position));
}

namespace
{
// Up/Down on their own move the focus inside a list, grid or slider — only
// outside of those do they fall through to the volume shortcut. Walks up
// from the focused widget, since the focus usually sits on a row's child.
bool FocusWantsArrowKeys(Gtk::Widget* focus)
{
  for (Gtk::Widget* w = focus; w != nullptr; w = w->get_parent())
  {
    if (dynamic_cast<Gtk::ListBox*>(w) || dynamic_cast<Gtk::FlowBox*>(w) || dynamic_cast<Gtk::ListView*>(w) ||
        dynamic_cast<Gtk::GridView*>(w) || dynamic_cast<Gtk::Range*>(w) || dynamic_cast<Gtk::Popover*>(w))
      return true;
  }
  return false;
}
}  // namespace

// Only the bare single-key shortcuts live here — everything with a
// modifier is a real accelerator (see GnomosApplication::on_startup()).
bool GnomosWindow::OnKeyPressed(guint keyval, guint /*keycode*/, Gdk::ModifierType state)
{
  constexpr auto kModifiers =
      Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::ALT_MASK | Gdk::ModifierType::SUPER_MASK;
  if ((state & kModifiers) != Gdk::ModifierType(0))
    return false;

  // Don't hijack these while the user is typing into a search box, the
  // library search dialog's entry, a spin button, etc. — Gtk::Editable is
  // the interface every text-entry-like widget implements, so this covers
  // all of them without listing each widget type.
  Gtk::Widget* focus = get_focus();
  if (dynamic_cast<Gtk::Editable*>(focus) != nullptr)
    return false;

  switch (keyval)
  {
    case GDK_KEY_space: TogglePlayPause(); return true;
    case GDK_KEY_n: backend_->Next(); return true;
    case GDK_KEY_p: backend_->Previous(); return true;
    case GDK_KEY_Up:
      if (FocusWantsArrowKeys(focus))
        return false;
      StepVolume(+5);
      return true;
    case GDK_KEY_Down:
      if (FocusWantsArrowKeys(focus))
        return false;
      StepVolume(-5);
      return true;
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

void GnomosWindow::ShowMiniPlayerWindow()
{
  // The two windows are meant to substitute for each other, not coexist —
  // opening the mini player hides the full window (same set_visible(false)
  // OnCloseRequest() itself uses for backgrounding, so this never trips
  // the "really closing" quit path below), and closing the mini player
  // brings the full window back.
  set_visible(false);

  if (mini_player_window_)
  {
    mini_player_window_->present();
    return;
  }

  auto* window = new MiniPlayerWindow();
  mini_player_window_ = window;
  // Hooked on close-request, not signal_hide() — closing goes through
  // this window's own corner button (MiniPlayerWindow calls close()), and confirmed
  // live: that path never emits "hide" at all here, it goes straight to
  // destroying the window (same "GTK's own default close-request
  // handling destroys the window without ever emitting a hide signal"
  // gotcha OnCloseRequest()'s own comment already documents for the main
  // window — it isn't ApplicationWindow-specific, a plain Gtk::Window
  // closed via its own default titlebar button hits it too). Returning
  // false here doesn't block that default destroy; it just gets our own
  // cleanup and the main window's present() to run reliably first, in
  // the same call chain as the user's actual click.
  window->signal_close_request().connect(
      [this, window] {
        if (mini_player_window_ == window)
          mini_player_window_ = nullptr;
        present();
        // GTK's own destroy (about to run right after this handler
        // returns) already tears down `window`'s underlying GtkWindow;
        // deleting the C++ wrapper here too, deferred to the next
        // main-loop iteration rather than inline — same reasoning as
        // RefreshOpenSettingsDialog()'s own comment — avoids touching it
        // while it's mid-teardown.
        Glib::signal_idle().connect_once([window] { delete window; });
        return false;
      },
      false);

  window->signal_play_pause().connect(sigc::mem_fun(*this, &GnomosWindow::TogglePlayPause));
  window->signal_next().connect([this] { backend_->Next(); });
  window->signal_previous().connect([this] { backend_->Previous(); });
  window->signal_seek_requested().connect([this](unsigned seconds) { backend_->SeekAsync(seconds); });
  window->SetCover(now_playing_view_.cover_texture(), now_playing_view_.palette());

  NowPlaying np = backend_->GetNowPlaying();
  // No standalone "is the player ready" accessor on NosonBackend —
  // player_bar_ itself is only ever enabled once, from OnPlayerReady()
  // (see its mirrored call above), and np.valid is already the same
  // proxy Update() below relies on for "is there a usable player state
  // at all", so it doubles as a reasonable initial guess here for a
  // window that can be opened after that point.
  window->SetEnabled(np.valid);
  window->Update(np);
  window->UpdatePosition(backend_->GetPosition(), np.duration);

  window->present();
}

void GnomosWindow::ShowSavePlaylistDialog()
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title(_("Save as Playlist"));
  dialog->set_default_size(360, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* label = Gtk::make_managed<Gtk::Label>(_("Playlist name"));
  label->set_halign(Gtk::Align::START);
  content->append(*label);

  auto* entry = Gtk::make_managed<Gtk::Entry>();
  entry->set_activates_default(true);
  content->append(*entry);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* save_button = Gtk::make_managed<Gtk::Button>(_("Save"));
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
  dialog->present();
  entry->grab_focus();
}

void GnomosWindow::ShowArtistInfoDialog(const std::string& artist_name)
{
  // AdwDialog (not a plain Gtk::Window): its own header bar supplies the
  // standard close ("X") action, so — unlike the old plain-window version
  // — no separate bottom "Schließen" button is needed at all.
  AdwDialog* dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, artist_name.c_str());
  adw_dialog_set_content_width(dialog, 420);
  adw_dialog_set_content_height(dialog, 520);

  GtkWidget* header_bar = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar), adw_window_title_new(artist_name.c_str(), nullptr));

  GtkWidget* toolbar_view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  // Bio — opt-in via the same api_key already configured for Last.fm
  // scrobbling (see ArtistInfoFetcher's own header comment for why no
  // separate key is needed for this read-only lookup); nothing shown at
  // all beyond a one-line explanation when it isn't set, rather than a
  // "wird geladen" placeholder that would never resolve.
  auto* bio_label = Gtk::make_managed<Gtk::Label>(
      lastfm_api_key_.empty()
          ? _("Enter a Last.fm API key in Preferences → General → Scrobbling to load a biography.")
          : _("Loading biography…"));
  bio_label->set_wrap(true);
  bio_label->set_halign(Gtk::Align::START);
  bio_label->set_justify(Gtk::Justification::LEFT);
  bio_label->set_selectable(true);
  // See ShowTrackInfoDialog()'s own lyrics_label for why this needs an
  // explicit set_can_focus(false) — a selectable Label is otherwise the
  // first focusable widget GTK auto-focuses when this window is
  // presented, which looked like the entire bio text was pre-selected.
  bio_label->set_can_focus(false);
  bio_label->add_css_class("caption");
  content->append(*bio_label);

  if (!lastfm_api_key_.empty())
  {
    auto bio_cancellable = Gio::Cancellable::create();
    g_signal_connect_data(dialog, "closed", G_CALLBACK(OnDialogClosed),
                           new std::function<void()>([bio_cancellable] { bio_cancellable->cancel(); }),
                           DeleteVoidCallback, static_cast<GConnectFlags>(0));
    std::string api_key = lastfm_api_key_;
    ArtistInfoFetcher::Instance().RequestBio(
        api_key, artist_name, [bio_label, bio_cancellable](std::string bio) {
          if (bio_cancellable->is_cancelled())
            return;
          bio_label->set_text(bio.empty() ? _("No biography found.") : bio);
        });
  }

  content->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  auto* related_heading = Gtk::make_managed<Gtk::Label>(_("Similar Artists"));
  related_heading->set_halign(Gtk::Align::START);
  related_heading->add_css_class("heading");
  content->append(*related_heading);

  auto* related_list = Gtk::make_managed<Gtk::ListBox>();
  related_list->set_selection_mode(Gtk::SelectionMode::NONE);
  related_list->add_css_class("boxed-list");
  auto* related_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  related_scroller->set_child(*related_list);
  related_scroller->set_vexpand(true);
  related_scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  content->append(*related_scroller);

  auto* related_loading_placeholder = Gtk::make_managed<Gtk::Label>(_("Loading…"));
  related_loading_placeholder->add_css_class("dimmed");
  related_loading_placeholder->set_margin_top(12);
  related_loading_placeholder->set_margin_bottom(12);
  related_list->append(*related_loading_placeholder);

  auto related_cancellable = Gio::Cancellable::create();
  g_signal_connect_data(dialog, "closed", G_CALLBACK(OnDialogClosed),
                         new std::function<void()>([related_cancellable] { related_cancellable->cancel(); }),
                         DeleteVoidCallback, static_cast<GConnectFlags>(0));
  ArtistInfoFetcher::Instance().RequestRelatedArtists(
      artist_name, [this, dialog, related_list, related_cancellable](std::vector<RelatedArtist> related) {
        if (related_cancellable->is_cancelled())
          return;
        while (Gtk::Widget* child = related_list->get_first_child())
          related_list->remove(*child);
        if (related.empty())
        {
          auto* placeholder = Gtk::make_managed<Gtk::Label>(_("No similar artists found."));
          placeholder->add_css_class("dimmed");
          placeholder->set_margin_top(12);
          placeholder->set_margin_bottom(12);
          related_list->append(*placeholder);
          return;
        }
        for (const RelatedArtist& related_artist : related)
        {
          auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
          row_box->set_margin_top(6);
          row_box->set_margin_bottom(6);
          row_box->set_margin_start(6);
          row_box->set_margin_end(6);
          auto* name_label = Gtk::make_managed<Gtk::Label>(related_artist.name);
          name_label->set_halign(Gtk::Align::START);
          name_label->set_hexpand(true);
          name_label->set_ellipsize(Pango::EllipsizeMode::END);
          row_box->append(*name_label);
          auto* search_button = Gtk::make_managed<Gtk::Button>();
          search_button->set_icon_name("system-search-symbolic");
          search_button->add_css_class("flat");
          search_button->set_valign(Gtk::Align::CENTER);
          search_button->set_tooltip_text(_("Search in Library"));
          std::string name = related_artist.name;
          search_button->signal_clicked().connect([this, dialog, name] {
            adw_dialog_close(dialog);
            ShowLibrarySearchDialog(name, "A:ALBUMARTIST");
          });
          row_box->append(*search_button);
          related_list->append(*row_box);
        }
      });

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(content->gobj()));
  adw_dialog_set_child(dialog, toolbar_view);
  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::ShowConfirmDialog(const std::string& heading, const std::string& body,
                                      const std::string& confirm_label, std::function<void()> on_confirmed)
{
  AdwDialog* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("Cancel"), "confirm", confirm_label.c_str(), nullptr);
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
  ShowConfirmDialog(_("Clear Queue?"),
                     _("Remove all tracks from the queue? This can't be undone."),
                     _("Clear"), [this] { backend_->ClearQueue(); });
}

void GnomosWindow::ShowRemoveSelectedQueueItemsConfirmDialog(std::vector<unsigned> indices)
{
  std::string body = Format(ngettext("Remove %zu track from the queue? This can't be undone.",
                                     "Remove %zu tracks from the queue? This can't be undone.", indices.size()),
                            indices.size());
  ShowConfirmDialog(_("Remove Selected?"), body, _("Remove"),
                     [this, indices] { backend_->RemoveQueueItems(indices); });
}



}  // namespace gnomos
