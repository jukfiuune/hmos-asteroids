# Location (GNSS)

Huawei's locationhub runs; the donor's GNSS HDI is for Huawei's own chip. The modem offers QMI LOC (QRTR service 16,
version 2), the engine Android's location stack uses.

`platform/qcom/gnss/asteroids_gnss.c` (`build-gnss.sh`) is HarmonyOS's GNSS vendor library over QMI LOC: location_host's
gnss_interface_service (the donor's libgnss_interface_service_2.0, OpenHarmony code) dlopens `vendorGnssAdapter.so`
and takes `GnssVendorInterface`; build-hmos-m2.sh installs ours over the donor's as
`vendor/lib64/hw/vendorGnssAdapter.so` and enables location_host (on demand). Also a static `asteroids_gnss [seconds]`
test tool.

- QMI notes: Register Events needs the client type TLV (0x11), otherwise INVALID_ARGUMENT and Start fails; Set NMEA
  Types is NOT_SUPPORTED; failed requests get two responses (the error, then INTERNAL).
- Checked (2026-11-14): shell test indoors, engine on, time injected from the system clock, 19 satellites in 20 s, no
  fix. Petal Maps' locate button reaches it through locationhub; the session ends when Maps lets go.

## Open

- A fix (needs the sky).
- AGNSS and measurement interfaces (locationhub logs their absence).
- Petal Maps' map stays black (separate; it also needs Huawei attestation for some services, [apps.md](apps.md)).
