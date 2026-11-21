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
# Scaled by the tent, not debanded: the counts below are the tent's arithmetic (the defaults are checked further on).
"$app" play "Living Room" --seconds 5 --tap right@1,cross@1.5 --scaler bilinear --deband off > "$work/play.out" 2> "$work/play.err" &
pid=$!
# The picture, read off the screen while it plays. The clip is drawn at three times its size
# (tools/launch-check.sh has the arithmetic): a block's own color fills 40x40 to 48x48 pixels.
for _ in $(seq 80); do grep -aq "first picture shown" "$work/play.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
check "a picture was shown, by the scaler asked for" has "$work/play.err" "render: first picture shown, 416x240, 8 bits, limited range, BT.709, drawn at 1248x720 by bilinear"
sleep 1
for patch in "red 191 0 1" "green 0 191 0" "blue 0 0 191"; do
    set -- $patch
    n=$(count "$2" "$3" "$4")
    between "$n" $((40 * 40)) $((48 * 48)) && ok "on the screen: the $1 patch, $n pixels of its color" || fail "the $1 patch: '$n' pixels of ($2,$3,$4)"
done
n=$(count 56 56 56)
between "$n" $((349 * 46 * 46)) $((349 * 48 * 48)) && ok "on the screen: $n pixels of the clip's gray" || fail "gray: '$n' pixels"
# The control for the statistics below: without --stats none of their colors is drawn.
check "without --stats no statistics are on the screen" [ "$(count 236 239 244) $(count 240 112 45)" = "0 0" ]
# The clip moves: the white blocks are not where they were a second ago.
cp "$PSVP_TEST_FB" "$work/screen-1.xwd"; sleep 1
cmp -s "$PSVP_TEST_FB" "$work/screen-1.xwd" && fail "the screen did not change in a second" || ok "the screen changed within a second: the clip moves"
wait $pid; rc=$?
sleep 0.3; stop_mock
out="$work/play.out"; log="$work/play.log"
[ $rc -eq 0 ] && ok "play: exit 0" || fail "play: exit $rc: $(tail -3 "$out") $(grep -a ERROR "$work/play.err" | head -3)"
check "it decoded with FFmpeg's H.264 decoder" grep -aqE "^Played \(H\.264, FFmpeg's h264 decoder in software\): " "$out"
# The software Vulkan driver behind the wall has no video queue: the window says so, and the decoder waited for it to say.
check "it says why the pictures were decoded in software" has "$out" "Decoding in software: the device has no VK_KHR_video_queue."
check "the window said it before the decoder opened" sh -c "grep -an 'render: pictures are decoded in software' '$work/play.err' | head -1 | cut -d: -f1 | { read a; b=\$(grep -an 'play: decoding with' '$work/play.err' | head -1 | cut -d: -f1); [ -n \"\$a\" ] && [ -n \"\$b\" ] && [ \"\$a\" -lt \"\$b\" ]; }"
# The mock says every 1.2 s how it sees the connection, always the same: the log has it once, with its time.
own_log="${XDG_STATE_HOME:-$HOME/.local/state}/ps-vulkanplay/logs/play.log"
[ "$(grep -ac "the console's view of the connection: target bitrate 9708 kbit/s, round-trip value 200.0, loss 0" "$own_log")" = 1 ] \
    && ok "the console's view of the connection is in the log once: it did not change" || fail "the console's view in the log: $(grep -a "console's view" "$own_log" | head -3)"
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
check "and that no delay was kept on purpose" grep -aqE "^Pacing: .* No cushion was kept; a picture was on its way to the screen [0-9.]+ ms after its frame had arrived, on average\.$" "$out"
# How many the screen takes depends on how fast this machine draws; that most do is the point. Under a
# sanitizer (its lanes are for memory errors) it is how fast the sanitizer lets software Vulkan draw:
# said, not judged (CI 44953206005: 140 of 309 on two cores); the count above still must add up.
if [ -n "${ASAN_OPTIONS:-}${TSAN_OPTIONS:-}" ]; then
    echo "  --    under a sanitizer $shown of $decoded pictures reached the screen (not judged)"
else
    between "$shown" $((decoded / 2)) "$decoded" && ok "more than half reached the screen" || fail "only '$shown' of '$decoded' pictures reached the screen"
fi
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
# The report says what the congestion reports were made of. (The mock loses whole frames here: a frame of
# which nothing arrived has no count of packets to miss, so it is reported by the frame-loss message alone.
# The ceiling itself is checked in the unit tests, with frames that lost some of their packets.)
grep -aqE "^Congestion reports: [1-9][0-9]* packets counted as arrived and [0-9]+ as lost \([0-9.]+%\); the console was told of [0-9]+ lost \([0-9]+ of [1-9][0-9]* reports lowered to the 5% ceiling\)\.$" "$out" \
    && ok "the report says what the congestion reports counted and told" || fail "congestion reports: $(grep -a '^Congestion reports' "$out")"
grep -aqE "^Acknowledgments of the console's messages: [1-9][0-9]*, and [1-9][0-9]* copies sent 40 and 120 ms behind them; the console sent 0 of its messages again" "$out" \
    && ok "each acknowledgment went out again, and nothing came twice on a link that loses nothing of it" || fail "acknowledgments: $(grep -a '^Acknowledgments' "$out")"
grep -aqE "^Our messages: [1-9][0-9]* acknowledged by the console, after [0-9.]+ ms on average and [0-9.]+ ms at most \(0 took over 150 ms\); 0 were sent again, unacknowledged after 200 ms\.$" "$out" \
    && ok "our messages were acknowledged in time, and none was sent again" || fail "our messages: $(grep -a '^Our messages' "$out")"
check "no controller of this machine was seen: the wall hides them" has "$out" "Controller: none was connected"
check "no error was logged" lacks "$work/play.err" " ERROR "
# With no DualSense connected (the wall hides every controller) the program announces a DualShock 4.
check "the console was told of a DualShock 4" has "$log" "message type 21 channel 1 flags 1 key-position 46 authentic: 0815b2010410011802"
check "and of no DualSense features" lacks "$log" "control message type 0x13"

echo "what the console sends for the controller is read; a DualSense is announced when asked for"
start_mock feedback --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --feedback-ms 800 --haptics-every 1
"$app" play "Living Room" --seconds 3 --controller dualsense --back-left cross --back-right r3 --cushion 30 > "$work/feedback.out" 2> "$work/feedback.err"; rc=$?
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
# The cushion asked for is delay that is kept: it is taken up over the first half second, so over three seconds
# a picture is well over half of it behind its frame, however fast this machine decodes.
check "the cushion asked for was kept" has "$out" "A cushion of 30 ms was kept for frames that come late; a picture was on its way to the screen"
through=$(number "$out" 'on its way to the screen ([0-9]+)\.[0-9] ms after its frame had arrived')
between "$through" 15 500 && ok "with it a picture was $through ms behind its frame on average" || fail "the delay with a cushion of 30 ms: '$through': $(grep -a '^Pacing' "$out")"
check "the back buttons are what was asked for" has "$out" "Back buttons: the left one is cross, the right one is r3; they were pressed 0 times."
check "the controller information says player 2, and strengths 1 and 2" has "$out" "(player 2, haptics strength 1, trigger strength 2, 1 asked to take the controller's position as its rest)"
check "no error was logged" lacks "$work/feedback.err" " ERROR "
# On the wall's software Vulkan nothing chosen is the tent, not debanded (a graphics card gets EWA Lanczos and debanding).
check "unless said otherwise, on a software device, the picture is scaled by the tent and not debanded" \
    sh -c "grep -aqE 'first picture shown, .*drawn at 1248x720 by bilinear$' '$work/feedback.err'"
check "and the log says why" has "$work/feedback.err" "render: a software device: the picture is scaled by the tent"
check "a scaler is one of the five" sh -c "! '$app' play 'Living Room' --scaler nearest > /dev/null 2>&1"
check "debanding is on or off" sh -c "! '$app' play 'Living Room' --deband maybe > /dev/null 2>&1"
check "an HDR peak is auto or 400 to 10000 cd/m²" sh -c "! '$app' play 'Living Room' --hdr-peak 300 > /dev/null 2>&1 && ! '$app' play 'Living Room' --hdr-peak bright > /dev/null 2>&1"
check "what does not go with the option is refused" sh -c "! '$app' play 'Living Room' --controller wheel > /dev/null 2>&1"
check "a cushion is 50 ms at most" sh -c "! '$app' play 'Living Room' --cushion 80 > /dev/null 2>&1"
check "and is part of the pacing: not without it" sh -c "! '$app' play 'Living Room' --cushion 20 --pacing off > /dev/null 2>&1"
check "the loss ceiling is a percent: 100 at most" sh -c "! '$app' play 'Living Room' --loss-ceiling 101 > /dev/null 2>&1"
check "a back button takes a button's name and nothing else" sh -c "! '$app' play 'Living Room' --back-left lever > /dev/null 2>&1"
check "the decoder is auto or software" sh -c "! '$app' play 'Living Room' --decoder vulkan > /dev/null 2>&1"

echo "the statistics over the picture, asked for with --stats: drawn in their panel at the top left"
start_mock stats --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10
"$app" play "Living Room" --seconds 4 --stats --decoder software > "$work/stats.out" 2> "$work/stats.err" &
pid=$!
for _ in $(seq 80); do grep -aq "first picture shown" "$work/stats.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
sleep 1.5   # the rates are worked out twice a second: by now they are there
text=$(count 236 239 244); title=$(count 240 112 45)
between "$text" 300 60000 && ok "the statistics' text is on the screen: $text pixels of its color" || fail "statistics text: '$text' pixels of (236,239,244)"
between "$title" 100 20000 && ok "and their title in the accent color: $title pixels" || fail "statistics title: '$title' pixels of (240,112,45)"
wait $pid; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "play --stats: exit 0" || fail "play --stats: exit $rc: $(tail -3 "$work/stats.out")"
check "no error was logged with the statistics shown" lacks "$work/stats.err" " ERROR "
check "--decoder software: the window does not lend its device, and the report says software was asked for" \
    sh -c "grep -aq 'render: pictures are decoded in software: not asked for' '$work/stats.err' && grep -aq 'Decoding in software: software was asked for.' '$work/stats.out'"

echo "the menu over the stream: L1 + R1 + Options held on a (pretended) controller opens it, Cross chooses in it"
menu_run() {   # menu_run NAME DOWNS: opens the menu, goes down DOWNS times, presses Cross
    local name="$1" downs="$2" taps="$work/$1.taps"
    : > "$taps"
    start_mock "$name" --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10
    "$app" play "Living Room" --seconds 12 --pretend 045e:028e --pretend-taps "$taps" > "$work/$name.out" 2> "$work/$name.err" &
    pid=$!
    for _ in $(seq 80); do grep -aq "first picture shown" "$work/$name.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
    before=$(count 240 112 45)
    # A moment apart, as a hand presses them: the console sees the two shoulders go down first.
    printf 'down left-shoulder\n' >> "$taps"; sleep 0.3
    printf 'down right-shoulder\n' >> "$taps"; sleep 0.3
    printf 'down start\n' >> "$taps"
    for _ in $(seq 40); do grep -aq "the menu over the stream opened" "$work/$name.err" 2>/dev/null && break; sleep 0.1; done
    sleep 0.5
    bar=$(count 240 112 45); hint=$(count 150 156 168)
    cp "$PSVP_TEST_FB" "$work/$name-open.xwd"   # the screen with the menu, to be looked at
    printf 'up start\nup left-shoulder\nup right-shoulder\n' >> "$taps"
    for _ in $(seq "$downs"); do printf 'tap down\n' >> "$taps"; sleep 0.25; done
    printf 'tap south\n' >> "$taps"
    started=$SECONDS
    wait $pid; rc=$?
    took=$((SECONDS - started))
    sleep 0.3; stop_mock
}
menu_run menu 3
check "the menu opened from the three buttons" has "$work/menu.err" "the menu over the stream opened"
between "$bar" 5000 200000 && ok "it is on the screen: $bar pixels of its bar's color (before: $before)" || fail "the menu's bar: '$bar' pixels of (240,112,45) (before: $before)"
between "$hint" 200 20000 && ok "with its hint: $hint pixels" || fail "the menu's hint: '$hint' pixels of (150,156,168)"
[ "$rc" -eq 0 ] && ok "quit from the menu: exit 0" || fail "quit from the menu: exit $rc: $(tail -3 "$work/menu.out")"
[ "$took" -lt 8 ] && ok "it ended when Quit was chosen, not at the end of its 12 seconds ($took s after)" || fail "it ended $took s after Quit was chosen"
check "the report says what was chosen" has "$work/menu.out" "The menu over the stream was opened 1 times; chosen in it last: quit."
check "the console was not sent to rest" lacks "$work/menu.log" "rest mode requested"
inputs="$(grep -a 'stream input ' "$work/menu.log" | sed -E 's/^mock: stream input [0-9]+: //' | tr '\n' ';')"
check "the console saw L1 and R1 go down, then up when the menu opened, and never Options: $inputs" \
    sh -c "printf '%s' '$inputs' | grep -q 'l1 pressed;r1 pressed;' && printf '%s' '$inputs' | grep -q 'l1 released' && printf '%s' '$inputs' | grep -q 'r1 released' && ! printf '%s' '$inputs' | grep -q options"
menu_run menu-rest 4
[ "$rc" -eq 0 ] && ok "quit and rest from the menu: exit 0" || fail "quit and rest: exit $rc"
check "the console was asked to go to rest mode" has "$work/menu-rest.log" "rest mode requested"
check "and the report says so" has "$work/menu-rest.out" "chosen in it last: quit and put the console to rest."
check "--pretend-taps needs --pretend" sh -c "! '$app' play 'Living Room' --pretend-taps /dev/null > /dev/null 2>&1"

echo "the controller as the user chose it (a pretended DualSense Edge; the mock sends a light, trigger effects and a player)"
pad_run() {   # pad_run NAME [PLAY_OPTIONS...]: a touch, a turn, the right Fn button, the left stick a little off rest
    local name="$1" taps="$work/$1.taps"; shift
    : > "$taps"
    start_mock "$name" --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --feedback-ms 800
    "$app" play "Living Room" --seconds 5 --pretend 054c:0df2 --pretend-taps "$taps" "$@" > "$work/$name.out" 2> "$work/$name.err" &
    pid=$!
    for _ in $(seq 80); do grep -aq "first picture shown" "$work/$name.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
    sleep 1.2   # past the mock's feedback
    printf 'touch 0.25 0.5\n' >> "$taps"; sleep 0.2
    printf 'lift\n' >> "$taps"; sleep 0.2
    printf 'gyro 0.5 0 0\n' >> "$taps"
    printf 'tap right-paddle2\n' >> "$taps"; sleep 0.5
    wait $pid; rc=$?
    sleep 0.3; stop_mock
    # What the pretended controller was asked, in the program's log (its DEBUG lines).
    cp "$own_log" "$work/$name.play.log"
}
pad_run pad-defaults --fn-right menu
[ "$rc" -eq 0 ] && ok "defaults: exit 0" || fail "defaults: exit $rc: $(tail -3 "$work/pad-defaults.out")"
check "the console's light bar color reached the controller" has "$work/pad-defaults.play.log" "pretend: light bar set to 0, 64, 255"
check "its player lights too (player 3: index 2)" has "$work/pad-defaults.play.log" "pretend: player lights set to player index 2"
check "its trigger effects too (the effect report)" has "$work/pad-defaults.play.log" "pretend: an effect report of 47 bytes"
check "the motion sensors were turned on" has "$work/pad-defaults.play.log" "pretend: motion sensors on"
check "the touch reached the console" grep -aqE "stream input [0-9]+: touch [0-9]+ down at" "$work/pad-defaults.log"
check "the right Fn button opened the menu" has "$work/pad-defaults.out" "The menu over the stream was opened 1 times"
check "and the report names that setting" has "$work/pad-defaults.out" "Controller settings: right Fn: menu; trigger effects held back by them 0 times, Fn buttons pressed 1 times."
pad_run pad-chosen --trigger-effects off --light-bar ff8000 --player-lights off --motion off --touchpad off --dead-zone 10
[ "$rc" -eq 0 ] && ok "chosen: exit 0" || fail "chosen: exit $rc: $(tail -3 "$work/pad-chosen.out")"
check "the light bar is the user's color" has "$work/pad-chosen.play.log" "pretend: light bar set to 255, 128, 0"
check "and never the console's" lacks "$work/pad-chosen.play.log" "pretend: light bar set to 0, 64, 255"
check "the player lights are off (index -1), never the console's" sh -c "grep -aq 'pretend: player lights set to player index -1' '$work/pad-chosen.play.log' && ! grep -aq 'player index 2' '$work/pad-chosen.play.log'"
check "no trigger effect reached the controller" lacks "$work/pad-chosen.play.log" "pretend: an effect report"
check "the motion sensors stayed off" lacks "$work/pad-chosen.play.log" "pretend: motion sensors on"
check "no touch reached the console" sh -c "! grep -aqE 'stream input [0-9]+: touch' '$work/pad-chosen.log'"
check "the right Fn button did nothing (none chosen)" lacks "$work/pad-chosen.out" "The menu over the stream was opened"
check "the report names every setting and the effects held back" \
    has "$work/pad-chosen.out" "Controller settings: trigger effects off, light bar ff8000, player lights off, motion off, touchpad off, dead zone 10%; trigger effects held back by them 1 times, Fn buttons pressed 0 times."
check "a light bar is console, off or six hexadecimal digits" sh -c "! '$app' play 'Living Room' --light-bar blue > /dev/null 2>&1"
check "a dead zone is 30% at most" sh -c "! '$app' play 'Living Room' --dead-zone 31 > /dev/null 2>&1"
check "an Fn button does none, menu, stats or fullscreen" sh -c "! '$app' play 'Living Room' --fn-left screenshot > /dev/null 2>&1"

echo "a connection that drops while playing is made again: the mock goes silent 2 s into the first stream"
start_mock reconnect --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --drop-after-ms 2000
# Each session is held 9 s: the first is lost about 5 s after the drop, the second runs its 9 s and ends.
"$app" play "Living Room" --seconds 9 > "$work/reconnect.out" 2> "$work/reconnect.err" &
pid=$!
for _ in $(seq 150); do grep -aq "the connection was lost; trying" "$work/reconnect.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
sleep 0.4
notice=$(count 255 196 92)
cp "$PSVP_TEST_FB" "$work/reconnect-notice.xwd"   # the notice over the last picture, to be looked at
for _ in $(seq 200); do grep -aq "the stream is back after" "$work/reconnect.err" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
sleep 1
cp "$PSVP_TEST_FB" "$work/reconnect-1.xwd"; sleep 1
moving=no; cmp -s "$PSVP_TEST_FB" "$work/reconnect-1.xwd" || moving=yes
after=$(count 255 196 92)
wait $pid; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "the session ended as asked: exit 0" || fail "exit $rc: $(tail -2 "$work/reconnect.out")"
check "the mock dropped the first stream" has "$work/reconnect.log" "the first stream and its control connection went silent"
check "the program saw the connection lost (after the console stopped acknowledging)" has "$work/reconnect.out" "Living Room: the connection was lost: the console stopped acknowledging the stream's messages."
between "$notice" 200 60000 && ok "the notice was over the picture meanwhile: $notice pixels of its color" || fail "the notice: '$notice' pixels of (255,196,92)"
check "a new session was made and its pictures came" has "$work/reconnect.err" "play: the stream is back after"
[ "$(grep -ac 'transport connected on port 9296' "$work/reconnect.log")" -ge 2 ] && ok "the mock set up a second stream" || fail "the mock set up $(grep -ac 'transport connected on port 9296' "$work/reconnect.log") streams"
[ "$moving" = yes ] && ok "the picture moves again" || fail "the screen did not change in a second after the stream came back"
[ "${after:-1}" -eq 0 ] && ok "and the notice is gone" || fail "the notice is still there: $after pixels"
check "the report counts it" has "$work/reconnect.out" "The connection was lost 1 times; it came back 1 times (after "
check "--reconnect is 600 seconds at most" sh -c "! '$app' play 'Living Room' --reconnect 601 > /dev/null 2>&1"

echo "a console that goes to rest while the connection is down is woken by the reconnecting (with --wake)"
start_mock rest-drop --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --drop-after-ms 2000 --rest-on-drop yes
"$app" play "Living Room" --wake --seconds 9 > "$work/rest-drop.out" 2> "$work/rest-drop.err" &
pid=$!
for _ in $(seq 200); do grep -aq "is in rest mode: waking it" "$work/rest-drop.out" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
sleep 0.2
waking=$(count 255 196 92)
cp "$PSVP_TEST_FB" "$work/rest-drop-notice.xwd"   # the notice while it wakes, to be looked at
wait $pid; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "the session ended as asked: exit 0" || fail "exit $rc: $(tail -2 "$work/rest-drop.out")"
check "the mock went to rest as the stream dropped" has "$work/rest-drop.log" "in rest mode now"
check "a session asked of it while it rested was not served" has "$work/rest-drop.log" "session request ignored: in rest mode"
check "the program found it resting and woke it" has "$work/rest-drop.out" "Living Room is in rest mode: waking it."
check "the mock took the wake-up" has "$work/rest-drop.log" "wake-up accepted, now on"
between "$waking" 200 60000 && ok "a notice was over the picture while it woke: $waking pixels" || fail "the waking notice: '$waking' pixels of (255,196,92)"
check "the stream came back" has "$work/rest-drop.err" "play: the stream is back after"
check "the report counts the wake" has "$work/rest-drop.out" "The console went to rest while the connection was down and was woken 1 times."
# Without --wake the reconnecting does not wake it: it tries sessions until its time is up.
start_mock rest-drop-nowake --kind ps5 --clip 416x240 --frame-ms 16 --media-ms 10 --drop-after-ms 1000 --rest-on-drop yes
"$app" play "Living Room" --seconds 30 --reconnect 4 > "$work/rest-drop-nowake.out" 2> "$work/rest-drop-nowake.err"; rc=$?
sleep 0.3; stop_mock
[ $rc -ne 0 ] && ok "without --wake it gives up after its reconnect time: exit $rc" || fail "without --wake: exit 0"
check "and no wake-up was sent" lacks "$work/rest-drop-nowake.log" "wake-up"

echo "a session that does not start opens no window"
start_mock in-use --kind ps5 --in-use yes
"$app" play "Living Room" --seconds 2 > "$work/in-use.out" 2> "$work/in-use.err"; rc=$?
stop_mock
[ $rc -eq 1 ] && ok "exit 1" || fail "a session with a console that is in use: exit $rc"
check "no window was opened for it" lacks "$work/in-use.err" "window: SDL"

# Without --wake nothing is sent to wake a resting console (and, as a console, the mock serves it no session).
start_mock asleep --kind ps5 --state standby --clip 416x240 --frame-ms 16 --media-ms 10
"$app" play "Living Room" --seconds 1 > "$work/asleep.out" 2> "$work/asleep.err"; rc=$?
stop_mock
[ $rc -eq 1 ] && ok "a resting console without --wake: no session, exit 1" || fail "a resting console without --wake: exit $rc"
check "without --wake no wake-up is sent" lacks "$work/asleep.log" "wake-up"
check "the mock was reached all the same, and served no session while it rested" has "$work/asleep.log" "session request ignored: in rest mode"

echo "play --wake wakes a console in rest mode, and asks one that takes no session yet again"
start_mock wake --kind ps5 --state standby --in-use 1 --clip 416x240 --frame-ms 16 --media-ms 10
"$app" play "Living Room" --wake --seconds 2 > "$work/wake.out" 2> "$work/wake.err"; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "play --wake: exit 0" || fail "play --wake: exit $rc: $(tail -3 "$work/wake.out") $(tail -3 "$work/wake.err")"
check "it saw that the console rests and woke it" has "$work/wake.out" "Living Room is in rest mode: waking it."
check "the mock took the wake-up" has "$work/wake.log" "wake-up accepted, now on"
check "the first try was refused as in use, and it asked again" has "$work/wake.out" "Living Room is not taking a session yet: asking again in 5 seconds."
check "then it played" grep -aqE "^Played \(H\.264" "$work/wake.out"
echo "the controllers that are connected are listed: behind the wall there is none, and one can be pretended"
"$app" controllers > "$work/controllers.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/controllers.out" "No controller is connected." && ok "no controller of this machine is seen, and the program says so" || fail "controllers: exit $rc: $(head -2 "$work/controllers.out")"
[ "$("$app" controllers --json 2>/dev/null)" = '{"controllers": []}' ] && ok "as JSON: an empty list on one line" || fail "controllers --json: $("$app" controllers --json 2>&1 | head -2)"
"$app" controllers --json --pretend 054c:0df2 > "$work/pretend.out" 2>&1; rc=$?
[ $rc -eq 0 ] && [ "$(wc -l < "$work/pretend.out")" = 1 ] && grep -aq '"kind": "dualsense-edge"' "$work/pretend.out" \
    && ok "a pretended DualSense Edge is listed as one" || fail "controllers --pretend: exit $rc: $(head -2 "$work/pretend.out")"
check "with no sound device of its own its haptics would be rumble" grep -aq '"haptics": "rumble"' "$work/pretend.out"
# Watching goes on until the program is told to end, and ends then.
"$app" controllers --json --watch > "$work/watch.out" 2>&1 &
watch_pid=$!
sleep 1; kill -TERM $watch_pid 2>/dev/null
for _ in $(seq 50); do kill -0 $watch_pid 2>/dev/null || break; sleep 0.1; done
kill -0 $watch_pid 2>/dev/null && { fail "controllers --watch did not end when told to"; kill -KILL $watch_pid; } || ok "controllers --watch says the list once and ends when told to"
wait $watch_pid 2>/dev/null
[ "$(cat "$work/watch.out")" = '{"controllers": []}' ] && ok "what it said while nothing changed: the list, once" || fail "watch said: $(head -3 "$work/watch.out")"

echo "what is pressed on a controller is said as what it means to an app: a pretended controller's buttons, pressed from a file"
presses() {   # presses VENDOR:PRODUCT NAME: taps and holds, what was said goes to $work/NAME.out
    local taps="$work/$2.taps" pid line
    : > "$taps"
    "$app" controllers --json --watch --presses --pretend "$1" --pretend-taps "$taps" > "$work/$2.out" 2> "$work/$2.err" &
    pid=$!
    for _ in $(seq 100); do [ -s "$work/$2.out" ] && break; sleep 0.05; done
    for line in "tap down" "tap south" "tap east" "tap left-shoulder" "tap right-shoulder" "tap up" "tap left" "tap right" "tap north" "not a line"; do
        echo "$line" >> "$taps"; sleep 0.15
    done
    echo "down right" >> "$taps"; sleep 0.75; echo "up right" >> "$taps"; sleep 0.2      # held: once, again after 0.4 s, then every 0.11 s
    echo "stick 0 -32768" >> "$taps"; sleep 0.15; echo "stick 0 0" >> "$taps"; sleep 0.15
    kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null
}
said() { grep -a '"press"' "$1" | sed -E 's/.*"press":"([a-z]+)".*/\1/' | tr '\n' ' '; }
presses 054c:0df2 pad-sony
check "the list is said first, and names the controller" sh -c "head -1 '$work/pad-sony.out' | grep -aq '\"kind\": \"dualsense-edge\"'"
[ "$(said "$work/pad-sony.out" | cut -d' ' -f1-8)" = "down accept back previous next up left right" ] \
    && ok "direction buttons, cross = accept, circle = back, the shoulder buttons = the page before and after; a button that means nothing says nothing" \
    || fail "a DualSense's presses were said as: $(said "$work/pad-sony.out")"
held=$(said "$work/pad-sony.out" | tr ' ' '\n' | sed -n '9,$p' | grep -c '^right$')
[ "$held" -ge 4 ] && [ "$held" -le 6 ] && ok "a direction held for 0.75 s is said $held times (once, again after 0.4 s, then every 0.11 s)" || fail "the held direction was said $held times: $(said "$work/pad-sony.out")"
[ "$(said "$work/pad-sony.out" | tr ' ' '\n' | grep -c .)" = $((8 + held + 1)) ] && [ "$(said "$work/pad-sony.out" | awk '{print $NF}')" = "up" ] \
    && ok "the left stick pushed up is 'up', once" || fail "after the held direction: $(said "$work/pad-sony.out")"
check "each press names its controller's place in the list" grep -aqF '{"press":"down","controller":0}' "$work/pad-sony.out"
check "a line of the file that is no press is said to be none, and nothing is pressed for it" has "$work/pad-sony.err" "not understood: not a line"
presses 057e:2009 pad-nintendo
[ "$(said "$work/pad-nintendo.out" | cut -d' ' -f1-3)" = "down back accept" ] \
    && ok "where the maker prints A on the right-hand button, that one is accept and the lower one is back" || fail "a Nintendo controller's presses were said as: $(said "$work/pad-nintendo.out")"
"$app" controllers --presses > "$work/presses-alone.out" 2>&1; rc=$?
[ $rc -eq 64 ] && has "$work/presses-alone.out" "--presses goes with --watch" && ok "--presses without --watch is refused" || fail "controllers --presses: exit $rc: $(head -2 "$work/presses-alone.out")"

echo "diagnose: what this computer offers, and how the last session ended"
"$app" diagnose > "$work/diagnose.out" 2>&1; rc=$?
[ $rc -eq 0 ] && ok "diagnose: exit 0" || fail "diagnose: exit $rc: $(head -3 "$work/diagnose.out")"
check "it names the software Vulkan driver and why pictures are decoded in software here" \
    sh -c "grep -q 'Graphics device: llvmpipe' '$work/diagnose.out' && grep -q 'Pictures are decoded in software: the device has no VK_KHR_video_queue.' '$work/diagnose.out'"
check "it has the last session's report" sh -c "grep -q '^The last session (started ' '$work/diagnose.out' && grep -q '^  Played (H.264' '$work/diagnose.out'"
"$app" diagnose --json > "$work/diagnose.json" 2> "$work/diagnose-json.err"
# What else the machine's libraries say goes to the error stream; the answer is the output's one line.
check "as JSON: one line, with the device and the report ($(wc -l < "$work/diagnose-json.err") lines on the error stream: $(head -c 160 "$work/diagnose-json.err" | tr '\n' ' '))" \
    sh -c "[ \$(wc -l < '$work/diagnose.json') -eq 1 ] && grep -q '\"gpu_decoding_why_not\": \"the device has no VK_KHR_video_queue\"' '$work/diagnose.json' && grep -q '\"last_session\": \\[\"' '$work/diagnose.json'"

[ $failed -eq 0 ] && echo "PHASE 3B PASSED" || echo "PHASE 3B FAILED"
exit $failed
