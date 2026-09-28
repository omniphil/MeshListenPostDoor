/*
 * main.c -- the Meshtastic Listening Post, as a BBS door.
 *
 * A listening post for the public Meshtastic network: a live US map of the nodes
 * people's radios hear, what they are, and the public chat.
 *   TRACE  the door sends a module and the state outlines (once, cached);
 *          TERMinator draws a map you can zoom and pan, with keyboard and mouse.
 *   ANSI   the same map in 24-bit half blocks, a node list and the chat.
 *
 * The collector (lpcollect) that listens to the feed runs only while someone is
 * using the door, and for an hour after: opening the door starts it.
 *
 * The same start menu as the Weather and Satellite doors: detect quietly, then
 * offer TRACE or ANSI, remembering the caller's choice.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ansi_lp.h"
#include "door.h"
#include "lp.h"
#include "trace_door.h"

#define CSI "\033["
#define MODULE_ID "listenpost"

enum { CHOICE_TRACE = 1, CHOICE_ANSI = 2 };
enum { TERM_PLAIN = 0, TERM_TRACE_OLD, TERM_TRACE };

static LpUserPrefs g_prefs;

/* ------------------------------------------------------------- screens --- */

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;36m"
               "        =================================================\r\n"
               "        " CSI "1;32m" "M E S H T A S T I C" CSI "1;37m" "   L I S T E N I N G   P O S T\r\n"
               CSI "1;36m"
               "        =================================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "                     BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

static void write_terminator(void)
{
    door_write(CSI "1;35m" "TERM" CSI "1;36m" "inator" CSI "0m");
}

static void press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key to return to the BBS...\r\n" CSI "0m");
    door_read_char();
}

/* ------------------------------------------------------- the start menu --- */

static tdoor_blob_t g_module, g_states;

static void on_module_message(const unsigned char *data, size_t len);

/* Asks the terminal what it is: TERMinator answers in milliseconds, others
 * never, and the question gives up after half a second. TRACE is offered only
 * with mouse=1 (1.1.3+), since the module uses the mouse. */
static int detect_terminal(void)
{
    tdoor_init(MODULE_ID, on_module_message);
    bool have = tdoor_load_blob("lp.wasm", &g_module, 8 * 1024 * 1024) &&
                tdoor_load_blob("states.bin", &g_states, 4 * 1024 * 1024);
    if (!have) return TERM_PLAIN;
    static const char *const need[] = { "send=1", "assets=1", NULL };
    if (!tdoor_detect(need)) return TERM_PLAIN;
    return tdoor_has("mouse=1") ? TERM_TRACE : TERM_TRACE_OLD;
}

static void draw_menu(int term, int recommended)
{
    char line[32];
    title();
    door_write(CSI "0;37m  Choose how to listen:\r\n\r\n");
    if (term == TERM_TRACE)
        door_write(CSI "1;37m  [1] " CSI "1;32mTRACE" CSI "1;37m graphics   " CSI "0;37m(live map, mouse + keys) "
                   CSI "1;32mDETECTED\r\n");
    else if (term == TERM_TRACE_OLD)
        door_write(CSI "1;30m  [1] TRACE graphics   (live map, mouse + keys) " CSI "1;33mUPDATE " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;33m to 1.1.3+\r\n");
    else
        door_write(CSI "1;30m  [1] TRACE graphics   (live map, mouse + keys) NOT FOUND (needs " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;30m)\r\n");
    door_write(CSI "1;37m  [2] ANSI 24-bit      " CSI "0;37m(any modern terminal)    " CSI "1;32mSUPPORTED\r\n\r\n");
    snprintf(line, sizeof(line), "[%d]", recommended);
    door_write(CSI "0;37m  Enter = " CSI "1;37m");
    door_write(line);
    door_write(CSI "0;37m     Q = back to the BBS " CSI "0m");
}

static int choose_display(int term)
{
    bool trace = term == TERM_TRACE;
    int recommended = trace ? CHOICE_TRACE : CHOICE_ANSI;
    if (g_prefs.display == CHOICE_ANSI || (g_prefs.display == CHOICE_TRACE && trace))
        recommended = g_prefs.display;
    for (;;) {
        draw_menu(term, recommended);
        int c = door_read_char();
        if (c < 0 || c == 'q' || c == 'Q' || c == 27) return 0;
        if (c == '\r' || c == '\n') c = '0' + recommended;
        if (c == '1' && !trace) continue;
        if (c == '1' || c == '2') {
            g_prefs.display = c - '0';
            lp_save_prefs(&g_prefs);
            return c - '0';
        }
    }
}

/* ------------------------------------------------------------ TRACE link --- */

static struct { bool ready, region; int regionIndex; } g_req;

static void on_module_message(const unsigned char *data, size_t len)
{
    if (len < sizeof(LpMsgHeader)) return;
    LpMsgHeader h;
    memcpy(&h, data, sizeof h);
    const unsigned char *body = data + sizeof h;
    if (len - sizeof h < h.bytes) return;
    switch (h.type) {
    case LP_OUT_READY: g_req.ready = true; break;
    case LP_OUT_REGION:
        if (h.bytes >= 1) { g_req.regionIndex = body[0]; g_req.region = true; }
        break;
    default: break;
    }
}

static void send_packet(uint8_t type, uint8_t flags, uint16_t count, const void *payload, size_t len)
{
    unsigned char buf[sizeof(LpMsgHeader) + LP_MAX_PAYLOAD];
    if (len > LP_MAX_PAYLOAD) return;
    LpMsgHeader h = { type, flags, count, (uint32_t)len };
    memcpy(buf, &h, sizeof h);
    if (payload && len) memcpy(buf + sizeof h, payload, len);
    tdoor_send("msg", buf, sizeof h + len);
}

static void node_wire(const LpNode *n, LpNodeWire *w)
{
    memset(w, 0, sizeof *w);
    w->id = n->id;
    w->lat1e5 = n->lat1e5;
    w->lon1e5 = n->lon1e5;
    w->firstHeard = n->firstHeard;
    w->lastHeard = n->lastHeard;
    w->voltage_cV = n->voltage_cV;
    w->hw = n->hw;
    w->battery = n->battery;
    w->state = (uint8_t)lp_node_state(n);
    w->flags = n->flags;
    w->role = n->role;
    w->humidity = n->humidity;
    memcpy(w->name, n->name, LP_WIRE_NAME - 1);
    memcpy(w->shortName, n->shortName, sizeof w->shortName - 1);
    w->packets = n->packets;
    w->temp_dC = (n->flags & LP_NODE_ENV) ? n->temp_dC : LP_NO_TEMP;
    w->links = 0;                          /* the module counts them from the links it gets */
}

/* Links by index list (or all of them), in packets. Only the last day's. */
static void send_links(const uint32_t *idx, uint32_t n, bool full)
{
    enum { PER = LP_MAX_PAYLOAD / sizeof(LpLinkWire) };
    LpLinkWire w[PER];
    uint32_t k = 0, now = (uint32_t)time(NULL);
    bool first = full;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || k == PER) {
            if (k || first) send_packet(LP_IN_LINKS, first ? 1 : 0, (uint16_t)k, w, k * sizeof *w);
            first = false;
            k = 0;
            if (i == n) break;
        }
        const LpLink *l = &lp_links[idx ? idx[i] : i];
        if (now - l->lastHeard > 86400) continue;
        LpLinkWire *o = &w[k++];
        memset(o, 0, sizeof *o);
        o->a = l->a;
        o->b = l->b;
        o->lastHeard = l->lastHeard;
        o->snr_q = l->snr_q;
        o->source = l->source;
    }
}

static void send_history(void)
{
    unsigned char buf[sizeof(LpHistHead) + LP_HIST_MIN * 2];
    LpHistHead h = { lp_histEnd, (uint16_t)lp_histCount, 0 };
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, lp_hist, lp_histCount * 2);
    send_packet(LP_IN_HISTORY, 0, 1, buf, sizeof h + lp_histCount * 2);
}

/* Nodes by index list (or all of them when idx is NULL), in packets. */
static void send_nodes(const uint32_t *idx, uint32_t n, bool full)
{
    enum { PER = LP_MAX_PAYLOAD / sizeof(LpNodeWire) };
    LpNodeWire w[PER];
    uint32_t k = 0;
    bool first = full;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || k == PER) {
            if (k || first) send_packet(LP_IN_NODES, first ? 1 : 0, (uint16_t)k, w, k * sizeof *w);
            first = false;
            k = 0;
            if (i == n) break;
        }
        node_wire(&lp_nodes[idx ? idx[i] : i], &w[k++]);
    }
    if (full) send_packet(LP_IN_DONE, 0, 0, NULL, 0);
}

static void send_chat(void)
{
    unsigned char buf[LP_MAX_PAYLOAD];
    size_t used = 0;
    uint16_t count = 0;
    bool first = true;
    for (uint32_t i = 0; i <= lp_nChat; i++) {
        size_t need = 0;
        const LpChat *c = i < lp_nChat ? &lp_chat[i] : NULL;
        char name[LP_NAME_LEN] = "";
        if (c) {
            lp_label(lp_find(c->from), c->from, name, sizeof name);
            need = sizeof(LpChatWire) + strlen(name) + strlen(c->channel) + strlen(c->text);
        }
        if (!c || used + need > LP_MAX_PAYLOAD) {
            send_packet(LP_IN_CHAT, first ? 1 : 0, count, buf, used);
            first = false;
            used = 0;
            count = 0;
            if (!c) break;
        }
        LpChatWire w = { c->time, c->state, (uint8_t)strlen(name), (uint8_t)strlen(c->text), (uint8_t)strlen(c->channel) };
        memcpy(buf + used, &w, sizeof w);
        used += sizeof w;
        memcpy(buf + used, name, w.nameLen); used += w.nameLen;
        memcpy(buf + used, c->channel, w.chanLen); used += w.chanLen;
        memcpy(buf + used, c->text, w.textLen); used += w.textLen;
        count++;
    }
}

static void send_stats(void)
{
    LpStats s;
    memset(&s, 0, sizeof s);
    s.doorNow = (uint32_t)time(NULL);
    s.feed = (uint8_t)lp_feed_state();
    s.startedAt = s.feed == LP_FEED_STOPPED ? 0 : lp_head.startedAt;
    s.lastPacketAt = lp_head.lastPacketAt;
    s.snapshotAt = lp_head.savedAt;
    s.pktPerMin = s.feed == LP_FEED_LIVE ? lp_head.pktPerMin : 0;
    s.nodesTotal = lp_nNodes;
    s.firstEver = lp_firstEver ? 1 : 0;
    send_packet(LP_IN_STATS, 0, 1, &s, sizeof s);
}

static void send_hello(void)
{
    LpHello h;
    memset(&h, 0, sizeof h);
    h.doorNow = (uint32_t)time(NULL);
    h.region = (uint8_t)g_prefs.region;
    h.nodeDays = (uint8_t)lp_config.nodeDays;
    if (g_prefs.haveHome) {
        h.haveHome = 1;
        h.homeState = (uint8_t)g_prefs.homeState;
        h.homeLat1e5 = (int32_t)lround(g_prefs.homeLat * 1e5);
        h.homeLon1e5 = (int32_t)lround(g_prefs.homeLon * 1e5);
        snprintf(h.homeName, sizeof h.homeName, "%.39s", g_prefs.homeName);
    }
    send_packet(LP_IN_HELLO, 0, 1, &h, sizeof h);
}

static void upload_progress(int percent)
{
    char buf[96];
    snprintf(buf, sizeof buf, "\r" CSI "0;36m  Sending the map (first visit only)... %3d%%" CSI "0m", percent);
    door_write(buf);
}

static int play_trace(void)
{
    title();
    door_write(CSI "1;32m  ");
    write_terminator();
    door_write(CSI "1;32m found. Opening the listening post...\r\n\r\n" CSI "0m");
    if (!tdoor_send_asset(&g_states, upload_progress) || !tdoor_open(&g_module, "exclusive=1", NULL)) {
        door_write(CSI "1;33m\r\n  The listening post couldn't be started on your terminal.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    cls();
    char buf[96];
    snprintf(buf, sizeof buf, "states=%s", g_states.hash);
    tdoor_send_text(buf);
    tdoor_send_text("start");

    memset(&g_req, 0, sizeof g_req);
    bool started = false;
    time_t lastPoll = 0, lastStats = 0;
    for (;;) {
        int reply = tdoor_wait_reply(200);
        if (reply == TDOOR_REPLY_CLOSED || !tdoor_is_open()) break;
        if (g_req.ready) {
            g_req.ready = false;
            send_hello();
            send_nodes(NULL, lp_nNodes, true);
            send_links(NULL, lp_nLinks, true);
            send_chat();
            send_history();
            send_stats();
            started = true;
            lastPoll = lastStats = time(NULL);
        }
        if (g_req.region) {
            g_req.region = false;
            if (g_req.regionIndex >= 0 && (!lp_haveGeo || g_req.regionIndex <= lp_geo.count)) {
                g_prefs.region = g_req.regionIndex;
                lp_save_prefs(&g_prefs);
            }
        }
        time_t now = time(NULL);
        lp_touch();
        if (started && now - lastPoll >= 2) {
            lastPoll = now;
            LpChanges c = lp_refresh();
            if (c.nodes) send_nodes(lp_changed, c.nodes, false);
            if (c.links) send_links(lp_changedLinks, c.links, false);
            if (c.chat) send_chat();
            if (c.hist) send_history();
            if (c.nodes || c.chat || now - lastStats >= 5) { send_stats(); lastStats = now; }
        }
        if (door_time_remaining() <= 0) break;
    }
    tdoor_close("quit", 2000);

    if (tdoor_last_close_code() != 0 || tdoor_last_close_reason()[0]) {
        title();
        door_write(CSI "1;31m  The picture stopped with an error.\r\n" CSI "0m");
        const char *why = tdoor_last_close_reason();
        char line[200];
        snprintf(line, sizeof line, CSI "1;33m  Reason: %s\r\n" CSI "0m", (why && *why) ? why : "(none given)");
        door_write(line);
        snprintf(line, sizeof line, CSI "1;30m  Your TERMinator offers: %.100s\r\n" CSI "0m", tdoor_info());
        door_write(line);
        door_write(CSI "0;37m  Please send those two lines to the sysop, or choose ANSI next time.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    cls();
    return 0;
}

static void on_signal(int sig)
{
    lp_leave();
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char *argv[])
{
    door_init(argc > 1 ? argv[1] : NULL);
    lp_load_states();
    lp_load_config();
    lp_load_prefs(&g_prefs);
    signal(SIGHUP, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, on_signal);

    title();
    lp_arrive();                           /* starts the collector now, so it warms up during the menu */
    lp_load_snapshot();
    int term = detect_terminal();
    int choice = choose_display(term);
    int status = 0;
    if (choice == CHOICE_TRACE) status = play_trace();
    else if (choice == CHOICE_ANSI) ansi_lp_run(&g_prefs);

    lp_leave();
    door_cleanup();
    return status;
}
