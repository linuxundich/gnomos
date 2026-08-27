// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpris-service.h"

#include <algorithm>
#include <cstdint>

#include <giomm/dbusintrospection.h>
#include <giomm/dbusownname.h>

#include "config.h"

namespace gnomos
{

namespace
{

// A single, fixed track id: Gnomos has no stable per-track identifier it
// could turn into a valid D-Bus object path (real track URIs contain
// characters the spec forbids there), and — since there's no MPRIS
// TrackList interface here — the spec allows a static id for "whatever is
// currently playing".
const char* const kTrackId = "/org/mpris/MediaPlayer2/gnomos/CurrentTrack";
const char* const kObjectPath = "/org/mpris/MediaPlayer2";
const char* const kRootInterface = "org.mpris.MediaPlayer2";
const char* const kPlayerInterface = "org.mpris.MediaPlayer2.Player";

// Rate is nominally read-write per the MPRIS spec, but Sonos has no notion
// of playback rate at all; advertising Minimum/MaximumRate == 1.0 (as done
// below) is the conventional way real-world players signal "not supported"
// without needing a working SetProperty for it.
const char* const kIntrospectionXml = R"XML(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek">
      <arg direction="in" name="Offset" type="x"/>
    </method>
    <method name="SetPosition">
      <arg direction="in" name="TrackId" type="o"/>
      <arg direction="in" name="Position" type="x"/>
    </method>
    <signal name="Seeked">
      <arg name="Position" type="x"/>
    </signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus" type="s" access="readwrite"/>
    <property name="Rate" type="d" access="read"/>
    <property name="Shuffle" type="b" access="readwrite"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
)XML";

const char* PlaybackStatusFor(const NowPlaying& np)
{
  if (!np.valid)
    return "Stopped";
  switch (np.state)
  {
    case TransportState::Playing:
      return "Playing";
    case TransportState::Paused:
      return "Paused";
    default:
      return "Stopped";
  }
}

// MPRIS's three LoopStatus values map directly onto RepeatMode — "Track"
// is the spec's name for repeat-one, "Playlist" for repeat-all.
const char* LoopStatusFor(RepeatMode mode)
{
  switch (mode)
  {
    case RepeatMode::One:
      return "Track";
    case RepeatMode::All:
      return "Playlist";
    default:
      return "None";
  }
}

}  // namespace

MprisService::MprisService(NosonBackend& backend, Gtk::ApplicationWindow& window)
: backend_(backend), window_(window), radio_filter_(backend)
{
  own_name_id_ = Gio::DBus::own_name(Gio::DBus::BusType::SESSION, "org.mpris.MediaPlayer2.gnomos",
                                      sigc::mem_fun(*this, &MprisService::OnBusAcquired));

  now_playing_connection_ = backend_.signal_now_playing_changed().connect(
      sigc::mem_fun(*this, &MprisService::OnNowPlayingChanged));
  volume_connection_ = backend_.signal_volume_changed().connect(sigc::mem_fun(*this, &MprisService::OnVolumeChanged));
}

MprisService::~MprisService()
{
  now_playing_connection_.disconnect();
  volume_connection_.disconnect();

  if (connection_)
  {
    if (root_registration_id_ != 0)
      connection_->unregister_object(root_registration_id_);
    if (player_registration_id_ != 0)
      connection_->unregister_object(player_registration_id_);
  }
  if (own_name_id_ != 0)
    Gio::DBus::unown_name(own_name_id_);
}

void MprisService::OnBusAcquired(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring&)
{
  connection_ = connection;

  Glib::RefPtr<Gio::DBus::NodeInfo> node_info;
  try
  {
    node_info = Gio::DBus::NodeInfo::create_for_xml(kIntrospectionXml);
  }
  catch (const Glib::Error&)
  {
    return;  // malformed XML would be a build-time bug, not a runtime condition to recover from
  }

  Glib::RefPtr<Gio::DBus::InterfaceInfo> root_info = node_info->lookup_interface(kRootInterface);
  Glib::RefPtr<Gio::DBus::InterfaceInfo> player_info = node_info->lookup_interface(kPlayerInterface);
  if (!root_info || !player_info)
    return;

  try
  {
    root_registration_id_ =
        connection_->register_object(kObjectPath, root_info, sigc::mem_fun(*this, &MprisService::OnMethodCall),
                                      sigc::mem_fun(*this, &MprisService::OnGetProperty));
    player_registration_id_ = connection_->register_object(
        kObjectPath, player_info, sigc::mem_fun(*this, &MprisService::OnMethodCall),
        sigc::mem_fun(*this, &MprisService::OnGetProperty), sigc::mem_fun(*this, &MprisService::OnSetProperty));
  }
  catch (const Glib::Error&)
  {
    // Another MPRIS-capable process already owns paths under this name —
    // shouldn't happen (the name itself is exclusively ours once acquired),
    // but isn't worth taking the app down over either.
  }
}

void MprisService::OnNowPlayingChanged()
{
  NowPlaying np = backend_.GetNowPlaying();
  std::map<Glib::ustring, Glib::VariantBase> changed;
  changed["PlaybackStatus"] = Glib::Variant<Glib::ustring>::create(PlaybackStatusFor(np));
  changed["LoopStatus"] = Glib::Variant<Glib::ustring>::create(LoopStatusFor(np.repeat));
  changed["Shuffle"] = Glib::Variant<bool>::create(np.shuffle);
  changed["Metadata"] = BuildMetadata();
  changed["CanGoNext"] = Glib::Variant<bool>::create(np.valid && np.can_go_next);
  changed["CanGoPrevious"] = Glib::Variant<bool>::create(np.valid && np.can_go_previous);
  changed["CanPlay"] = Glib::Variant<bool>::create(np.valid);
  changed["CanPause"] = Glib::Variant<bool>::create(np.valid && np.can_pause);
  changed["CanSeek"] = Glib::Variant<bool>::create(np.valid && np.duration > 0);
  changed["CanControl"] = Glib::Variant<bool>::create(np.valid);
  EmitPropertiesChanged(kPlayerInterface, changed);
}

void MprisService::OnVolumeChanged()
{
  VolumeInfo volume = backend_.GetVolume();
  std::map<Glib::ustring, Glib::VariantBase> changed;
  changed["Volume"] = Glib::Variant<double>::create(volume.volume / 100.0);
  EmitPropertiesChanged(kPlayerInterface, changed);
}

Glib::VariantBase MprisService::BuildMetadata()
{
  NowPlaying np = backend_.GetNowPlaying();

  // Radio stations rotate their reported "now playing" content between the
  // actual song and interstitial ad/ident text (confirmed live: e.g.
  // "song1 / artist1", "werbung1", "werbung2", "song1 / artist1", ...).
  // Republishing xesam:artist on every one of those rotations pops GNOME
  // Shell's media notification on every ad break, not just on a real song
  // change. Only radio-like sources (duration == 0) with a known stream
  // are affected — a queued track's artist is stable and passes through
  // unchanged below. See RadioContentFilter for the actual filtering.
  if (np.duration == 0 && !np.stream_uri.empty())
    np.artist = radio_filter_.Filter(np.stream_uri, np.artist);

  std::map<Glib::ustring, Glib::VariantBase> dict;
  dict["mpris:trackid"] = Glib::Variant<Glib::DBusObjectPathString>::create(kTrackId);
  if (np.duration > 0)
    dict["mpris:length"] = Glib::Variant<gint64>::create(static_cast<gint64>(np.duration) * 1000000);
  if (!np.title.empty())
    dict["xesam:title"] = Glib::Variant<Glib::ustring>::create(np.title);
  if (!np.artist.empty())
    dict["xesam:artist"] = Glib::Variant<std::vector<Glib::ustring>>::create({np.artist});
  if (!np.album.empty())
    dict["xesam:album"] = Glib::Variant<Glib::ustring>::create(np.album);
  if (!np.art_uri.empty())
    dict["mpris:artUrl"] = Glib::Variant<Glib::ustring>::create(np.art_uri);
  return Glib::Variant<std::map<Glib::ustring, Glib::VariantBase>>::create(dict);
}

void MprisService::EmitPropertiesChanged(const Glib::ustring& interface_name,
                                          const std::map<Glib::ustring, Glib::VariantBase>& changed)
{
  if (!connection_)
    return;
  std::vector<Glib::VariantBase> args = {
      Glib::Variant<Glib::ustring>::create(interface_name),
      Glib::Variant<std::map<Glib::ustring, Glib::VariantBase>>::create(changed),
      Glib::Variant<std::vector<Glib::ustring>>::create({}),
  };
  try
  {
    connection_->emit_signal(kObjectPath, "org.freedesktop.DBus.Properties", "PropertiesChanged", {},
                              Glib::VariantContainerBase::create_tuple(args));
  }
  catch (const Glib::Error&)
  {
    // Non-fatal — worst case, an MPRIS client shows stale state until its
    // next poll.
  }
}

void MprisService::OnMethodCall(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring&,
                                 const Glib::ustring& object_path, const Glib::ustring& interface_name,
                                 const Glib::ustring& method_name, const Glib::VariantContainerBase& parameters,
                                 const Glib::RefPtr<Gio::DBus::MethodInvocation>& invocation)
{
  if (interface_name == kRootInterface)
  {
    if (method_name == "Raise")
    {
      window_.present();
    }
    else if (method_name == "Quit")
    {
      if (auto app = window_.get_application())
        app->quit();
    }
    invocation->return_value({});
    return;
  }

  if (interface_name != kPlayerInterface)
  {
    invocation->return_value({});
    return;
  }

  if (method_name == "Next")
    backend_.Next();
  else if (method_name == "Previous")
    backend_.Previous();
  else if (method_name == "Pause")
    backend_.PauseOrStop();
  else if (method_name == "Stop")
    backend_.PauseOrStop();
  else if (method_name == "Play")
    backend_.Play();
  else if (method_name == "PlayPause")
  {
    NowPlaying np = backend_.GetNowPlaying();
    if (np.valid && np.state == TransportState::Playing)
      backend_.PauseOrStop();
    else
      backend_.Play();
  }
  else if (method_name == "Seek")
  {
    Glib::VariantBase offset_variant;
    parameters.get_child(offset_variant, 0);
    gint64 offset_us = offset_variant.get_dynamic<gint64>();

    NowPlaying np = backend_.GetNowPlaying();
    long long new_position =
        static_cast<long long>(backend_.GetPosition()) + offset_us / 1000000;
    new_position = std::clamp<long long>(new_position, 0, np.duration);
    backend_.SeekAsync(static_cast<unsigned>(new_position));
    connection->emit_signal(
        object_path, kPlayerInterface, "Seeked", {},
        Glib::VariantContainerBase::create_tuple(Glib::Variant<gint64>::create(new_position * 1000000)));
  }
  else if (method_name == "SetPosition")
  {
    Glib::VariantBase track_id_variant;
    parameters.get_child(track_id_variant, 0);
    Glib::VariantBase position_variant;
    parameters.get_child(position_variant, 1);
    // Per spec: if TrackId doesn't match the current track, do nothing —
    // not an error. kTrackId is the only id Gnomos ever hands out (see its
    // own comment), so this also rejects stale ids from a track that has
    // since changed.
    if (track_id_variant.get_dynamic<Glib::DBusObjectPathString>() == kTrackId)
    {
      NowPlaying np = backend_.GetNowPlaying();
      long long position = position_variant.get_dynamic<gint64>() / 1000000;
      position = std::clamp<long long>(position, 0, np.duration);
      backend_.SeekAsync(static_cast<unsigned>(position));
      connection->emit_signal(
          object_path, kPlayerInterface, "Seeked", {},
          Glib::VariantContainerBase::create_tuple(Glib::Variant<gint64>::create(position * 1000000)));
    }
  }

  invocation->return_value({});
}

void MprisService::OnGetProperty(Glib::VariantBase& property, const Glib::RefPtr<Gio::DBus::Connection>&,
                                  const Glib::ustring&, const Glib::ustring&, const Glib::ustring& interface_name,
                                  const Glib::ustring& property_name)
{
  if (interface_name == kRootInterface)
  {
    if (property_name == "CanQuit")
      property = Glib::Variant<bool>::create(true);
    else if (property_name == "CanRaise")
      property = Glib::Variant<bool>::create(true);
    else if (property_name == "HasTrackList")
      property = Glib::Variant<bool>::create(false);
    else if (property_name == "Identity")
      property = Glib::Variant<Glib::ustring>::create("Gnomos");
    else if (property_name == "DesktopEntry")
      property = Glib::Variant<Glib::ustring>::create(APPLICATION_ID);
    else if (property_name == "SupportedUriSchemes")
      property = Glib::Variant<std::vector<Glib::ustring>>::create({});
    else if (property_name == "SupportedMimeTypes")
      property = Glib::Variant<std::vector<Glib::ustring>>::create({});
    return;
  }

  if (interface_name != kPlayerInterface)
    return;

  NowPlaying np = backend_.GetNowPlaying();
  if (property_name == "PlaybackStatus")
    property = Glib::Variant<Glib::ustring>::create(PlaybackStatusFor(np));
  else if (property_name == "LoopStatus")
    property = Glib::Variant<Glib::ustring>::create(LoopStatusFor(np.repeat));
  else if (property_name == "Rate")
    property = Glib::Variant<double>::create(1.0);
  else if (property_name == "Shuffle")
    property = Glib::Variant<bool>::create(np.shuffle);
  else if (property_name == "Metadata")
    property = BuildMetadata();
  else if (property_name == "Volume")
    property = Glib::Variant<double>::create(backend_.GetVolume().volume / 100.0);
  else if (property_name == "Position")
    property = Glib::Variant<gint64>::create(static_cast<gint64>(backend_.GetPosition()) * 1000000);
  else if (property_name == "MinimumRate")
    property = Glib::Variant<double>::create(1.0);
  else if (property_name == "MaximumRate")
    property = Glib::Variant<double>::create(1.0);
  else if (property_name == "CanGoNext")
    property = Glib::Variant<bool>::create(np.valid && np.can_go_next);
  else if (property_name == "CanGoPrevious")
    property = Glib::Variant<bool>::create(np.valid && np.can_go_previous);
  else if (property_name == "CanPlay")
    property = Glib::Variant<bool>::create(np.valid);
  else if (property_name == "CanPause")
    property = Glib::Variant<bool>::create(np.valid && np.can_pause);
  else if (property_name == "CanSeek")
    property = Glib::Variant<bool>::create(np.valid && np.duration > 0);
  else if (property_name == "CanControl")
    property = Glib::Variant<bool>::create(np.valid);
}

bool MprisService::OnSetProperty(const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&,
                                  const Glib::ustring&, const Glib::ustring& interface_name,
                                  const Glib::ustring& property_name, const Glib::VariantBase& value)
{
  if (interface_name != kPlayerInterface)
    return false;

  if (property_name == "Volume")
  {
    double volume = value.get_dynamic<double>();
    backend_.SetVolume(static_cast<uint8_t>(std::clamp(volume, 0.0, 1.0) * 100.0));
    return true;
  }
  if (property_name == "Shuffle")
  {
    backend_.SetShuffle(value.get_dynamic<bool>());
    return true;
  }
  if (property_name == "LoopStatus")
  {
    Glib::ustring status = value.get_dynamic<Glib::ustring>();
    RepeatMode mode = status == "Track" ? RepeatMode::One : status == "Playlist" ? RepeatMode::All : RepeatMode::Off;
    backend_.SetRepeatMode(mode);
    return true;
  }
  return false;
}

}  // namespace gnomos
