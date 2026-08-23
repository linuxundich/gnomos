// SPDX-License-Identifier: GPL-3.0-or-later

#include "alarms-view.h"

#include <algorithm>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/switch.h>
#include <pangomm/layout.h>

namespace gnomos
{

AlarmsView::AlarmsView() : Gtk::Box(Gtk::Orientation::VERTICAL, 0), placeholder_("Keine Alarme eingerichtet.")
{
  set_vexpand(true);
  set_hexpand(true);

  auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  toolbar->set_halign(Gtk::Align::END);
  toolbar->set_margin_top(6);
  toolbar->set_margin_end(12);
  add_button_.set_icon_name("list-add-symbolic");
  add_button_.set_tooltip_text("Alarm hinzufügen");
  add_button_.add_css_class("flat");
  add_button_.signal_clicked().connect([this] { signal_add_requested_.emit(); });
  toolbar->append(add_button_);
  append(*toolbar);

  placeholder_.set_wrap(true);
  placeholder_.add_css_class("dim-label");
  placeholder_.set_margin_top(24);
  placeholder_.set_margin_bottom(24);

  list_box_.set_placeholder(placeholder_);
  list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  list_box_.add_css_class("boxed-list");
  list_box_.set_margin_top(6);
  list_box_.set_margin_bottom(12);
  list_box_.set_margin_start(12);
  list_box_.set_margin_end(12);

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);
}

void AlarmsView::SetItems(const std::vector<AlarmInfo>& items)
{
  Clear();

  // Sort by start_time ("HH:MM:SS", device-native — lexicographic order
  // matches chronological order for that fixed-width format) so alarms
  // appear in the order they'll actually fire during the day, not
  // creation order.
  std::vector<AlarmInfo> sorted_items = items;
  std::sort(sorted_items.begin(), sorted_items.end(),
            [](const AlarmInfo& a, const AlarmInfo& b) { return a.start_time < b.start_time; });

  for (const AlarmInfo& alarm : sorted_items)
  {
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    row_box->set_margin_top(8);
    row_box->set_margin_bottom(8);
    row_box->set_margin_start(8);
    row_box->set_margin_end(8);

    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_hexpand(true);

    auto* time_label = Gtk::make_managed<Gtk::Label>(alarm.start_time + " — " + alarm.room_name);
    time_label->set_halign(Gtk::Align::START);
    time_label->set_ellipsize(Pango::EllipsizeMode::END);
    time_label->add_css_class("heading");
    labels->append(*time_label);

    if (!alarm.recurrence.empty())
    {
      auto* recurrence_label = Gtk::make_managed<Gtk::Label>(alarm.recurrence);
      recurrence_label->set_halign(Gtk::Align::START);
      recurrence_label->set_ellipsize(Pango::EllipsizeMode::END);
      recurrence_label->add_css_class("dim-label");
      recurrence_label->add_css_class("caption");
      labels->append(*recurrence_label);
    }

    row_box->append(*labels);

    auto* linked_zones_button = Gtk::make_managed<Gtk::ToggleButton>();
    linked_zones_button->set_icon_name("network-workgroup-symbolic");
    linked_zones_button->add_css_class("flat");
    linked_zones_button->set_valign(Gtk::Align::CENTER);
    linked_zones_button->set_tooltip_text(
        "Auch in verknüpften Räumen abspielen (nicht nur in diesem Raum)");
    linked_zones_button->set_active(alarm.include_linked_zones);
    std::string id_for_linked_zones = alarm.id;
    // signal_clicked() (not signal_toggled()), same reasoning as the
    // player bar's shuffle/repeat buttons: fires only on a real user
    // click, never for the programmatic set_active() above.
    linked_zones_button->signal_clicked().connect([this, linked_zones_button, id_for_linked_zones] {
      signal_include_linked_zones_toggled_.emit(id_for_linked_zones, linked_zones_button->get_active());
    });
    row_box->append(*linked_zones_button);

    auto* edit_button = Gtk::make_managed<Gtk::Button>();
    edit_button->set_icon_name("document-edit-symbolic");
    edit_button->add_css_class("flat");
    edit_button->set_valign(Gtk::Align::CENTER);
    edit_button->set_tooltip_text("Alarm bearbeiten");
    std::string id_for_edit = alarm.id;
    edit_button->signal_clicked().connect([this, id_for_edit] { signal_edit_requested_.emit(id_for_edit); });
    row_box->append(*edit_button);

    auto* delete_button = Gtk::make_managed<Gtk::Button>();
    delete_button->set_icon_name("user-trash-symbolic");
    delete_button->add_css_class("flat");
    delete_button->set_valign(Gtk::Align::CENTER);
    delete_button->set_tooltip_text("Alarm löschen");
    std::string id_for_delete = alarm.id;
    delete_button->signal_clicked().connect(
        [this, id_for_delete] { signal_delete_requested_.emit(id_for_delete); });
    row_box->append(*delete_button);

    auto* enabled_switch = Gtk::make_managed<Gtk::Switch>();
    enabled_switch->set_valign(Gtk::Align::CENTER);
    enabled_switch->set_active(alarm.enabled);
    std::string id_for_toggle = alarm.id;
    // signal_state_set() (not notify::active) only fires for user
    // interaction, so the set_active() call above can't cause a spurious
    // toggle here — same reasoning as the grouping popover's switches.
    enabled_switch->signal_state_set().connect(
        [this, id_for_toggle](bool state) -> bool {
          signal_enabled_toggled_.emit(id_for_toggle, state);
          return true;
        },
        false);
    row_box->append(*enabled_switch);

    list_box_.append(*row_box);
  }
}

void AlarmsView::Clear()
{
  while (Gtk::Widget* child = list_box_.get_first_child())
    list_box_.remove(*child);
}

}  // namespace gnomos
