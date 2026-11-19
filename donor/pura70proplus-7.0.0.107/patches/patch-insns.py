#!/usr/bin/env python3
"""sheng's donor instruction patches, found again in our donor (Pura 70 Pro+ 7.0.0.107).

    patch-insns.py <library name> <in> <out>

sheng's port patches a few client libraries so SceneBoard's UI thread doesn't block on services that can't come up
(patches/donor/*.json there name the function of each). Their offsets are for sheng's donor; the sites below are the
same instructions in ours, located by function and instruction pattern. Each is checked before it is changed.
"""
import struct
import sys

# library: [(virtual address, old instruction, new instruction, what it does)]
PATCHES = {
    "libaudio_policy_client.z.so": [
        # AudioPolicyManager::GetAudioPolicyManagerProxy(bool block): the flag handed to the proxy lookup is forced to
        # false, so the audio policy proxy is fetched without blocking (sheng: and w0,w20,#1 -> mov w0,#0).
        (0x36014, 0x12000281, 0x52800001, "and w1, w20, #0x1 -> mov w1, #0"),
    ],
    # The rest switch a blocking lookup through the system ability manager's vtable to CheckSystemAbility (slot +40),
    # which returns at once when the service isn't there, as sheng does.
    "libsensor_client.z.so": [
        # SensorServiceClient::LoadSensorService: LoadSystemAbility(3601, 10000 ms), slot +120.
        (0x22014, 0xf9403ea9, 0xf94016a9, "ldr x9, [x21, #120] -> [x21, #40]"),
    ],
    "libsps_privacy_indicator_service_intf.z.so": [
        # PrivacyIndicatorClient::InitializeProxy: GetSystemAbility(0x1022a), slot +32.
        (0x1a604, 0xf94012c9, 0xf94016c9, "ldr x9, [x22, #32] -> [x22, #40]"),
    ],
    "libwifi_sdk.z.so": [
        # The Wi-Fi device proxy lookup SceneBoard's network-speed widget froze in: GetSystemAbility, slot +32.
        (0x49abc, 0xf94012e9, 0xf94016e9, "ldr x9, [x23, #32] -> [x23, #40]"),
    ],
    "libnet_tether_manager_if.z.so": [
        # NetworkShareClient's proxy lookup: GetSystemAbility(1154), slot +32.
        (0x80b0, 0xf94012e9, 0xf94016e9, "ldr x9, [x23, #32] -> [x23, #40]"),
    ],
    # Ours (anco): Huawei's Android container manager (hmos_fusion_manager, SA 66854) serves only its own apps, checked
    # by signing identity, so DroiTong re-signed with our cert was refused (1003) even holding ACCESS_FUSION_MANAGER:
    # consent couldn't be saved and the sync switch failed. Its three caller checks (HmosFusionPermission: CheckPermission
    # line 51, the uid-5523 / privileged-app check line 140, CheckCallingMatched line 150) return 0 at once.
    "libhmos_fusion_manager.z.so": [
        (0x5cef4, 0xd503237f, 0x2a1f03e0, "CheckPermission: pacibsp -> mov w0, wzr"),
        (0x5cef8, 0xd10203ff, 0xd65f03c0, "CheckPermission: sub sp, sp, #0x80 -> ret"),
        (0x5da60, 0xd503237f, 0x2a1f03e0, "caller check (line 140): pacibsp -> mov w0, wzr"),
        (0x5da64, 0xd10203ff, 0xd65f03c0, "caller check (line 140): sub sp, sp, #0x80 -> ret"),
        (0x5dbcc, 0xd503237f, 0x2a1f03e0, "CheckCallingMatched: pacibsp -> mov w0, wzr"),
        (0x5dbd0, 0xd10203ff, 0xd65f03c0, "CheckCallingMatched: sub sp, sp, #0x80 -> ret"),
    ],
    "libcli_tool_client.z.so": [
        # CliToolMGRClient::LoadCliToolMgrService: load wait 4 s (0xee6b2800 ns) -> 100 ms (0x05f5e100).
        (0x1fb40, 0x52850008, 0x529c2008, "mov w8, #0x2800 -> #0xe100"),
        (0x1fb4c, 0x72bdcd68, 0x72a0bea8, "movk w8, #0xee6b, lsl #16 -> #0x05f5"),
    ],
    # Not sheng's. WakeupController::RegisterMonitor(state) subscribes powermgr to input activity
    # (InputManager::SubscribeInputActive) with a 1000 ms interval while the screen is on and 0 otherwise. With 1000
    # ms no event ever arrives here, so touches never reset the screen-off timer and the screen dims under the
    # finger; with 0 every touch arrives, and the callback already limits refreshes to one per 100 ms.
    # sheng's sensor package (PARCEL_PATCHES in tools/package_sensors.py): Sensor::ReadFromParcel reads the three floats
    # at +0x80, +0x78, +0x7c where Sensor::Marshalling writes +0x78, +0x7c, +0x80, so every sensor's range, resolution
    # and power came back rotated. The reads go to the written order.
    "libsensor_utils.z.so": [
        (0x11c68, 0x91020281, 0x9101e281, "add x1, x20, #0x80 -> #0x78"),
        (0x11c90, 0x9101e281, 0x9101f281, "add x1, x20, #0x78 -> #0x7c"),
        (0x11cb8, 0x9101f281, 0x91020281, "add x1, x20, #0x7c -> #0x80"),
    ],
    # sheng's libtime_system_ability.z.so.json: the timer manager's setup makes one timerfd per clock in a loop of 7;
    # the last is clock 12 (non-blocking), a Huawei kernel clock Linux doesn't have, and its failure abandons the whole
    # setup (time_service had one timerfd). Its slot gets -1 and the loop goes on to its end, as sheng.
    "libtime_system_ability.z.so": [
        (0x3c458, 0x52800180, 0x12800000, "mov w0, #12 -> mov w0, #-1"),
        (0x3c45c, 0x52810001, 0xb8337b40, "mov w1, #0x800 -> str w0, [x26, x19, lsl #2]"),
        (0x3c460, 0x14000003, 0x1400003c, "b timerfd_create -> b loop tail (0x3c550)"),
    ],
    # sheng's display/present-fence-export.json: the composer service's reply to a commit packs a dup of the present
    # fence for render_service, then Huawei's build (not DISPLAY_COMMUNITY) calls HdifdParcelable::Move() on the
    # original, so its destructor never closes it: one leaked sync_file per frame (32768 fds in about five minutes of
    # animation). Our composer VDI returns real fences, so the Move goes: in OnCommit and in
    # ReplyCommitAndGetReleaseFence, each right after replyCommandCnt_++. The input-fence Moves stay.
    "libdisplay_composer_service_1.5.z.so": [
        (0x2ae48, 0x940020ba, 0xd503201f, "OnCommit: bl HdifdParcelable::Move -> nop"),
        (0x2cf14, 0x94001887, 0xd503201f, "ReplyCommitAndGetReleaseFence: bl HdifdParcelable::Move -> nop"),
    ],
    # Ours: MemMgrService::Init gives up when ReclaimStrategyManager::Init fails, and it does here (no Huawei memcg
    # extensions: /dev/memcg/memory.app_score, memory.name). OnStart then never publishes SA 1909, and every client
    # waits for it: inputmethod_service about 1.2 s on each keyboard show and hide, so the soft keyboard never stayed up.
    # The failure is ignored; the reclaim strategy stays off (its users check its initialized flag).
    "libmemmgrservice.z.so": [
        (0x15f24c, 0x36003620, 0xd503201f, "tbz w0, #0, ReclaimStrategyManager init failed -> nop"),
    ],
    # Ours: msdp's activity recognition (MovementAr::ConnectMovementHdi) tries the AR HDI, which nothing provides here,
    # 20 times 100 ms apart on each enable; a few types in a row pass the 5 s XCollie limit and msdp is killed, taking
    # auto-rotate's subscription down with it. One try is enough.
    "libmovement_ar.z.so": [
        (0x8b60, 0x7100567f, 0x71000a7f, "cmp w19, #21 -> #2"),
    ],
    "libpowermgrservice.z.so": [
        (0xdb204, 0x9a9f0102, 0xaa1f03e2, "csel x2, x8, xzr, eq -> mov x2, xzr"),
    ],
    # sheng's rs-commit-sync (librender_service_base.z.so.json there), not applied: apps' UI commits are one-way
    # binder calls, and render_service drops a non-system app's commits whose caller pid is 0, which is what Linux
    # binder gives one-way calls. Our kernel now reports one-way senders (common/kernel/binder-oneway-pid.py), so the
    # commits stay async. Without that kernel change, this makes RSClientToRenderConnectionProxy::CommitTransaction
    # send TF_SYNC instead.
    "librender_service_base.z.so": [
        (0x3cc314, 0x52800021, 0x52800001, "MessageOption::SetFlags(TF_ASYNC) -> SetFlags(TF_SYNC)"),
    ],
}


def file_offset(data, va):
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
        if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
            return va - p_vaddr + p_offset
    sys.exit("%#x is in no loadable segment" % va)


name, src, out = sys.argv[1:4]
data = bytearray(open(src, "rb").read())
for va, old, new, what in PATCHES[name]:
    off = file_offset(data, va)
    current, = struct.unpack_from("<I", data, off)
    if current != new:
        if current != old:
            sys.exit("%s %#x: found %#010x, expected %#010x (a different build?)" % (name, va, current, old))
        struct.pack_into("<I", data, off, new)
    print("%s %#x: %s" % (name, va, what))
open(out, "wb").write(data)
