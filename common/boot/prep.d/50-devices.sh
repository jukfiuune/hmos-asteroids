# The GPU, display and dma-heap nodes for the composer, allocator and render_service, and the panel's backlight.
chmod 666 $GPU_NODES $BACKLIGHT/brightness $BACKLIGHT/bl_power
# Trace points: HarmonyOS's HitraceMeter writes to /sys/kernel/tracing/trace_marker, else the debugfs one, which only
# root can reach. Mounted before the handoff so every service inherits it (render_service starts in a namespace
# init's own jobs never reach); trace_marker's mode is the tracefs instance's, so it holds on every mount.
mount -t tracefs -o nodev,noexec,nosuid tracefs $R/sys/kernel/tracing && chmod 0755 $R/sys/kernel/tracing &&
    chmod 0222 $R/sys/kernel/tracing/trace_marker || log "tracefs not mounted"
# Preload apps (as sheng's apply-preload.sh): the donor's preload partition (from Huawei's
# update_full_preload_HBN-AL10_all_cn_R3 package) at /preload; etc/baselayer.cfg adds its policy directory.
# HarmonyOS installs preload apps on first boot only; asteroids_bundle installs them on a running system, only the
# directories listed in /data/local/tmp/asteroids-install.list, so apps the user uninstalled aren't brought back.
mount -t erofs -o ro /dev/mapper/preload$SLOT $R/preload || log "preload not mounted"
