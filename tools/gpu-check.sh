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

[ $failed = 0 ] && echo "GPU CHECK PASSED" || echo "GPU CHECK FAILED"
exit $failed
