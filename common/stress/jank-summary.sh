#!/bin/sh
# Summary of /data/asteroids/jank: frame-gap totals, missed share, fence waits, CPU frequency spread.
grep FRAME_GAPS /data/asteroids/jank/metrics.txt | sed "s/.*counts=//" | awk -F, '{for(i=1;i<=8;i++) s[i]+=$i} END{n=s[1]+s[2]+s[3]+s[4]+s[5]+s[6]+s[7]; m=s[3]+s[4]+s[5]+s[6]+s[7]; printf "gaps le8..gt100: %d %d %d %d %d %d %d %d  missed %d/%d = %.1f%%  2+ refreshes %d\n", s[1],s[2],s[3],s[4],s[5],s[6],s[7],s[8], m, n, 100*m/n, s[4]+s[5]+s[6]+s[7]}'
grep "submit frames" /data/asteroids/jank/metrics.txt | grep -o "fence=[0-9]*" | cut -d= -f2 | awk '{s+=$1; n++} END{printf "fence wait avg %d us\n", s/n}'
awk '{a+=$4; b+=$5; c+=$6; n++} END{printf "CPU MHz avg: %d %d %d\n", a/n/1000, b/n/1000, c/n/1000}' /data/asteroids/jank/clocks.txt
awk '{g+=$2; n++} END{printf "GPU MHz avg: %d\n", g/n/1e6}' /data/asteroids/jank/clocks.txt
