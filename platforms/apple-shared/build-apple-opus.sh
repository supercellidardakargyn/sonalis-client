#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CLIENT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE="$CLIENT_DIR/core/vendor/opus"
TARGET=${1:-}

if [ ! -f "$SOURCE/CMakeLists.txt" ]; then
  echo "Run prepare-apple-dependencies.sh first" >&2
  exit 70
fi

case "$TARGET" in
  macos)
    BUILD="$SOURCE/build-macos"
    cmake -S "$SOURCE" -B "$BUILD" -G Xcode \
      -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF \
      -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
    ;;
  ios-simulator)
    BUILD="$SOURCE/build-ios-sim"
    cmake -S "$SOURCE" -B "$BUILD" -G Xcode \
      -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF \
      -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
    ;;
  ios-device)
    BUILD="$SOURCE/build-ios-device"
    cmake -S "$SOURCE" -B "$BUILD" -G Xcode \
      -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF \
      -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
    ;;
  *)
    echo "usage: $0 macos|ios-simulator|ios-device" >&2
    exit 64
    ;;
esac
cmake --build "$BUILD" --config Release --target opus
