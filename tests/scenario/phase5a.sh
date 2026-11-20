#!/usr/bin/env bash
# The app, end to end: its window on the real stream program, against the mock
# console. Two halves: the app's own tests driven through its window with the
# pointer (no stand-in for the program), and the real application started as a
# person starts it, its window read off the screen of the virtual X server.
#
#   tools/wall.sh tests/scenario/phase5a.sh BUILD_DIR [APP_DIR]
#
# APP_DIR is where 'dotnet build' put the app (default: app/PSVulkanPlay/bin/Release/net10.0);
# its tests are taken from beside it. Only through tools/wall.sh.
# Exit: 0 passed | 1 a check failed | 2 not run through tools/wall.sh | 3 the app is not built
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase5a.sh BUILD_DIR [APP_DIR]}
app_dir=${2:-app/PSVulkanPlay/bin/Release/net10.0}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ] || [ -z "${DISPLAY:-}" ] || [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; then
    echo "phase5a: run it through tools/wall.sh" >&2; exit 2
fi
native="$(realpath "$build/ps-vulkanplay")"
mock_bin="$build/psvp-mock-console"
fbcheck="$build/psvp_fbcheck"
app="$(realpath "$app_dir")/PSVulkanPlay"
tests="$(realpath "$app_dir/../../../../PSVulkanPlay.Tests/bin/Release/net10.0")/PSVulkanPlay.Tests"
[ -x "$app" ] && [ -x "$tests" ] || { echo "phase5a: the app is not built (tools/build-app.sh): $app" >&2; exit 3; }
work="$PSVP_TEST_TMP/phase5a"
rm -rf "$work"; mkdir -p "$work"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }
count() { "$fbcheck" "$PSVP_TEST_FB" --count "$@" 2>/dev/null; }

mock_pid=""
start_mock() {   # start_mock LOG_NAME OPTIONS...
    local log="$work/$1.log"; shift
    rm -f "$work/ready"
    "$mock_bin" --ready-file "$work/ready" --heartbeat-ms 200 --name "Living Room" "$@" > "$log" 2>&1 &
    mock_pid=$!
    for _ in $(seq 100); do [ -e "$work/ready" ] && return 0; sleep 0.05; done
    fail "the mock console did not start: $(head -3 "$log")"; return 1
}
stop_mock() { [ -n "$mock_pid" ] && { kill "$mock_pid" 2>/dev/null; wait "$mock_pid" 2>/dev/null; }; mock_pid=""; }

rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
start_mock register --kind ps5 --pin 12345678
"$native" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/register.out" 2>&1
check "registered with the mock" has "$work/register.out" "Registered with Living Room (PS5)"
stop_mock

echo "the app's window on the real program: the console is found, Play is pressed, the session is ended from the app"
start_mock play --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10
PSVP_E2E_NATIVE="$native" "$tests" -method "*The_app_plays_on_the_stand_in_console_with_the_real_program" > "$work/e2e.out" 2>&1; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && grep -aq "Total: 1, Errors: 0, Failed: 0, Skipped: 0" "$work/e2e.out" && ok "the session ran from the app's window and ended with a goodbye" \
    || fail "the app's session: exit $rc: $(sed 's/\x1b\[[0-9;]*m//g' "$work/e2e.out" | grep -aE 'not happen|Failure|Expected|Actual|Skip|Total' | head -6)"
check "the mock took the session" has "$work/play.log" "session request"
check "and was told goodbye" grep -aqi "goodbye\|disconnect" "$work/play.log"

echo "a controller the real program reads (a pretended one): its presses move through the app's window and press Play"
start_mock pad --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10
PSVP_E2E_NATIVE="$native" PSVP_E2E_CONTROLLER=1 "$tests" -method "*A_controller_the_real_program_reads_moves_through_the_app_and_presses_Play" > "$work/pad.out" 2>&1; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && grep -aq "Total: 1, Errors: 0, Failed: 0, Skipped: 0" "$work/pad.out" && ok "the controller's presses reached the window through the real program, and its cross started the session" \
    || fail "the controller in the app: exit $rc: $(sed 's/\x1b\[[0-9;]*m//g' "$work/pad.out" | grep -aE 'not happen|Failure|Expected|Actual|Skip|Total' | head -6)"
check "the mock took the session the controller started" has "$work/pad.log" "session request"

echo "a console that is in use: the page shows the program's own reason"
start_mock in-use --kind ps5 --in-use yes
PSVP_E2E_NATIVE="$native" PSVP_E2E_IN_USE=1 "$tests" -method "*A_console_that_refuses_gives_the_programs_reason_on_the_page" > "$work/in-use.out" 2>&1; rc=$?
stop_mock
[ $rc -eq 0 ] && grep -aq "Total: 1, Errors: 0, Failed: 0, Skipped: 0" "$work/in-use.out" && ok "the refusal is on the page in the program's words" \
    || fail "the refusal: exit $rc: $(sed 's/\x1b\[[0-9;]*m//g' "$work/in-use.out" | grep -aE 'not happen|Failure|Expected|Actual|Skip|Total' | head -6)"

echo "the application as a person starts it: the stream program, run with nothing to do, starts the app beside it"
# The layout of a build and of a package: the program, and the app in a folder 'app' beside it.
mkdir -p "$work/install" && cp "$native" "$work/install/ps-vulkanplay" && cp -a "$(dirname "$app")" "$work/install/app"
# A font the font cache has not seen: with it the text shaper's library is opened by the system's
# font library too, which is what an app that binds to the wrong one dies of at its first window.
font="$(find /usr/share/fonts -name '*.ttf' 2>/dev/null | head -1)"
[ -n "$font" ] && mkdir -p "$XDG_DATA_HOME/fonts" && cp "$font" "$XDG_DATA_HOME/fonts/unseen-$$.ttf"
mkdir -p "$XDG_CONFIG_HOME/ps-vulkanplay" && printf '{ "theme": "slate" }\n' > "$XDG_CONFIG_HOME/ps-vulkanplay/app.json"
start_mock launch --kind ps5
check "before: none of the app's colors is on the screen" [ "$(count 20 23 28) $(count 240 112 45)" = "0 0" ]
"$work/install/ps-vulkanplay" > "$work/launch.out" 2>&1 &
pid=$!
shown=0
for _ in $(seq 150); do
    kill -0 $pid 2>/dev/null || break
    # The window's own background, and the orange of the one console's Play button.
    if [ "$(count 20 23 28)" -gt 200000 ] 2>/dev/null && [ "$(count 240 112 45)" -gt 800 ] 2>/dev/null; then shown=1; break; fi
    sleep 0.1
done
[ $shown -eq 1 ] && ok "the app's window is on the screen with the registered console's card ($(count 20 23 28) pixels of its background, $(count 240 112 45) of its Play button)" \
    || fail "the app's window: background $(count 20 23 28), orange $(count 240 112 45) pixels; running: $(kill -0 $pid 2>/dev/null && echo yes || echo no); $(head -5 "$work/launch.out")"
if kill -0 $pid 2>/dev/null; then
    check "the process is the app, started by the program" grep -aq "PSVulkanPlay" "/proc/$pid/cmdline"
    # An app whose UI thread is blocked does not close when asked.
    kill -TERM $pid
    for _ in $(seq 50); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
    kill -0 $pid 2>/dev/null && { fail "the app did not close within five seconds of being asked"; kill -KILL $pid; } || ok "the app closed when asked"
else
    fail "the app was not running: $(head -5 "$work/launch.out")"
fi
wait $pid 2>/dev/null
stop_mock
# What the app had started (the program that watches the controllers) notices that the app has gone, within a second or two.
left() { for exe in /proc/[0-9]*/exe; do [ "$(readlink "$exe" 2>/dev/null)" = "$work/install/ps-vulkanplay" ] && echo "$exe"; done; }
for _ in $(seq 40); do [ -z "$(left)" ] && break; sleep 0.1; done
[ -z "$(left)" ] && ok "what it started ended with it: no stream program is left" || fail "left running after the app: $(left | tr '\n' ' ')"

[ $failed -eq 0 ] && echo "PHASE 5A PASSED" || echo "PHASE 5A FAILED"
exit $failed
