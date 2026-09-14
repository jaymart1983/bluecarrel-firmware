#!/usr/bin/env bash
# Publish a firmware build to an update page for the X4 Pro Sync app.
#
#   scripts/make_firmware_json.sh <update-page-dir> [firmware.bin]
#
# Copies the image to <dir>/crosspoint-x4pro-<stamp>.bin and writes
# <dir>/firmware.json = {version, file, size, sha256}. Serve <dir> with any static
# web server and enter its URL as the update page in the app.
set -euo pipefail

dir=${1:?usage: $0 <update-page-dir> [firmware.bin]}
bin=${2:-.pio/build/x4pro/firmware.bin}
header=src/network/BuildStamp.h

[ -f "$bin" ] || { echo "no firmware image at $bin (build with: pio run -e x4pro)" >&2; exit 1; }
stamp=$(sed -n 's/.*X4_BUILD_STAMP "\([0-9.]*\)".*/\1/p' "$header")
[ -n "$stamp" ] || { echo "no X4_BUILD_STAMP in $header" >&2; exit 1; }

if command -v sha256sum >/dev/null; then
  sha=$(sha256sum "$bin" | cut -d' ' -f1)
else
  sha=$(shasum -a 256 "$bin" | cut -d' ' -f1)
fi
size=$(wc -c < "$bin" | tr -d ' ')
file="crosspoint-x4pro-$stamp.bin"

mkdir -p "$dir"
cp "$bin" "$dir/$file"
# Write the manifest last and atomically: the app must never see a manifest that
# names an image that is not fully there yet.
printf '{"version":"%s","file":"%s","size":%s,"sha256":"%s"}\n' "$stamp" "$file" "$size" "$sha" > "$dir/firmware.json.tmp"
chmod 644 "$dir/$file" "$dir/firmware.json.tmp"
mv "$dir/firmware.json.tmp" "$dir/firmware.json"
cat "$dir/firmware.json"
