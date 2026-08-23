// SPDX-License-Identifier: GPL-3.0-or-later

#include "cover-thumbnail.h"

#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>

#include "art-cache.h"
#include "artist-image-fetcher.h"

namespace gnomos
{

CoverThumbnail::CoverThumbnail(int pixel_size)
: pixel_size_(pixel_size), fallback_pixel_size_(pixel_size * 3 / 5)
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

void CoverThumbnail::ShowFallback()
{
  set_pixel_size(fallback_pixel_size_);
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

  if (auto cached = ArtCache::Instance().GetScaled(uri, pixel_size_))
  {
    set(cached);
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
      // Put() first, so ArtCache has this uri's raw bytes on hand — then
      // GetScaled() re-decodes from exactly those (a cache hit, no disk
      // I/O) at this widget's own pixel_size_ rather than displaying
      // Put()'s full-resolution return value directly.
      if (ArtCache::Instance().Put(current_uri_, bytes))
      {
        if (auto scaled = ArtCache::Instance().GetScaled(current_uri_, pixel_size_))
          set(scaled);
        else
          ShowFallback();
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
