// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <giomm/dbusconnection.h>
#include <giomm/dbusmethodinvocation.h>
#include <glibmm/variant.h>
#include <sigc++/connection.h>

#include "backend/noson-backend.h"

namespace gnomos
{

// Exposes a small custom interface (de.christophlangner.Gnomos.Zone) on its
// own session-bus name, purely so a companion GNOME Shell extension can add
// the currently selected Sonos zone's volume as its own slider inside
// GNOME's system volume Quick Settings panel. MPRIS's own read-write
// Volume property (MprisService) can't be reused for this: GNOME Shell's
// Quick Settings sliders only ever come from real PulseAudio/PipeWire
// stream volumes, never from an MPRIS player's Volume — confirmed against
// GNOME Shell's own js/ui/mpris.js, which has no slider/volume handling at
// all, only transport controls and metadata.
//
// Deliberately a separate D-Bus service, not folded into MprisService:
// MPRIS's name/paths/interface are fixed by that spec and unrelated to this
// (a plain property bag plus a few setters, nothing about playback), and a
// consumer only interested in volume shouldn't need to understand MPRIS at
// all. Volume/Muted/Rooms are read-only *properties* (so a proxy's normal
// GetAll/PropertiesChanged bookkeeping keeps them in sync for free) but
// read-write via explicit SetVolume()/SetMuted()/SetRoomVolume() *methods*
// rather than Properties.Set — simpler for a GJS consumer to call directly,
// no need to hand-wrap a value in the right variant type for Properties.Set.
//
// Rooms exists so the extension's slider can also reach every *other* room
// in the household from an expandable submenu (mirroring the in-app
// grouping popover's own per-room sliders — see NosonBackend::GetRoomVolume()'s
// own comment for why that, not the current zone's own group members list,
// is the right source: it already covers every physically known room,
// regardless of current grouping), not just the currently selected zone the
// main slider already controls.
class ZoneVolumeService
{
public:
  explicit ZoneVolumeService(NosonBackend& backend);
  ~ZoneVolumeService();

  ZoneVolumeService(const ZoneVolumeService&) = delete;
  ZoneVolumeService& operator=(const ZoneVolumeService&) = delete;

private:
  void OnBusAcquired(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring& name);
  void OnStateChanged();

  void OnMethodCall(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring& sender,
                     const Glib::ustring& object_path, const Glib::ustring& interface_name,
                     const Glib::ustring& method_name, const Glib::VariantContainerBase& parameters,
                     const Glib::RefPtr<Gio::DBus::MethodInvocation>& invocation);
  void OnGetProperty(Glib::VariantBase& property, const Glib::RefPtr<Gio::DBus::Connection>& connection,
                      const Glib::ustring& sender, const Glib::ustring& object_path,
                      const Glib::ustring& interface_name, const Glib::ustring& property_name);

  // uuid/name/volume for every room GetRoomVolume() currently has a value
  // for (i.e. reached by at least one RefreshGroupVolumesAsync() call) —
  // a room never refreshed yet is omitted rather than shown with a stale
  // or fabricated 0%.
  Glib::VariantBase BuildRooms();
  void EmitPropertiesChanged();

  NosonBackend& backend_;

  guint own_name_id_ = 0;
  Glib::RefPtr<Gio::DBus::Connection> connection_;
  guint registration_id_ = 0;

  sigc::connection volume_connection_;
  sigc::connection player_ready_connection_;
  sigc::connection group_volumes_connection_;
  sigc::connection zones_connection_;
};

}  // namespace gnomos
