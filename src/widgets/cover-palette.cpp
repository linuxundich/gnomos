// SPDX-License-Identifier: GPL-3.0-or-later

#include "cover-palette.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace gnomos
{

namespace
{

struct Hsl
{
  double h = 0, s = 0, l = 0;
};

Hsl ToHsl(double r, double g, double b)
{
  double max = std::max({r, g, b}), min = std::min({r, g, b});
  Hsl out;
  out.l = (max + min) / 2;
  if (max == min)
    return out;
  double d = max - min;
  out.s = out.l > 0.5 ? d / (2 - max - min) : d / (max + min);
  if (max == r)
    out.h = (g - b) / d + (g < b ? 6 : 0);
  else if (max == g)
    out.h = (b - r) / d + 2;
  else
    out.h = (r - g) / d + 4;
  out.h *= 60;
  return out;
}

Gdk::RGBA FromHsl(const Hsl& hsl)
{
  auto channel = [&](double n) {
    double k = std::fmod(n + hsl.h / 30.0, 12.0);
    double a = hsl.s * std::min(hsl.l, 1.0 - hsl.l);
    return hsl.l - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
  };
  Gdk::RGBA rgba;
  rgba.set_rgba(channel(0), channel(8), channel(4), 1.0);
  return rgba;
}

double HueDistance(double a, double b)
{
  double d = std::fabs(a - b);
  return std::min(d, 360 - d);
}

struct Bucket
{
  unsigned count = 0;
  double r = 0, g = 0, b = 0;
  Hsl hsl;
};

}  // namespace

CoverPalette ExtractCoverPalette(const Glib::RefPtr<Gdk::Texture>& texture)
{
  CoverPalette palette;
  if (!texture)
    return palette;
  int width = texture->get_width(), height = texture->get_height();
  if (width <= 0 || height <= 0)
    return palette;

  // gdk_texture_download() always yields CAIRO_FORMAT_ARGB32: premultiplied,
  // native-endian 32-bit pixels — B, G, R, A in memory on little endian.
  std::vector<guchar> pixels(static_cast<size_t>(width) * height * 4);
  texture->download(pixels.data(), static_cast<gsize>(width) * 4);

  int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(width) * height / 4000.0)));
  std::map<unsigned, Bucket> buckets;
  unsigned total = 0;
  for (int y = 0; y < height; y += step)
  {
    for (int x = 0; x < width; x += step)
    {
      const guchar* p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
      guint32 value = *reinterpret_cast<const guint32*>(p);
      double a = ((value >> 24) & 0xff) / 255.0;
      if (a < 0.5)
        continue;
      double r = ((value >> 16) & 0xff) / 255.0 / a;
      double g = ((value >> 8) & 0xff) / 255.0 / a;
      double b = (value & 0xff) / 255.0 / a;
      // 3 bits per channel: 512 buckets, coarse enough that a photo's
      // noise lands in a handful of them.
      unsigned key = (static_cast<unsigned>(r * 7.99) << 6) | (static_cast<unsigned>(g * 7.99) << 3) |
                     static_cast<unsigned>(b * 7.99);
      Bucket& bucket = buckets[key];
      bucket.count++;
      bucket.r += r;
      bucket.g += g;
      bucket.b += b;
      total++;
    }
  }
  if (total == 0)
    return palette;

  std::vector<Bucket> list;
  list.reserve(buckets.size());
  for (auto& [key, bucket] : buckets)
  {
    bucket.r /= bucket.count;
    bucket.g /= bucket.count;
    bucket.b /= bucket.count;
    bucket.hsl = ToHsl(bucket.r, bucket.g, bucket.b);
    list.push_back(bucket);
  }

  auto usable = [](const Bucket& b) { return b.hsl.l > 0.15 && b.hsl.l < 0.85 && b.hsl.s > 0.18; };

  // Base: frequent *and* colorful — a mostly grey cover with a red logo
  // should still come out red, but a single stray pixel shouldn't win.
  const Bucket* base = nullptr;
  double base_score = -1;
  for (const Bucket& b : list)
  {
    if (!usable(b))
      continue;
    double score = b.count * (0.3 + b.hsl.s);
    if (score > base_score)
    {
      base_score = score;
      base = &b;
    }
  }
  bool monochrome = base == nullptr || base->count < total / 50;
  if (base == nullptr)
    base = &*std::max_element(list.begin(), list.end(),
                              [](const Bucket& a, const Bucket& b) { return a.count < b.count; });

  const Bucket* second = nullptr;
  for (const Bucket& b : list)
  {
    if (!usable(b) || HueDistance(b.hsl.h, base->hsl.h) < 40)
      continue;
    if (!second || b.count > second->count)
      second = &b;
  }

  const Bucket* glow = nullptr;
  for (const Bucket& b : list)
  {
    if (b.hsl.l < 0.45 || b.hsl.s < 0.25 || b.count < total / 200)
      continue;
    if (!glow || b.hsl.s + b.hsl.l > glow->hsl.s + glow->hsl.l)
      glow = &b;
  }

  Hsl base_hsl = base->hsl;
  // A black-and-white cover gets a neutral, slightly cool tint rather than
  // whatever near-grey bucket happened to be most frequent.
  if (monochrome)
    base_hsl.s = std::min(base_hsl.s, 0.12);
  palette.base = FromHsl(base_hsl);

  if (second)
  {
    palette.second = FromHsl(second->hsl);
  }
  else
  {
    Hsl shifted = base_hsl;
    shifted.h = std::fmod(shifted.h + 25, 360.0);
    shifted.l = std::max(0.12, shifted.l - 0.18);
    palette.second = FromHsl(shifted);
  }

  if (glow)
  {
    palette.glow = FromHsl(glow->hsl);
  }
  else
  {
    Hsl lighter = base_hsl;
    lighter.l = std::min(0.8, lighter.l + 0.25);
    palette.glow = FromHsl(lighter);
  }
  palette.valid = true;
  return palette;
}

Gdk::RGBA ClampLightness(const Gdk::RGBA& color, double min_l, double max_l)
{
  Hsl hsl = ToHsl(color.get_red(), color.get_green(), color.get_blue());
  hsl.l = std::clamp(hsl.l, min_l, max_l);
  return FromHsl(hsl);
}

std::string ToCssHex(const Gdk::RGBA& color)
{
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", static_cast<unsigned>(std::lround(color.get_red() * 255)),
                static_cast<unsigned>(std::lround(color.get_green() * 255)),
                static_cast<unsigned>(std::lround(color.get_blue() * 255)));
  return buf;
}

bool PrefersLightText(const Gdk::RGBA& color)
{
  auto linear = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  double luminance = 0.2126 * linear(color.get_red()) + 0.7152 * linear(color.get_green()) +
                     0.0722 * linear(color.get_blue());
  // Contrast against white vs. against black, WCAG style.
  return (1.05 / (luminance + 0.05)) >= ((luminance + 0.05) / 0.05);
}

}  // namespace gnomos
