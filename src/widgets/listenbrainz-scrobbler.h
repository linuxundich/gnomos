// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <string>

namespace gnomos
{

// Fire-and-forget submission of a single "listen" (scrobble) to
// ListenBrainz (https://listenbrainz.org) — opt-in (see GnomosWindow's own
// "ListenBrainz-Benutzer-Token" setting: empty means disabled, no separate
// switch needed since the token itself is required for every call). Unlike
// every other external lookup in this app (Deezer, LRCLIB, radio-browser.info),
// this *sends* listening history rather than fetching something — the
// same disclosure reasoning applies (see ArtistImageFetcher's own header),
// just in the other direction.
//
// Uses its own SoupSession rather than HttpFetch()'s shared one: HttpFetch()
// is GET-only (see its own comment), and this needs POST with a JSON body
// and an Authorization header, both outside its scope. Call volume here is
// low (at most one submission per genuinely-finished track), so a
// dedicated session with no concurrency cap of its own is fine.
class ListenBrainzScrobbler
{
public:
  static ListenBrainzScrobbler& Instance();

  // No callback — the caller has nothing more specific to do differently
  // on success vs. failure (a dropped scrobble isn't worth retrying or
  // surfacing to the user), so this fires the request and moves on.
  // album may be empty (not every track has one, e.g. a single).
  void Scrobble(const std::string& token, const std::string& artist, const std::string& track,
                const std::string& album, std::chrono::system_clock::time_point listened_at);

private:
  ListenBrainzScrobbler() = default;
};

}  // namespace gnomos
