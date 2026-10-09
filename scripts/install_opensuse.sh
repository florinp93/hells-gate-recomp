#!/usr/bin/env bash
set -euo pipefail

# Build dependencies for hells-gate-recomp + ReXGlue on openSUSE Tumbleweed.
# openSUSE counterpart of install_dantes_min.sh (Ubuntu). PowerShell is not
# needed: use ./setup.sh instead of setup.ps1.

if [[ ! -r /etc/os-release ]]; then
  echo "ERROR: cannot detect the distribution (/etc/os-release missing)." >&2
  exit 1
fi

# shellcheck source=/dev/null
. /etc/os-release

case "${ID:-}" in
  opensuse-tumbleweed|opensuse-slowroll) ;;
  *)
    echo "ERROR: this script targets openSUSE Tumbleweed; found '${PRETTY_NAME:-unknown}'." >&2
    echo "  Leap's packaged Clang and CMake are too old for ReXGlue." >&2
    exit 1
    ;;
esac

echo "==> ${PRETTY_NAME}"

# Use a graphical password prompt when an askpass helper is configured.
SUDO=(sudo)
if [[ -n "${SUDO_ASKPASS:-}" ]]; then
  SUDO=(sudo -A -p "hells-gate-recomp: install build dependencies with zypper")
fi

packages=(
  # Toolchain used by the build.
  clang
  lld
  gcc-c++
  cmake
  ninja
  pkgconf
  git
  python3
  # X11/Wayland, audio, Vulkan and GTK development files.
  gtk3-devel
  libXss-devel
  libX11-devel
  libXext-devel
  libXrandr-devel
  libXcursor-devel
  libXi-devel
  libXinerama-devel
  libXtst-devel
  libxcb-devel
  libxkbcommon-devel
  wayland-devel
  wayland-protocols-devel
  alsa-devel
  libpulse-devel
  vulkan-devel
  # FidelityFX (on by default): glslc for the SDK's shader check, and Wine to
  # run FidelityFX_SC.exe, which generates the shader permutations.
  shaderc
  wine
)

"${SUDO[@]}" zypper --non-interactive install --no-recommends "${packages[@]}"

# ReXGlue needs Clang 18+ and CMake 3.25+.
clang_major="$(clang -dumpversion | cut -d. -f1)"
if (( clang_major < 18 )); then
  echo "ERROR: Clang $clang_major found; ReXGlue needs 18 or newer." >&2
  exit 1
fi
cmake_version="$(cmake --version | awk 'NR==1 {print $3}')"
if [[ "$(printf '%s\n' 3.25 "$cmake_version" | sort -V | head -n1)" != 3.25 ]]; then
  echo "ERROR: CMake $cmake_version found; ReXGlue needs 3.25 or newer." >&2
  exit 1
fi

echo
echo "===== Toolchain ====="
clang --version | head -n 1
cmake --version | head -n 1
echo "ninja $(ninja --version)"
echo
echo "Dependencies ready. Next: ./setup.sh"
