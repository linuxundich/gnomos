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

// Tooltip plus accessible label in one call — an icon-only button has no
// text a screen reader could announce otherwise.
void SetButtonLabel(Gtk::Widget& widget, const std::string& label);

}  // namespace gnomos
