#!/usr/bin/env bash
# Shows the generated clip with the Khronos validation layer watching every
# Vulkan call the window makes, and fails on anything the layer reports.
#
#   tools/wall.sh tools/validation-check.sh APP
#
# Needs the layer installed (Arch: vulkan-validation-layers). Only through
# tools/wall.sh, as every run of the application in a test.
# Exit: 0 passed | 1 a check failed | 2 not run through tools/wall.sh
set -uo pipefail
app=${1:?usage: tools/wall.sh tools/validation-check.sh APP}
if [ -z "${DISPLAY:-}" ] || [ -z "${PSVP_TEST_TMP:-}" ]; then
    echo "validation-check: run it through tools/wall.sh" >&2; exit 2
fi
out="$PSVP_TEST_TMP/validation.out"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }

VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LOADER_LAYERS_ENABLE='*validation*' VK_LOADER_DEBUG=layer \
    "$app" --test-clip --frames 150 > "$out" 2>&1; rc=$?
# The control: the loader says that it put the layer in. Without it, silence would mean nothing.
if grep -aq 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$out"; then ok "the validation layer is in place"
else fail "the validation layer was not loaded (is it installed?)"; fi
[ $rc -eq 0 ] && ok "150 frames of the clip were shown (exit 0)" || fail "exit $rc"
grep -aq 'render: stopped after 150 frames, [1-9][0-9]* pictures' "$out" && ok "$(grep -ao 'render: stopped after.*' "$out")" || fail "the clip was not shown"
reports=$(grep -ac 'VUID\|Validation Error\|Validation Warning\|Validation Performance' "$out")
if [ "$reports" = 0 ]; then ok "the layer reported nothing"
else fail "the layer reported $reports lines:"; grep -a 'VUID\|Validation' "$out" | sed -E 's/0x[0-9a-f]+/0x/g' | sort | uniq -c | cut -c1-300 | head -10; fi

[ $failed = 0 ] && echo "VALIDATION CHECK PASSED" || echo "VALIDATION CHECK FAILED"
exit $failed
