#!/bin/bash
# Lists the system abilities processes asked samgr for this boot that weren't running ("NF SA:<id>,<pid>" in hilog),
# most-asked first, with the donor profile that provides each and the processes that asked. A service asked for often
# blocks its callers (callui waited ~1.2 s per lookup of SA 70633): the candidates for donor/pura70proplus-7.0.0.107/desktop-services.txt.
cd "$(dirname "$0")/.."
python3 scripts/m0net.py sh '
chroot /hmos-root /system/bin/hilog -x 2>/dev/null | grep -o "NF SA:[0-9]*,[0-9]*" | sed "s/NF SA://" > /tmp/nf.txt
echo "$(wc -l < /tmp/nf.txt) lookups of missing services"
cut -d, -f1 /tmp/nf.txt | sort | uniq -c | sort -rn | head -${1:-30} | while read n sa; do
    f=$(grep -l "\"name\" *: *$sa\b" /hmos-root/system/profile/*.json 2>/dev/null | head -1)
    who=$(grep "^$sa," /tmp/nf.txt | cut -d, -f2 | sort -u | head -6 | while read p; do
        tr "\0" " " < /proc/$p/cmdline 2>/dev/null | cut -d" " -f1 | sed "s|.*/||"; done | sort -u | tr "\n" " ")
    echo "$n SA $sa $(basename "${f:-(no profile)}" .json) <- $who"
done' 120
