// SPDX-License-Identifier: GPL-3.0-or-later

#include "cover-thumbnail.h"

#include <cmath>
#include <map>

#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <gdkmm/memorytexture.h>
#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>
#include <glibmm/main.h>

#include "art-cache.h"
#include "art-decode-pool.h"
#include "artist-image-fetcher.h"
#include "http-fetch.h"
#include <pangomm/layout.h>

namespace gnomos
{

double CoverThumbnail::s_fallback_icon_scale = 1.0;

namespace
{

// Up to two initials: the first letter or digit of the first two words
// ("21st Century Breakdown" -> "2C", "Adele" -> "A", "#Beste" -> "B").
std::string InitialsFor(const std::string& text)
{
  Glib::ustring utext(text);
  Glib::ustring initials;
  bool at_word_start = true;
  for (gunichar c : utext)
  {
    if (g_unichar_isalnum(c))
    {
      if (at_word_start)
      {
        initials += Glib::Unicode::toupper(c);
        if (initials.size() == 2)
          break;
      }
      at_word_start = false;
    }
    else if (g_unichar_isspace(c))
    {
      at_word_start = true;
    }
  }
  return initials.empty() ? "♪" : initials.raw();
}

void HslToRgb(double h, double s, double l, double& r, double& g, double& b)
{
  auto channel = [&](double n) {
    double k = std::fmod(n + h / 30.0, 12.0);
    double a = s * std::min(l, 1.0 - l);
    return l - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
  };
  r = channel(0);
  g = channel(8);
  b = channel(4);
}

// Renders (and caches) the generated cover for seed_text. Cairo draws into
// an ARGB32 image surface, which is premultiplied BGRA in memory on the
// little-endian machines Flathub builds for — exactly GDK's
// B8G8R8A8_PREMULTIPLIED, so the pixels go into the texture as they are.
Glib::RefPtr<Gdk::Texture> GeneratedCover(const std::string& seed_text, int size)
{
  static std::map<std::string, Glib::RefPtr<Gdk::Texture>> cache;
  std::string key = std::to_string(size) + ":" + seed_text;
  if (auto it = cache.find(key); it != cache.end())
    return it->second;
  // Generated covers are cheap to redraw; a plain size cap keeps a long
  // session's map from growing without bound.
  if (cache.size() > 4000)
    cache.clear();

  guint hash = g_str_hash(seed_text.c_str());
  // Spread similar strings across the color wheel — g_str_hash() alone
  // keeps neighbors close in its low bits.
  guint mixed = hash * 2654435761u;
  double hue = (mixed >> 16) % 360;
  double r1, g1, b1, r2, g2, b2;
  HslToRgb(hue, 0.55, 0.52, r1, g1, b1);
  HslToRgb(std::fmod(hue + 40 + (hash >> 9) % 30, 360.0), 0.60, 0.32, r2, g2, b2);

  auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, size, size);
  auto cr = Cairo::Context::create(surface);
  auto gradient = Cairo::LinearGradient::create(0, 0, size, size);
  gradient->add_color_stop_rgb(0, r1, g1, b1);
  gradient->add_color_stop_rgb(1, r2, g2, b2);
  cr->set_source(gradient);
  cr->paint();

  // A soft ring in the lower right corner — a hint of a record, so the
  // tile still reads as "music" and not as a plain color swatch.
  cr->set_source_rgba(1, 1, 1, 0.10);
  cr->set_line_width(size * 0.06);
  cr->arc(size * 0.86, size * 0.86, size * 0.42, 0, 2 * M_PI);
  cr->stroke();

  auto layout = Pango::Layout::create(cr);
  Pango::FontDescription font("Adwaita Sans, Cantarell, sans-serif");
  font.set_weight(Pango::Weight::HEAVY);
  font.set_absolute_size(size * 0.34 * Pango::SCALE);
  layout->set_font_description(font);
  layout->set_text(InitialsFor(seed_text));
  int text_w = 0, text_h = 0;
  layout->get_pixel_size(text_w, text_h);
  cr->move_to((size - text_w) / 2.0, (size - text_h) / 2.0);
  cr->set_source_rgba(1, 1, 1, 0.92);
  layout->show_in_cairo_context(cr);
  surface->flush();

  int stride = surface->get_stride();
  auto bytes = Glib::Bytes::create(surface->get_data(), static_cast<gsize>(stride) * size);
  auto texture = Gdk::MemoryTexture::create(size, size, Gdk::MemoryTexture::Format::B8G8R8A8_PREMULTIPLIED, bytes,
                                            static_cast<gsize>(stride));
  cache.emplace(key, texture);
  return texture;
}

}  // namespace

Glib::RefPtr<Gdk::Texture> GeneratedCoverTexture(const std::string& seed_text, int size)
{
  return GeneratedCover(seed_text, size);
}

CoverThumbnail::CoverThumbnail(int pixel_size) : pixel_size_(pixel_size)
{
  add_css_class("card");
  add_css_class("cover-thumbnail");
  // Clips a generated cover (and real art) to the card's rounded corners.
  set_overflow(Gtk::Overflow::HIDDEN);
  ShowFallback();
}

CoverThumbnail::~CoverThumbnail()
{
  *alive_ = false;
  if (cancellable_)
    cancellable_->cancel();
}

void CoverThumbnail::SetFallbackIconScale(double scale)
{
  s_fallback_icon_scale = scale;
}

void CoverThumbnail::ShowFallback()
{
  if (!generated_seed_.empty())
  {
    set_pixel_size(pixel_size_);
    set(GeneratedCover(generated_seed_, pixel_size_ * get_scale_factor()));
    return;
  }
  set_pixel_size(static_cast<int>(pixel_size_ * s_fallback_icon_scale));
  set_from_icon_name(fallback_icon_name_);
}

void CoverThumbnail::SetFallbackIconName(const std::string& icon_name)
{
  fallback_icon_name_ = icon_name.empty() ? "audio-x-generic-symbolic" : icon_name;
  if (current_uri_.empty())
    ShowFallback();
}

void CoverThumbnail::SetGeneratedFallback(const std::string& seed_text)
{
  if (seed_text == generated_seed_)
    return;
  generated_seed_ = seed_text;
  if (current_uri_.empty())
    ShowFallback();
}

void CoverThumbnail::LoadArtistImage(const std::string& artist_name)
{
  if (cancellable_)
    cancellable_->cancel();
  unsigned generation = ++generation_;
  pending_artist_name_ = artist_name;
  auto alive = alive_;  // captured by value — see its own header comment
  ArtistImageFetcher::Instance().RequestArtistImage(artist_name, [this, generation, alive](std::string url) {
    if (!*alive)
      return;  // this CoverThumbnail was destroyed before the lookup finished
    if (generation != generation_)
      return;  // superseded by a newer SetArtUri()/LoadArtistImage() call
    SetArtUri(url);
  });
}

void CoverThumbnail::PrioritizeLoad()
{
  // Both no-ops unless actually applicable — see each's own comment for
  // when. A widget mid-artist-lookup has both fields set (the name lookup
  // still pending, current_uri_ not yet known); one still fetching its
  // image directly only has current_uri_.
  if (!pending_artist_name_.empty())
    ArtistImageFetcher::Instance().PrioritizeArtist(pending_artist_name_);
  if (!current_uri_.empty())
    HttpFetchPrioritize(current_uri_);
}

void CoverThumbnail::SetArtUri(const std::string& uri)
{
  pending_artist_name_.clear();
  if (uri == current_uri_)
    return;
  current_uri_ = uri;
  unsigned generation = ++generation_;

  if (cancellable_)
    cancellable_->cancel();

  if (uri.empty())
  {
    ShowFallback();
    return;
  }

  // GetRawBytes() is the same memory/disk lookup GetScaled() used to do
  // inline — cheap. The decode that used to follow it directly (~6ms each
  // on this system, confirmed live — see ArtDecodePool's own comment) is
  // what actually made rebuilding a large grid (bonob's/the local
  // library's "Albums", 1000+ entries, all cache hits on a revisit) freeze
  // the UI for several seconds: hundreds of these calls back-to-back,
  // synchronously, in the same tile-building loop. Decoding on a pool
  // thread instead turns that into "tiles appear immediately, art fills in
  // over the next moment" rather than one long freeze before anything
  // shows at all.
  if (auto raw_bytes = ArtCache::Instance().GetRawBytes(uri))
  {
    auto alive = alive_;  // captured by value — see the header comment on alive_
    int target_size = pixel_size_;
    ArtDecodePool::Instance().Push([this, raw_bytes, target_size, generation, alive] {
      auto texture = ArtCache::DecodeScaledTexture(raw_bytes, target_size);
      // Marshal back to the main thread before touching `this`/any GTK
      // API — decode jobs run on ArtDecodePool's own worker threads.
      Glib::signal_idle().connect_once([this, texture, generation, alive] {
        if (!*alive)
          return;  // this CoverThumbnail was destroyed before the decode finished
        if (generation != generation_)
          return;  // superseded by a newer SetArtUri()/LoadArtistImage() call
        if (texture)
          set(texture);
        else
          ShowFallback();
      });
    });
    return;
  }

  cancellable_ = Gio::Cancellable::create();
  auto alive = alive_;  // captured by value — see the header comment on alive_
  HttpFetch(
      uri,
      [this, generation, alive](std::string body) {
        if (!*alive)
          return;  // this CoverThumbnail was destroyed before the load finished
        OnLoaded(std::move(body), generation);
      },
      cancellable_);
}

void CoverThumbnail::OnLoaded(std::string body, unsigned generation)
{
  if (generation != generation_)
    return;  // superseded by a newer SetArtUri() before this load finished

  if (body.empty())
  {
    // Network error, non-2xx status, or a genuinely empty response — see
    // HttpFetch()'s own comment for why these all collapse to one signal.
    ShowFallback();
    return;
  }

  auto bytes = Glib::Bytes::create(body.data(), body.size());
  // Put() first, so ArtCache has this uri's raw bytes on hand for next
  // time — then decode those same bytes directly (no need to look them
  // back up via GetRawBytes()) on ArtDecodePool, same reasoning as
  // SetArtUri()'s own cache-hit path: a grid whose art is all fresh
  // downloads would otherwise stall the main thread once per
  // completion, exactly like the cache-hit case did before that was
  // moved off-thread too.
  if (ArtCache::Instance().Put(current_uri_, bytes))
  {
    auto alive = alive_;  // captured by value — see the header comment on alive_
    int target_size = pixel_size_;
    ArtDecodePool::Instance().Push([this, bytes, target_size, generation, alive] {
      auto texture = ArtCache::DecodeScaledTexture(bytes, target_size);
      Glib::signal_idle().connect_once([this, texture, generation, alive] {
        if (!*alive)
          return;  // this CoverThumbnail was destroyed before the decode finished
        if (generation != generation_)
          return;  // superseded by a newer SetArtUri()/LoadArtistImage() call
        if (texture)
          set(texture);
        else
          ShowFallback();
      });
    });
  }
  else
  {
    ShowFallback();
  }
}

}  // namespace gnomos
