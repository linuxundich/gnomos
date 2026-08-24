// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

#include <giomm/cancellable.h>

namespace gnomos
{

// Fetches an http(s):// URL asynchronously (GET only — every current
// caller is a read-only API/image lookup) and calls `callback` on the main
// loop with the response body, or an empty string on any failure —
// transport-level (DNS, connection refused, TLS, ...) or a non-2xx HTTP
// status alike. Every caller already treats an empty/unparseable body the
// same as "nothing found", so this single failure signal is enough; no
// caller has ever needed to distinguish *why* a fetch failed.
//
// `cancellable`, if given, lets the caller abandon an in-flight fetch it no
// longer cares about (e.g. CoverThumbnail superseded by a newer SetArtUri()
// before the previous one finished) — purely a network-efficiency
// optimization: `callback` may still fire afterward (with an empty body,
// same as any other failure), same as GCancellable's usual semantics, so
// callers that care about stale results already need their own generation/
// liveness guard regardless (see CoverThumbnail::generation_/alive_).
void HttpFetch(const std::string& url, std::function<void(std::string body)> callback,
                const Glib::RefPtr<Gio::Cancellable>& cancellable = {});

}  // namespace gnomos
