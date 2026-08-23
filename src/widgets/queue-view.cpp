// SPDX-License-Identifier: GPL-3.0-or-later

#include "queue-view.h"

#include <string>

#include <gdkmm/contentprovider.h>
#include <glibmm/value.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <pangomm/layout.h>

namespace gnomos
{

QueueView::QueueView() : Gtk::Box(Gtk::Orientation::VERTICAL, 0)
{
  set_vexpand(true);
  set_hexpand(true);

  auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  toolbar->set_margin_top(6);
  toolbar->set_margin_start(12);
  toolbar->set_margin_end(12);
  count_label_.add_css_class("dim-label");
  count_label_.add_css_class("caption");
  count_label_.set_hexpand(true);
  count_label_.set_halign(Gtk::Align::START);
  toolbar->append(count_label_);
  save_playlist_button_.set_icon_name("document-save-as-symbolic");
  save_playlist_button_.set_tooltip_text("Als Playlist speichern");
  save_playlist_button_.add_css_class("flat");
  save_playlist_button_.signal_clicked().connect([this] { signal_save_playlist_requested_.emit(); });
  toolbar->append(save_playlist_button_);
  clear_button_.set_icon_name("user-trash-symbolic");
  clear_button_.set_tooltip_text("Warteschlange leeren");
  clear_button_.add_css_class("flat");
  clear_button_.signal_clicked().connect([this] { signal_clear_requested_.emit(); });
  toolbar->append(clear_button_);
  append(*toolbar);

  list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  list_box_.add_css_class("boxed-list");
  list_box_.set_margin_top(6);
  list_box_.set_margin_bottom(12);
  list_box_.set_margin_start(12);
  list_box_.set_margin_end(12);

  list_box_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
    if (row)
      signal_item_activated_.emit(static_cast<unsigned>(row->get_index()));
  });

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);
}

void QueueView::SetItems(const std::vector<QueueItem>& items)
{
  Clear();
  count_label_.set_text(items.empty()      ? "Leer"
                         : items.size() == 1 ? "1 Titel"
                                              : std::to_string(items.size()) + " Titel");
  for (const QueueItem& item : items)
  {
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(6);
    row_box->set_margin_bottom(6);
    row_box->set_margin_start(6);
    row_box->set_margin_end(6);

    auto* drag_handle = Gtk::make_managed<Gtk::Image>();
    drag_handle->set_from_icon_name("list-drag-handle-symbolic");
    drag_handle->add_css_class("dim-label");
    row_box->append(*drag_handle);

    auto* now_playing_icon = Gtk::make_managed<Gtk::Image>();
    now_playing_icon->set_from_icon_name("media-playback-start-symbolic");
    now_playing_icon->add_css_class("accent");
    now_playing_icon->set_visible(static_cast<int>(item.index) == current_index_);
    row_box->append(*now_playing_icon);
    now_playing_icons_.push_back(now_playing_icon);

    auto* thumbnail = Gtk::make_managed<CoverThumbnail>();
    thumbnail->SetArtUri(item.art_uri);
    row_box->append(*thumbnail);

    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_hexpand(true);

    auto* title = Gtk::make_managed<Gtk::Label>(item.title.empty() ? "Unbekannter Titel" : item.title);
    title->set_halign(Gtk::Align::START);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    labels->append(*title);

    std::string subtitle = item.artist;
    if (!item.album.empty())
      subtitle += (subtitle.empty() ? "" : " — ") + item.album;
    if (!subtitle.empty())
    {
      auto* artist = Gtk::make_managed<Gtk::Label>(subtitle);
      artist->set_halign(Gtk::Align::START);
      artist->set_ellipsize(Pango::EllipsizeMode::END);
      artist->add_css_class("dim-label");
      artist->add_css_class("caption");
      labels->append(*artist);
    }
    row_box->append(*labels);

    auto* remove_button = Gtk::make_managed<Gtk::Button>();
    remove_button->set_icon_name("user-trash-symbolic");
    remove_button->add_css_class("flat");
    remove_button->set_valign(Gtk::Align::CENTER);
    remove_button->set_tooltip_text("Aus Warteschlange entfernen");
    unsigned index = item.index;
    remove_button->signal_clicked().connect([this, index] { signal_item_remove_requested_.emit(index); });
    row_box->append(*remove_button);

    // Drag-and-drop reorder: the dragged row's index travels as the drag
    // content itself (a plain int); the row under the cursor when it's
    // dropped is the target — both indices are into the list SetItems()
    // was last called with, same as every other index this widget emits.
    auto drag_source = Gtk::DragSource::create();
    drag_source->set_actions(Gdk::DragAction::MOVE);
    drag_source->signal_prepare().connect(
        [index](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
          Glib::Value<int> value;
          value.init(Glib::Value<int>::value_type());
          value.set(static_cast<int>(index));
          return Gdk::ContentProvider::create(value);
        },
        false);
    row_box->add_controller(drag_source);

    auto drop_target = Gtk::DropTarget::create(Glib::Value<int>::value_type(), Gdk::DragAction::MOVE);
    drop_target->signal_drop().connect(
        [this, index](const Glib::ValueBase& value, double, double) -> bool {
          if (value.gobj()->g_type != Glib::Value<int>::value_type())
            return false;
          Glib::Value<int> int_value;
          int_value.init(value.gobj());
          unsigned from = static_cast<unsigned>(int_value.get());
          if (from != index)
            signal_reorder_requested_.emit(from, index);
          return true;
        },
        false);
    row_box->add_controller(drop_target);

    list_box_.append(*row_box);
  }
}

void QueueView::SetCurrentIndex(int index)
{
  current_index_ = index;
  for (size_t i = 0; i < now_playing_icons_.size(); ++i)
    now_playing_icons_[i]->set_visible(static_cast<int>(i) == index);
}

void QueueView::Clear()
{
  now_playing_icons_.clear();
  while (Gtk::Widget* child = list_box_.get_first_child())
    list_box_.remove(*child);
}

}  // namespace gnomos
