#!/bin/bash
# Rebuild the VAAPI shim from the shipped source. The prebuilt .so in ../lib
# is the verified artifact; this script is for reference/reproducibility.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
gcc -shared -fPIC -O2 -Wall -o "$HERE/../lib/vaapi-shim.so" "$HERE/vaapi-shim.c" -ldl
echo "rebuilt: $HERE/../lib/vaapi-shim.so"
echo "HISI_VAAPI_SPOOF=0 disables the 1.14->1.17 spoof; VAAPI_SHIM_DEBUG=1 traces it."
