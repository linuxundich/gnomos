// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small helpers shared by every place that shows playback controls
// (PlayerBar, MiniPlayerWindow, the Now Playing sheet) — they used to be
// copied into each of those files' anonymous namespaces.

#include <string>

#include <gtkmm/widget.h>

#include "../backend/noson-types.h"

namespace gnomos
{

// "1:02" / "1:02:03".
std::string FormatTime(unsigned seconds);

const char* PlayPauseIconForState(TransportState state);
// Matching tooltip/accessible label: what a click will do.
const char* PlayPauseLabelForState(TransportState state);

// Mirrors the system volume icon convention (low/medium/high thresholds at
// roughly a third and two-thirds).
const char* IconForVolume(unsigned volume, bool muted);

// A thin ring drawn around a round button — the sleep timer's remaining
// time around the play button. Fraction 1 = full ring, <= 0 = hidden.
// Drawn in the current text color (style it with CSS).
class ProgressRing : public Gtk::Widget
{
public:
  ProgressRing();
  void SetFraction(double fraction);

protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

private:
  double fraction_ = 0.0;
};

// Tooltip plus accessible label in one call — an icon-only button has no
// text a screen reader could announce otherwise.
void SetButtonLabel(Gtk::Widget& widget, const std::string& label);

}  // namespace gnomos
