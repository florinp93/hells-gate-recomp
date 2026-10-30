#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
export LD_LIBRARY_PATH="$PWD/thirdparty/rexglue-sdk/out/linux-amd64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./out/build/linux-release/dantes_inferno "$@"
