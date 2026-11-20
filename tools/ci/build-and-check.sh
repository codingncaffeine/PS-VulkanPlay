#!/usr/bin/env bash
# Builds the tree it stands in from source with warnings as errors, runs the
# tests twice, and starts what was built behind the test wall. The same steps
# run in every CI job and in tools/container-check.sh.
#
#   tools/ci/build-and-check.sh [--compiler clang-libcxx] [--sanitize address|thread] [--no-launch]
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh

compiler=default sanitize="" launch=1
while [ $# -gt 0 ]; do
    case "$1" in
        --compiler) compiler="$2"; shift 2 ;;
        --sanitize) sanitize="$2"; shift 2 ;;
        --no-launch) launch=0; shift ;;
        *) echo "build-and-check: unknown option $1" >&2; exit 2 ;;
    esac
done

dir=build/check
args=(-S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSVP_WERROR=ON)
cxx=c++
if [ "$compiler" = clang-libcxx ]; then
    cxx=clang++
    args+=(-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++)
elif [ "$compiler" != default ]; then
    echo "build-and-check: unknown compiler $compiler" >&2; exit 2
fi
[ -n "$sanitize" ] && args+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo "-DPSVP_SANITIZE=$sanitize")

echo "── $(. /etc/os-release && echo "$PRETTY_NAME"), $($cxx --version | head -1), $(cmake --version | head -1)"
if ! configure_output="$(psvp_nice cmake "${args[@]}" 2>&1)"; then
    printf '%s\n' "$configure_output"
    exit 1
fi
psvp_nice cmake --build "$dir" -j "$PSVP_JOBS"

# Twice: a test that only passes the first time is not a passing test.
ctest --test-dir "$dir" --output-on-failure
ctest --test-dir "$dir" --output-on-failure | tail -3

if [ $launch = 1 ]; then
    tools/wall.sh --check
    # Under AddressSanitizer the window run looks for memory errors only: the window
    # system's libraries keep allocations until the process ends, which reads as leaks.
    [ "$sanitize" = address ] && export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
    tools/wall.sh tools/launch-check.sh "$dir/ps-vulkanplay"
    # The commands against the mock console, on real sockets and the consoles' real ports.
    # One after another: in a container they all share the one network it has.
    tools/wall.sh tests/scenario/phase1.sh "$dir"
    tools/wall.sh tests/scenario/phase2a.sh "$dir"
    tools/wall.sh tests/scenario/phase2b.sh "$dir"
    tools/wall.sh tests/scenario/phase2c.sh "$dir"
    tools/wall.sh tests/scenario/phase2d.sh "$dir"
    tools/wall.sh tests/scenario/phase2e.sh "$dir"
    tools/wall.sh tests/scenario/phase3a.sh "$dir"
    tools/wall.sh tests/scenario/phase3b.sh "$dir"
fi
echo "BUILD AND CHECK PASSED"
