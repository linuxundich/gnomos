// SPDX-License-Identifier: GPL-3.0-or-later

#include "mini-player-window.h"

#include <cstdio>

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <pangomm/layout.h>

#include "widgets/playback-ui.h"

namespace gnomos
{


MiniPlayerWindow::MiniPlayerWindow()
{
  set_title("Gnomos – Mini-Player");
  set_default_size(260, 360);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  art_.set_halign(Gtk::Align::CENTER);
  art_.add_css_class("card");
  content->append(art_);

  title_label_.set_halign(Gtk::Align::CENTER);
  title_label_.set_justify(Gtk::Justification::CENTER);
  title_label_.set_ellipsize(Pango::EllipsizeMode::END);
  title_label_.add_css_class("heading");
  content->append(title_label_);

  subtitle_label_.set_halign(Gtk::Align::CENTER);
  subtitle_label_.set_justify(Gtk::Justification::CENTER);
  subtitle_label_.set_ellipsize(Pango::EllipsizeMode::END);
  subtitle_label_.add_css_class("dimmed");
  subtitle_label_.add_css_class("caption");
  content->append(subtitle_label_);

  auto* seek_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  seek_row->set_margin_top(6);
  elapsed_label_.add_css_class("caption");
  elapsed_label_.add_css_class("dimmed");
  seek_row->append(elapsed_label_);
  position_scale_.set_range(0, 1);
  position_scale_.set_draw_value(false);
  position_scale_.set_hexpand(true);
  position_scale_.set_valign(Gtk::Align::CENTER);
  // Same debounce PlayerBar's own position_scale_ uses — see its header
  // comment — coalescing a drag's many intermediate ticks into one seek
  // once the value settles, rather than flooding the device with a
  // blocking SOAP call per tick.
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
  duration_label_.add_css_class("dimmed");
  seek_row->append(duration_label_);
  seek_row->set_visible(false);
  seek_row_ = seek_row;
  content->append(*seek_row);

  // Same "explicit equal width/height + valign(CENTER), never the Box
  // default of FILL" convention PlayerBar's own round transport buttons
  // already establish — without both, a fixed-height row stretches a
  // circular button into an oval.
  auto* transport_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  transport_row->set_halign(Gtk::Align::CENTER);
  transport_row->set_margin_top(6);

  previous_button_.set_icon_name("media-skip-backward-symbolic");
  previous_button_.add_css_class("flat");
  previous_button_.add_css_class("circular");
  previous_button_.set_size_request(36, 36);
  previous_button_.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(previous_button_, "Vorheriger Titel");
  previous_button_.signal_clicked().connect([this] { signal_previous_.emit(); });
  transport_row->append(previous_button_);

  play_pause_button_.set_icon_name(PlayPauseIconForState(TransportState::Stopped));
  play_pause_button_.add_css_class("circular");
  play_pause_button_.add_css_class("suggested-action");
  play_pause_button_.set_size_request(48, 48);
  play_pause_button_.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(play_pause_button_, PlayPauseLabelForState(TransportState::Stopped));
  play_pause_button_.signal_clicked().connect([this] { signal_play_pause_.emit(); });
  transport_row->append(play_pause_button_);

  next_button_.set_icon_name("media-skip-forward-symbolic");
  next_button_.add_css_class("flat");
  next_button_.add_css_class("circular");
  next_button_.set_size_request(36, 36);
  next_button_.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(next_button_, "Nächster Titel");
  next_button_.signal_clicked().connect([this] { signal_next_.emit(); });
  transport_row->append(next_button_);

  content->append(*transport_row);

  set_child(*content);

  SetEnabled(false);
}

void MiniPlayerWindow::Update(const NowPlaying& now_playing)
{
  if (!now_playing.valid)
  {
    title_label_.set_text("Keine Wiedergabe");
    subtitle_label_.set_text("");
    play_pause_button_.set_icon_name(PlayPauseIconForState(TransportState::Stopped));
    seek_row_->set_visible(false);
    art_.SetArtUri("");
    return;
  }

  title_label_.set_text(now_playing.title.empty() ? "Unbekannter Titel" : now_playing.title);
  std::string subtitle = now_playing.artist;
  if (!now_playing.album.empty())
    subtitle += (subtitle.empty() ? "" : " — ") + now_playing.album;
  subtitle_label_.set_text(subtitle);

  play_pause_button_.set_icon_name(PlayPauseIconForState(now_playing.state));
  SetButtonLabel(play_pause_button_, PlayPauseLabelForState(now_playing.state));
  // Same device-reported-capability gating PlayerBar's own transport
  // buttons use — some radio stations don't support Next/Previous at all.
  next_button_.set_sensitive(now_playing.can_go_next);
  previous_button_.set_sensitive(now_playing.can_go_previous);

  seek_row_->set_visible(now_playing.duration > 0);
  if (now_playing.duration > 0)
  {
    last_duration_seconds_ = now_playing.duration;
    duration_label_.set_text(FormatTime(now_playing.duration));
    if (!user_seeking_)
    {
      suppress_position_signal_ = true;
      position_scale_.set_range(0, now_playing.duration);
      suppress_position_signal_ = false;
    }
  }

  art_.SetArtUri(now_playing.art_uri);
}

void MiniPlayerWindow::UpdatePosition(unsigned position_seconds, unsigned duration_seconds)
{
  if (duration_seconds == 0 || user_seeking_)
    return;  // live stream (nothing to show) or mid-drag (don't fight the pointer)

  elapsed_label_.set_text(FormatTime(position_seconds));
  suppress_position_signal_ = true;
  position_scale_.set_value(static_cast<double>(position_seconds));
  suppress_position_signal_ = false;
}

void MiniPlayerWindow::SetEnabled(bool enabled)
{
  previous_button_.set_sensitive(enabled);
  play_pause_button_.set_sensitive(enabled);
  next_button_.set_sensitive(enabled);
}

}  // namespace gnomos
