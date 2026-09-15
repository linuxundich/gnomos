# Gnomos Zone Volume

A small GNOME Shell extension that adds the Sonos zone volume currently
controlled by [Gnomos](https://github.com/linuxundich/gnomos) as its own
slider in the system volume Quick Settings menu — alongside the regular
output/input volume rows.

It talks to Gnomos over a small custom D-Bus interface
(`de.christophlangner.Gnomos.Zone`, see `src/zone-volume-service.cpp` in the
main repo) rather than MPRIS: GNOME Shell's Quick Settings sliders only ever
come from real PulseAudio/PipeWire stream volumes, so MPRIS's own `Volume`
property (which Gnomos also exposes, for other MPRIS clients) can't drive
one directly.

The slider only appears while Gnomos is actually running — there is nothing
to control otherwise, the same way an app's own volume row in the system
mixer only exists while it has an active audio stream.

## Requirements

Gnomos itself needs `--own-name=de.christophlangner.Gnomos.Zone` in its
Flatpak permissions (already in `build-aux/flatpak/de.christophlangner.Gnomos.json`)
to be allowed to claim that bus name from inside the sandbox. A native,
non-Flatpak build needs no extra permission at all.

## Installing

```sh
mkdir -p ~/.local/share/gnome-shell/extensions
ln -s "$(pwd)" ~/.local/share/gnome-shell/extensions/gnomos-volume@christophlangner.de
gnome-extensions enable gnomos-volume@christophlangner.de
```

Then log out and back in (or, on X11 only, `Alt`+`F2` → `r` → `Enter` to
restart GNOME Shell — not possible on Wayland) so GNOME Shell picks up the
new extension.

## Developing

`gnome-extensions disable`/`enable` genuinely toggles the extension's
`INACTIVE`/`ACTIVE` state and re-runs `enable()`/`disable()`, but GNOME
Shell caches the extension's JS module by file URL for the life of the
Shell process — editing `extension.js` and cycling disable/enable does
**not** pick up the change, only a full Shell restart does (confirmed live:
temporary debug tracing at the very top of `enable()` never fired across
several disable/enable cycles after an edit). So on Wayland, every code
change needs another logout/login to actually verify, not just the first
install.
