// SPDX-License-Identifier: GPL-3.0-or-later

#include "cover-thumbnail.h"

#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>
#include <glibmm/main.h>

#include "art-cache.h"
#include "art-decode-pool.h"
#include "artist-image-fetcher.h"

namespace gnomos
{

double CoverThumbnail::s_fallback_icon_scale = 1.0;

CoverThumbnail::CoverThumbnail(int pixel_size) : pixel_size_(pixel_size)
{
  add_css_class("card");
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
  set_pixel_size(static_cast<int>(pixel_size_ * s_fallback_icon_scale));
  set_from_icon_name(fallback_icon_name_);
}

void CoverThumbnail::SetFallbackIconName(const std::string& icon_name)
{
  fallback_icon_name_ = icon_name.empty() ? "audio-x-generic-symbolic" : icon_name;
  if (current_uri_.empty())
    ShowFallback();
}

void CoverThumbnail::LoadArtistImage(const std::string& artist_name)
{
  if (cancellable_)
    cancellable_->cancel();
  unsigned generation = ++generation_;
  auto alive = alive_;  // captured by value — see its own header comment
  ArtistImageFetcher::Instance().RequestArtistImage(artist_name, [this, generation, alive](std::string url) {
    if (!*alive)
      return;  // this CoverThumbnail was destroyed before the lookup finished
    if (generation != generation_)
      return;  // superseded by a newer SetArtUri()/LoadArtistImage() call
    SetArtUri(url);
  });
}

void CoverThumbnail::SetArtUri(const std::string& uri)
{
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
  auto file = Gio::File::create_for_uri(uri);
  auto alive = alive_;  // captured by value — see the header comment on alive_
  file->load_contents_async(
      [this, file, generation, alive](Glib::RefPtr<Gio::AsyncResult>& result) {
        if (!*alive)
          return;  // this CoverThumbnail was destroyed before the load finished
        OnLoaded(result, file, generation);
      },
      cancellable_);
}

void CoverThumbnail::OnLoaded(Glib::RefPtr<Gio::AsyncResult>& result, const Glib::RefPtr<Gio::File>& file,
                               unsigned generation)
{
  if (generation != generation_)
    return;  // superseded by a newer SetArtUri() before this load finished

  try
  {
    char* contents = nullptr;
    gsize length = 0;
    if (file->load_contents_finish(result, contents, length) && contents)
    {
      auto bytes = Glib::Bytes::create(contents, length);
      g_free(contents);
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
    else
    {
      ShowFallback();
    }
  }
  catch (const Glib::Error&)
  {
    ShowFallback();
  }
}

}  // namespace gnomos
