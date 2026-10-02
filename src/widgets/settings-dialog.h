// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <adwaita.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/widget.h>

namespace gnomos
{

// The preferences dialog's frame: an AdwDialog split into a sidebar of
// categories (AdwSidebar, in sections) and the selected category's page on
// the right (an AdwPreferencesPage each, scrolling on its own). On narrow
// windows the split view collapses into GNOME-Settings-style navigation:
// the category list first, the page after picking one, with a back button.
//
// A search field above the sidebar filters categories by their title and a
// set of keywords, so "token" still finds Scrobbling — the replacement for
// AdwPreferencesDialog's own search, which searched row titles.
//
// Purely the frame: GnomosWindow builds the rows and groups (they're tied
// to its state) and adds one page per category. Create with `new`; it
// deletes itself once the dialog has closed.
class SettingsDialog
{
public:
  SettingsDialog();
  SettingsDialog(const SettingsDialog&) = delete;
  SettingsDialog& operator=(const SettingsDialog&) = delete;

  // Starts a new sidebar section; an empty title gives an untitled one.
  void AddSection(const std::string& title);
  // Adds a category to the current section. `page` is an
  // AdwPreferencesPage; keywords are space-separated, lower case.
  void AddCategory(const std::string& id, const std::string& title, const std::string& icon_name,
                   const std::string& keywords, GtkWidget* page);

  // Picks the category to show first (falls back to the first one).
  void Present(Gtk::Widget& parent, const std::string& category_id);
  // Closes without the closing animation — for reopening right away.
  void ForceClose();

  // The category the user switched to, for remembering it.
  std::function<void(const std::string&)> on_category_changed;
  // The dialog has closed; the object deletes itself right after.
  std::function<void()> on_closed;

  AdwDialog* gobj() { return dialog_; }

private:
  ~SettingsDialog();
  struct Category
  {
    std::string id;
    std::string title;
    std::string haystack;  // title + keywords, lower case, for the filter
    AdwSidebarItem* item = nullptr;
  };

  void ShowCategory(const std::string& id, bool navigate);
  void OnSearchChanged();
  const Category* FindByItem(AdwSidebarItem* item) const;
  static gboolean FilterItem(gpointer item, gpointer user_data);
  static void OnSidebarActivated(AdwSidebar* sidebar, guint index, gpointer user_data);
  static void OnSelectedChanged(GObject* sidebar, GParamSpec*, gpointer user_data);
  static void OnDialogClosed(AdwDialog* dialog, gpointer user_data);

  AdwDialog* dialog_ = nullptr;
  GtkWidget* split_view_ = nullptr;
  GtkWidget* sidebar_ = nullptr;
  GtkWidget* stack_ = nullptr;
  GtkWidget* content_page_ = nullptr;
  AdwSidebarSection* section_ = nullptr;
  GtkFilter* filter_ = nullptr;
  Gtk::SearchEntry search_entry_;
  std::vector<Category> categories_;
  std::string current_;
  bool updating_ = false;
};

}  // namespace gnomos
