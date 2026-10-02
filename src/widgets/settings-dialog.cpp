// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings-dialog.h"
#include "../i18n.h"

#include <algorithm>
#include <cctype>

#include <glibmm/main.h>

namespace gnomos
{

namespace
{
constexpr const char* kCategoryIdKey = "gnomos-settings-category";

std::string Lower(std::string text)
{
  // Good enough for the filter: Glib's casefold handles umlauts, which
  // std::tolower doesn't.
  gchar* folded = g_utf8_casefold(text.c_str(), -1);
  std::string result = folded ? folded : "";
  g_free(folded);
  return result;
}
}  // namespace

SettingsDialog::SettingsDialog()
{
  dialog_ = adw_dialog_new();
  g_object_ref_sink(dialog_);
  adw_dialog_set_title(dialog_, _("Preferences"));
  adw_dialog_set_content_width(dialog_, 860);
  adw_dialog_set_content_height(dialog_, 600);
  g_signal_connect(dialog_, "closed", G_CALLBACK(OnDialogClosed), this);

  // --- Sidebar: search field, then the categories ---
  sidebar_ = adw_sidebar_new();
  filter_ = GTK_FILTER(gtk_custom_filter_new(FilterItem, this, nullptr));
  adw_sidebar_set_filter(ADW_SIDEBAR(sidebar_), filter_);
  GtkWidget* no_match = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(no_match), "edit-find-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(no_match), _("No Matching Setting"));
  gtk_widget_add_css_class(no_match, "compact");
  adw_sidebar_set_placeholder(ADW_SIDEBAR(sidebar_), no_match);
  g_signal_connect(sidebar_, "activated", G_CALLBACK(OnSidebarActivated), this);
  g_signal_connect(sidebar_, "notify::selected", G_CALLBACK(OnSelectedChanged), this);

  search_entry_.set_placeholder_text(_("Search preferences"));
  search_entry_.set_margin_start(8);
  search_entry_.set_margin_end(8);
  search_entry_.set_margin_bottom(6);
  search_entry_.signal_search_changed().connect(sigc::mem_fun(*this, &SettingsDialog::OnSearchChanged));

  GtkWidget* sidebar_toolbar = adw_toolbar_view_new();
  GtkWidget* sidebar_header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebar_toolbar), sidebar_header);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebar_toolbar), GTK_WIDGET(search_entry_.gobj()));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebar_toolbar), sidebar_);
  AdwNavigationPage* sidebar_page = adw_navigation_page_new(sidebar_toolbar, _("Preferences"));

  // --- Content: one page per category, each scrolling by itself ---
  stack_ = adw_view_stack_new();
  GtkWidget* content_toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_toolbar), stack_);
  content_page_ = GTK_WIDGET(adw_navigation_page_new(content_toolbar, ""));

  split_view_ = adw_navigation_split_view_new();
  adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split_view_), sidebar_page);
  adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split_view_), ADW_NAVIGATION_PAGE(content_page_));
  adw_navigation_split_view_set_min_sidebar_width(ADW_NAVIGATION_SPLIT_VIEW(split_view_), 200);
  adw_navigation_split_view_set_max_sidebar_width(ADW_NAVIGATION_SPLIT_VIEW(split_view_), 240);
  adw_dialog_set_child(dialog_, split_view_);

  // Narrow: category list first, the page after picking one (with a back
  // button from the navigation view), and the sidebar drawn as a boxed
  // list page — the way GNOME Settings behaves on a phone.
  AdwBreakpoint* narrow =
      adw_breakpoint_new(adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 600, ADW_LENGTH_UNIT_SP));
  GValue collapsed = G_VALUE_INIT;
  g_value_init(&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean(&collapsed, TRUE);
  adw_breakpoint_add_setter(narrow, G_OBJECT(split_view_), "collapsed", &collapsed);
  g_value_unset(&collapsed);
  GValue mode = G_VALUE_INIT;
  g_value_init(&mode, ADW_TYPE_SIDEBAR_MODE);
  g_value_set_enum(&mode, ADW_SIDEBAR_MODE_PAGE);
  adw_breakpoint_add_setter(narrow, G_OBJECT(sidebar_), "mode", &mode);
  g_value_unset(&mode);
  adw_dialog_add_breakpoint(dialog_, narrow);
}

SettingsDialog::~SettingsDialog()
{
  g_signal_handlers_disconnect_by_data(dialog_, this);
  g_signal_handlers_disconnect_by_data(sidebar_, this);
  g_object_unref(filter_);
  g_object_unref(dialog_);
}

void SettingsDialog::AddSection(const std::string& title)
{
  section_ = adw_sidebar_section_new();
  if (!title.empty())
    adw_sidebar_section_set_title(section_, title.c_str());
  adw_sidebar_append(ADW_SIDEBAR(sidebar_), section_);
}

void SettingsDialog::AddCategory(const std::string& id, const std::string& title, const std::string& icon_name,
                                 const std::string& keywords, GtkWidget* page)
{
  if (!section_)
    AddSection("");
  AdwSidebarItem* item = adw_sidebar_item_new(title.c_str());
  adw_sidebar_item_set_icon_name(item, icon_name.c_str());
  g_object_set_data_full(G_OBJECT(item), kCategoryIdKey, g_strdup(id.c_str()), g_free);
  adw_sidebar_section_append(section_, item);
  adw_view_stack_add_named(ADW_VIEW_STACK(stack_), page, id.c_str());
  categories_.push_back({id, title, Lower(title + " " + keywords), item});
}

void SettingsDialog::Present(Gtk::Widget& parent, const std::string& category_id)
{
  bool known = std::any_of(categories_.begin(), categories_.end(),
                           [&](const Category& c) { return c.id == category_id; });
  ShowCategory(known ? category_id : (categories_.empty() ? "" : categories_.front().id), false);
  adw_dialog_present(dialog_, GTK_WIDGET(parent.gobj()));
}

void SettingsDialog::ForceClose()
{
  adw_dialog_force_close(dialog_);
}

void SettingsDialog::ShowCategory(const std::string& id, bool navigate)
{
  auto it = std::find_if(categories_.begin(), categories_.end(), [&](const Category& c) { return c.id == id; });
  if (it == categories_.end())
    return;
  current_ = id;
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(stack_), id.c_str());
  adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(content_page_), it->title.c_str());

  // Keep the sidebar's selection in step (it's the source when the user
  // clicks, but not when presenting or filtering).
  updating_ = true;
  GtkSelectionModel* items = adw_sidebar_get_items(ADW_SIDEBAR(sidebar_));
  guint n = g_list_model_get_n_items(G_LIST_MODEL(items));
  for (guint i = 0; i < n; ++i)
  {
    auto* item = static_cast<AdwSidebarItem*>(g_list_model_get_item(G_LIST_MODEL(items), i));
    bool match = item == it->item;
    g_object_unref(item);
    if (match)
    {
      adw_sidebar_set_selected(ADW_SIDEBAR(sidebar_), i);
      break;
    }
  }
  updating_ = false;

  if (navigate)
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(split_view_), TRUE);
  if (on_category_changed)
    on_category_changed(id);
}

const SettingsDialog::Category* SettingsDialog::FindByItem(AdwSidebarItem* item) const
{
  for (const Category& c : categories_)
    if (c.item == item)
      return &c;
  return nullptr;
}

gboolean SettingsDialog::FilterItem(gpointer item, gpointer user_data)
{
  auto* self = static_cast<SettingsDialog*>(user_data);
  std::string term = Lower(self->search_entry_.get_text());
  if (term.empty())
    return TRUE;
  const Category* category = self->FindByItem(static_cast<AdwSidebarItem*>(item));
  return category && category->haystack.find(term) != std::string::npos;
}

void SettingsDialog::OnSearchChanged()
{
  gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT);
  // On a wide dialog the first hit opens right away; when narrow, the
  // filtered list itself is what the user is looking at.
  if (adw_navigation_split_view_get_collapsed(ADW_NAVIGATION_SPLIT_VIEW(split_view_)))
    return;
  std::string term = Lower(search_entry_.get_text());
  for (const Category& c : categories_)
  {
    if (term.empty() ? c.id == current_ : c.haystack.find(term) != std::string::npos)
    {
      ShowCategory(c.id, false);
      return;
    }
  }
}

void SettingsDialog::OnSidebarActivated(AdwSidebar* sidebar, guint, gpointer user_data)
{
  auto* self = static_cast<SettingsDialog*>(user_data);
  AdwSidebarItem* item = adw_sidebar_get_selected_item(sidebar);
  if (const Category* category = item ? self->FindByItem(item) : nullptr)
    self->ShowCategory(category->id, true);
}

void SettingsDialog::OnSelectedChanged(GObject* sidebar, GParamSpec*, gpointer user_data)
{
  auto* self = static_cast<SettingsDialog*>(user_data);
  if (self->updating_)
    return;
  AdwSidebarItem* item = adw_sidebar_get_selected_item(ADW_SIDEBAR(sidebar));
  if (const Category* category = item ? self->FindByItem(item) : nullptr)
    self->ShowCategory(category->id, false);
}

void SettingsDialog::OnDialogClosed(AdwDialog*, gpointer user_data)
{
  auto* self = static_cast<SettingsDialog*>(user_data);
  if (self->on_closed)
    self->on_closed();
  Glib::signal_idle().connect_once([self] { delete self; });
}

}  // namespace gnomos
