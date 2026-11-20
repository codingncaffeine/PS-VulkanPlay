#!/usr/bin/env bash
# Builds the app (app/, .NET) and its tests, and refreshes bin/app: the app that
# bin/ps-vulkanplay starts when it is run with nothing to do.
#
#   tools/build-app.sh            build, and refresh bin/app
#   tools/build-app.sh --test     also run the app's tests (no window is opened, nothing is asked of a console)
#
# Exit: 0 done | 1 it did not build, or a test failed | 3 built, but bin/app is in use and was left alone
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/env.sh
if ! command -v dotnet >/dev/null 2>&1; then
    echo "build-app: no dotnet here; the app needs the .NET 10 SDK (the stream program builds without it)" >&2
    exit 1
fi
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
psvp_nice dotnet build app/PSVulkanPlay.Tests -c Release -v q --nologo -m:"$PSVP_JOBS"
if [ "${1:-}" = "--test" ]; then
    psvp_nice app/PSVulkanPlay.Tests/bin/Release/net10.0/PSVulkanPlay.Tests
fi

# Not while the app runs from there: the folder is replaced, and a running app that then reaches
# for a file it had not loaded yet finds another build, or nothing.
if pgrep -f "$PWD/bin/app/PSVulkanPlay" >/dev/null 2>&1; then
    echo "build-app: built, but bin/app was NOT refreshed: the app is running from it. Close it and run this again." >&2
    exit 3
fi
rm -rf bin/app && mkdir -p bin/app
cp -a app/PSVulkanPlay/bin/Release/net10.0/. bin/app/
test -x bin/app/PSVulkanPlay || { echo "build-app: the app is missing from bin/app" >&2; exit 1; }
echo "bin/app is now PS VulkanPlay $(sed -nE 's:.*<Version>([^<]+)</Version>.*:\1:p' app/PSVulkanPlay/PSVulkanPlay.csproj) (started by bin/ps-vulkanplay)"
