#!/usr/bin/env bash
# Builds and checks the working tree from source inside a throwaway container,
# the way a user of that distribution would: its own compiler and libraries, a
# clean copy of the files (tracked, and new ones that are not ignored).
#
#   tools/container-check.sh IMAGE [deps: clang|app] [-- build-and-check options]
#   tools/container-check.sh docker.io/library/debian:13
#   tools/container-check.sh docker.io/library/debian:13 clang -- --compiler clang-libcxx
#   tools/container-check.sh docker.io/library/alpine:latest app -- --app
#
# Uses a quarter of the cores at low priority unless PSVP_JOBS says otherwise.
set -uo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
image="${1:?usage: tools/container-check.sh IMAGE [clang|app] [-- build-and-check options]}"; shift
deps=""
if [ "${1:-}" = clang ] || [ "${1:-}" = app ]; then deps=$1; shift; fi
[ "${1:-}" = "--" ] && shift
runtime=$(command -v podman || command -v docker || true)
[ -n "$runtime" ] || { echo "container-check: no podman or docker" >&2; exit 2; }

name="$(printf '%s' "${image##*/}${deps:+ $deps}${*:+ $*}" | tr -c 'A-Za-z0-9._' '-')"
out="scratch/containers/$name"
rm -rf "$out/src"; mkdir -p "$out/src"
{ git ls-files -z; git ls-files -z --others --exclude-standard; } |
    while IFS= read -r -d '' f; do [ -f "$f" ] && printf '%s\0' "$f"; done |
    xargs -0 -r cp --parents -t "$out/src"

# First an image with the packages, built with the network and kept until
# tools/ci/deps.sh changes. Then the build and its checks in a container that has
# no network at all: the tests open the consoles' ports and send broadcasts, and
# none of that may reach the network this machine is on.
# The app's packages come from the network too: they are fetched into the image, for the project
# files as they are now, and the build finds them there.
projects=()
[ "$deps" = app ] && projects=(app/PSVulkanPlay/PSVulkanPlay.csproj app/PSVulkanPlay.Tests/PSVulkanPlay.Tests.csproj)
key="$(cat tools/ci/deps.sh "${projects[@]}" | sha256sum | cut -c1-12)"
repo="localhost/psvp-check-$(printf '%s' "${image##*/}${deps:+-$deps}" | tr -c 'a-z0-9._\n' '-')"
if ! "$runtime" image inspect "$repo:$key" > /dev/null 2>&1; then
    context="scratch/containers/image-$name"
    rm -rf "$context"; mkdir -p "$context"
    cp tools/ci/deps.sh "$context/deps.sh"
    printf 'FROM %s\nCOPY deps.sh /deps.sh\nRUN command -v bash >/dev/null 2>&1 || apk add --no-cache -q bash\nRUN bash /deps.sh %s\n' \
        "$image" "$deps" > "$context/Containerfile"
    if [ ${#projects[@]} -gt 0 ]; then
        cp --parents -t "$context" "${projects[@]}"
        printf 'COPY app /restore/app\nRUN DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 dotnet restore /restore/app/PSVulkanPlay.Tests -v q && rm -rf /restore\n' >> "$context/Containerfile"
    fi
    if ! psvp_nice "$runtime" build --network=host -q -t "$repo:$key" "$context" > "$out/image.log" 2>&1; then
        cat "$out/image.log"; echo "CONTAINER CHECK FAILED ($image: the image with the packages did not build)"; exit 1
    fi
    # Images of earlier versions of the package list are of no more use.
    "$runtime" images --format '{{.Repository}}:{{.Tag}}' | while IFS= read -r old; do
        case "$old" in "$repo:$key") ;; "$repo":*) "$runtime" rmi "$old" > /dev/null 2>&1 || true ;; esac
    done
fi

psvp_nice "$runtime" run --rm --network=none --cap-add NET_ADMIN -e PSVP_JOBS="$PSVP_JOBS" -e PSVP_FIRST_CORE="$PSVP_FIRST_CORE" -e PSVP_TEST_ISOLATED_NET=1 \
    -v "$PWD/$out/src:/src:Z" -w /src "$repo:$key" bash tools/ci/build-and-check.sh "$@" > "$out/log" 2>&1
rc=$?
cat "$out/log"
if [ $rc -eq 0 ] && grep -q '^BUILD AND CHECK PASSED' "$out/log"; then
    echo "CONTAINER CHECK PASSED ($image${deps:+, $deps}${*:+, $*})"
else
    echo "CONTAINER CHECK FAILED ($image${deps:+, $deps}${*:+, $*}; exit $rc)"; exit 1
fi
