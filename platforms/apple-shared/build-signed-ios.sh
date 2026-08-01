#!/usr/bin/env bash
set -euo pipefail

: "${APPLE_TEAM_ID:?APPLE_TEAM_ID is required}"
: "${IOS_SIGN_IDENTITY:?IOS_SIGN_IDENTITY is required}"
: "${IOS_PROFILE_UUID:?IOS_PROFILE_UUID is required}"
: "${IOS_PROFILE_NAME:?IOS_PROFILE_NAME is required}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
client_dir="$(cd "$script_dir/../.." && pwd)"
version="${SONALIS_VERSION_NAME:-5.2.0}"
build_number="${SONALIS_BUILD_NUMBER:-1}"
build_root="${SONALIS_APPLE_BUILD_DIR:-$client_dir/build/ios-release}"
artifact_dir="${SONALIS_ARTIFACT_DIR:-$client_dir/platforms/artifacts/ios}"
project_dir="$client_dir/platforms/ios"

mkdir -p "$build_root" "$artifact_dir"
(cd "$script_dir" && ./prepare-apple-dependencies.sh && ./build-apple-opus.sh ios-device)
(cd "$project_dir" && xcodegen generate)

xcodebuild -project "$project_dir/SonalisIOS.xcodeproj" -scheme SonalisIOS \
  -configuration Release -destination 'generic/platform=iOS' \
  -archivePath "$build_root/Sonalis.xcarchive" \
  MARKETING_VERSION="$version" CURRENT_PROJECT_VERSION="$build_number" \
  DEVELOPMENT_TEAM="$APPLE_TEAM_ID" CODE_SIGN_STYLE=Manual \
  CODE_SIGN_IDENTITY="$IOS_SIGN_IDENTITY" PROVISIONING_PROFILE_SPECIFIER="$IOS_PROFILE_UUID" \
  LIBRARY_SEARCH_PATHS="$client_dir/core/vendor/opus/build-ios-device/Release" archive

export_options="$build_root/ExportOptions.plist"
cat > "$export_options" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>app-store-connect</string>
  <key>signingStyle</key><string>manual</string>
  <key>teamID</key><string>${APPLE_TEAM_ID}</string>
  <key>provisioningProfiles</key><dict>
    <key>tr.sonalis.mobile</key><string>${IOS_PROFILE_NAME}</string>
  </dict>
  <key>uploadSymbols</key><true/>
</dict></plist>
PLIST

xcodebuild -exportArchive -archivePath "$build_root/Sonalis.xcarchive" \
  -exportPath "$build_root/export" -exportOptionsPlist "$export_options"
ipa_source=$(find "$build_root/export" -maxdepth 1 -name '*.ipa' -print -quit)
if [[ -z "$ipa_source" ]]; then
  echo "Signed IPA was not produced" >&2
  exit 70
fi
ipa="$artifact_dir/Sonalis-$version-iOS.ipa"
cp "$ipa_source" "$ipa"
codesign --verify --deep --strict --verbose=2 "$build_root/Sonalis.xcarchive/Products/Applications/Sonalis.app"
shasum -a 256 "$ipa" > "$artifact_dir/SHA256SUMS.txt"
printf 'Signed iOS package: %s\n' "$ipa"
