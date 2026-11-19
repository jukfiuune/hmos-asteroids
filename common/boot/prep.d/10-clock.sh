# The clock from the RTC plus the saved offset (svc/clock.sh), before anything in HarmonyOS reads the time.
sh $A/boot/svc/clock.sh restore || true
