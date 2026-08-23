// SPDX-License-Identifier: GPL-3.0-or-later

#include "history-view.h"

#include <gtkmm/box.h>
#include <pangomm/layout.h>

#include "cover-thumbnail.h"

namespace gnomos
{

HistoryView::HistoryView() : Gtk::Box(Gtk::Orientation::VERTICAL, 0), placeholder_("Noch nichts gespielt.")
{
  set_vexpand(true);
  set_hexpand(true);

  auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  toolbar->set_halign(Gtk::Align::END);
  toolbar->set_margin_top(6);
  toolbar->set_margin_end(12);
  clear_button_.set_icon_name("user-trash-symbolic");
  clear_button_.set_tooltip_text("Verlauf leeren");
  clear_button_.add_css_class("flat");
  clear_button_.signal_clicked().connect([this] { signal_clear_requested_.emit(); });
  toolbar->append(clear_button_);
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

void HistoryView::SetItems(const std::vector<HistoryEntry>& items)
{
  Clear();
  unsigned index = 0;
  for (const HistoryEntry& entry : items)
  {
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

    auto* title = Gtk::make_managed<Gtk::Label>(entry.title.empty() ? "Unbekannter Titel" : entry.title);
    title->set_halign(Gtk::Align::START);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    labels->append(*title);

    std::string subtitle_text = entry.artist;
    if (!entry.album.empty())
      subtitle_text += (subtitle_text.empty() ? "" : " — ") + entry.album;
    if (!subtitle_text.empty())
    {
      auto* subtitle = Gtk::make_managed<Gtk::Label>(subtitle_text);
      subtitle->set_halign(Gtk::Align::START);
      subtitle->set_ellipsize(Pango::EllipsizeMode::END);
      subtitle->add_css_class("dim-label");
      subtitle->add_css_class("caption");
      labels->append(*subtitle);
    }
    row_box->append(*labels);

    auto* search_button = Gtk::make_managed<Gtk::Button>();
    search_button->set_icon_name("system-search-symbolic");
    search_button->add_css_class("flat");
    search_button->set_valign(Gtk::Align::CENTER);
    search_button->set_tooltip_text("In der Bibliothek suchen");
    search_button->signal_clicked().connect([this, index] { signal_search_requested_.emit(index); });
    row_box->append(*search_button);

    list_box_.append(*row_box);
    ++index;
  }
}

void HistoryView::Clear()
{
  while (Gtk::Widget* child = list_box_.get_first_child())
    list_box_.remove(*child);
}

}  // namespace gnomos
