#!/usr/bin/env bash
set -euo pipefail

: "${APPLE_CERTIFICATE_P12_PATH:?APPLE_CERTIFICATE_P12_PATH is required}"
: "${APPLE_CERTIFICATE_PASSWORD:?APPLE_CERTIFICATE_PASSWORD is required}"

keychain_path="${APPLE_KEYCHAIN_PATH:-${RUNNER_TEMP:-/tmp}/sonalis-signing.keychain-db}"
keychain_password="${APPLE_KEYCHAIN_PASSWORD:-$(openssl rand -base64 32)}"

security create-keychain -p "$keychain_password" "$keychain_path"
security set-keychain-settings -lut 21600 "$keychain_path"
security unlock-keychain -p "$keychain_password" "$keychain_path"
security import "$APPLE_CERTIFICATE_P12_PATH" -k "$keychain_path" \
  -P "$APPLE_CERTIFICATE_PASSWORD" -T /usr/bin/codesign -T /usr/bin/security
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
  -k "$keychain_password" "$keychain_path"
security list-keychains -d user -s "$keychain_path" login.keychain-db

if [[ -n "${GITHUB_ENV:-}" ]]; then
  printf 'APPLE_KEYCHAIN_PATH=%s\n' "$keychain_path" >> "$GITHUB_ENV"
  printf 'APPLE_KEYCHAIN_PASSWORD=%s\n' "$keychain_password" >> "$GITHUB_ENV"
else
  printf 'APPLE_KEYCHAIN_PATH=%s\n' "$keychain_path"
fi
