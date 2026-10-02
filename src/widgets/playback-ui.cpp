// SPDX-License-Identifier: GPL-3.0-or-later

#include "playback-ui.h"

#include <algorithm>
#include <cstdio>

#include <cmath>

#include <gtk/gtk.h>
#include <gtkmm/snapshot.h>

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

ProgressRing::ProgressRing() : Glib::ObjectBase("GnomosProgressRing"), Gtk::Widget()
{
  add_css_class("progress-ring");
  set_can_target(false);
}

void ProgressRing::SetFraction(double fraction)
{
  fraction = std::clamp(fraction, 0.0, 1.0);
  if (std::fabs(fraction - fraction_) < 0.001)
    return;
  fraction_ = fraction;
  queue_draw();
}

void ProgressRing::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot)
{
  if (fraction_ <= 0.0)
    return;
  float width = get_width(), height = get_height();
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  GdkRGBA color;
  gtk_widget_get_color(GTK_WIDGET(gobj()), &color);
  cairo_t* cr = gtk_snapshot_append_cairo(snapshot->gobj(), &bounds);
  double line = 2.5;
  double radius = std::min(width, height) / 2.0 - line / 2.0;
  // A faint full track, then the remaining time on top, clockwise from 12.
  cairo_set_line_width(cr, line);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha * 0.18);
  cairo_arc(cr, width / 2.0, height / 2.0, radius, 0, 2 * M_PI);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
  cairo_arc(cr, width / 2.0, height / 2.0, radius, -M_PI / 2, -M_PI / 2 + 2 * M_PI * fraction_);
  cairo_stroke(cr);
  cairo_destroy(cr);
}

void SetButtonLabel(Gtk::Widget& widget, const std::string& label)
{
  widget.set_tooltip_text(label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(widget.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
}

}  // namespace gnomos
