// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "backend/noson-backend.h"

namespace gnomos
{

// A radio station's own reported "now playing" content (NowPlaying::artist
// for a radio-like source) rotates between the actual song and interstitial
// ad/ident text — confirmed live, e.g. "song1 / artist1", "werbung1",
// "werbung2", "song1 / artist1", ... — so naively treating every content
// change as a new song spams both MPRIS clients (a popup per ad break) and
// the History tab (a bogus entry, and a desktop notification if enabled,
// per ad break too). Filter() applies the station's own settings
// (NosonBackend::GetRadioMprisSettings() — an opt-out switch and an
// optional regex a real song's text must match) plus a global heuristic
// (NosonBackend::GetRadioSpamWhitespaceFilterEnabled() — content with more
// than two consecutive spaces is filler almost everywhere it's been
// observed, even without a station-specific regex configured at all), and
// only ever advances to a new value once a fresh, genuinely different,
// accepted song comes in.
//
// Every independent consumer (MprisService, GnomosWindow's History
// recording) owns its own instance — deliberately not a single shared
// state: each instance's own dedup state advances every time Filter() is
// called, so two consumers sharing one instance would only let the FIRST
// caller for a given change ever see it as "new", silently starving the
// second. Separate instances applying identical rules avoids that, at the
// cost of each independently re-deciding the same "is this a real song"
// question — cheap, and each consumer's own NowPlaying-driven call
// frequency already differs anyway.
class RadioContentFilter
{
public:
  explicit RadioContentFilter(NosonBackend& backend) : backend_(backend) {}

  // stream_uri/content: NowPlaying::stream_uri/artist as observed right
  // now. Returns the song text to treat as current — sticky: on filler
  // (didn't pass the station's own regex or the global whitespace
  // heuristic) or a repeat of the last accepted song, the *previously*
  // accepted song is returned unchanged, not emptied out, so a caller can
  // always ask "what's the last real song on this station" and get an
  // answer even mid-ad-break. Only actually empty when nothing has ever
  // been accepted yet for this stream_uri (including right after a
  // station change, which resets this), or the station has opted out of
  // this filtering entirely (RadioMprisSettings::mpris_enabled == false).
  std::string Filter(const std::string& stream_uri, const std::string& content);

private:
  NosonBackend& backend_;
  std::string stream_key_;
  std::string last_matched_content_;
  std::string effective_content_;
};

}  // namespace gnomos
