#!/bin/bash
# Builds "Silicon Path.app" in this folder. Needs only the Xcode Command Line Tools.
set -euo pipefail
cd "$(dirname "$0")"

APP="Silicon Path.app"

echo "Compiling…"
swift build -c release --product SiliconPath 2>&1 | grep -v "ld: warning: search path"
BIN_DIR="$(swift build -c release --show-bin-path)"

if [ ! -f Resources/AppIcon.icns ]; then
  echo "Drawing icon…"
  swift scripts/make_icon.swift
fi

echo "Packaging…"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/SiliconPath" "$APP/Contents/MacOS/SiliconPath"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
codesign --force --sign - "$APP" >/dev/null 2>&1

echo "Done: $(pwd)/$APP"
