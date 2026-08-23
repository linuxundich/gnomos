// SPDX-License-Identifier: GPL-3.0-or-later

#include "favorites-view.h"

#include <algorithm>
#include <cctype>

#include <gtkmm/box.h>
#include <pangomm/layout.h>

namespace gnomos
{

namespace
{
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle_lower)
{
  std::string haystack_lower = haystack;
  std::transform(haystack_lower.begin(), haystack_lower.end(), haystack_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return haystack_lower.find(needle_lower) != std::string::npos;
}
}  // namespace

FavoritesView::FavoritesView() : Gtk::Box(Gtk::Orientation::VERTICAL, 0), placeholder_("Keine Favoriten gefunden.")
{
  set_vexpand(true);
  set_hexpand(true);

  search_entry_.set_placeholder_text("Favoriten durchsuchen…");
  search_entry_.set_margin_top(12);
  search_entry_.set_margin_start(12);
  search_entry_.set_margin_end(12);
  search_entry_.signal_search_changed().connect(sigc::mem_fun(*this, &FavoritesView::ApplyFilter));
  append(search_entry_);

  placeholder_.set_wrap(true);
  placeholder_.add_css_class("dim-label");
  placeholder_.set_margin_top(24);
  placeholder_.set_margin_bottom(24);

  list_box_.set_placeholder(placeholder_);
  list_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  list_box_.add_css_class("boxed-list");
  list_box_.set_margin_top(12);
  list_box_.set_margin_bottom(12);
  list_box_.set_margin_start(12);
  list_box_.set_margin_end(12);

  list_box_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
    if (!row)
      return;
    size_t position = static_cast<size_t>(row->get_index());
    if (position < row_index_map_.size())
      signal_item_activated_.emit(row_index_map_[position]);
  });

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);
}

void FavoritesView::SetItems(const std::vector<FavoriteItem>& items)
{
  all_items_ = items;
  ApplyFilter();
}

void FavoritesView::ApplyFilter()
{
  Clear();
  row_index_map_.clear();

  std::string term = search_entry_.get_text();
  std::transform(term.begin(), term.end(), term.begin(), [](unsigned char c) { return std::tolower(c); });

  for (const FavoriteItem& item : all_items_)
  {
    if (!term.empty() && !ContainsCaseInsensitive(item.title, term) && !ContainsCaseInsensitive(item.subtitle, term))
      continue;

    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(6);
    row_box->set_margin_bottom(6);
    row_box->set_margin_start(6);
    row_box->set_margin_end(6);

    auto* thumbnail = Gtk::make_managed<CoverThumbnail>();
    thumbnail->SetArtUri(item.art_uri);
    row_box->append(*thumbnail);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    box->set_hexpand(true);

    auto* title = Gtk::make_managed<Gtk::Label>(item.title.empty() ? "Unbenannter Favorit" : item.title);
    title->set_halign(Gtk::Align::START);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    box->append(*title);

    if (!item.subtitle.empty())
    {
      auto* subtitle = Gtk::make_managed<Gtk::Label>(item.subtitle);
      subtitle->set_halign(Gtk::Align::START);
      subtitle->set_ellipsize(Pango::EllipsizeMode::END);
      subtitle->add_css_class("dim-label");
      subtitle->add_css_class("caption");
      box->append(*subtitle);
    }
    row_box->append(*box);

    auto* add_button = Gtk::make_managed<Gtk::Button>();
    add_button->set_icon_name("list-add-symbolic");
    add_button->add_css_class("flat");
    add_button->set_valign(Gtk::Align::CENTER);
    add_button->set_tooltip_text("Zur Warteschlange hinzufügen");
    unsigned index = item.index;
    add_button->signal_clicked().connect([this, index] { signal_add_to_queue_requested_.emit(index); });
    row_box->append(*add_button);

    auto* play_next_button = Gtk::make_managed<Gtk::Button>();
    play_next_button->set_icon_name("media-skip-forward-symbolic");
    play_next_button->add_css_class("flat");
    play_next_button->set_valign(Gtk::Align::CENTER);
    play_next_button->set_tooltip_text("Als nächstes abspielen");
    play_next_button->signal_clicked().connect([this, index] { signal_play_next_requested_.emit(index); });
    row_box->append(*play_next_button);

    auto* delete_button = Gtk::make_managed<Gtk::Button>();
    delete_button->set_icon_name("user-trash-symbolic");
    delete_button->add_css_class("flat");
    delete_button->set_valign(Gtk::Align::CENTER);
    delete_button->set_tooltip_text("Favorit löschen");
    delete_button->signal_clicked().connect([this, index] { signal_delete_requested_.emit(index); });
    row_box->append(*delete_button);

    row_index_map_.push_back(index);
    list_box_.append(*row_box);
  }
}

void FavoritesView::Clear()
{
  while (Gtk::Widget* child = list_box_.get_first_child())
    list_box_.remove(*child);
}

}  // namespace gnomos
