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
QT_LIB_DIR="${QT_PREFIX}/lib"

copy_framework() {
  local name="$1"
  local target="$APP_PATH/Contents/Frameworks/${name}.framework"
  if [[ -d "$target" ]]; then
    return 0
  fi
  local candidates=(
    "$QT_LIB_DIR/${name}.framework"
    "/usr/local/opt/qtbase/lib/${name}.framework"
    "/usr/local/opt/qtvirtualkeyboard/lib/${name}.framework"
    "/usr/local/opt/qtdeclarative/lib/${name}.framework"
    "/opt/homebrew/opt/qt@6/lib/${name}.framework"
    "/opt/homebrew/opt/qtbase/lib/${name}.framework"
    "/opt/homebrew/opt/qtvirtualkeyboard/lib/${name}.framework"
    "/opt/homebrew/opt/qtdeclarative/lib/${name}.framework"
  )
  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate" ]]; then
      cp -R "$candidate" "$APP_PATH/Contents/Frameworks/"
      return 0
    fi
  done
  return 1
}

copy_framework "QtDBus"
copy_framework "QtSvg"
copy_framework "QtVirtualKeyboard"
copy_framework "QtVirtualKeyboardQml"

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

BROTLI_COMMON=""
for candidate in "$(brew --prefix)/lib/libbrotlicommon.1.dylib" "/usr/local/opt/brotli/lib/libbrotlicommon.1.dylib" "/opt/homebrew/opt/brotli/lib/libbrotlicommon.1.dylib"; do
  if [[ -f "$candidate" ]]; then
    BROTLI_COMMON="$candidate"
    break
  fi
done
if [[ -n "$BROTLI_COMMON" ]]; then
  mkdir -p "$APP_PATH/Contents/Frameworks"
  cp "$BROTLI_COMMON" "$APP_PATH/Contents/Frameworks/"
  install_name_tool -id "@rpath/libbrotlicommon.1.dylib" "$APP_PATH/Contents/Frameworks/libbrotlicommon.1.dylib"
fi

codesign --force --deep --sign - "$APP_PATH"
echo "Packaged: $APP_PATH"
