#!/usr/bin/env bash
# Runs an installed or unpacked PS VulkanPlay the way its user will: the stream
# program plays a session with the mock console (the picture is read off the
# screen of the virtual X server), and the application is started as a person
# starts it and must show its window with the registered console's card. A
# package can build and install cleanly and still not start, or start and show
# nothing; only running it shows that.
#
#   tools/wall.sh packaging/smoke-test.sh PROGRAM TOOLS VERSION
#     PROGRAM  the installed ps-vulkanplay (/usr/bin/ps-vulkanplay, or the one in an unpacked tarball)
#     TOOLS    the folder with psvp-mock-console and psvp_fbcheck built beside that program
#              (packaging/out/native-glibc/tools or native-musl/tools; they are not shipped)
#     VERSION  what the program must say it is
#
# Only through tools/wall.sh: no sound, no network but its own, no device of
# this machine, a private profile.
# Exit: 0 passed | 1 a check failed | 2 not run through tools/wall.sh
set -uo pipefail
program=${1:?usage: tools/wall.sh packaging/smoke-test.sh PROGRAM TOOLS VERSION}
tools=${2:?the folder with psvp-mock-console and psvp_fbcheck}
version=${3:?the version}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ] || [ -z "${DISPLAY:-}" ] || [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; then
    echo "smoke-test: run it through tools/wall.sh" >&2; exit 2
fi
here="$(cd "$(dirname "$0")/.." && pwd)"
program="$(realpath "$program")"
mock_bin="$tools/psvp-mock-console"
fbcheck="$tools/psvp_fbcheck"
work="$PSVP_TEST_TMP/smoke"
rm -rf "$work"; mkdir -p "$work"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
count() { "$fbcheck" "$PSVP_TEST_FB" --count "$@" 2>/dev/null; }

echo "what is installed"
[ -x "$program" ] || { fail "no program at $program"; exit 1; }
# Where a package and a tarball put the app: beside the program, or in lib/ps-vulkanplay next to its bin.
app=""
for folder in "$(dirname "$program")/app" "$(dirname "$(dirname "$program")")/lib/ps-vulkanplay/app"; do
    [ -x "$folder/PSVulkanPlay" ] && { app="$folder"; break; }
done
[ -n "$app" ] && ok "the app is at $app" || { fail "no app beside $program or in ../lib/ps-vulkanplay"; exit 1; }
n=$(find "$app" -maxdepth 1 -name '*.dll' | wc -l)
[ "$n" -ge 150 ] && ok "$n managed assemblies" || fail "only $n managed assemblies (a whole app has about 200)"
for f in PSVulkanPlay.dll PSVulkanPlay.runtimeconfig.json PSVulkanPlay.deps.json System.Private.CoreLib.dll Avalonia.Base.dll \
         libcoreclr.so libhostfxr.so libhostpolicy.so libSkiaSharp.so libHarfBuzzSharp.so; do
    [ -f "$app/$f" ] || fail "app/$f is missing"
done
said="$("$program" --version 2>&1)"
[ "$said" = "ps-vulkanplay $version" ] && ok "--version: $said" || fail "--version says '$said', expected 'ps-vulkanplay $version'"

echo "the stream program plays against the mock console (tests/scenario/phase3b.sh, on the installed program)"
mkdir -p "$work/view"
ln -s "$program" "$work/view/ps-vulkanplay"
ln -s "$(realpath "$mock_bin")" "$work/view/psvp-mock-console"
ln -s "$(realpath "$fbcheck")" "$work/view/psvp_fbcheck"
if bash "$here/tests/scenario/phase3b.sh" "$work/view" > "$work/phase3b.out" 2>&1 && grep -q '^PHASE 3B PASSED' "$work/phase3b.out"; then
    ok "a session was played: $(grep -c '^  ok ' "$work/phase3b.out") checks, the picture read off the screen, buttons at the mock"
else
    fail "playing against the mock console:"
    grep -E '^  FAIL|^phase3b' "$work/phase3b.out" | head -12 | sed 's/^/           | /'
fi

echo "the application as a person starts it: the program, run with nothing to do, starts the app"
mock_pid=""
start_mock() {
    rm -f "$work/ready"
    "$mock_bin" --ready-file "$work/ready" --heartbeat-ms 200 --name "Living Room" "$@" > "$work/mock.log" 2>&1 &
    mock_pid=$!
    for _ in $(seq 100); do [ -e "$work/ready" ] && return 0; sleep 0.05; done
    fail "the mock console did not start: $(head -3 "$work/mock.log")"; return 1
}
stop_mock() { [ -n "$mock_pid" ] && { kill "$mock_pid" 2>/dev/null; wait "$mock_pid" 2>/dev/null; }; mock_pid=""; }
# A font the font cache has not seen: with it the text shaper's library is opened by the system's
# font library too, which is what an app that binds to the wrong one dies of at its first window.
font="$(find /usr/share/fonts -name '*.ttf' 2>/dev/null | head -1)"
[ -n "$font" ] && mkdir -p "$XDG_DATA_HOME/fonts" && cp "$font" "$XDG_DATA_HOME/fonts/unseen-$$.ttf"
[ -n "$font" ] || fail "no font on this system: the app's text cannot be drawn, install one for the check"
# The console the scenario above registered is still in the profile: its card is what the window shows.
check "a console is registered in this profile" test -s "$XDG_CONFIG_HOME/ps-vulkanplay/consoles.json"
printf '{ "theme": "slate" }\n' > "$XDG_CONFIG_HOME/ps-vulkanplay/app.json"
start_mock --kind ps5
sleep 1.5   # the last window of the scenario above is off the screen
check "before: none of the app's colors is on the screen" [ "$(count 20 23 28) $(count 240 112 45)" = "0 0" ]
"$program" > "$work/launch.out" 2>&1 &
pid=$!
shown=0
for _ in $(seq 300); do
    kill -0 $pid 2>/dev/null || break
    # The window's own background, and the orange of the one console's Play button.
    if [ "$(count 20 23 28)" -gt 200000 ] 2>/dev/null && [ "$(count 240 112 45)" -gt 800 ] 2>/dev/null; then shown=1; break; fi
    sleep 0.1
done
[ $shown -eq 1 ] && ok "the app's window is on the screen with the registered console's card ($(count 20 23 28) pixels of its background, $(count 240 112 45) of its Play button)" \
    || fail "the app's window: background $(count 20 23 28), orange $(count 240 112 45) pixels; running: $(kill -0 $pid 2>/dev/null && echo yes || echo no); $(head -5 "$work/launch.out")"
if kill -0 $pid 2>/dev/null; then
    [ "$(readlink "/proc/$pid/exe")" = "$app/PSVulkanPlay" ] && ok "the process is the installed app, started by the program" \
        || fail "the process is $(readlink "/proc/$pid/exe"), not $app/PSVulkanPlay"
    grep -q "$app/libHarfBuzzSharp.so" "/proc/$pid/maps" && ok "it draws its text with the text shaper it came with" || fail "the app's own libHarfBuzzSharp.so is not loaded"
    grep -q 'libharfbuzz\.so' "/proc/$pid/maps" && ok "the system's HarfBuzz is in the process too, and the app is up beside it" \
        || ok "(the system's HarfBuzz was not loaded here: no such library, or a font library that does not open it)"
    # The program the app runs for everything a console is asked: the installed one, watching the controllers.
    watcher=""
    for _ in $(seq 50); do
        for exe in /proc/[0-9]*/exe; do [ "$(readlink "$exe" 2>/dev/null)" = "$program" ] && watcher="$exe"; done
        [ -n "$watcher" ] && break; sleep 0.1
    done
    [ -n "$watcher" ] && ok "the app runs the installed stream program (it is watching the controllers)" || fail "the app started no stream program"
    # An app whose UI thread is blocked does not close when asked.
    kill -TERM $pid
    for _ in $(seq 50); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
    kill -0 $pid 2>/dev/null && { fail "the app did not close within five seconds of being asked"; kill -KILL $pid; } || ok "the app closed when asked"
else
    fail "the app was not running: $(head -5 "$work/launch.out")"
fi
wait $pid 2>/dev/null
stop_mock
if grep -qiE 'A fatal error|Failed to load|FileNotFoundException|XamlLoadException|DllNotFoundException|Unhandled exception' "$work/launch.out"; then
    fail "the app said:"; grep -iE 'A fatal error|Failed to load|FileNotFoundException|XamlLoadException|DllNotFoundException|Unhandled exception' "$work/launch.out" | head -5 | sed 's/^/           | /'
fi
left() { for exe in /proc/[0-9]*/exe; do [ "$(readlink "$exe" 2>/dev/null)" = "$program" ] && echo "$exe"; done; }
for _ in $(seq 40); do [ -z "$(left)" ] && break; sleep 0.1; done
[ -z "$(left)" ] && ok "what it started ended with it: no stream program is left" || fail "left running after the app: $(left | tr '\n' ' ')"

[ $failed -eq 0 ] && echo "SMOKE TEST PASSED: $version plays and its app starts" || echo "SMOKE TEST FAILED: do NOT release this artifact"
exit $failed
