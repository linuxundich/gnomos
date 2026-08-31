// SPDX-License-Identifier: GPL-3.0-or-later

#include "radio-browser-service.h"

#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>

#include "http-fetch.h"

namespace gnomos
{

namespace
{

constexpr const char* kApiBase = "https://all.api.radio-browser.info";

const char* StringMember(JsonObject* obj, const char* key)
{
  return json_object_has_member(obj, key) ? json_object_get_string_member(obj, key) : "";
}

std::vector<RadioBrowserStation> ParseStations(const std::string& body)
{
  std::vector<RadioBrowserStation> stations;

  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return stations;
  }

  JsonNode* root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_ARRAY(root))
  {
    JsonArray* array = json_node_get_array(root);
    guint length = json_array_get_length(array);
    stations.reserve(length);
    for (guint i = 0; i < length; ++i)
    {
      JsonObject* entry = json_array_get_object_element(array, i);
      if (!entry)
        continue;
      RadioBrowserStation station;
      station.name = StringMember(entry, "name");
      // url_resolved (the stream after following any redirects the
      // directory itself already chased) is what actually plays reliably —
      // url is the raw, as-submitted value, kept only as a fallback for the
      // rare entry that has one but not the other.
      std::string url_resolved = StringMember(entry, "url_resolved");
      station.url = !url_resolved.empty() ? url_resolved : StringMember(entry, "url");
      station.countrycode = StringMember(entry, "countrycode");
      station.tags = StringMember(entry, "tags");
      station.codec = StringMember(entry, "codec");
      station.favicon = StringMember(entry, "favicon");
      if (json_object_has_member(entry, "bitrate"))
        station.bitrate = static_cast<int>(json_object_get_int_member(entry, "bitrate"));
      if (!station.name.empty() && !station.url.empty())
        stations.push_back(std::move(station));
    }
  }

  g_object_unref(parser);
  return stations;
}

std::vector<RadioBrowserCountry> ParseCountries(const std::string& body)
{
  std::vector<RadioBrowserCountry> countries;

  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return countries;
  }

  JsonNode* root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_ARRAY(root))
  {
    JsonArray* array = json_node_get_array(root);
    guint length = json_array_get_length(array);
    countries.reserve(length);
    for (guint i = 0; i < length; ++i)
    {
      JsonObject* entry = json_array_get_object_element(array, i);
      if (!entry)
        continue;
      RadioBrowserCountry country;
      country.name = StringMember(entry, "name");
      // iso_3166_1 is the API's own field name for the country code here
      // (distinct from a station's own "countrycode" field above, despite
      // meaning the same kind of value) — defensive against it being
      // absent, same reasoning as everywhere else in this file: an empty
      // code just means the picker falls back to name-based filtering for
      // that entry instead of countrycode-based.
      country.countrycode = StringMember(entry, "iso_3166_1");
      if (json_object_has_member(entry, "stationcount"))
        country.station_count = static_cast<int>(json_object_get_int_member(entry, "stationcount"));
      if (!country.name.empty())
        countries.push_back(std::move(country));
    }
  }

  g_object_unref(parser);
  return countries;
}

std::vector<RadioBrowserTag> ParseTags(const std::string& body)
{
  std::vector<RadioBrowserTag> tags;

  JsonParser* parser = json_parser_new();
  GError* error = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), &error))
  {
    if (error)
      g_error_free(error);
    g_object_unref(parser);
    return tags;
  }

  JsonNode* root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_ARRAY(root))
  {
    JsonArray* array = json_node_get_array(root);
    guint length = json_array_get_length(array);
    tags.reserve(length);
    for (guint i = 0; i < length; ++i)
    {
      JsonObject* entry = json_array_get_object_element(array, i);
      if (!entry)
        continue;
      RadioBrowserTag tag;
      tag.name = StringMember(entry, "name");
      if (json_object_has_member(entry, "stationcount"))
        tag.station_count = static_cast<int>(json_object_get_int_member(entry, "stationcount"));
      if (!tag.name.empty())
        tags.push_back(std::move(tag));
    }
  }

  g_object_unref(parser);
  return tags;
}

}  // namespace

RadioBrowserService& RadioBrowserService::Instance()
{
  static RadioBrowserService instance;
  return instance;
}

void RadioBrowserService::SearchStations(const std::string& name, const std::string& countrycode,
                                          const std::string& tag,
                                          std::function<void(std::vector<RadioBrowserStation>)> callback)
{
  std::string url = std::string(kApiBase) + "/json/stations/search?limit=" + std::to_string(kMaxResults) +
                     "&hidebroken=true&order=clickcount&reverse=true";
  if (!name.empty())
    url += "&name=" + Glib::uri_escape_string(name);
  if (!countrycode.empty())
    url += "&countrycode=" + Glib::uri_escape_string(countrycode);
  if (!tag.empty())
    url += "&tag=" + Glib::uri_escape_string(tag);

  // An empty body (network error, non-2xx status, ...) resolves as no
  // results below like any other empty/unparseable response — see
  // HttpFetch()'s own comment.
  HttpFetch(url, [callback](std::string body) { callback(ParseStations(body)); });
}

void RadioBrowserService::FetchCountries(std::function<void(std::vector<RadioBrowserCountry>)> callback)
{
  if (countries_loaded_)
  {
    callback(countries_cache_);
    return;
  }

  pending_country_callbacks_.push_back(std::move(callback));
  if (pending_country_callbacks_.size() > 1)
    return;  // already in flight — just added another waiting callback above

  std::string url = std::string(kApiBase) + "/json/countries";
  // An empty body (network error, non-2xx status, ...) resolves below same
  // as a genuinely empty directory — see HttpFetch()'s own comment;
  // countries_loaded_ deliberately NOT set in that case so a later retry
  // (opening the dialog again) gets a fresh attempt rather than being stuck
  // caching a failure forever.
  HttpFetch(url, [this](std::string body) {
    std::vector<RadioBrowserCountry> countries = ParseCountries(body);
    if (!countries.empty())
    {
      countries_cache_ = countries;
      countries_loaded_ = true;
    }

    auto callbacks = std::move(pending_country_callbacks_);
    pending_country_callbacks_.clear();
    for (auto& cb : callbacks)
      cb(countries);
  });
}

void RadioBrowserService::FetchTags(std::function<void(std::vector<RadioBrowserTag>)> callback)
{
  if (tags_loaded_)
  {
    callback(tags_cache_);
    return;
  }

  pending_tag_callbacks_.push_back(std::move(callback));
  if (pending_tag_callbacks_.size() > 1)
    return;  // already in flight — just added another waiting callback above

  // Unlike /json/countries (a few hundred entries total), the tag
  // directory is free-text and effectively unbounded — sorted by
  // popularity and capped here rather than fetched in full, same
  // kMaxResults-style reasoning as SearchStations() itself.
  std::string url = std::string(kApiBase) + "/json/tags?order=stationcount&reverse=true&limit=200";
  HttpFetch(url, [this](std::string body) {
    std::vector<RadioBrowserTag> tags = ParseTags(body);
    if (!tags.empty())
    {
      tags_cache_ = tags;
      tags_loaded_ = true;
    }

    auto callbacks = std::move(pending_tag_callbacks_);
    pending_tag_callbacks_.clear();
    for (auto& cb : callbacks)
      cb(tags);
  });
}

}  // namespace gnomos
