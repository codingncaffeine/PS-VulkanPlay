#!/usr/bin/env bash
# Builds the fuzzers (clang, libFuzzer, AddressSanitizer) and runs one for a while.
# New inputs it finds stay in scratch/fuzz/; the saved inputs under
# tests/fuzz/corpus/ are the starting set and are not changed.
#
#   tools/fuzz.sh [SECONDS, default 30] [TARGET, default: every target, SECONDS each]
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
seconds="${1:-30}"

tools/build.sh fuzz > /dev/null
if [ $# -lt 2 ]; then
    for file in tests/fuzz/fuzz_*.cpp; do
        name="${file##*/fuzz_}"
        "$0" "$seconds" "${name%.cpp}"
    done
    exit 0
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
