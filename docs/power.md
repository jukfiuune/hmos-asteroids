# Battery, charging, suspend, thermal

## Battery (2026-11-12)

The kernel's power_supply nodes were right (battery, usb, wireless), but nothing registered `battery_interface_service`.
`common/battery/` is sheng's compat/battery: `native_battery_hdi_publisher` loads the donor's
`libbattery_interface_service_2.0` (Huawei's sysfs reader) and registers it (asteroids_battery_hdi, power_host, log
`/data/local/tmp/sheng-battery-hdi.txt`). sheng's charge-type adapter reads Xiaomi fast-charge nodes; it now reports
plain wired charging without them. The donor passes the kernel's µA where HarmonyOS expects mA (DevInfo showed -773 A);
the adapter divides by 1000 (2026-11-14). SceneBoard shows level and the charging bolt.

## Charging

- On the laptop's USB port (SDP) the ADSP's charger firmware limits input to 500 mA; with the screen on and the phone
  busy it can drain while "charging". `qti_battery_charger` doesn't take input-current writes (NT_EDIT). The
  `/proc/charger/is_aging_test` knob didn't lift it (tried 2026-11-14, reverted).
- Not checked: a real charger (PD/QC up to 50 W), charge limits, behaviour when full or hot, wireless (none on 3a).

## Power HDI and suspend (2026-11-13)

powermgr's StartSuspend goes through the power HDI. `common/power/native_power_hdi_publisher.cpp` registers the donor's
`libpower_interface_service_1.3` (plain OpenHarmony code on /sys/power) with its own stub (asteroids_power_hdi,
power_host, CAP_BLOCK_SUSPEND; `build-power.sh`). asteroids-desktop.cfg gives /sys/power/state, wakeup_count,
wake_lock and wake_unlock to power_host. It mirrors running locks into the kernel and waits in pm_get_wakeup_count.

- On the laptop's USB the phone never suspends on either OS (a600000.ssusb and chg_wakelock hold wakeup sources).
- Unplugged: Wi-Fi's teardown aborted every suspend until `platform/qcom/wifi/wowlan.c` set wake-on-WLAN triggers; then it slept
  (~4 minutes in one s2idle, woken by the power key). `scripts/suspend-report.sh` reads the counters after replugging.
- Not checked: whether calls, SMS and Wi-Fi traffic wake it; drain over hours (the user's next test).

## Background-app freezer (2026-11-13)

resource_schedule_service's suspend manager talks to Huawei's kernel over netlink unit 30 ("kstate"); on our kernel
unit 30 was qcacld's unused cesium socket. `common/power/kstate.c` is that kernel side as a module: per-socket event masks,
reports of signals and synchronous binder calls to frozen tasks (GKI vendor hooks, through a ring and a work item).
prep.sh loads it before the WLAN driver. resource_schedule_service gets SYS_NICE (v1 cgroups need it to move tasks).
Checked: with the screen off it froze 4-7 background apps; a signal thawed one within 3 s; counters in
`/sys/module/kstate/parameters`. lowpower_manager (unit 30, mask 80) isn't running.

## Thermal

Only the kernel acts: 59 thermal zones and 73 cooling devices with the device tree's trips. No HarmonyOS
thermal_manager/thermal_protector and no Android thermal HAL or thermal-engine run (checked 2026-11-14), so nothing
applies skin-temperature limits, charge-current limits by temperature or user-visible warnings. Temperatures seen:
~35 °C idle, GPU up to 46 °C in stress 3.

## Open

- Thermal policy (the donor's thermal services run in power_host with a thermal daemon; or Android's policy as a
  config for HarmonyOS's thermal_manager).
- Charging current and battery health/cycle data in Settings; drain measurements.
- Freezer: apps that stay frozen while a foreground app needs them (watch for stalls).
