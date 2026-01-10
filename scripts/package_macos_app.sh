#!/usr/bin/env bash
set -euo pipefail

APP_PATH="${1:-}"

if [[ -z "$APP_PATH" ]]; then
  echo "Usage: $0 /path/to/MotionCamFuse.app" >&2
  exit 1
fi

if [[ ! -d "$APP_PATH" ]]; then
  echo "App not found: $APP_PATH" >&2
  exit 1
fi

QT_DEPLOY="$(command -v macdeployqt || true)"
if [[ -z "$QT_DEPLOY" ]]; then
  QT_DEPLOY="$(brew --prefix qt@6 2>/dev/null)/bin/macdeployqt"
fi
if [[ ! -x "$QT_DEPLOY" ]]; then
  QT_DEPLOY="$(brew --prefix)/opt/qtbase/bin/macdeployqt"
fi
if [[ ! -x "$QT_DEPLOY" ]]; then
  echo "macdeployqt not found. Install Qt or ensure macdeployqt is in PATH." >&2
  exit 1
fi

"$QT_DEPLOY" "$APP_PATH" -verbose=2 || true

QT_PREFIX="$(cd "$(dirname "$QT_DEPLOY")/.." && pwd)"
QT_DBUS="${QT_PREFIX}/lib/QtDBus.framework"
if [[ -d "$QT_DBUS" && ! -d "$APP_PATH/Contents/Frameworks/QtDBus.framework" ]]; then
  cp -R "$QT_DBUS" "$APP_PATH/Contents/Frameworks/"
fi

BIN="$APP_PATH/Contents/MacOS/MotionCamFuse"
if [[ -f "$BIN" ]]; then
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN" 2>/dev/null || true
fi

PLUGINS_DIR="$APP_PATH/Contents/PlugIns"
if [[ -d "$PLUGINS_DIR" ]]; then
  find "$PLUGINS_DIR" -type f -name "*.dylib" -print0 | \
    xargs -0 -n1 -I{} sh -c 'install_name_tool -add_rpath "@loader_path/../../Frameworks" "$1" 2>/dev/null || true' _ {}
fi

FUSE_LIB=""
for candidate in "$(brew --prefix)/lib/libfuse.2.dylib" "/usr/local/lib/libfuse.2.dylib" "/opt/homebrew/lib/libfuse.2.dylib"; do
  if [[ -f "$candidate" ]]; then
    FUSE_LIB="$candidate"
    break
  fi
done
if [[ -n "$FUSE_LIB" ]]; then
  mkdir -p "$APP_PATH/Contents/Frameworks"
  cp "$FUSE_LIB" "$APP_PATH/Contents/Frameworks/"
  install_name_tool -id "@rpath/libfuse.2.dylib" "$APP_PATH/Contents/Frameworks/libfuse.2.dylib"
fi

codesign --force --deep --sign - "$APP_PATH"
echo "Packaged: $APP_PATH"
