# The clock across power-offs. The PMIC's real-time clock (rtc-pm8xxx) keeps counting while the phone is off, but
# Linux can't set it (it reads ~1970 here), so after a cold boot HarmonyOS showed Jan 1970 until NTP corrected it.
# Android's time_daemon keeps the difference between real time and the RTC in /data and adds it back at boot; this
# does the same, with the difference in /data/asteroids/clock-offset (seconds).
#   clock.sh restore   prep.d/10-clock.sh, before the handoff: the clock becomes the RTC plus the saved difference
#   clock.sh keep      HarmonyOS init service asteroids_clock, from boot: saves the difference every 10 min while the clock looks set
#                      (NTP or a manual change: a year from 2025 on), when it moved by more than 2 s
F=/data/asteroids/clock-offset
rtc() { cat /sys/class/rtc/rtc0/since_epoch; }
case $1 in
restore)
    [ -s $F ] || exit 0
    date -s @$(($(rtc) + $(cat $F))) >/dev/null && echo "hmos-clock: set from the RTC + $(cat $F) s" > /dev/kmsg
    ;;
keep)
    while :; do
        now=$(date +%s)
        if [ $now -ge 1735689600 ]; then
            off=$((now - $(rtc)))
            old=$(cat $F 2>/dev/null || echo 0)
            d=$((off - old))
            if [ $d -gt 2 ] || [ $d -lt -2 ]; then
                echo $off > $F.new && mv $F.new $F
            fi
        fi
        sleep 600
    done
    ;;
esac
