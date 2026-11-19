# HarmonyOS's scheduling cgroups, in the mount namespace its services run in (HarmonyOS init service asteroids_cgroups,
# once samgr is ready: bootevent.samgr.ready=true).
#
# init.sched.cfg mounts the cpu and cpuset hierarchies on /dev/cpuctl and /dev/cpuset in pre-init, then makes the
# groups resource_schedule_service moves processes into (top-app, foreground, background on cores 0-3...) in init.
# Those mounts stay behind in M1's mount namespace: init's own namespace (where resource_schedule_service and
# hilogd run) and the one most services run in (samgr, foundation, render_service) share the /dev tmpfs but have no
# cgroup mounts, so the init jobs made plain directories, every task stayed in the root group on all cores, and the
# scheduler's moves did nothing. This mounts both hierarchies in both namespaces, replays init.sched.cfg's
# mkdir/write/chown/chmod commands for them once (the mounts share one hierarchy), and restarts
# resource_schedule_service, which opens the group files when it starts.
# The commands run in samgr's namespace go by /system/bin: /asteroids isn't there.
TAG=cgroups
. /asteroids/boot/lib.sh
CFG=$HR/system/etc/init/init.sched.cfg
# init makes the group directories in its init stage; usually long done when samgr is ready.
until_ 300 [ -d /dev/cpuset/top-app ] || log "no /dev/cpuset/top-app yet"
[ -f $CFG ] || { log "no init.sched.cfg"; exit 0; }

# The commands, one per line ("mkdir /dev/cpuset/top-app", "write /dev/cpuset/top-app/cpuset.cpus 0-7", ...).
grep -o '"\(mkdir\|write\|chown\|chmod\) /dev/cpu\(set\|ctl\)[^"]*"' $CFG | tr -d '"' > /tmp/cgroup-cmds
# init's namespace and the services' (samgr's).
S=$(ps -ef | grep "[ /]samgr$" | awk '{print $1}' | head -1)
nsenter -t 1 -m /system/bin/rm -rf /dev/cpuset /dev/cpuctl
for t in 1 $S; do
    for h in cpuset:/dev/cpuset cpu:/dev/cpuctl; do
        nsenter -t $t -m /system/bin/sh -c "PATH=/system/bin; grep -q ' ${h#*:} cgroup ' /proc/mounts ||
            { mkdir -p ${h#*:} && mount -t cgroup -o ${h%%:*} none ${h#*:}; }" || log "mount ${h#*:} in $t's namespace failed"
    done
done
n=0
while read -r op a b c; do
    case $op in
    mkdir) nsenter -t 1 -m /system/bin/mkdir -p "$a" ;;
    write) nsenter -t 1 -m /system/bin/sh -c "PATH=/system/bin; echo '$b' > '$a'" 2>/dev/null ;;  # Kirin-only nodes just fail
    chown) nsenter -t 1 -m /system/bin/chown "$a:$b" "$c" 2>/dev/null ;;
    chmod) nsenter -t 1 -m /system/bin/chmod "$a" "$b" 2>/dev/null ;;
    esac
    n=$((n + 1))
done < /tmp/cgroup-cmds
log "$n commands from init.sched.cfg; $(nsenter -t 1 -m /system/bin/find /dev/cpuset -mindepth 1 -maxdepth 1 -type d | wc -l) cpuset groups"
# The scheduler opened the old plain files at start: a new one opens the groups (init restarts it).
p=$(ps -ef | grep "[r]esource_schedule_service" | awk '{print $1}' | head -1)
[ -n "$p" ] && kill $p && log "resource_schedule_service restarted"
