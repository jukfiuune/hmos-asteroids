/*
 * asteroids_atd: an AT proxy between HarmonyOS's RIL vendor library and the Qualcomm modem.
 *
 * riladapter_host loads OpenHarmony's reference AT vendor library (libril_vendor.z.so, telephony_ril_adapter
 * services/vendor; ours is built by build-ril-vendor.sh), written for Huawei's Balong modems: next to 3GPP
 * 27.007/27.005 commands it sends Huawei ones (^HCSQ, ^SYSINFOEX, ^MONSC, ^MONNC, ^CURCEX, ^SIMST, ^TIME...), which the
 * Qualcomm modem answers with ERROR. This proxy gives the library a pty (a symlink, /dev/asteroids-at by default, set
 * as const.telephony.ril.attty.path), passes 3GPP commands through to the modem line by line, and answers the Huawei
 * ones itself, from 3GPP queries where they report state. Registration answers are rewritten into the form the library
 * parses, with LTE's packet registration (+CEREG) as +CGREG and 27.007 access technologies as the library's; signal
 * strength comes from Qualcomm's $QCRSRP?/$QCRSRQ? and +CSQ, as the modem refuses +CESQ.
 *
 * Reports to the library come from a status watch on slot 1 (see WATCH_MS): SIM state as ^SIMST, registration as
 * +CREG/+CGREG, signal as ^HCSQ, voice calls as ^CCALLSTATE, each on a change. The modem's $QCSIMSTAT reports also
 * become ^SIMST. +CLCC answers lose the modem's data bearers, and ATH becomes +CHUP. Incoming SMS are fetched from
 * the modem's storage and handed over as +CMT. Data calls (Balong's ^NDISDUP) run asteroids_data (see DataCommand).
 *
 * Two SIM slots: the modem's AT ports each serve one subscription ($QCSIMAPP). Slot 1's commands go to the first port
 * (/dev/at_mdm0), slot 2's, which our library sends as "#1#AT...", to the second (/dev/smd8), bound to SUB2 at open.
 * Unsolicited reports from the second port are only logged: the library would take them for slot 1's.
 *
 * Runs as a HarmonyOS init service (asteroids-desktop.cfg) as root: it makes the link in /dev and gives the pty to
 * radio. Logs to /data/local/tmp/asteroids-atd.log, long digit and hex runs (IMEI, PDUs) masked.
 * Usage: asteroids_atd [slot 1 port] [link] [slot 2 port]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RADIO_UID 1001
#define RADIO_GID 1001
#define LOG_PATH "/data/local/tmp/asteroids-atd.log"
#define CMD_TIMEOUT_MS 15000
#define SLOTS 2
#define SLOT_TAG "#1#"

struct Port {
    const char *path;
    const char *bind; /* the subscription command sent at open */
    int bound;        /* the modem took it: early in its boot SUB2 refuses (ERROR), so it is retried */
    int fd;
    char buf[4096];
    size_t len;
    int closed;       /* it went down once: the modem restarted */
};
static struct Port g_ports[SLOTS] = {
    { "/dev/at_mdm0", "AT$QCSIMAPP=0", 0, -1, { 0 }, 0, 0 },
    { "/dev/smd8", "AT$QCSIMAPP=1", 0, -1, { 0 }, 0, 0 },
};
static const char *g_linkPath = "/dev/asteroids-at";
static FILE *g_log;
static int g_pty = -1;

/* The command in flight (the library's, forwarded, or an internal query for a synthesized answer) and its port; DATA is
 * the library's AT^NDISDUP while its data call comes up. */
enum { IDLE, CLIENT, INTERNAL, DATA };
static int g_state = IDLE;
static int g_port;
static long long g_deadline;
static int g_pduPort = -1; /* after "> ": the library's bytes go to that port as they are, up to Ctrl-Z or Esc */
/* Without a SIM the modem's AT processor has no subscription and answers registration and operator queries with
 * a bare ERROR: those get the "not registered, no operator" answer instead (with a SIM the modem's own goes through). */
static const char *g_fallback;
/* The modem answers +CGSN with the IMEI line but no final result: the IMEI line completes it. */
static int g_imei;
static long long g_swallowOkUntil;
/* +CGMR's version line has no prefix, and the library keeps only prefixed lines: it gets "+CGMR: ". Like +CGSN,
 * no final result follows. */
static int g_cgmr;
/* The library's +CPIN? on slot 1, passed through: its answer is what the library knows of the SIM (see BuildCreg). */
static int g_cpin;

/* Internal queries: a list of commands, their answer lines collected, then a builder makes the reply. */
#define MAX_LINES 40
static const char *g_queries[12];
static int g_queryIndex, g_queryCount;
static char g_lines[MAX_LINES][512]; /* an SMS PDU line is up to ~380 hex digits */
static int g_lineCount;
static void (*g_build)(void);
/* The status watch on slot 1: every WATCH_MS while idle the proxy queries the modem itself and sends the library the
 * reports it knows, on a change only. The modem's own reports don't cover it: it has no ^SIMST, reports LTE's packet
 * registration only in +CEREG, sends no signal reports, and a card reset (the modem does one after activating a SIM,
 * as it loads the carrier's configuration) clears its report settings. */
#define WATCH_MS 5000
#define CALL_WATCH_MS 1000
static int g_watch; /* the internal query in flight is the watch's: nothing goes to the library but its reports */
static long long g_nextWatch;
static int g_lastSim = -1, g_lastLevel = -1;
/* The modem forgets the library's report and SMS settings when it resets the card (after the first activation, about
 * 38 s into boot, after the library's init): once the SIM is back, the proxy sends them again (see Reapply). */
static int g_libraryUp, g_reapply;
/* A modem restart brings the modem back in low power mode with all of that forgotten; the library knows nothing of it
 * (Android's qcril sets the radio power again). The library's last AT+CFUN goes again once slot 1's port is back. */
static char g_cfun[24];
static int g_restoreCfun;
/* Incoming SMS: the modem stores them in its own memory (ME, the library's receive storage) instead of routing them
 * as +CMT, whatever +CNMI says. The watch counts them (+CPMS?); one at a time they are read (+CMGL) and handed to the
 * library as +CMT, and deleted once the library acknowledges it (+CNMA=1, sent after telephony has kept the message;
 * the proxy answers that itself). A message the library refuses (+CNMA=2) or doesn't acknowledge stays. */
#define SMS_ACK_MS 60000
static int g_smsWaiting;          /* messages in ME, from the last round */
static int g_smsIndex = -1;       /* the message handed to the library, until acknowledged */
static long long g_smsDeadline;   /* after this an unacknowledged message is handed over again */
static int g_smsDelete = -1;      /* acknowledged: delete it on the next idle moment */
static char g_lastCs[128], g_lastPs[128], g_lastRat[16];
/* Slot 1's state for the Android container's RIL (common/anco/vril), written after each watch round to STATE_PATH
 * (root only): the watch's answers, and what the library reads through the proxy that the watch doesn't (the IMSI,
 * the ICCID's EF, the IMEI, the operator's long name). */
#define STATE_PATH "/data/asteroids/modem-state"
static char g_imsi[32], g_iccid[64], g_opLong[64], g_imeiValue[32];
static int g_wantImsi, g_wantIccid;
/* Voice calls as "id,stat;" from +CLCC: the library learns of call changes from Huawei's ^CCALLSTATE, on which it
 * reads the call list again; the modem sends nothing of the kind. */
static char g_lastCalls[128];
static int g_lastCallId;
/* Mobile data: the library speaks Balong's NDIS commands. Its contexts (AT+CGDCONT, with AT^AUTHDATA's credentials)
 * are kept here, away from the modem's own (LTE attach, IMS); AT^NDISDUP=<cid>,1 runs asteroids_data on the
 * context's APN and IP type, which brings up the call's rmnet_data interface and prints the network's settings;
 * AT+CGACT? and AT^DHCP=<cid> report the call from them, and telephony's netmanager configures the interface. Up to
 * DATA_CALLS calls at once (the internet, MMS...), call slot i on mux i + 1 (rmnet_data<i>). When the network drops
 * one the library hears ^DATADISCONN and reads the context list again. */
#define DATA_TOOL "/system/bin/asteroids_data"
#define DATA_LOG "/data/local/tmp/asteroids-data.log"
#define DATA_SETUP_MS 45000
#define DATA_CIDS 11
#define DATA_CALLS 4
static char g_ctxType[DATA_CIDS + 1][16], g_ctxApn[DATA_CIDS + 1][100], g_ctxUser[DATA_CIDS + 1][64],
    g_ctxPassword[DATA_CIDS + 1][64];
static int g_ctxAuth[DATA_CIDS + 1];
struct DataCall {
    int cid, pid, fd, up;
    char out[1024];
    size_t outLen;
    char dhcp[512];
};
static struct DataCall g_calls[DATA_CALLS];
static int g_dataPending = -1; /* the call whose AT^NDISDUP the library waits on (state DATA) */

/* Call audio: the PAL server (platform/qcom/audio/sheng/pal_server.c, in /android) opens the modem's voice stream while a call
 * exists; the audio flows between the modem and the ADSP. Its socket protocol is sheng's (protocol.h). */
#define PAL_SOCKET "sheng.pal.pcm.v1"
#define PAL_MAGIC 0x53415031u
#define PAL_VOICE_START 100
#define PAL_VOICE_STOP 101
#define PAL_DEVICE_OUT_HANDSET 2
#define PAL_DEVICE_IN_HANDSET_MIC 27
#define VSID_SUB1 0x11C05000u
static void Log(const char *fmt, ...);
static int g_voiceOn = -1; /* unknown at start: the first round stops a stream a previous run may have left */

static int PalRequest(uint32_t op, uint32_t arg0, uint32_t arg1)
{
    struct { uint32_t magic, op, bytes, sequence, arg0, arg1; } req = { PAL_MAGIC, op, 0, 1, arg0, arg1 };
    struct { uint32_t magic, sequence; int32_t rc; uint32_t bytes; uint64_t frames; } reply = { 0 };
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct timeval tv = { 3, 0 };
    int rc = -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    memcpy(addr.sun_path + 1, PAL_SOCKET, sizeof(PAL_SOCKET) - 1);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, offsetof(struct sockaddr_un, sun_path) + sizeof(PAL_SOCKET)) == 0 &&
        send(fd, &req, sizeof(req), MSG_NOSIGNAL) == sizeof(req) && recv(fd, &reply, sizeof(reply), 0) >= 12)
        rc = reply.rc;
    else
        rc = -errno;
    close(fd);
    return rc;
}

static void CallAudio(int on)
{
    if (on == g_voiceOn)
        return;
    int rc = on ? PalRequest(PAL_VOICE_START, PAL_DEVICE_OUT_HANDSET | PAL_DEVICE_IN_HANDSET_MIC << 16, VSID_SUB1)
                : PalRequest(PAL_VOICE_STOP, 0, 0);
    Log("call audio %s: %d", on ? "on" : "off", rc);
    g_voiceOn = on;
}

static char g_clientBuf[4096];
static size_t g_clientLen;
/* Library commands that arrive while one is in flight (it waits for each answer, but its init sends a burst). */
static char g_queue[16][512];
static int g_queueHead, g_queueTail;

static int PortCommand(int fd, const char *cmd);
static void Finish(void);

static long long NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void Log(const char *fmt, ...)
{
    char buf[1024];
    char out[1024];
    va_list ap;
    size_t i, j, k;

    if (g_log == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Mask runs of 10+ digits and 16+ hex digits: IMEI, IMSI, ICCID, phone numbers, PDUs. */
    for (i = 0, j = 0; buf[i] != '\0' && j < sizeof(out) - 8;) {
        for (k = i; isxdigit((unsigned char)buf[k]); k++)
            ;
        int digits = 1;
        for (size_t m = i; m < k; m++)
            digits &= isdigit((unsigned char)buf[m]) != 0;
        if (k - i >= 16 || (digits && k - i >= 10)) {
            j += snprintf(out + j, sizeof(out) - j, "<%zu>", k - i);
            i = k;
        } else if (k > i) {
            while (i < k && j < sizeof(out) - 8)
                out[j++] = buf[i++];
        } else {
            out[j++] = buf[i++];
        }
    }
    out[j] = '\0';
    fprintf(g_log, "%lld %s\n", NowMs(), out);
    fflush(g_log);
}

static void WriteAll(int fd, const char *s, size_t n)
{
    while (n > 0 && fd >= 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void ToClient(const char *line)
{
    Log("<< %s", line);
    WriteAll(g_pty, "\r\n", 2);
    WriteAll(g_pty, line, strlen(line));
    WriteAll(g_pty, "\r\n", 2);
}

static int ToModem(int port, const char *cmd)
{
    if (g_ports[port].fd < 0)
        return -1;
    if (!g_watch)
        Log("-%d> %s", port + 1, cmd);
    WriteAll(g_ports[port].fd, cmd, strlen(cmd));
    WriteAll(g_ports[port].fd, "\r", 1);
    return 0;
}

static int IsFinal(const char *line)
{
    return !strcmp(line, "OK") || !strcmp(line, "ERROR") || !strcmp(line, "NO CARRIER") ||
           !strcmp(line, "NO ANSWER") || !strcmp(line, "BUSY") || !strcmp(line, "NO DIALTONE") ||
           !strncmp(line, "+CME ERROR:", 11) || !strncmp(line, "+CMS ERROR:", 11) || !strcmp(line, "CONNECT");
}

static const char *FindLine(const char *prefix)
{
    for (int i = 0; i < g_lineCount; i++) {
        if (!strncmp(g_lines[i], prefix, strlen(prefix)))
            return g_lines[i];
    }
    return NULL;
}

/* Registration, "+CxREG: [n,]stat[,"lac","ci"[,act]]" (27.007; +CEREG's area is a TAC). */
struct Reg {
    int stat;
    char lac[16], ci[16];
    int act;
};

static int Registered(int stat)
{
    return stat == 1 || stat == 5;
}

/* The modem's answer to a registration query (it carries the n). */
static int ParseReg(const char *line, struct Reg *r)
{
    const char *p = line != NULL ? strchr(line, ':') : NULL;
    memset(r, 0, sizeof(*r));
    r->act = -1;
    return p != NULL && sscanf(p + 1, " %*d,%d,\"%15[^\"]\",\"%15[^\"]\",%d", &r->stat, r->lac, r->ci, &r->act) >= 1;
}

/* Packet registration: +CGREG covers GSM and UMTS only; on LTE the modem reports it in +CEREG, which the library never
 * asks for. Area, cell and access technology come from +CREG where the modem's +CEREG reporting leaves them out. */
static void PsReg(struct Reg *ps)
{
    struct Reg cs, eps;
    ParseReg(FindLine("+CGREG:"), ps);
    ParseReg(FindLine("+CREG:"), &cs);
    if (ParseReg(FindLine("+CEREG:"), &eps) && Registered(eps.stat) && !Registered(ps->stat)) {
        *ps = eps;
        if (ps->act < 0)
            ps->act = 7;
    }
    if (ps->lac[0] == '\0') {
        memcpy(ps->lac, cs.lac, sizeof(ps->lac));
        memcpy(ps->ci, cs.ci, sizeof(ps->ci));
    }
    if (ps->act < 0)
        ps->act = cs.act;
}

/* 27.007 AcT to the library's HRilActTech: 0-7 are the same; E-UTRAN on a 5G core (9, 10) and E-UTRA-NR dual
 * connectivity (13, NSA) are LTE to it (7), NR (11, 12) is 19. */
static int HrilAct(int act)
{
    if (act == 9 || act == 10 || act == 13)
        return 7;
    if (act == 11 || act == 12)
        return 19;
    return act >= 0 && act <= 7 ? act : 0;
}

/* The library takes "n,stat,lac,ci,act" as an answer and "stat,lac,ci,act" as a report. (Its 5G form adds
 * ",nr available,EN-DC available,DC-NR restricted"; with 1,1,0 on act 13 telephony's display type became NR, but
 * SceneBoard then drew no type label at all, so LTE cells report as LTE.) */
static void FormatReg(char *out, size_t size, const char *prefix, const struct Reg *r, int answer)
{
    snprintf(out, size, "%s %s%d,\"%s\",\"%s\",%d", prefix, answer ? "2," : "", r->stat, r->lac[0] ? r->lac : "0",
        r->ci[0] ? r->ci : "0", HrilAct(r->act));
}

/* The watch reports a change against what the library last heard, its own answers included: the library's query
 * can catch a moment the watch didn't (the card reset after activation reads as not registered for a few ms), and
 * with only the watch's view the library would keep that answer. */
static void BuildCreg(void)
{
    struct Reg r;
    char reply[128];
    ParseReg(FindLine("+CREG:"), &r);
    FormatReg(reply, sizeof(reply), "+CREG:", &r, 1);
    ToClient(reply);
    ToClient("OK");
    FormatReg(g_lastCs, sizeof(g_lastCs), "+CREG:", &r, 0);
}

static void BuildCgreg(void)
{
    struct Reg r;
    char reply[128];
    PsReg(&r);
    FormatReg(reply, sizeof(reply), "+CGREG:", &r, 1);
    ToClient(reply);
    ToClient("OK");
    FormatReg(g_lastPs, sizeof(g_lastPs), "+CGREG:", &r, 0);
}

/* ^SYSINFOEX: <srv_status>,<srv_domain>,<roam>,<sim_state>,<lock>,<sysmode>,<name>,<submode>,<name> from
 * +CPIN?, +CREG?/+CGREG?/+CEREG? and +COPS?'s access technology. srv_status 0 no service, 2 normal; sim_state 1 valid,
 * 255 absent; sysmode 3 GSM, 5 WCDMA, 6 LTE, 7 NR (Huawei's numbering). */
static void BuildSysinfoex(void)
{
    const char *cpin = FindLine("+CPIN:");
    const char *creg = FindLine("+CREG:");
    const char *cops = FindLine("+COPS:");
    int sim = (cpin != NULL && strstr(cpin, "READY") != NULL) ? 1 : 255;
    int cs = 0, ps = 0, n, act = -1;
    char oper[64];
    if (creg != NULL)
        sscanf(creg, "+CREG: %d,%d", &n, &cs);
    struct Reg psReg;
    PsReg(&psReg);
    ps = psReg.stat;
    if (cops != NULL && sscanf(cops, "+COPS: %d,%d,\"%63[^\"]\",%d", &n, &n, oper, &act) != 4)
        act = -1;
    int csOn = cs == 1 || cs == 5, psOn = ps == 1 || ps == 5;
    int srv = (csOn || psOn) ? 2 : 0;
    int domain = csOn && psOn ? 3 : csOn ? 1 : psOn ? 2 : 0;
    int roam = cs == 5 || ps == 5;
    int sysmode = 0, submode = 0;
    const char *name = "";
    /* +COPS AcT (27.007): 7, 9, 10 E-UTRAN; 13 E-UTRA-NR dual connectivity (NSA 5G, voice on LTE); 11, 12 NR (SA).
     * Submodes are the library's HRilVoiceSubMode: GSM 1, WCDMA 41, LTE 101, NR 103. */
    if (srv && (act == 7 || act == 9 || act == 10 || act == 13)) {
        sysmode = 6, submode = 101, name = "LTE";
    } else if (srv && (act == 11 || act == 12)) {
        sysmode = 7, submode = 103, name = "NR";
    } else if (srv && (act == 2 || act == 4 || act == 5 || act == 6)) {
        sysmode = 5, submode = 41, name = "WCDMA";
    } else if (srv && act >= 0) {
        sysmode = 3, submode = 1, name = "GSM";
    }
    char reply[160];
    snprintf(reply, sizeof(reply), "^SYSINFOEX: %d,%d,%d,%d,,%d,\"%s\",%d,\"%s\"", srv, domain, roam, sim, sysmode,
        name, submode, name);
    ToClient(reply);
    ToClient("OK");
}

/* Signal strength from what this modem answers (+CESQ is ERROR here), by +COPS?'s access technology: LTE from
 * $QCRSRP? and $QCRSRQ? (serving cell first: PCI, EARFCN, "dBm"), otherwise +CSQ's RSSI. The library reads the
 * fields after the RAT as magnitudes in dBm and dB (it negates them). Writes the RAT and fields, sets *level (the
 * main magnitude); -1 without service. */
static int Signal(char *out, size_t size, char *rat, int *level)
{
    const char *cops = FindLine("+COPS:");
    const char *rsrpLine = FindLine("$QCRSRP:");
    const char *rsrqLine = FindLine("$QCRSRQ:");
    const char *csq = FindLine("+CSQ:");
    int n, act = -1, rssi = 99, ber = 99;
    float rsrp, rsrq = -10;
    char oper[64];
    if (cops == NULL || sscanf(cops, "+COPS: %d,%d,\"%63[^\"]\",%d", &n, &n, oper, &act) != 4)
        act = -1;
    if (csq != NULL)
        sscanf(csq, "+CSQ: %d,%d", &rssi, &ber);
    int rssiDb = rssi < 32 ? 113 - 2 * rssi : -1;
    ber = ber == 99 ? 0 : ber;
    if ((act == 7 || act == 9 || act == 10 || act == 13 || act == 11 || act == 12) && rsrpLine != NULL &&
        sscanf(rsrpLine, "$QCRSRP: %*d,%*d,\"%f\"", &rsrp) == 1) {
        if (rsrqLine != NULL)
            sscanf(rsrqLine, "$QCRSRQ: %*d,%*d,\"%f\"", &rsrq);
        *level = (int)(-rsrp + 0.5f);
        strcpy(rat, "LTE");
        snprintf(out, size, "\"LTE\",%d,%d,%d,0", rssiDb >= 0 ? rssiDb : *level - 29, (int)(-rsrq + 0.5f), *level);
    } else if ((act == 2 || act == 4 || act == 5 || act == 6) && rssiDb >= 0) {
        *level = rssiDb;
        strcpy(rat, "WCDMA");
        snprintf(out, size, "\"WCDMA\",%d,10,%d,%d", rssiDb, rssiDb, ber);
    } else if (act >= 0 && rssiDb >= 0) {
        *level = rssiDb;
        strcpy(rat, "GSM");
        snprintf(out, size, "\"GSM\",%d,%d", rssiDb, ber);
    } else {
        *level = -1;
        strcpy(rat, "");
        return -1;
    }
    return 0;
}

/* ^HCSQ's answer: ^HCSQ: <n>,<n>,"<rat>",... (its report has no leading numbers). */
static void BuildHcsq(void)
{
    char body[128], rat[16], reply[160];
    int level;
    if (Signal(body, sizeof(body), rat, &level) == 0)
        snprintf(reply, sizeof(reply), "^HCSQ: 0,0,%s", body);
    else
        snprintf(reply, sizeof(reply), "^HCSQ: \"NOSERVICE\"");
    ToClient(reply);
    ToClient("OK");
}

/* The voice calls in +CLCC: <id>,<dir>,<stat>,<mode>,... The modem lists its LTE data bearers there too (mode 1):
 * left in, telephony takes them for calls. */
static int IsVoiceCall(const char *line, int *id, int *stat)
{
    int dir, mode;
    return sscanf(line, "+CLCC: %d,%d,%d,%d", id, &dir, stat, &mode) == 4 && mode == 0;
}

static void BuildClcc(void)
{
    int id, stat;
    for (int i = 0; i < g_lineCount; i++) {
        if (IsVoiceCall(g_lines[i], &id, &stat))
            ToClient(g_lines[i]);
    }
    ToClient("OK");
}

/* ^CCALLSTATE: <id>,<state>,<voice domain 0 = CS> for each call that changed, or for the last one when all ended
 * (the library only takes it as a cue). */
static void WatchCalls(void)
{
    char calls[128] = "", item[16], line[64];
    int id, stat, audio = 0;
    for (int i = 0; i < g_lineCount; i++) {
        if (IsVoiceCall(g_lines[i], &id, &stat) && strlen(calls) < sizeof(calls) - sizeof(item)) {
            snprintf(item, sizeof(item), "%d,%d;", id, stat);
            strcat(calls, item);
            if (strstr(g_lastCalls, item) == NULL) {
                snprintf(line, sizeof(line), "^CCALLSTATE: %d,%d,0", id, stat);
                ToClient(line);
            }
            g_lastCallId = id;
            /* Active, held, dialing or alerting: an incoming call gets audio once answered, as on Android. */
            audio |= stat <= 3;
        }
    }
    if (calls[0] == '\0' && g_lastCalls[0] != '\0') {
        snprintf(line, sizeof(line), "^CCALLSTATE: %d,6,0", g_lastCallId);
        ToClient(line);
    }
    if (strcmp(calls, g_lastCalls))
        Log("watch: calls \"%s\"", calls);
    snprintf(g_lastCalls, sizeof(g_lastCalls), "%s", calls);
    CallAudio(audio);
    /* Every second while a call exists (the round was scheduled before it knew). */
    if (calls[0] != '\0' && g_nextWatch > NowMs() + CALL_WATCH_MS)
        g_nextWatch = NowMs() + CALL_WATCH_MS;
}

static void BuildNothing(void)
{
}

/* STATE_PATH, key=value lines (raw= the watch's answer lines, data= each call that is up: cid, APN, ^DHCP answer). */
static void WriteState(int sim)
{
    char tmp[sizeof(STATE_PATH) + 4];
    snprintf(tmp, sizeof(tmp), "%s.new", STATE_PATH);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        return;
    }
    fprintf(f, "sim=%d\nimsi=%s\niccid=%s\nimei=%s\nname=%s\ncs=%s\nps=%s\n", sim, g_imsi, g_iccid, g_imeiValue,
        g_opLong, g_lastCs, g_lastPs);
    for (int i = 0; i < g_lineCount; i++)
        if (g_lines[i][0] == '+' || g_lines[i][0] == '$')
            fprintf(f, "raw=%s\n", g_lines[i]);
    for (int i = 0; i < DATA_CALLS; i++)
        if (g_calls[i].pid > 0 && g_calls[i].up)
            fprintf(f, "data=%d,%s,%s\n", g_calls[i].cid, g_ctxApn[g_calls[i].cid], g_calls[i].dhcp);
    if (fclose(f) == 0)
        rename(tmp, STATE_PATH);
}

static void BuildWatch(void)
{
    const char *cpin = FindLine("+CPIN:");
    int sim = cpin != NULL && strstr(cpin, "READY") != NULL;
    char line[160], body[128], rat[16];
    struct Reg r;
    int level;
    if (sim != g_lastSim) {
        Log("watch: SIM %s", sim ? "ready" : "not ready");
        g_reapply = sim && g_libraryUp;
        g_lastSim = sim;
        ToClient("^SIMST: 1");
    }
    ParseReg(FindLine("+CREG:"), &r);
    FormatReg(line, sizeof(line), "+CREG:", &r, 0);
    if (strcmp(line, g_lastCs)) {
        snprintf(g_lastCs, sizeof(g_lastCs), "%s", line);
        ToClient(line);
    }
    PsReg(&r);
    FormatReg(line, sizeof(line), "+CGREG:", &r, 0);
    if (strcmp(line, g_lastPs)) {
        snprintf(g_lastPs, sizeof(g_lastPs), "%s", line);
        ToClient(line);
    }
    /* Signal: on a change of RAT or of 3 dB or more (RSRP moves by fractions every few seconds). */
    if (Signal(body, sizeof(body), rat, &level) == 0 &&
        (strcmp(rat, g_lastRat) || g_lastLevel < 0 || abs(level - g_lastLevel) >= 3)) {
        snprintf(g_lastRat, sizeof(g_lastRat), "%s", rat);
        g_lastLevel = level;
        snprintf(line, sizeof(line), "^HCSQ: %s", body);
        ToClient(line);
    } else if (level < 0) {
        g_lastRat[0] = '\0';
        g_lastLevel = -1;
    }
    WatchCalls();
    /* +CPMS: "SM",n,m,"SM",n,m,"ME",used,total: the third is where the modem keeps incoming messages. */
    const char *cpms = FindLine("+CPMS:");
    int used = 0;
    if (cpms != NULL && sscanf(cpms, "+CPMS: \"%*[^\"]\",%*d,%*d,\"%*[^\"]\",%*d,%*d,\"%*[^\"]\",%d", &used) == 1)
        g_smsWaiting = used;
    WriteState(sim);
}

/* The first stored message, to the library as an incoming one. */
static void BuildSmsFetch(void)
{
    int index, stat, len;
    char line[64];
    for (int i = 0; i + 1 < g_lineCount; i++) {
        if (sscanf(g_lines[i], "+CMGL: %d,%d,%*[^,],%d", &index, &stat, &len) != 3 &&
            sscanf(g_lines[i], "+CMGL: %d,%d,,%d", &index, &stat, &len) != 3)
            continue;
        if (!strncmp(g_lines[i + 1], "+CMGL:", 6))
            continue;
        Log("sms: message %d to the library", index);
        snprintf(line, sizeof(line), "+CMT: ,%d", len);
        ToClient(line);
        ToClient(g_lines[i + 1]);
        g_smsIndex = index;
        g_smsDeadline = NowMs() + SMS_ACK_MS;
        return;
    }
    Log("sms: %d waiting but none listed", g_smsWaiting);
    g_smsWaiting = 0;
}

/* The +COPS? answers in order, or "no operator" when the modem had none (no subscription). */
static void BuildCops(void)
{
    int n = 0;
    for (int i = 0; i < g_lineCount; i++) {
        if (!strncmp(g_lines[i], "+COPS:", 6)) {
            ToClient(g_lines[i]);
            n++;
        }
    }
    if (n == 0)
        ToClient("+COPS: 0");
    ToClient("OK");
}

static void StartInternal(int port, void (*build)(void), int count, ...)
{
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++)
        g_queries[i] = va_arg(ap, const char *);
    va_end(ap);
    g_queryCount = count;
    g_queryIndex = 0;
    g_lineCount = 0;
    g_build = build;
    g_state = INTERNAL;
    g_port = port;
    g_deadline = NowMs() + CMD_TIMEOUT_MS;
    ToModem(port, g_queries[0]);
}

static void DataInit(void)
{
    for (int i = 0; i < DATA_CALLS; i++)
        g_calls[i] = (struct DataCall){ .cid = -1, .pid = -1, .fd = -1 };
}

static struct DataCall *DataFind(int cid)
{
    for (int i = 0; i < DATA_CALLS; i++)
        if (g_calls[i].pid > 0 && g_calls[i].cid == cid)
            return &g_calls[i];
    return NULL;
}

/* A call's tool has exited (or been told to): reap it. */
static int DataReap(struct DataCall *c)
{
    int status = 0;
    if (c->fd >= 0)
        close(c->fd);
    if (c->pid > 0)
        waitpid(c->pid, &status, 0);
    Log("data: call on context %d ended (%s %d)", c->cid, WIFSIGNALED(status) ? "signal" : "exit",
        WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    if (g_dataPending == c - g_calls)
        g_dataPending = -1;
    *c = (struct DataCall){ .cid = -1, .pid = -1, .fd = -1 };
    return status;
}

/* Ends a call: asteroids_data stops the network on SIGTERM, within a few seconds. */
static void DataStop(struct DataCall *c)
{
    if (c->pid <= 0)
        return;
    c->up = 0;
    kill(c->pid, SIGTERM);
    for (long long until = NowMs() + 5000; NowMs() < until;) {
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        char buf[256];
        if (poll(&pfd, 1, 200) > 0 && read(c->fd, buf, sizeof(buf)) == 0)
            break;
    }
    if (waitpid(c->pid, NULL, WNOHANG) == 0) {
        kill(c->pid, SIGKILL);
        Log("data: asteroids_data killed");
    }
    DataReap(c);
}

/* A call a previous run of the proxy started: the library starts over, so it goes. */
static void DataKillStale(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        char path[64], comm[32] = "";
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL)
            continue;
        if (fgets(comm, sizeof(comm), f) != NULL && !strcmp(comm, "asteroids_data\n")) {
            Log("data: ending a previous run's call (pid %s)", e->d_name);
            kill(atoi(e->d_name), SIGTERM);
        }
        fclose(f);
    }
    if (d != NULL)
        closedir(d);
}

/* Starts a call on the context in a free call slot; returns the slot or -1. 27.007's PDP types to IP families. */
static int DataStart(int cid)
{
    int fds[2], slot;
    const char *type = g_ctxType[cid];
    const char *family = !strcasecmp(type, "IPV6") ? "6" : !strcasecmp(type, "IPV4V6") ? "46" : "4";
    char mux[4], auth[4];
    for (slot = 0; slot < DATA_CALLS && g_calls[slot].pid > 0; slot++)
        ;
    if (slot == DATA_CALLS || pipe2(fds, O_CLOEXEC) < 0)
        return -1;
    snprintf(mux, sizeof(mux), "%d", slot + 1);
    snprintf(auth, sizeof(auth), "%d", g_ctxAuth[cid]);
    pid_t pid = fork();
    if (pid == 0) {
        int log = open(DATA_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        dup2(fds[1], 1);
        if (log >= 0)
            dup2(log, 2);
        for (int fd = 3; fd < 256; fd++)
            close(fd);
        execl(DATA_TOOL, "asteroids_data", g_ctxApn[cid], family, mux, auth, g_ctxUser[cid], g_ctxPassword[cid],
            (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return -1;
    }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    g_calls[slot] = (struct DataCall){ .cid = cid, .pid = pid, .fd = fds[0] };
    Log("data: starting a call on context %d (%s, IPv%s, rmnet_data%d)", cid, g_ctxApn[cid], family, slot);
    return slot;
}

static unsigned int Ipv4(const char *s)
{
    struct in_addr a = { 0 };
    inet_pton(AF_INET, s, &a);
    return a.s_addr;
}

/* asteroids_data's settings, once all are out (MTU= is the last): Huawei's ^DHCP line, IPv4 addresses as the library
 * reads them (hex of the address's bytes in memory order, 0 without IPv4), then the interface, MTU and IPv6 settings
 * (see patch-ril-vendor.py's data_link_info), and the call is up. */
static void DataSettings(struct DataCall *c)
{
    char iface[16] = "", addr[64] = "", gw[64] = "", dns1[64] = "", dns2[64] = "", addr6[64] = "", gw6[64] = "",
        dns6_1[64] = "", dns6_2[64] = "";
    int prefix = 0, prefix6 = 0, mtu = 0, done = 0;
    struct { const char *key; char *value; } keys[] = { { "IFACE=", iface }, { "ADDR=", addr }, { "GW=", gw },
        { "DNS1=", dns1 }, { "DNS2=", dns2 }, { "ADDR6=", addr6 }, { "GW6=", gw6 }, { "DNS6_1=", dns6_1 },
        { "DNS6_2=", dns6_2 } };
    c->out[c->outLen] = '\0';
    for (char *line = strtok(c->out, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (!strncmp(line, keys[k].key, strlen(keys[k].key)))
                snprintf(keys[k].value, k == 0 ? sizeof(iface) : 64, "%s", line + strlen(keys[k].key));
        if (!strncmp(line, "PREFIX=", 7))
            prefix = atoi(line + 7);
        else if (!strncmp(line, "PREFIX6=", 8))
            prefix6 = atoi(line + 8);
        else if (!strncmp(line, "MTU=", 4)) {
            mtu = atoi(line + 4);
            done = 1;
        }
    }
    c->outLen = 0;
    if (!done)
        return;
    unsigned int mask = htonl(addr[0] && prefix > 0 && prefix <= 32 ? 0xffffffffu << (32 - prefix) : 0);
    char v6[80] = "";
    if (addr6[0])
        snprintf(v6, sizeof(v6), "%s/%d", addr6, prefix6);
    snprintf(c->dhcp, sizeof(c->dhcp), "^DHCP: %x,%x,%x,%x,%x,%x,150000000,50000000,\"%s\",%d,\"%s\",\"%s\",\"%s\","
        "\"%s\"", Ipv4(addr), mask, Ipv4(gw), Ipv4(gw), Ipv4(dns1), Ipv4(dns2), iface, mtu, v6, gw6, dns6_1, dns6_2);
    c->up = 1;
    Log("data: up on context %d (%s): %s/%d via %s, DNS %s %s; %s via %s, DNS %s %s; MTU %d", c->cid, iface, addr,
        prefix, gw, dns1, dns2, v6[0] ? v6 : "no IPv6", gw6, dns6_1, dns6_2, mtu);
    if (g_state == DATA && g_dataPending == c - g_calls) {
        g_dataPending = -1;
        ToClient("OK");
        Finish();
    }
}

/* Output from a call's asteroids_data, or its end. */
static void DataInput(struct DataCall *c)
{
    char buf[512];
    ssize_t r = read(c->fd, buf, sizeof(buf));
    if (r < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (r > 0) {
        if (c->outLen + (size_t)r >= sizeof(c->out))
            c->outLen = 0;
        memcpy(c->out + c->outLen, buf, (size_t)r);
        c->outLen += (size_t)r;
        c->out[c->outLen] = '\0';
        if (!c->up && strstr(c->out, "MTU=") != NULL && c->out[c->outLen - 1] == '\n')
            DataSettings(c);
        return;
    }
    int wasUp = c->up, pending = g_state == DATA && g_dataPending == c - g_calls;
    DataReap(c);
    if (pending) {
        ToClient("ERROR");
        Finish();
    } else if (wasUp) {
        ToClient("^DATADISCONN");
    }
}

static void DataCommand(const char *cmd, const char *up)
{
    char line[600];
    int cid, on;
    struct DataCall *c;
    if (!strncmp(up, "AT+CGDCONT=", 11)) {
        /* <cid>[,"<type>","<apn>",...]; a cid alone clears it. */
        char type[16] = "", apn[100] = "";
        int n = sscanf(cmd + 11, "%d,\"%15[^\"]\",\"%99[^\"]\"", &cid, type, apn);
        if (n < 1 || cid < 1 || cid > DATA_CIDS) {
            ToClient("ERROR");
            return;
        }
        snprintf(g_ctxType[cid], sizeof(g_ctxType[cid]), "%s", n >= 2 ? type : "");
        snprintf(g_ctxApn[cid], sizeof(g_ctxApn[cid]), "%s", n >= 3 ? apn : "");
        g_ctxAuth[cid] = 0;
        g_ctxUser[cid][0] = g_ctxPassword[cid][0] = '\0';
        ToClient("OK");
    } else if (!strcmp(up, "AT+CGDCONT?")) {
        for (cid = 1; cid <= DATA_CIDS; cid++) {
            if (g_ctxType[cid][0] == '\0')
                continue;
            snprintf(line, sizeof(line), "+CGDCONT: %d,\"%s\",\"%s\",\"\",0,0", cid, g_ctxType[cid], g_ctxApn[cid]);
            ToClient(line);
        }
        ToClient("OK");
    } else if (!strcmp(up, "AT+CGACT?")) {
        for (cid = 1; cid <= DATA_CIDS; cid++) {
            if (g_ctxType[cid][0] == '\0')
                continue;
            c = DataFind(cid);
            snprintf(line, sizeof(line), "+CGACT: %d,%d", cid, c != NULL && c->up);
            ToClient(line);
        }
        ToClient("OK");
    } else if (sscanf(up, "AT^DHCP=%d", &cid) == 1 || !strcmp(up, "AT^DHCP?")) {
        /* AT^DHCP? (the unpatched library's form): the first call that is up. */
        c = NULL;
        for (int i = 0; i < DATA_CALLS && c == NULL; i++)
            if (g_calls[i].up && (up[7] == '?' || g_calls[i].cid == cid))
                c = &g_calls[i];
        if (c != NULL)
            ToClient(c->dhcp);
        ToClient(c != NULL ? "OK" : "ERROR");
    } else if (!strncmp(up, "AT^AUTHDATA=", 12)) {
        /* <cid>,<auth>,"","<password>","<user>": case kept, from the command as sent. Auth 0 none, 1 PAP, 2 CHAP,
         * 3 either (QMI's preference bits). */
        char password[64] = "", user[64] = "";
        int auth = 0;
        if (sscanf(cmd + 12, "%d,%d,\"\",\"%63[^\"]\",\"%63[^\"]\"", &cid, &auth, password, user) >= 2 && cid >= 1 &&
            cid <= DATA_CIDS) {
            g_ctxAuth[cid] = auth >= 0 && auth <= 3 && user[0] ? auth : 0;
            snprintf(g_ctxUser[cid], sizeof(g_ctxUser[cid]), "%s", user);
            snprintf(g_ctxPassword[cid], sizeof(g_ctxPassword[cid]), "%s", password);
        }
        ToClient("OK");
    } else if (sscanf(up + 11, "%d,%d", &cid, &on) == 2) { /* AT^NDISDUP=<cid>,<on> */
        if (!on) {
            for (int i = 0; i < DATA_CALLS; i++)
                if (g_calls[i].pid > 0 && (g_calls[i].cid == cid || cid < 0)) {
                    Log("data: the library ends the call on context %d", g_calls[i].cid);
                    DataStop(&g_calls[i]);
                }
            ToClient("OK");
        } else if ((c = DataFind(cid)) != NULL) {
            ToClient(c->up ? "OK" : "ERROR");
        } else if (cid < 1 || cid > DATA_CIDS || g_ctxType[cid][0] == '\0' || (g_dataPending = DataStart(cid)) < 0) {
            ToClient("ERROR");
        } else {
            g_state = DATA;
            g_port = 0;
            g_deadline = NowMs() + DATA_SETUP_MS;
        }
    } else {
        ToClient("ERROR");
    }
}

/* A command from the library: answered here, or passed to its slot's port. */
static void HandleCommand(const char *line)
{
    char up[512];
    size_t i;
    int port = 0;
    const char *cmd = line;
    if (!strncmp(cmd, SLOT_TAG, strlen(SLOT_TAG))) {
        port = 1;
        cmd += strlen(SLOT_TAG);
    }
    for (i = 0; cmd[i] != '\0' && i < sizeof(up) - 1; i++)
        up[i] = (char)toupper((unsigned char)cmd[i]);
    up[i] = '\0';
    Log(">%d> %s", port + 1, cmd);
    if (port == 0 && !strncmp(up, "AT+CFUN=", 8))
        snprintf(g_cfun, sizeof(g_cfun), "%s", up);
    /* The library's first command after it opens the pty: it missed any earlier reports, so the watch sends them all
     * again on its next round. */
    if (!strncmp(up, "ATE0", 4)) {
        g_libraryUp = 1;
        g_lastSim = g_lastLevel = -1;
        g_lastCs[0] = g_lastPs[0] = g_lastRat[0] = g_lastCalls[0] = '\0';
    }
    /* Call control: the watch looks at the call list right after it. */
    if (up[0] == 'A' && up[1] == 'T' && (up[2] == 'D' || up[2] == 'A' || up[2] == 'H' || !strncmp(up, "AT+CHLD", 7) ||
        !strncmp(up, "AT+CHUP", 7)))
        g_nextWatch = 0;

    /* The library's acknowledgement of an incoming message: for one the proxy handed over from storage, the modem has
     * nothing pending; the stored copy goes once telephony has kept it. */
    if (port == 0 && !strncmp(up, "AT+CNMA", 7) && g_smsIndex >= 0) {
        int ok = !strcmp(up, "AT+CNMA") || !strcmp(up, "AT+CNMA=1") || !strcmp(up, "AT+CNMA=0");
        Log("sms: message %d %s", g_smsIndex, ok ? "kept by telephony" : "refused by telephony");
        if (ok)
            g_smsDelete = g_smsIndex;
        g_smsIndex = -1;
        ToClient("OK");
        return;
    }
    /* Huawei report and mode settings: nothing to set on this modem. */
    if (!strncmp(up, "AT^HCSQ=", 8) || !strncmp(up, "AT^CURCEX=", 10) || !strncmp(up, "AT^SIMST=", 9) ||
        !strncmp(up, "AT^TIME=", 8)) {
        ToClient("OK");
        return;
    }
    if (!strncmp(up, "AT+CGDCONT", 10) || !strcmp(up, "AT+CGACT?") || !strncmp(up, "AT^NDISDUP=", 11) ||
        !strncmp(up, "AT^DHCP", 7) || !strncmp(up, "AT^AUTHDATA=", 12)) {
        if (port == 0)
            DataCommand(cmd, up);
        else
            ToClient("ERROR");
        return;
    }
    if (!strcmp(up, "AT^MONSC")) {
        ToClient("^MONSC: NONE");
        ToClient("OK");
        return;
    }
    if (!strcmp(up, "AT^MONNC")) {
        ToClient("^MONNC: NONE");
        ToClient("OK");
        return;
    }
    if (g_ports[port].fd < 0) {
        ToClient("ERROR");
        return;
    }
    /* A port not yet on its subscription would answer for the other slot. */
    if (!g_ports[port].bound) {
        g_ports[port].bound = PortCommand(g_ports[port].fd, g_ports[port].bind) == 0;
        Log("slot %d: %s %s", port + 1, g_ports[port].bind, g_ports[port].bound ? "ok" : "failed");
        if (!g_ports[port].bound) {
            ToClient("ERROR");
            return;
        }
    }
    if (!strcmp(up, "AT^HCSQ?")) {
        StartInternal(port, BuildHcsq, 4, "AT+COPS?", "AT$QCRSRP?", "AT$QCRSRQ?", "AT+CSQ");
        return;
    }
    /* The operator in its three formats; split up, as with no subscription the modem's answer to the compound
     * command trails past its ERROR and runs into the next command. */
    if (!strcmp(up, "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?")) {
        StartInternal(port, BuildCops, 6, "AT+COPS=3,0", "AT+COPS?", "AT+COPS=3,1", "AT+COPS?", "AT+COPS=3,2",
            "AT+COPS?");
        return;
    }
    if (!strcmp(up, "AT+CLCC")) {
        StartInternal(port, BuildClcc, 1, "AT+CLCC");
        return;
    }
    /* ATH ends data calls on this modem's AT processor; +CHUP ends voice calls. */
    if (!strcmp(up, "ATH"))
        cmd = "AT+CHUP";
    if (!strcmp(up, "AT^SYSINFOEX")) {
        StartInternal(port, BuildSysinfoex, 5, "AT+CPIN?", "AT+CREG?", "AT+CGREG?", "AT+CEREG?", "AT+COPS?");
        return;
    }
    /* Registration in the form the library parses (see FormatReg); packet registration includes LTE's. */
    if (!strcmp(up, "AT+CREG?")) {
        StartInternal(port, BuildCreg, 1, "AT+CREG?");
        return;
    }
    if (!strcmp(up, "AT+CGREG?")) {
        StartInternal(port, BuildCgreg, 3, "AT+CGREG?", "AT+CEREG?", "AT+CREG?");
        return;
    }
    g_fallback = NULL;
    if (!strcmp(up, "AT+CEREG?"))
        g_fallback = "+CEREG: 2,0";
    else if (!strcmp(up, "AT+COPS?"))
        g_fallback = "+COPS: 0";
    g_imei = !strcmp(up, "AT+CGSN");
    g_cpin = port == 0 && !strcmp(up, "AT+CPIN?");
    g_wantImsi = port == 0 && !strcmp(up, "AT+CIMI");
    g_wantIccid = port == 0 && !strncmp(up, "AT+CRSM=176,12258,", 18);
    g_cgmr = !strcmp(up, "AT+CGMR");
    g_state = CLIENT;
    g_port = port;
    g_deadline = NowMs() + CMD_TIMEOUT_MS;
    ToModem(port, cmd);
}

static int IsImei(const char *line)
{
    size_t n = strlen(line);
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)line[i]))
            return 0;
    }
    return n >= 14 && n <= 17;
}

static void NextCommand(void)
{
    while (g_state == IDLE && g_queueHead != g_queueTail) {
        char cmd[512];
        memcpy(cmd, g_queue[g_queueHead], sizeof(cmd));
        g_queueHead = (g_queueHead + 1) % 16;
        HandleCommand(cmd);
    }
}

static void Finish(void)
{
    g_state = IDLE;
    g_watch = 0;
    NextCommand();
}

/* A line that answers query: "AT+COPS=3,0" and "AT+COPS?" are both answered by "+COPS: ...". */
static int Answers(const char *query, const char *line)
{
    size_t n = strcspn(query + 2, "?=;");
    return !strncmp(line, query + 2, n) && line[n] == ':';
}

static void Unsolicited(int port, const char *line)
{
    /* The library has one channel and takes every report for slot 1's. */
    if (port == 0) {
        if (!strcmp(line, "OK") && NowMs() < g_swallowOkUntil) {
            Log("<-1 OK (late, for +CGSN)");
            return;
        }
        /* The modem's registration reports carry 27.007's AcT numbers, which the library reads as its own: the watch
         * reports registration instead. */
        if (!strncmp(line, "+CREG:", 6) || !strncmp(line, "+CGREG:", 7) || !strncmp(line, "+CEREG:", 7)) {
            Log("<-1 %s (the watch reports it)", line);
            return;
        }
        /* A call arriving or ending: the watch looks at the call list at once. */
        if (!strcmp(line, "RING") || !strncmp(line, "+CRING:", 7) || !strcmp(line, "NO CARRIER") ||
            !strncmp(line, "+CLIP:", 6) || !strncmp(line, "+CCWA:", 6))
            g_nextWatch = 0;
        /* Qualcomm's SIM state report, for Huawei's: the library then asks +CPIN? again. */
        if (!strncmp(line, "$QCSIMSTAT:", 11)) {
            Log("<-1 %s", line);
            ToClient("^SIMST: 1");
            return;
        }
        ToClient(line);
    } else {
        Log("<-2 %s (slot 2 report, not passed on)", line);
    }
}

static void ModemLine(int port, const char *line)
{
    if (g_state == IDLE || g_state == DATA || port != g_port) {
        Unsolicited(port, line);
        return;
    }
    if (g_state == INTERNAL) {
        if (IsFinal(line)) {
            if (!g_watch)
                Log("<-%d %s", port + 1, line);
            if (++g_queryIndex < g_queryCount) {
                ToModem(port, g_queries[g_queryIndex]);
                return;
            }
            g_build();
            Finish();
            return;
        }
        /* +CMGL's PDU follows its header line, unprefixed. */
        if (g_lineCount > 0 && g_lineCount < MAX_LINES && !strncmp(g_lines[g_lineCount - 1], "+CMGL:", 6) &&
            line[0] != '+' && isxdigit((unsigned char)line[0])) {
            snprintf(g_lines[g_lineCount++], sizeof(g_lines[0]), "%s", line);
            return;
        }
        if (Answers(g_queries[g_queryIndex], line) && g_lineCount < MAX_LINES) {
            if (!g_watch)
                Log("<-%d %s", port + 1, line);
            snprintf(g_lines[g_lineCount++], sizeof(g_lines[0]), "%s", line);
            return;
        }
        Unsolicited(port, line);
        return;
    }
    if (g_fallback != NULL && !strcmp(line, "ERROR")) {
        Log("<-%d ERROR (no subscription)", port + 1);
        ToClient(g_fallback);
        ToClient("OK");
        Finish();
        return;
    }
    if (g_cgmr && line[0] != '+' && !IsFinal(line)) {
        char reply[300];
        snprintf(reply, sizeof(reply), "+CGMR: %.280s", line);
        ToClient(reply);
        /* No final result follows, as with +CGSN. */
        ToClient("OK");
        g_swallowOkUntil = NowMs() + 2000;
        Finish();
        return;
    }
    ToClient(line);
    if (port == 0) {
        char name[64];
        if (sscanf(line, "+COPS: %*d,0,\"%63[^\"]\"", name) == 1)
            snprintf(g_opLong, sizeof(g_opLong), "%s", name);
        if (g_wantImsi && strspn(line, "0123456789") == strlen(line) && strlen(line) >= 6)
            snprintf(g_imsi, sizeof(g_imsi), "%s", line);
        if (g_wantIccid && sscanf(line, "+CRSM: 144,0,\"%63[0-9A-Fa-f]\"", name) == 1)
            snprintf(g_iccid, sizeof(g_iccid), "%s", name);
    }
    if (g_cpin && (!strncmp(line, "+CPIN:", 6) || !strncmp(line, "+CME ERROR:", 11)))
        g_lastSim = strstr(line, "READY") != NULL;
    if (g_imei && IsImei(line)) {
        if (port == 0)
            snprintf(g_imeiValue, sizeof(g_imeiValue), "%s", line);
        ToClient("OK");
        g_swallowOkUntil = NowMs() + 2000;
        Finish();
        return;
    }
    if (IsFinal(line))
        Finish();
}

static void ClientInput(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (g_pduPort >= 0) {
            WriteAll(g_ports[g_pduPort].fd, &c, 1);
            if (c == 0x1a || c == 0x1b) {
                g_pduPort = -1;
                Log("-> <pdu>");
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (g_clientLen == 0)
                continue;
            g_clientBuf[g_clientLen] = '\0';
            g_clientLen = 0;
            if (g_state == IDLE) {
                HandleCommand(g_clientBuf);
            } else if ((g_queueTail + 1) % 16 != g_queueHead) {
                snprintf(g_queue[g_queueTail], sizeof(g_queue[0]), "%s", g_clientBuf);
                g_queueTail = (g_queueTail + 1) % 16;
            } else {
                Log("queue full, dropped %s", g_clientBuf);
            }
            continue;
        }
        if (g_clientLen < sizeof(g_clientBuf) - 1)
            g_clientBuf[g_clientLen++] = c;
    }
}

static void ModemInput(int port, const char *data, size_t n)
{
    struct Port *p = &g_ports[port];
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\r' || c == '\n') {
            if (p->len > 0) {
                p->buf[p->len] = '\0';
                p->len = 0;
                ModemLine(port, p->buf);
            }
            continue;
        }
        if (p->len < sizeof(p->buf) - 1)
            p->buf[p->len++] = c;
        /* The SMS PDU prompt has no line end. */
        if (p->len == 2 && p->buf[0] == '>' && p->buf[1] == ' ') {
            p->len = 0;
            Log("<< > ");
            WriteAll(g_pty, "> ", 2);
            g_pduPort = port;
        }
    }
}

/* Sends cmd and waits up to 2 s for its final result, before the port joins the main loop. */
static int PortCommand(int fd, const char *cmd)
{
    char buf[512];
    size_t len = 0;
    long long end = NowMs() + 2000;
    WriteAll(fd, cmd, strlen(cmd));
    WriteAll(fd, "\r", 1);
    while (NowMs() < end) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0)
            continue;
        ssize_t r = read(fd, buf + len, sizeof(buf) - 1 - len);
        if (r <= 0)
            return -1;
        len += (size_t)r;
        buf[len] = '\0';
        if (strstr(buf, "OK\r") || strstr(buf, "OK\n"))
            return 0;
        if (strstr(buf, "ERROR"))
            return -1;
        if (len >= sizeof(buf) - 1)
            len = 0;
    }
    return -1;
}

static void OpenPort(int port)
{
    struct Port *p = &g_ports[port];
    p->fd = open(p->path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (p->fd < 0)
        return;
    p->len = 0;
    if (port == 0 && p->closed) {
        g_restoreCfun = g_cfun[0] != '\0';
        g_reapply = g_libraryUp;
    }
    PortCommand(p->fd, "ATE0");
    PortCommand(p->fd, "AT+CMEE=1"); /* numeric errors, as the library sets them (a restarted modem forgets) */
    p->bound = PortCommand(p->fd, p->bind) == 0;
    Log("slot %d: %s open, %s %s", port + 1, p->path, p->bind, p->bound ? "ok" : "failed (retried per command)");
    /* SIM state reports (the card becomes ready once asteroids_uim activates it, or is inserted or removed). */
    if (port == 0)
        Log("slot 1: SIM reports %s", PortCommand(p->fd, "AT$QCSIMSTAT=1") == 0 ? "on" : "failed");
}

static void ClosePort(int port, const char *why)
{
    Log("slot %d: %s closed: %s", port + 1, g_ports[port].path, why);
    close(g_ports[port].fd);
    g_ports[port].fd = -1;
    g_ports[port].bound = 0;
    g_ports[port].closed = 1;
    if (g_pduPort == port)
        g_pduPort = -1;
    if (g_state != IDLE && g_port == port) {
        if (!g_watch)
            ToClient("ERROR");
        Finish();
    }
}

static int MakePty(void)
{
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0 || grantpt(fd) < 0 || unlockpt(fd) < 0) {
        Log("pty: %s", strerror(errno));
        return -1;
    }
    char *name = ptsname(fd);
    if (name == NULL)
        return -1;
    /* The slave stays open here too, so the library closing and reopening it never gives us EIO; raw, as the
     * library sets only its default device raw. */
    int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0)
        return -1;
    struct termios ios;
    tcgetattr(slave, &ios);
    cfmakeraw(&ios);
    tcsetattr(slave, TCSANOW, &ios);
    chown(name, RADIO_UID, RADIO_GID);
    chmod(name, 0660);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.new", g_linkPath);
    unlink(tmp);
    if (symlink(name, tmp) < 0 || rename(tmp, g_linkPath) < 0) {
        Log("link %s: %s", g_linkPath, strerror(errno));
        return -1;
    }
    Log("%s -> %s", g_linkPath, name);
    return fd;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        g_ports[0].path = argv[1];
    if (argc > 2)
        g_linkPath = argv[2];
    if (argc > 3)
        g_ports[1].path = argv[3];
    g_log = fopen(LOG_PATH, "a");
    if (g_log != NULL)
        chmod(LOG_PATH, 0644);
    g_pty = MakePty();
    if (g_pty < 0)
        return 1;
    for (int i = 0; i < SLOTS; i++)
        OpenPort(i);
    DataInit();
    DataKillStale();

    char buf[4096];
    for (;;) {
        struct pollfd fds[1 + SLOTS + DATA_CALLS];
        int down = 0;
        fds[0] = (struct pollfd){ g_pty, POLLIN, 0 };
        for (int i = 0; i < SLOTS; i++) {
            fds[1 + i] = (struct pollfd){ g_ports[i].fd, POLLIN, 0 };
            down |= g_ports[i].fd < 0;
        }
        for (int i = 0; i < DATA_CALLS; i++)
            fds[1 + SLOTS + i] = (struct pollfd){ g_calls[i].fd, POLLIN, 0 };
        long long untilWatch = g_nextWatch - NowMs();
        int timeout = down ? 1000 : g_state != IDLE ? 500 : untilWatch < 0 ? 0 : untilWatch > WATCH_MS ? WATCH_MS :
            (int)untilWatch;
        int n = poll(fds, 1 + SLOTS + DATA_CALLS, timeout);
        if (n < 0 && errno != EINTR)
            return 1;

        for (int i = 0; i < SLOTS; i++) {
            if (g_ports[i].fd < 0)
                OpenPort(i);
        }
        if (g_state != IDLE && NowMs() > g_deadline) {
            Log(g_watch ? "watch: timeout" : "timeout");
            if (g_state == DATA && g_dataPending >= 0)
                DataStop(&g_calls[g_dataPending]);
            if (!g_watch)
                ToClient("ERROR");
            g_pduPort = -1;
            Finish();
        }
        if (g_restoreCfun && g_state == IDLE && g_queueHead == g_queueTail && g_pduPort < 0 && g_ports[0].fd >= 0 &&
            g_ports[0].bound) {
            g_restoreCfun = 0;
            Log("watch: radio power again after a modem restart (%s)", g_cfun);
            g_watch = 1;
            StartInternal(0, BuildNothing, 1, g_cfun);
        }
        /* The library's settings again after a card reset: SMS in PDU mode, delivered as +CMT with acknowledgement,
         * and registration reports; after a modem restart also numeric errors (+CMEE=1: the library reads the codes,
         * and the restarted modem answers in words). Quiet, like the watch. */
        if (g_reapply && g_state == IDLE && g_queueHead == g_queueTail && g_pduPort < 0 && g_ports[0].fd >= 0 &&
            g_ports[0].bound) {
            g_reapply = 0;
            Log("watch: library settings again");
            g_watch = 1;
            StartInternal(0, BuildNothing, 6, "AT+CMEE=1", "AT+CMGF=0", "AT+CSMS=1", "AT+CNMI=1,2,0,1,1",
                "AT+CREG=2", "AT+CGREG=2");
        }
        /* Stored messages, in the receive storage (ME) and back to the library's own (SM) after. */
        if (g_state == IDLE && g_queueHead == g_queueTail && g_pduPort < 0 && g_ports[0].fd >= 0 &&
            g_ports[0].bound && g_libraryUp) {
            static char del[32];
            if (g_smsIndex >= 0 && NowMs() > g_smsDeadline) {
                Log("sms: message %d not acknowledged", g_smsIndex);
                g_smsIndex = -1;
            }
            if (g_smsDelete >= 0) {
                snprintf(del, sizeof(del), "AT+CMGD=%d", g_smsDelete);
                Log("sms: deleting stored message %d", g_smsDelete);
                g_smsDelete = -1;
                g_smsWaiting = 0;
                g_watch = 1;
                StartInternal(0, BuildNothing, 3, "AT+CPMS=\"ME\"", del, "AT+CPMS=\"SM\"");
            } else if (g_smsWaiting > 0 && g_smsIndex < 0) {
                g_watch = 1;
                StartInternal(0, BuildSmsFetch, 3, "AT+CPMS=\"ME\"", "AT+CMGL=4", "AT+CPMS=\"SM\"");
            }
        }
        if (g_state == IDLE && g_queueHead == g_queueTail && g_pduPort < 0 && g_ports[0].fd >= 0 &&
            g_ports[0].bound && NowMs() >= g_nextWatch) {
            g_nextWatch = NowMs() + (g_lastCalls[0] != '\0' ? CALL_WATCH_MS : WATCH_MS);
            g_watch = 1;
            StartInternal(0, BuildWatch, 10, "AT+CPIN?", "AT+CREG?", "AT+CGREG?", "AT+CEREG?", "AT+COPS?",
                "AT$QCRSRP?", "AT$QCRSRQ?", "AT+CSQ", "AT+CLCC", "AT+CPMS?");
        }
        if (n <= 0)
            continue;
        for (int i = 0; i < DATA_CALLS; i++)
            if (g_calls[i].fd >= 0 && fds[1 + SLOTS + i].fd == g_calls[i].fd &&
                (fds[1 + SLOTS + i].revents & (POLLIN | POLLHUP | POLLERR)))
                DataInput(&g_calls[i]);
        if (fds[0].revents & POLLIN) {
            ssize_t r = read(g_pty, buf, sizeof(buf));
            if (r > 0)
                ClientInput(buf, (size_t)r);
        }
        for (int i = 0; i < SLOTS; i++) {
            if (fds[1 + i].fd < 0 || !(fds[1 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            /* The modem went down (a restart recreates the channels): ERROR until it is back. A channel that hung up
             * is closed without a read, which on a dead glink channel blocks for good. */
            if (fds[1 + i].revents & (POLLHUP | POLLERR)) {
                ClosePort(i, "hangup");
                continue;
            }
            ssize_t r = read(g_ports[i].fd, buf, sizeof(buf));
            if (r > 0)
                ModemInput(i, buf, (size_t)r);
            else if (r == 0 || (errno != EINTR && errno != EAGAIN))
                ClosePort(i, r == 0 ? "eof" : strerror(errno));
        }
    }
}
