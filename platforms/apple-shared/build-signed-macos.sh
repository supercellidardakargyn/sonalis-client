#!/usr/bin/env bash
set -euo pipefail

: "${APPLE_TEAM_ID:?APPLE_TEAM_ID is required}"
: "${MACOS_SIGN_IDENTITY:?MACOS_SIGN_IDENTITY is required}"
: "${APPLE_NOTARY_KEY_PATH:?APPLE_NOTARY_KEY_PATH is required}"
: "${APPLE_NOTARY_KEY_ID:?APPLE_NOTARY_KEY_ID is required}"
: "${APPLE_NOTARY_ISSUER_ID:?APPLE_NOTARY_ISSUER_ID is required}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
client_dir="$(cd "$script_dir/../.." && pwd)"
version="${SONALIS_VERSION_NAME:-5.2.0}"
build_root="${SONALIS_APPLE_BUILD_DIR:-$client_dir/build/apple-release}"
artifact_dir="${SONALIS_ARTIFACT_DIR:-$client_dir/platforms/artifacts/macos}"
project_dir="$client_dir/platforms/macos"

mkdir -p "$build_root" "$artifact_dir"
(cd "$script_dir" && ./prepare-apple-dependencies.sh && ./build-apple-opus.sh macos)
(cd "$project_dir" && xcodegen generate)
xcodebuild -project "$project_dir/SonalisMac.xcodeproj" -scheme SonalisMac \
  -configuration Release -derivedDataPath "$build_root/derived" \
  MARKETING_VERSION="$version" CURRENT_PROJECT_VERSION="${SONALIS_BUILD_NUMBER:-1}" \
  CODE_SIGNING_ALLOWED=NO build

app_source="$build_root/derived/Build/Products/Release/Sonalis.app"
app_stage="$build_root/Sonalis.app"
rm -rf "$app_stage"
ditto "$app_source" "$app_stage"
codesign --force --deep --options runtime --timestamp \
  --entitlements "$project_dir/App/Sonalis.entitlements" \
  --sign "$MACOS_SIGN_IDENTITY" "$app_stage"
codesign --verify --deep --strict --verbose=2 "$app_stage"

dmg="$artifact_dir/Sonalis-$version-macOS.dmg"
dmg_root="$build_root/dmg-root"
rm -f "$dmg"
rm -rf "$dmg_root"
mkdir -p "$dmg_root"
ditto "$app_stage" "$dmg_root/Sonalis.app"
ln -s /Applications "$dmg_root/Applications"
hdiutil create -volname "Sonalis" -srcfolder "$dmg_root" -ov -format UDZO "$dmg"
codesign --force --timestamp --sign "$MACOS_SIGN_IDENTITY" "$dmg"
xcrun notarytool submit "$dmg" --key "$APPLE_NOTARY_KEY_PATH" \
  --key-id "$APPLE_NOTARY_KEY_ID" --issuer "$APPLE_NOTARY_ISSUER_ID" --wait
xcrun stapler staple "$dmg"
xcrun stapler validate "$dmg"
codesign --verify --strict --verbose=2 "$dmg"
shasum -a 256 "$dmg" > "$artifact_dir/SHA256SUMS.txt"
printf 'Signed and notarized macOS package: %s\n' "$dmg"
