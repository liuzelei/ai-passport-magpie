#!/bin/bash
set -euo pipefail

source_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(cd -- "$source_dir/../.." && pwd)"
output_dir="$project_dir/build/magpie-ble"
bundle="$output_dir/Magpie Passport BLE.app"
executable="$bundle/Contents/MacOS/magpie-passport-ble"

if [[ -x "$executable" && "$executable" -nt "$source_dir/MagpieBLE.swift" &&
      "$executable" -nt "$source_dir/Info.plist" && "$executable" -nt "$source_dir/build.sh" ]] &&
   /usr/bin/codesign --verify --strict "$bundle" 2>/dev/null; then
    printf '%s\n' "$executable"
    exit 0
fi

mkdir -p "$output_dir"
scratch_dir="$(mktemp -d "$output_dir/.compile.XXXXXXXX")"
trap 'rm -rf -- "$scratch_dir"' EXIT
sdk="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
arch="$(uname -m)"
/usr/bin/xcrun swiftc -swift-version 5 -O -sdk "$sdk" -target "$arch-apple-macos13.0" \
    -framework CoreBluetooth \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$source_dir/Info.plist" \
    "$source_dir/MagpieBLE.swift" -o "$scratch_dir/magpie-passport-ble"

mkdir -p "$bundle/Contents/MacOS"
cp "$source_dir/Info.plist" "$bundle/Contents/Info.plist"
mv "$scratch_dir/magpie-passport-ble" "$executable"
/usr/bin/codesign --force --sign - "$bundle" >&2
/usr/bin/codesign --verify --strict "$bundle"
printf '%s\n' "$executable"
