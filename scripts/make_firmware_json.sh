#!/usr/bin/env bash
# Publish a signed firmware build to an update page for the Bluecarrel app.
#
#   FIRMWARE_SIGNING_KEY_FILE=key.pem scripts/make_firmware_json.sh <update-page-dir> [firmware.bin]
#
# Writes into <dir>:
#   bluecarrel-x4pro-<stamp>.bin       the image
#   bluecarrel-x4pro-<stamp>.bin.sig   its signature as hex text (copy to /firmware/firmware.bin.sig)
#   firmware.json                      {version, file, size, sha256, signature}
# Serve <dir> with any static web server and enter its URL as the update page in the app.
#
# Signature (docs/security-v2.md): ECDSA P-256 with SHA-256 over the ASCII bytes
# "X4FW1|<stamp>|<sha256 lowercase hex>" (no newline), DER encoded, lowercase hex.
# The key must match src/network/FirmwareSigningKey.h. Without FIRMWARE_SIGNING_KEY_FILE
# the script refuses unless ALLOW_UNSIGNED=1; readers refuse unsigned images.
set -euo pipefail

dir=${1:?usage: $0 <update-page-dir> [firmware.bin]}
bin=${2:-.pio/build/x4pro/firmware.bin}
header=src/network/BuildStamp.h
key_header=src/network/FirmwareSigningKey.h

[ -f "$bin" ] || { echo "no firmware image at $bin (build with: pio run -e x4pro)" >&2; exit 1; }
stamp=$(sed -n 's/.*X4_BUILD_STAMP "\([0-9.]*\)".*/\1/p' "$header")
[ -n "$stamp" ] || { echo "no X4_BUILD_STAMP in $header" >&2; exit 1; }
[[ "$stamp" =~ ^[0-9]{8}\.[0-9]{4}$ ]] || { echo "X4_BUILD_STAMP '$stamp' is not yyyyMMdd.HHmm" >&2; exit 1; }

if command -v sha256sum >/dev/null; then
  sha=$(sha256sum "$bin" | cut -d' ' -f1)
else
  sha=$(shasum -a 256 "$bin" | cut -d' ' -f1)
fi
sha=$(printf '%s' "$sha" | tr 'A-F' 'a-f')
size=$(wc -c < "$bin" | tr -d ' ')
file="bluecarrel-x4pro-$stamp.bin"

# Lowercase hex of stdin, no separators (od is on both macOS and Linux).
to_hex() { od -An -v -tx1 | tr -d ' \n'; }

signature=""
if [ -n "${FIRMWARE_SIGNING_KEY_FILE:-}" ]; then
  [ -r "$FIRMWARE_SIGNING_KEY_FILE" ] || { echo "cannot read FIRMWARE_SIGNING_KEY_FILE ($FIRMWARE_SIGNING_KEY_FILE)" >&2; exit 1; }
  work=$(mktemp -d "${TMPDIR:-/tmp}/x4fw.XXXXXX")
  trap 'rm -rf "$work"' EXIT

  printf '%s' "X4FW1|$stamp|$sha" > "$work/message"
  openssl dgst -sha256 -sign "$FIRMWARE_SIGNING_KEY_FILE" -out "$work/signature.der" "$work/message"

  openssl pkey -in "$FIRMWARE_SIGNING_KEY_FILE" -pubout -out "$work/public.pem"
  if ! openssl dgst -sha256 -verify "$work/public.pem" -signature "$work/signature.der" "$work/message" >/dev/null; then
    echo "signature does not verify with the signing key's public key" >&2
    exit 1
  fi

  public_hex=$(openssl pkey -pubin -in "$work/public.pem" -outform DER | to_hex)
  firmware_hex=$(grep -o '0x[0-9a-fA-F][0-9a-fA-F]' "$key_header" | sed 's/^0x//' | tr -d '\n' | tr 'A-F' 'a-f')
  if [ "$public_hex" != "$firmware_hex" ]; then
    echo "signing key does not match $key_header; readers would refuse this image" >&2
    exit 1
  fi

  signature=$(to_hex < "$work/signature.der")
  if [ -z "$signature" ] || [ "${#signature}" -gt 256 ]; then
    echo "unexpected signature length ${#signature}" >&2
    exit 1
  fi
elif [ "${ALLOW_UNSIGNED:-}" = "1" ]; then
  echo "warning: ALLOW_UNSIGNED=1, publishing an unsigned image; readers will refuse to install it" >&2
else
  echo "FIRMWARE_SIGNING_KEY_FILE is not set; refusing to publish an unsigned image (set ALLOW_UNSIGNED=1 to override)" >&2
  exit 1
fi

mkdir -p "$dir"
cp "$bin" "$dir/$file"
chmod 644 "$dir/$file"
if [ -n "$signature" ]; then
  printf '%s\n' "$signature" > "$dir/$file.sig.tmp"
  chmod 644 "$dir/$file.sig.tmp"
  mv "$dir/$file.sig.tmp" "$dir/$file.sig"
  manifest=$(printf '{"version":"%s","file":"%s","size":%s,"sha256":"%s","signature":"%s"}' \
    "$stamp" "$file" "$size" "$sha" "$signature")
else
  rm -f "$dir/$file.sig"
  manifest=$(printf '{"version":"%s","file":"%s","size":%s,"sha256":"%s"}' "$stamp" "$file" "$size" "$sha")
fi
# Write the manifest last and atomically: the app must never see a manifest that
# names an image that is not fully there yet.
printf '%s\n' "$manifest" > "$dir/firmware.json.tmp"
chmod 644 "$dir/firmware.json.tmp"
mv "$dir/firmware.json.tmp" "$dir/firmware.json"
cat "$dir/firmware.json"
