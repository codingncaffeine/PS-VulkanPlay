#!/usr/bin/env bash
# Builds the tree it stands in from source with warnings as errors, runs the
# tests twice, and starts what was built behind the test wall. The same steps
# run in every CI job and in tools/container-check.sh.
#
#   tools/ci/build-and-check.sh [--compiler clang-libcxx] [--sanitize address|thread] [--no-launch] [--app]
#
# --app: the app (app/, .NET) instead of the scenarios of the stream program, which the other
# jobs run: the stream program is built, then the app and its tests, and the app is walked
# through its window on the real program and started as a person starts it.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh

compiler=default sanitize="" launch=1 app=0
while [ $# -gt 0 ]; do
    case "$1" in
        --compiler) compiler="$2"; shift 2 ;;
        --sanitize) sanitize="$2"; shift 2 ;;
        --no-launch) launch=0; shift ;;
        --app) app=1; shift ;;
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

if [ $app = 1 ]; then
    export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
    echo "── .NET SDK $(dotnet --version)"
    psvp_nice dotnet build app/PSVulkanPlay.Tests -c Release -v q --nologo -m:"$PSVP_JOBS"
    # Twice, as the others. A run that fails shows all it said: which test, and why. Every test's start
    # is said (the last one started is the one a crash came in), and a crash leaves the runtime's own
    # report of every thread's stack, which is shown (CI 44958130689: a segfault with nothing to go on).
    app_tests() {
        rm -f "$dir"/app-crash*
        if DOTNET_DbgEnableMiniDump=1 DOTNET_DbgMiniDumpType=1 DOTNET_EnableCrashReport=1 DOTNET_DbgMiniDumpName="$dir/app-crash-%p" \
            app/PSVulkanPlay.Tests/bin/Release/net10.0/PSVulkanPlay.Tests -reporter verbose > "$dir/app-tests.log" 2>&1; then
            grep -a 'Total:' "$dir/app-tests.log" | tail -1
        else
            cat "$dir/app-tests.log"
            for report in "$dir"/app-crash*.crashreport.json; do
                [ -f "$report" ] || continue
                # Its frames by module and name, thread after thread (the whole report is half a megabyte).
                echo "── the runtime's crash report ($report), its frames:"
                grep -oE '"(crashed|native_module|method_name|unmanaged_name)": *("[^"]*"|true|false)' "$report" | head -400
            done
            return 1
        fi
    }
    app_tests
    app_tests
    tools/wall.sh --check
    tools/wall.sh tests/scenario/phase5a.sh "$dir"
elif [ $launch = 1 ]; then
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
    tools/tone-check.sh "$dir"
fi
echo "BUILD AND CHECK PASSED"
