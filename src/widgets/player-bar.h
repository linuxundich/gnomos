// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <giomm/asyncresult.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/connection.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"

namespace gnomos
{

// The "Now Playing" bottom bar: cover art + title/artist on the left,
// transport controls above a wide seek bar in the middle, favorite/mute/
// volume on the right — a horizontal bar spanning the full window width,
// docked along the bottom (see GnomosWindow's root_box), rather than a
// side panel. Deliberately gives the seek bar generous, hexpand()'d width
// rather than the ~250px a side panel could ever offer it. Every round
// icon button gets an explicit equal width/height size_request() plus
// valign(CENTER) so the bar's own fixed height can never stretch it into
// an oval. Purely a view: it emits signals for user actions and exposes
// Update*() setters; GnomosWindow wires it to NosonBackend so this widget
// has no knowledge of libnoson.
class PlayerBar : public Gtk::Box
{
public:
  PlayerBar();

  void Update(const NowPlaying& now_playing);
  // Called on a 1s timer by GnomosWindow (UPnP doesn't push elapsed
  // position; see NosonBackend::RefreshPositionAsync()'s comment) with
  // seconds. duration comes from the same NowPlaying as Update() gets;
  // duration == 0 (live stream) hides the seek bar entirely rather than
  // showing a meaningless 0/0.
  void UpdatePosition(unsigned position_seconds, unsigned duration_seconds);
  void UpdateVolume(const VolumeInfo& volume);
  // title of the next queue track, or empty to hide the hint entirely —
  // GnomosWindow computes this from GetQueue()/NowPlaying::current_queue_index,
  // the same pairing that drives QueueView's now-playing highlight, so it's
  // only ever shown when we're confidently playing from the queue (not a
  // stream) and there's another track queued up after this one.
  void UpdateNextTrack(const std::string& title);
  // Disables controls and shows a neutral state while no zone is selected.
  void SetEnabled(bool enabled);

  sigc::signal<void()>& signal_play_pause() { return signal_play_pause_; }
  sigc::signal<void()>& signal_next() { return signal_next_; }
  sigc::signal<void()>& signal_previous() { return signal_previous_; }
  sigc::signal<void(double)>& signal_volume_changed() { return signal_volume_changed_; }
  sigc::signal<void(bool)>& signal_mute_toggled() { return signal_mute_toggled_; }
  // Fire on user click only (Gtk::Button::signal_clicked(), not
  // ToggleButton's own signal_toggled(), which also fires for the
  // programmatic set_active() calls Update() makes) — the backend decides
  // the next mode from the *current* one, so no bool payload is needed.
  sigc::signal<void()>& signal_shuffle_clicked() { return signal_shuffle_clicked_; }
  sigc::signal<void()>& signal_repeat_clicked() { return signal_repeat_clicked_; }
  sigc::signal<void()>& signal_add_to_favorites_clicked() { return signal_add_to_favorites_clicked_; }
  // Fires ~400ms after the seek bar's value last changed by user
  // interaction (debounced; see the constructor's comment on
  // position_scale_), never from UpdatePosition()'s own programmatic
  // set_value() calls.
  sigc::signal<void(unsigned)>& signal_seek_requested() { return signal_seek_requested_; }
  sigc::signal<void()>& signal_art_clicked() { return signal_art_clicked_; }

private:
  void LoadArt(const std::string& uri);
  void OnArtLoaded(Glib::RefPtr<Gio::AsyncResult>& result, const Glib::RefPtr<Gio::File>& file, unsigned generation);
  void RenderDurationLabel();

  // AdwAvatar (raw C API — no gtkmm binding exists for libadwaita, same
  // reasoning as header_bar_/toast_overlay_ in GnomosWindow) gives free,
  // native circular clipping of a custom GdkPaintable — the "vinyl record"
  // look Euphonica's Now Playing panel uses — without hand-rolling CSS
  // clip-path/border-radius tricks on a plain Gtk::Image.
  GtkWidget* avatar_ = nullptr;
  Gtk::Button art_button_;
  std::string current_art_uri_;
  // Bumped on every LoadArt() call so a slow, superseded load (an older
  // track's art arriving after a newer track already started loading)
  // can detect it's stale and not clobber the current image.
  unsigned art_generation_ = 0;
  Glib::RefPtr<Gio::Cancellable> art_cancellable_;

  Gtk::Label title_label_;
  Gtk::Label subtitle_label_;
  Gtk::Label next_track_label_;
  // Read-only — see NowPlaying::crossfade_enabled's own comment for why
  // there's no toggle. Same visible-only-when-relevant treatment as
  // next_track_label_, but independent of it (no next-track hint doesn't
  // mean crossfade info shouldn't still show).
  Gtk::Label crossfade_label_;
  Gtk::Button previous_button_;
  Gtk::Button play_pause_button_;
  Gtk::Button next_button_;
  Gtk::ToggleButton shuffle_button_;
  Gtk::ToggleButton repeat_button_;
  Gtk::Button favorite_button_;
  Gtk::Button mute_button_;
  Gtk::Scale volume_scale_;

  Gtk::Box* position_row_ = nullptr;  // managed by Gtk; hidden entirely for live streams (duration == 0)
  Gtk::Label elapsed_label_;
  Gtk::Scale position_scale_;
  Gtk::Button duration_button_;
  Gtk::Label duration_label_;
  // Toggled by clicking duration_label_ (via duration_button_): false shows
  // the track's total duration ("3:45"), true shows time remaining
  // ("-1:15"). Re-rendered from the cached values below on every toggle,
  // not just the next UpdatePosition() tick.
  bool show_remaining_ = false;
  unsigned last_position_seconds_ = 0;
  unsigned last_duration_seconds_ = 0;
  // True from the first user-driven value_changed() until the debounced
  // seek fires, so UpdatePosition()'s 1s-timer updates don't fight the
  // slider out from under the user's pointer mid-drag (a real risk here,
  // unlike volume_scale_, which is only ever updated from event-driven —
  // not polled — refreshes).
  bool user_seeking_ = false;
  // Guards position_scale_'s signal_value_changed() against firing for our
  // own programmatic set_value()/set_range() calls (UpdatePosition(),
  // Update()) — the same role suppress_volume_signal_ plays for
  // volume_scale_ below.
  bool suppress_position_signal_ = false;
  sigc::connection seek_debounce_connection_;

  bool suppress_volume_signal_ = false;
  bool muted_ = false;

  sigc::signal<void()> signal_play_pause_;
  sigc::signal<void()> signal_next_;
  sigc::signal<void()> signal_previous_;
  sigc::signal<void(double)> signal_volume_changed_;
  sigc::signal<void(bool)> signal_mute_toggled_;
  sigc::signal<void()> signal_shuffle_clicked_;
  sigc::signal<void()> signal_repeat_clicked_;
  sigc::signal<void()> signal_add_to_favorites_clicked_;
  sigc::signal<void(unsigned)> signal_seek_requested_;
  sigc::signal<void()> signal_art_clicked_;
};

}  // namespace gnomos
