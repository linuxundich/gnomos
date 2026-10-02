// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gdkmm/texture.h>
#include <gtkmm/button.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/label.h>
#include <gtkmm/picture.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scale.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <sigc++/sigc++.h>

#include "backend/noson-types.h"
#include "widgets/cover-palette.h"

namespace gnomos
{

// A small, standalone secondary window mirroring PlayerBar's core controls
// (art, title/artist, previous/play-pause/next, seek) — a "poster": the
// cover fills the whole (square, resizable) window, and title and
// controls fade in over a dark gradient at the bottom while the pointer is
// over it or nothing is playing. No title bar; dragging the cover moves
// the window, a small button in the corner closes it and brings the main
// window back. Precedent: Spotify/Apple Music/VLC's own mini players.
//
// Deliberately NOT "always on top": GTK4 removed gtk_window_set_keep_above()
// entirely, and Wayland compositors (this project's actual target
// platform) give an ordinary top-level window no way to request that
// without a layer-shell-style protocol GTK4/libadwaita doesn't use here —
// confirmed against the real platform before building this at all, rather
// than promising "always on top" and silently not delivering it. Still
// genuinely useful as a much smaller footprint than the full window while
// the Sonos system keeps playing regardless of what any app window is
// doing — same spirit run_in_background_ already established.
//
// Purely a view, same division of labor PlayerBar itself already follows:
// emits signals for user actions and exposes Update*() setters;
// GnomosWindow wires it to NosonBackend, mirroring the exact same calls it
// already makes to player_bar_ (see ShowMiniPlayerWindow()'s own
// construction site).
class MiniPlayerWindow : public Gtk::Window
{
public:
  MiniPlayerWindow();

  void Update(const NowPlaying& now_playing);
  // duration_seconds == 0 (live stream) hides the seek row entirely, same
  // convention PlayerBar's own UpdatePosition() uses.
  void UpdatePosition(unsigned position_seconds, unsigned duration_seconds);
  // Disables the transport buttons while no zone is selected — same
  // "neutral state" PlayerBar::SetEnabled() gives its own controls.
  void SetEnabled(bool enabled);
  // The cover comes from NowPlayingView, which has already loaded and
  // decoded it (and picked its colors, which tint the play button here).
  void SetCover(const Glib::RefPtr<Gdk::Texture>& texture, const CoverPalette& palette);

  sigc::signal<void()>& signal_play_pause() { return signal_play_pause_; }
  sigc::signal<void()>& signal_next() { return signal_next_; }
  sigc::signal<void()>& signal_previous() { return signal_previous_; }
  // Fires ~400ms after the seek bar's value last changed by user
  // interaction (debounced; see the constructor's comment on
  // position_scale_), never from UpdatePosition()'s own programmatic
  // set_value() calls.
  sigc::signal<void(unsigned)>& signal_seek_requested() { return signal_seek_requested_; }

private:
  void UpdateControlsVisibility();

  Gtk::Picture art_;
  Gtk::Revealer controls_revealer_;
  Gtk::Revealer close_revealer_;
  bool hovering_ = false;
  bool playing_ = false;
  Glib::RefPtr<Gtk::CssProvider> palette_css_;
  Gtk::Label title_label_;
  Gtk::Label subtitle_label_;
  Gtk::Button previous_button_;
  Gtk::Button play_pause_button_;
  Gtk::Button next_button_;
  Gtk::Label elapsed_label_;
  Gtk::Scale position_scale_;
  Gtk::Label duration_label_;
  // The seek row as a whole — hidden entirely for a live stream (duration
  // == 0), same as PlayerBar's own position_row_.
  Gtk::Widget* seek_row_ = nullptr;

  // Guards position_scale_'s signal_value_changed() against firing for our
  // own programmatic set_value()/set_range() calls in Update()/
  // UpdatePosition() (suppress_position_signal_), and keeps a 1s position
  // tick arriving mid-drag from fighting the pointer (user_seeking_) — the
  // exact same pair PlayerBar's own position_scale_ already relies on, see
  // its header comment.
  bool suppress_position_signal_ = false;
  bool user_seeking_ = false;
  sigc::connection seek_debounce_connection_;
  unsigned last_duration_seconds_ = 0;

  sigc::signal<void()> signal_play_pause_;
  sigc::signal<void()> signal_next_;
  sigc::signal<void()> signal_previous_;
  sigc::signal<void(unsigned)> signal_seek_requested_;
};

}  // namespace gnomos
