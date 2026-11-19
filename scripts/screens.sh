#!/bin/bash
# Pulls the newest N frames of the phone's capture loop (common/boot/svc/capture.sh runs capture_scanout every second into
# /data/local/tmp/cap/fNN.ppm, quarter size) and joins them left to right, oldest first, with their times. Frames
# stop while the panel is off.
#   screens.sh [N] [out.png]
N=${1:-6}
OUT=${2:-/tmp/screens.png}
W=$(mktemp -d)
cd "$(dirname "$0")/.."
python3 scripts/m0net.py sh "cd /data/local/tmp/cap && tar -cf /data/local/tmp/caps.tar \$(ls -t *.ppm | head -$N) \$(ls -t *.ppm | head -$N | sed 's/$/.time/')" 20 >/dev/null
python3 scripts/m0net.py get /data/local/tmp/caps.tar $W/caps.tar >/dev/null
tar -xf $W/caps.tar -C $W
frames=()
for f in $(cd $W && ls -tr *.ppm 2>/dev/null); do
    t=$(cat $W/$f.time 2>/dev/null)
    magick $W/$f -resize 200x -background white -gravity north -splice 0x16 -pointsize 12 -annotate +0+1 "$t" $W/$f.png
    frames+=($W/$f.png)
done
# Times come from each frame's .time file; ls -tr orders by when the capture finished.
magick "${frames[@]}" +append "$OUT"
rm -rf $W
echo "$OUT"
