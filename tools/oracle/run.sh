#!/usr/bin/env bash
# The differential tests: our protocol code against the reference implementation
# (chiaki-ng), on the same inputs.
#
# The reference is fetched at a pinned commit into scratch/oracle/ and compiled
# into a test program only. It is never copied into this repository and never
# part of anything that ships.
#
#   tools/oracle/run.sh             the comparisons (all must agree), then the control:
#                                   one of our results corrupted on purpose, which the
#                                   comparison must report
#   tools/oracle/run.sh --vectors   prints results computed by the reference for fixed
#                                   inputs, as the C++ that tests/unit/vectors_regist.h holds
#   tools/oracle/run.sh --vectors-stream   the same for tests/unit/vectors_stream.h
#
# The reference's library needs json-c, libevent, Opus, OpenSSL and a curl with
# WebSocket support on this machine; it fetches miniupnpc itself.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh

repo=https://github.com/chiaki-ng/chiaki-ng.git
pin=43728af2e639fecb8a7af1cd68c29820a40de973
ref=scratch/oracle/reference

if [ "$(git -C "$ref" rev-parse HEAD 2>/dev/null || true)" != "$pin" ]; then
    rm -rf "$ref"
    mkdir -p "$ref"
    git -C "$ref" init -q
    git -C "$ref" remote add origin "$repo"
    git -C "$ref" fetch -q --depth 1 origin "$pin"
    git -C "$ref" checkout -q FETCH_HEAD
fi
# The parts of the reference that its library is built from.
git -C "$ref" submodule update -q --init --depth 1 third-party/nanopb third-party/jerasure third-party/gf-complete

dir=build/oracle
if ! configure_output="$(psvp_nice cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSVP_WERROR=ON \
        -DPSVP_BUILD_APP=OFF "-DPSVP_ORACLE_DIR=$PWD/$ref" 2>&1)"; then
    printf '%s\n' "$configure_output"
    exit 1
fi
psvp_nice cmake --build "$dir" --target psvp_oracle_tests psvp_reference_client psvp-mock-console -j "$PSVP_JOBS" > "$dir/build.log" 2>&1 || {
    tail -40 "$dir/build.log"; echo "ORACLE FAILED: the test program did not build"; exit 1; }

if [ "${1:-}" = "--vectors" ] || [ "${1:-}" = "--vectors-stream" ]; then
    exec "$dir/psvp_oracle_tests" "$1"
fi

"$dir/psvp_oracle_tests"
set +e
"$dir/psvp_oracle_tests" --plant > "$dir/plant.log" 2>&1
rc=$?
set -e
# Each planted difference must be reported by name, and nothing else may be.
planted=("base64 encode" "stream shared secret" "stream key stream" "stream authentication code" "stream packet code"
         "stream launch specification" "stream sealed launch specification" "stream opening message"
         "erasure repair units" "erasure repaired units" "media packet header fields" "media frame put together"
         "media congestion report" "media frames-not-arrived message" "media input state")
missing=""
for what in "${planted[@]}"; do
    grep -q "MISMATCH $what" "$dir/plant.log" || missing="$missing [$what]"
done
if [ $rc -eq 1 ] && [ -z "$missing" ] && [ "$(grep -c MISMATCH "$dir/plant.log")" = "${#planted[@]}" ]; then
    echo "control ok: the ${#planted[@]} planted differences were reported, each by name, and nothing else"
else
    cat "$dir/plant.log"
    echo "ORACLE FAILED: planted differences not reported as planted (exit $rc; missing:$missing)"; exit 1
fi
echo "ORACLE PASSED (reference at ${pin:0:7})"
