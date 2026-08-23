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
  // What to show for an empty uri or a failed load — a generic note glyph
  // by default, but LibraryEntry::icon_name (derived from the item's own
  // DIDL subType — artist/album/genre/playlist/folder) gives a much more
  // specific, GNOME-consistent icon per entry when the caller knows one.
  // Call before SetArtUri() (or it won't take effect until the next call,
  // since SetArtUri() only re-renders the fallback for an already-empty
  // uri if the uri actually *changes* — see its own early-return check).
  void SetFallbackIconName(const std::string& icon_name);
  // Resolves artist_name to a real photo via ArtistImageFetcher (Deezer —
  // see its own header for the opt-in/privacy reasoning), then loads it
  // the same way SetArtUri() would once resolved. Call this *instead of*
  // SetArtUri() when the caller has both an artist-name hint and the
  // "load artist images" preference on; the fallback icon (see
  // SetFallbackIconName()) stays showing for however long the lookup
  // takes, same as it would for any other pending load.
  void LoadArtistImage(const std::string& artist_name);

private:
  void OnLoaded(Glib::RefPtr<Gio::AsyncResult>& result, const Glib::RefPtr<Gio::File>& file, unsigned generation);
  // Switches to fallback_icon_name_ at fallback_pixel_size_ — every
  // "nothing to show" case (empty uri, failed load) goes through this
  // rather than calling set_from_icon_name() directly, so the smaller
  // fallback size (see fallback_pixel_size_'s own comment) can't
  // accidentally get skipped at one of the call sites.
  void ShowFallback();

  // Constructor's own pixel_size argument, kept for ArtCache::GetScaled()
  // calls later — a plain Gdk::Texture set via Gtk::Image::set() has no
  // size cap of its own (unlike icon-name/GIcon rendering, which
  // set_pixel_size() already handles), so every real image load needs to
  // ask ArtCache to decode already scaled to this, rather than relying on
  // the widget to shrink an arbitrarily-sized source image on its own.
  int pixel_size_ = 40;
  // Smaller than pixel_size_ on purpose — confirmed live: a symbolic
  // fallback icon (e.g. "audio-x-generic-symbolic", whose glyph fills
  // most of its own square canvas) rendered at the *full* pixel_size_
  // reads as visibly larger than a neighboring tile showing real
  // downloaded art or a more generously-padded icon (an avatar silhouette,
  // say), even though both are the exact same pixel_size_ box. Icons meant
  // to sit inside a fixed-size thumbnail slot need their own margin, the
  // same way a padded icon already carries one built into its own SVG.
  int fallback_pixel_size_ = 24;
  std::string fallback_icon_name_ = "audio-x-generic-symbolic";
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
