
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
APPDIR="${APPDIR:-$BUILD_DIR/AppDir}"
OUTPUT="${OUTPUT:-$BUILD_DIR/DantesInferno-aarch64.AppImage}"

if [[ -z "${DANTES_BIN:-}" ]]; then
  if [[ -x "$BUILD_DIR/dantes_inferno" ]]; then
    DANTES_BIN="$BUILD_DIR/dantes_inferno"
  else
    DANTES_BIN="$ROOT/dantes_inferno"
  fi
fi
LAUNCHER_BIN="${LAUNCHER_BIN:-$BUILD_DIR/launcher-linux/dantes_inferno_launcher}"
RUNTIME_LIB="${RUNTIME_LIB:-$ROOT/librexruntime.so}"
if [[ -z "${GPU_LIB:-}" ]]; then
  if [[ -f "$BUILD_DIR/librexgpu-xenos.so" ]]; then
    GPU_LIB="$BUILD_DIR/librexgpu-xenos.so"
  else
    GPU_LIB="$ROOT/librexgpu-xenos.so"
  fi
fi

LINUXDEPLOY="${LINUXDEPLOY:-$(command -v linuxdeploy || true)}"
APPIMAGETOOL="${APPIMAGETOOL:-$(command -v appimagetool || true)}"

if [[ "$(uname -m)" != "aarch64" && "${ALLOW_CROSS_PACKAGE:-0}" != "1" ]]; then
  echo "This package must be assembled on AArch64." >&2
  exit 1
fi

for artifact in "$DANTES_BIN" "$LAUNCHER_BIN" "$RUNTIME_LIB" "$GPU_LIB"; do
  if [[ ! -f "$artifact" ]]; then
    echo "Missing ARM64 artifact: $artifact" >&2
    exit 1
  fi
done
if [[ -z "$LINUXDEPLOY" || ! -x "$LINUXDEPLOY" ]]; then
  echo "Set LINUXDEPLOY to an AArch64 linuxdeploy executable." >&2
  exit 1
fi
if [[ -z "$APPIMAGETOOL" || ! -x "$APPIMAGETOOL" ]]; then
  echo "Set APPIMAGETOOL to an AArch64 appimagetool executable." >&2
  exit 1
fi
if ! command -v linuxdeploy-plugin-qt >/dev/null 2>&1; then
  echo "linuxdeploy-plugin-qt must be available in PATH." >&2
  exit 1
fi
QMAKE="${QMAKE:-$(command -v qmake6 || true)}"
if [[ -z "$QMAKE" || ! -x "$QMAKE" ]]; then
  echo "Qt 6 qmake was not found; set QMAKE to qmake6." >&2
  exit 1
fi
export QMAKE

EXTRACT_XISO_BIN="${EXTRACT_XISO_BIN:-}"
EXTRACT_XISO_LICENSE=""
if [[ -z "$EXTRACT_XISO_BIN" ]]; then
  EXTRACT_SOURCE="$BUILD_DIR/appimage-tools/extract-xiso"
  EXTRACT_BUILD="$EXTRACT_SOURCE/build-arm64"
  EXTRACT_COMMIT="b72e5b60d598ec6df80534cda19cdcd4361aa18c"
  if [[ ! -d "$EXTRACT_SOURCE/.git" ]]; then
    mkdir -p "$(dirname "$EXTRACT_SOURCE")"
    git clone https://github.com/XboxDev/extract-xiso.git "$EXTRACT_SOURCE"
  fi
  git -C "$EXTRACT_SOURCE" fetch --depth 1 origin "$EXTRACT_COMMIT"
  git -C "$EXTRACT_SOURCE" checkout --detach "$EXTRACT_COMMIT"
  cmake -S "$EXTRACT_SOURCE" -B "$EXTRACT_BUILD" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "$EXTRACT_BUILD" --parallel
  EXTRACT_XISO_BIN="$EXTRACT_BUILD/extract-xiso"
  EXTRACT_XISO_LICENSE="$EXTRACT_SOURCE/LICENSE.TXT"
fi
if [[ ! -x "$EXTRACT_XISO_BIN" ]]; then
  echo "extract-xiso ARM64 executable not found: $EXTRACT_XISO_BIN" >&2
  exit 1
fi

rm -rf "$APPDIR"
install -d \
  "$APPDIR/usr/bin" \
  "$APPDIR/usr/lib" \
  "$APPDIR/usr/libexec" \
  "$APPDIR/usr/share/applications" \
  "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
  "$APPDIR/usr/share/licenses/extract-xiso"

install -m755 "$DANTES_BIN" "$APPDIR/usr/bin/dantes_inferno"
install -m755 "$LAUNCHER_BIN" "$APPDIR/usr/bin/dantes_inferno_launcher"
install -m755 "$RUNTIME_LIB" "$APPDIR/usr/lib/librexruntime.so"
install -m755 "$GPU_LIB" "$APPDIR/usr/lib/librexgpu-xenos.so"
install -m755 "$GPU_LIB" "$APPDIR/usr/bin/librexgpu-xenos.so"
install -m755 "$EXTRACT_XISO_BIN" "$APPDIR/usr/libexec/extract-xiso"
install -m644 "$ROOT/packaging/appimage/dantes-inferno.desktop" \
  "$APPDIR/usr/share/applications/dantes-inferno.desktop"
install -m644 "$ROOT/packaging/appimage/dantes-inferno.svg" \
  "$APPDIR/usr/share/icons/hicolor/scalable/apps/dantes-inferno.svg"
if [[ -n "$EXTRACT_XISO_LICENSE" ]]; then
  install -m644 "$EXTRACT_XISO_LICENSE" \
    "$APPDIR/usr/share/licenses/extract-xiso/LICENSE.TXT"
fi

"$LINUXDEPLOY" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/dantes_inferno_launcher" \
  --executable "$APPDIR/usr/bin/dantes_inferno" \
  --desktop-file "$APPDIR/usr/share/applications/dantes-inferno.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/dantes-inferno.svg" \
  --plugin qt \
  --custom-apprun "$ROOT/packaging/appimage/AppRun"






install -d "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/dantes-platform.sh" <<'HOOK'



export QT_QPA_PLATFORM=xcb
HOOK


if [[ -f "$APPDIR/AppRun" ]] && ! grep -q "dantes-platform" "$APPDIR/AppRun"; then
  sed -i '/^exec /i source "$this_dir"/apprun-hooks/"dantes-platform.sh"' "$APPDIR/AppRun"
fi



WAYLAND_PLUGIN_SRC_1="/usr/lib/qt6/plugins/platforms/libqwayland.so"
WAYLAND_PLUGIN_SRC_2="/usr/lib/aarch64-linux-gnu/qt6/plugins/platforms/libqwayland.so"
WAYLAND_PLUGIN_SRC=""
if [[ -f "$WAYLAND_PLUGIN_SRC_2" ]]; then
  WAYLAND_PLUGIN_SRC="$WAYLAND_PLUGIN_SRC_2"
elif [[ -f "$WAYLAND_PLUGIN_SRC_1" ]]; then
  WAYLAND_PLUGIN_SRC="$WAYLAND_PLUGIN_SRC_1"
fi

if [[ -n "$WAYLAND_PLUGIN_SRC" && -f "$WAYLAND_PLUGIN_SRC" ]]; then
  install -m755 -d "$APPDIR/usr/plugins/platforms"
  install -m644 "$WAYLAND_PLUGIN_SRC" "$APPDIR/usr/plugins/platforms/libqwayland.so"
  for lib in libwayland-client.so.0; do
    if [[ ! -f "$APPDIR/usr/lib/$lib" ]]; then
      if [[ -f "/usr/lib/aarch64-linux-gnu/$lib" ]]; then
        install -m644 "/usr/lib/aarch64-linux-gnu/$lib" "$APPDIR/usr/lib/$lib"
      elif [[ -f "/usr/lib/$lib" ]]; then
        install -m644 "/usr/lib/$lib" "$APPDIR/usr/lib/$lib"
      fi
    fi
  done
fi

# RecompLauncher reads this from the AppImage root; every release ships it.
install -m644 "$ROOT/recomplauncher.json" "$APPDIR/recomplauncher.json"

ARCH=aarch64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"
echo "Created: $OUTPUT"
