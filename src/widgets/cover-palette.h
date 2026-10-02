// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include <gdkmm/rgba.h>
#include <gdkmm/texture.h>

namespace gnomos
{

// Three colors taken from a cover image, used to tint the player bar and
// the Now Playing sheet:
//  - base:   the most prominent reasonably saturated color
//  - second: a frequent color with a clearly different hue (or a darker
//            base if the cover has only one)
//  - glow:   the brightest saturated color, for highlights
// `valid` is false for a missing/unusable texture; callers then fall back
// to the system accent color.
struct CoverPalette
{
  bool valid = false;
  Gdk::RGBA base;
  Gdk::RGBA second;
  Gdk::RGBA glow;
};

// Works on any texture size; downloads the pixels and samples a grid of at
// most ~4000 of them, so it's cheap even for a full-size cover.
CoverPalette ExtractCoverPalette(const Glib::RefPtr<Gdk::Texture>& texture);

// The same color with its HSL lightness clamped into [min_l, max_l] —
// keeps a tinted button readable on both light and dark backgrounds.
Gdk::RGBA ClampLightness(const Gdk::RGBA& color, double min_l, double max_l);

// "#rrggbb" for CSS.
std::string ToCssHex(const Gdk::RGBA& color);

// Whether white text reads better than black on `color`.
bool PrefersLightText(const Gdk::RGBA& color);

}  // namespace gnomos
