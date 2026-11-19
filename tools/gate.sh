#!/usr/bin/env bash
# The gate: everything a batch of work must pass before it is pushed. One run
# per batch, with its result read before any push.
#
#   tools/gate.sh          every lane
#   tools/gate.sh host     only the lanes that run on this machine (no containers)
#
# Each lane's output is kept in scratch/gate/<lane>.log. Exit 0 only if every lane passed.
set -uo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
scope="${1:-all}"
out=scratch/gate
rm -rf "$out"; mkdir -p "$out"
failed=0

lane() {
    local name="$1"; shift
    local start=$SECONDS
    if "$@" > "$out/$name.log" 2>&1; then
        printf '  ok    %-18s %4ss\n' "$name" "$((SECONDS - start))"
    else
        printf '  FAIL  %-18s %4ss   (%s)\n' "$name" "$((SECONDS - start))" "$out/$name.log"
        failed=1
    fi
}

# Build a preset and run its tests twice.
preset() { tools/build.sh "$1" && ctest --preset "$1" && ctest --preset "$1"; }
launch() { tools/wall.sh tools/launch-check.sh "build/$1/ps-vulkanplay"; }

lane wall-control      tools/wall.sh --check
lane release           preset release
lane launch            launch release
lane sanitizers        preset san
# Memory errors only: the window system's libraries hold allocations until exit.
lane launch-asan       env ASAN_OPTIONS=detect_leaks=0 tools/wall.sh tools/launch-check.sh build/san/ps-vulkanplay
lane thread-sanitizer  preset tsan
lane clang-libcxx      preset clang-libcxx
lane launch-libcxx     launch clang-libcxx
lane fuzz              tools/fuzz.sh 8
lane oracle            tools/oracle/run.sh
# The commands against the mock console, with the reference's own client as the control.
lane scenario          tools/wall.sh tests/scenario/phase1.sh build/release build/oracle
lane scenario-asan     tools/wall.sh tests/scenario/phase1.sh build/san
lane session           tools/wall.sh tests/scenario/phase2a.sh build/release build/oracle
lane session-asan      tools/wall.sh tests/scenario/phase2a.sh build/san
lane probe             tools/wall.sh tests/scenario/phase2b.sh build/release build/oracle
lane probe-asan        tools/wall.sh tests/scenario/phase2b.sh build/san
lane identity          tools/identity-audit.sh
if [ "$scope" = all ]; then
    # The four CI jobs, in the same containers.
    lane debian-13         tools/container-check.sh docker.io/library/debian:13
    lane alpine-musl       tools/alpine-build.sh
    lane debian-clang      tools/container-check.sh docker.io/library/debian:13 clang -- --compiler clang-libcxx
    lane debian-asan       tools/container-check.sh docker.io/library/debian:13 -- --sanitize address
fi

if [ $failed -eq 0 ]; then echo "GATE PASSED"; else echo "GATE FAILED"; fi
exit $failed
