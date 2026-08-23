// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>

#include <giomm/dbusconnection.h>
#include <giomm/dbusmethodinvocation.h>
#include <glibmm/variant.h>
#include <gtkmm/applicationwindow.h>
#include <sigc++/connection.h>

#include "backend/noson-backend.h"

namespace gnomos
{

// Exposes org.mpris.MediaPlayer2[.Player] on the session bus, so GNOME's
// media keys, the Quick Settings media widget, lock-screen controls, and
// any other MPRIS client (e.g. playerctl) can see and control Gnomos —
// noson-app itself has the same integration (backend/NosonApp/dbus/mpris2.cpp),
// just via Qt's D-Bus bindings instead of GDBus/giomm.
//
// Scope, deliberately: the core Player + root interfaces only (Play/Pause/
// Next/Previous/Seek/Volume/Metadata/PlaybackStatus). NOT implemented:
// Shuffle/LoopStatus as *settable* MPRIS properties — mapping MPRIS's
// separate shuffle-bool + tri-state loop-status onto Sonos's single
// combined PlayMode_t enum would need a new backend entry point, and the
// read-only info is already visible in the main window regardless; the
// optional TrackList/Playlists interfaces, unneeded for a single-queue
// player.
class MprisService
{
public:
  MprisService(NosonBackend& backend, Gtk::ApplicationWindow& window);
  ~MprisService();

  MprisService(const MprisService&) = delete;
  MprisService& operator=(const MprisService&) = delete;

private:
  void OnBusAcquired(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring& name);
  void OnNowPlayingChanged();
  void OnVolumeChanged();

  void OnMethodCall(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring& sender,
                     const Glib::ustring& object_path, const Glib::ustring& interface_name,
                     const Glib::ustring& method_name, const Glib::VariantContainerBase& parameters,
                     const Glib::RefPtr<Gio::DBus::MethodInvocation>& invocation);
  void OnGetProperty(Glib::VariantBase& property, const Glib::RefPtr<Gio::DBus::Connection>& connection,
                      const Glib::ustring& sender, const Glib::ustring& object_path,
                      const Glib::ustring& interface_name, const Glib::ustring& property_name);
  bool OnSetProperty(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring& sender,
                      const Glib::ustring& object_path, const Glib::ustring& interface_name,
                      const Glib::ustring& property_name, const Glib::VariantBase& value);

  Glib::VariantBase BuildMetadata() const;
  void EmitPropertiesChanged(const Glib::ustring& interface_name,
                              const std::map<Glib::ustring, Glib::VariantBase>& changed);

  NosonBackend& backend_;
  Gtk::ApplicationWindow& window_;

  guint own_name_id_ = 0;
  Glib::RefPtr<Gio::DBus::Connection> connection_;
  guint root_registration_id_ = 0;
  guint player_registration_id_ = 0;

  sigc::connection now_playing_connection_;
  sigc::connection volume_connection_;
};

}  // namespace gnomos
