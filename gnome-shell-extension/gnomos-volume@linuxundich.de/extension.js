// SPDX-License-Identifier: GPL-3.0-or-later

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Slider} from 'resource:///org/gnome/shell/ui/slider.js';
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
const BUS_NAME = 'de.linuxundich.Gnomos.Zone';
const OBJECT_PATH = '/de/linuxundich/Gnomos/Zone';

// Mirrors zone-volume-service.cpp's own introspection XML exactly — kept
// as a small local copy rather than fetched at runtime, since a mismatch
// here would only ever be caught by testing anyway (Gio.DBusProxy doesn't
// validate against the service's actual introspection unless asked).
const INTERFACE_XML = `
<node>
  <interface name="de.linuxundich.Gnomos.Zone">
    <method name="SetVolume">
      <arg direction="in" name="Volume" type="d"/>
    </method>
    <method name="SetMuted">
      <arg direction="in" name="Muted" type="b"/>
    </method>
    <method name="SetRoomVolume">
      <arg direction="in" name="PlayerUuid" type="s"/>
      <arg direction="in" name="Volume" type="d"/>
    </method>
    <method name="SetRoomMuted">
      <arg direction="in" name="PlayerUuid" type="s"/>
      <arg direction="in" name="Muted" type="b"/>
    </method>
    <method name="RefreshRooms"/>
    <property name="Volume" type="d" access="read"/>
    <property name="Muted" type="b" access="read"/>
    <property name="ZoneName" type="s" access="read"/>
    <property name="Rooms" type="a(ssdb)" access="read"/>
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

// Safety cap for the room-name column's width — the actual shared column
// width used across all rows is the *narrower* of "the longest name
// currently shown" and this, so one implausibly long room name can't blow
// out every row's slider length; see _syncRoomsFromProxy()'s own comment
// for how that shared width is computed and applied.
const MAX_ROOM_NAME_CHARS = 18;

function truncateRoomName(name) {
  if (name.length <= MAX_ROOM_NAME_CHARS)
    return name;
  return `${name.slice(0, MAX_ROOM_NAME_CHARS - 1)}…`;
}

// One row in the "other rooms" submenu — a clickable mute icon, a room
// name, and its own independent slider, mirroring Gnomos's own in-app
// grouping popover (a master fader for the selected zone plus one slider
// per physical room) with the main slider's own icon-click-to-mute added
// on top (requested afterward — the main row could already mute *its own*
// zone this way, but every other room had no mute at all). Not
// activatable: PopupBaseMenuItem's default click-to-activate-and-close
// behavior is exactly wrong for a slider row, the same reason QuickSlider
// itself is built the same way.
const GnomosRoomSliderItem = GObject.registerClass(
class GnomosRoomSliderItem extends PopupMenu.PopupBaseMenuItem {
  _init(uuid, name, volume, muted, proxy) {
    super._init({reactive: false, activate: false, can_focus: false});

    this._uuid = uuid;
    this._proxy = proxy;
    this._applyingRemote = false;
    this._volume = volume;
    this._muted = muted;

    // St.Button wrapping St.Icon, the same construction QuickSlider's own
    // clickable icon uses internally (see quickSettings.js's QuickSlider —
    // there's no ready-made "clickable icon" item PopupBaseMenuItem itself
    // offers, so this is built by hand rather than through any inherited
    // iconReactive/icon-clicked property the way the main slider has).
    this._icon = new St.Icon({icon_name: iconNameForVolume(volume, muted)});
    this._iconButton = new St.Button({
      child: this._icon,
      style_class: 'icon-button flat',
      can_focus: true,
      x_expand: false,
      y_expand: true,
    });
    this._iconButton.connect('clicked', () => this._onIconClicked());
    this.add_child(this._iconButton);

    this._label = new St.Label({text: truncateRoomName(name), y_align: Clutter.ActorAlign.CENTER});
    this.add_child(this._label);

    this.slider = new Slider(volume);
    this.slider.xExpand = true;
    this._sliderChangedId = this.slider.connect('notify::value', () => this._onSliderChanged());
    const sliderBin = new St.Bin({
      child: this.slider,
      x_expand: true,
      y_align: Clutter.ActorAlign.CENTER,
    });
    this.add_child(sliderBin);

    this._updateMutedStyle();
  }

  setName(name) {
    this._label.text = truncateRoomName(name);
  }

  // Natural (unconstrained) width of the room-name label as currently
  // set — used by _syncRoomsFromProxy() to find the widest name among all
  // currently shown rows, before applying that shared width to every
  // row's label so their sliders all line up at the same x position.
  getLabelNaturalWidth() {
    return this._label.get_preferred_width(-1)[1];
  }

  setLabelWidth(width) {
    this._label.width = width;
  }

  setVolume(volume) {
    this._volume = volume;
    this._applyingRemote = true;
    this.slider.value = volume;
    this._applyingRemote = false;
    this._icon.iconName = iconNameForVolume(this._volume, this._muted);
  }

  setMuted(muted) {
    this._muted = muted;
    this._icon.iconName = iconNameForVolume(this._volume, this._muted);
    this._updateMutedStyle();
  }

  // Greys out and disables dragging the slider itself while muted — Sonos
  // keeps mute and volume as two fully independent properties (the same
  // reason the main slider's own icon-click-to-mute never touches its
  // slider value at all), but leaving a muted room's slider fully
  // interactive reads as "this still does something" when it doesn't.
  _updateMutedStyle() {
    this.slider.reactive = !this._muted;
    this.slider.opacity = this._muted ? 128 : 255;
  }

  _onSliderChanged() {
    if (this._applyingRemote || !this._proxy)
      return;
    this._proxy.SetRoomVolumeRemote(this._uuid, this.slider.value, () => {});
  }

  _onIconClicked() {
    if (!this._proxy)
      return;
    // Applied optimistically, unlike the slider (whose value the user's
    // own drag already moves directly): Rooms only ever refreshes on a
    // throwaway poll (menu open, or another explicit RefreshRooms call),
    // not a live push subscription the way the *current* zone's own
    // Muted property gets — without this, clicking the icon would visibly
    // do nothing until the menu was closed and reopened.
    //
    // The optimistic value alone isn't quite enough, though: it's local
    // to this widget, not the proxy's own cached Rooms — an unrelated
    // property tick (the *current* zone's own live Volume/Muted updates
    // fire independently) would re-run _syncRoomsFromProxy() against that
    // still-stale cached Rooms and immediately stomp this back to the old
    // value. Chaining RefreshRoomsRemote() into SetRoomMutedRemote()'s own
    // completion callback (both land on NosonBackend's single serial task
    // queue in that order, so the refresh's GetMute() is guaranteed to run
    // after the mute itself actually reached the device, not just after
    // the D-Bus call returned) gets a real, correct Rooms update back
    // quickly instead of waiting for the next menu open.
    const newMuted = !this._muted;
    this._proxy.SetRoomMutedRemote(this._uuid, newMuted, () => {
      this._proxy?.RefreshRoomsRemote(() => {});
    });
    this.setMuted(newMuted);
  }

  destroy() {
    if (this._sliderChangedId) {
      this.slider.disconnect(this._sliderChangedId);
      this._sliderChangedId = 0;
    }
    this._proxy = null;
    super.destroy();
  }
});

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

    // uuid -> GnomosRoomSliderItem, for every room currently shown in the
    // "other rooms" submenu — see _syncRoomsFromProxy() for why this is
    // diffed against Rooms rather than torn down and rebuilt on every
    // property sync (which fires on every volume tick, not just when the
    // room list itself actually changes).
    this._roomItems = new Map();
    this.menu.setHeader('network-wireless-symbolic', 'Andere Räume');
    // Refreshes on open, the same "meant to be called while the popover is
    // open" pattern NosonBackend::RefreshGroupVolumesAsync() already
    // documents for the in-app grouping popover — Rooms would otherwise
    // only ever reflect whatever the last refresh (at connect time)
    // happened to catch.
    this.menu.connect('open-state-changed', (menu, isOpen) => {
      if (isOpen)
        this._proxy?.RefreshRoomsRemote(() => {});
    });

    this._proxy = new ZoneProxy(Gio.DBus.session, BUS_NAME, OBJECT_PATH, (proxy, error) => {
      if (error) {
        console.error(`Gnomos zone volume: failed to connect (${error.message})`);
        return;
      }
      this._propertiesChangedId = this._proxy.connect('g-properties-changed', () => this._syncFromProxy());
      this._syncFromProxy();
      this._proxy.RefreshRoomsRemote(() => {});
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
    this.slider.accessibleName = label;
    this.iconLabel = label;

    this._syncRoomsFromProxy();
  }

  // Adds/removes rows to match the current Rooms set and updates every
  // surviving row's value in place, rather than clearing and rebuilding
  // the whole submenu on every sync — Rooms arrives on every property
  // change (including plain volume ticks), and tearing down a room's
  // slider mid-drag just because *some* property changed would fight the
  // user's own in-progress drag on that exact slider.
  _syncRoomsFromProxy() {
    const rooms = this._proxy.Rooms ?? [];
    const seen = new Set();

    for (const [uuid, name, volume, muted] of rooms) {
      seen.add(uuid);
      let item = this._roomItems.get(uuid);
      if (!item) {
        item = new GnomosRoomSliderItem(uuid, name, volume, muted, this._proxy);
        this._roomItems.set(uuid, item);
        this.menu.addMenuItem(item);
      } else {
        item.setName(name);
        item.setVolume(volume);
        item.setMuted(muted);
      }
    }

    for (const [uuid, item] of this._roomItems) {
      if (!seen.has(uuid)) {
        item.destroy();
        this._roomItems.delete(uuid);
      }
    }

    // Every row's own label naturally sizes to its own room name, so left
    // to itself each slider would start at a different x position — the
    // widest current name (already capped, see MAX_ROOM_NAME_CHARS) sets a
    // shared column width applied to every row, so every slider lines up
    // and is the same length regardless of room name length.
    let labelWidth = 0;
    for (const item of this._roomItems.values())
      labelWidth = Math.max(labelWidth, item.getLabelNaturalWidth());
    for (const item of this._roomItems.values())
      item.setLabelWidth(labelWidth);

    this.menuEnabled = this._roomItems.size > 0;
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

    // Not strictly required — this.menu (and everything in it) gets torn
    // down by QuickSettingsItem's own destroy() regardless — but each
    // item's own destroy() also drops its slider signal handler explicitly
    // (see GnomosRoomSliderItem), so this keeps that same explicit-over-
    // implicit cleanup discipline rather than relying on it happening only
    // as a side effect further up the chain.
    for (const item of this._roomItems.values())
      item.destroy();
    this._roomItems.clear();

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
