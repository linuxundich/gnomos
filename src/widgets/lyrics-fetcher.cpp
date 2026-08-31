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

  std::string url = "https://lrclib.net/api/search?track_name=" + Glib::uri_escape_string(title);
  if (!artist.empty())
    url += "&artist_name=" + Glib::uri_escape_string(artist);
  if (!album.empty())
    url += "&album_name=" + Glib::uri_escape_string(album);

  HttpFetch(
      url,
      [this, key, artist, callback](std::string body) {
        std::string lyrics = ExtractBestLyrics(body, artist);
        cache_[key] = lyrics;
        callback(lyrics);
      },
      cancellable);
}

}  // namespace gnomos
