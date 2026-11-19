#!/system/bin/sh
# Run by the asteroids_bundle init service (uid system, INSTALL_BUNDLE): installs the app directories listed, one per
# line, in /data/local/tmp/asteroids-install.list (preload apps in /preload/app/<name>, sideloaded ones in
# /data/local/tmp/asteroids-apps/<name>, signed with our dev cert, see common/signing), then empties the list. Only listed
# directories are touched, so apps the user uninstalled stay uninstalled. Each directory's .hap and .hsp modules go in
# one bm transaction. asteroids-desktop.cfg runs it from /asteroids/boot/svc;
# "begetctl start_service asteroids_bundle", log /data/local/tmp/asteroids-bundle.txt.
# A line "!<bundle name>" uninstalls that bundle keeping its data (bm uninstall -k): an installed app can't be updated
# into a different app type (normal to system app), so the reinstall goes uninstall first, then its directory.
L=/data/local/tmp/asteroids-install.list
[ -f $L ] || { echo "nothing listed in $L"; exit 0; }
while read -r d; do
    case "$d" in
        !*) echo "== uninstall ${d#!} (keeping data)"; bm uninstall -n "${d#!}" -k 2>&1; continue ;;
    esac
    [ -d "$d" ] || { [ -n "$d" ] && echo "== $d: no such directory"; continue; }
    echo "== $(basename "$d")"
    set --
    for f in "$d"/*.hap "$d"/*.hsp; do [ -f "$f" ] && set -- "$@" "$f"; done
    [ $# -gt 0 ] && bm install -p "$@" 2>&1 || echo "no .hap in $d"
done < $L
: > $L
echo done
