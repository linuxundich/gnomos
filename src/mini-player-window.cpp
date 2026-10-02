// SPDX-License-Identifier: GPL-3.0-or-later

#include "mini-player-window.h"

#include <cstdio>

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/overlay.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/windowhandle.h>
#include <pangomm/layout.h>

#include "widgets/playback-ui.h"

namespace gnomos
{


MiniPlayerWindow::MiniPlayerWindow()
{
  set_title("Gnomos – Mini-Player");
  set_default_size(320, 320);
  set_decorated(false);
  add_css_class("mini-player");

  auto* overlay = Gtk::make_managed<Gtk::Overlay>();

  art_.set_content_fit(Gtk::ContentFit::COVER);
  art_.set_can_shrink(true);
  art_.set_size_request(220, 220);
  art_.add_css_class("mini-player-art");
  // Dragging the cover moves the window — there's no title bar to grab.
  auto* handle = Gtk::make_managed<Gtk::WindowHandle>();
  handle->set_child(art_);
  overlay->set_child(*handle);

  auto* close_button = Gtk::make_managed<Gtk::Button>();
  close_button->set_icon_name("window-close-symbolic");
  close_button->add_css_class("circular");
  close_button->add_css_class("osd");
  SetButtonLabel(*close_button, "Zurück zum Hauptfenster");
  close_button->signal_clicked().connect([this] { close(); });
  close_revealer_.set_child(*close_button);
  close_revealer_.set_transition_type(Gtk::RevealerTransitionType::CROSSFADE);
  close_revealer_.set_halign(Gtk::Align::END);
  close_revealer_.set_valign(Gtk::Align::START);
  close_revealer_.set_margin_top(8);
  close_revealer_.set_margin_end(8);
  overlay->add_overlay(close_revealer_);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  content->add_css_class("mini-player-controls");

  title_label_.set_halign(Gtk::Align::START);
  title_label_.set_ellipsize(Pango::EllipsizeMode::END);
  title_label_.add_css_class("heading");
  content->append(title_label_);

  subtitle_label_.set_halign(Gtk::Align::START);
  subtitle_label_.set_ellipsize(Pango::EllipsizeMode::END);
  subtitle_label_.add_css_class("caption");
  subtitle_label_.add_css_class("mini-player-subtitle");
  content->append(subtitle_label_);

  auto* seek_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  elapsed_label_.add_css_class("caption");
  elapsed_label_.add_css_class("numeric");
  seek_row->append(elapsed_label_);
  position_scale_.set_range(0, 1);
  position_scale_.set_draw_value(false);
  position_scale_.set_hexpand(true);
  position_scale_.set_valign(Gtk::Align::CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(position_scale_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Wiedergabeposition", -1);
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
  duration_label_.add_css_class("numeric");
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
  play_pause_button_.add_css_class("play-button");
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

  controls_revealer_.set_child(*content);
  controls_revealer_.set_transition_type(Gtk::RevealerTransitionType::CROSSFADE);
  controls_revealer_.set_transition_duration(250);
  controls_revealer_.set_valign(Gtk::Align::END);
  overlay->add_overlay(controls_revealer_);

  // Controls show while the pointer is over the window, and always while
  // nothing plays (so the play button is there when you need it).
  auto motion = Gtk::EventControllerMotion::create();
  motion->signal_enter().connect([this](double, double) {
    hovering_ = true;
    UpdateControlsVisibility();
  });
  motion->signal_leave().connect([this] {
    hovering_ = false;
    UpdateControlsVisibility();
  });
  add_controller(motion);

  set_child(*overlay);

  SetEnabled(false);
  UpdateControlsVisibility();
}

void MiniPlayerWindow::UpdateControlsVisibility()
{
  bool show = hovering_ || !playing_;
  controls_revealer_.set_reveal_child(show);
  close_revealer_.set_reveal_child(show);
}

void MiniPlayerWindow::SetCover(const Glib::RefPtr<Gdk::Texture>& texture, const CoverPalette& palette)
{
  art_.set_paintable(texture);
  // The play button takes the cover's main color, like the main window's
  // — kept mid-light so the white icon on it reads.
  if (!palette_css_)
  {
    palette_css_ = Gtk::CssProvider::create();
    Gtk::StyleContext::add_provider_for_display(get_display(), palette_css_,
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
  if (palette.valid)
  {
    Gdk::RGBA button = ClampLightness(palette.base, 0.38, 0.55);
    palette_css_->load_from_string(".mini-player .play-button { background-color: " + ToCssHex(button) +
                                   "; color: " + (PrefersLightText(button) ? "#ffffff" : "#000000") + "; }");
  }
  else
  {
    palette_css_->load_from_string("");
  }
}

void MiniPlayerWindow::Update(const NowPlaying& now_playing)
{
  if (!now_playing.valid)
  {
    title_label_.set_text("Keine Wiedergabe");
    subtitle_label_.set_text("");
    play_pause_button_.set_icon_name(PlayPauseIconForState(TransportState::Stopped));
    seek_row_->set_visible(false);
    playing_ = false;
    UpdateControlsVisibility();
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

  playing_ = now_playing.state == TransportState::Playing;
  UpdateControlsVisibility();
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
