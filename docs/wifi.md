# Wi-Fi

QCA6750 (WPSS remoteproc, qcacld `qca_cld3_qca6750` over icnss2). Scans, connects (WPA2, DHCP), reaches the internet
(checked 2026-11-14: connected, `wlan0` has an address). Wi-Fi 6 is reported to the UI.

## Bring-up (2026-11-12)

- Firmware: `qca6750/wpss.mdt` is on the modem partition (M1 mounts it at `/vendor/firmware_mnt`); prep.sh bind-mounts
  it onto an empty `firmware_mnt` in our overlay. `echo 1 > /sys/kernel/icnss/wpss_boot` boots WPSS.
- Driver config: qcacld wants `wlan/qca_cld/qca6750/WCNSS_qcom_cfg.ini` (Android's, in `platform/qcom/wifi/`, installed under our
  `/vendor/firmware`); without it the probe fails silently with -1.
- HarmonyOS: the donor's `wifi_host` (hdf host 14), `wpa_host` (15) and `hostapd_host` (16) from chip_prod's
  hdf_devhost.cfg, plus `/data/service/el1/public/wifi`. The donor's Wi-Fi HAL works over nl80211 as is.
- Category: sheng's preload in wifi_host (`platform/qcom/wifi/sheng`) answers Huawei's private `CMD_WIFI_CATEGORY` ioctl (HE,
  so Wi-Fi 6; log `/data/local/tmp/sheng-wifi-category-v134.txt`).

## MAC and timing

- The device tree's `use-nv-mac` made icnss2 wait for the modem's DMS service for the MAC (200 × 50 ms) and then fail.
  `platform/qcom/wifi/icnss2-no-dms-mac.patch` asks once and goes on; vendor_boot carries the rebuilt icnss2.ko (applied in the PC's module tree).
  Before it, the driver came up at ~41 s and wifi_manager's one restore of the saved state (~13 s into boot) found
  nothing, so Wi-Fi stayed off after boot; now it connects ~50 s after boot.
- Since the modem runs ([modem.md](modem.md)), icnss2 gets the real MAC from DMS. Without the modem, the firmware
  reports the chip's own (its factory address); this qcacld build ignores wlan_mac.bin (WLAN_MAC_ADDR_UPDATE_DISABLE)
  and cnss_utils' debugfs MAC write rejects everything.

## Suspend

With no wake-on-WLAN triggers, cfg80211 disconnects at suspend and qcacld's teardown aborts it. `platform/qcom/wifi/wowlan.c`
sets "any" over nl80211 (svc/booted.sh, once phy0 exists: "wowlan: phy0 wakes on any"). [power.md](power.md).

## Hotspot (2026-11-16)

Control Center's Personal hotspot starts it: wifi_manager turns wlan0 into the access point (Wi-Fi as a client stops
meanwhile), hostapd_host runs hostapd on it ("AP-ENABLED"), wifi_manager's own DHCP server serves 192.168.43.0/24, and
netmanager's sharing puts in the NAT (MASQUERADE out of rmnet_data0) and forwarding rules, taking our mobile data
connection ([modem.md](modem.md)) as the upstream. Checked with another device as the client: it gets an address and reaches the internet.

- What stopped it: before starting hostapd, wifi_manager sets a country code on the AP interface and gives up on the
  hotspot if that fails ("set countryCode=CN failed", "Ap start fail"). The donor's vendor HAL (libwifi_hal_hw) sends
  it as a vendor command of Huawei's chip (OUI 0x1018), which qcacld doesn't know. The Wi-Fi preload in wifi_host
  (`platform/qcom/wifi/sheng/country_code_v134.c`) patches that HAL function (SetCountryCodeCommand, its function
  table's wifiSetCountryCode) to return success once the HAL is loaded, after checking its first instructions; the
  regulatory domain stays the driver's. The code asked for is the framework's default (CN), not the SIM's country
  (BG), so applying it would be wrong. `/data/local/tmp/wifi-country-code.txt` says whether the patch went in.
- Wrapping the HAL's dlsym lookup instead broke wifi_host (and with it Wi-Fi): its linker namespaces resolve dlsym by
  the caller, which became the preload.
- Swapping files into wifi_host by bind mounts in init's namespace doesn't reach it (its sandbox keeps the image's).
- Control Center's tile can keep a stale "on" after a failed start; the next tap then turns it off.

## Open

- Sharing Wi-Fi (STA+AP at once).
- Wi-Fi Direct, 5/6 GHz behaviour and roaming: untested.
