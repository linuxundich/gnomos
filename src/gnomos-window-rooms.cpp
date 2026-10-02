// SPDX-License-Identifier: GPL-3.0-or-later
//
// GnomosWindow, continued: Grouping, device info, alarms, the sleep timer and sound settings.
// The class itself and its constructor live in gnomos-window.cpp.

#include "gnomos-window.h"

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

  // "Alle Räume" master fader — a proportional scale (preserves each
  // member's relative balance) rather than an absolute one, computed and
  // applied entirely client-side: SetRoomVolume() per member, same call
  // the individual sliders below already make, no native synced-group-
  // volume SOAP action exists in this libnoson fork to call instead.
  // room_scales lets dragging the master also move each member's own
  // slider live, rather than waiting for the next RebuildGroupingPopover()
  // (itself only triggered by a real volume-changed event coming back).
  auto room_scales = std::make_shared<std::map<std::string, Gtk::Scale*>>();
  std::vector<std::pair<std::string, uint8_t>> member_volumes;
  for (const RoomInfo& room : rooms)
  {
    uint8_t v = 0;
    if (room.group_id == selected_group_id_ && backend_->GetRoomVolume(room.player_uuid, v))
      member_volumes.push_back({room.player_uuid, v});
  }
  // Redundant with the one per-room slider right below it for a
  // single-member "group" — only worth showing once there's actually
  // more than one room to balance against each other.
  if (member_volumes.size() > 1)
  {
    int sum = 0;
    for (const auto& [uuid, v] : member_volumes)
      sum += v;
    int average = sum / static_cast<int>(member_volumes.size());

    auto* master_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    master_row->set_margin_top(6);
    master_row->set_margin_bottom(6);
    master_row->set_margin_start(6);
    master_row->set_margin_end(6);
    auto* master_label = Gtk::make_managed<Gtk::Label>("Alle Räume");
    master_label->set_halign(Gtk::Align::START);
    master_label->add_css_class("heading");
    master_row->append(*master_label);
    auto* master_scale = Gtk::make_managed<Gtk::Scale>();
    master_scale->set_range(0, 100);
    master_scale->set_increments(2, 10);
    master_scale->set_value(average);
    master_scale->set_draw_value(false);
    master_row->append(*master_scale);
    // No separator needed after this — grouping_list_box_ already has the
    // "boxed-list" CSS class (see its own setup), which draws a divider
    // between every row automatically; an explicit Gtk::Separator here
    // was redundant and, wrapped in its own implicit row, showed up as a
    // stray extra line rather than a clean boundary.
    grouping_list_box_.append(*master_row);

    master_scale->signal_value_changed().connect([this, master_scale, member_volumes, average, room_scales] {
      int new_value = static_cast<int>(master_scale->get_value());
      for (const auto& [uuid, baseline] : member_volumes)
      {
        // average == 0 means every member started silent — nothing to
        // scale proportionally (baseline * ratio stays 0 forever), so
        // fall back to setting everyone to the same new value instead.
        int scaled = average > 0
                          ? static_cast<int>(std::lround(baseline * (static_cast<double>(new_value) / average)))
                          : new_value;
        scaled = std::clamp(scaled, 0, 100);
        backend_->SetRoomVolume(uuid, static_cast<uint8_t>(scaled));
        auto it = room_scales->find(uuid);
        if (it != room_scales->end())
          it->second->set_value(scaled);
      }
    });
  }

  std::set<std::string> current_members;
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
      badge->add_css_class("dimmed");
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

    // A room that just joined slides into place with a short accent glow
    // (.just-joined in style.css) — it's the one row that changed.
    if (in_selected_group && grouping_seen_group_id_ == selected_group_id_ &&
        !grouping_seen_members_.count(room.player_uuid))
      row_box->add_css_class("just-joined");
    if (in_selected_group)
      current_members.insert(room.player_uuid);

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
      (*room_scales)[uuid] = volume_scale;
      row_box->append(*volume_scale);
    }

    grouping_list_box_.append(*row_box);
  }
  grouping_seen_group_id_ = selected_group_id_;
  grouping_seen_members_ = std::move(current_members);
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

  auto* dialog = new DialogShell(*this);
  dialog->set_title("Geräteinfo");
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
      badge->add_css_class("dimmed");
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
      label->add_css_class("dimmed");
      grid->attach(*label, 0, row);

      auto* value = Gtk::make_managed<Gtk::Label>(value_text);
      value->set_halign(Gtk::Align::START);
      value->set_selectable(true);
      // See ShowTrackInfoDialog()'s own lyrics_label for why this needs an
      // explicit set_can_focus(false) — a selectable Label is otherwise the
      // first focusable widget GTK auto-focuses when this window is
      // presented, which looked like the first room's "Modell" value (the
      // very first field of the very first room) was always highlighted
      // blue regardless of which model it actually was.
      value->set_can_focus(false);
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

  auto* dialog = new DialogShell(*this);
  dialog->set_title(editing ? "Alarm bearbeiten" : (duplicate ? "Alarm duplizieren" : "Neuer Alarm"));
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
  dialog->present();
}

void GnomosWindow::OnSleepTimerChanged()
{
  SleepTimerInfo info = backend_->GetSleepTimerInfo();
  sleep_timer_status_label_.set_text(info.active ? "Aktiv, verbleibend: " + info.remaining : "Kein Sleep-Timer aktiv");

  // "H:MM:SS" -> seconds, counted down locally from here (see
  // UpdateSleepRing()) — the device isn't asked again until it should
  // have run out.
  unsigned h = 0, m = 0, sec = 0;
  if (info.active && std::sscanf(info.remaining.c_str(), "%u:%u:%u", &h, &m, &sec) == 3)
  {
    unsigned remaining = h * 3600 + m * 60 + sec;
    // A timer set elsewhere (another app, the device itself) has no known
    // total — the first remaining time seen stands in for it.
    if (sleep_total_seconds_ < remaining)
      sleep_total_seconds_ = remaining;
    sleep_deadline_ = g_get_monotonic_time() + static_cast<gint64>(remaining) * G_USEC_PER_SEC;
    sleep_timer_button_.add_css_class("sleep-active");
  }
  else
  {
    sleep_total_seconds_ = 0;
    sleep_deadline_ = 0;
    sleep_timer_button_.remove_css_class("sleep-active");
  }
  UpdateSleepRing();
}

void GnomosWindow::UpdateSleepRing()
{
  if (sleep_deadline_ == 0 || sleep_total_seconds_ == 0)
  {
    player_bar_.SetSleepProgress(0);
    return;
  }
  double remaining = (sleep_deadline_ - g_get_monotonic_time()) / static_cast<double>(G_USEC_PER_SEC);
  if (remaining <= 0)
  {
    player_bar_.SetSleepProgress(0);
    sleep_deadline_ = 0;
    backend_->RefreshSleepTimerAsync();
    return;
  }
  player_bar_.SetSleepProgress(remaining / sleep_total_seconds_);
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

void GnomosWindow::ShowDeleteAlarmConfirmDialog(std::string alarm_id)
{
  ShowConfirmDialog("Alarm löschen?", "Diesen Alarm wirklich löschen?", "Löschen",
                     [this, alarm_id] { backend_->DeleteAlarm(alarm_id); });
}

}  // namespace gnomos
