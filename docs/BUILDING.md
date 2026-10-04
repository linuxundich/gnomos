# Building Gnomos

Prebuilt Flatpak bundles are on the [Releases page](https://github.com/linuxundich/gnomos/releases); see the [README](../README.md#install). To build Gnomos yourself:

Dependencies: `meson`, `ninja`, a C++17 compiler, `pkgconf`, `openssl`,
`zlib`, `gtkmm-4.0` (>= 4.10), `libadwaita-1` (>= 1.9), `json-glib-1.0`,
`libsoup-3.0` and `libwebp`. On Arch Linux:

```sh
sudo pacman -S meson ninja gcc pkgconf openssl zlib gtkmm-4.0 libadwaita json-glib libsoup3 libwebp
```

Then:

```sh
git clone --recurse-submodules https://github.com/linuxundich/gnomos.git
cd gnomos
meson setup build
ninja -C build
./build/src/gnomos
```

libnoson is a git submodule under `noson/` (`--recurse-submodules` above
fetches it too) and is built together with Gnomos; you don't need to
install it separately. If you already cloned without that flag, run
`git submodule update --init` to fetch it afterwards.

A Flatpak manifest exists under `build-aux/flatpak/` and has been verified
end to end (builds, installs, and runs against `org.gnome.Platform`//51).
To build and install the Flatpak yourself:

```sh
cd build-aux/flatpak
flatpak-builder --user --install --force-clean --repo=.flatpak-repo .flatpak-build de.linuxundich.Gnomos.json
```

See [ARCHITECTURE.md](../ARCHITECTURE.md) for details on the manifest's version
pins.

The companion GNOME Shell extension
(`gnome-shell-extension/gnomos-volume@linuxundich.de/`) is a separate,
optional install — see its own [README](../gnome-shell-extension/gnomos-volume@linuxundich.de/README.md)
for how to add it.

## Upgrading from 0.20.x or earlier

Version 0.21.0 changed the application ID from `de.christophlangner.Gnomos`
to `de.linuxundich.Gnomos`, so the new bundle installs as a separate app
next to the old one rather than updating it. To keep your linked services,
scenes and settings, quit the old Gnomos and copy its data over before the
first start of the new one, then remove the old app:

```sh
cp -a ~/.var/app/de.christophlangner.Gnomos ~/.var/app/de.linuxundich.Gnomos
flatpak uninstall --user de.christophlangner.Gnomos
```

The Shell extension was renamed as well, to `gnomos-volume@linuxundich.de`:
remove the old `gnomos-volume@christophlangner.de` and install the new one
as described in its [README](../gnome-shell-extension/gnomos-volume@linuxundich.de/README.md).
