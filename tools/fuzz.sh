#!/usr/bin/env bash
# Builds the fuzzers (clang, libFuzzer, AddressSanitizer) and runs one for a while.
# New inputs it finds stay in scratch/fuzz/; the saved inputs under
# tests/fuzz/corpus/ are the starting set and are not changed.
#
#   tools/fuzz.sh [SECONDS, default 30] [TARGET, default: every target, SECONDS each,
#                                                 as many at once as the machine's share allows]
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
seconds="${1:-30}"

# Built once, also when every target is run: each of them is then started from here again.
if [ -z "${PSVP_FUZZ_BUILT:-}" ]; then
    tools/build.sh fuzz > /dev/null
    export PSVP_FUZZ_BUILT=1
fi
if [ $# -lt 2 ]; then
    # Each target is one process on one core.
    rc=0
    printf '%s\n' tests/fuzz/fuzz_*.cpp | sed 's|.*/fuzz_||; s|\.cpp$||' | xargs -P "$PSVP_JOBS" -I{} "$0" "$seconds" {} || rc=1
    exit $rc
fi
target="$2"
work="scratch/fuzz/$target"
mkdir -p "$work/found" "$work/crashes"
psvp_nice "build/fuzz/psvp_fuzz_$target" -max_total_time="$seconds" -print_final_stats=1 \
    -artifact_prefix="$work/crashes/" "$work/found" "tests/fuzz/corpus/$target" > "$work/log" 2>&1 || {
    tail -30 "$work/log"; echo "FUZZ FAILED ($target): see $work/crashes"; exit 1; }
runs="$(grep -o 'stat::number_of_executed_units: [0-9]*' "$work/log" | grep -o '[0-9]*$')"
[ "${runs:-0}" -gt 0 ] || { tail -10 "$work/log"; echo "FUZZ FAILED ($target): nothing ran"; exit 1; }
echo "FUZZ PASSED ($target): $runs inputs in $seconds s, no failure"
