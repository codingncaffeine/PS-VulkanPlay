#!/usr/bin/env bash
# The whole release check in one command: builds every artifact from a clean copy
# of the repository's files, runs the glibc ones here behind the test wall, and
# installs and runs each one in clean containers (the .deb on Debian 12 and 13
# and Ubuntu 22.04, 24.04 and 26.04, the glibc tarball on Fedora and Arch, the
# musl tarball on Alpine 3.22 and the latest Alpine), and reads the .deb with
# lintian. The container runs go three at a time. Logs land in packaging/out/logs/.
#
#   packaging/release-check.sh          exit 0 = every artifact passed everything
#   packaging/release-check.sh --keep   check the artifacts that are in packaging/out/ already
#
# A release is tagged only from a commit this has passed on.
set -uo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
VER="$(sed -nE 's/^ +VERSION ([0-9]+\.[0-9]+\.[0-9]+)$/\1/p' CMakeLists.txt | head -1)"
OUT=packaging/out
LOGS=$OUT/logs

# What gets tested must be what gets tagged: tracked files have to match a commit.
COMMIT="$(git rev-parse --short HEAD)"
dirty() { [ -n "$(git status --porcelain --untracked-files=no)" ] && [ "${PSVP_ALLOW_DIRTY:-}" != 1 ]; }
if dirty; then
    echo "RELEASE CHECK REFUSED: uncommitted changes to tracked files (PSVP_ALLOW_DIRTY=1 for a trial run):"
    git status --short --untracked-files=no
    exit 3
fi

if [ "${1:-}" != "--keep" ]; then
    packaging/build-release.sh all || { echo "RELEASE CHECK FAILED: the build"; exit 1; }
    if [ "$(git rev-parse --short HEAD)" != "$COMMIT" ] || dirty; then
        echo "RELEASE CHECK FAILED: the tree changed while the artifacts were being built"; exit 1
    fi
fi
rm -rf "$LOGS"; mkdir -p "$LOGS"
DEB=$OUT/ps-vulkanplay_${VER}_amd64.deb
TAR=$OUT/PS-VulkanPlay-$VER-linux-x64.tar.gz
MUSL=$OUT/PS-VulkanPlay-$VER-linux-musl-x64.tar.gz
for artifact in "$DEB" "$TAR" "$MUSL"; do
    [ -f "$artifact" ] || { echo "RELEASE CHECK FAILED: no $artifact"; exit 1; }
done
( cd "$OUT" && sha256sum -c SHA256SUMS > /dev/null ) || { echo "RELEASE CHECK FAILED: the artifacts are not the ones SHA256SUMS names"; exit 1; }

# The glibc tarball as it unpacks, on this machine, behind the wall.
here() {
    tools/wall.sh --check || return 1
    tools/wall.sh packaging/smoke-test.sh "$OUT/PS-VulkanPlay-$VER-linux-x64/ps-vulkanplay" "$OUT/native-glibc/tools" "$VER"
}
# The .deb as Debian's own checker reads it. Errors fail the check; what is tolerated is named
# in packaging/debian/lintian-overrides with the reason.
lintian_check() {
    local runtime; runtime="$(command -v podman || command -v docker)"
    "$runtime" run --rm --network=host -v "$PWD/$DEB":/tmp/"$(basename "$DEB")":ro,Z -v "$PWD/packaging/debian":/debian:ro,Z \
        docker.io/library/debian:13 sh -c '
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq > /dev/null && apt-get install -y -qq --no-install-recommends lintian > /dev/null 2>&1 || { echo "lintian could not be installed"; exit 1; }
            lintian --no-tag-display-limit --suppress-tags-from-file /debian/lintian-overrides /tmp/*.deb > /tmp/lintian.out 2>&1
            cat /tmp/lintian.out
            ! grep -q "^E:" /tmp/lintian.out'
}

declare -A runs
JOBS=3
start() {
    local name=$1; shift
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    ( "$@" > "$LOGS/$name.log" 2>&1 ) & runs[$name]=$!
}
start smoke-here            here
start lintian               lintian_check
start deb-debian-12         packaging/install-test.sh "$DEB" docker.io/library/debian:12
start deb-debian-13         packaging/install-test.sh "$DEB" docker.io/library/debian:13
start deb-ubuntu-22.04      packaging/install-test.sh "$DEB" docker.io/library/ubuntu:22.04
start deb-ubuntu-24.04      packaging/install-test.sh "$DEB" docker.io/library/ubuntu:24.04
start deb-ubuntu-26.04      packaging/install-test.sh "$DEB" docker.io/library/ubuntu:26.04
start tarball-fedora        packaging/install-test.sh "$TAR" registry.fedoraproject.org/fedora:latest
start tarball-arch          packaging/install-test.sh "$TAR" docker.io/library/archlinux:latest
start tarball-musl-alpine-3.22    packaging/install-test.sh "$MUSL" docker.io/library/alpine:3.22
start tarball-musl-alpine-latest  packaging/install-test.sh "$MUSL" docker.io/library/alpine:latest

failed=0
for name in $(printf '%s\n' "${!runs[@]}" | sort); do
    if wait "${runs[$name]}"; then printf '  ok    %s\n' "$name"
    else printf '  FAIL  %s   (see %s)\n' "$name" "$LOGS/$name.log"; failed=1; fi
done
echo
( cd "$OUT" && cat SHA256SUMS )
if (( failed )); then echo "RELEASE CHECK FAILED: do NOT release $VER"; exit 1; fi
echo "RELEASE CHECK PASSED: $VER at $COMMIT"
