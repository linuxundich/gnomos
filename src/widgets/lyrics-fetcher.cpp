// SPDX-License-Identifier: GPL-3.0-or-later

#include "lyrics-fetcher.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include <glibmm/main.h>
#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>

#include "http-fetch.h"

namespace gnomos
{

namespace
{
std::string ToLower(const std::string& s)
{
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  return lower;
}

// Strips one trailing "(...)"/"[...]" group (and any whitespace right
// before it) from a title or album string, if the string ends with one —
// a release-group/rip-source tag like "(PMEDIA)" can land in *either*
// field depending on what a linked service (e.g. bonob) reports, and
// LRCLIB's search treats both as real filters rather than ignoring the
// noise in them (confirmed live for both: "Invaincu (PMEDIA)" as the
// track title itself found nothing until stripped down to "Invaincu";
// "Santé" with album "Multitude (PMEDIA)" needed the album stripped
// instead). Returns the input unchanged if it doesn't end in a bracket, or
// if the brackets don't balance (better to leave odd input alone than
// mis-strip it).
std::string StripTrailingBracketedSuffix(const std::string& s)
{
  if (s.empty())
    return s;
  char close = s.back();
  char open = close == ')' ? '(' : close == ']' ? '[' : '\0';
  if (!open)
    return s;

  int depth = 0;
  for (std::string::size_type i = s.size(); i-- > 0;)
  {
    if (s[i] == close)
      ++depth;
    else if (s[i] == open && --depth == 0)
    {
      std::string stripped = s.substr(0, i);
      while (!stripped.empty() && std::isspace(static_cast<unsigned char>(stripped.back())))
        stripped.pop_back();
      return stripped;
    }
  }
  return s;  // unbalanced — leave as-is
}

// Parses LRC text ("[01:23.45] line") into (ms, line) pairs. Lines
// without a timestamp (metadata tags like "[ar:...]", blank lines) are
// skipped; a line with several timestamps (a repeated chorus written once)
// is added once per timestamp, then everything is sorted by time.
std::vector<std::pair<unsigned, std::string>> ParseLrc(const std::string& lrc)
{
  std::vector<std::pair<unsigned, std::string>> lines;
  std::istringstream stream(lrc);
  std::string raw;
  while (std::getline(stream, raw))
  {
    std::vector<unsigned> stamps;
    size_t pos = 0;
    while (pos < raw.size() && raw[pos] == '[')
    {
      size_t close = raw.find(']', pos);
      if (close == std::string::npos)
        break;
      unsigned minutes = 0, seconds = 0, fraction = 0;
      int fraction_digits = 0;
      std::string tag = raw.substr(pos + 1, close - pos - 1);
      if (std::sscanf(tag.c_str(), "%u:%u.%n", &minutes, &seconds, &fraction_digits) >= 2)
      {
        size_t dot = tag.find('.');
        std::string frac = dot == std::string::npos ? "" : tag.substr(dot + 1);
        fraction = frac.empty() ? 0 : static_cast<unsigned>(std::stoul(frac));
        unsigned ms = frac.size() == 1 ? fraction * 100 : frac.size() == 2 ? fraction * 10 : fraction;
        stamps.push_back((minutes * 60 + seconds) * 1000 + ms);
      }
      pos = close + 1;
    }
    if (stamps.empty())
      continue;
    std::string text = raw.substr(pos);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
      text.erase(text.begin());
    while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    for (unsigned stamp : stamps)
      lines.emplace_back(stamp, text);
  }
  std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return lines;
}

// Picks the best entry from an /api/search response body. LRCLIB's search
// (unlike its exact-match /api/get) tolerates a duration that doesn't line
// up perfectly with what Sonos reports, at the cost of sometimes returning
// entries for a different recording of the same song — prefer one whose
// artistName actually contains the artist we asked for (LRCLIB sometimes
// doubles it, e.g. "The Beatles - The Beatles", confirmed live — hence
// substring rather than exact match), falling back to the first
// non-instrumental result with lyrics at all. Among equally good matches,
// one with synced lyrics wins. Returns an empty Lyrics if the response
// can't be parsed, carries no results, or every result is
// instrumental/lyrics-less.
Lyrics ExtractBestLyrics(const std::string& body, const std::string& artist_name)
{
  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return {};
  }

  auto string_member = [](JsonObject* entry, const char* name) -> const char* {
    if (!json_object_has_member(entry, name))
      return nullptr;
    JsonNode* node = json_object_get_member(entry, name);
    return JSON_NODE_HOLDS_VALUE(node) ? json_object_get_string_member(entry, name) : nullptr;
  };

  JsonNode* root = json_parser_get_root(parser);
  JsonObject* best = nullptr;
  int best_score = -1;
  if (root && JSON_NODE_HOLDS_ARRAY(root))
  {
    JsonArray* data = json_node_get_array(root);
    std::string wanted_lower = ToLower(artist_name);
    guint length = json_array_get_length(data);
    for (guint i = 0; i < length; ++i)
    {
      JsonObject* entry = json_array_get_object_element(data, i);
      if (!entry)
        continue;
      bool instrumental =
          json_object_has_member(entry, "instrumental") && json_object_get_boolean_member(entry, "instrumental");
      const char* plain = string_member(entry, "plainLyrics");
      const char* synced = string_member(entry, "syncedLyrics");
      if (instrumental || ((!plain || !*plain) && (!synced || !*synced)))
        continue;

      const char* entry_artist = string_member(entry, "artistName");
      bool artist_matches = !wanted_lower.empty() && entry_artist &&
                            ToLower(entry_artist).find(wanted_lower) != std::string::npos;
      int score = (artist_matches ? 2 : 0) + (synced && *synced ? 1 : 0);
      if (score > best_score)
      {
        best = entry;
        best_score = score;
      }
    }
  }

  Lyrics result;
  if (best)
  {
    const char* plain = string_member(best, "plainLyrics");
    const char* synced = string_member(best, "syncedLyrics");
    if (synced && *synced)
      result.synced = ParseLrc(synced);
    if (plain && *plain)
    {
      result.plain = plain;
    }
    else
    {
      for (const auto& [ms, line] : result.synced)
        result.plain += line + "\n";
    }
  }

  g_object_unref(parser);
  return result;
}
}  // namespace

LyricsFetcher& LyricsFetcher::Instance()
{
  static LyricsFetcher instance;
  return instance;
}

void LyricsFetcher::RequestLyrics(const std::string& artist, const std::string& title, const std::string& album,
                                   std::function<void(Lyrics)> callback,
                                   const Glib::RefPtr<Gio::Cancellable>& cancellable)
{
  if (title.empty())
  {
    callback({});
    return;
  }

  const std::string key = artist + '\x1f' + title + '\x1f' + album;
  auto cached = cache_.find(key);
  if (cached != cache_.end())
  {
    callback(cached->second);
    return;
  }

  // Built once, tried in order until one finds a match:
  //  1. title/album exactly as reported — the common case, unpolluted.
  //  2. both with a trailing "(...)"/"[...]" release-group tag stripped —
  //     only added if that actually changes something, since a
  //     genuinely meaningful trailing group (say "(Live)") should still
  //     get its own real shot first, not be skipped straight to.
  //  3. stripped title with album dropped entirely — LRCLIB's search
  //     treats album_name as a real filter, so even a *clean-looking*
  //     album name can zero out a match a plain title+artist search would
  //     have found (e.g. a compilation title LRCLIB doesn't have indexed
  //     under). Album-only, since title is what's actually being searched
  //     for.
  std::string stripped_title = StripTrailingBracketedSuffix(title);
  std::string stripped_album = StripTrailingBracketedSuffix(album);
  std::vector<std::pair<std::string, std::string>> attempts;
  attempts.emplace_back(title, album);
  if (stripped_title != title || stripped_album != album)
    attempts.emplace_back(stripped_title, stripped_album);
  if (!album.empty())
    attempts.emplace_back(stripped_title, "");

  RequestLyricsAttempt(artist, std::move(attempts), 0, key, std::move(callback), cancellable);
}

void LyricsFetcher::RequestLyricsAttempt(const std::string& artist,
                                          std::vector<std::pair<std::string, std::string>> attempts, size_t index,
                                          const std::string& cache_key, std::function<void(Lyrics)> callback,
                                          const Glib::RefPtr<Gio::Cancellable>& cancellable)
{
  const std::string& title = attempts[index].first;
  const std::string& album = attempts[index].second;

  std::string url = "https://lrclib.net/api/search?track_name=" + Glib::uri_escape_string(title);
  if (!artist.empty())
    url += "&artist_name=" + Glib::uri_escape_string(artist);
  if (!album.empty())
    url += "&album_name=" + Glib::uri_escape_string(album);

  HttpFetch(
      url,
      [this, artist, attempts, index, cache_key, callback, cancellable](std::string body) mutable {
        // No body at all is a failed or cancelled request, not an answer:
        // give up on this lookup without caching it, so the next try (the
        // track coming back, the view reopening) asks LRCLIB again.
        if (body.empty())
        {
          callback({});
          return;
        }
        Lyrics lyrics = ExtractBestLyrics(body, artist);
        if (lyrics.empty() && index + 1 < attempts.size())
        {
          RequestLyricsAttempt(artist, std::move(attempts), index + 1, cache_key, std::move(callback), cancellable);
          return;
        }
        cache_[cache_key] = lyrics;
        callback(lyrics);
      },
      cancellable);
}

}  // namespace gnomos
