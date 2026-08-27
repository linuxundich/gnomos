// SPDX-License-Identifier: GPL-3.0-or-later

#include "radio-content-filter.h"

#include <regex>

namespace gnomos
{

namespace
{

// Wraps std::regex_search so a malformed user-supplied pattern (typed into
// GnomosWindow's per-station settings dialog) can't crash the app — it
// just fails to match, same as any other non-matching content, so the
// station simply stops accepting new "songs" until the pattern is fixed.
bool RegexMatches(const std::string& content, const std::string& pattern)
{
  try
  {
    return std::regex_search(content, std::regex(pattern));
  }
  catch (const std::regex_error&)
  {
    return false;
  }
}

// More than two consecutive spaces is filler almost everywhere it's been
// observed live — padding around an ad slot, a station ident, or similar
// non-song content — even on a station with no custom regex configured at
// all. Real "song / artist"-style content essentially never contains a
// run like this.
bool HasExcessiveWhitespace(const std::string& content)
{
  int run = 0;
  for (char c : content)
  {
    if (c == ' ')
    {
      if (++run > 2)
        return true;
    }
    else
    {
      run = 0;
    }
  }
  return false;
}

}  // namespace

std::string RadioContentFilter::Filter(const std::string& stream_uri, const std::string& content)
{
  if (stream_uri != stream_key_)
  {
    // Station changed (or this is the first radio stream this instance
    // has seen) — start fresh rather than carrying over whatever the
    // previous station last accepted.
    stream_key_ = stream_uri;
    last_matched_content_.clear();
    effective_content_.clear();
  }

  RadioMprisSettings settings = backend_.GetRadioMprisSettings(stream_uri);
  if (!settings.mpris_enabled)
  {
    // Opted out entirely for this station.
    effective_content_.clear();
    return effective_content_;
  }

  bool matches = (settings.regex.empty() || RegexMatches(content, settings.regex)) &&
                 !(backend_.GetRadioSpamWhitespaceFilterEnabled() && HasExcessiveWhitespace(content));
  if (matches && content != last_matched_content_)
  {
    // A genuinely new, accepted song — advance both the dedup key and
    // what's actually returned.
    last_matched_content_ = content;
    effective_content_ = content;
  }
  // else: filler that didn't pass, or a repeat of the same song already
  // accepted — leave effective_content_ untouched.
  return effective_content_;
}

}  // namespace gnomos
