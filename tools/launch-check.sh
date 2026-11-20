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
    # What a desktop identifies the window by, and the icon it shows for it.
    if command -v xprop > /dev/null 2>&1; then
        class="$(xprop -name "PS VulkanPlay" WM_CLASS 2>/dev/null)"
        case "$class" in
            *'"io.github.codingncaffeine.PSVulkanPlay"'*) ok "window class is the application ID" ;;
            *) fail "window class: ${class:-none}" ;;
        esac
        icon="$(xprop -name "PS VulkanPlay" -notype 32c _NET_WM_ICON 2>/dev/null | tr -d '\n' | sed 's/.*= //' | cut -d, -f1,2 | tr -d ' ')"
        [ "$icon" = "256,256" ] && ok "window icon: 256x256" || fail "window icon: ${icon:-none}"
    else
        fail "xprop is not installed: the window's class and icon were not checked"
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

# A third start shows the generated clip: decoded like a console's picture and drawn. The clip is
# 416x240 in blocks of 16; the window is 1280x720, so it is drawn at three times its size, 1248x720,
# and every block is 48 screen pixels wide and high. Where two blocks of different brightness meet,
# one pixel on each side is a mix; a color sample is two brightness samples wide, so there it is up
# to three. A block's own color therefore fills from 42x42 to 48x48 pixels.
# The colors are what BT.709 in the limited range makes of the clip's samples
# (src/media/test_clip.cpp): gray (64 - 16) * 255 / 219 = 56; the patches 0.75 * 255 = 191.
count() { "$fbcheck" "$PSVP_TEST_FB" --count "$@" 2>/dev/null; }
within() { [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]; }
before="$(count 191 0 1) $(count 0 191 0) $(count 0 0 191) $(count 56 56 56)"
[ "$before" = "0 0 0 0" ] && ok "before the clip: none of its colors is on the screen" || fail "before the clip the screen already has its colors: $before"
: > "$log"
# One frame of it, standing still, so that the screen is read whole: frame 23130, whose number is
# 0101101001011010 (no two white blocks side by side) and whose moving block stands alone on gray.
"$app" --test-clip-frame 23130 > "$PSVP_TEST_TMP/clip.out" 2>&1 &
pid=$!
for _ in $(seq 150); do
    [ -n "$(line 'render: first picture shown.*')" ] && break
    kill -0 $pid 2>/dev/null || break
    sleep 0.1
done
l=$(line 'test clip: .*');                    [ -n "$l" ] && ok "$l" || fail "the clip's decoder did not open"
l=$(line 'render: first picture shown.*');    [ -n "$l" ] && ok "$l" || fail "no picture was shown"
case "$l" in *"416x240, 8 bits, limited range, BT.709, drawn at 1248x720"*) ok "the picture is drawn as large as it fits" ;; *) fail "the picture's size or colors are not the clip's" ;; esac
if kill -0 $pid 2>/dev/null; then
    sleep 0.7
    for patch in "red 191 0 1" "green 0 191 0" "blue 0 0 191" "yellow 191 191 0" "cyan 0 191 191" "magenta 191 0 191"; do
        set -- $patch
        n=$(count "$2" "$3" "$4")
        within "$n" $((42 * 42)) $((48 * 48)) && ok "the $1 patch: $n pixels of its color" || fail "the $1 patch: '$n' pixels of ($2,$3,$4), not $((42 * 42)) to $((48 * 48))"
    done
    # Sixteen white blocks show the frame's number (each bit is white in one of the two rows), and one more is the moving block.
    n=$(count 255 255 255)
    within "$n" $((17 * 46 * 46)) $((17 * 48 * 48)) && ok "white: $n pixels, the sixteen blocks of the number and the moving one" || fail "white: '$n' pixels, not seventeen blocks"
    # 390 blocks less the 32 of the number, the 8 patches and the moving one.
    n=$(count 56 56 56)
    within "$n" $((349 * 46 * 46)) $((349 * 48 * 48)) && ok "gray: $n pixels, the other 349 blocks" || fail "gray: '$n' pixels, not 349 blocks"
    n=$(count "${color[@]}")
    [ "$n" = 0 ] && ok "beside the picture the window is black, not its background color" || fail "$n pixels of the background color beside a picture"
    kill -TERM $pid 2>/dev/null
    wait $pid; rc=$?
    [ $rc -eq 0 ] && ok "stopped cleanly when asked (exit 0)" || fail "exit code $rc when asked to stop"
    l=$(line 'render: stopped after [0-9]* frames, 1 pictures')
    [ -n "$l" ] && ok "$l: the one frame, drawn again only when the window asked" || fail "not one picture: $(line 'render: stopped after.*')"
else
    wait $pid; rc=$?
    fail "exited by itself with code $rc"; sed 's/^/           | /' "$PSVP_TEST_TMP/clip.out" | head -20
fi
if grep -q ' ERROR ' "$log" 2>/dev/null; then
    fail "the application logged an error:"; grep ' ERROR ' "$log" | sed 's/^/           | /' | head -5
fi

# The window opens the way it was left. The runs above left it at the size of a first run; a
# window left smaller comes back smaller, and says so again when it closes. (Nothing here can
# drag a window's edge: that a resize is noted is not covered.)
place="$XDG_STATE_HOME/ps-vulkanplay/window.json"
if grep -q '"width": 1280' "$place" 2>/dev/null && grep -q '"height": 720' "$place" 2>/dev/null; then ok "the window's size was kept when it closed: 1280x720"
else fail "no kept window size: $(tr -d '\n' < "$place" 2>/dev/null)"; fi
printf '{"width": 960, "height": 540}\n' > "$place"
: > "$log"
"$app" --frames 5 > "$PSVP_TEST_TMP/place.out" 2>&1; rc=$?
l=$(line 'render: first frame presented, 960x540')
[ $rc -eq 0 ] && [ -n "$l" ] && ok "a window left at 960x540 opens at that size" || fail "left at 960x540, it opened as: $(line 'render: first frame presented, [0-9x]*') (exit $rc)"
if grep -q '"width": 960' "$place" && grep -q '"height": 540' "$place" && grep -q '"maximized": false' "$place"; then ok "and is kept at it"
else fail "the kept size after that run: $(tr -d '\n' < "$place")"; fi
# What makes no sense in the file counts for nothing.
printf '{"width": 3, "height": -1}\n' > "$place"
: > "$log"
"$app" --frames 5 > "$PSVP_TEST_TMP/place.out" 2>&1
l=$(line 'render: first frame presented, 1280x720'); [ -n "$l" ] && ok "a kept size that makes no sense is not used" || fail "a senseless kept size gave: $(line 'render: first frame presented, [0-9x]*')"
rm -f "$place"

[ $failed = 0 ] && echo "LAUNCH CHECK PASSED" || echo "LAUNCH CHECK FAILED"
exit $failed
