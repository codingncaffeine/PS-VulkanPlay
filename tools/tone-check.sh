#!/usr/bin/env bash
# HDR pictures on a screen that shows SDR: the stream window's shader against the numbers of
# src/media/tone_map.cpp. Patches of a 10-bit BT.2020 picture drawn as SDR, HLG and PQ (at three
# peaks) by psvp_render_check --pattern, on the wall's software Vulkan (no graphics card needed).
#
#   tools/tone-check.sh BUILD_DIR
#
# Exit: 0 passed | 1 a check failed
set -uo pipefail
cd "$(dirname "$0")/.."
build=${1:?usage: tools/tone-check.sh BUILD_DIR}
check="$build/psvp_render_check"
[ -x "$check" ] || { echo "tone-check: $check is not built" >&2; exit 1; }
out="$(mktemp "${TMPDIR:-/tmp}/psvp-tone-check.XXXXXX")"
trap 'rm -f "$out"' EXIT
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }

timeout 300 tools/wall.sh "$check" --pattern --peak 1000 --peak 4000 --peak 400 > "$out" 2>&1
rc=$?
grep -a 'render-check: drawing' "$out" | sed 's/^render-check: /  --    /'
[ $rc = 0 ] && ok "exit 0" || fail "exit $rc: $(tail -3 "$out")"
# Five pictures, each patch within 1 of 255 of the numbers, each HDR one unlike the untouched colors.
within=$(grep -ac 'patches within 1 of 255' "$out")
[ "$within" = 5 ] && ok "SDR, HLG and PQ at 1000, 4000 and 400 cd/m²: every patch within 1 of 255" || fail "$within of 5 pictures within 1 of 255:"$'\n'"$(grep -a 'render-check:' "$out" | cut -c1-240)"
grep -aq 'control failed' "$out" && fail "$(grep -a 'control failed' "$out")" || ok "the control: tone mapped pictures differ from the untouched colors"

[ $failed = 0 ] && echo "TONE CHECK PASSED" || echo "TONE CHECK FAILED"
exit $failed
