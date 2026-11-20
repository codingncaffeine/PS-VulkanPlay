#!/usr/bin/env bash
# Decoding, end to end: the mock console sends the generated clip (pictures that
# show their frame's number, a tone as sound), the real program decodes what
# arrives ('session --expect-clip') and compares every picture with the frame of
# the clip it should be, sample for sample, and the sound with the tone.
#
#   tools/wall.sh tests/scenario/phase3a.sh BUILD_DIR
#
# Only on a network that is nobody's (see tests/scenario/phase1.sh).
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase3a.sh BUILD_DIR}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase3a: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/phase3a"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }
lacks() { ! grep -aqF -- "$2" "$1"; }
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

# What the report lines say, as numbers.
pictures()  { number "$1" 'Decoded \(H\.264, FFmpeg.s h264 decoder in software\): ([0-9]+) pictures from [0-9]+ frames \(416x240, 8 bits, limited range, BT\.709\)'; }
frames()    { number "$1" 'Decoded [^:]*: [0-9]+ pictures from ([0-9]+) frames'; }
clip_right() { number "$1" 'Test clip: ([0-9]+) pictures are their frame.s, sample for sample; 0 are not;'; }
in_repair() { number "$1" 'Test clip: .*; ([0-9]+) more are not between a lost frame and its repair'; }
longest()   { number "$1" 'the longest wait: ([0-9]+) pictures'; }
sound_frames() { number "$1" 'Sound decoded \(Opus\): ([0-9]+) frames \(480 samples a frame, 48000 Hz, 2 channels\); filled in for frames that did not arrive: 0; unreadable: 0\.'; }
left()      { number "$1" 'Test tone: 440 Hz is ([0-9]+)% of the left channel'; }
right()     { number "$1" '880 Hz is ([0-9]+)% of the right'; }

for kind in ps4 ps5; do
    family=$(echo "$kind" | tr a-z A-Z)
    rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
    start_mock "$kind-register" --kind "$kind" --pin 12345678
    "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/$kind-register.out" 2>&1
    check "$family: registered with the mock" has "$work/$kind-register.out" "Registered with Living Room ($family)"
    stop_mock

    echo "$family: the clip arrives whole"
    start_mock "$kind-clean" --kind "$kind" --clip 416x240 --frame-ms 16 --media-ms 10
    "$app" session "Living Room" --seconds 3 --expect-clip --dump "$work/$kind-dump" > "$work/$kind-clean.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-clean.out"
    [ $rc -eq 0 ] && ok "session --expect-clip: exit 0" || fail "session --expect-clip: exit $rc: $(tail -3 "$out")"
    check "the stream information names the clip's size" has "$out" "picture 416x240"
    n=$(pictures "$out")
    between "$n" 150 200 && ok "$n pictures came out of FFmpeg's H.264 decoder: 416x240, 8 bits, limited range, BT.709" || fail "pictures: '$n': $(grep -a '^Decoded' "$out")"
    check "one for every frame, none marked as damaged, no frame without a picture" grep -aqE "pictures from $n frames \([^)]*\); marked as damaged: 0; frames that gave no picture: 0;" "$out"
    [ "$(clip_right "$out")" = "$n" ] && ok "every one of them is its frame of the clip, sample for sample" || fail "clip: $(grep -a '^Test clip' "$out")"
    check "none was waiting for a repair" [ "$(in_repair "$out")" = 0 ]
    s=$(sound_frames "$out")
    between "$s" 250 320 && ok "$s sound frames decoded, none filled in, none unreadable" || fail "sound: '$s': $(grep -a '^Sound decoded' "$out")"
    between "$(left "$out")" 90 100 && between "$(right "$out")" 90 100 && ok "the tone is there: 440 Hz left, 880 Hz right" || fail "tone: $(grep -a '^Test tone' "$out")"

    echo "$family: what was kept decodes the same way without a console"
    "$app" decode "$work/$kind-dump" --expect-clip > "$work/$kind-decode.out" 2>&1; rc=$?
    [ $rc -eq 0 ] && ok "decode --expect-clip: exit 0" || fail "decode: exit $rc: $(tail -3 "$work/$kind-decode.out")"
    check "the same number of pictures, every one its frame's" [ "$(clip_right "$work/$kind-decode.out")" = "$n" ]
    check "the same number of sound frames" [ "$(sound_frames "$work/$kind-decode.out")" = "$s" ]

    echo "$family: one packet of every seventh frame is lost, and restored"
    start_mock "$kind-restored" --kind "$kind" --clip 416x240 --frame-ms 16 --media-ms 10 --lose-every 7 --lose-units 1
    "$app" session "Living Room" --seconds 3 --expect-clip > "$work/$kind-restored.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-restored.out"
    [ $rc -eq 0 ] && ok "exit 0" || fail "exit $rc: $(tail -3 "$out")"
    restored=$(number "$out" 'Picture: [0-9]+ frames put together \([0-9]+ bytes\), ([0-9]+) of them with restored packets')
    between "$restored" 20 30 && ok "$restored frames were put together with a restored packet" || fail "restored: '$restored': $(grep -a '^Picture' "$out")"
    n=$(pictures "$out")
    [ -n "$n" ] && [ "$(clip_right "$out")" = "$n" ] && ok "all $n pictures are their frame's: a restored frame is the frame" || fail "clip: $(grep -a '^Test clip' "$out")"

    echo "$family: every fortieth frame is lost whole; the mock repairs the picture as a console does"
    start_mock "$kind-lost" --kind "$kind" --clip 416x240 --frame-ms 16 --media-ms 10 --lose-every 40 --lose-units 999 --lose-sound-every 50
    "$app" session "Living Room" --seconds 3 --expect-clip > "$work/$kind-lost.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-lost.out"; log="$work/$kind-lost.log"
    [ $rc -eq 0 ] && ok "exit 0: the pictures outside a repair are all their frame's" || fail "exit $rc: $(tail -3 "$out")"
    lost=$(number "$out" 'Picture: .* ([0-9]+) frames lost;')
    between "$lost" 3 5 && ok "$lost frames were lost" || fail "lost: '$lost': $(grep -a '^Picture' "$out")"
    check "the mock was told of each" [ "$(grep -ac 'reported as not arrived' "$log")" = "$lost" ]
    n=$(pictures "$out"); f=$(frames "$out")
    [ -n "$n" ] && [ "$n" = "$f" ] && ok "the decoder went on: a picture for each of the $f frames that arrived" || fail "pictures '$n' of frames '$f'"
    waiting=$(in_repair "$out")
    # After each lost frame the next few show what the decoder kept; then the repair arrives. How many
    # depends on how soon the report gets through: one or two on an idle machine, more on a busy one.
    between "$waiting" "$lost" $((lost * 6)) && ok "$waiting pictures were not their frame's, all between a lost frame and its repair" || fail "in repair: '$waiting': $(grep -a '^Test clip' "$out")"
    between "$(longest "$out")" 1 6 && ok "the picture was right again after $(longest "$out") pictures at most, without a keyframe" || fail "longest wait: $(grep -a '^Test clip' "$out")"
    [ "$(clip_right "$out")" = $((n - waiting)) ] && ok "the other $((n - waiting)) are their frame's, sample for sample" || fail "clip: $(grep -a '^Test clip' "$out")"
    check "only the first frame was a keyframe" [ "$(grep -ac ', a keyframe' "$log")" = 1 ]
    check "lost sound packets cost nothing: their frames came with the next packet" grep -aqE 'Sound decoded \(Opus\): [0-9]+ frames \([^)]*\); filled in for frames that did not arrive: 0; unreadable: 0\.' "$out"
    between "$(left "$out")" 90 100 && between "$(right "$out")" 90 100 && ok "and the tone is whole" || fail "tone: $(grep -a '^Test tone' "$out")"

    echo "$family: the checks can fail"
    start_mock "$kind-pattern" --kind "$kind" --frame-ms 16 --media-ms 10
    "$app" session "Living Room" --seconds 2 --expect-clip > "$work/$kind-pattern.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-pattern.out"
    [ $rc -ne 0 ] && ok "a stream that is not the clip: exit $rc" || fail "a stream that is not the clip passed"
    check "it says that the picture header is no decoder's" has "$out" "Pictures could not be decoded: the picture header of 8 bytes is neither H.264's nor HEVC's."
    check "and that this is not the test clip" has "$out" "what was decoded is not the test clip"
done

"$app" decode "$work/nothing-here" > "$work/decode-missing.out" 2>&1; rc=$?
[ $rc -ne 0 ] && has "$work/decode-missing.out" "is not a folder that 'session --dump' wrote" && ok "decode of a folder that is no dump: refused, exit $rc" || fail "decode of a missing folder: exit $rc"

[ $failed -eq 0 ] && echo "PHASE 3A PASSED" || echo "PHASE 3A FAILED"
exit $failed
