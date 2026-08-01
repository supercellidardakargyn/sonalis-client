#!/usr/bin/env bash
set -euo pipefail

client_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="${SONALIS_VERSION_NAME:-5.2.0}"
build_dir="${SONALIS_LINUX_BUILD_DIR:-$client_dir/build/linux-release}"
artifact_dir="${SONALIS_ARTIFACT_DIR:-$client_dir/platforms/artifacts/linux}"

cmake -S "$client_dir/platforms/linux" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCPACK_PACKAGE_VERSION="$version"
cmake --build "$build_dir" --parallel "${SONALIS_BUILD_JOBS:-2}"
ctest --test-dir "$build_dir" --output-on-failure
mkdir -p "$artifact_dir"
(cd "$build_dir" && cpack -G DEB && cpack -G TGZ)
if command -v rpmbuild >/dev/null 2>&1; then
  (cd "$build_dir" && cpack -G RPM)
fi
find "$build_dir" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' -o -name '*.tar.gz' \) \
  -exec cp -f {} "$artifact_dir/" \;
(cd "$artifact_dir" && sha256sum ./*.deb ./*.tar.gz ./*.rpm 2>/dev/null > SHA256SUMS || \
  sha256sum ./*.deb ./*.tar.gz > SHA256SUMS)

if [[ -n "${SONALIS_LINUX_GPG_KEY_ID:-}" ]]; then
  gpg_args=(--batch --yes --local-user "$SONALIS_LINUX_GPG_KEY_ID")
  if [[ -n "${SONALIS_LINUX_GPG_PASSPHRASE:-}" ]]; then
    gpg_args+=(--pinentry-mode loopback --passphrase "$SONALIS_LINUX_GPG_PASSPHRASE")
  fi
  gpg "${gpg_args[@]}" --detach-sign --armor "$artifact_dir/SHA256SUMS"
fi
printf 'Sonalis Linux paketleri: %s\n' "$artifact_dir"
