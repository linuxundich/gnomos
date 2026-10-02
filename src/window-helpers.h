// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small helpers shared by GnomosWindow's source files (gnomos-window.cpp
// and gnomos-window-*.cpp): file locations, a few parsers, and the
// extern "C" trampolines that connect raw libadwaita signals to
// heap-allocated std::function callbacks (gtkmm has no bindings for the Adw
// widgets — see ARCHITECTURE.md). Internal to the window; nothing else
// includes this.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <adwaita.h>

#include "backend/noson-types.h"

namespace gnomos
{

// Sidebar items carry their action as GObject data, run by
// OnNavSidebarSelectedChanged() when the item gets selected.
void SetSidebarItemAction(AdwSidebarItem* item, std::function<void()> action);
extern "C" void OnNavSidebarSelectedChanged(GObject* sidebar_obj, GParamSpec*, gpointer);

// "<title> / <artist>" radio content → title and artist; false if the
// content doesn't follow that convention.
bool ParseRadioSongContent(const std::string& content, std::string& title, std::string& artist);

// ~/.config/gnomos/... files.
std::string StateFilePath();
std::string HistoryFilePath();
std::string ScenesFilePath();
constexpr size_t kMaxHistoryEntries = 50;

// Alarm recurrence ("ON_12345" etc.) → weekday numbers, and the
// "Nächster Wecker" line for the alarms page.
std::vector<int> ParseRecurrenceDays(const std::string& recurrence);
std::string NextAlarmSummary(const std::vector<AlarmInfo>& alarms);

// Signal trampolines: each calls the std::function<…>* passed as
// user_data; the matching Delete*Callback frees it with the closure.
extern "C" void OnDialogClosed(AdwDialog*, gpointer user_data);
extern "C" void OnToggleGroupActiveChanged(GObject* object, GParamSpec*, gpointer user_data);
extern "C" void DeleteGuintCallback(gpointer data, GClosure*);
extern "C" void OnButtonRowActivated(AdwButtonRow*, gpointer user_data);
extern "C" void OnSpinRowValueChanged(GObject* object, GParamSpec*, gpointer user_data);
extern "C" void DeleteVoidCallback(gpointer data, GClosure*);
extern "C" void OnSwitchRowActiveChanged(GObject* object, GParamSpec*, gpointer user_data);
extern "C" void DeleteBoolCallback(gpointer data, GClosure*);
extern "C" void OnDoubleSpinRowValueChanged(GObject* object, GParamSpec*, gpointer user_data);
extern "C" void DeleteDoubleCallback(gpointer data, GClosure*);
extern "C" void OnEntryRowTextChanged(GObject* object, GParamSpec*, gpointer user_data);
extern "C" void DeleteStringCallback(gpointer data, GClosure*);
extern "C" void OnConfirmDialogResponse(AdwAlertDialog*, const char* response, gpointer user_data);

}  // namespace gnomos
