// SPDX-License-Identifier: GPL-3.0-or-later

#include "zone-volume-service.h"

#include <algorithm>

#include <giomm/dbusintrospection.h>
#include <giomm/dbusownname.h>

namespace gnomos
{

namespace
{

const char* const kBusName = "de.christophlangner.Gnomos.Zone";
const char* const kObjectPath = "/de/christophlangner/Gnomos/Zone";
const char* const kInterfaceName = "de.christophlangner.Gnomos.Zone";

const char* const kIntrospectionXml = R"XML(
<node>
  <interface name="de.christophlangner.Gnomos.Zone">
    <method name="SetVolume">
      <arg direction="in" name="Volume" type="d"/>
    </method>
    <method name="SetMuted">
      <arg direction="in" name="Muted" type="b"/>
    </method>
    <property name="Volume" type="d" access="read"/>
    <property name="Muted" type="b" access="read"/>
    <property name="ZoneName" type="s" access="read"/>
  </interface>
</node>
)XML";

}  // namespace

ZoneVolumeService::ZoneVolumeService(NosonBackend& backend) : backend_(backend)
{
  own_name_id_ =
      Gio::DBus::own_name(Gio::DBus::BusType::SESSION, kBusName, sigc::mem_fun(*this, &ZoneVolumeService::OnBusAcquired));

  // player_ready fires on every zone switch (see SelectZone()'s own
  // comment) — needed on top of volume_changed since a zone switch changes
  // ZoneName even on a tick where the new zone's volume happens to be
  // identical to the old one's.
  volume_connection_ = backend_.signal_volume_changed().connect(sigc::mem_fun(*this, &ZoneVolumeService::OnStateChanged));
  player_ready_connection_ =
      backend_.signal_player_ready().connect(sigc::mem_fun(*this, &ZoneVolumeService::OnStateChanged));
}

ZoneVolumeService::~ZoneVolumeService()
{
  volume_connection_.disconnect();
  player_ready_connection_.disconnect();

  if (connection_ && registration_id_ != 0)
    connection_->unregister_object(registration_id_);
  if (own_name_id_ != 0)
    Gio::DBus::unown_name(own_name_id_);
}

void ZoneVolumeService::OnBusAcquired(const Glib::RefPtr<Gio::DBus::Connection>& connection, const Glib::ustring&)
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

  Glib::RefPtr<Gio::DBus::InterfaceInfo> interface_info = node_info->lookup_interface(kInterfaceName);
  if (!interface_info)
    return;

  try
  {
    registration_id_ = connection_->register_object(kObjectPath, interface_info,
                                                      sigc::mem_fun(*this, &ZoneVolumeService::OnMethodCall),
                                                      sigc::mem_fun(*this, &ZoneVolumeService::OnGetProperty));
  }
  catch (const Glib::Error&)
  {
    // Another process already owns this path/interface — shouldn't happen
    // (the bus name itself is exclusively ours once acquired), but isn't
    // worth taking the app down over.
  }
}

void ZoneVolumeService::OnStateChanged()
{
  EmitPropertiesChanged();
}

void ZoneVolumeService::OnMethodCall(const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&,
                                      const Glib::ustring&, const Glib::ustring& interface_name,
                                      const Glib::ustring& method_name, const Glib::VariantContainerBase& parameters,
                                      const Glib::RefPtr<Gio::DBus::MethodInvocation>& invocation)
{
  if (interface_name == kInterfaceName)
  {
    if (method_name == "SetVolume")
    {
      Glib::VariantBase volume_variant;
      parameters.get_child(volume_variant, 0);
      double volume = volume_variant.get_dynamic<double>();
      backend_.SetVolume(static_cast<uint8_t>(std::clamp(volume, 0.0, 1.0) * 100.0));
    }
    else if (method_name == "SetMuted")
    {
      Glib::VariantBase muted_variant;
      parameters.get_child(muted_variant, 0);
      backend_.SetMuted(muted_variant.get_dynamic<bool>());
    }
  }
  invocation->return_value({});
}

void ZoneVolumeService::OnGetProperty(Glib::VariantBase& property, const Glib::RefPtr<Gio::DBus::Connection>&,
                                       const Glib::ustring&, const Glib::ustring&, const Glib::ustring& interface_name,
                                       const Glib::ustring& property_name)
{
  if (interface_name != kInterfaceName)
    return;

  if (property_name == "Volume")
    property = Glib::Variant<double>::create(backend_.GetVolume().volume / 100.0);
  else if (property_name == "Muted")
    property = Glib::Variant<bool>::create(backend_.GetVolume().muted);
  else if (property_name == "ZoneName")
    property = Glib::Variant<Glib::ustring>::create(backend_.GetCurrentZoneName());
}

void ZoneVolumeService::EmitPropertiesChanged()
{
  if (!connection_)
    return;

  VolumeInfo volume = backend_.GetVolume();
  std::map<Glib::ustring, Glib::VariantBase> changed;
  changed["Volume"] = Glib::Variant<double>::create(volume.volume / 100.0);
  changed["Muted"] = Glib::Variant<bool>::create(volume.muted);
  changed["ZoneName"] = Glib::Variant<Glib::ustring>::create(backend_.GetCurrentZoneName());

  std::vector<Glib::VariantBase> args = {
      Glib::Variant<Glib::ustring>::create(kInterfaceName),
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
    // Non-fatal — worst case, the extension shows stale state until its
    // next poll.
  }
}

}  // namespace gnomos
