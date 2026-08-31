// SPDX-License-Identifier: GPL-3.0-or-later

#include "listenbrainz-scrobbler.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

namespace gnomos
{

namespace
{

// One shared, long-lived session for every scrobble — matches HttpFetch()'s
// own reasoning (connection reuse/keep-alive), even though call volume here
// is far lower.
SoupSession* Session()
{
  static SoupSession* session = soup_session_new();
  return session;
}

extern "C" void OnScrobbleReady(GObject* source, GAsyncResult* result, gpointer user_data)
{
  // No callback to the caller (see Scrobble()'s own comment) — this only
  // exists to free the response and the message itself; the result is
  // otherwise ignored.
  GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, nullptr);
  if (bytes)
    g_bytes_unref(bytes);
  g_object_unref(static_cast<SoupMessage*>(user_data));
}

}  // namespace

ListenBrainzScrobbler& ListenBrainzScrobbler::Instance()
{
  static ListenBrainzScrobbler instance;
  return instance;
}

void ListenBrainzScrobbler::Scrobble(const std::string& token, const std::string& artist, const std::string& track,
                                      const std::string& album, std::chrono::system_clock::time_point listened_at)
{
  if (token.empty() || artist.empty() || track.empty())
    return;

  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "listen_type");
  json_builder_add_string_value(builder, "single");
  json_builder_set_member_name(builder, "payload");
  json_builder_begin_array(builder);
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "listened_at");
  json_builder_add_int_value(
      builder, static_cast<gint64>(std::chrono::system_clock::to_time_t(listened_at)));
  json_builder_set_member_name(builder, "track_metadata");
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "artist_name");
  json_builder_add_string_value(builder, artist.c_str());
  json_builder_set_member_name(builder, "track_name");
  json_builder_add_string_value(builder, track.c_str());
  if (!album.empty())
  {
    json_builder_set_member_name(builder, "release_name");
    json_builder_add_string_value(builder, album.c_str());
  }
  json_builder_end_object(builder);  // track_metadata
  json_builder_end_object(builder);  // payload[0]
  json_builder_end_array(builder);   // payload
  json_builder_end_object(builder);  // root

  JsonGenerator* generator = json_generator_new();
  JsonNode* root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gsize length = 0;
  gchar* data = json_generator_to_data(generator, &length);

  SoupMessage* message = soup_message_new(SOUP_METHOD_POST, "https://api.listenbrainz.org/1/submit-listens");
  if (message)
  {
    soup_message_headers_append(soup_message_get_request_headers(message), "Authorization",
                                 ("Token " + token).c_str());
    GBytes* body = g_bytes_new_take(data, length);  // takes ownership of `data`
    soup_message_set_request_body_from_bytes(message, "application/json", body);
    g_bytes_unref(body);
    soup_session_send_and_read_async(Session(), message, G_PRIORITY_DEFAULT, nullptr, OnScrobbleReady, message);
  }
  else
  {
    g_free(data);
  }

  json_node_free(root);
  g_object_unref(generator);
  g_object_unref(builder);
}

}  // namespace gnomos
