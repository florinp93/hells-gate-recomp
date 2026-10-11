#!/usr/bin/env bash
set -euo pipefail

# Build dependencies for the Linux port on Ubuntu: Clang 22, CMake 3.25+,
# Ninja, PowerShell 7 (SDK patch scripts) and the X11 / Wayland / audio /
# Vulkan / GTK development libraries.

if [[ ! -r /etc/os-release ]]; then
  echo "ERROR: cannot detect the distribution."
  exit 1
fi

. /etc/os-release

CODENAME="${VERSION_CODENAME:-}"
if [[ -z "$CODENAME" ]]; then
  echo "ERROR: cannot detect VERSION_CODENAME."
  exit 1
fi

echo "==> Ubuntu: ${PRETTY_NAME:-unknown} (${CODENAME})"

sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  git \
  python3 \
  wget \
  gnupg \
  ca-certificates \
  apt-transport-https \
  software-properties-common

sudo apt install -y \
  libgtk-3-dev \
  libxss-dev \
  libx11-dev \
  libxext-dev \
  libxrandr-dev \
  libxcursor-dev \
  libxi-dev \
  libxinerama-dev \
  libxtst-dev \
  libx11-xcb-dev \
  libxcb1-dev \
  libxkbcommon-dev \
  libwayland-dev \
  wayland-protocols \
  libasound2-dev \
  libpulse-dev \
  libvulkan-dev

# PowerShell 7 from Microsoft's package repository.
if ! command -v pwsh >/dev/null 2>&1; then
  echo "==> Installing PowerShell 7..."
  wget -q "https://packages.microsoft.com/config/ubuntu/${VERSION_ID}/packages-microsoft-prod.deb" \
    -O packages-microsoft-prod.deb
  sudo dpkg -i packages-microsoft-prod.deb
  rm -f packages-microsoft-prod.deb
  sudo apt update
  sudo apt install -y powershell
fi

# ReXGlue needs CMake 3.25+; Ubuntu 22.04 ships an older one.
CMAKE_VERSION="$(cmake --version | awk 'NR==1 {print $3}')"
if dpkg --compare-versions "$CMAKE_VERSION" lt "3.25"; then
  echo "==> CMake ${CMAKE_VERSION} is older than 3.25; installing Kitware's CMake..."
  wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
    | gpg --dearmor \
    | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
  echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${CODENAME} main" \
    | sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
  sudo apt update
  sudo apt install -y cmake
fi

# Clang 22 from apt.llvm.org.
if ! command -v clang-22 >/dev/null 2>&1; then
  echo "==> Installing Clang 22 from apt.llvm.org..."
  TMP_LLVM="$(mktemp)"
  wget -q https://apt.llvm.org/llvm.sh -O "$TMP_LLVM"
  chmod +x "$TMP_LLVM"
  sudo "$TMP_LLVM" 22
  rm -f "$TMP_LLVM"
fi

sudo apt install -y clang-22 lld-22 libc++-22-dev libc++abi-22-dev

sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-22 220
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-22 220
if [ -f /usr/bin/ld.lld-22 ]; then
  sudo update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-22 220
fi

# Ubuntu 22.04: libstdc++ 13 for ReXGlue's C++23 code (Clang 22 still compiles).
if [[ "$CODENAME" == "jammy" ]]; then
  echo "==> Jammy: installing libstdc++ 13..."
  sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
  sudo apt update
  sudo apt install -y g++-13 libstdc++-13-dev
fi

echo
echo "===== Toolchain ====="
clang --version | head -n 1
clang++ --version | head -n 1
cmake --version | head -n 1
ninja --version
pwsh --version
echo
echo "Dependencies installed. Configure CMake with /usr/bin/clang-22 and /usr/bin/clang++-22."
