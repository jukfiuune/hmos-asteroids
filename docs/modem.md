# Modem and telephony

The modem runs and HarmonyOS's own telephony sits on top, with no Android userspace: prep.sh boots the modem, the
donor's RIL host loads OpenHarmony's reference AT library (rebuilt by us), and our AT proxy translates between it and
Qualcomm's AT ports. Same design as another HarmonyOS 7 port to a Snapdragon phone (Redmi K90 Pro Max, "hmatd"), which
also got SMS and VoLTE with a SIM.

Checked 2026-11-14 (no SIM inserted): mss remoteproc running; rmtfs, tqftpserv, asteroids_uim, asteroids_atd,
riladapter_host and telephony up.

## Modem boot

`platform/qcom/boot/svc/modem.sh` (from prep.d/80-modem.sh, after pd-mapper): Linaro's rmtfs and tqftpserv (`build-modem.sh`, pinned,
static against musl), then the modem remoteproc with recovery on.

- rmtfs serves the EFS (modemst1, modemst2, fsg, fsc) as Android's rmt_storage does; `patch-rmtfs.py` makes it use this
  kernel's msm_sharedmem UIO ("rmtfs"). It prints nothing until it maps the UIO ("using /dev/uio0"); modem.sh
  waits for that line.
- tqftpserv serves the modem partition's `image/` read-only and keeps `/data/asteroids/modem`.
  `mcfg_ddr/rf/rf_static_903_00_00.mbn` isn't on the partition (Android's server misses it too).
- The modem then lists NAS, UIM, DMS, WDS, voice, WMS and IMS on QRTR.
- EFS backup before the first boot: [storage.md](storage.md).

## RIL

- The donor's riladapter_host loads `libril_vendor.z.so`, which we build from OpenHarmony's telephony_ril_adapter
  (`platform/qcom/modem/build-ril-vendor.sh`, gitee e7458985) with `patch-ril-vendor.py`, into the /vendor overlay. prep.sh binds
  a ril_param common.para (`const.sys.radio.vendorlib.path`, `const.telephony.ril.attty.path=/dev/asteroids-at`).
- Patches: per-slot request handlers (slot 2 goes out as `#1#AT...`); radio-on reported to both slots; baseband
  version via +CGMR; operator answer length in bytes (Huawei's libhril; it was "No service"); IMSI length (without it
  no MCC/MNC, no carrier APNs, no data); `sim_absent` takes CME 10 and 13 as not inserted; `radio_slot2` (slot 2's
  radio requests don't power the shared modem down); data link/auth info ([data](#mobile-data)).

## AT proxy (`platform/qcom/modem/asteroids_atd.c`)

Between the library's pty (`/dev/asteroids-at`) and `/dev/at_mdm0` (slot 1) and `/dev/smd8` (slot 2, bound with
`AT$QCSIMAPP=1`, retried while SUB2 refuses early in boot). Passes 3GPP commands through and answers Huawei's own
(^HCSQ, ^CURCEX, ^SIMST, ^TIME, ^MONSC/^MONNC = NONE, ^SYSINFOEX from +CPIN/+CREG/+CGREG). With no subscription the
modem answers ETSI queries with a bare ERROR; the proxy answers not registered / no operator. Completes +CGSN and +CGMR
(no OK from the modem), splits the compound operator query, sets AT+CMEE=1 on every port. Log
`/data/local/tmp/asteroids-atd.log` (IMEI-length digits and PDUs masked). Reopens a port on POLLHUP/POLLERR (modem
restart) and repeats the library's last +CFUN and report/SMS settings.

Slot-1 status watch (every 5 s idle): +CPIN, +CREG, +CGREG, +CEREG, +COPS, $QCRSRP/$QCRSRQ, +CSQ → ^SIMST, +CREG/+CGREG,
^HCSQ reports on change; the modem's raw reports are dropped. +CGREG? is answered from +CEREG on LTE; access
technology in the library's numbering (LTE 7).

## SIM

`platform/qcom/modem/asteroids_uim.c` (QMI UIM over QRTR) provisions each card's USIM: slot 1 primary, slot 2 secondary, then
follows card status changes (hot insertion), and the UIM service through a modem restart. The modem resets the card
once ~25-38 s after the first activation; the proxy's status watch carries telephony through it. Removal and
re-insertion work (2026-11-14). `asteroids_uim powerup <slot>` power-cycles a slot (unused).

## Calls

- Signalling: the library learns of call changes only from Huawei's ^CCALLSTATE, which the proxy now generates from
  +CLCC (polled each second during calls; non-voice entries such as LTE data bearers dropped); ATH is sent as +CHUP.
  Incoming calls ring and connect; outgoing calls connect since CollaborationFwk runs (the call screen's 1.2 s
  lookups of SA 70633 had stalled it). Calls fall back from LTE to 2G (CS fallback; no VoLTE).
- Audio (2026-11-13/14): the PAL server's voice ops open PAL_STREAM_VOICE_CALL with SUB1's VSID (0x11C05000) when a
  slot-1 call is active; earpiece 2 + handset mic 27, speaker 3 + speaker mic 28; the VDI implements route, voice
  volume and mic mute. The user heard both ways, earpiece and speakerphone. Slot 2's VSID isn't handled.
- Far-end hang-up arrives ~20 s late on outgoing calls: the network's disconnect (seen with
  `platform/qcom/modem/asteroids_voicelog.c`, end reason 145), not the phone.

## SMS

Incoming (2026-11-13): the modem stores messages in ME and sends no +CMT; the proxy counts stored messages (+CPMS?),
reads them (+CMGL=4), hands each to the library as +CMT, acknowledges +CNMA itself and deletes once telephony kept it
(resent after 60 s unacknowledged). Three messages from Vivacom reached Messages. Sending: untested (roaming costs).

## Mobile data

`platform/qcom/modem/asteroids_data.c` makes the call without Android's netmanager: rmnet_ipa0's extended ioctls (once per modem
lifetime), rmnet_data<i> by rtnetlink, QMI DPM Open Port, WDS Bind Mux, Start Network, Get Current Settings. The proxy
answers the library's Balong data commands (+CGDCONT, ^NDISDUP, +CGACT?, ^DHCP, ^AUTHDATA, ^DATADISCONN). Telephony
starts the call by itself (~42 s into boot), netmanager's probe gets 204, pages load. Up to four calls (MMS APN tested
by hand), dual-stack contexts (IPv6 untested end to end: the roaming network grants IPv4 only). Modem restart
(`echo stop/start` on `4080000.remoteproc-mss`): data back ~10 s later. Without AGG_DATA the IPA driver hit a kernel
BUG and hung the phone. `asteroids_data --profiles` dumps the modem's profiles.

## Not done

- VoLTE/IMS: Qualcomm's IMS stack runs on the AP side in Android (qcrild + IMS daemons). UBports'
  ofono-binder-plugin-ext-qti (PC: `~/harmony-build/ref`) documents qcrild's IMS interface; useful only if we run
  qcrild in /android. The VoLTE switch is hidden by Vivacom's carrier config, not by us.
- 5G label: with NR reported SceneBoard drew no label, so the proxy reports LTE.
- Cell info (^MONSC/^MONNC answer NONE), emergency calls, the SIM PIN prompt, a SIM in slot 2 (its unsolicited reports
  are only logged), MMS through telephony, sending SMS.
