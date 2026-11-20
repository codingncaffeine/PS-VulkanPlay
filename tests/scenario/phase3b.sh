#!/usr/bin/env bash
# Playing, end to end: the mock console sends the generated clip, the real
# program plays it ('play'): the picture is read off the screen of the virtual X
# server, the sound goes to a sound driver that plays to nobody, and the buttons
# the program presses arrive at the mock.
#
#   tools/wall.sh tests/scenario/phase3b.sh BUILD_DIR
#
# Only through tools/wall.sh: it needs the network that is nobody's, the virtual
# screen, and a machine on which no sound can be heard.
# Exit: 0 passed | 1 a check failed | 2 not run through tools/wall.sh
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase3b.sh BUILD_DIR}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ] || [ -z "${DISPLAY:-}" ] || [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; then
    echo "phase3b: run it through tools/wall.sh" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
fbcheck="$build/psvp_fbcheck"
work="$PSVP_TEST_TMP/phase3b"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }
lacks() { ! grep -aqF -- "$2" "$1"; }
# The pattern must begin with text, not with the number: what stands before it is matched as far as it goes.
number() { sed -nE "s/.*$2.*/\1/p" "$1" | head -1; }
between() { [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]; }
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
"$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/register.out" 2>&1
check "registered with the mock" has "$work/register.out" "Registered with Living Room (PS5)"
stop_mock

echo "the clip is played for five seconds; two buttons are pressed on the way"
check "before: none of the clip's colors is on the screen" [ "$(count 191 0 1) $(count 56 56 56)" = "0 0" ]
start_mock play --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --lose-every 90 --lose-units 999
"$app" play "Living Room" --seconds 5 --tap right@1,cross@1.5 > "$work/play.out" 2> "$work/play.err" &
pid=$!
# The picture, read off the screen while it plays. The clip is drawn at three times its size
# (tools/launch-check.sh has the arithmetic): a block's own color fills 42x42 to 48x48 pixels.
for _ in $(seq 80); do grep -aq "first picture shown" "$work/play.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
check "a picture was shown" has "$work/play.err" "render: first picture shown, 416x240, 8 bits, limited range, BT.709, drawn at 1248x720"
sleep 1
for patch in "red 191 0 1" "green 0 191 0" "blue 0 0 191"; do
    set -- $patch
    n=$(count "$2" "$3" "$4")
    between "$n" $((42 * 42)) $((48 * 48)) && ok "on the screen: the $1 patch, $n pixels of its color" || fail "the $1 patch: '$n' pixels of ($2,$3,$4)"
done
n=$(count 56 56 56)
between "$n" $((349 * 46 * 46)) $((349 * 48 * 48)) && ok "on the screen: $n pixels of the clip's gray" || fail "gray: '$n' pixels"
# The clip moves: the white blocks are not where they were a second ago.
cp "$PSVP_TEST_FB" "$work/screen-1.xwd"; sleep 1
cmp -s "$PSVP_TEST_FB" "$work/screen-1.xwd" && fail "the screen did not change in a second" || ok "the screen changed within a second: the clip moves"
wait $pid; rc=$?
sleep 0.3; stop_mock
out="$work/play.out"; log="$work/play.log"
[ $rc -eq 0 ] && ok "play: exit 0" || fail "play: exit $rc: $(tail -3 "$out") $(grep -a ERROR "$work/play.err" | head -3)"
check "it decoded with FFmpeg's H.264 decoder" grep -aqE "^Played \(H\.264, FFmpeg's h264 decoder in software\): " "$out"
decoded=$(number "$out" 'Played [^:]*: ([0-9]+) pictures decoded from [0-9]+ frames')
between "$decoded" 280 320 && ok "$decoded pictures were decoded in five seconds" || fail "decoded: '$decoded': $(grep -a '^Played' "$out")"
shown=$(number "$out" 'pictures decoded from [0-9]+ frames, ([0-9]+) shown')
replaced=$(number "$out" 'shown, ([0-9]+) replaced by a newer one')
# Every decoded picture that was not held back reaches the screen or is replaced by a newer one first. When the
# session ends, one picture can be on its way to the screen and one more waiting for its turn to be shown.
held=$(number "$out" 'took them, ([0-9]+) held back as damaged')
[ -n "$shown" ] && [ -n "$replaced" ] && [ -n "$held" ] && [ $((shown + replaced + held)) -ge $((decoded - 2)) ] && [ $((shown + replaced + held)) -le "$decoded" ] \
    && ok "$shown reached the screen, $replaced were replaced by a newer one first, $held were held back" || fail "shown '$shown' replaced '$replaced' held '$held' of '$decoded'"
check "pictures are shown evenly: the report says how long they were held back for it" grep -aqE "^Pacing: [0-9]+ of [0-9]+ pictures were held back to follow the one before by a frame's time, by [0-9.]+ ms on average" "$out"
# How many the screen takes depends on how fast this machine draws; that most do is the point.
between "$shown" $((decoded / 2)) "$decoded" && ok "more than half reached the screen" || fail "only '$shown' of '$decoded' pictures reached the screen"
check "no frame was without a picture" grep -aqE ' 0 frames gave no picture; ' "$out"
lost=$(number "$out" 'Picture: .* ([0-9]+) frames lost;')
between "$lost" 2 4 && ok "$lost frames were lost on the way, and the stream went on" || fail "lost: '$lost'"
sound=$(number "$out" 'Sound: ([0-9]+) frames played')
left_out=$(number "$out" 'did not arrive, ([0-9]+) left out to keep up')
# A frame every 10 ms for five seconds. Each is played, or left out when more than 60 ms of sound wait:
# on a machine that stalls, frames arrive in a bunch and some are left out; they are never many.
[ -n "$sound" ] && [ -n "$left_out" ] && between $((sound + left_out)) 440 520 && ok "$((sound + left_out)) sound frames arrived: $sound played, $left_out left out to keep up with the picture" \
    || fail "sound: played '$sound', left out '$left_out': $(grep -a '^Sound: [0-9]* frames played' "$out")"
[ -n "$sound" ] && [ -n "$left_out" ] && [ "$left_out" -le $((sound / 4)) ] && ok "at least four of five were played" || fail "left out: '$left_out' of '$sound'"
check "the sound went to the driver that plays to nobody" has "$work/play.err" "play: sound through SDL's dummy driver, 48000 Hz, 2 channels"
check "the mock read the two presses and releases" [ "$(grep -a 'stream input ' "$log" | sed -E 's/^mock: stream //' | tr '\n' ';')" = "input 0: right pressed;input 1: right released;input 2: cross pressed;input 3: cross released;" ]
check "nothing the program sent was refused" lacks "$log" "REJECTED"
check "it said goodbye" has "$out" "Goodbye sent and acknowledged."
check "no controller of this machine was seen: the wall hides them" has "$out" "Controller: none was connected"
check "no error was logged" lacks "$work/play.err" " ERROR "
# With no DualSense connected (the wall hides every controller) the program announces a DualShock 4.
check "the console was told of a DualShock 4" has "$log" "message type 21 channel 1 flags 1 key-position 46 authentic: 0815b2010410011802"
check "and of no DualSense features" lacks "$log" "control message type 0x13"

echo "what the console sends for the controller is read; a DualSense is announced when asked for"
start_mock feedback --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --feedback-ms 800 --haptics-every 1
"$app" play "Living Room" --seconds 3 --controller dualsense > "$work/feedback.out" 2> "$work/feedback.err"; rc=$?
sleep 0.3; stop_mock
out="$work/feedback.out"; log="$work/feedback.log"
[ $rc -eq 0 ] && ok "play --controller dualsense: exit 0" || fail "play --controller dualsense: exit $rc: $(tail -3 "$out")"
check "the program says what it told the console" has "$out" "Told Living Room that the controller is a DualSense."
# On the control channel, before the microphone and the displays: the features, then the controller.
[ "$(grep -a 'control message type' "$log" | sed -E 's/^mock: //' | head -3 | tr '\n' ';')" = "control message type 0x13 with 3 bytes;control message type 0x11 with 16 bytes;control message type 0x36 with 4 bytes;" ] \
    && ok "the console heard of the DualSense's features and of the controller first" || fail "control messages: $(grep -a 'control message type' "$log" | head -4 | tr '\n' ';')"
check "and in the stream of a controller of type 6" has "$log" "message type 21 channel 1 flags 1 key-position 46 authentic: 0815b2010410011806"
# The mock sends rumble, trigger effects and a light color 800 ms into the stream, and stops the rumble 400 ms later;
# with every sound packet it sends a frame of haptics. Behind the wall there is no controller for any of it to reach,
# and no sound device of one: the haptics would become rumble.
grep -aqE "^For the controller the console sent: rumble 2 times \(0 reached the controller; the strongest 200 and 90 of 255\), a light bar color 2 times \(0 reached it; the last: 0, 64, 255\), trigger effects 1 times \(0 reached a DualSense\), and [0-9]+ frames of haptics \(0 played on the controller's own sound device, 0 times turned into rumble, 0 let go as too late, 0 unusable\)" "$out" \
    && ok "rumble, the light bar's color, trigger effects and haptics were read as the mock sent them" || fail "controller data: $(grep -a '^For the controller' "$out")"
haptics=$(number "$out" 'and ([0-9]+) frames of haptics')
between "$haptics" 250 320 && ok "$haptics frames of haptics in three seconds, each of the size a console's are" || fail "haptics frames: '$haptics'"
check "with no sound device of a DualSense the haptics are to become rumble" has "$work/feedback.err" "haptics become rumble"
check "the controller information says player 2, and strengths 1 and 2" has "$out" "(player 2, haptics strength 1, trigger strength 2, 1 asked to take the controller's position as its rest)"
check "no error was logged" lacks "$work/feedback.err" " ERROR "
check "what does not go with the option is refused" sh -c "! '$app' play 'Living Room' --controller wheel > /dev/null 2>&1"

echo "a session that does not start opens no window"
start_mock in-use --kind ps5 --in-use yes
"$app" play "Living Room" --seconds 2 > "$work/in-use.out" 2> "$work/in-use.err"; rc=$?
stop_mock
[ $rc -ne 0 ] && ok "exit $rc" || fail "a session with a console that is in use passed"
check "no window was opened for it" lacks "$work/in-use.err" "window: SDL"

[ $failed -eq 0 ] && echo "PHASE 3B PASSED" || echo "PHASE 3B FAILED"
exit $failed
