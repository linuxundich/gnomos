// SPDX-License-Identifier: GPL-3.0-or-later

#include "artist-info-fetcher.h"

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

// Same "prefer an exact (case-insensitive) name match with the most fans,
// fall back to Deezer's own first result" heuristic
// ArtistImageFetcher::ExtractBestPictureUrl() already uses — see that
// method's own comment for why Deezer's own search ranking alone isn't
// popularity-aware. Returns 0 (an invalid Deezer id) if nothing usable was
// found.
gint64 ExtractBestArtistId(const std::string& body, const std::string& artist_name)
{
  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return 0;
  }

  gint64 result = 0;
  JsonNode* root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject* root_obj = json_node_get_object(root);
    if (json_object_has_member(root_obj, "data") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(root_obj, "data")))
    {
      JsonArray* data = json_object_get_array_member(root_obj, "data");
      std::string wanted_lower = ToLower(artist_name);

      gint64 first_id = 0;
      gint64 best_exact_id = 0;
      gint64 best_exact_fans = -1;

      guint length = json_array_get_length(data);
      for (guint i = 0; i < length; ++i)
      {
        JsonObject* entry = json_array_get_object_element(data, i);
        if (!entry || !json_object_has_member(entry, "id"))
          continue;
        gint64 id = json_object_get_int_member(entry, "id");
        if (first_id == 0)
          first_id = id;
        const char* name = json_object_has_member(entry, "name") ? json_object_get_string_member(entry, "name") : "";
        if (ToLower(name) == wanted_lower)
        {
          gint64 fans = json_object_has_member(entry, "nb_fan") ? json_object_get_int_member(entry, "nb_fan") : 0;
          if (fans > best_exact_fans)
          {
            best_exact_fans = fans;
            best_exact_id = id;
          }
        }
      }
      result = best_exact_id != 0 ? best_exact_id : first_id;
    }
  }

  g_object_unref(parser);
  return result;
}

std::vector<RelatedArtist> ParseRelatedArtists(const std::string& body)
{
  std::vector<RelatedArtist> result;

  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return result;
  }

  JsonNode* root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject* root_obj = json_node_get_object(root);
    if (json_object_has_member(root_obj, "data") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(root_obj, "data")))
    {
      JsonArray* data = json_object_get_array_member(root_obj, "data");
      guint length = json_array_get_length(data);
      result.reserve(length);
      for (guint i = 0; i < length; ++i)
      {
        JsonObject* entry = json_array_get_object_element(data, i);
        if (!entry || !json_object_has_member(entry, "name"))
          continue;
        RelatedArtist related;
        related.name = json_object_get_string_member(entry, "name");
        if (json_object_has_member(entry, "picture_medium"))
          related.picture_url = json_object_get_string_member(entry, "picture_medium");
        if (!related.name.empty())
          result.push_back(std::move(related));
      }
    }
  }

  g_object_unref(parser);
  return result;
}

// Last.fm's own bio text ends with a "<a href="...">Read more on
// Last.fm</a>" link appended to otherwise-plain text — confirmed against
// the API's documented behavior; stripped here since this is shown in a
// plain Gtk::Label, not something that renders HTML. No other markup
// appears in the body text itself.
std::string StripTrailingLastFmLink(const std::string& text)
{
  size_t pos = text.find("<a href=");
  std::string trimmed = pos != std::string::npos ? text.substr(0, pos) : text;
  while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == ' '))
    trimmed.pop_back();
  return trimmed;
}

std::string ExtractBio(const std::string& body)
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

  std::string bio;
  JsonNode* root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_OBJECT(root))
  {
    JsonObject* root_obj = json_node_get_object(root);
    if (json_object_has_member(root_obj, "artist") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(root_obj, "artist")))
    {
      JsonObject* artist_obj = json_object_get_object_member(root_obj, "artist");
      if (json_object_has_member(artist_obj, "bio") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(artist_obj, "bio")))
      {
        JsonObject* bio_obj = json_object_get_object_member(artist_obj, "bio");
        if (json_object_has_member(bio_obj, "summary"))
          bio = StripTrailingLastFmLink(json_object_get_string_member(bio_obj, "summary"));
      }
    }
  }

  g_object_unref(parser);
  return bio;
}

}  // namespace

ArtistInfoFetcher& ArtistInfoFetcher::Instance()
{
  static ArtistInfoFetcher instance;
  return instance;
}

void ArtistInfoFetcher::RequestBio(const std::string& api_key, const std::string& artist_name,
                                    std::function<void(std::string)> callback)
{
  if (api_key.empty() || artist_name.empty())
  {
    callback("");
    return;
  }

  std::string url = "https://ws.audioscrobbler.com/2.0/?method=artist.getinfo&artist=" +
                     Glib::uri_escape_string(artist_name) + "&api_key=" + Glib::uri_escape_string(api_key) +
                     "&format=json";
  HttpFetch(url, [callback = std::move(callback)](std::string body) { callback(ExtractBio(body)); });
}

void ArtistInfoFetcher::RequestRelatedArtists(const std::string& artist_name,
                                               std::function<void(std::vector<RelatedArtist>)> callback)
{
  if (artist_name.empty())
  {
    callback({});
    return;
  }

  std::string search_url = "https://api.deezer.com/search/artist?q=" + Glib::uri_escape_string(artist_name) +
                            "&limit=10";
  HttpFetch(search_url, [artist_name, callback = std::move(callback)](std::string search_body) {
    gint64 id = ExtractBestArtistId(search_body, artist_name);
    if (id == 0)
    {
      callback({});
      return;
    }
    std::string related_url = "https://api.deezer.com/artist/" + std::to_string(id) + "/related?limit=10";
    HttpFetch(related_url, [callback](std::string related_body) { callback(ParseRelatedArtists(related_body)); });
  });
}

}  // namespace gnomos
