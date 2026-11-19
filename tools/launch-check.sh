#!/usr/bin/env bash
# Starts a built application and checks what a person would see and what its own
# log says. A build can succeed and still not start; this is the check for that,
# on the machine (or in the container) the build was made on.
#
#   tools/wall.sh tools/launch-check.sh APP
#     APP: the executable, e.g. build/release/ps-vulkanplay; psvp_fbcheck is
#          expected beside it
#
# Only through tools/wall.sh: it supplies the virtual screen and the private
# profile, and makes sound and the network unreachable.
# Exit: 0 passed | 1 a check failed | 2 not run through tools/wall.sh
set -uo pipefail
app=${1:?usage: tools/wall.sh tools/launch-check.sh APP}
if [ -z "${DISPLAY:-}" ] || [ -z "${PSVP_TEST_TMP:-}" ] || [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; then
    echo "launch-check: run it through tools/wall.sh" >&2; exit 2
fi
fbcheck="$(dirname "$app")/psvp_fbcheck"
[ -x "$app" ] || { echo "launch-check: no executable at $app" >&2; exit 1; }
[ -x "$fbcheck" ] || { echo "launch-check: no psvp_fbcheck beside $app" >&2; exit 1; }

# The window background, as in src/render/vk_window.h.
color=(20 23 28)
log="$XDG_STATE_HOME/ps-vulkanplay/logs/ps-vulkanplay.log"
out="$PSVP_TEST_TMP/launch.out"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
line() { grep -o -- "$1" "$log" 2>/dev/null | head -1; }

version="$("$app" --version 2>&1)"
if printf '%s' "$version" | grep -qE '^ps-vulkanplay [0-9]+\.[0-9]+\.[0-9]+$'; then ok "--version: $version"
else fail "--version printed: $version"; fi
[ ! -e "$log" ] && ok "--version wrote no log" || fail "--version opened the log"

# Before the application runs, the screen must NOT be its color: the control for the screen check.
if "$fbcheck" "$PSVP_TEST_FB" "${color[@]}" > "$PSVP_TEST_TMP/fb-before.out" 2>&1; then
    fail "the empty screen already reads as the application's color: $(cat "$PSVP_TEST_TMP/fb-before.out")"
else
    ok "before: $(cat "$PSVP_TEST_TMP/fb-before.out")"
fi

"$app" > "$out" 2>&1 &
pid=$!
for _ in $(seq 150); do
    [ -n "$(line 'render: first frame presented.*')" ] && break
    kill -0 $pid 2>/dev/null || break
    sleep 0.1
done
l=$(line 'ps-vulkanplay [0-9.]* starting .*');   [ -n "$l" ] && ok "$l" || fail "no version line in the log"
l=$(line 'window: SDL .*');                       [ -n "$l" ] && ok "$l" || fail "the window system did not start"
l=$(line 'render: first frame presented.*');      [ -n "$l" ] && ok "$l" || fail "no frame was presented"
if kill -0 $pid 2>/dev/null; then
    sleep 0.5
    if "$fbcheck" "$PSVP_TEST_FB" "${color[@]}" > "$PSVP_TEST_TMP/fb-after.out" 2>&1; then
        ok "on screen: $(cat "$PSVP_TEST_TMP/fb-after.out")"
    else
        fail "the window is not on the screen: $(cat "$PSVP_TEST_TMP/fb-after.out")"
    fi
    # Asked to stop the way a desktop asks: it must close down cleanly.
    kill -TERM $pid 2>/dev/null
    wait $pid; rc=$?
    [ $rc -eq 0 ] && ok "stopped cleanly when asked (exit 0)" || fail "exit code $rc when asked to stop"
    l=$(line 'render: stopped after [0-9]* frames'); [ -n "$l" ] && ok "$l" || fail "no closing line in the log"
else
    wait $pid; rc=$?
    fail "exited by itself with code $rc"; sed 's/^/           | /' "$out" | head -20
fi
if grep -q ' ERROR ' "$log" 2>/dev/null; then
    fail "the application logged an error:"; grep ' ERROR ' "$log" | sed 's/^/           | /' | head -5
fi

# A second start that ends by itself after a set number of frames.
"$app" --frames 20 > "$PSVP_TEST_TMP/frames.out" 2>&1 &
pid=$!
for _ in $(seq 200); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done
if kill -0 $pid 2>/dev/null; then
    fail "--frames 20 was still running after 20 s"; kill -TERM $pid 2>/dev/null; wait $pid
else
    wait $pid; rc=$?
    l=$(line 'render: stopped after 20 frames')
    if [ $rc -eq 0 ] && [ -n "$l" ]; then ok "--frames 20: $l, exit 0"
    else fail "--frames 20: exit code $rc, closing line '$l'"; sed 's/^/           | /' "$PSVP_TEST_TMP/frames.out" | head -20; fi
fi
[ -f "$log.1" ] && ok "the previous run's log was kept beside the new one" || fail "the previous log was not kept"

[ $failed = 0 ] && echo "LAUNCH CHECK PASSED" || echo "LAUNCH CHECK FAILED"
exit $failed
