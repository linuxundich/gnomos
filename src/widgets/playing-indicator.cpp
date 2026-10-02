// SPDX-License-Identifier: GPL-3.0-or-later

#include "playing-indicator.h"

#include <cmath>

#include <gdkmm/frameclock.h>
#include <gtkmm/settings.h>
#include <gtkmm/snapshot.h>

namespace gnomos
{

PlayingIndicator::PlayingIndicator() : Glib::ObjectBase("GnomosPlayingIndicator"), Gtk::Widget()
{
  add_css_class("playing-indicator");
  set_valign(Gtk::Align::CENTER);
  set_can_target(false);
  gtk_accessible_update_property(GTK_ACCESSIBLE(gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, "Spielt gerade", -1);
  signal_map().connect(sigc::mem_fun(*this, &PlayingIndicator::UpdateTicking));
  signal_unmap().connect(sigc::mem_fun(*this, &PlayingIndicator::UpdateTicking));
}

PlayingIndicator::~PlayingIndicator()
{
  if (tick_id_)
    remove_tick_callback(tick_id_);
}

void PlayingIndicator::SetPlaying(bool playing)
{
  if (playing == playing_)
    return;
  playing_ = playing;
  UpdateTicking();
  queue_draw();
}

// Only ticks while it's both playing and on screen — a hidden row's
// indicator costs nothing.
void PlayingIndicator::UpdateTicking()
{
  auto settings = Gtk::Settings::get_default();
  bool animations = !settings || settings->property_gtk_enable_animations().get_value();
  bool tick = playing_ && animations && get_mapped();
  if (tick && !tick_id_)
  {
    last_frame_time_ = 0;
    tick_id_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>& clock) {
      gint64 now = clock->get_frame_time();
      if (last_frame_time_ != 0)
        time_ += (now - last_frame_time_) / 1e6;
      last_frame_time_ = now;
      queue_draw();
      return true;
    });
  }
  else if (!tick && tick_id_)
  {
    remove_tick_callback(tick_id_);
    tick_id_ = 0;
  }
}

Gtk::SizeRequestMode PlayingIndicator::get_request_mode_vfunc() const
{
  return Gtk::SizeRequestMode::CONSTANT_SIZE;
}

void PlayingIndicator::measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural, int& minimum_baseline,
                                     int& natural_baseline) const
{
  minimum = natural = 16;
  minimum_baseline = natural_baseline = -1;
}

void PlayingIndicator::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot_ref)
{
  GtkSnapshot* snapshot = snapshot_ref->gobj();
  float width = get_width(), height = get_height();
  float size = std::min(width, height);
  float x0 = (width - size) / 2, y0 = (height - size) / 2;
  GdkRGBA color;
  gtk_widget_get_color(GTK_WIDGET(gobj()), &color);

  constexpr int kBars = 3;
  float gap = size * 0.14f;
  float bar_width = (size - gap * (kBars - 1)) / kBars;
  // Two sine waves per bar at unrelated frequencies — never quite repeats,
  // which reads as music rather than as a loading animation.
  static constexpr double kFreqA[kBars] = {2.1, 2.9, 1.7};
  static constexpr double kFreqB[kBars] = {3.7, 1.3, 4.3};
  static constexpr double kRest[kBars] = {0.45, 0.8, 0.6};
  for (int i = 0; i < kBars; ++i)
  {
    double level = kRest[i];
    if (playing_ && tick_id_)
      level = 0.55 + 0.25 * std::sin(time_ * kFreqA[i] * M_PI + i) + 0.2 * std::sin(time_ * kFreqB[i] * M_PI);
    level = std::clamp(level, 0.18, 1.0);
    float h = static_cast<float>(size * level);
    graphene_rect_t rect = GRAPHENE_RECT_INIT(x0 + i * (bar_width + gap), y0 + size - h, bar_width, h);
    GskRoundedRect rounded;
    gsk_rounded_rect_init_from_rect(&rounded, &rect, std::min(bar_width / 2, 1.5f));
    gtk_snapshot_push_rounded_clip(snapshot, &rounded);
    gtk_snapshot_append_color(snapshot, &color, &rect);
    gtk_snapshot_pop(snapshot);
  }
}

}  // namespace gnomos
