#!/usr/bin/env bash
# Configures and builds one preset of CMakePresets.json. The release preset
# also refreshes bin/ps-vulkanplay, the one place to run the current build from.
#
#   tools/build.sh [release|san|tsan|clang-libcxx|fuzz]     default: release
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
preset="${1:-release}"

# Configuring is quiet unless it fails.
if ! configure_output="$(psvp_nice cmake --preset "$preset" 2>&1)"; then
    printf '%s\n' "$configure_output"
    exit 1
fi
psvp_nice cmake --build --preset "$preset" -j "$PSVP_JOBS"

if [ "$preset" = release ] && [ -x build/release/ps-vulkanplay ]; then
    mkdir -p bin
    install -m 755 build/release/ps-vulkanplay bin/ps-vulkanplay
    echo "bin/ps-vulkanplay is now $(bin/ps-vulkanplay --version)"
fi
