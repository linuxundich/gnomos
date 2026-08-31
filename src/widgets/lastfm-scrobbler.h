// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace gnomos
{

// Last.fm's Audioscrobbler API (https://www.last.fm/api), used for
// scrobbling — opt-in, same disclosure reasoning as ListenBrainzScrobbler,
// but structurally heavier: unlike ListenBrainz's single personal token,
// Last.fm requires a *registered API application* (an api_key + shared
// secret pair, obtained by creating one at last.fm/api/account/create —
// something only a human can do, Gnomos has no way to self-register one),
// a signed request for every call (api_sig — an MD5 of the sorted
// parameters plus the shared secret, computed identically here for every
// method), and a 3-step desktop-auth flow (auth.getToken, a browser visit
// to authorize it, then auth.getSession to exchange it for a long-lived
// session key) before a single scrobble can be sent. See
// GnomosWindow::ShowLastFmAuthDialog() for that flow's own UI.
class LastFmScrobbler
{
public:
  static LastFmScrobbler& Instance();

  // Step 1 of the desktop-auth flow — callback fires once with the
  // one-time token to embed in the web-authorization URL, or an empty
  // token on any failure (network error, bad api_key/secret, ...).
  void RequestAuthToken(const std::string& api_key, const std::string& shared_secret,
                        std::function<void(std::string token)> callback);

  // Step 3 (after the user has completed the browser authorization for
  // the token from RequestAuthToken()) — exchanges it for a long-lived
  // session key plus the authorized account's own username, or both empty
  // on failure. The token itself is single-use and discarded either way.
  void RequestSession(const std::string& api_key, const std::string& shared_secret, const std::string& token,
                      std::function<void(std::string session_key, std::string username)> callback);

  // Fire-and-forget, same reasoning as ListenBrainzScrobbler::Scrobble()
  // (no callback — nothing more specific to do on failure than on
  // success). album may be empty.
  void Scrobble(const std::string& api_key, const std::string& shared_secret, const std::string& session_key,
                const std::string& artist, const std::string& track, const std::string& album,
                std::chrono::system_clock::time_point listened_at);

private:
  LastFmScrobbler() = default;
};

}  // namespace gnomos
