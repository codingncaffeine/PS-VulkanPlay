#!/usr/bin/env bash
# The controller, end to end: while a stream is held the real program presses
# buttons ('session --tap'); the mock console decrypts the input history packets
# and tells which button changes it read.
#
#   tools/wall.sh tests/scenario/phase2e.sh BUILD_DIR [REFERENCE_BUILD_DIR]
#
# With REFERENCE_BUILD_DIR the reference's own client presses the same buttons:
# the mock must read the same changes, with the same numbers, from both.
# Only on a network that is nobody's (see tests/scenario/phase1.sh).
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase2e.sh BUILD_DIR [REFERENCE_BUILD_DIR]}
reference=${2:-}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase2e: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/phase2e"
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
# The button changes the mock read, in the order it read them.
told() { grep -a "stream input " "$1" | sed -E 's/^mock: stream //'; }
expected="input 0: right pressed
input 1: right released
input 2: cross pressed
input 3: cross released"

for kind in ps4 ps5; do
    family=$(echo "$kind" | tr a-z A-Z)
    rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
    start_mock "$kind-register" --kind "$kind" --pin 12345678
    "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/$kind-register.out" 2>&1
    check "$family: registered with the mock" has "$work/$kind-register.out" "Registered with Living Room ($family)"
    stop_mock

    echo "$family: two buttons are pressed for a moment"
    start_mock "$kind-taps" --kind "$kind" --frame-ms 16 --media-ms 10
    "$app" session "Living Room" --seconds 3 --tap right@1,cross@1.5 > "$work/$kind-taps.out" 2>&1; rc=$?
    sleep 0.3; stop_mock
    out="$work/$kind-taps.out"; log="$work/$kind-taps.log"
    [ $rc -eq 0 ] && ok "session --tap: exit 0" || fail "session --tap: exit $rc: $(tail -2 "$out")"
    check "the program says when it pressed and released each" [ "$(grep -acE '^[0-9.]+ s: (right|cross) (pressed|released)\.$' "$out")" = 4 ]
    [ "$(told "$log")" = "$expected" ] && ok "the mock read the four changes, each once, in order, numbered from 0" || fail "the mock read: $(told "$log" | tr '\n' ';')"
    packets=$(number "$out" 'Buttons: 4 changes told to the console in ([0-9]+) input history packets')
    # Each change's own packet, and the packet of each release four times more (20, 50, 100 and 200 ms later);
    # a press's packet goes out again until its release follows, 100 ms later: two or three times.
    between "$packets" 14 18 && ok "in $packets packets: every change's own, and the last one sent again until the next change" || fail "history packets: '$packets': $(grep -a '^Buttons' "$out")"
    check "the mock found every one of them readable, and each change once" grep -aqE "history-packets $packets events 4 unreadable 0" "$log"
    check "their key positions follow the rule" grep -aqE 'key-positions [0-9]+ out-of-step 0 ' "$log"
    check "nothing the client sent was refused" lacks "$log" "REJECTED"
    check "the stream went on: frames were put together all the while" grep -aqE 'Picture: 1[6-9][0-9] frames put together' "$out"
done

echo "what the command line refuses"
"$app" session "Living Room" --tap start@1 > "$work/bad-button.out" 2>&1; rc=$?
[ $rc -eq 64 ] && has "$work/bad-button.out" "--tap takes buttons" && ok "a button that does not exist: exit 64 and the names" || fail "bad button: exit $rc"
"$app" session "Living Room" --tap right > "$work/bad-time.out" 2>&1; rc=$?
[ $rc -eq 64 ] && ok "a button without its second: exit 64" || fail "no second: exit $rc"
"$app" session "Living Room" --tap right@1.2345 > "$work/bad-decimals.out" 2>&1; rc=$?
[ $rc -eq 64 ] && ok "a second with more than three decimals: exit 64" || fail "decimals: exit $rc"

if [ -n "$reference" ]; then
    for kind in ps5 ps4; do
        family=$(echo "$kind" | tr a-z A-Z)
        echo "$family: the reference's own client presses the same buttons"
        rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
        start_mock "$kind-register2" --kind "$kind" --pin 12345678
        "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > /dev/null 2>&1
        stop_mock
        start_mock "$kind-reference" --kind "$kind"
        PSVP_REFERENCE_TAPS=1 timeout 60 "$reference/psvp_reference_client" session 127.0.0.1 "$kind" 1a2b3c4d a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 4 \
            > "$work/$kind-reference.out" 2>&1
        sleep 0.3; stop_mock
        log="$work/$kind-reference.log"
        [ "$(told "$log")" = "$expected" ] && ok "the mock read from it what it read from this client: the same four changes with the same numbers" \
            || fail "from the reference the mock read: $(told "$log" | tr '\n' ';')"
        check "every history packet it sent was readable" grep -aqE 'history-packets [1-9][0-9]* events 4 unreadable 0' "$log"
        check "nothing it sent was rejected" lacks "$log" "REJECTED"
    done
else
    echo "the reference's own client pressing the same buttons: not run here (it needs the oracle build)"
fi

[ $failed = 0 ] && echo "PHASE 2E SCENARIO PASSED" || echo "PHASE 2E SCENARIO FAILED"
exit $failed
