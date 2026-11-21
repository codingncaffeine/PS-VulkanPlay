#!/usr/bin/env bash
# Decoding on the graphics card, drawn the way the stream window draws it, on this machine's card:
# tests/data/clip.h265 is decoded through Vulkan Video and every third picture is drawn from the
# card's memory and the software way, offscreen (psvp_render_check), with the Khronos validation
# layer watching every Vulkan call. No window, no sound, no input device: the wall with the
# card's render nodes only (PSVP_WALL_GPU=1).
#
#   tools/gpu-check.sh BUILD_DIR
#
# Exit: 0 passed, or this machine has no card that decodes (said so) | 1 a check failed
set -uo pipefail
cd "$(dirname "$0")/.."
build=${1:?usage: tools/gpu-check.sh BUILD_DIR}
check="$build/psvp_render_check"
[ -x "$check" ] || { echo "gpu-check: $check is not built" >&2; exit 1; }
out="$(mktemp "${TMPDIR:-/tmp}/psvp-gpu-check.XXXXXX")"
trap 'rm -f "$out"' EXIT
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }

PSVP_WALL_GPU=1 timeout 300 tools/wall.sh env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LOADER_LAYERS_ENABLE='*validation*' \
    VK_LOADER_DEBUG=layer "$check" tests/data/clip.h265 --every 3 --checks 3 > "$out" 2>&1
rc=$?
if [ $rc = 3 ]; then
    echo "  skip  no card here decodes through Vulkan Video: $(grep -a 'render-check:' "$out" | tail -1 | cut -c1-200)"
    echo "GPU CHECK SKIPPED"; exit 0
fi
# The control: the loader put the layer in. Without it, silence from the layer would mean nothing.
grep -aq 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$out" && ok "the validation layer is in place" || fail "the validation layer was not loaded (is it installed?)"
line="$(grep -a 'drawn both ways' "$out")"
[ $rc = 0 ] && ok "${line#render-check: }" || fail "exit $rc: $(tail -3 "$out")"
# FFmpeg makes its video images with flags the card's video format list does not name (its own call, not
# this program's): that report is left out by its number; every other one fails the check.
reports=$(grep -a 'VUID\|Validation Error\|Validation Warning' "$out" | grep -avc 'VUID-VkImageCreateInfo-pNext-06811')
ffmpeg_own=$(grep -ac 'VUID-VkImageCreateInfo-pNext-06811' "$out")
if [ "$reports" = 0 ]; then ok "the layer reported nothing of this program's ($ffmpeg_own reports of FFmpeg's own image flags left out)"
else fail "the layer reported $reports lines:"; grep -a 'VUID\|Validation' "$out" | grep -av 'pNext-06811' | sed -E 's/0x[0-9a-f]+/0x/g' | sort | uniq -c | cut -c1-300 | head -10; fi

# Tone mapping on the card: the HDR pictures are filtered in the shader (textureGather), with the layer watching.
PSVP_WALL_GPU=1 timeout 300 tools/wall.sh env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LOADER_LAYERS_ENABLE='*validation*' \
    "$check" --pattern --peak 1000 --peak 4000 --peak 400 > "$out" 2>&1
rc=$?
within=$(grep -ac 'patches within [01] of 255 of media::screen_color;' "$out")
[ $rc = 0 ] && [ "$within" = 5 ] && ok "tone mapping on $(grep -ao 'picture on .*' "$out" | head -1 | cut -c12-): SDR, HLG and PQ at three peaks within 1 of 255" \
    || fail "tone mapping on the card: exit $rc, $within of 5 pictures within 1 of 255"
line="$(grep -a 'PQ measured: the peak' "$out")"
# (The two peaks compared as text: BusyBox's grep has no back-references.)
got=$(printf '%s' "$line" | sed -n 's/.*the peak measured \([0-9.]*\) cd.*/\1/p'); counted=$(printf '%s' "$line" | sed -n "s/.*the machine's count: \([0-9.]*\)).*/\1/p")
if [ -n "$got" ] && [ "$got" = "$counted" ] && printf '%s' "$line" | grep -qE 'patches within [01] of 255' && ! grep -aq 'PQ measured: the control failed' "$out"; then ok "on the card: ${line#render-check: }"
else fail "PQ measured on the card: '${line#render-check: }'"; fi
reports=$(grep -ac 'VUID\|Validation Error\|Validation Warning' "$out")
[ "$reports" = 0 ] && ok "the layer reported nothing while the patterns were drawn" || fail "the layer reported $reports lines while the patterns were drawn: $(grep -a 'VUID' "$out" | head -2 | cut -c1-240)"


# Scaling and debanding (psvp_render_check --scaling): five scalers at five sizes against the
# references of src/media/scaler.cpp, debanding against its reference, each with its control.
timeout 300 env PSVP_WALL_GPU=1 tools/wall.sh env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LOADER_LAYERS_ENABLE='*validation*' "$check" --scaling --time > "$out" 2>&1
rc=$?
within=$(grep -ac 'render-check: scaler .*: same$' "$out")
[ $rc = 0 ] && [ "$within" = 25 ] && ok "on the card: every scaler at every size within 0.05 of a step of 255 of its reference" \
    || fail "scaling on the card: exit $rc, $within of 25 drawings the same as their reference:"$'\n'"$(grep -a 'render-check:' "$out" | grep -v ': same$' | cut -c1-240 | head -8)"
controls=$(grep -ac 'control at .*differ by more than' "$out")
[ "$controls" = 4 ] && ! grep -aq 'control failed' "$out" && ok "the control: every two scalers' references differ at the four sizes that scale" || fail "the scalers' control: $controls of 4 sizes, $(grep -a 'control failed' "$out" | head -1)"
line="$(grep -a 'debanding: ' "$out")"
grep -aq 'control: the bands' "$out" && ! grep -aq 'control failed' "$out" && ok "${line#render-check: }; $(grep -a 'control: the bands' "$out" | sed 's/^render-check: //')" \
    || fail "debanding: ${line#render-check: } $(grep -a 'control' "$out" | tail -2)"
reports=$(grep -ac 'VUID\|Validation Error\|Validation Warning' "$out")
[ "$reports" = 0 ] && ok "the layer reported nothing while scaling and debanding" || fail "the layer reported $reports lines while scaling and debanding: $(grep -a 'VUID' "$out" | head -2 | cut -c1-240)"
grep -a 'time: ' "$out" | sed 's/^render-check: /  --    /'

[ $failed = 0 ] && echo "GPU CHECK PASSED" || echo "GPU CHECK FAILED"
exit $failed
