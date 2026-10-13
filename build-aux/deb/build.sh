#!/bin/bash
# Build the .deb packages for Debian testing in a throwaway Podman container.
# Output: build-aux/deb/out/*.deb
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
out=$root/build-aux/deb/out
mkdir -p "$out"
podman run --rm \
  -v "$root":/src:ro \
  -v "$out":/out \
  docker.io/library/debian:testing bash -euxc '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq build-essential devscripts equivs rsync
    rsync -a --exclude=_build --exclude=build --exclude=build-flatpak \
          --exclude=build-aux/deb/out --exclude=.git /src/ /work/
    cd /work
    mk-build-deps -ir -t "apt-get -y -qq --no-install-recommends"
    dpkg-buildpackage -us -uc -b
    cp /gnomos*.deb /gnome-shell-extension-gnomos-volume*.deb /out/
  '
ls -l "$out"
