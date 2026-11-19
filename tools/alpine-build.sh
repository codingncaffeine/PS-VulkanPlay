#!/usr/bin/env bash
# Builds PS VulkanPlay from source on Alpine (musl) in a throwaway container,
# with Alpine's own compiler and libraries and warnings as errors, then runs
# the tests and starts the application behind the test wall.
#
#   tools/alpine-build.sh [alpine tag, default latest]
set -euo pipefail
exec "$(dirname "$0")/container-check.sh" "docker.io/library/alpine:${1:-latest}"
