// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gnomos
{

// One related artist — from Deezer's own /artist/{id}/related, see
// RequestRelatedArtists()'s own comment.
struct RelatedArtist
{
  std::string name;
  std::string picture_url;  // Deezer's "picture_medium" — may be empty
};

// Backs ShowArtistInfoDialog()'s "Über den Interpreten" panel — two
// independent, unrelated lookups bundled into one fetcher only because
// they're always requested together for the same dialog:
//
// - RequestBio(): Last.fm's artist.getInfo, opt-in via the *same*
//   api_key already configured for Last.fm scrobbling (see
//   GnomosWindow::lastfm_api_key_) — no separate key needed for this
//   read-only lookup, and unlike auth.getToken/getSession, artist.getInfo
//   needs no api_sig at all (confirmed live: an unsigned request got a
//   substantive "Invalid API key" response, not a "missing parameter"
//   one).
// - RequestRelatedArtists(): Deezer's public API, same one
//   ArtistImageFetcher already uses for photos, no key needed at all —
//   resolves the artist name to a Deezer ID via search first (same
//   exact-match-by-name/most-fans heuristic ArtistImageFetcher itself
//   uses, duplicated here rather than shared since the two classes
//   otherwise have nothing in common), then fetches that artist's own
//   related list.
class ArtistInfoFetcher
{
public:
  static ArtistInfoFetcher& Instance();

  // callback fires once, on the main thread, with plain text (Last.fm's
  // own trailing "Read more on Last.fm" link stripped) or empty — no
  // api_key configured, no bio found, or a network error are all
  // indistinguishable here, same as every other best-effort fetcher in
  // this app.
  void RequestBio(const std::string& api_key, const std::string& artist_name,
                   std::function<void(std::string bio)> callback);

  // callback fires once with up to 10 related artists (possibly empty —
  // no match found, or Deezer reports none).
  void RequestRelatedArtists(const std::string& artist_name, std::function<void(std::vector<RelatedArtist>)> callback);

private:
  ArtistInfoFetcher() = default;
};

}  // namespace gnomos
