#!/usr/bin/env bash
# Receiving a stream, end to end: the mock console sends picture frames (cut
# into packets, with repair packets) and sound packets, sealed and encrypted;
# the real program puts the frames together again and keeps them with
# 'session --dump'; what it kept is compared with what the mock sends.
#
#   tools/wall.sh tests/scenario/phase2d.sh BUILD_DIR [REFERENCE_BUILD_DIR]
#
# With REFERENCE_BUILD_DIR the reference's own client receives the same stream
# from the mock: what it hands to a decoder must be, byte for byte, what this
# client kept.
# Only on a network that is nobody's (see tests/scenario/phase1.sh).
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase2d.sh BUILD_DIR [REFERENCE_BUILD_DIR]}
reference=${2:-}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase2d: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/phase2d"
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
# What the dump checker says about the picture, and about the sound.
picture() { "$mock_bin" --check-dump "$1" --media-seed 5 2>&1 | grep -a '^dump: picture'; }
sound()   { "$mock_bin" --check-dump "$1" --media-seed 5 2>&1 | grep -a '^dump: sound'; }

# 62 picture frames, one every 16 ms: about a second. A sound frame every 10 ms for as long as the stream is held.
stream="--frame-ms 16 --frames 62 --media-seed 5 --media-ms 10"

for kind in ps4 ps5; do
    family=$(echo "$kind" | tr a-z A-Z)
    rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
    start_mock "$kind-register" --kind "$kind" --pin 12345678
    "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/$kind-register.out" 2>&1
    check "$family: registered with the mock" has "$work/$kind-register.out" "Registered with Living Room ($family)"
    stop_mock

    echo "$family: a stream that arrives whole"
    # shellcheck disable=SC2086
    start_mock "$kind-whole" --kind "$kind" $stream --haptics-every 4
    "$app" session "Living Room" --seconds 2 --dump "$work/$kind-whole" > "$work/$kind-whole.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-whole.out"; log="$work/$kind-whole.log"
    [ $rc -eq 0 ] && ok "session --dump: exit 0" || fail "session --dump: exit $rc: $(tail -2 "$out")"
    check "every picture frame was put together, none restored, none lost" has "$out" \
        "Picture: 62 frames put together"
    # Every frame was whole before its two repair packets came: 124 packets it did not need.
    check "the repair packets were not needed; no packet was too late, twice, unusable or unreadable" has "$out" \
        "0 of them with restored packets (0 packets restored), 0 frames lost; packets not needed: 124, too late: 0, twice: 0, unusable: 0, unreadable: 0."
    [ "$(picture "$work/$kind-whole")" = "dump: picture frames 62 first 1 last 62 wrong 0 keyframes 1 repaired 0 headers 1 missing: none" ] \
        && ok "what was kept is the header and the 62 frames the mock sent, byte for byte" || fail "kept picture: $(picture "$work/$kind-whole")"
    frames=$(number "$out" 'Sound: ([0-9]+) frames')
    between "$frames" 190 215 && ok "a sound frame every 10 ms for two seconds: $frames" || fail "sound frames: '$frames'"
    if [ $kind = ps5 ]; then
        haptics=$(number "$out" 'haptics: ([0-9]+) frames, 0 lost')
        between "$haptics" 47 54 && ok "a haptics frame with every fourth sound packet: $haptics" || fail "haptics frames: '$haptics': $(grep -a '^Sound' "$out")"
        expected="dump: sound frames $frames haptics frames $haptics wrong 0 from-a-later-packet 0 lost 0"
    else
        check "a PS4 sends no haptics" has "$out" "haptics: 0 frames, 0 lost."
        expected="dump: sound frames $frames haptics frames 0 wrong 0 from-a-later-packet 0 lost 0"
    fi
    [ "$(sound "$work/$kind-whole")" = "$expected" ] && ok "the sound kept is the sound the mock sent, byte for byte" || fail "kept sound: $(sound "$work/$kind-whole")"
    check "the console was told of no frame that did not arrive" has "$out" "Told the console: 0 times of frames that did not arrive"
    check "the mock got no such report" lacks "$log" "reported as not arrived"
    reports=$(number "$out" ', ([0-9]+) congestion reports')
    between "$reports" 9 11 && ok "a congestion report every 200 ms: $reports" || fail "congestion reports: '$reports'"
    check "the mock found the code of each report and each input state right, at the key position the rule gives" grep -aqE "kind-5-authentic $reports kind-6-authentic [0-9]+ key-positions [0-9]+ out-of-step 0 " "$log"
    check "the reports told of packets that arrived and of none that did not" grep -aqE 'congestion-received [1-9][0-9]* congestion-lost 0$' "$log"
    check "nothing the client sent was refused" lacks "$log" "REJECTED"
    states=$(number "$log" 'input-states ([0-9]+) at-rest')
    between "$states" 9 11 && has "$log" "input-states $states at-rest $states unreadable 0 out-of-order 0" \
        && ok "an input state every 200 ms: the mock decrypted $states, each a resting controller's, in order" \
        || fail "input states: $(grep -a 'stream summary' "$log")"
    check "one line per packet was kept" [ "$(grep -c picture "$work/$kind-whole/packets.tsv")" -ge 400 ]
    check "the list of the console's other messages is there, with its heading" [ "$(head -1 "$work/$kind-whole/data.tsv")" = "$(printf 'ms\twhat\tchannel\tbytes\thex')" ]
    check "with what the mock sends first, as a PS5 was seen to: controller information and 'stream information received'" \
        [ "$(cut -f2-5 "$work/$kind-whole/data.tsv" | sed -n 2,3p | tr '\t\n' ' ;')" = "data kind 9 0 17 0000000001000001000000010101000000;message type 14 0 2 080e;" ]
    quality=$(grep -c "message type 16" "$work/$kind-whole/data.tsv")
    between "$quality" 1 2 && ok "and its view of the connection every 1.2 s: $quality in two seconds" || fail "connection quality messages kept: $quality"
    check "which is reported" has "$out" "The console's latest view of the connection: target bitrate 9708 kbit/s, round-trip value 200.0, loss 0."
    beats=$(number "$out" 'Messages from the console: type 3 x([0-9]+)')
    between "$beats" 5 7 && ok "the mock's own heartbeats, three a second, were counted: $beats" || fail "console heartbeats: $(grep -a 'Messages from' "$out")"

    echo "$family: lost packets that the repair packets cover"
    # Every third frame loses its first two packets, as many as it has repair packets;
    # every fourth frame's last packet comes behind the next frame's first.
    # shellcheck disable=SC2086
    start_mock "$kind-repair" --kind "$kind" $stream --lose-every 3 --lose-units 2 --late-every 4
    "$app" session "Living Room" --seconds 2 --dump "$work/$kind-repair" > "$work/$kind-repair.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-repair.out"; log="$work/$kind-repair.log"
    [ $rc -eq 0 ] && ok "session: exit 0" || fail "session: exit $rc: $(tail -2 "$out")"
    check "twenty frames were restored, two packets each; none was lost" has "$out" \
        "Picture: 62 frames put together" 
    check "the restored packets are counted" has "$out" "20 of them with restored packets (40 packets restored), 0 frames lost;"
    [ "$(picture "$work/$kind-repair")" = "dump: picture frames 62 first 1 last 62 wrong 0 keyframes 1 repaired 20 headers 1 missing: none" ] \
        && ok "what was kept is the 62 frames the mock made, byte for byte and in order" || fail "kept picture: $(picture "$work/$kind-repair")"
    check "the console was told of no frame that did not arrive" lacks "$log" "reported as not arrived"
    check "the mock withheld the 40 packets" has "$log" " withheld 40 "
    check "the reports told the console of packets that did not arrive" grep -aqE 'congestion-lost [1-9][0-9]*$' "$log"

    echo "$family: lost packets that the repair packets do not cover"
    # Every tenth frame loses three packets, one more than it has repair packets;
    # every seventh sound packet is lost, and the next one brings its frame again.
    # shellcheck disable=SC2086
    start_mock "$kind-loss" --kind "$kind" $stream --lose-every 10 --lose-units 3 --lose-sound-every 7
    "$app" session "Living Room" --seconds 2 --dump "$work/$kind-loss" > "$work/$kind-loss.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-loss.out"; log="$work/$kind-loss.log"
    [ $rc -eq 0 ] && ok "session: exit 0" || fail "session: exit $rc: $(tail -2 "$out")"
    check "six frames are counted as lost" has "$out" "Picture: 56 frames put together"
    check "none of them was handed on damaged" has "$out" "(0 packets restored), 6 frames lost;"
    [ "$(picture "$work/$kind-loss")" = "dump: picture frames 56 first 1 last 62 wrong 0 keyframes 1 repaired 0 headers 1 missing: 10 20 30 40 50 60" ] \
        && ok "what was kept is every other frame, byte for byte; the six are missing and nothing is damaged" || fail "kept picture: $(picture "$work/$kind-loss")"
    check "the console was told six times" has "$out" "Told the console: 6 times of frames that did not arrive"
    for lost in 10 20 30 40 50 60; do
        has "$log" "frames $lost to $lost reported as not arrived (channel 2, authentic)" || { fail "the mock was not told of frame $lost"; lost=""; break; }
    done
    [ -n "$lost" ] && ok "the mock was told of each of the six, in sealed messages on channel 2"
    check "and of no other" [ "$(grep -c 'reported as not arrived' "$log")" = 6 ]
    frames=$(number "$out" 'Sound: ([0-9]+) frames')
    again=$(number "$out" 'frames \([0-9]+ bytes with the haptics\), ([0-9]+) of them taken from a later packet, 0 lost')
    between "$again" 27 30 && ok "the sound has no gap: $again of its $frames frames came from a later packet" || fail "sound: $(grep -a '^Sound' "$out")"
    [ "$(sound "$work/$kind-loss")" = "dump: sound frames $frames haptics frames 0 wrong 0 from-a-later-packet $again lost 0" ] \
        && ok "the sound kept is the sound the mock made, byte for byte" || fail "kept sound: $(sound "$work/$kind-loss")"
done

echo "packets that were changed on the way"
# shellcheck disable=SC2086
start_mock corrupt --kind ps5 $stream --corrupt-media yes
"$app" session "Living Room" --seconds 2 --dump "$work/corrupt" > "$work/corrupt.out" 2>&1; rc=$?
stop_mock
check "no frame is put together from them" has "$work/corrupt.out" "Picture: 0 frames put together (0 bytes)"
check "no sound frame either" has "$work/corrupt.out" "Sound: 0 frames (0 bytes with the haptics)"
check "they are counted as carrying a wrong code" grep -aqE '0 picture and 0 sound packets arrived with the right authentication code, [1-9][0-9]+ with a wrong one' "$work/corrupt.out"
[ "$(picture "$work/corrupt")" = "dump: picture frames 0 first 0 last 0 wrong 0 keyframes 0 repaired 0 headers 0 missing: none" ] \
    && ok "nothing was kept" || fail "kept: $(picture "$work/corrupt")"

echo "the dump checker tells a changed byte"
cp -r "$work/ps5-whole" "$work/changed"
printf '\x00' | dd of="$work/changed/video.bin" bs=1 seek=5000 conv=notrunc 2> /dev/null
"$mock_bin" --check-dump "$work/changed" --media-seed 5 > "$work/changed.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/changed.out" " wrong 1 " && ok "one changed byte in the kept picture: one frame wrong, exit 1" || fail "changed dump: exit $rc: $(cat "$work/changed.out")"
"$mock_bin" --check-dump "$work/ps5-whole" --media-seed 6 > "$work/seed.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/seed.out" " wrong 62 " && ok "checked against another stream: every frame wrong, exit 1" || fail "other seed: exit $rc: $(cat "$work/seed.out")"

echo "what the command line refuses"
"$app" session "Living Room" --dump > "$work/bad-dump.out" 2>&1; rc=$?
[ $rc -eq 64 ] && ok "--dump without a folder: exit 64" || fail "--dump without a folder: exit $rc"
"$app" session "Living Room" --dump /proc/nowhere/at/all > "$work/bad-folder.out" 2>&1; rc=$?
[ $rc -ne 0 ] && has "$work/bad-folder.out" "--dump:" && ok "--dump to a folder that cannot be made: refused before anything is sent" \
    || fail "--dump to a bad folder: exit $rc: $(cat "$work/bad-folder.out")"

if [ -n "$reference" ]; then
    for kind in ps5 ps4; do
        family=$(echo "$kind" | tr a-z A-Z)
        echo "$family: the reference's own client receives the same stream"
        rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
        start_mock "$kind-register2" --kind "$kind" --pin 12345678
        "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > /dev/null 2>&1
        stop_mock
        for run in whole repair; do
            [ $run = whole ] && trouble="" || trouble="--lose-every 3 --lose-units 2 --late-every 4"
            # shellcheck disable=SC2086
            start_mock "$kind-$run-ours" --kind "$kind" $stream $trouble
            "$app" session "Living Room" --seconds 3 --dump "$work/$kind-$run-ours" > "$work/$kind-$run-ours.out" 2>&1
            sleep 0.3; stop_mock
            mkdir -p "$work/$kind-$run-reference"
            # shellcheck disable=SC2086
            start_mock "$kind-$run-reference" --kind "$kind" $stream $trouble
            PSVP_REFERENCE_DUMP="$work/$kind-$run-reference" timeout 60 "$reference/psvp_reference_client" session 127.0.0.1 "$kind" 1a2b3c4d \
                a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 4 > "$work/$kind-$run-reference.out" 2>&1
            sleep 0.3; stop_mock
            out="$work/$kind-$run-reference.out"; log="$work/$kind-$run-reference.log"
            ours="$work/$kind-$run-ours"; theirs="$work/$kind-$run-reference"
            # The header of the picture size and 62 frames: 63 samples.
            check "$run: it took the header and all 62 frames from the mock" has "$out" "reference: 63 picture samples"
            check "$run: it found no wrong code on anything the mock sent" lacks "$out" "MAC mismatch"
            if [ -s "$ours/video.bin" ] && cmp -s "$ours/video.bin" "$theirs/video.bin"; then
                ok "$run: what it hands to a decoder is what this client kept, byte for byte ($(wc -c < "$ours/video.bin") bytes)"
            else
                fail "$run: the pictures differ: ours $(wc -c < "$ours/video.bin") bytes, the reference's $(wc -c < "$theirs/video.bin")"
            fi
            [ "$(picture "$ours")" = "$(picture "$work/$kind-$run")" ] && ok "$run: and it is what the mock sent" || fail "$run: $(picture "$ours")"
            # Each ran for its own time: the sound is compared as far as the shorter one goes.
            ours_size=$(wc -c < "$ours/sound.bin"); theirs_size=$(wc -c < "$theirs/sound.bin")
            shorter=$(( ours_size < theirs_size ? ours_size : theirs_size ))
            if [ "$shorter" -ge 4000 ] && cmp -s -n "$shorter" "$ours/sound.bin" "$theirs/sound.bin"; then
                ok "$run: its sound frames are this client's, byte for byte ($shorter bytes compared)"
            else
                fail "$run: the sound differs (ours $ours_size bytes, the reference's $theirs_size)"
            fi
            if [ $run = repair ]; then
                check "$run: the mock withheld packets from it too" has "$log" " withheld 40 "
                check "$run: it reported no frame as not arrived" lacks "$log" "reported as not arrived"
            fi
            check "$run: nothing it sent was rejected" lacks "$log" "REJECTED"
            # Its input states, decrypted by the mock the way this client's are.
            states=$(number "$log" 'input-states ([0-9]+) at-rest')
            between "$states" 5 60 && has "$log" "input-states $states at-rest $states unreadable 0 out-of-order 0" \
                && ok "$run: the mock decrypted its $states input states: each is the resting controller's that this client sends" \
                || fail "$run: its input states: $(grep -a 'stream summary' "$log")"
        done
    done
else
    echo "the reference's own client on the same stream: not run here (it needs the oracle build)"
fi

[ $failed = 0 ] && echo "PHASE 2D SCENARIO PASSED" || echo "PHASE 2D SCENARIO FAILED"
exit $failed
