#!/bin/sh
# RenderFrame durations (ms) of render_service's render thread from /data/asteroids/jank/trace.txt: count, avg,
# p50/p90/p99 and the share over one 90 Hz refresh (11.1 ms).
T=/data/asteroids/jank/trace.txt
grep -E "RSUniRenderThre-[0-9]+ .*tracing_mark_write: [BE]\|" $T | awk '
{ t=$4; sub(":","",t); kind=substr($0, index($0,"tracing_mark_write: ")+20, 1)
  if (kind=="B") { d++; if (index($0,"|H:RenderFrame")) s[d]=t }
  else { if (d in s) { printf "%.3f\n", (t-s[d])*1000; delete s[d] } d-- } }' | sort -n | awk '
{ v[++n]=$1; sum+=$1; if ($1>11.1) o++ }
END { if (n) printf "RenderFrame n=%d avg %.2f p50 %.2f p90 %.2f p99 %.2f >11.1ms %.1f%%\n", n, sum/n, v[int(n*.5)], v[int(n*.9)], v[int(n*.99)], 100*o/n }'
