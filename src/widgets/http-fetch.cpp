// SPDX-License-Identifier: GPL-3.0-or-later

#include "http-fetch.h"

#include <libsoup/soup.h>

namespace gnomos
{

namespace
{

// One shared, long-lived session for the whole app, rather than one per
// request — matches libsoup's own recommended usage (connection reuse/
// keep-alive) and how ArtistImageFetcher/RadioBrowserService are
// themselves already process-wide singletons.
SoupSession* Session()
{
  static SoupSession* session = soup_session_new();
  return session;
}

struct FetchContext
{
  SoupMessage* message;
  std::function<void(std::string)> callback;
};

extern "C" void OnFetchReady(GObject* source, GAsyncResult* result, gpointer user_data)
{
  auto* ctx = static_cast<FetchContext*>(user_data);
  std::string body;
  // Passing nullptr for GError** — every caller already treats "no body"
  // as the one failure signal (see HttpFetch()'s own comment); the
  // specific GError, if any, isn't otherwise surfaced anywhere.
  GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, nullptr);
  if (bytes)
  {
    if (SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(ctx->message)))
    {
      gsize size = 0;
      const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
      if (data && size > 0)
        body.assign(data, size);
    }
    g_bytes_unref(bytes);
  }
  g_object_unref(ctx->message);
  ctx->callback(std::move(body));
  delete ctx;
}

}  // namespace

void HttpFetch(const std::string& url, std::function<void(std::string body)> callback,
                const Glib::RefPtr<Gio::Cancellable>& cancellable)
{
  SoupMessage* message = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!message)
  {
    // A malformed URL — shouldn't happen (every caller builds these from
    // known-good, hardcoded API base URLs), but resolve as "no response"
    // rather than dereferencing a null message below.
    callback("");
    return;
  }
  auto* ctx = new FetchContext{message, std::move(callback)};
  soup_session_send_and_read_async(Session(), message, G_PRIORITY_DEFAULT,
                                    cancellable ? cancellable->gobj() : nullptr, OnFetchReady, ctx);
}

}  // namespace gnomos
