#!/usr/bin/env bash
# Writes tests/data/clip.h265 again: 48 frames of FFmpeg's moving test pattern,
# 320x180, as an HEVC stream the way a console's is laid out (one keyframe, then
# frames that each need earlier ones; the parameter sets once, in front).
# Only needed when the clip is to change; it takes an ffmpeg with libx265.
set -euo pipefail
cd "$(dirname "$0")/.."
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=320x180:rate=60" -frames:v 48 -pix_fmt yuv420p \
    -c:v libx265 -preset medium -tune zerolatency \
    -x265-params "keyint=-1:min-keyint=1000:bframes=0:ref=4:rc-lookahead=0:scenecut=0:repeat-headers=1:info=0:aud=0:log-level=none:bitrate=120:vbv-maxrate=120:vbv-bufsize=60:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited" \
    -bsf:v "filter_units=remove_types=39-40" -f hevc tests/data/clip.h265
ls -l tests/data/clip.h265
