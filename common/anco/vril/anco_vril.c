// The modem RIL behind libril_msgtransfer.z.so in anco_vrild: Android's RIL interface (VRIL_Init is RIL_Init), so
// libril_msgtransfer can relay the guest's requests to it. It answers from slot 1's state as asteroids_atd publishes
// it (/data/asteroids/modem-state, refreshed every 5 s): SIM, registration, operator. The guest's mobile data goes out
// over its cellular link to the host (a veth pair), so its data call is that link, not a modem bearer.
// Answer layouts are Android's (ril.h); libril_msgtransfer copies most of them to the guest as they are
// (RIL_PROXY_EncodeContinuousData), so they hold no pointers the guest would follow.
#include <arpa/inet.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void vril_log(const char *fmt, ...);

#define STATE_PATH "/data/asteroids/modem-state"
#define SLOTS 2

typedef void *RIL_Token;
typedef struct {
    void (*OnRequestComplete)(RIL_Token t, int e, void *response, size_t responselen);
    void (*OnUnsolicitedResponse)(int unsol, const void *data, size_t datalen, int socket);
    void (*RequestTimedCallback)(void (*cb)(void *), void *param, const void *relativeTime);
    void (*OnRequestAck)(RIL_Token t);
} RIL_Env;

typedef struct {
    int version;
    void (*onRequest)(int request, void *data, size_t datalen, RIL_Token t, int socket);
    int (*onStateRequest)(int socket);
    int (*supports)(int requestCode);
    void (*onCancel)(RIL_Token t);
    const char *(*getVersion)(void);
} RIL_RadioFunctions;

enum { RIL_E_SUCCESS = 0, RIL_E_GENERIC_FAILURE = 2, RIL_E_REQUEST_NOT_SUPPORTED = 6 };
enum { RADIO_STATE_ON = 10 };
enum {
    RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED = 1000,
    RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED = 1002,
    RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED = 1019,
};
enum {
    RIL_REQUEST_GET_SIM_STATUS = 1,
    RIL_REQUEST_GET_CURRENT_CALLS = 9,
    RIL_REQUEST_GET_IMSI = 11,
    RIL_REQUEST_VOICE_REGISTRATION_STATE = 20,
    RIL_REQUEST_DATA_REGISTRATION_STATE = 21,
    RIL_REQUEST_OPERATOR = 22,
    RIL_REQUEST_RADIO_POWER = 23,
    RIL_REQUEST_SETUP_DATA_CALL = 27,
    RIL_REQUEST_DEACTIVATE_DATA_CALL = 41,
    RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE = 45,
    RIL_REQUEST_BASEBAND_VERSION = 51,
    RIL_REQUEST_DATA_CALL_LIST = 57,
    RIL_REQUEST_SCREEN_STATE = 61,
    RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE = 73,
    RIL_REQUEST_SET_TTY_MODE = 80,
    RIL_REQUEST_DEVICE_IDENTITY = 98,
    RIL_REQUEST_VOICE_RADIO_TECH = 108,
    RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE = 110,
    RIL_REQUEST_SET_INITIAL_ATTACH_APN = 111,
    RIL_REQUEST_IMS_REGISTRATION_STATE = 112,
    RIL_REQUEST_SET_UICC_SUBSCRIPTION = 122,
    RIL_REQUEST_ALLOW_DATA = 123,
    RIL_REQUEST_SET_DATA_PROFILE = 128,
    RIL_REQUEST_SEND_DEVICE_STATE = 138,
    RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER = 139,
    RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA = 151,
    RIL_REQUEST_SET_LINK_CAPACITY_REPORTING_CRITERIA = 152,
};

// RIL_CardStatus_v6 and Huawei's tail (physical slot, ATR, ICCID, EID): 0x1b8 bytes.
typedef struct {
    int app_type, app_state, perso_substate;
    char *aid_ptr, *app_label_ptr;
    int pin1_replaced, pin1, pin2;
} RIL_AppStatus;
typedef struct {
    int card_state, universal_pin_state;
    int gsm_umts_subscription_app_index, cdma_subscription_app_index, ims_subscription_app_index;
    int num_applications;
    RIL_AppStatus applications[8];
    int physicalSlotId;
    char *atr, *iccid, *eid;
} RIL_CardStatus;
_Static_assert(sizeof(RIL_CardStatus) == 0x1b8, "card status");

// RIL_CellIdentity_v16: the cell type and a union of the per-technology identities (LTE's here; the operator names
// are pointers, left NULL).
typedef struct {
    int cellInfoType;
    union {
        struct {
            int mcc, mnc, ci, pci, tac, earfcn;
            char *alphaShort, *alphaLong;
        } lte;
        char raw[40];
    };
} RIL_CellIdentity;
_Static_assert(sizeof(RIL_CellIdentity) == 0x30, "cell identity");
typedef struct {
    int regState, rat, cssSupported, roamingIndicator, systemIsInPrl, defaultRoamingIndicator, reasonForDenial;
    RIL_CellIdentity cellIdentity;
} RIL_VoiceRegistrationStateResponse;
_Static_assert(sizeof(RIL_VoiceRegistrationStateResponse) == 0x50, "voice registration");
typedef struct {
    int regState, rat, reasonDataDenied, maxDataCalls;
    RIL_CellIdentity cellIdentity;
    int extra[2]; /* LTE VoPS info */
} RIL_DataRegistrationStateResponse;
_Static_assert(sizeof(RIL_DataRegistrationStateResponse) == 0x48, "data registration");

// A data call: Android's RIL_Data_Call_Response_v11 as Huawei extends it (0x80 bytes: a 64-bit retry time, MTUs for
// IPv4 and IPv6, newer strings), as libril_msgtransfer's encoder reads it.
typedef struct {
    int status;
    int64_t suggestedRetryTime;
    int cid, active;
    char *type, *ifname, *addresses, *dnses, *gateways, *pcscf;
    int mtuV4, mtuV6;
    char *more[4];
    int tail[4];
} RIL_Data_Call_Response;
_Static_assert(sizeof(RIL_Data_Call_Response) == 0x80, "data call");

struct State {
    int sim;
    char imsi[32], iccid[32], imei[32], name[64], numeric[16];
    int csStat, psStat, act;
    // Slot 1's data call, from asteroids_atd's "data=<cid>,<apn>,^DHCP: <ip>,<mask>,<gw>,<dhcp>,<dns1>,<dns2>,...,
    // "<ifname>",<mtu>,..." (addresses as hex, least significant byte first).
    int dataCid, dataMtu;
    char dataIf[32], dataAddr[40], dataGw[20], dataDns[40];
};

static const RIL_Env *g_env;

static char *Value(char *line, const char *key)
{
    size_t n = strlen(key);
    return !strncmp(line, key, n) && line[n] == '=' ? line + n + 1 : NULL;
}

// "+CxREG: [n,]stat[,...]": the stat, -1 if none.
static int RegStat(const char *s)
{
    int a, b;
    if (s == NULL) return -1;
    int n = sscanf(s, "%*[^:]: %d,%d", &a, &b);
    return n == 2 ? b : n == 1 ? a : -1;
}

static void Ipv4(char *out, size_t size, unsigned int le)
{
    snprintf(out, size, "%u.%u.%u.%u", le & 0xff, (le >> 8) & 0xff, (le >> 16) & 0xff, le >> 24);
}

static void ParseData(struct State *st, const char *v)
{
    unsigned int ip, mask, gw, srv, dns1, dns2;
    unsigned long long rx, tx;
    int cid, mtu;
    char ifname[32];
    const char *d = strstr(v, "^DHCP:");
    if (sscanf(v, "%d,", &cid) != 1 || d == NULL ||
        sscanf(d, "^DHCP: %x,%x,%x,%x,%x,%x,%llu,%llu,\"%31[^\"]\",%d", &ip, &mask, &gw, &srv, &dns1, &dns2, &rx,
            &tx, ifname, &mtu) != 10)
        return;
    int prefix = __builtin_popcount(mask);
    char a[20], b[20];
    Ipv4(a, sizeof(a), ip);
    snprintf(st->dataAddr, sizeof(st->dataAddr), "%s/%d", a, prefix);
    Ipv4(st->dataGw, sizeof(st->dataGw), gw);
    Ipv4(a, sizeof(a), dns1);
    Ipv4(b, sizeof(b), dns2);
    snprintf(st->dataDns, sizeof(st->dataDns), "%s %s", a, b);
    snprintf(st->dataIf, sizeof(st->dataIf), "%s", ifname);
    st->dataCid = cid;
    st->dataMtu = mtu;
}

static void ReadState(struct State *st)
{
    memset(st, 0, sizeof(*st));
    st->csStat = st->psStat = st->act = -1;
    FILE *f = fopen(STATE_PATH, "r");
    if (f == NULL) return;
    char line[600], *v;
    int eps = -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if ((v = Value(line, "sim"))) st->sim = atoi(v);
        else if ((v = Value(line, "imsi"))) snprintf(st->imsi, sizeof(st->imsi), "%s", v);
        else if ((v = Value(line, "imei"))) snprintf(st->imei, sizeof(st->imei), "%s", v);
        else if ((v = Value(line, "name"))) snprintf(st->name, sizeof(st->name), "%s", v);
        else if ((v = Value(line, "cs"))) st->csStat = RegStat(v);
        else if ((v = Value(line, "data")) && !st->dataIf[0]) ParseData(st, v);
        else if ((v = Value(line, "ps"))) st->psStat = RegStat(v);
        else if ((v = Value(line, "iccid"))) {
            // EF_ICCID: BCD with the digits of each byte swapped, padded with F.
            size_t k = 0;
            for (size_t i = 0; v[i] && v[i + 1] && k + 2 < sizeof(st->iccid); i += 2) {
                if (v[i + 1] != 'F' && v[i + 1] != 'f') st->iccid[k++] = v[i + 1];
                if (v[i] != 'F' && v[i] != 'f') st->iccid[k++] = v[i];
            }
            st->iccid[k] = '\0';
        } else if ((v = Value(line, "raw"))) {
            int mode, fmt, act;
            char oper[64];
            if (sscanf(v, "+COPS: %d,%d,\"%63[^\"]\",%d", &mode, &fmt, oper, &act) == 4) {
                st->act = act;
                if (fmt == 2) snprintf(st->numeric, sizeof(st->numeric), "%s", oper);
                else if (fmt == 0 && !st->name[0]) snprintf(st->name, sizeof(st->name), "%s", oper);
            } else if (!strncmp(v, "+CEREG:", 7)) {
                eps = RegStat(v);
            }
        }
    }
    fclose(f);
    if (st->psStat != 1 && st->psStat != 5 && (eps == 1 || eps == 5)) st->psStat = eps;
    if (!st->numeric[0] && strlen(st->imsi) >= 5) snprintf(st->numeric, sizeof(st->numeric), "%.5s", st->imsi);
}

// 27.007 access technology to Android's RIL_RadioTechnology.
static int RilRat(int act)
{
    switch (act) {
    case 0: case 1: case 3: return 16;           /* GSM */
    case 2: return 3;                            /* UMTS */
    case 4: case 5: case 6: return 11;           /* HSPA */
    case 7: case 9: case 10: case 13: return 14; /* LTE (13: with NR, non-standalone) */
    case 11: case 12: return 20;                 /* NR */
    default: return 0;
    }
}

static void Complete(RIL_Token t, int e, void *data, size_t len) { g_env->OnRequestComplete(t, e, data, len); }

static void SimStatus(RIL_Token t, int socket, const struct State *st)
{
    RIL_CardStatus cs;
    memset(&cs, 0, sizeof(cs));
    cs.physicalSlotId = socket;
    cs.cdma_subscription_app_index = cs.ims_subscription_app_index = -1;
    cs.atr = cs.eid = "";
    if (socket == 0 && st->sim) {
        cs.card_state = 1; /* present */
        cs.gsm_umts_subscription_app_index = 0;
        cs.num_applications = 1;
        cs.applications[0] = (RIL_AppStatus){ .app_type = 2 /* USIM */, .app_state = 5 /* ready */,
            .perso_substate = 2 /* ready */, .aid_ptr = "", .app_label_ptr = "", .pin1 = 3 /* disabled */,
            .pin2 = 3 };
        cs.iccid = (char *)st->iccid;
    } else {
        cs.gsm_umts_subscription_app_index = -1;
        cs.iccid = "";
    }
    Complete(t, RIL_E_SUCCESS, &cs, sizeof(cs));
}

static void FillCell(RIL_CellIdentity *c, const struct State *st)
{
    memset(c, 0, sizeof(*c));
    if (RilRat(st->act) != 14 || strlen(st->numeric) < 5) return;
    c->cellInfoType = 3; /* LTE */
    char mcc[4] = { 0 };
    memcpy(mcc, st->numeric, 3);
    c->lte.mcc = atoi(mcc);
    c->lte.mnc = atoi(st->numeric + 3);
    c->lte.ci = c->lte.pci = c->lte.tac = c->lte.earfcn = 0x7fffffff;
}

// The phone's data call as the guest gets it: its cellular link to the host (the container engine's veth pair,
// host ancormnet_data0, guest rmnet_data0, at Huawei's fixed 172.17.0.2/24 with the host as gateway, the same values
// libril_msgtransfer puts in with persist.ril.need_modify_anco_link_addr on), with the network's DNS servers.
static int FillDataCall(RIL_Data_Call_Response *r, struct State *st)
{
    memset(r, 0, sizeof(*r));
    if (!st->dataIf[0]) {
        vril_log("data call: the phone has none");
        return 0;
    }
    r->suggestedRetryTime = -1;
    r->cid = st->dataCid;
    r->active = 2; /* up */
    r->type = "IP";
    r->ifname = "rmnet_data0";
    r->addresses = "172.17.0.2/24";
    r->dnses = st->dataDns;
    r->gateways = "172.17.0.1";
    r->pcscf = "";
    r->mtuV4 = st->dataMtu;
    r->more[0] = r->more[1] = r->more[2] = r->more[3] = "";
    return 1;
}

static void OnRequest(int request, void *data, size_t datalen, RIL_Token t, int socket)
{
    struct State st;
    ReadState(&st);
    int registered = socket == 0 && st.sim;
    switch (request) {
    case RIL_REQUEST_GET_SIM_STATUS:
        SimStatus(t, socket, &st);
        break;
    case RIL_REQUEST_GET_IMSI:
        if (registered && st.imsi[0]) Complete(t, RIL_E_SUCCESS, st.imsi, strlen(st.imsi) + 1);
        else Complete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        break;
    case RIL_REQUEST_GET_CURRENT_CALLS:
        Complete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_VOICE_REGISTRATION_STATE: {
        RIL_VoiceRegistrationStateResponse r;
        memset(&r, 0, sizeof(r));
        r.regState = registered && st.csStat >= 0 ? st.csStat : 0;
        r.rat = r.regState == 1 || r.regState == 5 ? RilRat(st.act) : 0;
        if (r.rat) FillCell(&r.cellIdentity, &st);
        Complete(t, RIL_E_SUCCESS, &r, sizeof(r));
        break;
    }
    case RIL_REQUEST_DATA_REGISTRATION_STATE: {
        RIL_DataRegistrationStateResponse r;
        memset(&r, 0, sizeof(r));
        r.regState = registered && st.psStat >= 0 ? st.psStat : 0;
        r.rat = r.regState == 1 || r.regState == 5 ? RilRat(st.act) : 0;
        r.maxDataCalls = 4;
        if (r.rat) FillCell(&r.cellIdentity, &st);
        Complete(t, RIL_E_SUCCESS, &r, sizeof(r));
        break;
    }
    case RIL_REQUEST_OPERATOR: {
        if (!registered || !st.numeric[0]) {
            Complete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        char *ops[3] = { st.name, st.name, st.numeric };
        Complete(t, RIL_E_SUCCESS, ops, sizeof(ops));
        break;
    }
    case RIL_REQUEST_SETUP_DATA_CALL: {
        RIL_Data_Call_Response r;
        if (registered && FillDataCall(&r, &st)) {
            vril_log("data call (phone's on %s): %s %s gw %s dns %s", st.dataIf, r.ifname, r.addresses, r.gateways,
                r.dnses);
            Complete(t, RIL_E_SUCCESS, &r, sizeof(r));
        } else {
            Complete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }
        break;
    }
    case RIL_REQUEST_DATA_CALL_LIST: {
        RIL_Data_Call_Response r;
        if (registered && FillDataCall(&r, &st)) Complete(t, RIL_E_SUCCESS, &r, sizeof(r));
        else Complete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    }
    case RIL_REQUEST_DEVICE_IDENTITY: {
        // IMEI, IMEISV, ESN, MEID; the second slot shares the modem's IMEI here (asteroids_atd reads slot 1's).
        char *ids[4] = { st.imei, "", "", "" };
        if (st.imei[0]) Complete(t, RIL_E_SUCCESS, ids, sizeof(ids));
        else Complete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        break;
    }
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        Complete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE: {
        int automatic = 0;
        Complete(t, RIL_E_SUCCESS, &automatic, sizeof(automatic));
        break;
    }
    case RIL_REQUEST_VOICE_RADIO_TECH: {
        int rat = registered ? RilRat(st.act) : 0;
        Complete(t, RIL_E_SUCCESS, &rat, sizeof(rat));
        break;
    }
    case RIL_REQUEST_IMS_REGISTRATION_STATE: {
        int ims[2] = { 0, 1 /* 3GPP */ };
        Complete(t, RIL_E_SUCCESS, ims, sizeof(ims));
        break;
    }
    case RIL_REQUEST_BASEBAND_VERSION: {
        static char version[] = "asteroids";
        Complete(t, RIL_E_SUCCESS, version, sizeof(version));
        break;
    }
    // Settings the guest makes that change nothing here.
    case RIL_REQUEST_RADIO_POWER:
    case RIL_REQUEST_SCREEN_STATE:
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_SET_TTY_MODE:
    case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
    case RIL_REQUEST_SET_UICC_SUBSCRIPTION:
    case RIL_REQUEST_ALLOW_DATA:
    case RIL_REQUEST_SET_DATA_PROFILE:
    case RIL_REQUEST_SEND_DEVICE_STATE:
    case RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER:
    case RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA:
    case RIL_REQUEST_SET_LINK_CAPACITY_REPORTING_CRITERIA:
        Complete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    default:
        vril_log("req %d len %zu socket %d: not supported", request, datalen, socket);
        Complete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    vril_log("req %d socket %d", request, socket);
}

static int OnStateRequest(int socket) { return RADIO_STATE_ON; }
static int Supports(int code) { return 1; }
static void OnCancel(RIL_Token t) {}
static const char *GetVersion(void) { return "asteroids anco vril 1"; }

static const RIL_RadioFunctions g_funcs = { 12, OnRequest, OnStateRequest, Supports, OnCancel, GetVersion };

// The guest learns of changes from reports: the radio and the SIM once, the network on each change of slot 1's
// registration, access technology or operator.
static void *Watch(void *p)
{
    sleep(2);
    for (int s = 0; s < SLOTS; s++) {
        g_env->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0, s);
        g_env->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0, s);
    }
    char last[160] = "";
    for (;;) {
        struct State st;
        ReadState(&st);
        char now[160];
        snprintf(now, sizeof(now), "%d %d %d %d %s %s", st.sim, st.csStat, st.psStat, st.act, st.numeric, st.iccid);
        if (strcmp(now, last)) {
            vril_log("state: sim %d cs %d ps %d act %d %s", st.sim, st.csStat, st.psStat, st.act, st.numeric);
            if (strncmp(now, last, 2)) g_env->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0, 0);
            g_env->OnUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0, 0);
            snprintf(last, sizeof(last), "%s", now);
        }
        sleep(5);
    }
    return NULL;
}

const RIL_RadioFunctions *VRIL_Init(const RIL_Env *env, int argc, char **argv)
{
    vril_log("VRIL_Init");
    g_env = env;
    pthread_t t;
    if (pthread_create(&t, NULL, Watch, NULL) == 0) pthread_detach(t);
    return &g_funcs;
}
