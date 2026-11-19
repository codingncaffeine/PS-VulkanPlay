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

if [ "${1:-}" = "--vectors" ]; then
    exec "$dir/psvp_oracle_tests" --vectors
fi

"$dir/psvp_oracle_tests"
set +e
"$dir/psvp_oracle_tests" --plant > "$dir/plant.log" 2>&1
rc=$?
set -e
if [ $rc -eq 1 ] && grep -q 'MISMATCH base64 encode' "$dir/plant.log"; then
    echo "control ok: the planted difference was reported ($(grep -c MISMATCH "$dir/plant.log") line)"
else
    cat "$dir/plant.log"
    echo "ORACLE FAILED: the planted difference was not reported (exit $rc)"; exit 1
fi
echo "ORACLE PASSED (reference at ${pin:0:7})"
