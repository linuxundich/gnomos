// SPDX-License-Identifier: GPL-3.0-or-later

#include "player-bar.h"

#include <cstdio>

#include <adwaita.h>
#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>
#include <glibmm/main.h>
#include <pangomm/layout.h>

#include "art-cache.h"

namespace gnomos
{

namespace
{

const char* IconForState(TransportState state)
{
  return state == TransportState::Playing ? "media-playback-pause-symbolic"
                                           : "media-playback-start-symbolic";
}

std::string FormatTime(unsigned seconds)
{
  unsigned h = seconds / 3600;
  unsigned m = (seconds % 3600) / 60;
  unsigned s = seconds % 60;
  char buf[16];
  if (h > 0)
    std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
  else
    std::snprintf(buf, sizeof(buf), "%u:%02u", m, s);
  return buf;
}

}  // namespace

PlayerBar::PlayerBar()
: Gtk::Box(Gtk::Orientation::VERTICAL, 0)
{
  // "view" gives the panel its own slightly different background shade
  // from the surrounding window chrome — visual separation matching
  // Euphonica's own Now Playing panel, which sits on a distinctly shaded
  // (there, image-derived) background from the rest of the window. Applied
  // to `this` (which fills the whole panel area content_split_view_ gives it)
  // rather than to the centered content block below, so the shaded
  // background actually covers the full panel instead of just a
  // center-aligned island within it.
  add_css_class("view");
  set_hexpand(true);
  set_vexpand(true);
  set_size_request(280, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(24);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);
  content->set_valign(Gtk::Align::CENTER);
  content->set_vexpand(true);
  append(*content);

  // Large circular art — AdwAvatar (raw C API; see the header comment on
  // avatar_) gives free native clipping of a custom image, no hand-rolled
  // CSS clipping needed. The icon_name fallback is music-appropriate
  // ("no album art") rather than AdwAvatar's own generic-person default,
  // which would read as "no profile picture".
  avatar_ = adw_avatar_new(200, nullptr, FALSE);
  adw_avatar_set_icon_name(ADW_AVATAR(avatar_), "audio-x-generic-symbolic");
  auto* avatar_widget = Glib::wrap(avatar_);
  art_button_.set_child(*avatar_widget);
  art_button_.add_css_class("flat");
  art_button_.add_css_class("circular");
  art_button_.set_halign(Gtk::Align::CENTER);
  art_button_.set_tooltip_text("Titel-Details");
  art_button_.signal_clicked().connect([this] { signal_art_clicked_.emit(); });
  content->append(art_button_);

  title_label_.set_halign(Gtk::Align::CENTER);
  title_label_.set_justify(Gtk::Justification::CENTER);
  title_label_.set_wrap(true);
  title_label_.set_lines(2);
  title_label_.set_ellipsize(Pango::EllipsizeMode::END);
  title_label_.add_css_class("title-2");
  content->append(title_label_);

  subtitle_label_.set_halign(Gtk::Align::CENTER);
  subtitle_label_.set_justify(Gtk::Justification::CENTER);
  subtitle_label_.set_wrap(true);
  subtitle_label_.set_ellipsize(Pango::EllipsizeMode::END);
  subtitle_label_.add_css_class("dim-label");
  content->append(subtitle_label_);

  auto* position_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  elapsed_label_.add_css_class("caption");
  elapsed_label_.add_css_class("dim-label");
  position_row->append(elapsed_label_);
  position_scale_.set_range(0, 1);
  position_scale_.set_draw_value(false);
  position_scale_.set_hexpand(true);
  // Gtk::Range's own internal drag gesture claims the pointer sequence, so
  // a separately-added Gtk::GestureClick sibling never reliably sees a
  // "released" for it (confirmed live: the marker moved, but no seek ever
  // fired). signal_value_changed() itself, however, always fires — for a
  // drag, a click-to-jump, and keyboard nudges alike, and for our own
  // programmatic set_value() calls in UpdatePosition() too (guarded off via
  // suppress_position_signal_). A short debounce coalesces a drag's many
  // intermediate ticks into a single seek once the value settles, instead
  // of flooding the device with a blocking SOAP call per tick.
  position_scale_.signal_value_changed().connect([this] {
    if (suppress_position_signal_)
      return;
    user_seeking_ = true;
    seek_debounce_connection_.disconnect();
    seek_debounce_connection_ = Glib::signal_timeout().connect(
        [this] {
          user_seeking_ = false;
          signal_seek_requested_.emit(static_cast<unsigned>(position_scale_.get_value()));
          return false;  // one-shot
        },
        400);
  });
  position_row->append(position_scale_);
  duration_label_.add_css_class("caption");
  duration_label_.add_css_class("dim-label");
  duration_button_.set_child(duration_label_);
  duration_button_.add_css_class("flat");
  duration_button_.set_tooltip_text("Gesamtdauer/Restzeit umschalten");
  duration_button_.signal_clicked().connect([this] {
    show_remaining_ = !show_remaining_;
    RenderDurationLabel();
  });
  position_row->append(duration_button_);
  position_row->set_visible(false);
  content->append(*position_row);
  position_row_ = position_row;

  next_track_label_.set_halign(Gtk::Align::CENTER);
  next_track_label_.set_justify(Gtk::Justification::CENTER);
  next_track_label_.set_ellipsize(Pango::EllipsizeMode::END);
  next_track_label_.add_css_class("dim-label");
  next_track_label_.add_css_class("caption");
  next_track_label_.set_visible(false);
  content->append(next_track_label_);

  // Transport controls — centered row, matching Euphonica's Now Playing
  // panel layout (shuffle/prev/play-pause/next/repeat in one line, rather
  // than split across a horizontal strip).
  auto* transport_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  transport_row->set_halign(Gtk::Align::CENTER);
  transport_row->set_margin_top(6);

  shuffle_button_.set_icon_name("media-playlist-shuffle-symbolic");
  shuffle_button_.add_css_class("flat");
  shuffle_button_.set_tooltip_text("Zufallswiedergabe");
  shuffle_button_.signal_clicked().connect([this] { signal_shuffle_clicked_.emit(); });
  transport_row->append(shuffle_button_);

  previous_button_.set_icon_name("media-skip-backward-symbolic");
  previous_button_.add_css_class("flat");
  previous_button_.add_css_class("circular");
  previous_button_.signal_clicked().connect([this] { signal_previous_.emit(); });
  transport_row->append(previous_button_);

  play_pause_button_.set_icon_name(IconForState(TransportState::Stopped));
  play_pause_button_.add_css_class("circular");
  play_pause_button_.add_css_class("suggested-action");
  play_pause_button_.set_size_request(52, 52);
  play_pause_button_.signal_clicked().connect([this] { signal_play_pause_.emit(); });
  transport_row->append(play_pause_button_);

  next_button_.set_icon_name("media-skip-forward-symbolic");
  next_button_.add_css_class("flat");
  next_button_.add_css_class("circular");
  next_button_.signal_clicked().connect([this] { signal_next_.emit(); });
  transport_row->append(next_button_);

  repeat_button_.set_icon_name("media-playlist-repeat-symbolic");
  repeat_button_.add_css_class("flat");
  repeat_button_.set_tooltip_text("Wiederholen");
  repeat_button_.signal_clicked().connect([this] { signal_repeat_clicked_.emit(); });
  transport_row->append(repeat_button_);

  content->append(*transport_row);

  // Secondary row — favorite + mute + volume, also centered.
  auto* secondary_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  secondary_row->set_halign(Gtk::Align::CENTER);
  secondary_row->set_margin_top(6);

  favorite_button_.set_icon_name("starred-symbolic");
  favorite_button_.add_css_class("flat");
  favorite_button_.set_tooltip_text("Zu Favoriten hinzufügen");
  favorite_button_.signal_clicked().connect([this] { signal_add_to_favorites_clicked_.emit(); });
  secondary_row->append(favorite_button_);

  mute_button_.set_icon_name("audio-volume-high-symbolic");
  mute_button_.add_css_class("flat");
  mute_button_.signal_clicked().connect([this] {
    muted_ = !muted_;
    signal_mute_toggled_.emit(muted_);
  });
  secondary_row->append(mute_button_);

  volume_scale_.set_range(0, 100);
  volume_scale_.set_size_request(140, -1);
  volume_scale_.set_valign(Gtk::Align::CENTER);
  volume_scale_.signal_value_changed().connect([this] {
    if (!suppress_volume_signal_)
      signal_volume_changed_.emit(volume_scale_.get_value());
  });
  secondary_row->append(volume_scale_);

  content->append(*secondary_row);

  SetEnabled(false);
}

void PlayerBar::Update(const NowPlaying& now_playing)
{
  if (!now_playing.valid)
  {
    title_label_.set_text("Keine Wiedergabe");
    subtitle_label_.set_text("");
    play_pause_button_.set_icon_name(IconForState(TransportState::Stopped));
    if (position_row_)
      position_row_->set_visible(false);
    return;
  }

  if (position_row_)
    position_row_->set_visible(now_playing.duration > 0);
  if (now_playing.duration > 0)
  {
    last_duration_seconds_ = now_playing.duration;
    RenderDurationLabel();
    if (!user_seeking_)
    {
      suppress_position_signal_ = true;
      position_scale_.set_range(0, now_playing.duration);
      suppress_position_signal_ = false;
    }
  }

  title_label_.set_text(now_playing.title.empty() ? "Unbekannter Titel" : now_playing.title);
  std::string subtitle = now_playing.artist;
  if (!now_playing.album.empty())
    subtitle += (subtitle.empty() ? "" : " — ") + now_playing.album;
  subtitle_label_.set_text(subtitle);

  play_pause_button_.set_icon_name(IconForState(now_playing.state));
  // set_active() only fires ToggleButton's own signal_toggled(), never the
  // inherited Button::signal_clicked() this widget actually connects to
  // (see the header comment on signal_shuffle_clicked()), so no suppress
  // flag is needed here the way volume_scale_ needs one.
  shuffle_button_.set_active(now_playing.shuffle);
  repeat_button_.set_active(now_playing.repeat != RepeatMode::Off);
  repeat_button_.set_icon_name(now_playing.repeat == RepeatMode::One ? "media-playlist-repeat-song-symbolic"
                                                                      : "media-playlist-repeat-symbolic");
  repeat_button_.set_tooltip_text(now_playing.repeat == RepeatMode::One ? "Titel wiederholen" : "Wiederholen");
  // Not every source supports shuffle/repeat at all (radio, line-in) —
  // see NowPlaying::shuffle_supported/repeat_supported's own comment.
  // Independent of SetEnabled()'s blanket on/off, which only ever runs
  // once when a zone becomes ready, before any real track is playing yet.
  shuffle_button_.set_sensitive(now_playing.shuffle_supported);
  repeat_button_.set_sensitive(now_playing.repeat_supported);
  // Same device-reported-capability gating as shuffle/repeat above, but
  // for AVTProperty::CurrentTransportActions — some radio stations don't
  // support Next/Previous at all.
  next_button_.set_sensitive(now_playing.can_go_next);
  previous_button_.set_sensitive(now_playing.can_go_previous);

  LoadArt(now_playing.art_uri);
}

void PlayerBar::UpdatePosition(unsigned position_seconds, unsigned duration_seconds)
{
  if (duration_seconds == 0 || user_seeking_)
    return;  // live stream (nothing to show) or mid-drag (don't fight the pointer)

  elapsed_label_.set_text(FormatTime(position_seconds));
  last_position_seconds_ = position_seconds;
  RenderDurationLabel();
  suppress_position_signal_ = true;
  position_scale_.set_value(static_cast<double>(position_seconds));
  suppress_position_signal_ = false;
}

void PlayerBar::RenderDurationLabel()
{
  if (last_duration_seconds_ == 0)
    return;
  if (show_remaining_)
  {
    unsigned remaining =
        last_position_seconds_ < last_duration_seconds_ ? last_duration_seconds_ - last_position_seconds_ : 0;
    duration_label_.set_text("-" + FormatTime(remaining));
  }
  else
  {
    duration_label_.set_text(FormatTime(last_duration_seconds_));
  }
}

void PlayerBar::UpdateNextTrack(const std::string& title)
{
  next_track_label_.set_visible(!title.empty());
  next_track_label_.set_text("Weiter: " + title);
}

void PlayerBar::LoadArt(const std::string& uri)
{
  if (uri == current_art_uri_)
    return;
  current_art_uri_ = uri;
  unsigned generation = ++art_generation_;

  if (art_cancellable_)
    art_cancellable_->cancel();

  if (uri.empty())
  {
    adw_avatar_set_custom_image(ADW_AVATAR(avatar_), nullptr);
    return;
  }

  if (auto cached = ArtCache::Instance().Get(uri))
  {
    adw_avatar_set_custom_image(ADW_AVATAR(avatar_), GDK_PAINTABLE(cached->gobj()));
    return;
  }

  art_cancellable_ = Gio::Cancellable::create();
  auto file = Gio::File::create_for_uri(uri);
  file->load_contents_async(
      [this, file, generation](Glib::RefPtr<Gio::AsyncResult>& result) { OnArtLoaded(result, file, generation); },
      art_cancellable_);
}

void PlayerBar::OnArtLoaded(Glib::RefPtr<Gio::AsyncResult>& result, const Glib::RefPtr<Gio::File>& file,
                             unsigned generation)
{
  if (generation != art_generation_)
    return;  // superseded by a newer track before this load finished

  try
  {
    char* contents = nullptr;
    gsize length = 0;
    if (file->load_contents_finish(result, contents, length) && contents)
    {
      auto bytes = Glib::Bytes::create(contents, length);
      g_free(contents);
      auto texture = ArtCache::Instance().Put(current_art_uri_, bytes);
      if (texture)
        adw_avatar_set_custom_image(ADW_AVATAR(avatar_), GDK_PAINTABLE(texture->gobj()));
      else
        adw_avatar_set_custom_image(ADW_AVATAR(avatar_), nullptr);
    }
    else
    {
      adw_avatar_set_custom_image(ADW_AVATAR(avatar_), nullptr);
    }
  }
  catch (const Glib::Error&)
  {
    adw_avatar_set_custom_image(ADW_AVATAR(avatar_), nullptr);
  }
}

void PlayerBar::UpdateVolume(const VolumeInfo& volume)
{
  suppress_volume_signal_ = true;
  volume_scale_.set_value(volume.volume);
  suppress_volume_signal_ = false;

  muted_ = volume.muted;
  mute_button_.set_icon_name(volume.muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
}

void PlayerBar::SetEnabled(bool enabled)
{
  previous_button_.set_sensitive(enabled);
  play_pause_button_.set_sensitive(enabled);
  next_button_.set_sensitive(enabled);
  shuffle_button_.set_sensitive(enabled);
  repeat_button_.set_sensitive(enabled);
  favorite_button_.set_sensitive(enabled);
  art_button_.set_sensitive(enabled);
  mute_button_.set_sensitive(enabled);
  volume_scale_.set_sensitive(enabled);
  if (!enabled)
  {
    if (position_row_)
      position_row_->set_visible(false);
    next_track_label_.set_visible(false);
    title_label_.set_text("Kein Sonos-Gerät ausgewählt");
    subtitle_label_.set_text("");
    LoadArt("");
  }
}

}  // namespace gnomos
