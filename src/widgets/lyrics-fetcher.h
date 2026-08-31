// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <giomm/cancellable.h>

namespace gnomos
{

// Best-effort plain-lyrics lookup against LRCLIB's public API
// (https://lrclib.net/api/search) — opt-in (see GnomosWindow's own
// "Songtexte laden" setting), since like ArtistImageFetcher's Deezer
// lookups, every request sends the current track's artist/title/album to a
// third-party server over the internet rather than staying on the local
// Sonos household.
//
// Process-wide singleton with a simple in-memory result cache, same
// reasoning as ArtistImageFetcher — reopening the "Titel-Details" dialog
// for a track already looked up this session resolves instantly. No
// queue/concurrency throttle of its own: unlike ArtistImageFetcher (which
// can fire off one request per grid tile, hundreds at once), this is only
// ever triggered one at a time by opening the track-info dialog, and
// HttpFetch() already has its own shared concurrency cap.
class LyricsFetcher
{
public:
  static LyricsFetcher& Instance();

  // callback fires exactly once, on the main thread, with plain-text
  // lyrics or an empty string if none could be found (no match,
  // instrumental track, network error, ...). Safe to call repeatedly for
  // the same artist/title — a cache hit resolves synchronously (before
  // this call returns). `cancellable`, if given, is forwarded to
  // HttpFetch() so a dialog closed before the response arrives can drop
  // the result — see HttpFetch()'s own comment.
  void RequestLyrics(const std::string& artist, const std::string& title, const std::string& album,
                      std::function<void(std::string)> callback,
                      const Glib::RefPtr<Gio::Cancellable>& cancellable = {});

private:
  LyricsFetcher() = default;

  // Empty string is itself a valid, cached "looked up, nothing found"
  // result — same convention as ArtistImageFetcher::cache_.
  std::unordered_map<std::string, std::string> cache_;
};

}  // namespace gnomos
