#!/usr/bin/env bash
# Finding, registering with and waking a console, end to end: the real program
# on real sockets and the consoles' real ports, against the mock console.
#
#   tools/wall.sh tests/scenario/phase1.sh BUILD_DIR [REFERENCE_BUILD_DIR]
#     BUILD_DIR            holds ps-vulkanplay and psvp-mock-console
#     REFERENCE_BUILD_DIR  holds psvp_reference_client (the oracle build); with it
#                          the reference's own client is run against the mock too
#
# Only on a network that is nobody's: inside tools/wall.sh, or in a container
# whose starter set PSVP_TEST_ISOLATED_NET. It opens ports 987, 9302 and 9295
# and sends broadcasts.
# Exit: 0 passed | 1 a check failed | 2 not on an isolated network
set -uo pipefail
build=${1:?usage: tools/wall.sh tests/scenario/phase1.sh BUILD_DIR [REFERENCE_BUILD_DIR]}
reference=${2:-}
if [ -z "${PSVP_TEST_TMP:-}" ] || [ -z "${PSVP_TEST_ISOLATED_NET:-}" ]; then
    echo "phase1: run it through tools/wall.sh, on a network of its own" >&2; exit 2
fi
app="$build/ps-vulkanplay"
mock_bin="$build/psvp-mock-console"
here="$(cd "$(dirname "$0")" && pwd)"
work="$PSVP_TEST_TMP/phase1"
rm -rf "$work"; mkdir -p "$work"
account=ywT7cR8BAAA=          # 1234567890123
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
# check DESCRIPTION COMMAND...: passes when the command succeeds
check() { local what="$1"; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else fail "$what"; fi; }
has()  { grep -qF -- "$2" "$1"; }

# A network with only loopback has no route for a broadcast; give it one. This
# changes the wall's own network, never the machine's.
if [ "$PSVP_TEST_ISOLATED_NET" = 1 ] && command -v ip > /dev/null 2>&1 && [ -z "$(ip route show default 2>/dev/null)" ]; then
    ip route add default dev lo 2>/dev/null || true
fi

mock_pid=""
start_mock() {   # start_mock LOG_NAME OPTIONS...
    local log="$work/$1.log"; shift
    rm -f "$work/ready"
    "$mock_bin" --ready-file "$work/ready" "$@" > "$log" 2>&1 &
    mock_pid=$!
    for _ in $(seq 100); do [ -e "$work/ready" ] && return 0; sleep 0.05; done
    fail "the mock console did not start: $(head -3 "$log")"; return 1
}
stop_mock() { [ -n "$mock_pid" ] && { kill "$mock_pid" 2>/dev/null; wait "$mock_pid" 2>/dev/null; }; mock_pid=""; }
# Each part starts from an empty list of consoles.
fresh_profile() { rm -rf "$XDG_CONFIG_HOME/ps-vulkanplay"; }

echo "PS5: discovery"
start_mock ps5-a --kind ps5 --name "Living Room" --pin 12345678
"$app" discover --host 127.0.0.1 --wait 1 --json > "$work/discover.json" 2> "$work/discover.err"; rc=$?
[ $rc -eq 0 ] && ok "discover --host finds it (exit 0)" || fail "discover --host: exit $rc: $(cat "$work/discover.err")"
check "it reports the name" has "$work/discover.json" '"name": "Living Room"'
check "it reports the family" has "$work/discover.json" '"kind": "PS5"'
check "it reports the state" has "$work/discover.json" '"state": "ready"'
check "it reports the host ID" has "$work/discover.json" '"host_id": "02005E102030"'
check "it is not registered yet" has "$work/discover.json" '"registered": false'
"$app" discover --host 127.0.0.2 --wait 1 > "$work/none.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/none.out" "No console answered" && ok "an address with no console: exit 1 and a reason" || fail "empty address: exit $rc"
stop_mock

echo "PS5: discovery by broadcast"
start_mock ps5-b --kind ps5 --name "Broadcast Room" --bind 0.0.0.0
"$app" discover --wait 2 > "$work/broadcast.out" 2>&1; rc=$?
if [ $rc -eq 0 ] && has "$work/broadcast.out" "Broadcast Room"; then ok "discover without an address finds it"
else fail "broadcast discovery: exit $rc: $(head -3 "$work/broadcast.out")"; fi
stop_mock

echo "PS5: registration"
fresh_profile
start_mock ps5-c --kind ps5 --name "Living Room" --pin 12345678 --account-id "$account" --dump "$work"
"$app" register --host 127.0.0.1 --pin 12345678 > "$work/noaccount.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/noaccount.out" "account ID" && ok "without an account ID: exit 1 and a reason" || fail "no account: exit $rc"
"$app" register --host 127.0.0.1 --pin 87654321 --account-id 1234567890123 > "$work/wrongpin.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/wrongpin.out" "refused" && has "$work/wrongpin.out" "80108b09" && ok "wrong PIN: refused, with the console's reason" || fail "wrong PIN: exit $rc: $(head -2 "$work/wrongpin.out")"
"$app" register --host 127.0.0.1 --pin 12345678 --account-id 999 > "$work/wrongacct.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/wrongacct.out" "refused" && ok "another account: refused" || fail "wrong account: exit $rc"
[ ! -e "$XDG_CONFIG_HOME/ps-vulkanplay/consoles.json" ] && ok "nothing was stored by the refused attempts" || fail "a refused registration left a file"
"$app" register --host 127.0.0.1 --pin "1234 5678" --account-id 1234567890123 > "$work/register.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/register.out" "Registered with Living Room (PS5)" && ok "right PIN: registered" || fail "register: exit $rc: $(head -3 "$work/register.out")"
store="$XDG_CONFIG_HOME/ps-vulkanplay/consoles.json"
[ "$(stat -c %a "$store" 2>/dev/null)" = 600 ] && ok "the list of consoles is readable by its owner only" || fail "store mode: $(stat -c %a "$store" 2>&1)"
check "it holds the registration key" has "$store" '"regist_key": "1a2b3c4d"'
check "it holds the shared secret" has "$store" '"key": "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"'
check "it holds the account" has "$store" "\"account_id\": \"$account\""
"$app" consoles > "$work/consoles.out" 2>&1
check "consoles lists it" has "$work/consoles.out" "Living Room  PS5  02:00:5e:10:20:30  127.0.0.1"
"$app" discover --host 127.0.0.1 --wait 1 --json > "$work/discover2.json" 2>&1
check "discovery now says registered" has "$work/discover2.json" '"registered": true'
check "the mock saw the wrong PIN" has "$work/ps5-c.log" "registration refused: wrong PIN"
check "the mock saw the wrong account" has "$work/ps5-c.log" "is not this console's"
check "the mock accepted the right one" has "$work/ps5-c.log" "registration accepted: account $account key-offset 1 wrap-offset 8"
our_head="$(ls "$work"/regist-*.head | tail -1)"
stop_mock

echo "PS5: not showing a PIN"
start_mock ps5-d --kind ps5 --name "Living Room"
"$app" register --host 127.0.0.1 --pin 12345678 > "$work/nopin.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/nopin.out" "not taking registrations" && has "$work/nopin.out" "Link Device" && ok "says where the PIN screen is" || fail "no PIN screen: exit $rc: $(head -2 "$work/nopin.out")"
stop_mock

echo "PS5: waking from rest mode"
start_mock ps5-e --kind ps5 --name "Living Room" --state standby
"$app" discover --host 127.0.0.1 --wait 1 > "$work/standby.out" 2>&1
check "discovery shows rest mode" has "$work/standby.out" "standby"
"$app" register --host 127.0.0.1 --pin 12345678 > "$work/standby-reg.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/standby-reg.out" "rest mode" && ok "registering with a sleeping console: exit 1 and a reason" || fail "standby register: exit $rc"
"$app" wake "living room" --wait 10 > "$work/wake.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/wake.out" "Living Room is on." && ok "wake by name: the console comes on" || fail "wake: exit $rc: $(head -2 "$work/wake.out")"
check "the mock accepted the wake-up" has "$work/ps5-e.log" "wake-up accepted, now on"
stop_mock
start_mock ps5-f --kind ps5 --name "Living Room" --state standby --regist-key 0badc0de
"$app" wake 02:00:5e:10:20:30 --wait 3 > "$work/wake2.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/wake2.out" "did not come on" && ok "a console that does not know this client stays asleep (exit 1)" || fail "foreign wake: exit $rc"
check "the mock refused that wake-up" has "$work/ps5-f.log" "wake-up refused"
stop_mock
"$app" wake "Nobody" > "$work/wake3.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/wake3.out" "No registered console" && ok "an unknown name: exit 1 and a reason" || fail "unknown console: exit $rc"

echo "PS4"
fresh_profile
start_mock ps4-a --kind ps4 --name "Den" --pin 11112222
"$app" discover --host 127.0.0.1 --wait 1 > "$work/ps4.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/ps4.out" "Den  PS4  ready" && ok "discover finds it on port 987" || fail "PS4 discover: exit $rc: $(head -2 "$work/ps4.out")"
"$app" register --host 127.0.0.1 --pin 11112222 --account-id "$account" > "$work/ps4-reg.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/ps4-reg.out" "Registered with Den (PS4)" && ok "registered" || fail "PS4 register: exit $rc: $(head -2 "$work/ps4-reg.out")"
check "the mock accepted it" has "$work/ps4-a.log" "registration accepted"
stop_mock
start_mock ps4-b --kind ps4 --name "Old Den" --pin 11112222 --system-version 07510000
"$app" register --host 127.0.0.1 --pin 11112222 > "$work/ps4-old.out" 2>&1; rc=$?
[ $rc -eq 1 ] && has "$work/ps4-old.out" "too old" && ok "system software before 8.00: exit 1 and a reason" || fail "old PS4: exit $rc: $(head -2 "$work/ps4-old.out")"
stop_mock

echo "the list of consoles"
"$app" forget Den > "$work/forget.out" 2>&1; rc=$?
[ $rc -eq 0 ] && ok "forget removes a console" || fail "forget: exit $rc"
"$app" consoles > "$work/consoles2.out" 2>&1
check "the list is empty again" has "$work/consoles2.out" "No console is registered yet"

echo "import from chiaki-ng"
fresh_profile
"$app" import "$here/fixtures/Chiaki.conf" > "$work/import.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/import.out" "2 consoles imported" && ok "import takes the registered consoles over" || fail "import: exit $rc: $(cat "$work/import.out")"
check "it reports the one it cannot use" has "$work/import.out" "older than 8.00"
"$app" consoles --json > "$work/imported.json" 2>&1
check "the PS5 is in the list" has "$work/imported.json" '"name": "Wohnzimmer PS5; \"Süd\", =1"'
check "the PS4 is in the list" has "$work/imported.json" '"kind": "PS4"'
"$app" account > "$work/account.out" 2>&1
check "the account ID came with it" has "$work/account.out" "$account"
"$app" import "$here/fixtures/Chiaki.conf" > "$work/import2.out" 2>&1
check "a second import adds nothing" has "$work/import2.out" "0 consoles imported"
# The imported registration must wake the console it was made with.
start_mock import-wake --kind ps5 --name "Imported" --state standby --regist-key 5e6f7a8b --bind 0.0.0.0
"$app" wake 02:00:5e:10:20:30 --wait 10 > "$work/import-wake.out" 2>&1; rc=$?
[ $rc -eq 0 ] && has "$work/import-wake.log" "wake-up accepted" && ok "an imported registration wakes its console (found by broadcast)" || fail "imported wake: exit $rc: $(head -2 "$work/import-wake.out")"
stop_mock

if [ -n "$reference" ]; then
    echo "the reference's own client against the mock"
    ref="$reference/psvp_reference_client"
    mkdir -p "$work/ref"
    start_mock ref-a --kind ps5 --name "Living Room" --pin 12345678 --dump "$work/ref"
    "$ref" discover 127.0.0.1 ps5 > "$work/ref-discover.out" 2>&1
    check "it finds the mock" has "$work/ref-discover.out" "reference: found name=Living Room type=PS5 state=ready host-id=02005E102030"
    "$ref" register 127.0.0.1 ps5 "$account" 87654321 > "$work/ref-wrong.out" 2>&1; rc=$?
    [ $rc -eq 1 ] && ok "it is refused with the wrong PIN" || fail "reference wrong PIN: exit $rc"
    "$ref" register 127.0.0.1 ps5 "$account" 12345678 > "$work/ref-register.out" 2>&1
    check "it registers and reads the same keys" has "$work/ref-register.out" "reference: registered nickname=Living Room mac=02005e102030 regist-key=1a2b3c4d key-type=2 key=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
    ref_head="$(ls "$work/ref"/regist-*.head | tail -1)"
    if cmp -s "$ref_head" "$our_head"; then ok "its request head is byte for byte the one we send ($(wc -c < "$our_head") bytes)"
    else fail "request heads differ: $(cmp "$ref_head" "$our_head" 2>&1 | head -1)"; fi
    stop_mock
    start_mock ref-b --kind ps5 --name "Living Room" --state standby
    "$ref" wake 127.0.0.1 ps5 1a2b3c4d > /dev/null 2>&1; sleep 0.3
    check "its wake-up is accepted" has "$work/ref-b.log" "wake-up accepted, now on"
    stop_mock
    start_mock ref-c --kind ps4 --name "Den" --pin 11112222
    "$ref" discover 127.0.0.1 ps4 > "$work/ref-ps4.out" 2>&1
    check "it finds the PS4 mock" has "$work/ref-ps4.out" "reference: found name=Den type=PS4"
    "$ref" register 127.0.0.1 ps4 "$account" 11112222 > "$work/ref-ps4-reg.out" 2>&1
    check "it registers with the PS4 mock" has "$work/ref-ps4-reg.out" "reference: registered nickname=Den"
    stop_mock
else
    echo "the reference's own client against the mock: not run here (it needs the oracle build)"
fi

[ $failed = 0 ] && echo "PHASE 1 SCENARIO PASSED" || echo "PHASE 1 SCENARIO FAILED"
exit $failed
