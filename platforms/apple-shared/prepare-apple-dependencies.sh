#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CLIENT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MONO_DIR="$CLIENT_DIR/core/vendor/monocypher"
OPUS_DIR="$CLIENT_DIR/core/vendor/opus"
MONO_ARCHIVE="${TMPDIR:-/tmp}/sonalis-monocypher-4.0.2.tar.gz"
OPUS_ARCHIVE="${TMPDIR:-/tmp}/sonalis-opus-1.5.2.tar.gz"

if [ ! -f "$MONO_DIR/src/monocypher.c" ] || [ ! -f "$MONO_DIR/src/optional/monocypher-ed25519.c" ]; then
  curl --fail --silent --show-error --location --proto '=https' --tlsv1.2 \
    'https://monocypher.org/download/monocypher-4.0.2.tar.gz' --output "$MONO_ARCHIVE"
  ACTUAL=$(shasum -a 256 "$MONO_ARCHIVE" | awk '{print $1}')
  if [ "$ACTUAL" != '38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c' ]; then
    rm -f "$MONO_ARCHIVE"
    echo "Monocypher SHA-256 verification failed" >&2
    exit 70
  fi
  TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sonalis-monocypher.XXXXXX")
  tar -xzf "$MONO_ARCHIVE" -C "$TEMP_DIR"
  SOURCE=$(find "$TEMP_DIR" -type f -path '*/src/monocypher.c' -print -quit)
  if [ -z "$SOURCE" ]; then
    rm -rf "$TEMP_DIR" "$MONO_ARCHIVE"
    echo "Monocypher source directory was not found" >&2
    exit 71
  fi
  ROOT=$(CDPATH= cd -- "$(dirname -- "$SOURCE")/.." && pwd)
  mkdir -p "$MONO_DIR/src/optional"
  cp "$ROOT/src/monocypher.c" "$ROOT/src/monocypher.h" "$MONO_DIR/src/"
  cp "$ROOT/src/optional/monocypher-ed25519.c" "$ROOT/src/optional/monocypher-ed25519.h" \
    "$MONO_DIR/src/optional/"
  rm -rf "$TEMP_DIR" "$MONO_ARCHIVE"
fi

if [ ! -f "$OPUS_DIR/include/opus.h" ]; then
  curl --fail --silent --show-error --location --proto '=https' --tlsv1.2 \
    'https://github.com/xiph/opus/archive/refs/tags/v1.5.2.tar.gz' --output "$OPUS_ARCHIVE"
  ACTUAL=$(shasum -a 256 "$OPUS_ARCHIVE" | awk '{print $1}')
  if [ "$ACTUAL" != '9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404' ]; then
    rm -f "$OPUS_ARCHIVE"
    echo "Opus SHA-256 verification failed" >&2
    exit 72
  fi
  TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sonalis-opus.XXXXXX")
  tar -xzf "$OPUS_ARCHIVE" -C "$TEMP_DIR"
  SOURCE=$(find "$TEMP_DIR" -type f -path '*/include/opus.h' -print -quit)
  if [ -z "$SOURCE" ]; then
    rm -rf "$TEMP_DIR" "$OPUS_ARCHIVE"
    echo "Opus source directory was not found" >&2
    exit 73
  fi
  ROOT=$(CDPATH= cd -- "$(dirname -- "$SOURCE")/.." && pwd)
  rm -rf "$OPUS_DIR"
  mkdir -p "$(dirname -- "$OPUS_DIR")"
  cp -R "$ROOT" "$OPUS_DIR"
  rm -rf "$TEMP_DIR" "$OPUS_ARCHIVE"
fi
