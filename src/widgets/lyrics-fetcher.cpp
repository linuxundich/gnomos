// SPDX-License-Identifier: GPL-3.0-or-later

#include "lyrics-fetcher.h"

#include <algorithm>
#include <cctype>

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

// Picks the best "plainLyrics" from an /api/search response body. LRCLIB's
// search (unlike its exact-match /api/get) tolerates a duration that
// doesn't line up perfectly with what Sonos reports, at the cost of
// sometimes returning entries for a different recording of the same song —
// prefer one whose artistName actually contains the artist we asked for
// (LRCLIB sometimes doubles it, e.g. "The Beatles - The Beatles",
// confirmed live — hence substring rather than exact match), falling back
// to the first non-instrumental result with lyrics at all. Returns an
// empty string if the response can't be parsed, carries no results, or
// every result is instrumental/lyrics-less.
std::string ExtractBestLyrics(const std::string& body, const std::string& artist_name)
{
  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return "";
  }

  JsonNode* root = json_parser_get_root(parser);
  std::string result;
  if (root && JSON_NODE_HOLDS_ARRAY(root))
  {
    JsonArray* data = json_node_get_array(root);
    std::string wanted_lower = ToLower(artist_name);

    const char* first_match = nullptr;

    guint length = json_array_get_length(data);
    for (guint i = 0; i < length; ++i)
    {
      JsonObject* entry = json_array_get_object_element(data, i);
      if (!entry)
        continue;
      bool instrumental =
          json_object_has_member(entry, "instrumental") && json_object_get_boolean_member(entry, "instrumental");
      const char* lyrics =
          json_object_has_member(entry, "plainLyrics") ? json_object_get_string_member(entry, "plainLyrics") : nullptr;
      if (instrumental || !lyrics || !*lyrics)
        continue;

      if (!first_match)
        first_match = lyrics;

      const char* entry_artist =
          json_object_has_member(entry, "artistName") ? json_object_get_string_member(entry, "artistName") : "";
      if (!wanted_lower.empty() && ToLower(entry_artist).find(wanted_lower) != std::string::npos)
      {
        result = lyrics;
        break;
      }
    }

    if (result.empty() && first_match)
      result = first_match;
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
                                   std::function<void(std::string)> callback,
                                   const Glib::RefPtr<Gio::Cancellable>& cancellable)
{
  if (title.empty())
  {
    callback("");
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
                                          const std::string& cache_key, std::function<void(std::string)> callback,
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
        std::string lyrics = ExtractBestLyrics(body, artist);
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
