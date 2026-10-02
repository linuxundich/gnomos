// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <adwaita.h>
#include <gdkmm/texture.h>
#include <giomm/cancellable.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/overlay.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/widget.h>
#include <sigc++/sigc++.h>

#include "../backend/noson-types.h"
#include "cover-palette.h"
#include "lyrics-fetcher.h"

namespace gnomos
{

// Full-window background of the Now Playing sheet: the current cover,
// heavily blurred and scaled to fill, washed with two soft radial gradients
// in the cover's own colors, under a veil in the window color that keeps
// text readable. Crossfades to the next cover on a track change.
class CoverBackdrop : public Gtk::Widget
{
public:
  CoverBackdrop();
  ~CoverBackdrop() override;

  void SetCover(const Glib::RefPtr<Gdk::Texture>& texture, const CoverPalette& palette);
  void SetBlur(bool enabled);
  void SetTint(bool enabled);

protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

private:
  struct Layer
  {
    Glib::RefPtr<Gdk::Texture> texture;
    CoverPalette palette;
  };
  void DrawLayer(GtkSnapshot* snapshot, const Layer& layer, float width, float height);

  Layer current_;
  Layer previous_;
  double fade_ = 1.0;  // 0 = previous_ only, 1 = current_ only
  bool blur_ = true;
  bool tint_ = true;
  AdwAnimation* fade_animation_ = nullptr;
  gulong dark_handler_ = 0;
};

// The big cover in the sheet. Square with rounded corners, or — in
// "Plattenteller" mode — a record: round, with grooves and a center label,
// slowly turning while the music plays. Animates between the two shapes
// and crossfades on a track change.
class CoverArt : public Gtk::Widget
{
public:
  CoverArt();
  ~CoverArt() override;

  void SetTexture(const Glib::RefPtr<Gdk::Texture>& texture);
  void SetVinyl(bool vinyl);
  void SetPlaying(bool playing);

protected:
  Gtk::SizeRequestMode get_request_mode_vfunc() const override;
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

private:
  void UpdateSpinning();
  void DrawCover(GtkSnapshot* snapshot, const Glib::RefPtr<Gdk::Texture>& texture, const graphene_rect_t& rect);

  Glib::RefPtr<Gdk::Texture> texture_;
  Glib::RefPtr<Gdk::Texture> previous_texture_;
  double fade_ = 1.0;
  double vinyl_progress_ = 0.0;  // 0 = square cover, 1 = record
  bool vinyl_ = false;
  bool playing_ = false;
  double angle_ = 0.0;
  gint64 last_frame_time_ = 0;
  guint tick_id_ = 0;
  AdwAnimation* fade_animation_ = nullptr;
  AdwAnimation* shape_animation_ = nullptr;
};

// The Now Playing sheet (AdwBottomSheet's sheet, see GnomosWindow): a big
// cover with title, actions and transport controls on one side, synced or
// plain lyrics and the upcoming queue on the other. Narrow windows drop the
// second column. Like PlayerBar, purely a view: it renders what it's given
// and reports user actions through signals.
class NowPlayingView : public Gtk::Box
{
public:
  NowPlayingView();
  ~NowPlayingView() override;

  void Update(const NowPlaying& now_playing);
  void UpdatePosition(unsigned position_seconds, unsigned duration_seconds);
  void UpdateVolume(const VolumeInfo& volume);
  void SetRoomName(const std::string& name);
  // queue: the whole zone queue; current_index: the playing entry, or -1.
  void SetUpNext(const std::vector<QueueItem>& queue, int current_index);

  // Lyrics states. SetLyricsAvailable(false) hides the lyrics tab entirely
  // (the user hasn't opted into LRCLIB lookups).
  void SetLyricsAvailable(bool available);
  void SetLyricsLoading();
  void SetLyrics(const Lyrics& lyrics);

  void SetTintEnabled(bool enabled);
  void SetBlurEnabled(bool enabled);
  void SetVinylEnabled(bool enabled);

  const CoverPalette& palette() const { return palette_; }

  sigc::signal<void()>& signal_play_pause() { return signal_play_pause_; }
  sigc::signal<void()>& signal_next() { return signal_next_; }
  sigc::signal<void()>& signal_previous() { return signal_previous_; }
  sigc::signal<void()>& signal_shuffle_clicked() { return signal_shuffle_clicked_; }
  sigc::signal<void()>& signal_repeat_clicked() { return signal_repeat_clicked_; }
  sigc::signal<void(unsigned)>& signal_seek_requested() { return signal_seek_requested_; }
  sigc::signal<void(double)>& signal_volume_changed() { return signal_volume_changed_; }
  sigc::signal<void(bool)>& signal_mute_toggled() { return signal_mute_toggled_; }
  sigc::signal<void()>& signal_close_requested() { return signal_close_requested_; }
  sigc::signal<void(unsigned)>& signal_queue_item_activated() { return signal_queue_item_activated_; }
  // Library search / artist info for the current track — the actions the
  // old "Titel-Details" dialog had.
  sigc::signal<void(std::string)>& signal_search_artist() { return signal_search_artist_; }
  sigc::signal<void(std::string)>& signal_search_album() { return signal_search_album_; }
  sigc::signal<void(std::string)>& signal_artist_info() { return signal_artist_info_; }
  // A new cover's colors are known (or invalid: no cover).
  sigc::signal<void(const CoverPalette&)>& signal_palette_changed() { return signal_palette_changed_; }

protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;

private:
  void LoadArt(const std::string& uri);
  void OnArtDecoded(const Glib::RefPtr<Gdk::Texture>& texture, const Glib::RefPtr<Gdk::Texture>& small);
  void ShowFallbackArt();
  void RenderDuration();
  // Synced lyrics follow an estimated position: the backend reports the
  // position once a second, this interpolates in between.
  bool OnLyricsTick();
  void HighlightLyricLine(int index, bool animate);
  void ClearLyricLines();

  CoverBackdrop backdrop_;
  Gtk::Overlay overlay_;
  Gtk::Box body_;
  Gtk::Box info_column_;
  Gtk::Box side_column_;
  GtkWidget* side_stack_ = nullptr;
  GtkWidget* side_toggles_ = nullptr;
  Gtk::Label room_label_;

  CoverArt art_;
  Gtk::Label title_label_;
  Gtk::Label subtitle_label_;
  Gtk::Button search_artist_button_;
  Gtk::Button artist_info_button_;
  Gtk::Button search_album_button_;

  Gtk::Box seek_row_;
  Gtk::Scale position_scale_;
  Gtk::Label elapsed_label_;
  Gtk::Label duration_label_;
  Gtk::ToggleButton shuffle_button_;
  Gtk::Button previous_button_;
  Gtk::Button play_pause_button_;
  Gtk::Button next_button_;
  Gtk::ToggleButton repeat_button_;
  Gtk::Button mute_button_;
  Gtk::Scale volume_scale_;

  Gtk::ScrolledWindow lyrics_scroller_;
  Gtk::Box lyrics_box_;
  Gtk::Label lyrics_status_;
  std::vector<std::pair<unsigned, Gtk::Label*>> lyric_lines_;
  int current_lyric_ = -1;
  gint64 user_scrolled_at_ = 0;
  bool scrolling_programmatically_ = false;
  AdwAnimation* lyrics_scroll_animation_ = nullptr;
  sigc::connection lyrics_tick_;

  Gtk::ScrolledWindow up_next_scroller_;
  Gtk::ListBox up_next_list_;

  NowPlaying now_playing_;
  std::string art_uri_;
  unsigned art_generation_ = 0;
  Glib::RefPtr<Gio::Cancellable> art_cancellable_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  CoverPalette palette_;
  unsigned position_seconds_ = 0;
  gint64 position_time_ = 0;  // monotonic time position_seconds_ was reported
  bool user_seeking_ = false;
  bool suppress_position_signal_ = false;
  bool suppress_volume_signal_ = false;
  bool muted_ = false;
  bool lyrics_available_ = false;
  sigc::connection seek_debounce_;

  sigc::signal<void()> signal_play_pause_;
  sigc::signal<void()> signal_next_;
  sigc::signal<void()> signal_previous_;
  sigc::signal<void()> signal_shuffle_clicked_;
  sigc::signal<void()> signal_repeat_clicked_;
  sigc::signal<void(unsigned)> signal_seek_requested_;
  sigc::signal<void(double)> signal_volume_changed_;
  sigc::signal<void(bool)> signal_mute_toggled_;
  sigc::signal<void()> signal_close_requested_;
  sigc::signal<void(unsigned)> signal_queue_item_activated_;
  sigc::signal<void(std::string)> signal_search_artist_;
  sigc::signal<void(std::string)> signal_search_album_;
  sigc::signal<void(std::string)> signal_artist_info_;
  sigc::signal<void(const CoverPalette&)> signal_palette_changed_;
};

}  // namespace gnomos
