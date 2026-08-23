// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gnomos
{

// Best-effort artist photo lookup against Deezer's public search API
// (https://api.deezer.com/search/artist) — opt-in (see GnomosWindow's own
// "Künstlerbilder laden" setting), since unlike everything else in this
// app, every lookup sends the artist's name to a third-party server over
// the internet rather than staying on the local Sonos household.
//
// Process-wide singleton, same reasoning as ArtCache: an in-memory result
// cache so revisiting "Interpreten" doesn't re-query the same few hundred
// names again. Requests are throttled to kMaxConcurrent at a time (a
// small, fixed in-process FIFO queue) rather than firing one per visible
// tile all at once — hundreds of simultaneous connections to a free
// public API the moment "Interpreten" opens would be poor citizenship and
// risks Deezer's own rate limiting kicking in for everyone using it that
// session.
//
// Deezer's own search ranking isn't popularity-aware — confirmed live,
// searching "Adele" ranks an obscure "Adèle & Zalem" (1.5k fans) above the
// real Adele (15.4M fans), and returns several *other* unrelated entries
// also literally named "Adele" with negligible fan counts. Matching
// picks the highest nb_fan among exact (case-insensitive) name matches in
// the first page of results, falling back to Deezer's own first result
// only when no exact match appears at all.
class ArtistImageFetcher
{
public:
  static ArtistImageFetcher& Instance();

  // callback fires exactly once, on the main thread — with a resolved
  // Deezer picture URL, or empty if no image could be found (name not
  // found, network error, ...). Safe to call repeatedly for the same
  // name — a cache hit resolves synchronously (before this call returns);
  // an already-in-flight or queued request just adds another waiting
  // callback rather than starting a duplicate lookup.
  void RequestArtistImage(const std::string& artist_name, std::function<void(std::string)> callback);

private:
  ArtistImageFetcher() = default;

  void StartNext();
  void OnResponse(const std::string& artist_name, const std::string& body);
  void Resolve(const std::string& artist_name, const std::string& picture_url);

  static constexpr size_t kMaxConcurrent = 2;

  // Empty string is itself a valid, cached "looked up, nothing found"
  // result — distinguished from "never looked up at all" via find()
  // against this map, not by checking the string's own emptiness.
  std::unordered_map<std::string, std::string> cache_;
  std::unordered_map<std::string, std::vector<std::function<void(std::string)>>> pending_callbacks_;
  std::deque<std::string> queue_;
  size_t in_flight_ = 0;
};

}  // namespace gnomos
