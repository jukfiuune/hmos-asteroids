# Modem: booted like the Wi-Fi processor, now that the mapper is up (svc/modem.sh: Linaro's rmtfs and tqftpserv, then
# the remoteproc). HarmonyOS's RIL host then uses the donor's AT vendor library (OpenHarmony's reference one) through
# asteroids_atd's pty: the proxy speaks the library's Huawei AT dialect on top of the modem's 3GPP AT channel
# (etc/ril-common.para).
sh $A/boot/svc/modem.sh || log "modem: not started"
