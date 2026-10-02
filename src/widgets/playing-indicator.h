// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtkmm/widget.h>

namespace gnomos
{

// Three little bars bobbing up and down — "this is what's playing". Shown
// next to the current queue entry, a playing room in the room popover and
// (with libadwaita 1.10) the queue's sidebar item. Purely a state display:
// Gnomos never hears the audio, so the bars follow a fixed, gently
// irregular rhythm rather than pretending to be a level meter. Paused, they
// rest at different heights; with reduced motion they don't move at all.
// Drawn in the current text color, so it picks up accent or tint from CSS.
class PlayingIndicator : public Gtk::Widget
{
public:
  PlayingIndicator();
  ~PlayingIndicator() override;

  void SetPlaying(bool playing);

protected:
  Gtk::SizeRequestMode get_request_mode_vfunc() const override;
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

private:
  void UpdateTicking();

  bool playing_ = false;
  double time_ = 0.0;
  gint64 last_frame_time_ = 0;
  guint tick_id_ = 0;
};

}  // namespace gnomos
