// SPDX-License-Identifier: GPL-3.0-or-later


#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

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

#include "window-helpers.h"
#include "i18n.h"

#include "config.h"
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

// Attaches an action directly onto an AdwSidebarItem (read back on
// selection by OnNavSidebarSelectedChanged()) — replaces the old
// nav_list_box_/nav_row_actions_ index-matched parallel-vector pattern
// (see nav_sidebar_'s own comment) with the action living on the item
// itself, so nothing can ever misindex.
constexpr const char* kSidebarItemActionKey = "gnomos-sidebar-action";

void DeleteSidebarItemAction(gpointer data)
{
  delete static_cast<std::function<void()>*>(data);
}

void SetSidebarItemAction(AdwSidebarItem* item, std::function<void()> action)
{
  g_object_set_data_full(G_OBJECT(item), kSidebarItemActionKey, new std::function<void()>(std::move(action)),
                          DeleteSidebarItemAction);
}

extern "C" void OnNavSidebarSelectedChanged(GObject* sidebar_obj, GParamSpec*, gpointer)
{
  AdwSidebarItem* item = adw_sidebar_get_selected_item(ADW_SIDEBAR(sidebar_obj));
  if (!item)
    return;
  auto* action = static_cast<std::function<void()>*>(g_object_get_data(G_OBJECT(item), kSidebarItemActionKey));
  if (action)
    (*action)();
}

// A radio-like NowPlaying::artist holds the station's own raw rotating
// "now playing" content, not a real artist name — see
// RadioContentFilter's own header comment: when it's an actual song
// (rather than ad/ident filler), it follows a "<title> / <artist>"
// convention, confirmed live (e.g. "Ho hey / The Lumineers" for SWR3).
// Splits on the first " / " and fills title/artist from it; returns false
// — nothing usable — for ad text or any station that doesn't follow this
// convention at all, so a lyrics lookup simply isn't attempted rather than
// searching on raw filler text.
bool ParseRadioSongContent(const std::string& content, std::string& title, std::string& artist)
{
  size_t sep = content.find(" / ");
  if (sep == std::string::npos)
    return false;
  title = content.substr(0, sep);
  artist = content.substr(sep + 3);
  return !title.empty() && !artist.empty();
}

// AdwDialog's "closed" signal — (AdwDialog*, gpointer), unlike every other
// signal trampoline above this point in the file, which are all
// "notify::property" (GObject*, GParamSpec*, gpointer). DeleteVoidCallback
// (defined near ShowSettingsDialog, in the same translation-unit-wide
// anonymous namespace) already matches std::function<void()>'s own
// cleanup, so only the actual invoking trampoline is new here. Shared by
// this dialog and ShowArtistInfoDialog() further down.
extern "C" void OnDialogClosed(AdwDialog*, gpointer user_data)
{
  (*static_cast<std::function<void()>*>(user_data))();
}

std::string StateFilePath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "state.ini");
}

std::string HistoryFilePath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "history.ini");
}

std::string ScenesFilePath()
{
  return Glib::build_filename(Glib::get_user_config_dir(), "gnomos", "scenes.ini");
}

// Recurrence strings are comma-separated 3-letter day abbreviations (see
// NSROOT::DayTable in alarm.h) — substring search is safe here since none
// of the seven tokens is a substring of another.
std::vector<int> ParseRecurrenceDays(const std::string& recurrence)
{
  static const std::array<std::pair<const char*, int>, 7> kDayTokens = {{
      {"SUN", 0},
      {"MON", 1},
      {"TUE", 2},
      {"WED", 3},
      {"THU", 4},
      {"FRI", 5},
      {"SAT", 6},
  }};
  std::vector<int> days;
  for (const auto& [token, value] : kDayTokens)
    if (recurrence.find(token) != std::string::npos)
      days.push_back(value);
  return days;
}

// Soonest enabled alarm's own summary ("Heute, 07:00 Uhr — Küche" /
// "Morgen, ..." / a weekday name), or empty if there are no enabled
// alarms with parseable recurrence days — an alarm using a recurrence
// Gnomos never itself generates (e.g. a literal "ONCE" from the official
// Sonos app rather than a day list) is silently skipped rather than
// guessed at, since ParseRecurrenceDays() would return no days for it.
std::string NextAlarmSummary(const std::vector<AlarmInfo>& alarms)
{
  std::time_t now_time = std::time(nullptr);
  std::tm now_tm{};
  localtime_r(&now_time, &now_tm);
  int now_wday = now_tm.tm_wday;
  int now_minutes = now_tm.tm_hour * 60 + now_tm.tm_min;

  static const std::array<const char*, 7> kWeekdayNames = {N_("Sunday"),   N_("Monday"), N_("Tuesday"),
                                                             N_("Wednesday"), N_("Thursday"), N_("Friday"),
                                                             N_("Saturday")};

  int best_day_offset = -1;
  int best_minutes = -1;
  const AlarmInfo* best_alarm = nullptr;
  for (const AlarmInfo& alarm : alarms)
  {
    if (!alarm.enabled)
      continue;
    int hour = 0, minute = 0;
    if (std::sscanf(alarm.start_time.c_str(), "%d:%d", &hour, &minute) != 2)
      continue;
    int alarm_minutes = hour * 60 + minute;
    std::vector<int> days = ParseRecurrenceDays(alarm.recurrence);
    if (days.empty())
      continue;
    for (int offset = 0; offset < 8; ++offset)
    {
      int wday = (now_wday + offset) % 7;
      if (std::find(days.begin(), days.end(), wday) == days.end())
        continue;
      if (offset == 0 && alarm_minutes < now_minutes)
        continue;  // today's own slot already passed — next real match is a week from now
      if (best_day_offset < 0 || offset < best_day_offset ||
          (offset == best_day_offset && alarm_minutes < best_minutes))
      {
        best_day_offset = offset;
        best_minutes = alarm_minutes;
        best_alarm = &alarm;
      }
      break;  // this alarm's own soonest occurrence found — move to the next alarm
    }
  }

  if (!best_alarm)
    return "";

  char time_buf[16];
  std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", best_minutes / 60, best_minutes % 60);
  const char* when = best_day_offset == 0    ? _("Today")
                     : best_day_offset == 1 ? _("Tomorrow")
                                             : _(kWeekdayNames[(now_wday + best_day_offset) % 7]);
  // Translators: day, time ("07:30"), room — e.g. "Tomorrow, 07:30 — Kitchen"
  return Format(_("%s, %s — %s"), when, time_buf, best_alarm->room_name.c_str());
}

// notify::active has no gtkmm binding on AdwToggleGroup (an Adw-only
// widget) — same "raw GObject signal + trampoline" approach as
// OnConfirmDialogResponse above.
extern "C" void OnToggleGroupActiveChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(guint)>*>(user_data);
  (*callback)(adw_toggle_group_get_active(ADW_TOGGLE_GROUP(object)));
}
extern "C" void DeleteGuintCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(guint)>*>(data);
}
extern "C" void OnButtonRowActivated(AdwButtonRow*, gpointer user_data)
{
  (*static_cast<std::function<void()>*>(user_data))();
}
// Cache size row: pushes the new value into ArtCache and refreshes the
// "N MB belegt" subtitle, since SetMaxDiskMb() may have just evicted files.
extern "C" void OnSpinRowValueChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  ArtCache::Instance().SetMaxDiskMb(static_cast<unsigned>(adw_spin_row_get_value(ADW_SPIN_ROW(object))));
  (*static_cast<std::function<void()>*>(user_data))();
}
extern "C" void DeleteVoidCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void()>*>(data);
}
extern "C" void OnSwitchRowActiveChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(bool)>*>(user_data);
  (*callback)(adw_switch_row_get_active(ADW_SWITCH_ROW(object)));
}
extern "C" void DeleteBoolCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(bool)>*>(data);
}
extern "C" void OnDoubleSpinRowValueChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(double)>*>(user_data);
  (*callback)(adw_spin_row_get_value(ADW_SPIN_ROW(object)));
}
extern "C" void DeleteDoubleCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(double)>*>(data);
}
// AdwEntryRow implements GtkEditable rather than exposing its own text
// property/signal, same "raw GObject + trampoline" approach as the rows
// above.
extern "C" void OnEntryRowTextChanged(GObject* object, GParamSpec*, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(const std::string&)>*>(user_data);
  const char* text = gtk_editable_get_text(GTK_EDITABLE(object));
  (*callback)(text ? text : "");
}
extern "C" void DeleteStringCallback(gpointer data, GClosure*)
{
  delete static_cast<std::function<void(const std::string&)>*>(data);
}

// AdwAlertDialog's "response" signal has no gtkmm binding (it's a plain
// GObject signal, not one of the widget signals gtkmm wraps) — this
// trampoline bridges it to a heap-allocated std::function, freed here once
// the single response has been delivered. AdwAlertDialog only ever emits
// "response" once per dialog (any response, including the close one),
// so there's no risk of a dangling callback firing twice.
extern "C" void OnConfirmDialogResponse(AdwAlertDialog*, const char* response, gpointer user_data)
{
  auto* callback = static_cast<std::function<void(std::string)>*>(user_data);
  (*callback)(response != nullptr ? response : "");
  delete callback;
}

}  // namespace gnomos
