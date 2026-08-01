#!/usr/bin/env bash
set -euo pipefail

: "${IOS_PROVISIONING_PROFILE_PATH:?IOS_PROVISIONING_PROFILE_PATH is required}"
profile_plist="${RUNNER_TEMP:-/tmp}/sonalis-profile.plist"
security cms -D -i "$IOS_PROVISIONING_PROFILE_PATH" > "$profile_plist"
profile_uuid=$(/usr/libexec/PlistBuddy -c 'Print :UUID' "$profile_plist")
profile_name=$(/usr/libexec/PlistBuddy -c 'Print :Name' "$profile_plist")
profile_team=$(/usr/libexec/PlistBuddy -c 'Print :TeamIdentifier:0' "$profile_plist")
profile_dir="$HOME/Library/MobileDevice/Provisioning Profiles"
mkdir -p "$profile_dir"
cp "$IOS_PROVISIONING_PROFILE_PATH" "$profile_dir/$profile_uuid.mobileprovision"

if [[ -n "${GITHUB_ENV:-}" ]]; then
  printf 'IOS_PROFILE_UUID=%s\n' "$profile_uuid" >> "$GITHUB_ENV"
  printf 'IOS_PROFILE_NAME=%s\n' "$profile_name" >> "$GITHUB_ENV"
  printf 'IOS_PROFILE_TEAM=%s\n' "$profile_team" >> "$GITHUB_ENV"
else
  printf 'IOS_PROFILE_UUID=%s\nIOS_PROFILE_NAME=%s\nIOS_PROFILE_TEAM=%s\n' \
    "$profile_uuid" "$profile_name" "$profile_team"
fi
