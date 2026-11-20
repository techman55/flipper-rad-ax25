#!/bin/bash
# Build FlipperTNC.app (Release, universal arm64 + x86_64) and optionally install it.
#
#   mac/build-app.sh              -> mac/dist/FlipperTNC.app
#   mac/build-app.sh --install    -> also copies it to /Applications
#
# Needs Xcode 15+ (not just the Command Line Tools). If xcodebuild complains, run:
#   sudo xcode-select -s /Applications/Xcode.app
set -euo pipefail
cd "$(dirname "$0")"

xcodebuild \
  -project FlipperTNC.xcodeproj \
  -scheme FlipperTNC \
  -configuration Release \
  -destination 'generic/platform=macOS' \
  -derivedDataPath build \
  -quiet \
  build

APP="build/Build/Products/Release/FlipperTNC.app"
mkdir -p dist
rm -rf dist/FlipperTNC.app
ditto "$APP" dist/FlipperTNC.app
echo "Built: $(pwd)/dist/FlipperTNC.app"

if [[ "${1:-}" == "--install" ]]; then
  osascript -e 'tell application "Flipper TNC" to quit' >/dev/null 2>&1 || true
  rm -rf /Applications/FlipperTNC.app
  ditto dist/FlipperTNC.app /Applications/FlipperTNC.app
  echo "Installed: /Applications/FlipperTNC.app"
fi
