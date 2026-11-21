#!/usr/bin/env bash
# The lab's clock on the screen (PLAN I3): the mock console sends the generated clip and writes
# when each frame left and when each press came in; psvp_screen_clock reads off the virtual screen,
# from outside the program, when each frame was shown and when the clip answered a press; the score
# says how long frames took from leaving the console to the screen, how evenly they followed one
# another there, and how long a press took to show, in its parts.
#
#   tools/wall.sh tests/scenario/lab.sh BUILD_DIR
#
# The control: a second run with a cushion of 20 ms, delay the program keeps on purpose, must read
# as 20 ms more from sent to on the screen. The scores of both runs are kept in the work folder
# (lab-plain.score, lab-cushion.score).
# Only through tools/wall.sh: the network that is nobody's, the virtual screen, no sound.
# Exit: 0 passed | 1 a check failed | 2 not run through tools/wall.sh
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/lab.sh BUILD_DIR}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ] || [ -z "${DISPLAY:-}" ] || [ -z "${PSVP_TEST_FB:-}" ] || [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; then
    echo "lab: run it through tools/wall.sh" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
clock="$build/psvp_screen_clock"
work="$PSVP_TEST_TMP/lab"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }
number() { sed -nE "s/.*$2.*/\1/p" "$1" | head -1; }
between() { [ -n "$1" ] && awk -v v="$1" -v a="$2" -v b="$3" 'BEGIN { exit !(v >= a && v <= b) }'; }
own_log="${XDG_STATE_HOME:-$HOME/.local/state}/ps-vulkanplay/logs/play.log"

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
"$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/register.out" 2>&1
check "registered with the mock" has "$work/register.out" "Registered with Living Room (PS5)"
stop_mock

presses=8
lab_run() {   # lab_run NAME PLAY_OPTIONS...: eight seconds of the clip, eight presses of Cross on a pretended pad
    local name="$1"; shift
    local taps="$work/$name.taps"
    : > "$taps"; rm -f "$work/$name.found"
    start_mock "$name-mock" --kind ps5 --clip 416x240 --fps 60 --media-ms 10 --timing-log "$work/$name.timing" || return
    "$clock" watch "$PSVP_TEST_FB" --clip 416x240 --seconds 11 --out "$work/$name.seen" --ready "$work/$name.found" 2> "$work/$name.clock" &
    local clock_pid=$!
    "$app" play "Living Room" --seconds 8 --pretend 045e:028e --pretend-taps "$taps" "$@" > "$work/$name.out" 2> "$work/$name.err" &
    local pid=$!
    for _ in $(seq 100); do [ -e "$work/$name.found" ] && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
    # Past the first second, which is not scored; then a press every 400 ms: far apart, so each
    # answer on the screen pairs with its press.
    sleep 1.5
    for _ in $(seq $presses); do printf 'tap south\n' >> "$taps"; sleep 0.4; done
    wait $pid; local rc=$?
    kill $clock_pid 2>/dev/null; wait $clock_pid 2>/dev/null
    sleep 0.2; stop_mock
    cp "$own_log" "$work/$name.play.log"
    [ $rc -eq 0 ] && ok "$name: play exit 0" || fail "$name: play exit $rc: $(tail -3 "$work/$name.err")"
    check "$name: the reader found the clip on the screen" has "$work/$name.clock" "screen clock: the clip is at"
    "$clock" score "$work/$name.timing" "$work/$name.seen" "$work/$name.play.log" > "$work/$name.score"; local scored=$?
    sed 's/^/        /' "$work/$name.score"
    [ $scored -eq 0 ] && ok "$name: a whole score" || fail "$name: score exit $scored"
}

echo "the clip with nothing kept on purpose"
lab_run plain
s="$work/plain.score"
sent=$(number "$s" 'Lab: ([0-9]+) frames sent after the first second')
reached=$(number "$s" 'frames sent after the first second, ([0-9]+) reached the screen')
plain=$(number "$s" 'From sent to on the screen: median ([0-9.]+) ms')
between "$sent" 370 440 && ok "$sent frames scored (about seven seconds at 60 a second, as a PS5 sends)" || fail "frames scored: '$sent'"
[ -n "$reached" ] && [ -n "$sent" ] && [ $((reached * 10)) -ge $((sent * 8)) ] && ok "$reached of them reached the screen: most" || fail "reached: '$reached' of '$sent'"
between "$plain" 0.5 60 && ok "median from sent to on the screen: $plain ms" || fail "median latency: '$plain'"
check "every press was answered on the screen" grep -aqE "^Lab: $presses presses, press to picture: median [0-9.]+ ms, most [0-9.]+ ms \(to the console: median [0-9.]+ ms; .*\); 0 never answered on the screen\.$" "$s"
picture=$(number "$s" 'press to picture: median ([0-9.]+) ms')
between "$picture" 1 120 && ok "a press took $picture ms to show (median)" || fail "press to picture: '$picture'"

echo "the control: a cushion of 20 ms must read as 20 ms more"
lab_run cushion --cushion 20
cushioned=$(number "$work/cushion.score" 'From sent to on the screen: median ([0-9.]+) ms')
added=$(awk -v a="$cushioned" -v b="$plain" 'BEGIN { if (a == "" || b == "") print ""; else printf "%.1f", a - b }')
between "$added" 14 26 && ok "the cushion added $added ms (median $plain -> $cushioned ms)" || fail "the cushion added '$added' ms (median '$plain' -> '$cushioned')"

[ $failed -eq 0 ] && echo "LAB PASSED" || echo "LAB FAILED"
exit $failed
