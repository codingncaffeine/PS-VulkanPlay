#!/usr/bin/env bash
# I5, the picture reference (PLAN §7): the test picture drawn by us (psvp_render_check --chart-out)
# and by libplacebo (psvp_placebo_check), compared: SDR within 1/255, HDR tone mapped from 1000 and
# 4000 cd/m² with its grays within CIEDE2000 1. The controls: libplacebo told BT.601's matrix, and our
# drawing tone mapped from 600 cd/m² against libplacebo's from 1000, must each fail. On the wall's
# software Vulkan. Skipped (and said) where the build has no libplacebo to compare with.
#
#   tools/placebo-check.sh BUILD_DIR
set -uo pipefail
cd "$(dirname "$0")/.."
build=${1:?usage: tools/placebo-check.sh BUILD_DIR}
render="$build/psvp_render_check" placebo="$build/psvp_placebo_check"
[ -x "$render" ] || { echo "placebo-check: $render is not built" >&2; exit 1; }
if [ ! -x "$placebo" ]; then echo "  skip  no libplacebo here to compare with"; echo "PLACEBO CHECK SKIPPED"; exit 0; fi
work="$PWD/scratch/placebo-check"
rm -rf "$work"; mkdir -p "$work"
failed=0
ok()   { printf '  ok    %s\n' "$*"; }
fail() { printf '  FAIL  %s\n' "$*"; failed=1; }
ours() {   # ours NAME [--hdr --peak N]
    local name=$1; shift
    timeout 120 tools/wall.sh "$render" --chart-out "$work/$name.f32" "$@" > "$work/$name.render.out" 2>&1 || { fail "our $name drawing: $(tail -2 "$work/$name.render.out")"; return 1; }
}
compare() {   # compare NAME WANT(0|1) WHAT PLACEBO_OPTIONS...
    local name=$1 want=$2 what=$3; shift 3
    timeout 300 tools/wall.sh "$placebo" "$work/$name.f32" "$@" > "$work/$name.$want.out" 2>&1
    local rc=$?
    local said; said=$(grep -a 'placebo-check: \(SDR\|HDR tone\)' "$work/$name.$want.out" | tail -1 | sed 's/^placebo-check: //')
    [ "$rc" = "$want" ] && ok "$what: $said" || fail "$what: exit $rc, wanted $want: ${said:-$(tail -2 "$work/$name.$want.out")}"
}
ours sdr && { compare sdr 0 "SDR as libplacebo draws it"; compare sdr 1 "the control: libplacebo told BT.601's matrix" --plant; }
ours hdr-1000 --hdr --peak 1000 && compare hdr-1000 0 "HDR from 1000 cd/m²" --hdr --peak 1000
ours hdr-4000 --hdr --peak 4000 && compare hdr-4000 0 "HDR from 4000 cd/m²" --hdr --peak 4000
ours hdr-600 --hdr --peak 600 && compare hdr-600 1 "the control: ours from 600 cd/m² against libplacebo's from 1000" --hdr --peak 1000
[ $failed = 0 ] && echo "PLACEBO CHECK PASSED" || echo "PLACEBO CHECK FAILED"
exit $failed
