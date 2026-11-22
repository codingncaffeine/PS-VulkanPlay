#!/usr/bin/env bash
# HDR to the screen (PLAN D42), end to end: the mock console sends an HEVC stream marked PQ, BT.2020,
# and the real program plays it in a window on a nested KWin (its virtual backend, inside the wall),
# whose surfaces offer HDR10 through the Wayland color management protocol: the swapchain must be
# made again as HDR10 when the first HDR picture comes, with the Khronos validation layer watching.
# The controls: the same stream with --hdr-output off is tone mapped to SDR, and an SDR stream on
# the same desktop keeps an SDR swapchain. (What the HDR10 pixels hold is checked by
# psvp_render_check --pattern against media::hdr10_signal; the nested desktop's screen is not read.)
#
#   tools/wall.sh tests/scenario/hdr.sh BUILD_DIR
#
# Exit: 0 passed (or skipped: no kwin_wayland here, said) | 1 a check failed | 2 not run through tools/wall.sh
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/hdr.sh BUILD_DIR}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ] || [ -z "${DISPLAY:-}" ] || [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; then
    echo "hdr: run it through tools/wall.sh" >&2; exit 2
fi
if ! command -v kwin_wayland > /dev/null; then
    echo "hdr: SKIPPED: no kwin_wayland on this machine (nothing here offers an HDR10 surface)"; exit 0
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/hdr"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }
lacks() { ! grep -aqF -- "$2" "$1"; }

mock_pid=""
kwin_pid=""
start_mock() {   # start_mock LOG_NAME OPTIONS...
    local log="$work/$1.log"; shift
    rm -f "$work/ready"
    "$mock_bin" --ready-file "$work/ready" --heartbeat-ms 200 --name "Living Room" "$@" > "$log" 2>&1 &
    mock_pid=$!
    for _ in $(seq 100); do [ -e "$work/ready" ] && return 0; sleep 0.05; done
    fail "the mock console did not start: $(head -3 "$log")"; return 1
}
stop_mock() { [ -n "$mock_pid" ] && { kill "$mock_pid" 2>/dev/null; wait "$mock_pid" 2>/dev/null; }; mock_pid=""; }
cleanup() { stop_mock; [ -n "$kwin_pid" ] && { kill "$kwin_pid" 2>/dev/null; wait "$kwin_pid" 2>/dev/null; }; }
trap cleanup EXIT

rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
start_mock register --kind ps5 --pin 12345678
"$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/register.out" 2>&1
check "registered with the mock" has "$work/register.out" "Registered with Living Room (PS5)"
stop_mock

# The nested desktop: its own socket in the wall's private runtime folder.
export XDG_RUNTIME_DIR="$work/run"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
kwin_wayland --virtual --no-lockscreen --no-global-shortcuts --socket psvp-hdr --width 1280 --height 720 > "$work/kwin.log" 2>&1 &
kwin_pid=$!
for _ in $(seq 100); do [ -S "$XDG_RUNTIME_DIR/psvp-hdr" ] && break; sleep 0.1; done
if [ ! -S "$XDG_RUNTIME_DIR/psvp-hdr" ]; then fail "the nested KWin did not start: $(tail -3 "$work/kwin.log")"; exit 1; fi

play_on_kwin() {   # play_on_kwin NAME CLIP_CODEC PLAY_OPTIONS...
    local name="$1" codec="$2"; shift 2
    start_mock "$name" --kind ps5 --clip 416x240 --clip-codec "$codec" --frame-ms 16 --media-ms 10
    SDL_VIDEO_DRIVER=wayland WAYLAND_DISPLAY=psvp-hdr \
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LOADER_LAYERS_ENABLE='*validation*' \
        "$app" play "Living Room" --seconds 3 --decoder software "$@" > "$work/$name.out" 2> "$work/$name.err"
    local rc=$?
    sleep 0.3; stop_mock
    # The play log has the DEBUG lines too (the swapchain's format): kept beside the run's output.
    cp "$XDG_STATE_HOME/ps-vulkanplay/logs/play.log" "$work/$name.log" 2>/dev/null || : > "$work/$name.log"
    [ $rc -eq 0 ] && ok "$name: exit 0" || fail "$name: exit $rc: $(tail -3 "$work/$name.out")"
    check "$name: on the nested desktop" has "$work/$name.err" "window: SDL"
    check "$name: on it through Wayland" has "$work/$name.err" "video driver wayland"
    check "$name: no error was logged" lacks "$work/$name.err" " ERROR "
    local reports
    reports=$(grep -ac 'VUID\|Validation Error\|Validation Warning' "$work/$name.err")
    [ "$reports" = 0 ] && ok "$name: the validation layer reported nothing" ||
        { fail "$name: the validation layer reported $reports lines:"; grep -a 'VUID\|Validation' "$work/$name.err" | sed -E 's/0x[0-9a-f]+/0x/g' | sort | uniq -c | cut -c1-300 | head -5; }
}

echo "an HDR stream on a desktop that takes HDR10: the swapchain is made HDR10 when the first HDR picture comes"
play_on_kwin hdr hevc-hdr --codec hdr --stats
check "the window says the picture goes to the screen as HDR10" has "$work/hdr.err" "render: HDR to the screen: HDR10 (PQ, BT.2020, A2B10G10R10)"
check "and the first picture was drawn as HDR10" has "$work/hdr.err" "HDR (PQ) sent to the screen as HDR10"
check "a swapchain of the 10-bit format was made (A2B10G10R10_UNORM_PACK32 = 64)" sh -c "grep -aq 'render: swapchain .*format 64' '$work/hdr.log'"
check "no tone mapping to SDR was said" lacks "$work/hdr.err" "tone mapped to SDR"

echo "the control: the same stream with --hdr-output off is tone mapped, on an SDR swapchain"
play_on_kwin off hevc-hdr --codec hdr --hdr-output off
check "the window says HDR to the screen is turned off" has "$work/off.err" "render: HDR pictures are tone mapped to SDR: HDR to the screen is turned off"
check "and the picture was tone mapped" has "$work/off.err" "HDR (PQ) tone mapped to SDR"
check "no HDR10 swapchain was made" lacks "$work/off.err" "HDR to the screen: HDR10"

echo "the control: an SDR stream on the same desktop keeps an SDR swapchain"
play_on_kwin sdr hevc --codec hevc
check "nothing of HDR was said" sh -c "! grep -aq 'HDR to the screen\|tone mapped to SDR' '$work/sdr.err'"
check "an 8-bit swapchain (B8G8R8A8_UNORM = 44 or R8G8B8A8_UNORM = 37)" sh -c "grep -aq 'render: swapchain .*format \(44\|37\)$' '$work/sdr.log' && ! grep -aq 'render: swapchain .*format 64' '$work/sdr.log'"

[ $failed -eq 0 ] && echo "HDR PASSED" || echo "HDR FAILED"
exit $failed
