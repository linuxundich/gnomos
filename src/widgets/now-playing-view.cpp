// SPDX-License-Identifier: GPL-3.0-or-later

#include "now-playing-view.h"
#include "../i18n.h"

#include <algorithm>
#include <cmath>

#include <glibmm/bytes.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gdkmm/frameclock.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/separator.h>
#include <gtkmm/settings.h>
#include <gtkmm/snapshot.h>
#include <pangomm/layout.h>

#include "art-cache.h"
#include "art-decode-pool.h"
#include "cover-thumbnail.h"
#include "http-fetch.h"
#include "playback-ui.h"

namespace gnomos
{

namespace
{

bool AnimationsEnabled()
{
  auto settings = Gtk::Settings::get_default();
  return !settings || settings->property_gtk_enable_animations().get_value();
}

GdkRGBA ToGdk(const Gdk::RGBA& color, float alpha)
{
  return GdkRGBA{static_cast<float>(color.get_red()), static_cast<float>(color.get_green()),
                 static_cast<float>(color.get_blue()), alpha};
}

bool IsDark()
{
  return adw_style_manager_get_dark(adw_style_manager_get_default());
}

// AdwAnimation's value callback, forwarding to a std::function owned by the
// target (freed together with it).
extern "C" void OnNowPlayingAnimationValue(double value, gpointer user_data)
{
  (*static_cast<std::function<void(double)>*>(user_data))(value);
}
extern "C" void DeleteNowPlayingAnimationCallback(gpointer data)
{
  delete static_cast<std::function<void(double)>*>(data);
}

AdwAnimation* MakeAnimation(GtkWidget* widget, guint duration_ms, std::function<void(double)> on_value)
{
  AdwAnimationTarget* target = adw_callback_animation_target_new(
      OnNowPlayingAnimationValue, new std::function<void(double)>(std::move(on_value)),
      DeleteNowPlayingAnimationCallback);
  AdwAnimation* animation = adw_timed_animation_new(widget, 0.0, 1.0, duration_ms, target);
  adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(animation), ADW_EASE_OUT_CUBIC);
  return animation;
}

// Cover rect for `texture` filling width x height completely (center crop),
// scaled up a bit more so the blur's soft edges fall outside the widget.
graphene_rect_t CoverRect(const Glib::RefPtr<Gdk::Texture>& texture, float width, float height, float overscan)
{
  float tw = texture->get_width(), th = texture->get_height();
  float scale = std::max(width / tw, height / th) * overscan;
  float w = tw * scale, h = th * scale;
  return GRAPHENE_RECT_INIT((width - w) / 2, (height - h) / 2, w, h);
}

}  // namespace

// ---------------------------------------------------------------------------
// CoverBackdrop

CoverBackdrop::CoverBackdrop() : Glib::ObjectBase("GnomosCoverBackdrop"), Gtk::Widget()
{
  set_can_target(false);
  fade_animation_ = MakeAnimation(GTK_WIDGET(gobj()), 700, [this](double value) {
    fade_ = value;
    queue_draw();
  });
  // The veil's color follows light/dark mode.
  dark_handler_ = g_signal_connect_swapped(adw_style_manager_get_default(), "notify::dark",
                                           G_CALLBACK(gtk_widget_queue_draw), gobj());
}

CoverBackdrop::~CoverBackdrop()
{
  g_signal_handler_disconnect(adw_style_manager_get_default(), dark_handler_);
  g_object_unref(fade_animation_);
}

void CoverBackdrop::SetCover(const Glib::RefPtr<Gdk::Texture>& texture, const CoverPalette& palette)
{
  previous_ = current_;
  current_ = {texture, palette};
  fade_ = 0.0;
  adw_animation_reset(fade_animation_);
  adw_animation_play(fade_animation_);
}

void CoverBackdrop::SetBlur(bool enabled)
{
  blur_ = enabled;
  queue_draw();
}

void CoverBackdrop::SetTint(bool enabled)
{
  tint_ = enabled;
  queue_draw();
}

void CoverBackdrop::DrawLayer(GtkSnapshot* snapshot, const Layer& layer, float width, float height)
{
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  if (blur_ && layer.texture)
  {
    gtk_snapshot_push_blur(snapshot, std::max(width, height) / 12.0);
    graphene_rect_t rect = CoverRect(layer.texture, width, height, 1.25f);
    gtk_snapshot_append_texture(snapshot, layer.texture->gobj(), &rect);
    gtk_snapshot_pop(snapshot);
  }
  if (tint_ && layer.palette.valid)
  {
    float strength = blur_ ? 0.45f : 0.55f;
    GskColorStop stops_a[2] = {{0.0f, ToGdk(layer.palette.base, strength)},
                               {1.0f, ToGdk(layer.palette.base, 0.0f)}};
    graphene_point_t center_a = GRAPHENE_POINT_INIT(width * 0.15f, height * 0.1f);
    gtk_snapshot_append_radial_gradient(snapshot, &bounds, &center_a, width * 0.9f, height * 0.9f, 0.0f, 1.0f,
                                        stops_a, 2);
    GskColorStop stops_b[2] = {{0.0f, ToGdk(layer.palette.second, strength)},
                               {1.0f, ToGdk(layer.palette.second, 0.0f)}};
    graphene_point_t center_b = GRAPHENE_POINT_INIT(width * 0.95f, height * 1.0f);
    gtk_snapshot_append_radial_gradient(snapshot, &bounds, &center_b, width * 0.9f, height * 0.9f, 0.0f, 1.0f,
                                        stops_b, 2);
  }
}

void CoverBackdrop::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot_ref)
{
  GtkSnapshot* snapshot = snapshot_ref->gobj();
  float width = get_width(), height = get_height();
  if (width <= 0 || height <= 0)
    return;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);

  gtk_snapshot_push_clip(snapshot, &bounds);
  if (fade_ < 1.0 && (previous_.texture || previous_.palette.valid))
  {
    gtk_snapshot_push_opacity(snapshot, 1.0 - fade_);
    DrawLayer(snapshot, previous_, width, height);
    gtk_snapshot_pop(snapshot);
  }
  gtk_snapshot_push_opacity(snapshot, fade_);
  DrawLayer(snapshot, current_, width, height);
  gtk_snapshot_pop(snapshot);

  // The veil: window color over everything, strong enough that text in
  // the default colors stays readable on any cover.
  bool dark = IsDark();
  GdkRGBA veil = dark ? GdkRGBA{0.11f, 0.11f, 0.12f, blur_ ? 0.58f : 0.35f}
                      : GdkRGBA{0.98f, 0.98f, 0.99f, blur_ ? 0.62f : 0.40f};
  gtk_snapshot_append_color(snapshot, &veil, &bounds);
  gtk_snapshot_pop(snapshot);
}

// ---------------------------------------------------------------------------
// CoverArt

CoverArt::CoverArt() : Glib::ObjectBase("GnomosCoverArt"), Gtk::Widget()
{
  set_overflow(Gtk::Overflow::VISIBLE);
  fade_animation_ = MakeAnimation(GTK_WIDGET(gobj()), 450, [this](double value) {
    fade_ = value;
    queue_draw();
  });
  shape_animation_ = MakeAnimation(GTK_WIDGET(gobj()), 500, [this](double value) {
    vinyl_progress_ = vinyl_ ? value : 1.0 - value;
    queue_draw();
  });
}

CoverArt::~CoverArt()
{
  if (tick_id_)
    remove_tick_callback(tick_id_);
  g_object_unref(fade_animation_);
  g_object_unref(shape_animation_);
}

Gtk::SizeRequestMode CoverArt::get_request_mode_vfunc() const
{
  return Gtk::SizeRequestMode::CONSTANT_SIZE;
}

void CoverArt::measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural, int& minimum_baseline,
                             int& natural_baseline) const
{
  minimum = 160;
  natural = 340;
  minimum_baseline = natural_baseline = -1;
}

void CoverArt::SetTexture(const Glib::RefPtr<Gdk::Texture>& texture)
{
  if (texture == texture_)
    return;
  previous_texture_ = texture_;
  texture_ = texture;
  fade_ = previous_texture_ ? 0.0 : 1.0;
  if (previous_texture_)
  {
    adw_animation_reset(fade_animation_);
    adw_animation_play(fade_animation_);
  }
  queue_draw();
}

void CoverArt::SetVinyl(bool vinyl)
{
  if (vinyl == vinyl_)
    return;
  vinyl_ = vinyl;
  adw_animation_reset(shape_animation_);
  adw_animation_play(shape_animation_);
  UpdateSpinning();
}

void CoverArt::SetPlaying(bool playing)
{
  playing_ = playing;
  UpdateSpinning();
}

// Only a record turns, and only while something plays — 6 s per turn, calm
// enough to sit in the corner of the eye. Off entirely when the desktop
// asks for reduced motion (gtk-enable-animations).
void CoverArt::UpdateSpinning()
{
  bool spin = vinyl_ && playing_ && AnimationsEnabled();
  if (spin && !tick_id_)
  {
    last_frame_time_ = 0;
    tick_id_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>& clock) {
      gint64 now = clock->get_frame_time();
      if (last_frame_time_ != 0)
        angle_ = std::fmod(angle_ + (now - last_frame_time_) / 1e6 * 60.0, 360.0);
      last_frame_time_ = now;
      queue_draw();
      return true;
    });
  }
  else if (!spin && tick_id_)
  {
    remove_tick_callback(tick_id_);
    tick_id_ = 0;
  }
}

void CoverArt::DrawCover(GtkSnapshot* snapshot, const Glib::RefPtr<Gdk::Texture>& texture,
                         const graphene_rect_t& rect)
{
  if (!texture)
    return;
  graphene_rect_t cover = CoverRect(texture, rect.size.width, rect.size.height, 1.0f);
  cover.origin.x += rect.origin.x;
  cover.origin.y += rect.origin.y;
  gtk_snapshot_append_scaled_texture(snapshot, texture->gobj(), GSK_SCALING_FILTER_TRILINEAR, &cover);
}

void CoverArt::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot_ref)
{
  GtkSnapshot* snapshot = snapshot_ref->gobj();
  float width = get_width(), height = get_height();
  float side = std::min(width, height);
  if (side <= 0)
    return;
  graphene_rect_t rect = GRAPHENE_RECT_INIT((width - side) / 2, (height - side) / 2, side, side);

  float v = static_cast<float>(vinyl_progress_);
  // Just under half the side even as a full circle: corners that meet
  // exactly left a hairline seam in the shadow (GSK).
  float radius = 14.0f + (side / 2 - 14.5f) * v;
  GskRoundedRect rounded;
  gsk_rounded_rect_init_from_rect(&rounded, &rect, radius);

  GdkRGBA shadow = {0, 0, 0, IsDark() ? 0.55f : 0.30f};
  gtk_snapshot_append_outset_shadow(snapshot, &rounded, &shadow, 0, side * 0.04f, 0, side * 0.09f);

  gtk_snapshot_save(snapshot);
  if (v > 0)
  {
    graphene_point_t center = GRAPHENE_POINT_INIT(width / 2, height / 2);
    gtk_snapshot_translate(snapshot, &center);
    gtk_snapshot_rotate(snapshot, static_cast<float>(angle_));
    graphene_point_t back = GRAPHENE_POINT_INIT(-width / 2, -height / 2);
    gtk_snapshot_translate(snapshot, &back);
  }
  gtk_snapshot_push_rounded_clip(snapshot, &rounded);

  GdkRGBA base = IsDark() ? GdkRGBA{0.2f, 0.2f, 0.22f, 1} : GdkRGBA{0.85f, 0.85f, 0.87f, 1};
  gtk_snapshot_append_color(snapshot, &base, &rect);
  if (fade_ < 1.0 && previous_texture_)
  {
    gtk_snapshot_push_opacity(snapshot, 1.0 - fade_);
    DrawCover(snapshot, previous_texture_, rect);
    gtk_snapshot_pop(snapshot);
  }
  gtk_snapshot_push_opacity(snapshot, fade_);
  DrawCover(snapshot, texture_, rect);
  gtk_snapshot_pop(snapshot);

  if (v > 0)
  {
    // Grooves, a sheen and the center label of a record, faded in with the
    // shape.
    cairo_t* cr = gtk_snapshot_append_cairo(snapshot, &rect);
    double cx = rect.origin.x + side / 2.0, cy = rect.origin.y + side / 2.0;
    cairo_set_line_width(cr, 1.0);
    for (double r = side * 0.18; r < side * 0.49; r += side * 0.012)
    {
      cairo_set_source_rgba(cr, 0, 0, 0, 0.16 * v);
      cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
      cairo_stroke(cr);
    }
    cairo_pattern_t* sheen = cairo_pattern_create_linear(rect.origin.x, rect.origin.y, rect.origin.x + side,
                                                         rect.origin.y + side);
    cairo_pattern_add_color_stop_rgba(sheen, 0.30, 1, 1, 1, 0);
    cairo_pattern_add_color_stop_rgba(sheen, 0.48, 1, 1, 1, 0.14 * v);
    cairo_pattern_add_color_stop_rgba(sheen, 0.60, 1, 1, 1, 0);
    cairo_set_source(cr, sheen);
    cairo_arc(cr, cx, cy, side / 2.0, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.08, v);
    cairo_arc(cr, cx, cy, side * 0.085, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.92, 0.92, 0.92, v);
    cairo_arc(cr, cx, cy, side * 0.018, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_destroy(cr);
  }

  gtk_snapshot_pop(snapshot);
  gtk_snapshot_restore(snapshot);
}

// ---------------------------------------------------------------------------
// NowPlayingView

namespace
{
Gtk::Button* MakeFlatIconButton(Gtk::Button& button, const char* icon, const char* label)
{
  button.set_icon_name(icon);
  button.add_css_class("flat");
  button.add_css_class("circular");
  button.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(button, label);
  return &button;
}
}  // namespace

NowPlayingView::NowPlayingView()
: Glib::ObjectBase("GnomosNowPlayingView"), Gtk::Box(Gtk::Orientation::VERTICAL, 0)
{
  add_css_class("now-playing");
  set_vexpand(true);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  // --- Header: which room this is, and a way back down ---
  auto* header = Gtk::make_managed<Gtk::CenterBox>();
  // Clear of AdwBottomSheet's drag handle, which sits on top of the sheet.
  header->set_margin_top(20);
  header->set_margin_start(12);
  header->set_margin_end(12);
  room_label_.add_css_class("heading");
  room_label_.add_css_class("dimmed");
  room_label_.set_ellipsize(Pango::EllipsizeMode::END);
  header->set_center_widget(room_label_);
  auto* close_button = Gtk::make_managed<Gtk::Button>();
  MakeFlatIconButton(*close_button, "go-down-symbolic", _("Close Now Playing"));
  close_button->signal_clicked().connect([this] { signal_close_requested_.emit(); });
  header->set_end_widget(*close_button);
  content->append(*header);

  // --- Body: two columns side by side, or one when narrow (see the
  // breakpoint at the end of this constructor) ---
  body_.set_orientation(Gtk::Orientation::HORIZONTAL);
  body_.set_spacing(40);
  body_.set_homogeneous(true);
  body_.set_vexpand(true);
  body_.set_margin_top(12);
  body_.set_margin_bottom(24);
  body_.set_margin_start(32);
  body_.set_margin_end(32);
  content->append(body_);

  // Info column: cover, title, actions, transport.
  info_column_.set_orientation(Gtk::Orientation::VERTICAL);
  info_column_.set_spacing(10);
  info_column_.set_valign(Gtk::Align::CENTER);
  info_column_.add_css_class("now-playing-info");

  art_.set_halign(Gtk::Align::FILL);
  art_.set_vexpand(true);
  art_.set_margin_bottom(14);
  info_column_.append(art_);

  title_label_.add_css_class("title-1");
  title_label_.add_css_class("now-playing-title");
  title_label_.set_wrap(true);
  title_label_.set_lines(2);
  title_label_.set_ellipsize(Pango::EllipsizeMode::END);
  title_label_.set_justify(Gtk::Justification::CENTER);
  title_label_.set_selectable(true);
  title_label_.set_can_focus(false);
  info_column_.append(title_label_);

  subtitle_label_.add_css_class("dimmed");
  subtitle_label_.set_wrap(true);
  subtitle_label_.set_justify(Gtk::Justification::CENTER);
  subtitle_label_.set_selectable(true);
  subtitle_label_.set_can_focus(false);
  info_column_.append(subtitle_label_);

  // The old "Titel-Details" dialog's actions, now right under the title.
  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  actions->set_halign(Gtk::Align::CENTER);
  auto* copy_button = Gtk::make_managed<Gtk::Button>();
  MakeFlatIconButton(*copy_button, "edit-copy-symbolic", _("Copy Track Info"));
  copy_button->signal_clicked().connect([this] {
    std::string text = now_playing_.title;
    if (!now_playing_.artist.empty())
      text += " — " + now_playing_.artist;
    if (!now_playing_.album.empty())
      text += " — " + now_playing_.album;
    get_clipboard()->set_text(text);
  });
  actions->append(*copy_button);
  MakeFlatIconButton(search_artist_button_, "system-search-symbolic", _("Search Artist in Library"));
  search_artist_button_.signal_clicked().connect([this] { signal_search_artist_.emit(now_playing_.artist); });
  actions->append(search_artist_button_);
  MakeFlatIconButton(artist_info_button_, "avatar-default-symbolic", _("About the Artist"));
  artist_info_button_.signal_clicked().connect([this] { signal_artist_info_.emit(now_playing_.artist); });
  actions->append(artist_info_button_);
  MakeFlatIconButton(search_album_button_, "media-optical-cd-audio-symbolic", _("Search Album in Library"));
  search_album_button_.signal_clicked().connect([this] { signal_search_album_.emit(now_playing_.album); });
  actions->append(search_album_button_);
  info_column_.append(*actions);

  // Seek bar.
  seek_row_.set_orientation(Gtk::Orientation::HORIZONTAL);
  seek_row_.set_spacing(10);
  seek_row_.set_margin_top(6);
  elapsed_label_.add_css_class("caption");
  elapsed_label_.add_css_class("dimmed");
  elapsed_label_.add_css_class("numeric");
  seek_row_.append(elapsed_label_);
  position_scale_.set_range(0, 1);
  position_scale_.set_draw_value(false);
  position_scale_.set_hexpand(true);
  position_scale_.add_css_class("position-scale");
  gtk_accessible_update_property(GTK_ACCESSIBLE(position_scale_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Playback position"), -1);
  // Same debounce as PlayerBar's own position scale — a drag's many
  // intermediate values coalesce into one seek.
  position_scale_.signal_value_changed().connect([this] {
    if (suppress_position_signal_)
      return;
    user_seeking_ = true;
    seek_debounce_.disconnect();
    seek_debounce_ = Glib::signal_timeout().connect(
        [this] {
          user_seeking_ = false;
          signal_seek_requested_.emit(static_cast<unsigned>(position_scale_.get_value()));
          return false;
        },
        400);
  });
  seek_row_.append(position_scale_);
  duration_label_.add_css_class("caption");
  duration_label_.add_css_class("dimmed");
  duration_label_.add_css_class("numeric");
  seek_row_.append(duration_label_);
  info_column_.append(seek_row_);

  // Transport.
  auto* transport = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
  transport->set_halign(Gtk::Align::CENTER);
  shuffle_button_.set_icon_name("media-playlist-shuffle-symbolic");
  shuffle_button_.add_css_class("flat");
  shuffle_button_.add_css_class("circular");
  shuffle_button_.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(shuffle_button_, _("Shuffle"));
  shuffle_button_.signal_clicked().connect([this] { signal_shuffle_clicked_.emit(); });
  transport->append(shuffle_button_);
  MakeFlatIconButton(previous_button_, "media-skip-backward-symbolic", _("Previous Track"));
  previous_button_.add_css_class("large-transport");
  previous_button_.signal_clicked().connect([this] { signal_previous_.emit(); });
  transport->append(previous_button_);
  play_pause_button_.set_icon_name(PlayPauseIconForState(TransportState::Stopped));
  play_pause_button_.add_css_class("circular");
  play_pause_button_.add_css_class("suggested-action");
  play_pause_button_.add_css_class("play-button");
  play_pause_button_.add_css_class("large-play-button");
  play_pause_button_.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(play_pause_button_, PlayPauseLabelForState(TransportState::Stopped));
  play_pause_button_.signal_clicked().connect([this] { signal_play_pause_.emit(); });
  transport->append(play_pause_button_);
  MakeFlatIconButton(next_button_, "media-skip-forward-symbolic", _("Next Track"));
  next_button_.add_css_class("large-transport");
  next_button_.signal_clicked().connect([this] { signal_next_.emit(); });
  transport->append(next_button_);
  repeat_button_.set_icon_name("media-playlist-repeat-symbolic");
  repeat_button_.add_css_class("flat");
  repeat_button_.add_css_class("circular");
  repeat_button_.set_valign(Gtk::Align::CENTER);
  SetButtonLabel(repeat_button_, _("Repeat"));
  repeat_button_.signal_clicked().connect([this] { signal_repeat_clicked_.emit(); });
  transport->append(repeat_button_);
  info_column_.append(*transport);

  // Volume.
  auto* volume_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  volume_row->set_halign(Gtk::Align::CENTER);
  MakeFlatIconButton(mute_button_, "audio-volume-high-symbolic", _("Mute"));
  mute_button_.signal_clicked().connect([this] {
    muted_ = !muted_;
    signal_mute_toggled_.emit(muted_);
  });
  volume_row->append(mute_button_);
  volume_scale_.set_range(0, 100);
  volume_scale_.set_increments(1, 2);
  volume_scale_.set_draw_value(false);
  volume_scale_.set_size_request(200, -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(volume_scale_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Volume"),
                                 -1);
  volume_scale_.signal_value_changed().connect([this] {
    if (!suppress_volume_signal_)
      signal_volume_changed_.emit(volume_scale_.get_value());
  });
  volume_row->append(volume_scale_);
  info_column_.append(*volume_row);

  body_.append(info_column_);

  // Side column: lyrics / up next, switched with an inline toggle group.
  side_column_.set_orientation(Gtk::Orientation::VERTICAL);
  side_column_.set_spacing(12);
  side_toggles_ = adw_toggle_group_new();
  gtk_widget_add_css_class(side_toggles_, "round");
  gtk_widget_set_halign(side_toggles_, GTK_ALIGN_CENTER);
  AdwToggle* lyrics_toggle = adw_toggle_new();
  adw_toggle_set_name(lyrics_toggle, "lyrics");
  adw_toggle_set_label(lyrics_toggle, _("Lyrics"));
  adw_toggle_group_add(ADW_TOGGLE_GROUP(side_toggles_), lyrics_toggle);
  AdwToggle* next_toggle = adw_toggle_new();
  adw_toggle_set_name(next_toggle, "up-next");
  adw_toggle_set_label(next_toggle, _("Up Next"));
  adw_toggle_group_add(ADW_TOGGLE_GROUP(side_toggles_), next_toggle);
  side_column_.append(*Glib::wrap(side_toggles_));

  side_stack_ = adw_view_stack_new();
  adw_view_stack_set_enable_transitions(ADW_VIEW_STACK(side_stack_), TRUE);
  gtk_widget_set_vexpand(side_stack_, TRUE);
  g_object_bind_property(side_toggles_, "active-name", side_stack_, "visible-child-name",
                         G_BINDING_SYNC_CREATE);

  // Lyrics page.
  lyrics_box_.set_orientation(Gtk::Orientation::VERTICAL);
  lyrics_box_.set_spacing(2);
  lyrics_box_.add_css_class("lyrics");
  lyrics_status_.add_css_class("dimmed");
  lyrics_status_.set_wrap(true);
  lyrics_status_.set_margin_top(48);
  lyrics_box_.append(lyrics_status_);
  lyrics_scroller_.set_child(lyrics_box_);
  lyrics_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  lyrics_scroller_.add_css_class("lyrics-scroller");
  // A scroll the user made themselves pauses the automatic follow for a
  // few seconds, so reading ahead isn't yanked back.
  lyrics_scroller_.get_vadjustment()->signal_value_changed().connect([this] {
    if (!scrolling_programmatically_)
      user_scrolled_at_ = g_get_monotonic_time();
  });
  lyrics_scroll_animation_ = MakeAnimation(GTK_WIDGET(lyrics_scroller_.gobj()), 450, [](double) {});
  adw_view_stack_add_named(ADW_VIEW_STACK(side_stack_), GTK_WIDGET(lyrics_scroller_.gobj()), "lyrics");

  // Up next page.
  up_next_list_.set_selection_mode(Gtk::SelectionMode::NONE);
  up_next_list_.add_css_class("navigation-sidebar");
  up_next_list_.add_css_class("up-next");
  up_next_list_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
    if (row)
      signal_queue_item_activated_.emit(
          static_cast<unsigned>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row->gobj()), "queue-index"))));
  });
  auto* up_next_placeholder = Gtk::make_managed<Gtk::Label>(_("Nothing more in the queue"));
  up_next_placeholder->add_css_class("dimmed");
  up_next_placeholder->set_margin_top(48);
  up_next_list_.set_placeholder(*up_next_placeholder);
  up_next_scroller_.set_child(up_next_list_);
  up_next_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  adw_view_stack_add_named(ADW_VIEW_STACK(side_stack_), GTK_WIDGET(up_next_scroller_.gobj()), "up-next");

  side_column_.append(*Glib::wrap(side_stack_));
  body_.append(side_column_);
  SetLyricsAvailable(false);

  // --- Backdrop under everything ---
  overlay_.set_child(backdrop_);
  overlay_.add_overlay(*content);
  overlay_.set_measure_overlay(*content, true);
  overlay_.set_vexpand(true);

  // Narrow: one column, no side column.
  GtkWidget* breakpoint_bin = adw_breakpoint_bin_new();
  gtk_widget_set_size_request(breakpoint_bin, 300, 360);
  adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(breakpoint_bin), GTK_WIDGET(overlay_.gobj()));
  AdwBreakpoint* narrow =
      adw_breakpoint_new(adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 760, ADW_LENGTH_UNIT_PX));
  GValue hidden = G_VALUE_INIT;
  g_value_init(&hidden, G_TYPE_BOOLEAN);
  g_value_set_boolean(&hidden, FALSE);
  adw_breakpoint_add_setter(narrow, G_OBJECT(side_column_.gobj()), "visible", &hidden);
  g_value_unset(&hidden);
  GValue margin = G_VALUE_INIT;
  g_value_init(&margin, G_TYPE_INT);
  g_value_set_int(&margin, 16);
  adw_breakpoint_add_setter(narrow, G_OBJECT(body_.gobj()), "margin-start", &margin);
  adw_breakpoint_add_setter(narrow, G_OBJECT(body_.gobj()), "margin-end", &margin);
  g_value_unset(&margin);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(breakpoint_bin), narrow);
  Gtk::Widget* wrapped_bin = Glib::wrap(breakpoint_bin);
  wrapped_bin->set_vexpand(true);
  append(*wrapped_bin);

  // Lyrics follow the music at 4 Hz — fine-grained enough that a line
  // lights up when it's sung, cheap since it only compares timestamps.
  lyrics_tick_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &NowPlayingView::OnLyricsTick), 250);

  ShowFallbackArt();
}

NowPlayingView::~NowPlayingView()
{
  *alive_ = false;
  lyrics_tick_.disconnect();
  seek_debounce_.disconnect();
  if (art_cancellable_)
    art_cancellable_->cancel();
  g_object_unref(lyrics_scroll_animation_);
}

// AdwBottomSheet gives its sheet its natural height (up to the window's).
// Asking for a lot makes the sheet cover nearly the whole window, like a
// full Now Playing screen, instead of hugging its content.
void NowPlayingView::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                                   int& minimum_baseline, int& natural_baseline) const
{
  Gtk::Box::measure_vfunc(orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
  if (orientation == Gtk::Orientation::VERTICAL)
    natural = std::max(natural, 4000);
}

void NowPlayingView::Update(const NowPlaying& now_playing)
{
  bool track_changed = now_playing.title != now_playing_.title || now_playing.artist != now_playing_.artist;
  bool was_playing = now_playing_.state == TransportState::Playing;
  now_playing_ = now_playing;
  // Pause freezes the clock where it is, resuming runs it on from there.
  bool playing_now = now_playing.valid && now_playing.state == TransportState::Playing;
  if (playing_now != was_playing && !track_changed)
  {
    clock_base_ = ClockNow();
    clock_time_ = g_get_monotonic_time();
    clock_rate_ = 1.0;
    clock_running_ = playing_now;
  }
  if (!now_playing.valid)
  {
    title_label_.set_text(_("Nothing Playing"));
    subtitle_label_.set_text("");
    art_.SetPlaying(false);
    return;
  }

  title_label_.set_text(now_playing.title.empty() ? _("Unknown Track") : now_playing.title);
  std::string subtitle = now_playing.artist;
  if (!now_playing.album.empty())
    subtitle += (subtitle.empty() ? "" : " · ") + now_playing.album;
  subtitle_label_.set_text(subtitle);
  subtitle_label_.set_visible(!subtitle.empty());
  search_artist_button_.set_visible(!now_playing.artist.empty());
  artist_info_button_.set_visible(!now_playing.artist.empty());
  search_album_button_.set_visible(!now_playing.album.empty());

  bool playing = now_playing.state == TransportState::Playing;
  play_pause_button_.set_icon_name(PlayPauseIconForState(now_playing.state));
  SetButtonLabel(play_pause_button_, PlayPauseLabelForState(now_playing.state));
  art_.SetPlaying(playing);

  shuffle_button_.set_active(now_playing.shuffle);
  shuffle_button_.set_sensitive(now_playing.shuffle_supported);
  repeat_button_.set_active(now_playing.repeat != RepeatMode::Off);
  repeat_button_.set_icon_name(now_playing.repeat == RepeatMode::One ? "media-playlist-repeat-song-symbolic"
                                                                      : "media-playlist-repeat-symbolic");
  repeat_button_.set_sensitive(now_playing.repeat_supported);
  next_button_.set_sensitive(now_playing.can_go_next);
  previous_button_.set_sensitive(now_playing.can_go_previous);

  seek_row_.set_visible(now_playing.duration > 0);
  if (now_playing.duration > 0 && !user_seeking_)
  {
    suppress_position_signal_ = true;
    position_scale_.set_range(0, now_playing.duration);
    suppress_position_signal_ = false;
    RenderDuration();
  }
  if (track_changed)
  {
    ResetClock(0);
    clock_running_ = playing;
  }

  LoadArt(now_playing.art_uri);
}

void NowPlayingView::UpdatePosition(unsigned position_seconds, unsigned duration_seconds)
{
  // Sonos reports the position in whole seconds ("RelTime"), polled once a
  // second, so a report is up to a second behind the real position (the
  // cut-off fraction) plus the poll's own round trip. Following each
  // report literally made the clock — and the highlighted lyric line —
  // fall back and catch up again every few seconds. Instead the clock
  // keeps running and only steers: a report ahead of it speeds it up a
  // little, one behind slows it down, so it never runs backwards. Only a
  // real jump (seek, skip within the track) of more than two seconds
  // resets it outright.
  double reported = position_seconds + 0.5;
  bool playing = now_playing_.valid && now_playing_.state == TransportState::Playing;
  if (!playing)
  {
    ResetClock(position_seconds);
    clock_running_ = false;
  }
  else if (position_seconds == last_reported_position_ && clock_running_)
  {
    // Measured live: every other report repeats the previous value (the
    // device's RelTime only advanced every two polls), so it carries no
    // news — steering by it pulled the clock back and forth.
  }
  else
  {
    double estimate = ClockNow();
    double drift = reported - estimate;
    if (!clock_running_ || std::fabs(drift) > 2.0)
    {
      ResetClock(reported);
    }
    else
    {
      clock_base_ = estimate;
      clock_time_ = g_get_monotonic_time();
      clock_rate_ = std::clamp(1.0 + drift * 0.5, 0.8, 1.25);
    }
    clock_running_ = true;
  }
  last_reported_position_ = position_seconds;
  if (duration_seconds == 0 || user_seeking_)
    return;
  elapsed_label_.set_text(FormatTime(position_seconds));
  suppress_position_signal_ = true;
  position_scale_.set_value(position_seconds);
  suppress_position_signal_ = false;
  RenderDuration();
}

void NowPlayingView::RenderDuration()
{
  duration_label_.set_text(FormatTime(now_playing_.duration));
}

void NowPlayingView::UpdateVolume(const VolumeInfo& volume)
{
  suppress_volume_signal_ = true;
  volume_scale_.set_value(volume.volume);
  suppress_volume_signal_ = false;
  muted_ = volume.muted;
  mute_button_.set_icon_name(IconForVolume(volume.volume, volume.muted));
  SetButtonLabel(mute_button_, volume.muted ? _("Unmute") : _("Mute"));
}

void NowPlayingView::SetRoomName(const std::string& name)
{
  room_label_.set_text(name.empty() ? "" : Format(_("Playing in %s"), name.c_str()));
}

void NowPlayingView::SetUpNext(const std::vector<QueueItem>& queue, int current_index)
{
  while (Gtk::Widget* child = up_next_list_.get_first_child())
    up_next_list_.remove(*child);
  // A long queue only needs its next stretch here; the full queue has its
  // own page.
  constexpr size_t kMaxRows = 40;
  size_t start = current_index >= 0 ? static_cast<size_t>(current_index) + 1 : 0;
  for (size_t i = start; i < queue.size() && i < start + kMaxRows; ++i)
  {
    const QueueItem& item = queue[i];
    auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    row_box->set_margin_top(4);
    row_box->set_margin_bottom(4);
    auto* thumbnail = Gtk::make_managed<CoverThumbnail>();
    thumbnail->SetGeneratedFallback(!item.album.empty() ? item.album : item.title);
    thumbnail->SetArtUri(item.art_uri);
    row_box->append(*thumbnail);
    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_valign(Gtk::Align::CENTER);
    labels->set_hexpand(true);
    auto* title = Gtk::make_managed<Gtk::Label>(item.title.empty() ? _("Unknown Track") : item.title);
    title->set_halign(Gtk::Align::START);
    title->set_ellipsize(Pango::EllipsizeMode::END);
    labels->append(*title);
    if (!item.artist.empty())
    {
      auto* artist = Gtk::make_managed<Gtk::Label>(item.artist);
      artist->set_halign(Gtk::Align::START);
      artist->set_ellipsize(Pango::EllipsizeMode::END);
      artist->add_css_class("dimmed");
      artist->add_css_class("caption");
      labels->append(*artist);
    }
    row_box->append(*labels);
    auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
    g_object_set_data(G_OBJECT(row->gobj()), "queue-index", GUINT_TO_POINTER(item.index));
    row->set_child(*row_box);
    up_next_list_.append(*row);
  }
}

void NowPlayingView::SetLyricsAvailable(bool available)
{
  lyrics_available_ = available;
  // Without lyrics there's only one page left — no need for a switcher.
  gtk_widget_set_visible(side_toggles_, available);
  adw_toggle_group_set_active_name(ADW_TOGGLE_GROUP(side_toggles_), available ? "lyrics" : "up-next");
  if (!available)
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(side_stack_), "up-next");
}

void NowPlayingView::ClearLyricLines()
{
  for (auto& [ms, label] : lyric_lines_)
    lyrics_box_.remove(*label);
  lyric_lines_.clear();
  current_lyric_ = -1;
}

void NowPlayingView::SetLyricsLoading()
{
  ClearLyricLines();
  lyrics_status_.set_text(_("Loading lyrics…"));
  lyrics_status_.set_visible(true);
}

void NowPlayingView::SetLyrics(const Lyrics& lyrics)
{
  ClearLyricLines();
  if (lyrics.empty())
  {
    lyrics_status_.set_text(_("No lyrics found."));
    lyrics_status_.set_visible(true);
    return;
  }
  lyrics_status_.set_visible(false);

  if (lyrics.synced.empty())
  {
    // Plain text: one label, read at your own pace.
    auto* label = Gtk::make_managed<Gtk::Label>(lyrics.plain);
    label->set_wrap(true);
    label->set_xalign(0);
    label->set_selectable(true);
    label->set_can_focus(false);
    label->add_css_class("lyrics-plain");
    lyrics_box_.append(*label);
    lyric_lines_.emplace_back(0, label);
    // Not a timed line — keeps OnLyricsTick() from highlighting it.
    current_lyric_ = -2;
    return;
  }

  for (const auto& [ms, text] : lyrics.synced)
  {
    auto* label = Gtk::make_managed<Gtk::Label>(text.empty() ? "♪" : text);
    label->set_wrap(true);
    label->set_xalign(0);
    label->add_css_class("lyrics-line");
    // Clicking a line jumps there.
    auto click = Gtk::GestureClick::create();
    unsigned seconds = ms / 1000;
    click->signal_released().connect([this, seconds](int, double, double) { signal_seek_requested_.emit(seconds); });
    label->add_controller(click);
    lyrics_box_.append(*label);
    lyric_lines_.emplace_back(ms, label);
  }
  user_scrolled_at_ = 0;
  lyrics_clock_reset_ = true;
  lyrics_scroller_.get_vadjustment()->set_value(0);
}

bool NowPlayingView::OnLyricsTick()
{
  if (current_lyric_ == -2 || lyric_lines_.empty() || !now_playing_.valid)
    return true;
  double position = ClockNow();
  // A clock that went back by more than two seconds was reset by a seek —
  // only then may the highlight move up again.
  bool jumped_back = lyrics_clock_reset_ || position < lyrics_shown_at_ - 2.0;
  lyrics_clock_reset_ = false;
  if (jumped_back || position > lyrics_shown_at_)
    lyrics_shown_at_ = position;
  gint64 position_ms = static_cast<gint64>(lyrics_shown_at_ * 1000);
  int index = -1;
  for (size_t i = 0; i < lyric_lines_.size(); ++i)
  {
    if (static_cast<gint64>(lyric_lines_[i].first) <= position_ms + 200)
      index = static_cast<int>(i);
    else
      break;
  }
  if (index != current_lyric_ && (index > current_lyric_ || jumped_back))
    HighlightLyricLine(index, true);
  return true;
}

double NowPlayingView::ClockNow() const
{
  if (!clock_running_ || clock_time_ == 0)
    return clock_base_;
  return clock_base_ + (g_get_monotonic_time() - clock_time_) / 1e6 * clock_rate_;
}

void NowPlayingView::ResetClock(double seconds)
{
  clock_base_ = seconds;
  clock_time_ = g_get_monotonic_time();
  clock_rate_ = 1.0;
  lyrics_clock_reset_ = true;
}

void NowPlayingView::HighlightLyricLine(int index, bool animate)
{
  for (size_t i = 0; i < lyric_lines_.size(); ++i)
  {
    Gtk::Label* label = lyric_lines_[i].second;
    label->remove_css_class("current");
    label->remove_css_class("past");
    if (static_cast<int>(i) == index)
      label->add_css_class("current");
    else if (static_cast<int>(i) < index)
      label->add_css_class("past");
  }
  current_lyric_ = index;
  if (index < 0 || !get_mapped())
    return;
  // Respect a recent manual scroll — see the vadjustment handler.
  if (user_scrolled_at_ != 0 && g_get_monotonic_time() - user_scrolled_at_ < 4 * G_USEC_PER_SEC)
    return;

  Gtk::Label* label = lyric_lines_[index].second;
  auto bounds = label->compute_bounds(lyrics_box_);
  if (!bounds)
    return;
  auto adjustment = lyrics_scroller_.get_vadjustment();
  double target = bounds->get_y() + bounds->get_height() / 2 - lyrics_scroller_.get_height() * 0.4;
  target = std::clamp(target, adjustment->get_lower(), adjustment->get_upper() - adjustment->get_page_size());
  double from = adjustment->get_value();
  if (!animate || !AnimationsEnabled())
  {
    scrolling_programmatically_ = true;
    adjustment->set_value(target);
    scrolling_programmatically_ = false;
    return;
  }
  g_object_unref(lyrics_scroll_animation_);
  lyrics_scroll_animation_ = MakeAnimation(GTK_WIDGET(lyrics_scroller_.gobj()), 500, [this, from, target](double v) {
    scrolling_programmatically_ = true;
    lyrics_scroller_.get_vadjustment()->set_value(from + (target - from) * v);
    scrolling_programmatically_ = false;
  });
  adw_animation_play(lyrics_scroll_animation_);
}

void NowPlayingView::SetTintEnabled(bool enabled)
{
  backdrop_.SetTint(enabled);
}

void NowPlayingView::SetBlurEnabled(bool enabled)
{
  backdrop_.SetBlur(enabled);
}

void NowPlayingView::SetVinylEnabled(bool enabled)
{
  art_.SetVinyl(enabled);
}

void NowPlayingView::ShowFallbackArt()
{
  std::string seed = !now_playing_.album.empty() ? now_playing_.album : now_playing_.title;
  auto texture = GeneratedCoverTexture(seed.empty() ? "Gnomos" : seed, 480);
  OnArtDecoded(texture, GeneratedCoverTexture(seed.empty() ? "Gnomos" : seed, 48));
}

void NowPlayingView::LoadArt(const std::string& uri)
{
  if (uri == art_uri_ && (uri.empty() || palette_.valid))
    return;
  art_uri_ = uri;
  unsigned generation = ++art_generation_;
  if (art_cancellable_)
    art_cancellable_->cancel();

  if (uri.empty())
  {
    ShowFallbackArt();
    return;
  }

  auto alive = alive_;
  // Decoding (twice: once large for the cover, once tiny for the blur and
  // the palette) runs on ArtDecodePool, like CoverThumbnail's.
  auto decode = [this, generation, alive](Glib::RefPtr<Glib::Bytes> bytes) {
    ArtDecodePool::Instance().Push([this, bytes, generation, alive] {
      auto large = ArtCache::DecodeScaledTexture(bytes, 720);
      auto small = ArtCache::DecodeScaledTexture(bytes, 64);
      Glib::signal_idle().connect_once([this, large, small, generation, alive] {
        if (!*alive || generation != art_generation_)
          return;
        if (large)
          OnArtDecoded(large, small ? small : large);
        else
          ShowFallbackArt();
      });
    });
  };

  if (auto raw = ArtCache::Instance().GetRawBytes(uri))
  {
    decode(raw);
    return;
  }
  art_cancellable_ = Gio::Cancellable::create();
  HttpFetch(
      uri,
      [this, uri, generation, alive, decode](std::string body) {
        if (!*alive || generation != art_generation_)
          return;
        if (body.empty())
        {
          ShowFallbackArt();
          return;
        }
        auto bytes = Glib::Bytes::create(body.data(), body.size());
        ArtCache::Instance().Put(uri, bytes);
        decode(bytes);
      },
      art_cancellable_);
}

void NowPlayingView::OnArtDecoded(const Glib::RefPtr<Gdk::Texture>& texture, const Glib::RefPtr<Gdk::Texture>& small)
{
  palette_ = ExtractCoverPalette(small);
  cover_texture_ = texture;
  art_.SetTexture(texture);
  backdrop_.SetCover(small, palette_);
  signal_palette_changed_.emit(palette_);
  signal_cover_changed_.emit(texture);
}

}  // namespace gnomos
