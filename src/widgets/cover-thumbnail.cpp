// SPDX-License-Identifier: GPL-3.0-or-later

#include "cover-thumbnail.h"

#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>

#include "art-cache.h"

namespace gnomos
{

CoverThumbnail::CoverThumbnail(int pixel_size)
{
  set_from_icon_name("audio-x-generic-symbolic");
  set_pixel_size(pixel_size);
  add_css_class("card");
}

CoverThumbnail::~CoverThumbnail()
{
  *alive_ = false;
  if (cancellable_)
    cancellable_->cancel();
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
    set_from_icon_name("audio-x-generic-symbolic");
    return;
  }

  if (auto cached = ArtCache::Instance().Get(uri))
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
      auto texture = ArtCache::Instance().Put(current_uri_, bytes);
      if (texture)
        set(texture);
      else
        set_from_icon_name("audio-x-generic-symbolic");
    }
    else
    {
      set_from_icon_name("audio-x-generic-symbolic");
    }
  }
  catch (const Glib::Error&)
  {
    set_from_icon_name("audio-x-generic-symbolic");
  }
}

}  // namespace gnomos
