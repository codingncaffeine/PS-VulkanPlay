#!/usr/bin/env bash
# Installs a release artifact in clean containers the way a user would, checks
# that every shipped binary finds its libraries there, and runs it: a session
# with the mock console, the app's window (packaging/smoke-test.sh). The steps
# inside the container are packaging/install-check.sh.
#
#   packaging/install-test.sh ARTIFACT [IMAGE ...]
#
# Default images by artifact:
#   ps-vulkanplay_*.deb                       Debian 12 and 13, Ubuntu 22.04, 24.04 and 26.04 (apt resolves the Depends)
#   PS-VulkanPlay-*-linux-x64.tar.gz          Fedora, Arch
#   PS-VulkanPlay-*-linux-musl-x64.tar.gz     Alpine 3.22 (the one it is built on) and the latest Alpine
#
# Installing needs the network; the run that follows gets none at all: the mock
# console opens the consoles' ports and answers broadcasts, and none of that may
# reach the network this machine is on.
# Exit: 0 every image passed | 1 something failed | 2 no container runtime
set -uo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
artifact="$(realpath "${1:?usage: install-test.sh ARTIFACT [IMAGE ...]}")"
shift
images=("$@")
file="$(basename "$artifact")"
case "$file" in
    *.deb) flavor=glibc; version="${file#ps-vulkanplay_}"; version="${version%_amd64.deb}" ;;
    *-linux-musl-x64.tar.gz) flavor=musl; version="${file#PS-VulkanPlay-}"; version="${version%-linux-musl-x64.tar.gz}" ;;
    *-linux-x64.tar.gz) flavor=glibc; version="${file#PS-VulkanPlay-}"; version="${version%-linux-x64.tar.gz}" ;;
    *) echo "install-test: not a release artifact: $file" >&2; exit 1 ;;
esac
if (( ! ${#images[@]} )); then
    case "$file" in
        *.deb) images=(docker.io/library/debian:12 docker.io/library/debian:13 docker.io/library/ubuntu:22.04
                       docker.io/library/ubuntu:24.04 docker.io/library/ubuntu:26.04) ;;
        *-linux-musl-x64.tar.gz) images=(docker.io/library/alpine:3.22 docker.io/library/alpine:latest) ;;
        *) images=(registry.fedoraproject.org/fedora:latest docker.io/library/archlinux:latest) ;;
    esac
fi
tools="$(dirname "$artifact")/native-$flavor/tools"
[ -x "$tools/psvp-mock-console" ] && [ -x "$tools/psvp_fbcheck" ] || { echo "install-test: no test programs in $tools (packaging/build-release.sh makes them)" >&2; exit 1; }
runtime="$(command -v podman || command -v docker || true)"
[ -n "$runtime" ] || { echo "no podman or docker" >&2; exit 2; }

rc=0
for image in "${images[@]}"; do
    printf '\n######## %s\n' "$image"
    name="$(printf '%s' "${image##*/}-$file" | tr -c 'a-z0-9._\n' '-')"
    context="packaging/out/install/$name"
    rm -rf "$context"; mkdir -p "$context"
    cp "$artifact" packaging/install-check.sh "$context/"
    printf 'FROM %s\nCOPY %s install-check.sh /tmp/\nRUN sh /tmp/install-check.sh install /tmp/%s\n' "$image" "$file" "$file" > "$context/Containerfile"
    tag="localhost/psvp-install-test:$name"
    if ! psvp_nice "$runtime" build --network=host -q -t "$tag" "$context" > "$context/image.log" 2>&1; then
        tail -30 "$context/image.log"; echo "FAIL the artifact did not install on $image"; rc=1; continue
    fi
    # Only what the check reads is handed to the container: the scripts, the scenario, the test programs.
    psvp_nice "$runtime" run --rm --network=none --cap-add NET_ADMIN -e PSVP_TEST_ISOLATED_NET=1 \
        -v "$PWD/tools":/repo/tools:ro,Z -v "$PWD/packaging/smoke-test.sh":/repo/packaging/smoke-test.sh:ro,Z \
        -v "$PWD/packaging/install-check.sh":/repo/packaging/install-check.sh:ro,Z \
        -v "$PWD/tests/scenario":/repo/tests/scenario:ro,Z -v "$tools":/repo/native-tools:ro,Z \
        "$tag" sh /repo/packaging/install-check.sh check "/tmp/$file" /repo/native-tools "$version" || rc=1
    "$runtime" rmi "$tag" > /dev/null 2>&1 || true
    rm -rf "$context"
done
printf '\n'
if (( rc == 0 )); then echo "INSTALL TEST PASSED on ${#images[@]} image(s): $file"
else echo "INSTALL TEST FAILED: $file"; fi
exit $rc
