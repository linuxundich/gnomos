// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gnomos
{

struct RadioBrowserStation
{
  std::string name;
  std::string url;          // stream URL, ready to hand to NosonBackend::AddRadioStation()
  std::string countrycode;  // ISO 3166-1 alpha-2, e.g. "DE" — empty if the station carries none
  std::string tags;
  std::string codec;
  int bitrate = 0;
  // A station-provided logo URL — often empty even when the station
  // itself is otherwise complete (a directory entry, not something every
  // submitter bothers to set). Passed straight through to
  // NosonBackend::AddRadioStation() when adding this result; see that
  // method's own comment for why it isn't sent to Sonos itself.
  std::string favicon;
};

struct RadioBrowserCountry
{
  std::string name;
  std::string countrycode;
  int station_count = 0;
};

// A free-text genre/style tag as the directory's own submitters have
// entered it (e.g. "pop", "classical", "80s") — not a curated taxonomy, so
// station_count varies wildly and near-duplicates exist (see FetchTags()'s
// own comment). Passed straight through to SearchStations()'s own tag
// filter.
struct RadioBrowserTag
{
  std::string name;
  int station_count = 0;
};

// Thin client for the public Radio-Browser API (https://www.radio-browser.info),
// a free, crowd-sourced directory of internet radio streams — backs the
// "Radiosender hinzufügen" dialog's search, replacing typing in a name and
// stream URL by hand as the primary way to add a station (that manual entry
// stays available as a fallback for anything the directory doesn't carry).
// Every method here is a genuine internet request, no different in kind
// from ArtistImageFetcher's own Deezer lookups — see that class's header
// for the same privacy/disclosure reasoning; unlike artist photos, this one
// only ever fires from an explicit user action (opening the dialog, typing
// a search), never automatically in the background, so it has no separate
// opt-in setting of its own — the dialog itself names the endpoint.
//
// Hits https://all.api.radio-browser.info directly rather than resolving a
// specific mirror via the DNS SRV-based server discovery the project's own
// docs recommend for heavier clients — reasonable for this app's usage
// (occasional, one-off, stateless searches, no pagination continuity to
// keep consistent across requests), and considerably simpler than
// implementing SRV lookups just for that.
class RadioBrowserService
{
public:
  static RadioBrowserService& Instance();

  // callback fires exactly once, on the main thread, with whatever the API
  // returned (possibly empty — no results, or a network/parse error, are
  // indistinguishable here since there's nothing more specific for a caller
  // to do differently either way). name/countrycode/tag may all be empty
  // (browse everything) — results are always ordered by click count
  // (descending), so an all-empty call itself already doubles as "browse
  // globally popular stations" with no separate endpoint needed. At least
  // one filter is recommended in practice, or the full unfiltered directory
  // (tens of thousands of stations) gets capped at kMaxResults essentially
  // at random.
  void SearchStations(const std::string& name, const std::string& countrycode, const std::string& tag,
                       std::function<void(std::vector<RadioBrowserStation>)> callback);

  // Full country list with station counts, for the dialog's own country
  // picker — fetched once and cached for the process's lifetime (country
  // boundaries don't change during a single run, and station counts being
  // slightly stale is harmless for a picker).
  void FetchCountries(std::function<void(std::vector<RadioBrowserCountry>)> callback);

  // Same shape/caching as FetchCountries() above, for the dialog's genre
  // picker — user-submitted free text, not curated (near-duplicates like
  // "pop"/"Pop"/"pop music" all appear as distinct entries with their own
  // counts), so the caller decides how much of the list is worth showing
  // rather than this method trying to deduplicate or normalize it.
  void FetchTags(std::function<void(std::vector<RadioBrowserTag>)> callback);

private:
  RadioBrowserService() = default;

  static constexpr int kMaxResults = 100;

  bool countries_loaded_ = false;
  std::vector<RadioBrowserCountry> countries_cache_;
  std::vector<std::function<void(std::vector<RadioBrowserCountry>)>> pending_country_callbacks_;

  bool tags_loaded_ = false;
  std::vector<RadioBrowserTag> tags_cache_;
  std::vector<std::function<void(std::vector<RadioBrowserTag>)>> pending_tag_callbacks_;
};

}  // namespace gnomos
