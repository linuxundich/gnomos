// SPDX-License-Identifier: GPL-3.0-or-later

#include "library-view.h"

#include <string>

#include <gtkmm/image.h>
#include <pangomm/layout.h>

namespace gnomos
{

LibraryView::LibraryView()
: Gtk::Box(Gtk::Orientation::VERTICAL, 0), placeholder_("Keine Einträge gefunden.")
{
  set_vexpand(true);
  set_hexpand(true);

  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  header->set_margin_top(6);
  header->set_margin_bottom(6);
  header->set_margin_start(6);
  header->set_margin_end(6);

  back_button_.set_icon_name("go-previous-symbolic");
  back_button_.add_css_class("flat");
  back_button_.set_tooltip_text("Zurück");
  back_button_.signal_clicked().connect([this] { signal_back_requested_.emit(); });
  header->append(back_button_);

  level_title_.set_halign(Gtk::Align::START);
  level_title_.set_hexpand(true);
  level_title_.set_ellipsize(Pango::EllipsizeMode::END);
  level_title_.add_css_class("heading");
  header->append(level_title_);

  search_button_.set_icon_name("system-search-symbolic");
  search_button_.add_css_class("flat");
  search_button_.set_tooltip_text("Suchen");
  search_button_.signal_clicked().connect([this] { signal_search_requested_.emit(); });
  header->append(search_button_);

  append(*header);

  count_label_.add_css_class("dim-label");
  count_label_.add_css_class("caption");
  count_label_.set_halign(Gtk::Align::START);
  count_label_.set_margin_start(12);
  count_label_.set_margin_bottom(4);
  append(count_label_);

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

  list_box_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
    if (row)
      signal_entry_activated_.emit(static_cast<unsigned>(row->get_index()));
  });

  // Grid mode (Albums/Artists — see the header comment on SetEntries()),
  // styled after Euphonica's own Albums/Artists grid
  // (https://github.com/htkhiem/euphonica): square tiles that reflow with
  // the available width, rather than a plain list.
  flow_box_.set_selection_mode(Gtk::SelectionMode::NONE);
  flow_box_.set_homogeneous(true);
  flow_box_.set_row_spacing(12);
  flow_box_.set_column_spacing(12);
  flow_box_.set_margin_top(12);
  flow_box_.set_margin_bottom(12);
  flow_box_.set_margin_start(12);
  flow_box_.set_margin_end(12);
  flow_box_.set_valign(Gtk::Align::START);
  flow_box_.set_activate_on_single_click(true);
  flow_box_.signal_child_activated().connect([this](Gtk::FlowBoxChild* child) {
    if (child)
      signal_entry_activated_.emit(static_cast<unsigned>(child->get_index()));
  });

  scroller_.set_child(list_box_);
  scroller_.set_vexpand(true);
  scroller_.set_hexpand(true);
  append(scroller_);
}

void LibraryView::SetEntries(const std::vector<LibraryEntry>& entries, bool grid)
{
  Clear();
  count_label_.set_text(entries.empty()      ? ""
                         : entries.size() == 1 ? "1 Eintrag"
                                                : std::to_string(entries.size()) + " Einträge");
  scroller_.set_child(grid ? static_cast<Gtk::Widget&>(flow_box_) : static_cast<Gtk::Widget&>(list_box_));
  if (grid)
    BuildGrid(entries);
  else
    BuildList(entries);
}

void LibraryView::BuildList(const std::vector<LibraryEntry>& entries)
{
  for (size_t i = 0; i < entries.size(); ++i)
  {
    const LibraryEntry& entry = entries[i];
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row_box->set_margin_top(6);
    row_box->set_margin_bottom(6);
    row_box->set_margin_start(6);
    row_box->set_margin_end(6);

    auto* thumbnail = Gtk::make_managed<CoverThumbnail>();
    thumbnail->SetArtUri(entry.art_uri);
    row_box->append(*thumbnail);

    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_hexpand(true);
    auto* title = Gtk::make_managed<Gtk::Label>(entry.title.empty() ? "Unbenannt" : entry.title);
    title->set_halign(Gtk::Align::START);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    labels->append(*title);
    if (!entry.subtitle.empty())
    {
      auto* subtitle = Gtk::make_managed<Gtk::Label>(entry.subtitle);
      subtitle->set_halign(Gtk::Align::START);
      subtitle->set_ellipsize(Pango::EllipsizeMode::END);
      subtitle->add_css_class("dim-label");
      subtitle->add_css_class("caption");
      labels->append(*subtitle);
    }
    row_box->append(*labels);

    if (entry.is_container)
    {
      auto* chevron = Gtk::make_managed<Gtk::Image>();
      chevron->set_from_icon_name("go-next-symbolic");
      chevron->add_css_class("dim-label");
      row_box->append(*chevron);
    }
    else
    {
      auto* add_button = Gtk::make_managed<Gtk::Button>();
      add_button->set_icon_name("list-add-symbolic");
      add_button->add_css_class("flat");
      add_button->set_valign(Gtk::Align::CENTER);
      add_button->set_tooltip_text("Zur Warteschlange hinzufügen");
      unsigned index = static_cast<unsigned>(i);
      add_button->signal_clicked().connect([this, index] { signal_add_to_queue_requested_.emit(index); });
      row_box->append(*add_button);

      auto* play_next_button = Gtk::make_managed<Gtk::Button>();
      play_next_button->set_icon_name("media-skip-forward-symbolic");
      play_next_button->add_css_class("flat");
      play_next_button->set_valign(Gtk::Align::CENTER);
      play_next_button->set_tooltip_text("Als nächstes abspielen");
      play_next_button->signal_clicked().connect([this, index] { signal_play_next_requested_.emit(index); });
      row_box->append(*play_next_button);
    }

    list_box_.append(*row_box);
  }
}

void LibraryView::BuildGrid(const std::vector<LibraryEntry>& entries)
{
  for (const LibraryEntry& entry : entries)
  {
    auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    tile->set_size_request(120, -1);

    auto* thumbnail = Gtk::make_managed<CoverThumbnail>(120);
    thumbnail->set_halign(Gtk::Align::CENTER);
    thumbnail->SetArtUri(entry.art_uri);
    tile->append(*thumbnail);

    auto* title = Gtk::make_managed<Gtk::Label>(entry.title.empty() ? "Unbenannt" : entry.title);
    title->set_halign(Gtk::Align::CENTER);
    title->set_justify(Gtk::Justification::CENTER);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    title->set_lines(2);
    title->set_wrap(true);
    title->set_max_width_chars(16);
    tile->append(*title);

    if (!entry.subtitle.empty())
    {
      auto* subtitle = Gtk::make_managed<Gtk::Label>(entry.subtitle);
      subtitle->set_halign(Gtk::Align::CENTER);
      subtitle->set_justify(Gtk::Justification::CENTER);
      subtitle->set_ellipsize(Pango::EllipsizeMode::END);
      subtitle->set_max_width_chars(16);
      subtitle->add_css_class("dim-label");
      subtitle->add_css_class("caption");
      tile->append(*subtitle);
    }

    flow_box_.append(*tile);
  }
}

void LibraryView::SetLevelTitle(const std::string& title)
{
  level_title_.set_text(title);
}

void LibraryView::SetBackVisible(bool visible)
{
  back_button_.set_visible(visible);
}

void LibraryView::Clear()
{
  while (Gtk::Widget* child = list_box_.get_first_child())
    list_box_.remove(*child);
  while (Gtk::Widget* child = flow_box_.get_first_child())
    flow_box_.remove(*child);
}

}  // namespace gnomos
