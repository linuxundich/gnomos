// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include <giomm/asyncresult.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <glibmm/refptr.h>
#include <gtkmm/image.h>

namespace gnomos
{

// Small async-loading cover art thumbnail for a list row (Queue/Favorites/
// Library), styled like Euphonica's own row thumbnails
// (https://github.com/htkhiem/euphonica). Same async-load-with-cancel
// mechanics as PlayerBar's own art loading, extracted here since three
// different views need it rather than three near-identical copies of the
// same ~30 lines.
class CoverThumbnail : public Gtk::Image
{
public:
  // pixel_size: 40 for a list row (the default); grid tiles (Albums/Artists,
  // see LibraryView) pass something larger.
  explicit CoverThumbnail(int pixel_size = 40);
  ~CoverThumbnail() override;

  // Empty uri shows the fallback icon. Safe to call repeatedly (e.g. from
  // a recycled row) — a superseded in-flight load is cancelled.
  void SetArtUri(const std::string& uri);

private:
  void OnLoaded(Glib::RefPtr<Gio::AsyncResult>& result, const Glib::RefPtr<Gio::File>& file, unsigned generation);

  std::string current_uri_;
  unsigned generation_ = 0;
  Glib::RefPtr<Gio::Cancellable> cancellable_;
  // Confirmed live: unlike PlayerBar's own single, never-destroyed art
  // widget, these rows get destroyed and rebuilt wholesale on every list
  // refresh — cancelling cancellable_ in the destructor does NOT stop an
  // already-scheduled async completion callback from still firing on the
  // main loop afterward (Gio still invokes it, just with a "cancelled"
  // result), and reading generation_ to detect that requires `this` to
  // still be valid memory in the first place, which it might not be by
  // then. alive_ is captured *by value* in the async lambda (its own heap
  // block, independent of the widget itself) and flipped to false in the
  // destructor, so the callback can check it before ever touching `this` —
  // this is the actual segfault fix, not the generation counter.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace gnomos
