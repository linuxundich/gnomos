// SPDX-License-Identifier: GPL-3.0-or-later

#include "lastfm-scrobbler.h"

#include <map>

#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

namespace gnomos
{

namespace
{

constexpr const char* kApiBase = "https://ws.audioscrobbler.com/2.0/";

// One shared, long-lived session — same reasoning as every other HTTP
// client singleton in this app (HttpFetch()'s own Session(),
// ListenBrainzScrobbler's own).
SoupSession* Session()
{
  static SoupSession* session = soup_session_new();
  return session;
}

// api_sig: an MD5 of every parameter (excluding api_sig/format themselves,
// per Last.fm's own spec) sorted alphabetically by name, concatenated as
// <name><value> with no separators, plus the shared secret appended at the
// end. std::map already sorts by key, so building the concatenation is
// just an ordered walk over it.
std::string ComputeApiSig(const std::map<std::string, std::string>& params, const std::string& shared_secret)
{
  std::string concat;
  for (const auto& [key, value] : params)
    concat += key + value;
  concat += shared_secret;
  gchar* digest = g_compute_checksum_for_string(G_CHECKSUM_MD5, concat.c_str(), concat.size());
  std::string result = digest ? digest : "";
  g_free(digest);
  return result;
}

std::string BuildQueryUrl(const std::map<std::string, std::string>& params)
{
  std::string url = kApiBase;
  url += "?format=json";
  for (const auto& [key, value] : params)
    url += "&" + key + "=" + Glib::uri_escape_string(value);
  return url;
}

std::string BuildFormBody(const std::map<std::string, std::string>& params)
{
  std::string body;
  for (const auto& [key, value] : params)
  {
    if (!body.empty())
      body += "&";
    body += key + "=" + Glib::uri_escape_string(value);
  }
  return body;
}

struct GetContext
{
  SoupMessage* message;
  std::function<void(std::string)> callback;
};

// GET with a query string already fully built (BuildQueryUrl() above) —
// same shape as HttpFetch(), duplicated rather than reused since this
// needs its own SoupSession (HttpFetch() is otherwise fine for plain GETs,
// but sharing would mean either exposing this session or pulling
// track.scrobble's own POST need into HttpFetch() itself, out of scope for
// what that shared helper is meant to stay: GET-only, no auth headers).
void Get(const std::string& url, std::function<void(std::string body)> callback)
{
  SoupMessage* message = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!message)
  {
    callback("");
    return;
  }
  // message must stay alive for the whole async call — freed from inside
  // the completion callback below, not here, same as Scrobble()'s own
  // message handling.
  auto* ctx = new GetContext{message, std::move(callback)};
  soup_session_send_and_read_async(
      Session(), message, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer user_data) {
        auto* ctx = static_cast<GetContext*>(user_data);
        GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, nullptr);
        std::string body;
        if (bytes)
        {
          gsize size = 0;
          const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
          if (data && size > 0)
            body.assign(data, size);
          g_bytes_unref(bytes);
        }
        g_object_unref(ctx->message);
        ctx->callback(std::move(body));
        delete ctx;
      },
      ctx);
}

}  // namespace

LastFmScrobbler& LastFmScrobbler::Instance()
{
  static LastFmScrobbler instance;
  return instance;
}

void LastFmScrobbler::RequestAuthToken(const std::string& api_key, const std::string& shared_secret,
                                       std::function<void(std::string token)> callback)
{
  if (api_key.empty() || shared_secret.empty())
  {
    callback("");
    return;
  }

  std::map<std::string, std::string> params = {{"api_key", api_key}, {"method", "auth.getToken"}};
  params["api_sig"] = ComputeApiSig(params, shared_secret);

  Get(BuildQueryUrl(params), [callback = std::move(callback)](std::string body) {
    JsonParser* parser = json_parser_new();
    std::string token;
    if (json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), nullptr))
    {
      JsonNode* root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_OBJECT(root))
      {
        JsonObject* obj = json_node_get_object(root);
        if (json_object_has_member(obj, "token"))
          token = json_object_get_string_member(obj, "token");
      }
    }
    g_object_unref(parser);
    callback(token);
  });
}

void LastFmScrobbler::RequestSession(const std::string& api_key, const std::string& shared_secret,
                                     const std::string& token,
                                     std::function<void(std::string session_key, std::string username)> callback)
{
  if (api_key.empty() || shared_secret.empty() || token.empty())
  {
    callback("", "");
    return;
  }

  std::map<std::string, std::string> params = {
      {"api_key", api_key}, {"method", "auth.getSession"}, {"token", token}};
  params["api_sig"] = ComputeApiSig(params, shared_secret);

  Get(BuildQueryUrl(params), [callback = std::move(callback)](std::string body) {
    JsonParser* parser = json_parser_new();
    std::string session_key, username;
    if (json_parser_load_from_data(parser, body.c_str(), static_cast<gssize>(body.size()), nullptr))
    {
      JsonNode* root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_OBJECT(root))
      {
        JsonObject* obj = json_node_get_object(root);
        if (json_object_has_member(obj, "session") &&
            JSON_NODE_HOLDS_OBJECT(json_object_get_member(obj, "session")))
        {
          JsonObject* session_obj = json_object_get_object_member(obj, "session");
          if (json_object_has_member(session_obj, "key"))
            session_key = json_object_get_string_member(session_obj, "key");
          if (json_object_has_member(session_obj, "name"))
            username = json_object_get_string_member(session_obj, "name");
        }
      }
    }
    g_object_unref(parser);
    callback(session_key, username);
  });
}

void LastFmScrobbler::Scrobble(const std::string& api_key, const std::string& shared_secret,
                               const std::string& session_key, const std::string& artist, const std::string& track,
                               const std::string& album, std::chrono::system_clock::time_point listened_at)
{
  if (api_key.empty() || shared_secret.empty() || session_key.empty() || artist.empty() || track.empty())
    return;

  std::map<std::string, std::string> params = {
      {"api_key", api_key},
      {"method", "track.scrobble"},
      {"sk", session_key},
      {"artist", artist},
      {"track", track},
      {"timestamp", std::to_string(std::chrono::system_clock::to_time_t(listened_at))},
  };
  if (!album.empty())
    params["album"] = album;
  params["api_sig"] = ComputeApiSig(params, shared_secret);

  SoupMessage* message = soup_message_new(SOUP_METHOD_POST, kApiBase);
  if (!message)
    return;
  std::string body = BuildFormBody(params);
  GBytes* bytes = g_bytes_new(body.data(), body.size());
  soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
  g_bytes_unref(bytes);
  soup_session_send_and_read_async(
      Session(), message, G_PRIORITY_DEFAULT, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer user_data) {
        // No callback to the caller (see this method's own comment) —
        // this only exists to free the response and the message itself.
        GBytes* response = soup_session_send_and_read_finish(SOUP_SESSION(source), result, nullptr);
        if (response)
          g_bytes_unref(response);
        g_object_unref(static_cast<SoupMessage*>(user_data));
      },
      message);
}

}  // namespace gnomos
