// SPDX-License-Identifier: GPL-3.0-or-later
//
// GnomosWindow, continued: Persisted settings (state.ini), the preferences dialog, About and the shortcut list.
// The class itself and its constructor live in gnomos-window.cpp.

#include "gnomos-window.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/texture.h>
#include <giomm/application.h>
#include <giomm/asyncresult.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/notification.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/bytes.h>
#include <glibmm/checksum.h>
#include <glibmm/error.h>
#include <glibmm/keyfile.h>
#include <glibmm/uriutils.h>
#include <json-glib/json-glib.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/editable.h>
#include <gtkmm/entry.h>
#include <gtkmm/expander.h>
#include <gtkmm/expression.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/gridview.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/linkbutton.h>
#include <gtkmm/listview.h>
#include <gtkmm/range.h>
#include <gtkmm/revealer.h>
#include <gtkmm/scale.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/stringobject.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include "config.h"
#include "window-helpers.h"
#include "widgets/art-cache.h"
#include "widgets/artist-info-fetcher.h"
#include "widgets/cover-thumbnail.h"
#include "widgets/dialog-shell.h"
#include "widgets/http-fetch.h"
#include "widgets/lastfm-scrobbler.h"
#include "widgets/listenbrainz-scrobbler.h"
#include "widgets/lyrics-fetcher.h"
#include "widgets/radio-browser-service.h"

namespace gnomos
{

void GnomosWindow::SaveLastRoom(const std::string& uuid) const
{
  if (uuid.empty())
    return;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);

  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("window", "last_room_uuid", uuid);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the room won't be remembered next launch
  }
}

std::string GnomosWindow::LoadLastRoomUuid() const
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return "";
    return keyfile->get_string("window", "last_room_uuid").raw();
  }
  catch (const Glib::Error&)
  {
    return "";
  }
}

void GnomosWindow::LoadWindowState()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    int width = keyfile->get_integer("window", "width");
    int height = keyfile->get_integer("window", "height");
    if (width > 0 && height > 0)
      set_default_size(width, height);
    if (keyfile->get_boolean("window", "maximized"))
      maximize();
  }
  catch (const Glib::Error&)
  {
    // fine — no saved size yet, the fixed default from the constructor applies
  }
}

void GnomosWindow::LoadSplitFractions()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    double sidebar_fraction = keyfile->get_double("window", "sidebar_fraction");
    if (sidebar_fraction > 0.0 && sidebar_fraction < 1.0)
      adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(split_view_), sidebar_fraction);
  }
  catch (const Glib::Error&)
  {
    // fine — no saved fraction yet, min/max_sidebar_width's own defaults apply
  }
}

void GnomosWindow::LoadColorScheme()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    ApplyColorScheme(keyfile->get_string("appearance", "color_scheme").raw());
  }
  catch (const Glib::Error&)
  {
    // fine — no override saved yet, AdwStyleManager's own default applies
  }
}

void GnomosWindow::ApplyColorScheme(const std::string& scheme)
{
  AdwStyleManager* style_manager = adw_style_manager_get_default();
  if (scheme == "light")
    adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
  else if (scheme == "dark")
    adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
  else
    adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_DEFAULT);

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("appearance", "color_scheme", scheme);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the override won't be remembered next launch
  }
}

void GnomosWindow::LoadLibraryViewPreference()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    prefer_grid_view_ = keyfile->get_boolean("library", "prefer_grid");
  }
  catch (const Glib::Error&)
  {
    // fine — no preference saved yet, stays at the true default
  }
}

void GnomosWindow::SetPreferGridView(bool prefer_grid)
{
  prefer_grid_view_ = prefer_grid;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("library", "prefer_grid", prefer_grid);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the preference won't be remembered next launch
  }
}

void GnomosWindow::LoadArtistImagesSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    load_artist_images_ = keyfile->get_boolean("library", "load_artist_images");
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays off (the default)
  }
}

void GnomosWindow::SetLoadArtistImages(bool enabled)
{
  load_artist_images_ = enabled;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("library", "load_artist_images", enabled);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
  // Takes effect immediately if "Interpreten" (or any other level with
  // artist-typed entries) happens to be the currently displayed one —
  // harmless no-op re-render otherwise, same as toggling prefer_grid_view_
  // already does unconditionally.
  OnLibraryChanged();
}

void GnomosWindow::LoadLyricsSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    load_lyrics_ = keyfile->get_boolean("player", "load_lyrics");
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays off (the default)
  }
}

void GnomosWindow::SetLoadLyrics(bool enabled)
{
  load_lyrics_ = enabled;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("player", "load_lyrics", enabled);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
}

void GnomosWindow::LoadAppearanceSettings()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (keyfile->load_from_file(StateFilePath()))
    {
      auto read = [&](const char* key, bool& target) {
        if (keyfile->has_key("appearance", key))
          target = keyfile->get_boolean("appearance", key);
      };
      read("cover_tint", cover_tint_);
      read("cover_blur", cover_blur_);
      read("vinyl_mode", vinyl_mode_);
    }
  }
  catch (const Glib::Error&)
  {
    // fine — nothing saved yet, the defaults stay
  }
  now_playing_view_.SetTintEnabled(cover_tint_);
  now_playing_view_.SetBlurEnabled(cover_blur_);
  now_playing_view_.SetVinylEnabled(vinyl_mode_);
}

void GnomosWindow::SaveAppearanceSetting(const char* key, bool value)
{
  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("appearance", key, value);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
}

void GnomosWindow::LoadListenBrainzToken()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    listenbrainz_token_ = keyfile->get_string("scrobbling", "listenbrainz_token");
  }
  catch (const Glib::Error&)
  {
    // fine — no token saved yet, scrobbling stays off (the default)
  }
}

void GnomosWindow::SetListenBrainzToken(const std::string& token)
{
  listenbrainz_token_ = token;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("scrobbling", "listenbrainz_token", token);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the token won't be remembered next launch
  }
}

void GnomosWindow::LoadLastFmSettings()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    // Each read is independent — a Glib::Error partway through (e.g. the
    // session/username keys not existing yet because auth was never
    // completed) only aborts the *remaining* reads, leaving whatever
    // already succeeded (api_key/shared_secret here) in place, same
    // graceful-partial-load behavior every other Load*() in this file
    // relies on.
    lastfm_api_key_ = keyfile->get_string("scrobbling", "lastfm_api_key");
    lastfm_shared_secret_ = keyfile->get_string("scrobbling", "lastfm_shared_secret");
    lastfm_session_key_ = keyfile->get_string("scrobbling", "lastfm_session_key");
    lastfm_username_ = keyfile->get_string("scrobbling", "lastfm_username");
  }
  catch (const Glib::Error&)
  {
    // fine — nothing saved yet, Last.fm scrobbling stays off (the default)
  }
}

void GnomosWindow::SetLastFmApiCredentials(const std::string& api_key, const std::string& shared_secret)
{
  lastfm_api_key_ = api_key;
  lastfm_shared_secret_ = shared_secret;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("scrobbling", "lastfm_api_key", api_key);
  keyfile->set_string("scrobbling", "lastfm_shared_secret", shared_secret);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means this won't be remembered next launch
  }
}

void GnomosWindow::SetLastFmSession(const std::string& session_key, const std::string& username)
{
  lastfm_session_key_ = session_key;
  lastfm_username_ = username;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_string("scrobbling", "lastfm_session_key", session_key);
  keyfile->set_string("scrobbling", "lastfm_username", username);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means this won't be remembered next launch
  }
  // Covers both directions through this one method — connecting (the auth
  // flow's own "Fertig" callback lands here) and disconnecting
  // (DisconnectLastFm() below calls straight through) — reported live as
  // confusing when the Settings row stayed stuck on the old state until
  // manually closed and reopened.
  RefreshOpenSettingsDialog();
}

void GnomosWindow::DisconnectLastFm()
{
  SetLastFmSession("", "");
}

void GnomosWindow::StartLastFmAuth()
{
  if (lastfm_api_key_.empty() || lastfm_shared_secret_.empty())
  {
    ShowToast(_("Enter the API key and shared secret first"));
    return;
  }
  std::string api_key = lastfm_api_key_;
  std::string shared_secret = lastfm_shared_secret_;
  LastFmScrobbler::Instance().RequestAuthToken(api_key, shared_secret, [this](std::string token) {
    if (token.empty())
    {
      ShowToast(_("Last.fm sign-in failed — check the API key and shared secret"));
      return;
    }
    ShowLastFmAuthDialog(token);
  });
}

void GnomosWindow::ShowLastFmAuthDialog(const std::string& token)
{
  auto* dialog = new DialogShell(*this);
  dialog->set_title(_("Last.fm Sign-In"));
  dialog->set_default_size(420, -1);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin_top(18);
  content->set_margin_bottom(18);
  content->set_margin_start(18);
  content->set_margin_end(18);

  auto* instructions = Gtk::make_managed<Gtk::Label>(
      _("Open the following link in a browser, sign in to Last.fm and allow access. Then come back here and click “Done”."));
  instructions->set_wrap(true);
  instructions->set_halign(Gtk::Align::START);
  content->append(*instructions);

  std::string auth_url =
      "https://www.last.fm/api/auth/?api_key=" + Glib::uri_escape_string(lastfm_api_key_) +
      "&token=" + Glib::uri_escape_string(token);
  auto* link_button = Gtk::make_managed<Gtk::LinkButton>(auth_url, auth_url);
  link_button->set_halign(Gtk::Align::START);
  content->append(*link_button);

  auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  button_box->set_halign(Gtk::Align::END);
  button_box->set_margin_top(6);
  auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
  cancel_button->signal_clicked().connect([dialog] { dialog->close(); });
  auto* done_button = Gtk::make_managed<Gtk::Button>(_("Done"));
  done_button->add_css_class("suggested-action");
  done_button->signal_clicked().connect([this, dialog, token] {
    std::string api_key = lastfm_api_key_;
    std::string shared_secret = lastfm_shared_secret_;
    LastFmScrobbler::Instance().RequestSession(
        api_key, shared_secret, token, [this](std::string session_key, std::string username) {
          if (session_key.empty())
          {
            ShowToast(_("Sign-in not finished — authorize in the browser and try again"));
            return;
          }
          SetLastFmSession(session_key, username);
          ShowToast(username.empty() ? std::string(_("Connected to Last.fm"))
                                     : Format(_("Connected to Last.fm as “%s”"), username.c_str()));
        });
    dialog->close();
  });
  button_box->append(*cancel_button);
  button_box->append(*done_button);
  content->append(*button_box);

  dialog->set_child(*content);
  dialog->present();
}

void GnomosWindow::LoadFallbackIconScaleSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    double scale = keyfile->get_double("library", "fallback_icon_scale");
    if (scale > 0.0 && scale <= 1.0)
      fallback_icon_scale_ = scale;
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays at the full-size default
  }
}

void GnomosWindow::SetFallbackIconScale(double scale)
{
  fallback_icon_scale_ = scale;
  CoverThumbnail::SetFallbackIconScale(scale);

  // Debounced: an AdwSpinRow fires notify::value many times a second while
  // being dragged/scrolled — unlike a switch row (SetLoadArtistImages()'s
  // own OnLibraryChanged() call), which only ever fires once per click.
  // Confirmed live: letting each intermediate value trigger its own full
  // save-to-disk + OnLibraryChanged() rebuild (potentially dozens/hundreds
  // of grid tiles, e.g. a real Albums listing) back-to-back crashed the
  // app. Same reasoning as NosonBackend::SetVolume()'s own debouncing —
  // only the settled value, a short delay after the last change, actually
  // persists and re-renders.
  fallback_icon_scale_debounce_connection_.disconnect();
  fallback_icon_scale_debounce_connection_ = Glib::signal_timeout().connect(
      [this, scale] {
        const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
        g_mkdir_with_parents(dir.c_str(), 0700);
        auto keyfile = Glib::KeyFile::create();
        try
        {
          keyfile->load_from_file(StateFilePath());
        }
        catch (const Glib::Error&)
        {
          // fine — first launch, nothing to preserve
        }
        keyfile->set_double("library", "fallback_icon_scale", scale);
        try
        {
          keyfile->save_to_file(StateFilePath());
        }
        catch (const Glib::Error&)
        {
          // non-fatal — just means the setting won't be remembered next launch
        }
        OnLibraryChanged();
        return false;  // one-shot
      },
      200);
}

void GnomosWindow::LoadNotificationSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    notify_on_track_change_ = keyfile->get_boolean("notifications", "track_change");
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays off (the default)
  }
}

void GnomosWindow::SetNotifyOnTrackChange(bool enabled)
{
  notify_on_track_change_ = enabled;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("notifications", "track_change", enabled);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
}

void GnomosWindow::LoadRunInBackgroundSetting()
{
  auto keyfile = Glib::KeyFile::create();
  try
  {
    if (!keyfile->load_from_file(StateFilePath()))
      return;
    run_in_background_ = keyfile->get_boolean("general", "run_in_background");
  }
  catch (const Glib::Error&)
  {
    // fine — no setting saved yet, stays on (the default)
  }
}

void GnomosWindow::SetRunInBackground(bool enabled)
{
  run_in_background_ = enabled;

  const std::string dir = Glib::build_filename(Glib::get_user_config_dir(), "gnomos");
  g_mkdir_with_parents(dir.c_str(), 0700);
  auto keyfile = Glib::KeyFile::create();
  try
  {
    keyfile->load_from_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // fine — first launch, nothing to preserve
  }
  keyfile->set_boolean("general", "run_in_background", enabled);
  try
  {
    keyfile->save_to_file(StateFilePath());
  }
  catch (const Glib::Error&)
  {
    // non-fatal — just means the setting won't be remembered next launch
  }
}

void GnomosWindow::ShowAboutDialog()
{
  AdwDialog* dialog = adw_about_dialog_new();
  AdwAboutDialog* about = ADW_ABOUT_DIALOG(dialog);

  adw_about_dialog_set_application_name(about, "Gnomos");
  adw_about_dialog_set_application_icon(about, APPLICATION_ID);
  adw_about_dialog_set_version(about, PACKAGE_VERSION);
  adw_about_dialog_set_developer_name(about, "Christoph Langner");
  adw_about_dialog_set_comments(
      about, _("A GTK4/libadwaita client for Sonos speakers, with a special focus on first-generation devices (ZP80, ZP90, ZP100, ZP120, CR100) that Sonos' own current apps no longer support."));
  adw_about_dialog_set_copyright(about, "© 2026 Christoph Langner");
  adw_about_dialog_set_website(about, "https://github.com/linuxundich/gnomos");
  adw_about_dialog_set_issue_url(about, "https://github.com/linuxundich/gnomos/issues");

  // Gnomos links libnoson (GPL-3.0-or-later) statically, so the combined
  // work is bound to those terms — see LICENSE and README.md.
  adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_legal_section(about, "libnoson", "© 2014-2024 Jean-Luc Barriere", GTK_LICENSE_GPL_3_0, nullptr);

  // "Name https://url" is AdwAboutDialog's documented format for a
  // clickable link in a credits/acknowledgement section (there's no link
  // field on add_legal_section itself).
  const char* libraries[] = {"libnoson (Jean-Luc Barriere) https://github.com/janbar/noson", nullptr};
  adw_about_dialog_add_acknowledgement_section(about, _("Libraries"), libraries);

  // Radio-Browser (see RadioBrowserService's own header) — the public
  // directory the "Radiosender hinzufügen" dialog's search is built on.
  const char* services[] = {"Radio Browser https://www.radio-browser.info", nullptr};
  adw_about_dialog_add_acknowledgement_section(about, _("Services"), services);

  const char* developers[] = {"Christoph Langner", nullptr};
  adw_about_dialog_set_developers(about, developers);

  // Mainly useful for troubleshooting a multi-household setup — shown in
  // AdwAboutDialog's own built-in "Debug-Informationen" section (a
  // copyable text block), no bespoke row needed for it.
  std::string household_id = backend_->GetHouseholdID();
  adw_about_dialog_set_debug_info(
      about, ("Gnomos " + std::string(PACKAGE_VERSION) + "\nHousehold ID: " +
              (household_id.empty() ? std::string("—") : household_id))
                 .c_str());

  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::ShowShortcutsDialog()
{
  AdwDialog* dialog = adw_shortcuts_dialog_new();

  AdwShortcutsSection* section = adw_shortcuts_section_new(_("Play"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Play/Pause"), "space"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Next Track"), "n"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Previous Track"), "p"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Volume Up"), "<Control>Up"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Volume Down"), "<Control>Down"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Mute"), "m"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Shuffle"), "s"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Repeat"), "r"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Forward 10 Seconds"), "<Alt>Right"));
  adw_shortcuts_section_add(section, adw_shortcuts_item_new(_("Back 10 Seconds"), "<Alt>Left"));
  adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), section);

  AdwShortcutsSection* navigation = adw_shortcuts_section_new(_("View"));
  adw_shortcuts_section_add(navigation, adw_shortcuts_item_new(_("Show/Hide Now Playing"), "<Control>i"));
  adw_shortcuts_section_add(navigation, adw_shortcuts_item_new(_("Jump to Current Track"), "<Control>j"));
  adw_shortcuts_section_add(navigation, adw_shortcuts_item_new(_("Search Library"), "<Control>f"));
  adw_shortcuts_section_add(navigation, adw_shortcuts_item_new(_("Show/Hide Sidebar"), "F9"));
  adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), navigation);

  AdwShortcutsSection* general = adw_shortcuts_section_new(_("General"));
  adw_shortcuts_section_add(general, adw_shortcuts_item_new(_("Preferences"), "<Control>comma"));
  adw_shortcuts_section_add(general, adw_shortcuts_item_new(_("Keyboard Shortcuts"), "<Control>question"));
  adw_shortcuts_section_add(general, adw_shortcuts_item_new(_("Close Window"), "<Control>w"));
  adw_shortcuts_section_add(general, adw_shortcuts_item_new(_("Quit Gnomos"), "<Control>q"));
  adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), general);

  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::ShowSettingsDialog()
{
  AdwDialog* dialog = adw_preferences_dialog_new();
  adw_preferences_dialog_set_search_enabled(ADW_PREFERENCES_DIALOG(dialog), false);
  open_settings_dialog_ = dialog;
  g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog* closed_dialog, gpointer user_data) {
                     auto* self = static_cast<GnomosWindow*>(user_data);
                     if (self->open_settings_dialog_ == closed_dialog)
                       self->open_settings_dialog_ = nullptr;
                   }),
                   this);

  // Three pages rather than one long one — AdwPreferencesDialog already
  // renders more than one page as a proper tab/page switcher on its own
  // (a sidebar on a wide window, a bottom switcher once narrow), no extra
  // widgetry needed; this had grown to six groups stacked on a single
  // page, confirmed live as "the settings window is too big now, it's
  // all just one long column".
  GtkWidget* general_page = adw_preferences_page_new();
  adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(general_page), _("General"));
  adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(general_page), "applications-system-symbolic");

  GtkWidget* library_page = adw_preferences_page_new();
  adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(library_page), _("Library"));
  adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(library_page), "folder-music-symbolic");

  GtkWidget* radio_page = adw_preferences_page_new();
  adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(radio_page), _("Radio"));
  adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(radio_page), "network-wireless-symbolic");

  // --- Fensterverhalten ---
  GtkWidget* window_behavior_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(window_behavior_group), _("Window Behavior"));

  GtkWidget* background_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(background_row), _("Keep Running in the Background"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(background_row),
      _("Closing the window no longer quits Gnomos — media keys, lock screen controls (MPRIS) and Last.fm/ListenBrainz scrobbling keep running without an open window. “Quit Gnomos” in the menu quits it completely at any time."));
  adw_switch_row_set_active(ADW_SWITCH_ROW(background_row), run_in_background_);
  g_signal_connect_data(
      background_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { SetRunInBackground(active); }), DeleteBoolCallback,
      static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(window_behavior_group), background_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(general_page), ADW_PREFERENCES_GROUP(window_behavior_group));

  // --- Erscheinungsbild ---
  GtkWidget* appearance_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(appearance_group), _("Appearance"));

  // AdwToggleGroup, not AdwComboRow — a 3-way segmented control shows all
  // the choices at once, matching what GNOME Settings' own Appearance
  // panel moved to for this exact light/dark/auto choice.
  GtkWidget* scheme_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scheme_row), _("Color Scheme"));
  GtkWidget* scheme_toggle_group = adw_toggle_group_new();
  AdwToggle* system_toggle = adw_toggle_new();
  adw_toggle_set_label(system_toggle, _("System"));
  adw_toggle_group_add(ADW_TOGGLE_GROUP(scheme_toggle_group), system_toggle);
  AdwToggle* light_toggle = adw_toggle_new();
  adw_toggle_set_label(light_toggle, _("Light"));
  adw_toggle_group_add(ADW_TOGGLE_GROUP(scheme_toggle_group), light_toggle);
  AdwToggle* dark_toggle = adw_toggle_new();
  adw_toggle_set_label(dark_toggle, _("Dark"));
  adw_toggle_group_add(ADW_TOGGLE_GROUP(scheme_toggle_group), dark_toggle);
  // AdwStyleManager itself is the source of truth for the current scheme
  // (ApplyColorScheme() sets it directly), so read it back rather than
  // tracking a separate member here.
  AdwColorScheme current_scheme = adw_style_manager_get_color_scheme(adw_style_manager_get_default());
  adw_toggle_group_set_active(ADW_TOGGLE_GROUP(scheme_toggle_group),
                               current_scheme == ADW_COLOR_SCHEME_FORCE_LIGHT  ? 1
                               : current_scheme == ADW_COLOR_SCHEME_FORCE_DARK ? 2
                                                                                : 0);
  auto* scheme_callback = new std::function<void(guint)>([this](guint active) {
    switch (active)
    {
      case 1: ApplyColorScheme("light"); break;
      case 2: ApplyColorScheme("dark"); break;
      default: ApplyColorScheme("default"); break;
    }
  });
  g_signal_connect_data(scheme_toggle_group, "notify::active", G_CALLBACK(OnToggleGroupActiveChanged),
                         scheme_callback, DeleteGuintCallback, static_cast<GConnectFlags>(0));
  gtk_widget_set_valign(scheme_toggle_group, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(scheme_row), scheme_toggle_group);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(appearance_group), scheme_row);

  // Now Playing look — see ApplyCoverTint() and NowPlayingView.
  auto add_appearance_switch = [&](const char* title, const char* subtitle, bool active,
                                   std::function<void(bool)> on_change) {
    GtkWidget* row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    g_signal_connect_data(row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
                           new std::function<void(bool)>(std::move(on_change)), DeleteBoolCallback,
                           static_cast<GConnectFlags>(0));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(appearance_group), row);
  };
  add_appearance_switch(_("Colors from the Cover"), _("Tints the player bar and the Now Playing view to match the album"),
                        cover_tint_, [this](bool active) {
                          cover_tint_ = active;
                          SaveAppearanceSetting("cover_tint", active);
                          now_playing_view_.SetTintEnabled(active);
                          ApplyCoverTint(now_playing_view_.palette());
                        });
  add_appearance_switch(_("Blurred Background"), _("The cover as a soft background for the Now Playing view"),
                        cover_blur_, [this](bool active) {
                          cover_blur_ = active;
                          SaveAppearanceSetting("cover_blur", active);
                          now_playing_view_.SetBlurEnabled(active);
                        });
  add_appearance_switch(_("Record Player"), _("Shows the cover as a record that turns while music plays"),
                        vinyl_mode_, [this](bool active) {
                          vinyl_mode_ = active;
                          SaveAppearanceSetting("vinyl_mode", active);
                          now_playing_view_.SetVinylEnabled(active);
                        });
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(general_page), ADW_PREFERENCES_GROUP(appearance_group));

  // --- Benachrichtigungen ---
  GtkWidget* notifications_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(notifications_group), _("Notifications"));

  GtkWidget* notify_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(notify_row), _("Notify on Track Change"));
  adw_switch_row_set_active(ADW_SWITCH_ROW(notify_row), notify_on_track_change_);
  g_signal_connect_data(
      notify_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { SetNotifyOnTrackChange(active); }), DeleteBoolCallback,
      static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(notifications_group), notify_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(general_page), ADW_PREFERENCES_GROUP(notifications_group));

  // --- Songtexte ---
  GtkWidget* lyrics_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(lyrics_group), _("Lyrics"));

  GtkWidget* lyrics_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lyrics_row), _("Load Lyrics"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(lyrics_row),
      _("Looks up the current track's lyrics from the public LRCLIB API (lrclib.net) in the Now Playing view — a real request over the internet, not a local Sonos query. Title, artist and album are sent to LRCLIB. LRCLIB's lyrics are community contributions without any rights guarantee (see the link below)."));
  adw_switch_row_set_active(ADW_SWITCH_ROW(lyrics_row), load_lyrics_);
  g_signal_connect_data(
      lyrics_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) {
        SetLoadLyrics(active);
        lyrics_track_key_.clear();
        RequestLyricsForCurrentTrack();
      }),
      DeleteBoolCallback,
      static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(lyrics_group), lyrics_row);

  GtkWidget* lrclib_terms_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lrclib_terms_row), "LRCLIB");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(lrclib_terms_row), "lrclib.net");
  auto* lrclib_link_button = Gtk::make_managed<Gtk::LinkButton>("https://lrclib.net", _("Open"));
  lrclib_link_button->set_valign(Gtk::Align::CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(lrclib_terms_row), GTK_WIDGET(lrclib_link_button->gobj()));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(lyrics_group), lrclib_terms_row);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(general_page), ADW_PREFERENCES_GROUP(lyrics_group));

  // --- Scrobbling ---
  GtkWidget* scrobbling_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(scrobbling_group), _("Scrobbling"));
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(scrobbling_group),
      _("Once a track has been listened to, sends artist, title and album to every service enabled here — a real transfer over the internet, not a local Sonos query. Radio stations are never sent."));

  // AdwEntryRow (unlike AdwActionRow) has no subtitle property at all —
  // same "wrong widget type for a subtitle" issue already hit once before
  // in this dialog with an AdwButtonRow (see refresh_index_row's own
  // comment) — so the "leer = aus" explanation lives on
  // listenbrainz_terms_row's subtitle below instead, the one row here
  // that's actually an AdwActionRow.
  GtkWidget* listenbrainz_token_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(listenbrainz_token_row), _("ListenBrainz User Token"));
  gtk_editable_set_text(GTK_EDITABLE(listenbrainz_token_row), listenbrainz_token_.c_str());
  g_signal_connect_data(
      listenbrainz_token_row, "notify::text", G_CALLBACK(OnEntryRowTextChanged),
      new std::function<void(const std::string&)>([this](const std::string& text) { SetListenBrainzToken(text); }),
      DeleteStringCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(scrobbling_group), listenbrainz_token_row);

  GtkWidget* listenbrainz_terms_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(listenbrainz_terms_row), "ListenBrainz");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(listenbrainz_terms_row),
                               _("Empty = off. Get your token at listenbrainz.org/settings."));
  auto* listenbrainz_link_button = Gtk::make_managed<Gtk::LinkButton>("https://listenbrainz.org/settings", _("Open"));
  listenbrainz_link_button->set_valign(Gtk::Align::CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(listenbrainz_terms_row), GTK_WIDGET(listenbrainz_link_button->gobj()));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(scrobbling_group), listenbrainz_terms_row);

  // Last.fm needs a *registered API application* (an api_key + shared
  // secret pair from last.fm/api/account/create — something only the user
  // themselves can obtain, Gnomos has no way to self-register one) plus a
  // 3-step desktop-auth flow before it can scrobble anything, unlike
  // ListenBrainz's single pasted token — see LastFmScrobbler's own header
  // comment.
  GtkWidget* lastfm_api_key_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lastfm_api_key_row), _("Last.fm API Key"));
  gtk_editable_set_text(GTK_EDITABLE(lastfm_api_key_row), lastfm_api_key_.c_str());
  g_signal_connect_data(
      lastfm_api_key_row, "notify::text", G_CALLBACK(OnEntryRowTextChanged),
      new std::function<void(const std::string&)>(
          [this](const std::string& text) { SetLastFmApiCredentials(text, lastfm_shared_secret_); }),
      DeleteStringCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(scrobbling_group), lastfm_api_key_row);

  GtkWidget* lastfm_secret_row = adw_password_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lastfm_secret_row), _("Last.fm Shared Secret"));
  gtk_editable_set_text(GTK_EDITABLE(lastfm_secret_row), lastfm_shared_secret_.c_str());
  g_signal_connect_data(
      lastfm_secret_row, "notify::text", G_CALLBACK(OnEntryRowTextChanged),
      new std::function<void(const std::string&)>(
          [this](const std::string& text) { SetLastFmApiCredentials(lastfm_api_key_, text); }),
      DeleteStringCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(scrobbling_group), lastfm_secret_row);

  GtkWidget* lastfm_connect_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lastfm_connect_row), "Last.fm");
  std::string lastfm_status_subtitle =
      lastfm_session_key_.empty()
          ? _("Not connected — create an account at last.fm/api/account/create")
          : (lastfm_username_.empty() ? std::string(_("Connected"))
                                       : Format(_("Connected as “%s”"), lastfm_username_.c_str()));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(lastfm_connect_row), lastfm_status_subtitle.c_str());
  if (lastfm_session_key_.empty())
  {
    auto* connect_button = Gtk::make_managed<Gtk::Button>(_("Sign In"));
    connect_button->set_valign(Gtk::Align::CENTER);
    // Not live-updated within this same open dialog — the row above still
    // shows "Nicht verbunden" until Settings is reopened, same as
    // elsewhere in this dialog nothing here rebuilds itself in place after
    // an async action lands (RebuildGroupingPopover()'s own live rebuild
    // is the exception, not the rule).
    connect_button->signal_clicked().connect([this] { StartLastFmAuth(); });
    adw_action_row_add_suffix(ADW_ACTION_ROW(lastfm_connect_row), GTK_WIDGET(connect_button->gobj()));
  }
  else
  {
    auto* disconnect_button = Gtk::make_managed<Gtk::Button>(_("Disconnect"));
    disconnect_button->set_valign(Gtk::Align::CENTER);
    disconnect_button->add_css_class("destructive-action");
    disconnect_button->signal_clicked().connect([this] {
      DisconnectLastFm();
      ShowToast(_("Disconnected from Last.fm"));
    });
    adw_action_row_add_suffix(ADW_ACTION_ROW(lastfm_connect_row), GTK_WIDGET(disconnect_button->gobj()));
  }
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(scrobbling_group), lastfm_connect_row);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(general_page), ADW_PREFERENCES_GROUP(scrobbling_group));

  // --- Cover-Art-Cache ---
  GtkWidget* cache_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(cache_group), _("Cover Art Cache"));

  GtkWidget* size_row = adw_spin_row_new_with_range(10, 2000, 10);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(size_row), _("Maximum Size"));
  adw_spin_row_set_value(ADW_SPIN_ROW(size_row), ArtCache::Instance().GetMaxDiskMb());
  auto update_subtitle = [size_row] {
    double mb = static_cast<double>(ArtCache::Instance().GetDiskUsageBytes()) / (1024.0 * 1024.0);
    char buf[64];
    std::snprintf(buf, sizeof(buf), _("%.1f MB used"), mb);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(size_row), buf);
  };
  update_subtitle();
  g_signal_connect_data(size_row, "notify::value", G_CALLBACK(OnSpinRowValueChanged),
                         new std::function<void()>(update_subtitle), DeleteVoidCallback,
                         static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(cache_group), size_row);

  GtkWidget* clear_row = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(clear_row), _("Clear Cache Now"));
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(clear_row), "user-trash-symbolic");
  gtk_widget_add_css_class(clear_row, "destructive-action");
  auto* clear_callback = new std::function<void()>([update_subtitle] {
    ArtCache::Instance().Clear();
    update_subtitle();
  });
  g_signal_connect_data(clear_row, "activated", G_CALLBACK(OnButtonRowActivated), clear_callback,
                         DeleteVoidCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(cache_group), clear_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(library_page), ADW_PREFERENCES_GROUP(cache_group));

  // --- Bibliothek ---
  GtkWidget* library_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(library_group), _("Library"));
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(library_group),
      _("Everything else in Gnomos stays within your Sonos household on the local network — the feature below is the only exception."));

  GtkWidget* icon_scale_row = adw_spin_row_new_with_range(20, 100, 5);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(icon_scale_row), _("Icon Size"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(icon_scale_row),
      _("Size of the icon inside a tile when no cover image is available"));
  adw_spin_row_set_value(ADW_SPIN_ROW(icon_scale_row), fallback_icon_scale_ * 100.0);
  g_signal_connect_data(
      icon_scale_row, "notify::value", G_CALLBACK(OnDoubleSpinRowValueChanged),
      new std::function<void(double)>([this](double percent) { SetFallbackIconScale(percent / 100.0); }),
      DeleteDoubleCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), icon_scale_row);

  GtkWidget* artist_images_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(artist_images_row), _("Load Artist Images"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(artist_images_row),
      _("Looks up a photo from the public Deezer API (api.deezer.com) for artists without a cover image — a real request over the internet, not a local Sonos query. The artist's name is sent to Deezer. Deezer's own terms of use for this API apply (see the link below)."));
  adw_switch_row_set_active(ADW_SWITCH_ROW(artist_images_row), load_artist_images_);
  g_signal_connect_data(
      artist_images_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { SetLoadArtistImages(active); }), DeleteBoolCallback,
      static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), artist_images_row);

  GtkWidget* deezer_terms_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(deezer_terms_row), _("Deezer API and Terms of Use"));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(deezer_terms_row), "developers.deezer.com");
  auto* deezer_link_button = Gtk::make_managed<Gtk::LinkButton>("https://developers.deezer.com/api", _("Open"));
  deezer_link_button->set_valign(Gtk::Align::CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(deezer_terms_row), GTK_WIDGET(deezer_link_button->gobj()));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), deezer_terms_row);

  // AdwButtonRow (used for clear_row above) has no subtitle property at
  // all — G_DECLARE_FINAL_TYPE straight off AdwPreferencesRow, just a
  // title plus start/end icons — so the explanatory text below needs an
  // AdwActionRow instead, matching deezer_terms_row's own suffix-button
  // pattern just above. Confirmed live: the AdwButtonRow version compiled
  // fine but hit an invalid-cast GLib-GObject-CRITICAL at runtime the
  // moment this dialog opened, from adw_action_row_set_subtitle() being
  // called on a row that was never an AdwActionRow to begin with.
  GtkWidget* refresh_index_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(refresh_index_row), _("Rescan Library"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(refresh_index_row),
      _("Has Sonos rescan the connected local share — after adding new files, for example"));
  auto* refresh_index_button = Gtk::make_managed<Gtk::Button>();
  refresh_index_button->set_icon_name("view-refresh-symbolic");
  refresh_index_button->set_valign(Gtk::Align::CENTER);
  refresh_index_button->add_css_class("flat");
  refresh_index_button->signal_clicked().connect([this] {
    backend_->RefreshLibraryIndex();
    ShowToast(_("Library scan started"));
    StartLibraryIndexProgressPolling();
  });
  adw_action_row_add_suffix(ADW_ACTION_ROW(refresh_index_row), GTK_WIDGET(refresh_index_button->gobj()));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(library_group), refresh_index_row);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(library_page), ADW_PREFERENCES_GROUP(library_group));

  // --- Genres ---
  GtkWidget* genre_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(genre_group), _("Genres"));
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(genre_group),
      _("Each character here splits several genres combined in one genre tag in the library's genre view, e.g. “;” for “Rap; Metal; Hard-Core” — several characters are possible (e.g. “;/|”)."));

  GtkWidget* genre_separators_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(genre_separators_row), _("Separators"));
  gtk_editable_set_text(GTK_EDITABLE(genre_separators_row), backend_->GetGenreSeparators().c_str());
  g_signal_connect_data(
      genre_separators_row, "notify::text", G_CALLBACK(OnEntryRowTextChanged),
      new std::function<void(const std::string&)>(
          [this](const std::string& text) { backend_->SetGenreSeparators(text); }),
      DeleteStringCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(genre_group), genre_separators_row);

  GtkWidget* genre_first_only_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(genre_first_only_row), _("Use First Genre Only"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(genre_first_only_row),
      _("Shows only the genre before the first separator instead of splitting them all — useful when Sonos itself already cuts off a long combined genre tag, so later genres arrive incomplete (e.g. “Elec” instead of “Electronic”)"));
  adw_switch_row_set_active(ADW_SWITCH_ROW(genre_first_only_row), backend_->GetGenreUseFirstOnly());
  g_signal_connect_data(
      genre_first_only_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { backend_->SetGenreUseFirstOnly(active); }),
      DeleteBoolCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(genre_group), genre_first_only_row);

  adw_preferences_page_add(ADW_PREFERENCES_PAGE(library_page), ADW_PREFERENCES_GROUP(genre_group));

  // --- Radio ---
  GtkWidget* radio_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(radio_group), _("Radio"));
  adw_preferences_group_set_description(
      ADW_PREFERENCES_GROUP(radio_group),
      _("Applies in addition to a custom pattern, which you can set per station with its gear icon under “Radio Stations”."));

  GtkWidget* spam_filter_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spam_filter_row), _("Detect Ads Automatically"));
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(spam_filter_row),
      _("Treats content with more than two consecutive spaces as ads or filler — affects notifications (MPRIS) and the history alike."));
  adw_switch_row_set_active(ADW_SWITCH_ROW(spam_filter_row), backend_->GetRadioSpamWhitespaceFilterEnabled());
  g_signal_connect_data(
      spam_filter_row, "notify::active", G_CALLBACK(OnSwitchRowActiveChanged),
      new std::function<void(bool)>([this](bool active) { backend_->SetRadioSpamWhitespaceFilterEnabled(active); }),
      DeleteBoolCallback, static_cast<GConnectFlags>(0));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(radio_group), spam_filter_row);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(radio_page), ADW_PREFERENCES_GROUP(radio_group));

  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(general_page));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(library_page));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(radio_page));
  adw_dialog_present(dialog, GTK_WIDGET(gobj()));
}

void GnomosWindow::RefreshOpenSettingsDialog()
{
  if (!open_settings_dialog_)
    return;
  // Deferred to the next main-loop iteration rather than closed here
  // directly — this can be called from inside a button click handler
  // belonging to a widget *inside* the very dialog being closed (e.g. the
  // "Trennen" button), and destroying that dialog synchronously from
  // within its own signal handler risks touching already-freed widget
  // state once that handler's own caller resumes. One tick's delay is
  // imperceptible either way.
  Glib::signal_idle().connect_once([this] {
    if (!open_settings_dialog_)
      return;  // closed some other way in the meantime — nothing to do
    // force_close(), not close(): this should always actually close
    // (there's no unsaved-changes-style reason AdwPreferencesDialog would
    // ever refuse here), and its own "closed" handler already clears
    // open_settings_dialog_ before ShowSettingsDialog() below reassigns
    // it to the fresh instance.
    adw_dialog_force_close(open_settings_dialog_);
    ShowSettingsDialog();
  });
}

}  // namespace gnomos
