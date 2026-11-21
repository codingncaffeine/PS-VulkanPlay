#!/usr/bin/env bash
# HDR pictures on a screen that shows SDR: the stream window's shader against the numbers of
# src/media/tone_map.cpp. Patches of a 10-bit BT.2020 picture drawn as SDR, HLG and PQ (at three
# peaks) by psvp_render_check --pattern, on the wall's software Vulkan (no graphics card needed).
# Then the scalers and debanding against src/media/scaler.cpp (--scaling), the same way.
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
within=$(grep -ac 'patches within [01] of 255 of media::screen_color;' "$out")
[ "$within" = 5 ] && ok "SDR, HLG and PQ at 1000, 4000 and 400 cd/m²: every patch within 1 of 255" || fail "$within of 5 pictures within 1 of 255:"$'\n'"$(grep -a 'render-check:' "$out" | cut -c1-240)"
grep -aq 'control failed' "$out" && fail "$(grep -a 'control failed' "$out")" || ok "the control: tone mapped pictures differ from the untouched colors"
# Measured (each PQ picture's own peak): the peak the card counted is the machine's, the patches tone mapped from it.
line="$(grep -a 'PQ measured: the peak' "$out")"
# (The two peaks compared as text: BusyBox's grep has no back-references.)
got=$(printf '%s' "$line" | sed -n 's/.*the peak measured \([0-9.]*\) cd.*/\1/p'); counted=$(printf '%s' "$line" | sed -n "s/.*the machine's count: \([0-9.]*\)).*/\1/p")
if [ -n "$got" ] && [ "$got" = "$counted" ] && printf '%s' "$line" | grep -qE 'patches within [01] of 255' && ! grep -aq 'PQ measured: the control failed' "$out"; then ok "${line#render-check: }"
else fail "PQ measured: '${line#render-check: }' $(grep -a 'PQ measured:' "$out" | grep -v 'the peak' | head -2)"; fi

# Scaling and debanding (psvp_render_check --scaling): five scalers at five sizes against the
# references of src/media/scaler.cpp, debanding against its reference, each with its control.
timeout 300 tools/wall.sh "$check" --scaling > "$out" 2>&1
rc=$?
within=$(grep -ac 'render-check: scaler .*: same$' "$out")
[ $rc = 0 ] && [ "$within" = 25 ] && ok "every scaler at every size within 0.05 of a step of 255 of its reference" \
    || fail "scaling: exit $rc, $within of 25 drawings the same as their reference:"$'\n'"$(grep -a 'render-check:' "$out" | grep -v ': same$' | cut -c1-240 | head -8)"
controls=$(grep -ac 'control at .*differ by more than' "$out")
[ "$controls" = 4 ] && ! grep -aq 'control failed' "$out" && ok "the control: every two scalers' references differ at the four sizes that scale" || fail "the scalers' control: $controls of 4 sizes, $(grep -a 'control failed' "$out" | head -1)"
line="$(grep -a 'debanding: ' "$out")"
grep -aq 'control: the bands' "$out" && ! grep -aq 'control failed' "$out" && ok "${line#render-check: }; $(grep -a 'control: the bands' "$out" | sed 's/^render-check: //')" \
    || fail "debanding: ${line#render-check: } $(grep -a 'control' "$out" | tail -2)"

[ $failed = 0 ] && echo "TONE CHECK PASSED" || echo "TONE CHECK FAILED"
exit $failed
