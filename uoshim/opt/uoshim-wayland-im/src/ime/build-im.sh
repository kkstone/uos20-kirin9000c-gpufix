#!/bin/bash
# Rebuild wayland-im-shim from source. Requires: gcc, libwayland-client-dev,
# libdbus-1-dev, libxkbcommon-dev. Generated protocol .c/.h are included,
# so wayland-scanner is NOT needed unless you regenerate them from the .xml.
set -e
cd "$(dirname "$0")"
gcc -O2 -o ../../bin/wayland-im-shim \
  wayland-im-shim.c input-method-unstable-v1-client-protocol.c \
  -lwayland-client -ldbus-1 -lxkbcommon
echo "built: $(md5sum ../../bin/wayland-im-shim)"
