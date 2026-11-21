#!/usr/bin/env bash
# The gate: everything a batch of work must pass before it is pushed. One run
# per batch, with its result read before any push.
#
#   tools/gate.sh          every lane
#   tools/gate.sh host     only the lanes that run on this machine (no containers)
#
# Each lane's output is kept in scratch/gate/<lane>.log. Exit 0 only if every lane passed.
# The builds run one after another, each on the machine's whole share of cores. What
# mostly waits (for a timer, a virtual screen, a console that is a mock) runs at once.
set -uo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
scope="${1:-all}"
out=scratch/gate
rm -rf "$out"; mkdir -p "$out"

lane() {
    local name="$1"; shift
    local start=$SECONDS
    if "$@" > "$out/$name.log" 2>&1; then
        printf '  ok    %-18s %4ss\n' "$name" "$((SECONDS - start))"
    else
        printf '  FAIL  %-18s %4ss   (%s)\n' "$name" "$((SECONDS - start))" "$out/$name.log"
        : > "$out/$name.failed"
    fi
}

# Build a preset and run its tests twice.
preset() { tools/build.sh "$1" && ctest --preset "$1" && ctest --preset "$1"; }
launch() { tools/wall.sh tools/launch-check.sh "build/$1/ps-vulkanplay"; }
scenario() { tools/wall.sh "tests/scenario/$1.sh" "${@:2}"; }

lane wall-control      tools/wall.sh --check
lane release           preset release
lane sanitizers        preset san
lane thread-sanitizer  preset tsan
lane clang-libcxx      preset clang-libcxx
lane oracle            tools/oracle/run.sh
lane fuzz              tools/fuzz.sh 8
# The app (.NET): built, its tests run twice. No window is opened and nothing is asked of a console.
# Exit 3 of the build is "built and tested, but bin/app is in use and was left alone": the gate is
# about the tree, and somebody using the build in bin/ does not make the tree wrong.
lane app               sh -c 'tools/build-app.sh --test; rc=$?; [ $rc -eq 0 ] || [ $rc -eq 3 ] || exit $rc; app/PSVulkanPlay.Tests/bin/Release/net10.0/PSVulkanPlay.Tests > /dev/null'
lane launch            launch release
# Memory errors only: the window system's libraries hold allocations until exit.
lane launch-asan       env ASAN_OPTIONS=detect_leaks=0 tools/wall.sh tools/launch-check.sh build/san/ps-vulkanplay
lane launch-libcxx     launch clang-libcxx
# Every Vulkan call of the window under the Khronos validation layer.
lane validation        tools/wall.sh tools/validation-check.sh build/release/ps-vulkanplay
# Decoding on this machine's graphics card, drawn as the window draws it (skips where no card decodes).
lane gpu               tools/gpu-check.sh build/release
lane tone              tools/tone-check.sh build/release
lane identity          tools/identity-audit.sh

# The commands against the mock console, with the reference's own client as the control.
# Here the wall gives every scenario a network of its own, so they can run at once
# (inside a container they share its one network, and run one after another).
lane scenario          scenario phase1 build/release build/oracle &
lane scenario-asan     scenario phase1 build/san &
lane session           scenario phase2a build/release build/oracle &
lane session-asan      scenario phase2a build/san &
lane probe             scenario phase2b build/release build/oracle &
lane probe-asan        scenario phase2b build/san &
lane stream            scenario phase2c build/release build/oracle &
lane stream-asan       scenario phase2c build/san &
lane receive           scenario phase2d build/release build/oracle &
lane receive-asan      scenario phase2d build/san &
lane input             scenario phase2e build/release build/oracle &
lane input-asan        scenario phase2e build/san &
lane decode            scenario phase3a build/release &
lane decode-asan       scenario phase3a build/san &
lane play              scenario phase3b build/release &
# The app's window on the real program against the mock, and the application started as a person starts it.
lane app-play          scenario phase5a build/release &
# Memory errors only, as for the other window runs.
lane play-asan         env ASAN_OPTIONS=detect_leaks=0 tools/wall.sh tests/scenario/phase3b.sh build/san &
wait

if [ "$scope" = all ]; then
    # The four CI jobs, in the same containers, at once: each on a quarter of this machine's share.
    share=$(( PSVP_JOBS / 4 > 0 ? PSVP_JOBS / 4 : 1 ))
    container() {
        local first=$(( PSVP_FIRST_CORE + $1 * share % PSVP_JOBS )); shift
        env PSVP_JOBS="$share" PSVP_FIRST_CORE="$first" "$@"
    }
    lane debian-13         container 0 tools/container-check.sh docker.io/library/debian:13 &
    lane alpine-musl       container 1 tools/alpine-build.sh &
    lane debian-clang      container 2 tools/container-check.sh docker.io/library/debian:13 clang -- --compiler clang-libcxx &
    lane debian-asan       container 3 tools/container-check.sh docker.io/library/debian:13 -- --sanitize address &
    lane debian-app        container 0 tools/container-check.sh docker.io/library/debian:13 app -- --app &
    lane alpine-app        container 1 tools/container-check.sh docker.io/library/alpine:latest app -- --app &
    wait
fi

if ls "$out"/*.failed > /dev/null 2>&1; then echo "GATE FAILED"; exit 1; fi
echo "GATE PASSED"
