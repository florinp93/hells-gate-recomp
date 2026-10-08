#!/usr/bin/env bash
# Bash equivalent of setup.ps1 for Linux, so PowerShell is not required.
# Clones the ReXGlue SDK, initialises its submodules and applies the local
# SDK (and, if present, DiligentCore) patches. Safe to run more than once.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk_dir="$root/thirdparty/rexglue-sdk"
tag="v0.10.0"  # keep in sync with setup.ps1

echo "== Dante's Inferno - ReXGlue project setup =="

for tool in git cmake ninja clang; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "MISSING: $tool not found on PATH." >&2
    echo "  ReXGlue requires: Clang 18+, CMake 3.25+, Ninja." >&2
    exit 1
  fi
done
echo "Prerequisites OK."

if [[ -d "$sdk_dir/.git" ]]; then
  echo "SDK already cloned at $sdk_dir"
else
  echo "Cloning ReXGlue SDK ($tag) into thirdparty/rexglue-sdk ..."
  git clone --branch "$tag" --depth 1 https://github.com/rexglue/rexglue-sdk.git "$sdk_dir"
fi

echo "Initializing SDK submodules (this can take a while) ..."
git -C "$sdk_dir" submodule update --init --recursive --depth 1

# Same logic as patches/apply_*_patches.ps1: skip if already applied,
# otherwise apply, falling back to a 3-way merge.
apply_patch() {
  local name="$1" dir="$2" patch="$3"
  echo "Applying $name patches to $dir ..."
  if git -C "$dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "$name patches already applied."
  elif git -C "$dir" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$dir" apply "$patch"
    echo "$name patches applied successfully."
  else
    echo "Attempting to apply with --3way..."
    git -C "$dir" apply --3way "$patch"
    echo "$name patches applied successfully."
  fi
}

apply_patch "SDK" "$sdk_dir" "$root/patches/sdk/rexglue-sdk-v0.10.0.patch"

# The native renderer (DANTESINFERNO_NATIVE_RENDERER, ON by default) needs
# DiligentCore; this is the clone command CMakeLists.txt asks for.
diligent_dir="$root/thirdparty/diligent-core"
if [[ -d "$diligent_dir/.git" ]]; then
  echo "DiligentCore already cloned at $diligent_dir"
else
  echo "Cloning DiligentCore into thirdparty/diligent-core ..."
  git clone --depth 1 --recurse-submodules https://github.com/DiligentGraphics/DiligentCore.git "$diligent_dir"
fi
apply_patch "DiligentCore" "$diligent_dir" "$root/patches/diligent/diligent-core.patch"

echo
echo "Setup complete."
echo "Next: follow the Linux build steps in README.md from step 3."
