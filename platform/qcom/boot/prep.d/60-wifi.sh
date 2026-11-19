# Wi-Fi: the modem partition's firmware (M1 mounts it at /vendor/firmware_mnt) under HarmonyOS's /vendor too, where the
# kernel looks once HarmonyOS's root is in place; then the Wi-Fi processor booted. The WLAN driver (M1 loaded it, with
# icnss2 patched not to wait for the modem's MAC: the firmware then reports the chip's own) probes once its firmware is
# ready (about 20 s), reads its config from /vendor/firmware/wlan and creates wlan0.
mount --bind /vendor/firmware_mnt $R/vendor/firmware_mnt || log "wifi: firmware_mnt not bound"
echo 1 > /sys/kernel/icnss/wpss_boot || log "wifi: WPSS boot request failed"
