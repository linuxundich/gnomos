// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include <giomm/cancellable.h>
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

  // Bumps this thumbnail's own pending fetch (if any — a no-op if it
  // already resolved, or never needed one at all) to the front of
  // HttpFetch's shared queue. LibraryView calls this for whatever's
  // actually visible after a scroll settles — see its own comment for why:
  // a large grid (1000+ entries) queues every tile's fetch up front in
  // index order, so jumping far ahead by scrolling would otherwise mean
  // waiting behind everything already queued between the old and new
  // position.
  void PrioritizeLoad();

  // Global, shared across every CoverThumbnail instance — how large a
  // fallback icon's own *glyph* renders relative to its pixel_size_ box,
  // as a fraction (1.0 = full size). Only affects icon-name rendering
  // (ShowFallback()); real texture content (a downloaded photo, a
  // service's own icon image) always fills its box regardless, via
  // ArtCache::GetScaled(). GnomosWindow's own Settings row calls this
  // whenever the user changes "Symbolgröße" — see its own state.ini entry.
  // Confirmed live: some fallback icons (e.g. "audio-x-generic-symbolic")
  // have a glyph that fills nearly their whole square canvas, unlike a
  // more generously-padded one (an avatar silhouette, a disc), so the
  // *same* pixel_size_ can still read as visually bigger — this exists so
  // that's the user's own call to make rather than a fixed value baked in
  // here, after an earlier fixed 3/5 shrink (since reverted) turned out to
  // be the wrong fix for what was actually being reported.
  static void SetFallbackIconScale(double scale);

private:
  void OnLoaded(std::string body, unsigned generation);
  // Switches to fallback_icon_name_, sized by pixel_size_ *
  // s_fallback_icon_scale — every "nothing to show" case (empty uri,
  // failed load) goes through this rather than calling
  // set_from_icon_name() directly, so that scaling can't accidentally get
  // skipped at one of the call sites.
  void ShowFallback();

  static double s_fallback_icon_scale;

  // Constructor's own pixel_size argument, kept for ArtCache::GetScaled()
  // calls later — a plain Gdk::Texture set via Gtk::Image::set() has no
  // size cap of its own (unlike icon-name/GIcon rendering, which
  // set_pixel_size() already handles), so every real image load needs to
  // ask ArtCache to decode already scaled to this, rather than relying on
  // the widget to shrink an arbitrarily-sized source image on its own.
  int pixel_size_ = 40;
  std::string fallback_icon_name_ = "audio-x-generic-symbolic";
  std::string current_uri_;
  // Set by LoadArtistImage(), cleared once SetArtUri() actually runs (the
  // lookup resolved, one way or another) — lets PrioritizeLoad() reach
  // ArtistImageFetcher's own queue during the name-lookup stage, when
  // current_uri_ is still empty and there's nothing yet for
  // HttpFetchPrioritize() to act on.
  std::string pending_artist_name_;
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
