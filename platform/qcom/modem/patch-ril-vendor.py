#!/usr/bin/env python3
"""Adapts OpenHarmony's reference RIL vendor library (telephony_ril_adapter services/vendor) for asteroids_atd;
run in the telephony_ril_adapter checkout. The donor ships this library unchanged (libril_vendor.z.so); we build it
from source for two things it lacks:

  - Both SIM slots. libhril hands both slots' requests to the one library, which has one AT channel and doesn't say
    which slot a command is for. Each request handler now records its slot (thread-local; hril calls handlers on
    the requesting thread), and a slot-2 command goes out as "#1#AT...". asteroids_atd routes those to a modem port
    bound to the second subscription (AT$QCSIMAPP=1).
  - Radio state for both slots: radio power (+CFUN) is the whole modem's, but the library reported it to slot 1 only,
    so slot 2 never looked powered and telephony never asked it for its IMEI.
  - The baseband version (HRilModemReq.GetBasebandVersion, left NULL): AT+CGMR, which asteroids_atd answers with a
    +CGMR: prefix (the modem's line has none, and the library keeps only prefixed or numeric lines).
  - The operator's names and PLMN: the library reported their length as 3 (the number of strings); the donor's libhril
    takes it in bytes and rejected the answer ("GetOperatorInfoResponse response is invalid"), so telephony had no
    operator, flagged the registration emergency-only and the lock screen said "No service". It is now the array's
    size.
  - The IMSI: reported with length 0, which the donor's libhril takes for no answer. Telephony had no IMSI, so no
    MCC/MNC, no operator key and none of the carrier's APNs, and never started a data call.
  - A removed SIM (sim_absent): the "SIM not inserted" error never became the not-inserted state.
  - Slot 2's radio power (radio_slot2): only recorded; the modem's radio is slot 1's to set.
  - Data calls through asteroids_atd (see data_routes and data_free_cid below): several at once, each on its own
    rmnet_data interface, dual-stack, with the APN's credentials (data_link_info, data_auth), and telephony hears when
    the network or a modem restart ends one (data_list_update).
"""
import re

SRC = "services/vendor/src/"


def edit(path, fn):
    p = SRC + path
    s = open(p).read()
    t = fn(s)
    if t != s:
        open(p, "w").write(t)


def slot_handlers(s):
    if "SetAtSlotId(" in s:
        return s
    s = re.sub(r'(#include "at_support.h"\n)', r'\1extern void SetAtSlotId(int32_t slotId);\n', s, count=1)
    if "SetAtSlotId(int32_t" not in s:
        s = re.sub(r'(#include [^\n]+\n)(?!#include)', r'\1extern void SetAtSlotId(int32_t slotId);\n', s, count=1)
    return re.sub(r'(\n(?:static )?(?:void|int32_t) \w+\(const ReqDataInfo \*requestInfo[^)]*\)\n\{\n)',
                  r'\1    SetAtSlotId(requestInfo != NULL ? requestInfo->slotId : 0);\n', s)


for f in ("at_call.c", "at_data.c", "at_modem.c", "at_network.c", "at_sim.c", "at_sms.c", "vendor_adapter.c"):
    edit(f, slot_handlers)


def support(s):
    if "g_atSlotId" in s:
        return s
    s = s.replace("void ReaderLoop(void)", '''static __thread int32_t g_atSlotId;

void SetAtSlotId(int32_t slotId)
{
    g_atSlotId = slotId;
}

void ReaderLoop(void)''', 1)
    old = "    err = WriteATCommand(command, 0, g_atFd);"
    assert old in s
    return s.replace(old, '''    if (g_atSlotId == 1) {
        char slotCommand[MAX_CMD_LENGTH + 4];
        (void)sprintf_s(slotCommand, sizeof(slotCommand), "#1#%s", command);
        err = WriteATCommand(slotCommand, 0, g_atFd);
    } else {
        err = WriteATCommand(command, 0, g_atFd);
    }''', 1)


edit("at_support.c", support)


def modem(s):
    if "ReqGetBasebandVersion" in s:
        return s
    return s + '''
void ReqGetBasebandVersion(const ReqDataInfo *requestInfo)
{
    if (requestInfo == NULL) {
        return;
    }
    SetAtSlotId(requestInfo->slotId);
    ResponseInfo *responseInfo = NULL;
    int32_t ret = SendCommandLock("AT+CGMR", "+CGMR:", DEFAULT_TIMEOUT, &responseInfo);
    struct ResponseAck respDataAck = { responseInfo, NULL, 0 };
    if (responseInfo == NULL) {
        ResponseModemReport(requestInfo->slotId, requestInfo, HRIL_ERR_NULL_POINT, &respDataAck);
        return;
    }
    if (ret != 0 || !responseInfo->success || responseInfo->head == NULL) {
        ResponseModemReport(requestInfo->slotId, requestInfo, HRIL_ERR_GENERIC_FAILURE, &respDataAck);
        return;
    }
    char *version = responseInfo->head->data;
    if (SkipATPrefix(&version) != 0 || version == NULL) {
        ResponseModemReport(requestInfo->slotId, requestInfo, HRIL_ERR_INVALID_RESPONSE, &respDataAck);
        return;
    }
    while (*version == ' ') {
        version++;
    }
    respDataAck.respDataPointer = (uint8_t *)version;
    respDataAck.respDataLen = strlen(version);
    ResponseModemReport(requestInfo->slotId, requestInfo, HRIL_ERR_SUCCESS, &respDataAck);
}
'''


edit("at_modem.c", modem)


def adapter(s):
    if ".GetBasebandVersion" in s:
        return s
    s = s.replace("    .GetVoiceRadioTechnology = ReqGetVoiceRadioTechnology,\n};",
                  "    .GetVoiceRadioTechnology = ReqGetVoiceRadioTechnology,\n"
                  "    .GetBasebandVersion = ReqGetBasebandVersion,\n};", 1)
    assert ".GetBasebandVersion" in s
    return s


edit("vendor_adapter.c", adapter)


def radio_both_slots(s):
    # Each slot hears the modem's radio state, slot 2 as off while it asked for off (radio_slot2 below).
    old = "OnModemReport(GetSlotId(NULL), reportInfo, (const uint8_t *)&g_radioState, sizeof(HRilRadioState));"
    new = ("for (int32_t radioSlot = 0; radioSlot < GetSimSlotCount(); radioSlot++) { "
           "HRilRadioState slotState = (radioSlot == 0 || g_asteroidsSlot2Radio) ? g_radioState : "
           "HRIL_RADIO_POWER_STATE_OFF; "
           "OnModemReport(radioSlot, reportInfo, (const uint8_t *)&slotState, sizeof(HRilRadioState)); }")
    assert old in s or new in s
    s = s.replace(old, new)
    if "g_asteroidsSlot2Radio;" not in s:
        s = s.replace("static HRilRadioState g_radioState", "extern int32_t g_asteroidsSlot2Radio;\nstatic HRilRadioState g_radioState", 1)
    return s


edit("vendor_adapter.c", radio_both_slots)

p = "services/vendor/include/at_modem.h"
s = open(p).read()
if "ReqGetBasebandVersion" not in s:
    s = s.replace("void ReqGetVoiceRadioTechnology(const ReqDataInfo *requestInfo);",
                  "void ReqGetVoiceRadioTechnology(const ReqDataInfo *requestInfo);\n"
                  "void ReqGetBasebandVersion(const ReqDataInfo *requestInfo);", 1)
    assert "ReqGetBasebandVersion" in s
    open(p, "w").write(s)


def operator_info_length(s):
    old = "OnNetworkReport(requestInfo->slotId, reportInfo, (const uint8_t *)response, NUM_COUNT);"
    new = "OnNetworkReport(requestInfo->slotId, reportInfo, (const uint8_t *)response, sizeof(response));"
    assert old in s or new in s
    return s.replace(old, new)


edit("at_network.c", operator_info_length)


def data_node(s):
    # The data call's interface is asteroids_data's rmnet_data0 (asteroids_atd runs it on AT^NDISDUP); each call's
    # own comes in its ^DHCP answer (data_link_info).
    return s.replace('#define NET_NODE "usb0"', '#define NET_NODE "rmnet_data0"')


p = "services/vendor/include/at_data.h"
s = open(p).read()
t = data_node(s)
assert 'NET_NODE "rmnet_data0"' in t
if t != s:
    open(p, "w").write(t)


def data_routes(s):
    # RouteUp/RouteDown ran "ifconfig usb0 up/down" (a Balong's NDIS link), and a failure failed the call: here the
    # link belongs to asteroids_data, and telephony's netmanager configures it.
    for fn in ("RouteUp", "RouteDown"):
        s = re.sub(r'(static int32_t %s\(void\)\n\{\n)    int32_t ret = system\("ifconfig usb0 (?:up|down)"\);\n'
                   r'.*?\n(    TELEPHONY_LOGI\()' % fn, r'\1\2', s, count=1, flags=re.S)
    assert 'system("ifconfig' not in s
    return s


def data_free_cid(s):
    # For an APN no context has, the library took the first cid in use instead of the first free one, so a second
    # APN (MMS) overwrote the first's context.
    old = "        if (isUsedCid == 1) {\n            cid = i;"
    new = "        if (isUsedCid == 0) {\n            cid = i;"
    assert old in s or new in s
    return s.replace(old, new)


edit("at_data.c", data_routes)
edit("at_data.c", data_free_cid)


def imsi_length(s):
    old = "OnSimReport(GetSlotId(requestInfo), reportInfo, (const uint8_t *)result, 0);"
    new = "OnSimReport(GetSlotId(requestInfo), reportInfo, (const uint8_t *)result, strlen(result));"
    assert old in s or new in s
    return s.replace(old, new)


edit("at_sim.c", imsi_length)


LINK_INFO = r'''/* A call's settings from asteroids_atd: AT^DHCP=<cid> answers Huawei's ^DHCP line (IPv4 address, mask, gateway,
 * DHCP server and DNS servers as hex of the address's bytes in memory order, then the rates; 0 for a call without
 * IPv4) followed by the call's interface, its MTU, and its IPv6 address/prefix, gateway and DNS servers. Telephony
 * reads each of address, gateway and dns as a space-separated list of both families. */
static void AppendAddress(char *out, size_t size, const char *addr)
{
    size_t used = strlen(out);
    if (addr == NULL || addr[0] == '\0' || used + strlen(addr) + 2 > size) {
        return;
    }
    if (used > 0) {
        out[used++] = ' ';
    }
    (void)strcpy_s(out + used, size - used, addr);
}

static void AppendIpv4(char *out, size_t size, uint64_t addr)
{
    char buf[MAX_CMD_LENGTH] = {0};
    if (addr != 0 && AddressFormat(addr, buf, sizeof(buf)) == HRIL_ERR_SUCCESS) {
        AppendAddress(out, size, buf);
    }
}

static ModemReportErrorInfo GetLinkInformation(int32_t activeIndex, HRilDataCallResponse **ppDcr)
{
    char cmd[MAX_CMD_LENGTH] = {0};
    char address[MAX_CMD_LENGTH] = {0};
    char gateway[MAX_CMD_LENGTH] = {0};
    char dns[MAX_CMD_LENGTH * 2] = {0};
    char *lineStr = NULL;
    char *iface = NULL;
    char *v6[4] = {NULL, NULL, NULL, NULL};
    uint64_t ip = 0, mask = 0, gw = 0, dhcp = 0, dns1 = 0, dns2 = 0, rate = 0;
    int32_t mtu = 0;
    ResponseInfo *pResponse = NULL;
    ModemReportErrorInfo errInfo = {};

    if (ppDcr == NULL || *ppDcr == NULL) {
        errInfo.errorNo = HRIL_ERR_NULL_POINT;
        return errInfo;
    }
    HRilDataCallResponse *call = &(*ppDcr)[activeIndex];
    GenerateCommand(cmd, MAX_CMD_LENGTH, "AT^DHCP=%d", call->cid);
    int32_t ret = SendCommandLock(cmd, "^DHCP:", 0, &pResponse);
    if (ret != 0 || pResponse == NULL || pResponse->head == NULL || !pResponse->success) {
        errInfo = GetReportErrorInfo(pResponse);
        errInfo.errorNo = (ret != HRIL_ERR_SUCCESS) ? ret : errInfo.errorNo;
        TELEPHONY_LOGE("send AT CMD failed! ret:%{public}d", errInfo.errorNo);
        FreeResponseInfo(pResponse);
        return errInfo;
    }
    lineStr = pResponse->head->data;
    SkipATPrefix(&lineStr);
    NextULongFromHex(&lineStr, &ip);
    NextULongFromHex(&lineStr, &mask);
    NextULongFromHex(&lineStr, &gw);
    NextULongFromHex(&lineStr, &dhcp);
    NextULongFromHex(&lineStr, &dns1);
    NextULongFromHex(&lineStr, &dns2);
    NextULongFromHex(&lineStr, &rate);
    NextULongFromHex(&lineStr, &rate);
    NextStr(&lineStr, &iface);
    NextInt(&lineStr, &mtu);
    for (int32_t i = 0; i < 4; i++) {
        NextStr(&lineStr, &v6[i]);
    }
    if (ip != 0) {
        char buf[MAX_CMD_LENGTH] = {0};
        AddressFormat(ip, buf, sizeof(buf));
        GenerateCommand(buf + strlen(buf), MAX_CMD_LENGTH - strlen(buf), "/%d",
            __builtin_popcountll(mask & 0xFFFFFFFFULL));
        AppendAddress(address, sizeof(address), buf);
    }
    AppendAddress(address, sizeof(address), v6[0]);
    AppendIpv4(gateway, sizeof(gateway), gw);
    AppendAddress(gateway, sizeof(gateway), v6[1]);
    AppendIpv4(dns, sizeof(dns), dns1);
    AppendIpv4(dns, sizeof(dns), dns2);
    AppendAddress(dns, sizeof(dns), v6[2]);
    AppendAddress(dns, sizeof(dns), v6[3]);
    call->address = strdup(address);
    call->gateway = strdup(gateway);
    call->dns = strdup(dns);
    call->maxTransferUnit = mtu;
    if (iface != NULL && iface[0] != '\0') {
        free(call->netPortName);
        call->netPortName = strdup(iface);
    }
    FreeResponseInfo(pResponse);
    call->reason = HRIL_PDP_ERR_NONE;
    return errInfo;
}
'''


def data_link_info(s):
    # Each call's own interface (asteroids_data puts call n on rmnet_data<n-1>), its MTU, and IPv6: the library asked
    # "AT^DHCP?" for every call alike and knew one IPv4 address.
    if "AppendAddress" in s:
        return s
    start = s.index("static ModemReportErrorInfo GetLinkInformation(")
    end = s.index("static ModemReportErrorInfo SendInquireCGDCONT(")
    return s[:start] + LINK_INFO + "\n" + s[end:]


AUTH = r'''/* The APN's credentials, as SetDataProfileInfo sends them for the attach APN (password before user name);
 * asteroids_atd passes them to the call. */
static void SendCmdAUTHDATA(int32_t cid, const HRilDataInfo *pDataInfo)
{
    char cmd[MAX_CMD_LENGTH] = {0};
    ResponseInfo *pResponse = NULL;

    if ((pDataInfo->verType < VERIFY_TYPE_MIN) || (pDataInfo->verType > VERIFY_TYPE_MAX + 1)) {
        return;
    }
    if (GenerateCommand(cmd, MAX_CMD_LENGTH, "AT^AUTHDATA=%d,%d,\"\",\"%s\",\"%s\"", cid, pDataInfo->verType,
        pDataInfo->password != NULL ? pDataInfo->password : "", pDataInfo->userName != NULL ? pDataInfo->userName : "") < 0) {
        return;
    }
    SendCommandLock(cmd, NULL, 0, &pResponse);
    FreeResponseInfo(pResponse);
}

'''


def data_auth(s):
    # The library sent an APN's user name and password only for the initial attach APN, never for a call.
    if "SendCmdAUTHDATA" in s:
        return s
    s = s.replace("static int32_t SendCmdNDISDUP(", AUTH + "static int32_t SendCmdNDISDUP(", 1)
    old = '''    if (SendCmdCGDCONT(cid, requestInfo, pDataInfo) != HRIL_ERR_SUCCESS) {
        RouteDown();
        return;
    }
'''
    assert old in s
    return s.replace(old, old + "    SendCmdAUTHDATA(cid, pDataInfo);\n", 1)


edit("at_data.c", data_link_info)
edit("at_data.c", data_auth)


LIST_UPDATE = r'''/* The donor's libhril never runs OnTimerCallback's function, so a ^DATADISCONN (asteroids_atd's report that a call
 * ended) never reached telephony, which kept a dead call "connected". The list is read on a thread of its own instead:
 * not on the reader thread that got the report, which has to read the answers. */
static void *PdpContextListThread(void *arg)
{
    (void)arg;
    usleep(CALLBACK_DELAY_MS * DELAY_US_OFFSET);
    InquirePdpContextList(DEFAULT_CID, NULL);
    return NULL;
}

void PdpContextListUpdate(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, PdpContextListThread, NULL) != 0) {
        TELEPHONY_LOGE("PdpContextListUpdate: no thread");
    }
    pthread_attr_destroy(&attr);
}'''


def data_list_update(s):
    if "PdpContextListThread" in s:
        return s
    old = '''void PdpContextListUpdate(void)
{
    struct timeval tv = {0, CALLBACK_DELAY_MS * DELAY_US_OFFSET};
    OnTimerCallback(PdpContextListCallback, NULL, &tv);
}'''
    assert old in s
    s = s.replace(old, LIST_UPDATE, 1)
    s = s.replace('''static void PdpContextListCallback(uint8_t *param)
{
    InquirePdpContextList(DEFAULT_CID, NULL);
}
''', "", 1)
    return s.replace('#include <stdlib.h>\n', '#include <pthread.h>\n#include <stdlib.h>\n#include <unistd.h>\n', 1)


edit("at_data.c", data_list_update)


def sim_absent(s):
    # +CPIN? with no card answers 27.007's CME ERROR 10 (SIM not inserted), or 13 (SIM failure) for a slot the modem
    # finds in error; the library compared the code with its own HRIL_ERR_NO_SIMCARD_INSERTED (11) and otherwise
    # reported the query as failed, so telephony kept a removed card "ready".
    old = "        if (ret == HRIL_ERR_NO_SIMCARD_INSERTED) {\n            cardState.simState = HRIL_SIM_NOT_INSERTED;"
    new = ("        if (ret == HRIL_ERR_NO_SIMCARD_INSERTED || ret == 10 || ret == 13) {\n"
           "            cardState.simState = HRIL_SIM_NOT_INSERTED;")
    assert old in s or new in s
    return s.replace(old, new)


edit("at_sim.c", sim_absent)


SLOT2_RADIO = r'''/* Radio power is the modem's, set by slot 1: AT+CFUN on slot 2's port powers the whole modem down. With slot 2
 * empty telephony turned its radio off every few seconds, which reset the card in slot 1 each time, and slot 1 turned
 * it back on. Slot 2's requests only record what it wants; it hears the radio on while the modem is on and it wants it. */
int32_t g_asteroidsSlot2Radio = 1;

static void ReportSlot2Radio(void)
{
    struct ReportInfo reportInfo = {0};
    HRilRadioState state = (GetRadioState() == HRIL_RADIO_POWER_STATE_ON && g_asteroidsSlot2Radio) ?
        HRIL_RADIO_POWER_STATE_ON : HRIL_RADIO_POWER_STATE_OFF;
    reportInfo.notifyId = HNOTI_MODEM_RADIO_STATE_UPDATED;
    reportInfo.type = HRIL_NOTIFICATION;
    reportInfo.error = HRIL_ERR_SUCCESS;
    OnModemReport(1, reportInfo, (const uint8_t *)&state, sizeof(HRilRadioState));
}

'''


def radio_slot2(s):
    if "g_asteroidsSlot2Radio" in s:
        return s
    s = s.replace("void ReqSetRadioState(", SLOT2_RADIO + "void ReqSetRadioState(", 1)
    s = s.replace('#include "at_modem.h"\n', '#include "at_modem.h"\n#include "hril_notification.h"\n', 1)
    old = """    struct ReportInfo reportInfo;
    int32_t ret = SetRadioState(function, reset);"""
    new = """    struct ReportInfo reportInfo;
    if (requestInfo->slotId != 0) {
        g_asteroidsSlot2Radio = function != 0;
        reportInfo = CreateReportInfo(requestInfo, HRIL_ERR_SUCCESS, HRIL_RESPONSE, 0);
        OnModemReport(requestInfo->slotId, reportInfo, NULL, 0);
        ReportSlot2Radio();
        return;
    }
    int32_t ret = SetRadioState(function, reset);"""
    assert old in s
    s = s.replace(old, new, 1)
    old = """    const long long timeOut = DEFAULT_TIMEOUT;
    char *pLine = NULL;
    int32_t radioState = -1;
    ResponseInfo *pResponse = NULL;
    struct ReportInfo reportInfo;
"""
    new = old + """    if (requestInfo->slotId != 0) {
        radioState = (GetRadioState() == HRIL_RADIO_POWER_STATE_ON && g_asteroidsSlot2Radio) ? 1 : 0;
        reportInfo = CreateReportInfo(requestInfo, HRIL_ERR_SUCCESS, HRIL_RESPONSE, 0);
        OnModemReport(requestInfo->slotId, reportInfo, (const uint8_t *)&radioState, sizeof(int32_t));
        return;
    }
"""
    assert old in s
    return s.replace(old, new, 1)


edit("at_modem.c", radio_slot2)
