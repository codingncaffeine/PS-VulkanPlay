#!/usr/bin/env bash
# Starting a session, end to end: the session request, the control channel, the
# session ID, heartbeats, the login passcode, "go to rest mode" and the stream
# transport's opening handshake, with the real program on real sockets against
# the mock console.
#
#   tools/wall.sh tests/scenario/phase2a.sh BUILD_DIR [REFERENCE_BUILD_DIR]
#
# With REFERENCE_BUILD_DIR the reference's own session code is run against the
# mock too, and what it sent is compared with what this client sent.
# Only on a network that is nobody's (see tests/scenario/phase1.sh).
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase2a.sh BUILD_DIR [REFERENCE_BUILD_DIR]}
reference=${2:-}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase2a: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/phase2a"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -qF -- "$2" "$1"; }

mock_pid=""
start_mock() {   # start_mock LOG_NAME OPTIONS...
    local log="$work/$1.log"; shift
    rm -f "$work/ready"
    "$mock_bin" --ready-file "$work/ready" --heartbeat-ms 200 "$@" > "$log" 2>&1 &
    mock_pid=$!
    for _ in $(seq 100); do [ -e "$work/ready" ] && return 0; sleep 0.05; done
    fail "the mock console did not start: $(head -3 "$log")"; return 1
}
stop_mock() { [ -n "$mock_pid" ] && { kill "$mock_pid" 2>/dev/null; wait "$mock_pid" 2>/dev/null; }; mock_pid=""; }
# The canonical line the mock writes for a request, without its "mock: " prefix.
line_of() { grep -m1 -F -- "$2" "$1" | sed 's/^mock: //'; }

for kind in ps5 ps4; do
    family=$(printf '%s' "$kind" | tr a-z A-Z)
    echo "$family: a session's control phase"
    rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
    start_mock "$kind-a" --kind "$kind" --name "Living Room" --pin 12345678
    "$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/$kind-register.out" 2>&1
    check "registered with the mock" has "$work/$kind-register.out" "Registered with Living Room ($family)"
    "$app" session "Living Room" --seconds 2 > "$work/$kind-session.out" 2>&1; rc=$?
    [ $rc -eq 0 ] && ok "session: exit 0" || fail "session: exit $rc: $(head -3 "$work/$kind-session.out")"
    check "the console accepted it and named its model" has "$work/$kind-session.out" "Living Room accepted the session ($family)."
    [ "$kind" = ps5 ] && id_length=16 || id_length=33
    check "the session ID arrived" has "$work/$kind-session.out" "Session ID received ($id_length characters)."
    answered=$(grep -o 'heartbeats answered: [0-9]*' "$work/$kind-session.out" | grep -o '[0-9]*$')
    # Two seconds at one request every 200 ms: about ten, never fewer than six.
    [ "${answered:-0}" -ge 6 ] && [ "${answered:-0}" -le 12 ] && ok "heartbeats were answered ($answered in 2 s)" || fail "heartbeats answered: ${answered:-none}"
    sleep 0.3
    check "the mock opened the registration key with the session cipher" has "$work/$kind-a.log" "control request accepted: auth ok device-id ok os-type 57696e31302e302e3000 bitrate 00000000"
    check "the mock counted the same heartbeats" has "$work/$kind-a.log" "heartbeats answered: $answered"
    check "the stream transport's handshake completed" has "$work/$kind-session.out" "Stream transport opened (handshake done, 100 streams each way)."
    check "the mock saw the handshake through" has "$work/$kind-a.log" "transport connected on port 9297"
    "$app" session "Living Room" --seconds 1 --rest > "$work/$kind-rest.out" 2>&1; rc=$?
    sleep 0.3
    [ $rc -eq 0 ] && has "$work/$kind-a.log" "rest mode requested" && ok "--rest sends the console to rest mode" || fail "--rest: exit $rc"
    "$app" discover --host 127.0.0.1 --wait 1 > "$work/$kind-after-rest.out" 2>&1
    check "discovery then shows rest mode" has "$work/$kind-after-rest.out" "standby"
    stop_mock
    cp "$work/$kind-a.log" "$work/$kind-ours.log"
done

echo "refusals"
start_mock in-use --kind ps4 --name "Living Room" --in-use yes
"$app" session "Living Room" --seconds 1 > "$work/in-use.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/in-use.out" "already in use" && ok "a console already in use: exit 1 and the reason" || fail "in use: exit $rc: $(head -2 "$work/in-use.out")"
stop_mock
start_mock stranger --kind ps4 --name "Living Room" --regist-key 0badc0de
"$app" session "Living Room" --seconds 1 > "$work/stranger.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/stranger.out" "register with it again" && ok "a console that no longer knows this client: exit 1 and what to do" || fail "stranger: exit $rc: $(head -2 "$work/stranger.out")"
stop_mock
"$app" session "Living Room" --seconds 1 > "$work/down.out" 2>&1; rc=$?
[ $rc -eq 1 ] && ok "no console at the address: exit 1" || fail "console down: exit $rc"
"$app" session "Nobody" > "$work/nobody.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/nobody.out" "No registered console" && ok "an unknown name: exit 1 and a reason" || fail "unknown console: exit $rc"

echo "login passcode"
start_mock passcode --kind ps4 --name "Living Room" --passcode 4321
"$app" session "Living Room" --seconds 1 > "$work/passcode-none.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/passcode-none.out" "asks for its login passcode" && ok "without one: exit 1 and how to give it" || fail "no passcode: exit $rc: $(head -2 "$work/passcode-none.out")"
"$app" session "Living Room" --seconds 1 --passcode 1111 > "$work/passcode-wrong.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/passcode-wrong.out" "did not accept the passcode" && ok "a wrong one: exit 1 and the reason" || fail "wrong passcode: exit $rc: $(head -2 "$work/passcode-wrong.out")"
"$app" session "Living Room" --seconds 1 --passcode 4321 > "$work/passcode-right.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/passcode-right.out" "Passcode accepted." && has "$work/passcode-right.out" "Session ID received" && ok "the right one: accepted, then the session ID" || fail "right passcode: exit $rc: $(head -3 "$work/passcode-right.out")"
sleep 0.2
check "the mock refused the wrong one" has "$work/passcode.log" "passcode refused"
check "the mock accepted the right one" has "$work/passcode.log" "passcode accepted"
stop_mock

if [ -n "$reference" ]; then
    ref="$reference/psvp_reference_client"
    for kind in ps5 ps4; do
        family=$(printf '%s' "$kind" | tr a-z A-Z)
        echo "$family: the reference's own session code against the mock"
        start_mock "$kind-reference" --kind "$kind" --name "Living Room"
        timeout 60 "$ref" session 127.0.0.1 "$kind" 1a2b3c4d a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 2 > "$work/$kind-reference.out" 2>&1
        sleep 0.3
        stop_mock
        check "it is accepted and goes on to the stream transport" has "$work/$kind-reference.out" "Takion sent init"
        check "the mock named the session for it" has "$work/$kind-reference.log" "session ID sent"
        check "its transport completes the handshake with the mock" has "$work/$kind-reference.out" "Takion connected"
        for request in "session request accepted" "control request accepted" "transport init on port 9297"; do
            ours="$(line_of "$work/$kind-ours.log" "$request")"
            theirs="$(line_of "$work/$kind-reference.log" "$request")"
            what="${request% accepted}"; what="${what% on port 9297}"
            if [ -n "$ours" ] && [ "$ours" = "$theirs" ]; then ok "its $what reads exactly like ours (${#ours} characters)"
            else fail "the ${what}s differ:"; printf '           ours:   %s\n           theirs: %s\n' "$ours" "$theirs" | cut -c1-400; fi
        done
    done
else
    echo "the reference's own session code against the mock: not run here (it needs the oracle build)"
fi

[ $failed = 0 ] && echo "PHASE 2A SCENARIO PASSED" || echo "PHASE 2A SCENARIO FAILED"
exit $failed
