#!/bin/bash
# Keeps the camera phone (wireless adb, pointed at the port target) connected: every 20 s it checks the link with a
# no-op shell command and, if that fails, rediscovers the phone's adb-tls-connect service over mDNS and reconnects.
# It also re-asserts "stay awake" so the screen (and the camera preview) stays on. Logs to stdout.
while true; do
    if ! timeout 8 adb shell true >/dev/null 2>&1; then
        echo "$(date +%T) link lost, reconnecting"
        addr=$(adb mdns services 2>/dev/null | awk '/_adb-tls-connect/ {print $NF; exit}')
        [ -n "$addr" ] && timeout 10 adb connect "$addr" 2>&1 | sed "s/^/$(date +%T) /"
    else
        timeout 8 adb shell svc power stayon true >/dev/null 2>&1
    fi
    sleep 20
done
