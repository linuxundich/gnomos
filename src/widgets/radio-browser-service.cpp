// SPDX-License-Identifier: GPL-3.0-or-later

#include "radio-browser-service.h"

#include <giomm/asyncresult.h>
#include <giomm/file.h>
#include <glibmm/error.h>
#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>

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

}  // namespace

RadioBrowserService& RadioBrowserService::Instance()
{
  static RadioBrowserService instance;
  return instance;
}

void RadioBrowserService::SearchStations(const std::string& name, const std::string& countrycode,
                                          std::function<void(std::vector<RadioBrowserStation>)> callback)
{
  std::string url = std::string(kApiBase) + "/json/stations/search?limit=" + std::to_string(kMaxResults) +
                     "&hidebroken=true&order=clickcount&reverse=true";
  if (!name.empty())
    url += "&name=" + Glib::uri_escape_string(name);
  if (!countrycode.empty())
    url += "&countrycode=" + Glib::uri_escape_string(countrycode);

  auto file = Gio::File::create_for_uri(url);
  file->load_contents_async([file, callback](Glib::RefPtr<Gio::AsyncResult>& result) {
    std::string body;
    try
    {
      char* contents = nullptr;
      gsize length = 0;
      if (file->load_contents_finish(result, contents, length) && contents)
      {
        body.assign(contents, length);
        g_free(contents);
      }
    }
    catch (const Glib::Error&)
    {
      // network error, DNS failure, ... — body stays empty, resolved as no
      // results below like any other empty/unparseable response.
    }
    callback(ParseStations(body));
  });
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
  auto file = Gio::File::create_for_uri(url);
  file->load_contents_async([this, file](Glib::RefPtr<Gio::AsyncResult>& result) {
    std::string body;
    try
    {
      char* contents = nullptr;
      gsize length = 0;
      if (file->load_contents_finish(result, contents, length) && contents)
      {
        body.assign(contents, length);
        g_free(contents);
      }
    }
    catch (const Glib::Error&)
    {
      // network error — countries_cache_ stays empty, resolved below same
      // as a genuinely empty directory; countries_loaded_ deliberately NOT
      // set in this case so a later retry (opening the dialog again) gets
      // a fresh attempt rather than being stuck caching a failure forever.
    }

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

}  // namespace gnomos
