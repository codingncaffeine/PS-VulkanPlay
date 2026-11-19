#!/usr/bin/env bash
# Setting a stream up, end to end: after the session's control phase and the
# network probe the real program opens the stream connection, sends its opening
# message, makes the stream keys with the mock console, takes the stream
# information, holds the stream and says goodbye.
#
#   tools/wall.sh tests/scenario/phase2c.sh BUILD_DIR [REFERENCE_BUILD_DIR]
#
# With REFERENCE_BUILD_DIR the reference's own client is run against the mock
# too: it must get through the set-up, every packet it sends must carry a code
# the mock's keys give, and its first messages must sit at the same key
# positions as this client's.
# Only on a network that is nobody's (see tests/scenario/phase1.sh).
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase2c.sh BUILD_DIR [REFERENCE_BUILD_DIR]}
reference=${2:-}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase2c: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/phase2c"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }
lacks() { ! grep -aqF -- "$2" "$1"; }
# number FILE REGEX: the first number that REGEX's one group captures in FILE.
number() { sed -nE "s/.*$2.*/\1/p" "$1" | head -1; }
between() { [ -n "$1" ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]; }

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
# The client's three messages of the set-up as the mock recorded them, without their key positions.
setup_of() { grep -aE 'stream message type (14|21|13) ' "$1" | sed -E 's/^mock: //; s/key-position [0-9]+ //'; }
# The launch specification the mock opened, without what differs from run to run.
spec_of() { grep -a "stream launch spec:" "$1" | sed -E 's/^mock: //; s/"handshakeKey":"[^"]*"/"handshakeKey":"-"/; s/"rtt":[0-9]+/"rtt":-/'; }

for kind in ps4 ps5; do
    family=$(echo "$kind" | tr a-z A-Z)
    [ $kind = ps5 ] && version=12 || version=9
    echo "$family: a stream is set up, held and closed"
    rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
    start_mock "$kind-register" --kind "$kind" --pin 12345678
    "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/$kind-register.out" 2>&1
    check "registered with the mock" has "$work/$kind-register.out" "Registered with Living Room ($family)"
    stop_mock

    start_mock "$kind-plain" --kind "$kind"
    "$app" session "Living Room" --seconds 2 > "$work/$kind-plain.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-plain.out"; log="$work/$kind-plain.log"
    [ $rc -eq 0 ] && ok "session: exit 0" || fail "session: exit $rc: $(tail -2 "$out")"
    check "the opening message went out whole, with the console's own session ID" has "$out" "in 1 datagram), session ID: the console's own."
    check "the console's answer is reported" has "$out" \
        "The console answered: stream protocol version $version; its key signature matches; the answer carried no authentication code; its session key repeats the one sent."
    check "the stream information is reported" has "$out" \
        "Stream information: picture 1280x720 (header of 8 bytes); sound 2 channels, 16 bits, 48000 Hz, frames of 480."
    check "no packet was refused, either way" has "$out" "0 with a wrong one, 0 before the keys; control packets refused: 0;"
    check "the goodbye was acknowledged" has "$out" "Goodbye sent and acknowledged."
    check "the mock opened the launch specification and found the signature right" has "$log" \
        "stream big: version $version chunks 1 key-position 0 code 00000000 session-id the one handed out encrypted-key 00000000 public-key 65 bytes signature ok"
    check "the launch specification asks for 1280x720 at 60" has "$log" '"resolution":{"width":1280,"height":720},"maxFps":60,"score":10'
    # The key positions follow from the sizes of what was sent before: 29 + 13, 2 + 2, 9 + 9, 20 + 4.
    check "the answer to the stream information is sealed, at key position 42" has "$log" \
        "stream message type 14 channel 9 flags 1 key-position 42 authentic: 080e"
    check "the controller is announced, at key position 46" has "$log" \
        "stream message type 21 channel 1 flags 1 key-position 46 authentic: 0815b2010410011802"
    check "the microphone's sound format is sent, at key position 64" has "$log" \
        "stream message type 13 channel 1 flags 1 key-position 64 authentic: 080d7a10120e01100000bb80000001e000000001"
    check "the mock got the goodbye" has "$log" "stream goodbye: Client Disconnecting"
    check "the mock refused none of its packets" has "$log" "control-rejected 0 before-keys 0"
    check "every key position follows from the sizes before it" grep -aqE 'stream summary: .* key-positions [0-9]+ out-of-step 0$' "$log"
    check "nothing was rejected or left unsealed" lacks "$log" "REJECTED"
    check "nothing after the keys was unsealed" lacks "$log" "NOT SEALED"
    sent=$(number "$out" 'heartbeats sent: ([0-9]+)\.')
    between "$sent" 1 2 && has "$log" "stream summary: heartbeats $sent " && ok "the mock counted the $sent heartbeats the client sent in 2 s" \
        || fail "heartbeats: sent '$sent', mock: $(grep -a 'stream summary' "$log")"
    check "the control channel was answered all the while" grep -aqE 'heartbeats answered: [1-9]' "$out"
done

echo "PS5: what the stream carries is counted by its authentication code"
start_mock media --media-ms 100
"$app" session "Living Room" --seconds 2 > "$work/media.out" 2>&1
sleep 0.3; stop_mock
good=$(number "$work/media.out" '0 picture and ([0-9]+) sound packets arrived with the right authentication code, 0 with a wrong one')
between "$good" 17 21 && ok "one sealed sound packet every 100 ms for 2 s: $good counted" || fail "sound packets counted: '$good': $(grep -a 'Stream held' "$work/media.out")"
start_mock media-bad --media-ms 100 --corrupt-media yes
"$app" session "Living Room" --seconds 2 > "$work/media-bad.out" 2>&1; rc=$?
sleep 0.3; stop_mock
bad=$(number "$work/media-bad.out" '0 picture and 0 sound packets arrived with the right authentication code, ([0-9]+) with a wrong one')
[ $rc -eq 0 ] && between "$bad" 17 21 && ok "the same packets with a byte changed: none counted as good, $bad as wrong" \
    || fail "changed packets: exit $rc, wrong '$bad': $(grep -a 'Stream held' "$work/media-bad.out")"

echo "PS5: a narrow path splits the opening message"
start_mock narrow --mtu-in 700 --mtu-out 620
"$app" session "Living Room" --seconds 1 > "$work/narrow.out" 2>&1; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "session: exit 0" || fail "narrow: exit $rc: $(tail -2 "$work/narrow.out")"
check "the path was measured" has "$work/narrow.out" "largest packet in 700, out 620."
# The smaller of the two sizes less 50 is the largest datagram: 570 bytes, so 1377 bytes need three.
check "three datagrams" has "$work/narrow.out" "in 3 datagrams), session ID: the console's own."
check "the mock joined the three chunks" has "$work/narrow.log" "stream big: version 12 chunks 3 "
check "the launch specification names the inbound size" has "$work/narrow.log" '"mtu":700,'
check "the stream was set up" has "$work/narrow.out" "Stream information: picture 1280x720"

echo "PS5: the choices of the command line reach the console"
start_mock choices
"$app" session "Living Room" --seconds 1 --resolution 1080 --fps 30 --bitrate 20000 --codec hevc --session-id invented > "$work/choices.out" 2>&1; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "session: exit 0" || fail "choices: exit $rc: $(tail -2 "$work/choices.out")"
check "1920x1080 at 30" has "$work/choices.log" '"resolution":{"width":1920,"height":1080},"maxFps":30,"score":10'
check "20000 kbit/s" has "$work/choices.log" '"bwKbpsSent":20000,'
check "HEVC, standard range" has "$work/choices.log" '"videoCodec":"hevc","dynamicRange":"SDR",'
check "the control channel asked for HEVC too" has "$work/choices.log" "streaming-type 02000000"
check "an invented session ID was sent" has "$work/choices.out" "session ID: an invented one."
check "the mock saw another ID than its own" grep -aqE 'stream big: .* session-id another, of [0-9]+ characters' "$work/choices.log"
check "the client noticed that the answer names another session" has "$work/choices.out" "its session key is another one, of 16 characters."
start_mock hdr
"$app" session "Living Room" --seconds 1 --codec hdr > "$work/hdr.out" 2>&1
sleep 0.3; stop_mock
check "HEVC, high dynamic range" has "$work/hdr.log" '"videoCodec":"hevc","dynamicRange":"HDR",'
check "the control channel asked for it too" has "$work/hdr.log" "streaming-type 03000000"

echo "PS5: consoles that do not go along"
start_mock silent --stream no
start=$SECONDS
"$app" session "Living Room" --seconds 1 > "$work/silent.out" 2>&1; rc=$?
took=$((SECONDS - start))
stop_mock
[ $rc -eq 1 ] && has "$work/silent.out" "Living Room: the stream: the console did not answer the stream's opening message." \
    && ok "no answer to the opening message: exit 1 and the reason" || fail "silent: exit $rc: $(tail -2 "$work/silent.out")"
between "$took" 5 7 && ok "after the five seconds an answer is waited for ($took s)" || fail "silent: gave up after $took s"
start_mock version --refuse-version yes
"$app" session "Living Room" --seconds 1 > "$work/version.out" 2>&1; rc=$?
stop_mock
[ $rc -eq 1 ] && has "$work/version.out" "the console does not speak version 12 of the stream protocol" \
    && ok "version not accepted: exit 1 and the reason" || fail "version: exit $rc: $(tail -2 "$work/version.out")"
start_mock signature --wrong-signature yes
"$app" session "Living Room" --seconds 1 > "$work/signature.out" 2>&1; rc=$?
stop_mock
[ $rc -eq 0 ] && has "$work/signature.out" "its key signature DOES NOT MATCH" \
    && ok "a key signature that does not match is reported" || fail "signature: exit $rc: $(tail -3 "$work/signature.out")"

echo "what the command line refuses"
"$app" session "Living Room" --codec vp9 > "$work/bad-codec.out" 2>&1; rc=$?
[ $rc -eq 64 ] && has "$work/bad-codec.out" "--codec is h264, hevc or hdr" && ok "an unknown codec: exit 64" || fail "codec: exit $rc"
"$app" session "Living Room" --resolution 999 > "$work/bad-size.out" 2>&1; rc=$?
[ $rc -eq 64 ] && ok "an unknown picture size: exit 64" || fail "size: exit $rc"
"$app" session "Living Room" --session-id other > "$work/bad-id.out" 2>&1; rc=$?
[ $rc -eq 64 ] && ok "an unknown session ID choice: exit 64" || fail "session ID: exit $rc"

if [ -n "$reference" ]; then
    for kind in ps5 ps4; do
        family=$(echo "$kind" | tr a-z A-Z)
        echo "$family: the reference's own client against the mock"
        rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
        start_mock "$kind-register2" --kind "$kind" --pin 12345678
        "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > /dev/null 2>&1
        stop_mock
        start_mock "$kind-ours" --kind "$kind"
        "$app" session "Living Room" --seconds 1 > "$work/$kind-ours.out" 2>&1
        sleep 0.3; stop_mock
        start_mock "$kind-reference" --kind "$kind"
        timeout 60 "$reference/psvp_reference_client" session 127.0.0.1 "$kind" 1a2b3c4d a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 4 > "$work/$kind-reference.out" 2>&1
        sleep 0.3; stop_mock
        out="$work/$kind-reference.out"; log="$work/$kind-reference.log"
        check "it takes the mock's answer and makes its keys" has "$out" "StreamConnection successfully received bang"
        check "it accepts the sealed stream information" has "$out" "StreamConnection successfully received streaminfo"
        check "it reports the session as connected" has "$out" "reference: connected"
        check "it found no wrong code on anything the mock sent" lacks "$out" "MAC mismatch"
        check "the mock found its signature right" grep -aqE 'stream big: .* signature ok' "$log"
        check "the mock refused none of its control packets" has "$log" "control-rejected 0 before-keys 0"
        check "nothing it sent was rejected" lacks "$log" "REJECTED"
        check "nothing it sent after the keys was unsealed" lacks "$log" "NOT SEALED"
        # Its congestion reports (kind 5) and input packets (kind 6) are read by nobody here, but their codes are checked.
        congestion=$(number "$log" 'kind-5-authentic ([0-9]+)')
        input=$(number "$log" 'kind-6-authentic ([0-9]+)')
        between "$congestion" 5 60 && ok "its $congestion congestion reports carry the code the mock's keys give" || fail "congestion reports verified: '$congestion'"
        between "$input" 5 60 && ok "its $input input packets carry the code the mock's keys give" || fail "input packets verified: '$input'"
        setup_of "$work/$kind-ours.log" > "$work/$kind-ours.setup"
        setup_of "$log" > "$work/$kind-reference.setup"
        # Its key positions are its own (its threads take them in an order that varies), but they
        # must follow the same rule: each packet's is the one before plus that packet's size.
        steps=$(number "$log" 'key-positions ([0-9]+) out-of-step 0$')
        between "$steps" 30 200 && ok "the key positions of its $steps sealed packets follow from their sizes, as this client's do" \
            || fail "key positions: $(grep -a 'stream summary' "$log")"
        if [ "$(wc -l < "$work/$kind-ours.setup")" = 3 ] && cmp -s "$work/$kind-ours.setup" "$work/$kind-reference.setup"; then
            ok "its three set-up messages are ours, byte for byte"
        else
            fail "the set-up differs:"; diff "$work/$kind-ours.setup" "$work/$kind-reference.setup" | head -8 | cut -c1-170
        fi
        spec_of "$work/$kind-ours.log" > "$work/$kind-ours.spec"
        spec_of "$log" > "$work/$kind-reference.spec"
        if [ -s "$work/$kind-ours.spec" ] && cmp -s "$work/$kind-ours.spec" "$work/$kind-reference.spec"; then
            ok "its launch specification, as the mock opened it, is ours"
        else
            fail "the launch specifications differ:"; diff "$work/$kind-ours.spec" "$work/$kind-reference.spec" | cut -c1-300 | head -4
        fi
    done
else
    echo "the reference's own client against the mock: not run here (it needs the oracle build)"
fi

[ $failed = 0 ] && echo "PHASE 2C SCENARIO PASSED" || echo "PHASE 2C SCENARIO FAILED"
exit $failed
