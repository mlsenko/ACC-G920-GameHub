#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/app/G920.app"
DIST="$ROOT/dist/G920.app"
OUT_DYLIB="$ROOT/app/libSDL2-2.0.0.dylib"
WINE_ROOT="$HOME/Library/Application Support/com.gamemac.www/wine-engine/containers/wine_installations"
SDK="$(xcrun --show-sdk-path)"
INSTALL=0
MAKE_DIST=0

for arg in "$@"; do
  case "$arg" in
    --install) INSTALL=1 ;;
    --dist) MAKE_DIST=1 ;;
    -h|--help)
      echo "Usage: $0 [--install] [--dist]"
      echo "  Builds G920.app (shim + menu bar helper)."
      echo "  --install  copy to ~/Applications/G920.app"
      echo "  --dist     copy ready app to dist/G920.app (for the repo)"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

pick_wine_lib() {
  if [[ -n "${WINE_LIB:-}" && -f "$WINE_LIB/libSDL2-2.0.0.dylib" ]]; then
    echo "$WINE_LIB"
    return
  fi
  local best=""
  local d
  for d in "$WINE_ROOT"/*/lib; do
    [[ -f "$d/libSDL2-2.0.0.dylib" || -f "$d/libSDL2-2.0.0.real.dylib" ]] || continue
    best="$d"
  done
  echo "$best"
}

WINE_LIB="$(pick_wine_lib)"
if [[ -z "$WINE_LIB" ]]; then
  echo "GameHub Wine SDL not found under:" >&2
  echo "  $WINE_ROOT" >&2
  echo "Install GameHub + ACC once, or set WINE_LIB to the wine …/lib directory." >&2
  exit 1
fi

REAL="$WINE_LIB/libSDL2-2.0.0.real.dylib"
CURRENT="$WINE_LIB/libSDL2-2.0.0.dylib"
echo "using Wine SDL in $WINE_LIB"

if [[ ! -f "$REAL" ]]; then
  if [[ -f "$CURRENT" ]] && strings "$CURRENT" | grep -q "g920 tune build"; then
    echo "Original SDL is missing and the current dylib is already the shim. Aborting." >&2
    exit 1
  fi
  if [[ ! -f "$CURRENT" ]]; then
    echo "Neither libSDL2-2.0.0.dylib nor .real.dylib found in $WINE_LIB" >&2
    exit 1
  fi
  cp "$CURRENT" "$REAL"
  install_name_tool -id @loader_path/libSDL2-2.0.0.real.dylib "$REAL"
  echo "Saved original SDL as libSDL2-2.0.0.real.dylib"
fi

clang -arch x86_64 -O2 -Wall -Wextra -dynamiclib \
  -isysroot "$SDK" \
  -I/opt/homebrew/include \
  -install_name @rpath/libSDL2-2.0.0.dylib \
  -Wl,-reexport_library,"$REAL" \
  -framework IOKit -framework CoreFoundation \
  -o "$OUT_DYLIB" \
  "$ROOT/shim/g920_sdl_ffb.c"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/app/Info.plist" "$APP/Contents/Info.plist"
cp "$ROOT/app/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
cp "$OUT_DYLIB" "$APP/Contents/Resources/libSDL2-2.0.0.dylib"
xcrun actool "$ROOT/app/Assets.xcassets" \
  --compile "$APP/Contents/Resources" \
  --app-icon AppIcon \
  --platform macosx \
  --minimum-deployment-target 14.0 \
  --output-partial-info-plist /tmp/G920-actool.plist >/dev/null
swiftc -O -parse-as-library -target arm64-apple-macos14.0 -sdk "$SDK" \
  -o "$APP/Contents/MacOS/G920" \
  "$ROOT/app/Sources/G920App.swift" \
  -framework SwiftUI -framework AppKit -framework IOKit -framework CoreFoundation
codesign -s - --force "$APP/Contents/Resources/libSDL2-2.0.0.dylib"
codesign -s - --force "$APP"
echo "built $APP"

if [[ "$INSTALL" -eq 1 ]]; then
  DEST="$HOME/Applications/G920.app"
  mkdir -p "$HOME/Applications"
  rm -rf "$DEST"
  ditto "$APP" "$DEST"
  codesign -s - --force "$DEST"
  echo "installed $DEST"
fi

if [[ "$MAKE_DIST" -eq 1 ]]; then
  mkdir -p "$ROOT/dist"
  rm -rf "$DIST"
  ditto "$APP" "$DIST"
  codesign -s - --force "$DIST"
  # Drop quarantine on the copy we ship so clones are less painful
  xattr -cr "$DIST" 2>/dev/null || true
  echo "dist $DIST"
fi
