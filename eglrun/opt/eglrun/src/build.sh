#!/bin/bash
# Rebuild the ICD wrapper from the shipped source. The prebuilt .so in ../lib
# is the verified artifact; this script is for reference/reproducibility.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
gcc -shared -fPIC -O2 -Wall -o "$HERE/../lib/icd-wrap.so" "$HERE/icd-wrap.c" -ldl
echo "rebuilt: $HERE/../lib/icd-wrap.so"
echo "manifest: $HERE/../lib/maleoon-wrap.json -> library_path must be /opt/eglrun/lib/icd-wrap.so"
