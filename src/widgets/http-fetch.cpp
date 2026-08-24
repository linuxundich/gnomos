// SPDX-License-Identifier: GPL-3.0-or-later

#include "http-fetch.h"

#include <deque>
#include <iterator>

#include <libsoup/soup.h>

namespace gnomos
{

namespace
{

// A large cover-art grid (Albums/Artists, 1000+ entries) fires one
// HttpFetch() per tile essentially all at once — BuildGrid()/BuildList()
// (library-view.cpp) create every CoverThumbnail up front, GTK4's FlowBox/
// ListBox aren't lazily virtualizing widgets the way a ListView with a
// factory would be. Most of that traffic targets the *same* host too: the
// local Sonos device itself (upnp:albumArtURI), embedded hardware with a
// tiny HTTP server connection budget. Confirmed live: without a cap here,
// only the first handful of tiles' fetches actually complete — the rest
// fail outright under the load and never populate ArtCache, so they stay
// blank even after scrolling back to them (a "cache miss" that keeps
// missing, since the fetch that was supposed to fill the cache never
// succeeded in the first place). Mirrors ArtistImageFetcher's own
// kMaxConcurrent throttle — same class of problem, a different remote
// server — as one shared queue here rather than one per caller, since the
// large majority of traffic through this file is exactly this cover-art
// case.
constexpr size_t kMaxConcurrent = 6;

struct PendingFetch
{
  std::string url;
  std::function<void(std::string)> callback;
  Glib::RefPtr<Gio::Cancellable> cancellable;
};

std::deque<PendingFetch> g_queue;
size_t g_in_flight = 0;

void StartNext();

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

  --g_in_flight;
  StartNext();
}

void StartNext()
{
  while (g_in_flight < kMaxConcurrent && !g_queue.empty())
  {
    PendingFetch pending = std::move(g_queue.front());
    g_queue.pop_front();

    if (pending.cancellable && pending.cancellable->is_cancelled())
    {
      // Superseded before its turn came (e.g. CoverThumbnail::SetArtUri()
      // called again for a different uri while this one was still queued)
      // — resolve as any other failure, without spending a real request
      // (and a slot) on something nobody wants anymore.
      pending.callback("");
      continue;
    }

    SoupMessage* message = soup_message_new(SOUP_METHOD_GET, pending.url.c_str());
    if (!message)
    {
      // A malformed URL — shouldn't happen (every caller builds these
      // from known-good, hardcoded API base URLs), but resolve as "no
      // response" rather than dereferencing a null message below.
      pending.callback("");
      continue;
    }
    ++g_in_flight;
    auto* ctx = new FetchContext{message, std::move(pending.callback)};
    soup_session_send_and_read_async(Session(), message, G_PRIORITY_DEFAULT,
                                      pending.cancellable ? pending.cancellable->gobj() : nullptr, OnFetchReady, ctx);
  }
}

}  // namespace

void HttpFetch(const std::string& url, std::function<void(std::string body)> callback,
                const Glib::RefPtr<Gio::Cancellable>& cancellable)
{
  g_queue.push_back({url, std::move(callback), cancellable});
  StartNext();
}

void HttpFetchPrioritize(const std::string& url)
{
  // Searches back-to-front: if the same uri was ever requested more than
  // once (a repeated album cover across several tiles, say), the most
  // recently queued one is the one most likely to still matter — an
  // older duplicate request for the same uri may already be stale.
  for (auto it = g_queue.rbegin(); it != g_queue.rend(); ++it)
  {
    if (it->url == url)
    {
      PendingFetch pending = std::move(*it);
      g_queue.erase(std::next(it).base());
      g_queue.push_front(std::move(pending));
      return;
    }
  }
}

}  // namespace gnomos
