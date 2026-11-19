#!/usr/bin/env bash
# Builds and checks the working tree from source inside a throwaway container,
# the way a user of that distribution would: its own compiler and libraries, a
# clean copy of the files (tracked, and new ones that are not ignored).
#
#   tools/container-check.sh IMAGE [deps: clang] [-- build-and-check options]
#   tools/container-check.sh docker.io/library/debian:13
#   tools/container-check.sh docker.io/library/debian:13 clang -- --compiler clang-libcxx
#
# Uses a quarter of the cores at low priority unless PSVP_JOBS says otherwise.
set -uo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
image="${1:?usage: tools/container-check.sh IMAGE [clang] [-- build-and-check options]}"; shift
deps=""
if [ "${1:-}" = clang ]; then deps=clang; shift; fi
[ "${1:-}" = "--" ] && shift
runtime=$(command -v podman || command -v docker || true)
[ -n "$runtime" ] || { echo "container-check: no podman or docker" >&2; exit 2; }

name="$(printf '%s' "${image##*/}${*:+ $*}" | tr -c 'A-Za-z0-9._' '-')"
out="scratch/containers/$name"
rm -rf "$out/src"; mkdir -p "$out/src"
{ git ls-files -z; git ls-files -z --others --exclude-standard; } |
    while IFS= read -r -d '' f; do [ -f "$f" ] && printf '%s\0' "$f"; done |
    xargs -0 -r cp --parents -t "$out/src"

psvp_nice "$runtime" run --rm --network=host -e PSVP_JOBS="$PSVP_JOBS" -e DEPS="$deps" \
    -v "$PWD/$out/src:/src:Z" -w /src "$image" sh -euc '
    command -v bash >/dev/null 2>&1 || apk add --no-cache -q bash
    bash tools/ci/deps.sh $DEPS
    bash tools/ci/build-and-check.sh "$@"
' sh "$@" > "$out/log" 2>&1
rc=$?
cat "$out/log"
if [ $rc -eq 0 ] && grep -q '^BUILD AND CHECK PASSED' "$out/log"; then
    echo "CONTAINER CHECK PASSED ($image${*:+, $*})"
else
    echo "CONTAINER CHECK FAILED ($image${*:+, $*}; exit $rc)"; exit 1
fi
