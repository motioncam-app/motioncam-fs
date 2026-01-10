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
  if [[ -L "$target" ]]; then
    rm -rf "$target"
  fi
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
      cp -R -L "$candidate" "$APP_PATH/Contents/Frameworks/"
      return 0
    fi
  done
  return 1
}

copy_file_force() {
  local src="$1"
  local dest="$2"
  if [[ -e "$dest" ]]; then
    chmod u+w "$dest" 2>/dev/null || true
    rm -f "$dest"
  fi
  cp -L "$src" "$dest"
}

copy_dylib() {
  local rel="$1"
  local name
  name="$(basename "$rel")"
  local target="$APP_PATH/Contents/Frameworks/$name"
  if [[ -f "$target" ]]; then
    return 0
  fi
  local candidates=(
    "$QT_LIB_DIR/$rel"
    "$QT_LIB_DIR/$name"
    "/usr/local/lib/$name"
    "/usr/local/opt/qt/lib/$name"
    "/usr/local/opt/qt@6/lib/$name"
    "/opt/homebrew/lib/$name"
    "/opt/homebrew/opt/qt/lib/$name"
    "/opt/homebrew/opt/qt@6/lib/$name"
  )
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      copy_file_force "$candidate" "$target"
      install_name_tool -id "@rpath/$name" "$target"
      return 0
    fi
  done
  return 1
}

list_missing_rpath() {
  find "$APP_PATH/Contents" -type f \( -perm -111 -o -name "*.dylib" -o -name "*.so" \) -print0 | \
    while IFS= read -r -d '' f; do
      otool -L "$f" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in
          @rpath/*)
            rel="${dep#@rpath/}"
            if ! find "$APP_PATH/Contents" -path "*/$rel" -print -quit | grep -q .; then
              echo "$rel"
            fi
            ;;
        esac
      done
    done | sort -u
}

copy_missing_rpath() {
  local missing
  missing="$(list_missing_rpath || true)"
  if [[ -z "$missing" ]]; then
    return 0
  fi
  while IFS= read -r rel; do
    if [[ "$rel" == *.framework/* ]]; then
      local fw="${rel%%.framework*}"
      copy_framework "$fw" || true
    elif [[ "$rel" == *.dylib ]]; then
      copy_dylib "$rel" || true
    fi
  done <<< "$missing"
}

copy_framework "QtDBus"
copy_framework "QtSvg"
copy_framework "QtVirtualKeyboard"
copy_framework "QtVirtualKeyboardQml"
copy_missing_rpath

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
  copy_file_force "$FUSE_LIB" "$APP_PATH/Contents/Frameworks/libfuse.2.dylib"
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
  copy_file_force "$BROTLI_COMMON" "$APP_PATH/Contents/Frameworks/libbrotlicommon.1.dylib"
  install_name_tool -id "@rpath/libbrotlicommon.1.dylib" "$APP_PATH/Contents/Frameworks/libbrotlicommon.1.dylib"
fi

LEFTOVER_MISSING="$(list_missing_rpath || true)"
if [[ -n "$LEFTOVER_MISSING" ]]; then
  echo "Warning: missing @rpath entries remain:" >&2
  echo "$LEFTOVER_MISSING" >&2
fi

codesign --force --deep --sign - "$APP_PATH"
echo "Packaged: $APP_PATH"
