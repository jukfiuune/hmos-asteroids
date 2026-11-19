# Bluetooth

sheng's design (2026-11-12): Qualcomm's HAL (`android.hardware.bluetooth@1.0-impl-qti.so`, WCN6750 "moselle",
firmware `msbtfw12` from bluetooth_b) runs in `/android` under sheng's bridge (`platform/qcom/bluetooth/sheng`, built by
`platform/qcom/bluetooth/build-bluetooth.sh` against libc++ headers with Android's `__1` namespace). It powers the chip through
`/dev/btpower` and `/dev/ttyHS0` and relays H4 over `/dev/unix/socket/sheng.bt.h4`.

HarmonyOS side: sheng's vendor library bound over `/vendor/lib64/libbt_vendor.z.so`, the HCI HDI publisher
(`asteroids_bluetooth_hdi`, blue_host), bluetooth_service (profile set to run-on-create),
fusion_connectivity_manager_ext (what Settings' switch talks to) and softbus_server, started by
`asteroids.bluetooth.start`, which `platform/qcom/boot/svc/bt-bridge.sh` sets (init service asteroids_bt_bridge, once /android is up;
log `/data/local/tmp/start-bluetooth.log`). NearLink capability off in hw_defaults.para, as sheng.

Address: generated once (22:22:..., kept in `/data/asteroids/android/vendor/bluetooth/bdaddr`). The modem runs now, so
the real address could come from it, as Android's does; not done.

`platform/qcom/bluetooth/bt_hci_test.c` checks the chip directly. Checked: switching on from Settings.

## Open

- Pairing, file transfer, audio profiles (A2DP, HFP) and LE devices: untested.
- The device's own Bluetooth address from the modem/NV.
