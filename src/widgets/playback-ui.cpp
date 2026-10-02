// SPDX-License-Identifier: GPL-3.0-or-later

#include "playback-ui.h"

#include <cstdio>

#include <gtk/gtk.h>

namespace gnomos
{

std::string FormatTime(unsigned seconds)
{
  unsigned h = seconds / 3600;
  unsigned m = (seconds % 3600) / 60;
  unsigned s = seconds % 60;
  char buf[16];
  if (h > 0)
    std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
  else
    std::snprintf(buf, sizeof(buf), "%u:%02u", m, s);
  return buf;
}

const char* PlayPauseIconForState(TransportState state)
{
  return state == TransportState::Playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
}

const char* PlayPauseLabelForState(TransportState state)
{
  return state == TransportState::Playing ? "Pause" : "Abspielen";
}

const char* IconForVolume(unsigned volume, bool muted)
{
  if (muted || volume == 0)
    return "audio-volume-muted-symbolic";
  if (volume < 34)
    return "audio-volume-low-symbolic";
  if (volume < 67)
    return "audio-volume-medium-symbolic";
  return "audio-volume-high-symbolic";
}

void SetButtonLabel(Gtk::Widget& widget, const std::string& label)
{
  widget.set_tooltip_text(label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(widget.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
}

}  // namespace gnomos
