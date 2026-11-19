#!/usr/bin/env bash
# The network probe, end to end: after a session's control phase the real
# program opens the probe port's transport, exchanges numbered messages with
# the mock console and measures the round trip and the largest packet each way.
#
#   tools/wall.sh tests/scenario/phase2b.sh BUILD_DIR [REFERENCE_BUILD_DIR]
#
# With REFERENCE_BUILD_DIR the reference's own probe is run against the mock
# too: it must measure what the mock was told to let through, and every message
# it sent must be, byte for byte, the one this client sent.
# Only on a network that is nobody's (see tests/scenario/phase1.sh).
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase2b.sh BUILD_DIR [REFERENCE_BUILD_DIR]}
reference=${2:-}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase2b: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
work="$PSVP_TEST_TMP/phase2b"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -aqF -- "$2" "$1"; }

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
# What a client sent in the probe, as the mock recorded it.
messages_of() { grep -a -e "probe message" -e "probe goodbye" "$1" | sed 's/^mock: //'; }

rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"
start_mock register --pin 12345678
"$app" register --host 127.0.0.1 --pin 12345678 --account-id 1234567890123 > "$work/register.out" 2>&1
check "registered with the mock" has "$work/register.out" "Registered with Living Room (PS5)"
stop_mock

echo "a path that lets everything through"
start_mock open
"$app" session "Living Room" --seconds 1 > "$work/open.out" 2>&1; rc=$?
sleep 0.3; stop_mock
[ $rc -eq 0 ] && ok "session: exit 0" || fail "session: exit $rc: $(head -3 "$work/open.out")"
check "ten of ten pings answered, both sizes at the ceiling" has "$work/open.out" "(10 of 10 pings answered), largest packet in 1454, out 1454."
check "the mock saw the version request first" has "$work/open.log" "probe message channel 1 flags 1 type 0: 081ffa01020809"
pings=$(grep -ac "probe ping 548 bytes" "$work/open.log")
[ "$pings" = 10 ] && ok "the mock counted ten pings of 548 bytes" || fail "pings of 548 bytes: $pings"
check "the mock got the goodbye" has "$work/open.log" "probe goodbye: Client Disconnecting"

echo "a path with a smaller limit each way"
for sizes in "1300 1203" "576 1454" "1001 577"; do
    set -- $sizes
    start_mock "limit-$1-$2" --mtu-in "$1" --mtu-out "$2"
    "$app" session "Living Room" --seconds 1 > "$work/limit-$1-$2.out" 2>&1
    sleep 0.3; stop_mock
    check "in $1, out $2: measured exactly" has "$work/limit-$1-$2.out" "largest packet in $1, out $2."
done

echo "a console that does not answer the probe"
start_mock silent --probe no
"$app" session "Living Room" --seconds 1 > "$work/silent.out" 2>&1; rc=$?
stop_mock
[ $rc -eq 0 ] && ok "session: exit 0 all the same" || fail "session: exit $rc"
check "it says why, and what a stream would use" has "$work/silent.out" \
    "The network could not be measured (the console did not answer the probe's version request); a stream would start with the defaults (largest packet 1454)."

if [ -n "$reference" ]; then
    echo "the reference's own probe against the mock"
    start_mock ours-limited --mtu-in 1300 --mtu-out 1203
    "$app" session "Living Room" --seconds 1 > "$work/ours-limited.out" 2>&1
    sleep 0.3; stop_mock
    start_mock reference --mtu-in 1300 --mtu-out 1203
    timeout 60 "$reference/psvp_reference_client" session 127.0.0.1 ps5 1a2b3c4d a0a1a2a3a4a5a6a7a8a9aaabacadaeaf 2 > "$work/reference.out" 2>&1
    sleep 0.3; stop_mock
    check "it measures the inbound limit the mock was given" has "$work/reference.out" "determined inbound MTU 1300"
    check "it measures the outbound limit the mock was given" has "$work/reference.out" "determined outbound MTU 1203"
    check "it goes on to the stream connection" has "$work/reference.out" "StreamConnection"
    messages_of "$work/ours-limited.log" > "$work/ours.messages"
    messages_of "$work/reference.log" > "$work/reference.messages"
    count=$(wc -l < "$work/ours.messages")
    if [ "$count" -ge 20 ] && cmp -s "$work/ours.messages" "$work/reference.messages"; then
        ok "its $count messages are byte for byte the ones this client sent"
    else
        fail "the probe messages differ ($count of ours, $(wc -l < "$work/reference.messages") of its):"
        diff "$work/ours.messages" "$work/reference.messages" | head -6 | cut -c1-160
    fi
else
    echo "the reference's own probe against the mock: not run here (it needs the oracle build)"
fi

[ $failed = 0 ] && echo "PHASE 2B SCENARIO PASSED" || echo "PHASE 2B SCENARIO FAILED"
exit $failed
