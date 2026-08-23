// SPDX-License-Identifier: GPL-3.0-or-later

#include "player-bar.h"

#include <cstdio>

#include <adwaita.h>
#include <gdkmm/texture.h>
#include <glibmm/bytes.h>
#include <glibmm/error.h>
#include <glibmm/main.h>
#include <gtkmm/separator.h>
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

// Mirrors the system volume icon convention (low/medium/high thresholds at
// roughly a third and two-thirds) instead of always showing "high" for any
// unmuted level, which read as inaccurate at low volumes.
const char* IconForVolume(uint8_t volume, bool muted)
{
  if (muted || volume == 0)
    return "audio-volume-muted-symbolic";
  if (volume < 34)
    return "audio-volume-low-symbolic";
  if (volume < 67)
    return "audio-volume-medium-symbolic";
  return "audio-volume-high-symbolic";
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
  // "view" gives the bar its own slightly different background shade from
  // the content area above it; the separator is what actually reads as
  // "docked to the bottom edge" (no custom CSS provider anywhere in this
  // app — see ARCHITECTURE.md — so a plain Gtk::Separator does this job
  // rather than a hand-rolled top border).
  add_css_class("view");
  set_hexpand(true);
  set_vexpand(false);
  append(*Gtk::make_managed<Gtk::Separator>());

  // --- Seek bar: its own full-width row above everything else, not
  // squeezed into a column between the info/transport/volume row's side
  // groups — styled after Euphonica's own now-playing bar
  // (https://github.com/htkhiem/euphonica), which puts the scrubber on a
  // dedicated row spanning almost the entire window width, with the
  // elapsed/duration labels flanking it at the very ends rather than
  // hugging close to the bar itself. This is the actual point of moving
  // the whole panel to the bottom in the first place: a side column could
  // never give the seek bar this much usable width. ---
  auto* seek_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  seek_row->set_margin_top(6);
  seek_row->set_margin_start(16);
  seek_row->set_margin_end(16);
  elapsed_label_.add_css_class("caption");
  elapsed_label_.add_css_class("dim-label");
  seek_row->append(elapsed_label_);
  position_scale_.set_range(0, 1);
  position_scale_.set_draw_value(false);
  position_scale_.set_hexpand(true);
  position_scale_.set_valign(Gtk::Align::CENTER);
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
  seek_row->append(position_scale_);
  duration_label_.add_css_class("caption");
  duration_label_.add_css_class("dim-label");
  duration_button_.set_child(duration_label_);
  duration_button_.add_css_class("flat");
  duration_button_.set_valign(Gtk::Align::CENTER);
  duration_button_.set_tooltip_text("Gesamtdauer/Restzeit umschalten");
  duration_button_.signal_clicked().connect([this] {
    show_remaining_ = !show_remaining_;
    RenderDurationLabel();
  });
  seek_row->append(duration_button_);
  seek_row->set_visible(false);
  position_row_ = seek_row;

  // Capped at a generous max width (matching GNOME Music's own
  // PlayerToolbar, which wraps its equivalent row in the same
  // AdwClamp/maximum-size pattern) so the bar gets almost the full window
  // width on any normal or even fairly wide window, without stretching
  // into an absurdly long, hard-to-read line on an ultrawide monitor.
  GtkWidget* seek_clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(seek_clamp), 1000);
  adw_clamp_set_child(ADW_CLAMP(seek_clamp), GTK_WIDGET(seek_row->gobj()));
  append(*Glib::wrap(seek_clamp));

  auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
  bar->set_margin_top(4);
  bar->set_margin_bottom(8);
  bar->set_margin_start(14);
  bar->set_margin_end(14);
  append(*bar);

  // --- Left: art + title/artist/next-track. No hexpand and no forced
  // minimum width here (title/subtitle ellipsize down to almost nothing)
  // — the center column (transport + seek bar) is what should claim
  // available width, not this one; a hard floor here would only inflate
  // the window's own enforced minimum size for no benefit. ---
  auto* info_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  info_box->set_valign(Gtk::Align::CENTER);

  // AdwAvatar (raw C API; see the header comment on avatar_) gives free
  // native circular clipping of a custom image, no hand-rolled CSS
  // clipping needed. The icon_name fallback is music-appropriate ("no
  // album art") rather than AdwAvatar's own generic-person default, which
  // would read as "no profile picture". A fixed 48px size (AdwAvatar is
  // always exactly square) plus explicit valign/halign CENTER on the
  // button wrapping it — never FILL — so the bar's own fixed height can't
  // stretch it into an oval.
  avatar_ = adw_avatar_new(48, nullptr, FALSE);
  adw_avatar_set_icon_name(ADW_AVATAR(avatar_), "audio-x-generic-symbolic");
  auto* avatar_widget = Glib::wrap(avatar_);
  art_button_.set_child(*avatar_widget);
  art_button_.add_css_class("flat");
  art_button_.add_css_class("circular");
  art_button_.set_valign(Gtk::Align::CENTER);
  art_button_.set_halign(Gtk::Align::CENTER);
  art_button_.set_tooltip_text("Titel-Details");
  art_button_.signal_clicked().connect([this] { signal_art_clicked_.emit(); });
  info_box->append(art_button_);

  auto* text_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  text_box->set_valign(Gtk::Align::CENTER);
  text_box->set_hexpand(true);

  title_label_.set_halign(Gtk::Align::START);
  title_label_.set_ellipsize(Pango::EllipsizeMode::END);
  title_label_.add_css_class("heading");
  text_box->append(title_label_);

  subtitle_label_.set_halign(Gtk::Align::START);
  subtitle_label_.set_ellipsize(Pango::EllipsizeMode::END);
  subtitle_label_.add_css_class("dim-label");
  subtitle_label_.add_css_class("caption");
  text_box->append(subtitle_label_);

  next_track_label_.set_halign(Gtk::Align::START);
  next_track_label_.set_ellipsize(Pango::EllipsizeMode::END);
  next_track_label_.add_css_class("dim-label");
  next_track_label_.add_css_class("caption");
  next_track_label_.set_visible(false);
  text_box->append(next_track_label_);

  info_box->append(*text_box);
  bar->append(*info_box);

  // --- Center: the transport row, centered in whatever width is left
  // between the two side columns (the seek bar itself lives in its own
  // full-width row above this one — see seek_clamp above). ---
  auto* center_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  center_box->set_hexpand(true);
  center_box->set_valign(Gtk::Align::CENTER);

  auto* transport_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  transport_row->set_halign(Gtk::Align::CENTER);
  transport_row->set_valign(Gtk::Align::CENTER);

  // Every round button below gets an explicit *equal* width/height
  // size_request() plus valign(CENTER) (never the Box default of FILL) —
  // without both, the bar's own fixed height would stretch it into an
  // oval rather than a circle.
  shuffle_button_.set_icon_name("media-playlist-shuffle-symbolic");
  shuffle_button_.add_css_class("flat");
  shuffle_button_.add_css_class("circular");
  shuffle_button_.set_size_request(32, 32);
  shuffle_button_.set_valign(Gtk::Align::CENTER);
  shuffle_button_.set_tooltip_text("Zufallswiedergabe");
  shuffle_button_.signal_clicked().connect([this] { signal_shuffle_clicked_.emit(); });
  transport_row->append(shuffle_button_);

  previous_button_.set_icon_name("media-skip-backward-symbolic");
  previous_button_.add_css_class("flat");
  previous_button_.add_css_class("circular");
  previous_button_.set_size_request(36, 36);
  previous_button_.set_valign(Gtk::Align::CENTER);
  previous_button_.signal_clicked().connect([this] { signal_previous_.emit(); });
  transport_row->append(previous_button_);

  play_pause_button_.set_icon_name(IconForState(TransportState::Stopped));
  play_pause_button_.add_css_class("circular");
  play_pause_button_.add_css_class("suggested-action");
  play_pause_button_.set_size_request(44, 44);
  play_pause_button_.set_valign(Gtk::Align::CENTER);
  play_pause_button_.signal_clicked().connect([this] { signal_play_pause_.emit(); });
  transport_row->append(play_pause_button_);

  next_button_.set_icon_name("media-skip-forward-symbolic");
  next_button_.add_css_class("flat");
  next_button_.add_css_class("circular");
  next_button_.set_size_request(36, 36);
  next_button_.set_valign(Gtk::Align::CENTER);
  next_button_.signal_clicked().connect([this] { signal_next_.emit(); });
  transport_row->append(next_button_);

  repeat_button_.set_icon_name("media-playlist-repeat-symbolic");
  repeat_button_.add_css_class("flat");
  repeat_button_.add_css_class("circular");
  repeat_button_.set_size_request(32, 32);
  repeat_button_.set_valign(Gtk::Align::CENTER);
  repeat_button_.set_tooltip_text("Wiederholen");
  repeat_button_.signal_clicked().connect([this] { signal_repeat_clicked_.emit(); });
  transport_row->append(repeat_button_);

  center_box->append(*transport_row);

  bar->append(*center_box);

  // --- Right: favorite + mute + volume, a fixed-ish-width column. ---
  auto* secondary_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  secondary_row->set_valign(Gtk::Align::CENTER);

  favorite_button_.set_icon_name("starred-symbolic");
  favorite_button_.add_css_class("flat");
  favorite_button_.add_css_class("circular");
  favorite_button_.set_size_request(32, 32);
  favorite_button_.set_valign(Gtk::Align::CENTER);
  favorite_button_.set_tooltip_text("Zu Favoriten hinzufügen");
  favorite_button_.signal_clicked().connect([this] { signal_add_to_favorites_clicked_.emit(); });
  secondary_row->append(favorite_button_);

  mute_button_.set_icon_name("audio-volume-high-symbolic");
  mute_button_.add_css_class("flat");
  mute_button_.add_css_class("circular");
  mute_button_.set_size_request(32, 32);
  mute_button_.set_valign(Gtk::Align::CENTER);
  mute_button_.signal_clicked().connect([this] {
    muted_ = !muted_;
    signal_mute_toggled_.emit(muted_);
  });
  secondary_row->append(mute_button_);

  volume_scale_.set_range(0, 100);
  volume_scale_.set_size_request(120, -1);
  volume_scale_.set_valign(Gtk::Align::CENTER);
  volume_scale_.signal_value_changed().connect([this] {
    if (!suppress_volume_signal_)
      signal_volume_changed_.emit(volume_scale_.get_value());
  });
  secondary_row->append(volume_scale_);

  bar->append(*secondary_row);

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
  volume_scale_.set_tooltip_text(std::to_string(volume.volume) + "%");

  muted_ = volume.muted;
  mute_button_.set_icon_name(IconForVolume(volume.volume, volume.muted));
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
