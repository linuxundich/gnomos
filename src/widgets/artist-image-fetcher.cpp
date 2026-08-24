// SPDX-License-Identifier: GPL-3.0-or-later

#include "artist-image-fetcher.h"

#include <algorithm>
#include <cctype>

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

// Picks the best "data" entry from a Deezer artist-search response body —
// see ArtistImageFetcher's own header comment for why this isn't just
// "the first result". Returns an empty string if the response can't be
// parsed at all, or carries no results.
std::string ExtractBestPictureUrl(const std::string& body, const std::string& artist_name)
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
  if (root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject* root_obj = json_node_get_object(root);
    if (json_object_has_member(root_obj, "data") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(root_obj, "data")))
    {
      JsonArray* data = json_object_get_array_member(root_obj, "data");
      std::string wanted_lower = ToLower(artist_name);

      const char* first_picture = nullptr;
      const char* best_exact_picture = nullptr;
      gint64 best_exact_fans = -1;

      guint length = json_array_get_length(data);
      for (guint i = 0; i < length; ++i)
      {
        JsonObject* entry = json_array_get_object_element(data, i);
        if (!entry)
          continue;
        const char* name = json_object_has_member(entry, "name") ? json_object_get_string_member(entry, "name") : "";
        const char* picture = json_object_has_member(entry, "picture_medium")
                                   ? json_object_get_string_member(entry, "picture_medium")
                                   : nullptr;
        if (!picture)
          continue;
        if (!first_picture)
          first_picture = picture;
        if (ToLower(name) == wanted_lower)
        {
          gint64 fans = json_object_has_member(entry, "nb_fan") ? json_object_get_int_member(entry, "nb_fan") : 0;
          if (fans > best_exact_fans)
          {
            best_exact_fans = fans;
            best_exact_picture = picture;
          }
        }
      }

      const char* chosen = best_exact_picture ? best_exact_picture : first_picture;
      if (chosen)
        result = chosen;
    }
  }

  g_object_unref(parser);
  return result;
}
}  // namespace

ArtistImageFetcher& ArtistImageFetcher::Instance()
{
  static ArtistImageFetcher instance;
  return instance;
}

void ArtistImageFetcher::RequestArtistImage(const std::string& artist_name, std::function<void(std::string)> callback)
{
  if (artist_name.empty())
  {
    callback("");
    return;
  }

  auto cached = cache_.find(artist_name);
  if (cached != cache_.end())
  {
    callback(cached->second);
    return;
  }

  pending_callbacks_[artist_name].push_back(std::move(callback));
  // Already in flight or queued for this exact name — just added another
  // waiting callback above, nothing more to do.
  if (pending_callbacks_[artist_name].size() > 1)
    return;

  queue_.push_back(artist_name);
  StartNext();
}

void ArtistImageFetcher::StartNext()
{
  while (in_flight_ < kMaxConcurrent && !queue_.empty())
  {
    std::string artist_name = queue_.front();
    queue_.pop_front();
    ++in_flight_;

    std::string url =
        "https://api.deezer.com/search/artist?q=" + Glib::uri_escape_string(artist_name) + "&limit=10";
    // An empty body (network error, non-2xx status, ...) resolves as "not
    // found" below like any other lookup miss — see HttpFetch()'s own
    // comment.
    HttpFetch(url, [this, artist_name](std::string body) { OnResponse(artist_name, body); });
  }
}

void ArtistImageFetcher::OnResponse(const std::string& artist_name, const std::string& body)
{
  Resolve(artist_name, ExtractBestPictureUrl(body, artist_name));
}

void ArtistImageFetcher::Resolve(const std::string& artist_name, const std::string& picture_url)
{
  cache_[artist_name] = picture_url;

  auto it = pending_callbacks_.find(artist_name);
  if (it != pending_callbacks_.end())
  {
    for (auto& callback : it->second)
      callback(picture_url);
    pending_callbacks_.erase(it);
  }

  if (in_flight_ > 0)
    --in_flight_;
  StartNext();
}

}  // namespace gnomos
