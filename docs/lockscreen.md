# Lock screen, PIN, biometrics

## PIN (2026-11-14, works)

Checked by the user: set a 4-digit PIN, locked, unlocked with it. Software only, without a TEE's protection (as
sheng's user auth).

- pin_auth_host exited 253 every 240 s: resource_schedule_service.cfg carries only a fragment of it, and the real entry
  (`hdf_devhost -i 26 -n pin_auth_host`, chip_prod's hdf_devhost.cfg) wasn't in our host list. Now it is.
- The donor's backend (`libpin_auth_interface_service_3.0`) is a client of Huawei's TEE. `common/pinauth/build-pinauth.sh`
  builds OpenHarmony's software backend (drivers_peripheral/pin_auth/hdi_service, interface 3.0) under that name in the
  /vendor overlay, below the donor's driver and stub. PINs live in `/data/service/el1/public/pinauth` (pin_auth_host,
  0700, made by asteroids-desktop.cfg). Shims for init's parameter.h and sysparam_errno.h.
- OpenHarmony's driver calls `PinAuth::Init` (PIN database, the executors' Ed25519 key pairs); Huawei's doesn't.
  `common/pinauth/pin_auth_init.cpp` wraps the entry point to do it once.
- useriam.json runs SA 941 (libpinauthservice) again.
- Huawei's useriam enrols and authenticates only through user-auth 4.1's BeginEnrollmentExt/BeginAuthenticationExt.
  `common/useriam/adapter41.cpp` forwards them to 4.0's calls (the parameter layouts checked in the donor's
  libuser_auth_stub_4.1).
- User auth's credential file needs `/data/service/el1/public/userauth` (user_auth_host, 0700), made by the cfg.
- Huawei's PIN input library sends 96 bytes when authenticating (the 64-byte scrypt result plus the PIN's SHA-256);
  `common/pinauth/pin-data-with-sha256.patch` takes the first 64 bytes of a 96-byte buffer.

## Open

- Fingerprint: the Goodix under-display sensor's driver is loaded (`goodix_fp`, `/dev/goodix_fp`), but its matching
  runs in a Goodix trusted app in Qualcomm's TEE (QSEE), which Android's fingerprint HAL reaches through
  qseecom/smcinvoke. HarmonyOS's fingerprint_auth HDI would need a bridge to that HAL in /android, as audio has.
- Face unlock: needs the camera ([camera.md](camera.md)).
- Lock-screen wallpaper, notifications on the lock screen: not reviewed.
- `common/boot/wake.sh` (`/asteroids/boot/wake.sh`) unlocks only without a credential: with the PIN set, it stops at the PIN pad.
