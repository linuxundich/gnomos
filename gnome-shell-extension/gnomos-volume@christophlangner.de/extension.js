// SPDX-License-Identifier: GPL-3.0-or-later

import Gio from 'gi://Gio';
import GObject from 'gi://GObject';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// Companion to Gnomos's own ZoneVolumeService (src/zone-volume-service.cpp)
// — that class exposes the currently selected Sonos zone's volume as a
// tiny custom D-Bus interface on its own well-known name specifically
// because GNOME Shell's Quick Settings volume sliders only ever come from
// real PulseAudio/PipeWire stream volumes, never from an MPRIS player's
// Volume property (which Gnomos also exposes, for every *other* MPRIS
// client). This extension is the other half: it proxies that interface
// and renders it as a QuickSlider, the same widget class GNOME Shell's own
// output/input volume rows use.
const BUS_NAME = 'de.christophlangner.Gnomos.Zone';
const OBJECT_PATH = '/de/christophlangner/Gnomos/Zone';

// Mirrors zone-volume-service.cpp's own introspection XML exactly — kept
// as a small local copy rather than fetched at runtime, since a mismatch
// here would only ever be caught by testing anyway (Gio.DBusProxy doesn't
// validate against the service's actual introspection unless asked).
const INTERFACE_XML = `
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
</node>`;

const ZoneProxy = Gio.DBusProxy.makeProxyWrapper(INTERFACE_XML);

// Same low/medium/high/muted threshold convention PlayerBar's own mute
// button icon uses (gnomos-window.cpp), so the slider's icon reads
// consistently with the rest of the app.
function iconNameForVolume(volume, muted) {
  if (muted || volume <= 0)
    return 'audio-volume-muted-symbolic';
  if (volume < 0.33)
    return 'audio-volume-low-symbolic';
  if (volume < 0.67)
    return 'audio-volume-medium-symbolic';
  return 'audio-volume-high-symbolic';
}

const GnomosZoneSlider = GObject.registerClass(
class GnomosZoneSlider extends QuickSettings.QuickSlider {
  _init() {
    super._init({iconName: 'audio-volume-high-symbolic'});

    this.iconReactive = true;
    this._iconClickedId = this.connect('icon-clicked', () => this._onIconClicked());
    this._sliderChangedId = this.slider.connect('notify::value', () => this._onSliderChanged());

    // Set while _syncFromProxy() is writing into the slider, so that
    // notify::value's own handler can tell "the proxy just told us the
    // real value" apart from "the user just dragged the slider" — without
    // this, every incoming property update would immediately bounce a
    // SetVolume() call right back at Gnomos.
    this._applyingRemote = false;
    this._proxy = null;
    this._propertiesChangedId = 0;

    this._proxy = new ZoneProxy(Gio.DBus.session, BUS_NAME, OBJECT_PATH, (proxy, error) => {
      if (error) {
        console.error(`Gnomos zone volume: failed to connect (${error.message})`);
        return;
      }
      this._propertiesChangedId = this._proxy.connect('g-properties-changed', () => this._syncFromProxy());
      this._syncFromProxy();
    });
  }

  _syncFromProxy() {
    if (!this._proxy)
      return;

    const volume = this._proxy.Volume ?? 0;
    const muted = this._proxy.Muted ?? false;
    const zoneName = this._proxy.ZoneName || '';

    this._applyingRemote = true;
    this.slider.value = volume;
    this._applyingRemote = false;

    this.iconName = iconNameForVolume(volume, muted);
    const label = zoneName ? `Gnomos (${zoneName})` : 'Gnomos';
    this.slider.accessible_name = label;
    this.iconLabel = label;
  }

  _onSliderChanged() {
    if (this._applyingRemote || !this._proxy)
      return;
    this._proxy.SetVolumeRemote(this.slider.value, () => {});
  }

  _onIconClicked() {
    if (!this._proxy)
      return;
    this._proxy.SetMutedRemote(!(this._proxy.Muted ?? false), () => {});
  }

  destroy() {
    if (this._proxy && this._propertiesChangedId)
      this._proxy.disconnect(this._propertiesChangedId);
    this._proxy = null;

    if (this._iconClickedId) {
      this.disconnect(this._iconClickedId);
      this._iconClickedId = 0;
    }
    if (this._sliderChangedId) {
      this.slider.disconnect(this._sliderChangedId);
      this._sliderChangedId = 0;
    }

    super.destroy();
  }
});

export default class GnomosVolumeExtension extends Extension {
  enable() {
    this._slider = null;
    // Only shown while Gnomos is actually running and has claimed the
    // name — there's nothing to control otherwise, the same way a
    // per-app PulseAudio volume row only exists while that app has an
    // active stream.
    this._nameWatcherId = Gio.bus_watch_name(
      Gio.BusType.SESSION, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
      () => this._addIndicator(),
      () => this._removeIndicator());
  }

  disable() {
    if (this._nameWatcherId) {
      Gio.bus_unwatch_name(this._nameWatcherId);
      this._nameWatcherId = null;
    }
    this._removeIndicator();
  }

  _addIndicator() {
    if (this._slider)
      return;
    this._slider = new GnomosZoneSlider();

    // Deliberately NOT QuickSettings.SystemIndicator + addExternalIndicator()
    // — that API inserts before the Background Apps toggle (or at the grid's
    // end if that toggle doesn't exist), nowhere near the volume slider.
    // Requested placement is directly under the system volume slider, so
    // this reaches into the QuickSettingsMenu grid directly instead:
    // this._grid.insert_child_below(item, sibling) — despite the name,
    // "below" here means *earlier* in child order (confirmed against
    // addExternalIndicator()'s own use of the same call, commented "Insert
    // before ..." for the exact same insert_child_below() call), which in
    // a top-to-bottom grid means visually *above* sibling. Passing the
    // brightness slider as sibling therefore lands this row between the
    // volume slider and the brightness slider.
    const quickSettings = Main.panel.statusArea.quickSettings;
    const brightnessItem = quickSettings._brightness?.quickSettingsItems?.[0] ?? null;
    if (brightnessItem)
      quickSettings.menu.insertItemBefore(this._slider, brightnessItem, 2);
    else
      quickSettings.menu.addItem(this._slider, 2);
  }

  _removeIndicator() {
    if (!this._slider)
      return;
    this._slider.destroy();
    this._slider = null;
  }
}
