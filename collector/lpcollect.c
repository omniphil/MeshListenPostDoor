/*
 * lpcollect.c - the Listening Post's collector: listens to the public
 * Meshtastic MQTT feed while people are using the door, and keeps what's worth
 * showing in data/snapshot.bin and data/live.bin (see common/lpdata.h).
 *
 *   lpcollect [dir]      dir = where lp.ini, states.bin and data/ live
 *                        (default: beside this binary)
 *
 * The door starts it; nobody else needs to. It runs while any door has touched
 * its "I'm here" file (data/doors/<pid>) in the last two minutes, then keeps
 * going for linger_minutes after the last one leaves, then saves and exits. A
 * second copy started while one runs sees the lock and leaves at once.
 *
 * What it keeps (and nothing else):
 *   - per node: its long and short names, hardware, battery, when it was first
 *     and last heard, and its position rounded to a 0.02 degree grid (~2 km);
 *   - broadcast text on channels it can read (the default key), last chat_lines;
 *   - sensor readings (temperature, humidity) from nodes that send them;
 *   - links: which radios hear each other directly, with the signal strength,
 *     for the last day. Mostly from the gateways themselves: a packet a gateway
 *     uplinks with no hops used was heard straight off the air from its sender.
 *     Neighbour reports and traceroute answers add more when they're readable;
 *   - counters: unique packets per minute, and three hours of them per minute.
 * Routing, traceroutes and the same packet arriving from other gateways are
 * dropped on arrival. Private channels and direct messages
 * can't be read and are only counted as "heard".
 *
 * No libraries beyond libc: MQTT 3.1.1, protobuf and AES-128-CTR are done here.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aes.h"
#include "geo.h"
#include "lpdata.h"

/* ------------------------------------------------------------- config --- */

static struct {
    char host[128], topic[128], user[64], pass[64];
    int  port, lingerMinutes, chatLines, nodeDays;
} cfg;

static char g_dir[1024], g_data[1100];

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void load_config(void)
{
    snprintf(cfg.host, sizeof cfg.host, "mqtt.meshtastic.org");
    snprintf(cfg.topic, sizeof cfg.topic, "msh/US/#");
    snprintf(cfg.user, sizeof cfg.user, "meshdev");
    snprintf(cfg.pass, sizeof cfg.pass, "large4cats");
    cfg.port = 1883;
    cfg.lingerMinutes = 15;
    cfg.chatLines = 200;
    cfg.nodeDays = 7;
    char path[1200], line[512];
    snprintf(path, sizeof path, "%s/lp.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, "linger_minutes")) cfg.lingerMinutes = atoi(v);
        else if (!strcmp(k, "chat_lines")) cfg.chatLines = atoi(v);
        else if (!strcmp(k, "node_days")) cfg.nodeDays = atoi(v);
        else if (!strcmp(k, "mqtt_host")) snprintf(cfg.host, sizeof cfg.host, "%s", v);
        else if (!strcmp(k, "mqtt_port")) cfg.port = atoi(v);
        else if (!strcmp(k, "mqtt_topic")) snprintf(cfg.topic, sizeof cfg.topic, "%s", v);
        else if (!strcmp(k, "mqtt_user")) snprintf(cfg.user, sizeof cfg.user, "%s", v);
        else if (!strcmp(k, "mqtt_pass")) snprintf(cfg.pass, sizeof cfg.pass, "%s", v);
    }
    fclose(f);
    if (cfg.lingerMinutes < 1) cfg.lingerMinutes = 1;
    if (cfg.chatLines < 10) cfg.chatLines = 10;
    if (cfg.chatLines > 2000) cfg.chatLines = 2000;
    if (cfg.nodeDays < 1) cfg.nodeDays = 1;
    if (cfg.nodeDays > 60) cfg.nodeDays = 60;
}

/* ---------------------------------------------------------------- log --- */

static void logf_(const char *fmt, ...)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/collector.log", g_data);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 256 * 1024) {
        char old[1300];
        snprintf(old, sizeof old, "%s.old", path);
        rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s [%d] ", ts, (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* -------------------------------------------------------------- state --- */

static GeoMap   g_geo;
static int      g_haveGeo = 0;

static LpNode  *g_nodes;
static uint32_t g_nNodes, g_capNodes;
static int32_t *g_hash;                    // node index by id, open addressing
static uint32_t g_hashCap;

static LpChat  *g_chat;                    // ring, oldest first when written
static int      g_chatN, g_chatHead;

static LpHead   g_head;
static uint32_t g_perSec[60];              // unique packets, by second of the minute
static uint32_t g_perSecAt[60];
static uint16_t g_hist[LP_HIST_MIN];       // unique packets per minute, ring by unix minute
static uint32_t g_histMin = 0;             // the unix minute g_hist was last advanced to

static LpLink  *g_links;
static uint32_t g_nLinks, g_capLinks;
static int32_t *g_linkHash;
static uint32_t g_linkHashCap;
static volatile sig_atomic_t g_stop = 0;

static uint32_t hash_id(uint32_t id) { id ^= id >> 16; id *= 0x7feb352d; id ^= id >> 15; id *= 0x846ca68b; id ^= id >> 16; return id; }

static void rehash(void)
{
    uint32_t cap = g_hashCap ? g_hashCap * 2 : 8192;
    while (cap < g_capNodes * 2) cap *= 2;
    int32_t *h = malloc(sizeof *h * cap);
    if (!h) return;
    for (uint32_t i = 0; i < cap; i++) h[i] = -1;
    for (uint32_t i = 0; i < g_nNodes; i++) {
        uint32_t k = hash_id(g_nodes[i].id) & (cap - 1);
        while (h[k] >= 0) k = (k + 1) & (cap - 1);
        h[k] = (int32_t)i;
    }
    free(g_hash);
    g_hash = h;
    g_hashCap = cap;
}

static LpNode *find_node(uint32_t id, int create)
{
    if (g_hashCap) {
        uint32_t k = hash_id(id) & (g_hashCap - 1);
        while (g_hash[k] >= 0) {
            if (g_nodes[g_hash[k]].id == id) return &g_nodes[g_hash[k]];
            k = (k + 1) & (g_hashCap - 1);
        }
    }
    if (!create) return NULL;
    if (g_nNodes == g_capNodes) {
        uint32_t cap = g_capNodes ? g_capNodes * 2 : 4096;
        LpNode *n = realloc(g_nodes, sizeof *n * cap);
        if (!n) return NULL;
        g_nodes = n;
        g_capNodes = cap;
    }
    if ((g_nNodes + 1) * 2 > g_hashCap) rehash();
    if (!g_hashCap) return NULL;
    LpNode *n = &g_nodes[g_nNodes];
    memset(n, 0, sizeof *n);
    n->id = id;
    n->battery = 255;
    n->temp_dC = LP_NO_TEMP;
    n->humidity = 255;
    uint32_t k = hash_id(id) & (g_hashCap - 1);
    while (g_hash[k] >= 0) k = (k + 1) & (g_hashCap - 1);
    g_hash[k] = (int32_t)g_nNodes++;
    return n;
}

// Drops nodes not heard for node_days (done before each full save).
static void expire_nodes(uint32_t now)
{
    uint32_t cut = now - (uint32_t)cfg.nodeDays * 86400u, w = 0;
    for (uint32_t i = 0; i < g_nNodes; i++)
        if (g_nodes[i].lastHeard >= cut) g_nodes[w++] = g_nodes[i];
    if (w != g_nNodes) {
        g_nNodes = w;
        g_hashCap = 0;
        free(g_hash);
        g_hash = NULL;
        rehash();
    }
}

/* Links: two nodes that hear each other. */
static uint32_t link_hash(uint32_t a, uint32_t b) { return hash_id(a * 2654435761u ^ hash_id(b)); }

static void link_rehash(void)
{
    uint32_t cap = 8192;
    while (cap < g_nLinks * 2 + 16) cap *= 2;
    int32_t *h = malloc(sizeof *h * cap);
    if (!h) return;
    for (uint32_t i = 0; i < cap; i++) h[i] = -1;
    for (uint32_t i = 0; i < g_nLinks; i++) {
        uint32_t k = link_hash(g_links[i].a, g_links[i].b) & (cap - 1);
        while (h[k] >= 0) k = (k + 1) & (cap - 1);
        h[k] = (int32_t)i;
    }
    free(g_linkHash);
    g_linkHash = h;
    g_linkHashCap = cap;
}

static void note_link(uint32_t a, uint32_t b, int32_t snr_q, uint8_t source, uint32_t now)
{
    if (!a || !b || a == b || a == 0xFFFFFFFFu || b == 0xFFFFFFFFu) return;
    if (a > b) { uint32_t t = a; a = b; b = t; }
    if (!g_linkHashCap) link_rehash();
    if (!g_linkHashCap) return;
    uint32_t k = link_hash(a, b) & (g_linkHashCap - 1);
    while (g_linkHash[k] >= 0) {
        LpLink *l = &g_links[g_linkHash[k]];
        if (l->a == a && l->b == b) {
            l->lastHeard = now;
            l->snr_q = (int16_t)snr_q;
            l->source = source;
            return;
        }
        k = (k + 1) & (g_linkHashCap - 1);
    }
    if (g_nLinks >= 200000) return;
    if (g_nLinks == g_capLinks) {
        uint32_t cap = g_capLinks ? g_capLinks * 2 : 4096;
        LpLink *nl = realloc(g_links, sizeof *nl * cap);
        if (!nl) return;
        g_links = nl;
        g_capLinks = cap;
    }
    LpLink *l = &g_links[g_nLinks];
    memset(l, 0, sizeof *l);
    l->a = a;
    l->b = b;
    l->lastHeard = now;
    l->snr_q = (int16_t)snr_q;
    l->source = source;
    g_linkHash[k] = (int32_t)g_nLinks++;
    if (g_nLinks * 2 > g_linkHashCap) link_rehash();
}

// Links not heard for a day are dropped, and those whose nodes are gone.
static void expire_links(uint32_t now)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < g_nLinks; i++)
        if (now - g_links[i].lastHeard <= 86400) g_links[w++] = g_links[i];
    if (w != g_nLinks) { g_nLinks = w; link_rehash(); }
}

/* The per-minute history: moves the ring up to this minute, zeroing any skipped. */
static void hist_advance(uint32_t now)
{
    uint32_t m = now / 60;
    if (!g_histMin || m - g_histMin >= LP_HIST_MIN) {
        memset(g_hist, 0, sizeof g_hist);
    } else {
        for (uint32_t k = g_histMin + 1; k <= m; k++) g_hist[k % LP_HIST_MIN] = 0;
    }
    if (m > g_histMin) g_histMin = m;
}

/* Recently seen packets, so a packet uplinked by twenty gateways counts once. */
#define SEEN_CAP 65536
static struct { uint64_t key; uint32_t at; } g_seen[SEEN_CAP];

static int seen_before(uint32_t from, uint32_t id, uint32_t now)
{
    uint64_t key = (uint64_t)from << 32 | id;
    uint32_t k = hash_id(from ^ (id * 2654435761u)) & (SEEN_CAP - 1);
    for (int probe = 0; probe < 32; probe++, k = (k + 1) & (SEEN_CAP - 1)) {
        if (g_seen[k].at && now - g_seen[k].at < 900 && g_seen[k].key == key) return 1;
        if (!g_seen[k].at || now - g_seen[k].at >= 900) {
            g_seen[k].key = key;
            g_seen[k].at = now;
            return 0;
        }
    }
    // A busy neighbourhood: overwrite the first slot rather than grow.
    k = hash_id(from ^ (id * 2654435761u)) & (SEEN_CAP - 1);
    g_seen[k].key = key;
    g_seen[k].at = now;
    return 0;
}

/* -------------------------------------------------------------- text --- */

// UTF-8 -> Latin-1, printable only: emoji and anything past U+00FF are dropped,
// control characters and newlines become spaces, runs of spaces become one.
static void to_latin1(const uint8_t *s, size_t n, char *out, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < size;) {
        uint32_t c = s[i], len = 1;
        if (c >= 0xF0 && i + 3 < n) { c = (c & 7) << 18 | (s[i + 1] & 63u) << 12 | (s[i + 2] & 63u) << 6 | (s[i + 3] & 63u); len = 4; }
        else if (c >= 0xE0 && i + 2 < n) { c = (c & 15) << 12 | (s[i + 1] & 63u) << 6 | (s[i + 2] & 63u); len = 3; }
        else if (c >= 0xC0 && i + 1 < n) { c = (c & 31) << 6 | (s[i + 1] & 63u); len = 2; }
        else if (c >= 0x80) c = '?';
        i += len;
        if (c == 0x2019 || c == 0x2018) c = '\'';
        else if (c == 0x201C || c == 0x201D) c = '"';
        else if (c == 0x2013 || c == 0x2014) c = '-';
        else if (c == 0x2026) { if (o + 4 < size) { memcpy(out + o, "...", 3); o += 3; } continue; }
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) c = ' ';
        if (c > 0xFF) continue;
        if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = (char)c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ----------------------------------------------------------- protobuf --- */

typedef struct { const uint8_t *p, *end; } Pb;

static int pb_varint(Pb *b, uint64_t *v)
{
    *v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (b->p >= b->end) return 0;
        uint8_t c = *b->p++;
        *v |= (uint64_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) return 1;
    }
    return 0;
}

// The next field: its number, and either a number (v) or bytes (ptr, len).
// Returns 1, 0 at the end, -1 on garbage.
static int pb_next(Pb *b, uint32_t *field, uint64_t *v, const uint8_t **ptr, size_t *len)
{
    if (b->p >= b->end) return 0;
    uint64_t tag;
    if (!pb_varint(b, &tag)) return -1;
    *field = (uint32_t)(tag >> 3);
    *ptr = NULL;
    *len = 0;
    *v = 0;
    switch (tag & 7) {
    case 0: return pb_varint(b, v) ? 1 : -1;
    case 1: if (b->end - b->p < 8) return -1; memcpy(v, b->p, 8); b->p += 8; return 1;
    case 5: { if (b->end - b->p < 4) return -1; uint32_t w; memcpy(&w, b->p, 4); *v = w; b->p += 4; return 1; }
    case 2: {
        uint64_t n;
        if (!pb_varint(b, &n) || n > (uint64_t)(b->end - b->p)) return -1;
        *ptr = b->p;
        *len = (size_t)n;
        b->p += n;
        return 1;
    }
    default: return -1;
    }
}

/* ------------------------------------------------------------ packets --- */

static Aes128 g_aes;
static const uint8_t DEFAULT_KEY[16] = { 0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                         0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01 };

enum { PORT_TEXT = 1, PORT_POSITION = 3, PORT_NODEINFO = 4, PORT_TELEMETRY = 67, PORT_TRACEROUTE = 70,
       PORT_NEIGHBORS = 71, PORT_MAPREPORT = 73 };

static int32_t blur(int64_t deg1e7)
{
    double d = deg1e7 / 1e7;
    return (int32_t)lround(round(d / 0.02) * 0.02 * 1e5);
}

static void set_position(LpNode *n, int64_t lat1e7, int64_t lon1e7, uint32_t now)
{
    if ((lat1e7 == 0 && lon1e7 == 0) || lat1e7 > 900000000 || lat1e7 < -900000000 ||
        lon1e7 > 1800000000 || lon1e7 < -1800000000) return;
    n->lat1e5 = blur(lat1e7);
    n->lon1e5 = blur(lon1e7);
    n->flags |= LP_NODE_POS;
    n->lastPos = now;
    if (g_haveGeo) {
        int st = geo_state_at(&g_geo, n->lat1e5 / 1e5f, n->lon1e5 / 1e5f);
        n->state = (uint8_t)(st >= 0 ? st + 1 : 0);
    }
}

static void set_name(LpNode *n, const uint8_t *p, size_t len)
{
    char name[LP_NAME_LEN];
    to_latin1(p, len, name, sizeof name);
    if (name[0]) {
        memcpy(n->name, name, sizeof name);
        n->flags |= LP_NODE_NAME;
    }
}

static void set_short(LpNode *n, const uint8_t *p, size_t len)
{
    char s[sizeof n->shortName];
    to_latin1(p, len, s, sizeof s);
    if (s[0]) memcpy(n->shortName, s, sizeof s);
}

static void on_position(LpNode *n, const uint8_t *p, size_t len, uint32_t now)
{
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    int64_t lat = 0, lon = 0;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 1) lat = (int32_t)(uint32_t)v;
        else if (f == 2) lon = (int32_t)(uint32_t)v;
    }
    set_position(n, lat, lon, now);
}

static void on_user(LpNode *n, const uint8_t *p, size_t len)
{
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 2 && q) set_name(n, q, ql);
        else if (f == 3 && q) set_short(n, q, ql);
        else if (f == 5) n->hw = (uint8_t)(v > 255 ? 255 : v);
        else if (f == 7) n->role = (uint8_t)v;
    }
}

static float pb_float(uint64_t v)
{
    float x;
    uint32_t w = (uint32_t)v;
    memcpy(&x, &w, 4);
    return x;
}

static void on_telemetry(LpNode *n, const uint8_t *p, size_t len, uint32_t now)
{
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 3 && q) {                                // environment_metrics: a sensor
            Pb e = { q, q + ql };
            int got = 0;
            while (pb_next(&e, &f, &v, &q, &ql) == 1) {
                float x = pb_float(v);
                if (f == 1 && x > -60 && x < 70 && x != 0) { n->temp_dC = (int16_t)lroundf(x * 10); got = 1; }
                else if (f == 2 && x > 0 && x <= 100) { n->humidity = (uint8_t)lroundf(x); got = 1; }
            }
            if (got) { n->flags |= LP_NODE_ENV; n->envAt = now; }
            continue;
        }
        if (f != 2 || !q) continue;                       // device_metrics
        Pb d = { q, q + ql };
        while (pb_next(&d, &f, &v, &q, &ql) == 1) {
            if (f == 1) { n->battery = (uint8_t)(v > 101 ? 101 : v); n->flags |= LP_NODE_TELEM; }
            else if (f == 2) {
                float volts;
                uint32_t w = (uint32_t)v;
                memcpy(&volts, &w, 4);
                if (volts > 0 && volts < 60) n->voltage_cV = (uint16_t)lroundf(volts * 100);
            }
        }
    }
}

static void on_map_report(LpNode *n, const uint8_t *p, size_t len, uint32_t now)
{
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    int64_t lat = 0, lon = 0;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 1 && q) set_name(n, q, ql);
        else if (f == 2 && q) set_short(n, q, ql);
        else if (f == 3) n->role = (uint8_t)v;
        else if (f == 4) n->hw = (uint8_t)(v > 255 ? 255 : v);
        else if (f == 9) lat = (int32_t)(uint32_t)v;
        else if (f == 10) lon = (int32_t)(uint32_t)v;
    }
    set_position(n, lat, lon, now);
}

// NeighborInfo: the nodes this one hears directly, with the signal from each.
static void on_neighbors(uint32_t from, const uint8_t *p, size_t len, uint32_t now)
{
    Pb b = { p, p + len };
    uint32_t f, reporter = from;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 1 && v) reporter = (uint32_t)v;
        else if (f == 4 && q) {
            Pb nb = { q, q + ql };
            uint32_t id = 0;
            float snr = 0;
            const uint8_t *q2;
            size_t ql2;
            while (pb_next(&nb, &f, &v, &q2, &ql2) == 1) {
                if (f == 1) id = (uint32_t)v;
                else if (f == 2) snr = pb_float(v);
            }
            if (id) note_link(reporter, id, (int32_t)lroundf(snr * 4), LP_LINK_NEIGHBOR, now);
        }
    }
}

// Reads a repeated field that may be packed (one length-delimited run) or not.
// fixed = 1 for fixed32 (node numbers), 0 for varints (signal, quarter dB).
static int pb_repeated(const uint8_t *q, size_t ql, uint64_t v, int fixed, int64_t *out, int n, int max)
{
    if (!q) { if (n < max) out[n++] = fixed ? (int64_t)(uint32_t)v : (int64_t)(int32_t)(int64_t)v; return n; }
    Pb r = { q, q + ql };
    while (r.p < r.end && n < max) {
        if (fixed) {
            if (r.end - r.p < 4) break;
            uint32_t w;
            memcpy(&w, r.p, 4);
            r.p += 4;
            out[n++] = w;
        } else {
            uint64_t x;
            if (!pb_varint(&r, &x)) break;
            out[n++] = (int32_t)(int64_t)x;
        }
    }
    return n;
}

// A traceroute's answer: the path out (origin -> hops -> destination) and the
// path back, each hop with the signal it was heard at.
static void on_traceroute(uint32_t from, uint32_t to, const uint8_t *p, size_t len, uint32_t now)
{
    int64_t route[16], snrT[17], back[16], snrB[17];
    int nr = 0, nt = 0, nb = 0, nsb = 0;
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 1) nr = pb_repeated(q, ql, v, 1, route, nr, 16);
        else if (f == 2) nt = pb_repeated(q, ql, v, 0, snrT, nt, 17);
        else if (f == 3) nb = pb_repeated(q, ql, v, 1, back, nb, 16);
        else if (f == 4) nsb = pb_repeated(q, ql, v, 0, snrB, nsb, 17);
    }
    if (to == 0xFFFFFFFFu) return;
    // Out: to (who asked) ... from (who answered).
    uint32_t chain[18];
    int n = 0;
    chain[n++] = to;
    for (int i = 0; i < nr; i++) chain[n++] = (uint32_t)route[i];
    chain[n++] = from;
    for (int i = 0; i + 1 < n; i++)
        note_link(chain[i], chain[i + 1], i < nt && snrT[i] != -128 ? (int32_t)snrT[i] : 0, LP_LINK_TRACE, now);
    if (!nsb && !nb) return;
    n = 0;
    chain[n++] = from;
    for (int i = 0; i < nb; i++) chain[n++] = (uint32_t)back[i];
    chain[n++] = to;
    for (int i = 0; i + 1 < n; i++)
        note_link(chain[i], chain[i + 1], i < nsb && snrB[i] != -128 ? (int32_t)snrB[i] : 0, LP_LINK_TRACE, now);
}

static void on_text(LpNode *n, uint32_t from, const char *channel, const uint8_t *p, size_t len, uint32_t now)
{
    char text[LP_TEXT_LEN];
    to_latin1(p, len, text, sizeof text);
    if (!text[0]) return;
    LpChat *c = &g_chat[(g_chatHead + g_chatN) % cfg.chatLines];
    if (g_chatN == cfg.chatLines) g_chatHead = (g_chatHead + 1) % cfg.chatLines;
    else g_chatN++;
    memset(c, 0, sizeof *c);
    c->time = now;
    c->from = from;
    snprintf(c->channel, sizeof c->channel, "%s", channel);
    memcpy(c->text, text, sizeof text);
    c->state = n->state ? n->state : n->topicState;
}

// One Data message (decoded or decrypted) from a node.
static void on_data(LpNode *n, uint32_t from, uint32_t to, const char *channel, const uint8_t *p, size_t len, uint32_t now)
{
    Pb b = { p, p + len };
    uint32_t f, port = 0, requestId = 0;
    uint64_t v;
    const uint8_t *q, *payload = NULL;
    size_t ql, plen = 0;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 1) port = (uint32_t)v;
        else if (f == 2) { payload = q; plen = ql; }
        else if (f == 6) requestId = (uint32_t)v;
    }
    if (!payload) return;
    switch (port) {
    case PORT_POSITION:  on_position(n, payload, plen, now); break;
    case PORT_NODEINFO:  on_user(n, payload, plen); break;
    case PORT_TELEMETRY: on_telemetry(n, payload, plen, now); break;
    case PORT_NEIGHBORS: on_neighbors(from, payload, plen, now); break;
    case PORT_TRACEROUTE: if (requestId) on_traceroute(from, to, payload, plen, now); break;
    case PORT_MAPREPORT: on_map_report(n, payload, plen, now); break;
    case PORT_TEXT:      if (to == 0xFFFFFFFFu) on_text(n, from, channel, payload, plen, now); break;
    default: break;
    }
}

// A Data message is believable if it parses and names a port.
static int data_ok(const uint8_t *p, size_t len)
{
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q;
    size_t ql;
    int r, port = 0;
    while ((r = pb_next(&b, &f, &v, &q, &ql)) == 1)
        if (f == 1) port = (int)v;
    return r == 0 && port > 0 && port < 1024;
}

// One MQTT message: a ServiceEnvelope on topic.
static void on_envelope(const char *topic, const uint8_t *p, size_t len)
{
    if (strstr(topic, "/json/") || strstr(topic, "/stat/")) return;
    uint32_t now = (uint32_t)time(NULL);
    Pb b = { p, p + len };
    uint32_t f;
    uint64_t v;
    const uint8_t *q, *pkt = NULL;
    size_t ql, pktLen = 0;
    char channel[LP_CHAN_LEN] = "";
    uint32_t gateway = 0;
    while (pb_next(&b, &f, &v, &q, &ql) == 1) {
        if (f == 1) { pkt = q; pktLen = ql; }
        else if (f == 2 && q) { size_t n = ql < LP_CHAN_LEN - 1 ? ql : LP_CHAN_LEN - 1; memcpy(channel, q, n); channel[n] = 0; }
        else if (f == 3 && q && ql == 9 && q[0] == '!') {          // gateway id, "!1a2b3c4d"
            char hex[9];
            memcpy(hex, q + 1, 8);
            hex[8] = 0;
            gateway = (uint32_t)strtoul(hex, NULL, 16);
        }
    }
    if (!pkt) return;

    uint32_t from = 0, to = 0, id = 0, hopLimit = 0, hopStart = 0, viaMqtt = 0;
    float rxSnr = 0;
    const uint8_t *decoded = NULL, *enc = NULL;
    size_t decLen = 0, encLen = 0;
    Pb m = { pkt, pkt + pktLen };
    while (pb_next(&m, &f, &v, &q, &ql) == 1) {
        if (f == 1) from = (uint32_t)v;
        else if (f == 2) to = (uint32_t)v;
        else if (f == 4) { decoded = q; decLen = ql; }
        else if (f == 5) { enc = q; encLen = ql; }
        else if (f == 6) id = (uint32_t)v;
        else if (f == 8) rxSnr = pb_float(v);
        else if (f == 9) hopLimit = (uint32_t)v;
        else if (f == 14) viaMqtt = (uint32_t)v;
        else if (f == 15) hopStart = (uint32_t)v;
    }
    // Empty stubs (some gateways uplink packets with no payload at all) and
    // anything without a sender are noise.
    if (!from || (!decoded && encLen == 0)) return;
    // Heard straight off the air by the gateway that uplinked it (no hops used, not
    // relayed over the internet): a real radio link. Every gateway's copy counts,
    // so this comes before the duplicate check.
    if (gateway && gateway != from && hopStart && hopStart == hopLimit && !viaMqtt)
        note_link(from, gateway, (int32_t)lroundf(rxSnr * 4), LP_LINK_HEARD, now);
    if (seen_before(from, id, now)) return;

    g_head.uniqueTotal++;
    g_head.lastPacketAt = now;
    int s = (int)(now % 60);
    if (g_perSecAt[s] != now) { g_perSecAt[s] = now; g_perSec[s] = 0; }
    g_perSec[s]++;
    hist_advance(now);
    if (g_hist[g_histMin % LP_HIST_MIN] < 65535) g_hist[g_histMin % LP_HIST_MIN]++;

    LpNode *n = find_node(from, 1);
    if (!n) return;
    if (!n->firstHeard) n->firstHeard = now;
    n->lastHeard = now;
    n->packets++;
    // The gateway's topic names a state (msh/US/GA/...): a guess for nodes that never send a position.
    if (g_haveGeo && !strncmp(topic, "msh/US/", 7) && isupper((unsigned char)topic[7]) &&
        isupper((unsigned char)topic[8]) && topic[9] == '/') {
        char ab[3] = { topic[7], topic[8], 0 };
        int st = geo_find_abbr(&g_geo, ab);
        if (st >= 0) n->topicState = (uint8_t)(st + 1);
    }

    if (decoded) { on_data(n, from, to, channel, decoded, decLen, now); return; }
    // Encrypted: try the default channel key. Anything else stays unread.
    uint8_t buf[512];
    if (encLen > sizeof buf) return;
    memcpy(buf, enc, encLen);
    uint8_t iv[16] = { 0 };
    uint64_t pid = id;
    memcpy(iv, &pid, 8);                          // packet id, little-endian, as 64 bits
    memcpy(iv + 8, &from, 4);                     // then the sender, then 4 zero bytes
    aes128_ctr(&g_aes, iv, buf, encLen);
    if (data_ok(buf, encLen)) on_data(n, from, to, channel, buf, encLen, now);
}

/* --------------------------------------------------------------- MQTT --- */

static int g_sock = -1;
static uint8_t g_rx[256 * 1024];
static size_t g_rxLen = 0;

static int send_all(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = send(g_sock, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return 0; }
        p += w;
        n -= (size_t)w;
    }
    return 1;
}

static size_t put_len(uint8_t *out, size_t n)
{
    size_t i = 0;
    do {
        uint8_t c = n & 0x7F;
        n >>= 7;
        out[i++] = (uint8_t)(c | (n ? 0x80 : 0));
    } while (n);
    return i;
}

static size_t put_str(uint8_t *out, const char *s)
{
    size_t n = strlen(s);
    out[0] = (uint8_t)(n >> 8);
    out[1] = (uint8_t)n;
    memcpy(out + 2, s, n);
    return n + 2;
}

static void mqtt_close(void)
{
    if (g_sock >= 0) close(g_sock);
    g_sock = -1;
    g_rxLen = 0;
}

static int mqtt_connect(void)
{
    char port[16];
    snprintf(port, sizeof port, "%d", cfg.port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg.host, port, &hints, &res) != 0 || !res) return 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        g_sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (g_sock < 0) continue;
        struct timeval tv = { 10, 0 };
        setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(g_sock, a->ai_addr, a->ai_addrlen) == 0) break;
        close(g_sock);
        g_sock = -1;
    }
    freeaddrinfo(res);
    if (g_sock < 0) return 0;

    char clientId[40];
    snprintf(clientId, sizeof clientId, "dms-listenpost-%d", (int)getpid());
    uint8_t body[512], pkt[520];
    size_t n = 0;
    n += put_str(body + n, "MQTT");
    body[n++] = 4;                                // 3.1.1
    body[n++] = 0xC2;                             // user, password, clean session
    body[n++] = 0;
    body[n++] = 60;                               // keep-alive seconds
    n += put_str(body + n, clientId);
    n += put_str(body + n, cfg.user);
    n += put_str(body + n, cfg.pass);
    size_t m = 0;
    pkt[m++] = 0x10;
    m += put_len(pkt + m, n);
    memcpy(pkt + m, body, n);
    if (!send_all(pkt, m + n)) { mqtt_close(); return 0; }

    n = 0;
    body[n++] = 0;
    body[n++] = 1;                                // packet id
    n += put_str(body + n, cfg.topic);
    body[n++] = 0;                                // QoS 0
    m = 0;
    pkt[m++] = 0x82;
    m += put_len(pkt + m, n);
    memcpy(pkt + m, body, n);
    if (!send_all(pkt, m + n)) { mqtt_close(); return 0; }
    return 1;
}

// Handles every whole MQTT packet in the buffer. Returns 0 if the server refused us.
static int mqtt_drain(void)
{
    size_t off = 0;
    while (g_rxLen - off >= 2) {
        uint8_t type = g_rx[off];
        size_t len = 0, i = 1;
        int shift = 0, done = 0;
        while (off + i < g_rxLen && i < 5) {
            uint8_t c = g_rx[off + i++];
            len |= (size_t)(c & 0x7F) << shift;
            shift += 7;
            if (!(c & 0x80)) { done = 1; break; }
        }
        if (!done) break;
        if (len > sizeof g_rx - 8) return 0;      // bigger than anything the feed sends
        if (g_rxLen - off < i + len) break;
        const uint8_t *p = g_rx + off + i;
        if ((type >> 4) == 2) {                   // CONNACK
            if (len >= 2 && p[1] != 0) { logf_("server refused the connection (code %d)", p[1]); return 0; }
            g_head.status = LP_ST_LIVE;
            logf_("connected to %s, listening on %s", cfg.host, cfg.topic);
        } else if ((type >> 4) == 3 && len >= 2) { // PUBLISH
            size_t tl = (size_t)p[0] << 8 | p[1];
            size_t hdr = 2 + tl + (((type >> 1) & 3) ? 2 : 0);
            if (hdr <= len && tl < 256) {
                char topic[256];
                memcpy(topic, p + 2, tl);
                topic[tl] = 0;
                on_envelope(topic, p + hdr, len - hdr);
            }
        }
        off += i + len;
    }
    if (off) {
        memmove(g_rx, g_rx + off, g_rxLen - off);
        g_rxLen -= off;
    }
    return 1;
}

/* ------------------------------------------------------------- saving --- */

static void chat_in_order(LpChat *out)
{
    for (int i = 0; i < g_chatN; i++) out[i] = g_chat[(g_chatHead + i) % cfg.chatLines];
}

static uint32_t per_minute(uint32_t now)
{
    uint32_t sum = 0;
    for (int i = 0; i < 60; i++)
        if (now - g_perSecAt[i] < 60) sum += g_perSec[i];
    return sum;
}

static void save(int full)
{
    uint32_t now = (uint32_t)time(NULL);
    g_head.savedAt = now;
    g_head.pktPerMin = per_minute(now);
    hist_advance(now);
    LpData d;
    memset(&d, 0, sizeof d);
    d.h = g_head;
    d.chats = g_chatN ? malloc(sizeof *d.chats * (size_t)g_chatN) : NULL;
    if (d.chats) chat_in_order(d.chats);
    d.h.chats = d.chats ? (uint32_t)g_chatN : 0;
    uint16_t hist[LP_HIST_MIN];
    for (int i = 0; i < LP_HIST_MIN; i++) hist[i] = g_hist[(g_histMin + 1 + (uint32_t)i) % LP_HIST_MIN];
    d.hist = hist;
    d.h.histCount = g_histMin ? LP_HIST_MIN : 0;
    d.h.histEndMin = g_histMin;
    char path[1200];
    if (full) {
        expire_nodes(now);
        expire_links(now);
        snprintf(path, sizeof path, "%s/snapshot.bin", g_data);
        d.nodes = g_nodes;
        d.h.nodes = g_nNodes;
        d.links = g_links;
        d.h.links = g_nLinks;
        if (!lp_write_data(path, &d)) logf_("couldn't write %s: %s", path, strerror(errno));
    }
    // live.bin: only the nodes and links heard in the last two minutes.
    LpNode *recent = g_nNodes ? malloc(sizeof *recent * g_nNodes) : NULL;
    LpLink *recentL = g_nLinks ? malloc(sizeof *recentL * g_nLinks) : NULL;
    uint32_t nr = 0, nl = 0;
    for (uint32_t i = 0; recent && i < g_nNodes; i++)
        if (now - g_nodes[i].lastHeard <= 120) recent[nr++] = g_nodes[i];
    for (uint32_t i = 0; recentL && i < g_nLinks; i++)
        if (now - g_links[i].lastHeard <= 120) recentL[nl++] = g_links[i];
    d.nodes = recent;
    d.h.nodes = nr;
    d.links = recentL;
    d.h.links = nl;
    snprintf(path, sizeof path, "%s/live.bin", g_data);
    lp_write_data(path, &d);
    free(recent);
    free(recentL);
    free(d.chats);
}

static void load_previous(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/snapshot.bin", g_data);
    LpData d;
    if (!lp_read_data(path, &d)) return;
    for (uint32_t i = 0; i < d.h.nodes; i++) {
        LpNode *n = find_node(d.nodes[i].id, 1);
        if (n) *n = d.nodes[i];
    }
    uint32_t first = d.h.chats > (uint32_t)cfg.chatLines ? d.h.chats - (uint32_t)cfg.chatLines : 0;
    for (uint32_t i = first; i < d.h.chats; i++) g_chat[g_chatN++] = d.chats[i];
    for (uint32_t i = 0; i < d.h.links; i++)
        note_link(d.links[i].a, d.links[i].b, d.links[i].snr_q, d.links[i].source, d.links[i].lastHeard);
    // The history carries on if the last run ended within three hours.
    if (d.h.histCount && d.h.histEndMin) {
        g_histMin = d.h.histEndMin;
        uint32_t n = d.h.histCount < LP_HIST_MIN ? d.h.histCount : LP_HIST_MIN;
        for (uint32_t i = 0; i < n; i++) g_hist[(g_histMin - (n - 1 - i)) % LP_HIST_MIN] = d.hist[d.h.histCount - n + i];
        hist_advance((uint32_t)time(NULL));
    }
    g_head.uniqueTotal = d.h.uniqueTotal;
    lp_data_free(&d);
    logf_("loaded %u nodes, %u links and %d chat lines from the last run", g_nNodes, g_nLinks, g_chatN);
}

/* ------------------------------------------------------ who's in here --- */

// Counts doors that are still there, removing the files of any that aren't.
static int doors_present(void)
{
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/doors", g_data);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int alive = 0;
    time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1500];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int pid = atoi(e->d_name);
        struct stat st;
        int fresh = stat(path, &st) == 0 && now - st.st_mtime < 120;
        int running = pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
        if (fresh && running) alive++;
        else unlink(path);
    }
    closedir(d);
    return alive;
}

/* --------------------------------------------------------------- main --- */

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void self_test(void)
{
    // FIPS-197 appendix C.1.
    static const uint8_t key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t ct[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    Aes128 a;
    uint8_t out[16];
    aes128_init(&a, key);
    aes128_encrypt_block(&a, pt, out);
    if (memcmp(out, ct, 16)) { fprintf(stderr, "AES self-test failed\n"); exit(2); }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") != 0) snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    else {
        char exe[1024];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return 1;
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        snprintf(g_dir, sizeof g_dir, "%s", exe);
    }
    self_test();
    if (argc > 1 && !strcmp(argv[1], "--selftest")) { puts("ok"); return 0; }

    snprintf(g_data, sizeof g_data, "%s/data", g_dir);
    mkdir(g_data, 0755);
    char doors[1200], lockPath[1200];
    snprintf(doors, sizeof doors, "%s/doors", g_data);
    mkdir(doors, 0755);

    // One collector at a time.
    snprintf(lockPath, sizeof lockPath, "%s/collector.lock", g_data);
    int lock = open(lockPath, O_RDWR | O_CREAT, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 0;

    load_config();
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    char path[1200];
    snprintf(path, sizeof path, "%s/states.bin", g_dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        static uint8_t blob[1 << 20];
        size_t n = fread(blob, 1, sizeof blob, f);
        fclose(f);
        g_haveGeo = geo_load(&g_geo, blob, n);
    }
    if (!g_haveGeo) logf_("no states.bin: nodes won't be placed in states");

    aes128_init(&g_aes, DEFAULT_KEY);
    g_chat = calloc((size_t)cfg.chatLines, sizeof *g_chat);
    if (!g_chat) return 1;
    rehash();
    load_previous();

    g_head.startedAt = (uint32_t)time(NULL);
    g_head.status = LP_ST_CONNECTING;
    logf_("started (linger %d min, keep %d days)", cfg.lingerMinutes, cfg.nodeDays);
    save(0);

    time_t lastSave = 0, lastFull = time(NULL), lastScan = 0, lastPing = 0, lastData = 0, nextTry = 0, alone = 0;
    int backoff = 5;
    while (!g_stop) {
        time_t now = time(NULL);
        if (g_sock < 0 && now >= nextTry) {
            g_head.status = g_head.status == LP_ST_OFFLINE ? LP_ST_OFFLINE : LP_ST_CONNECTING;
            if (mqtt_connect()) {
                lastData = lastPing = now;
                backoff = 5;
            } else {
                g_head.status = LP_ST_OFFLINE;
                logf_("couldn't reach %s:%d, trying again in %d s", cfg.host, cfg.port, backoff);
                nextTry = now + backoff;
                backoff = backoff * 2 > 120 ? 120 : backoff * 2;
            }
        }
        if (g_sock >= 0) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(g_sock, &rd);
            struct timeval tv = { 1, 0 };
            int r = select(g_sock + 1, &rd, NULL, NULL, &tv);
            if (r > 0) {
                ssize_t got = recv(g_sock, g_rx + g_rxLen, sizeof g_rx - g_rxLen, 0);
                if (got <= 0) {
                    logf_("the server closed the connection");
                    mqtt_close();
                    g_head.status = LP_ST_OFFLINE;
                    nextTry = now + backoff;
                } else {
                    g_rxLen += (size_t)got;
                    lastData = now;
                    if (!mqtt_drain()) { mqtt_close(); g_head.status = LP_ST_OFFLINE; nextTry = now + 60; }
                }
            }
            if (g_sock >= 0 && now - lastPing >= 30) {
                static const uint8_t ping[2] = { 0xC0, 0x00 };
                if (!send_all(ping, 2)) { mqtt_close(); g_head.status = LP_ST_OFFLINE; nextTry = now + backoff; }
                lastPing = now;
            }
            if (g_sock >= 0 && now - lastData > 120) {
                logf_("nothing heard for two minutes: reconnecting");
                mqtt_close();
                g_head.status = LP_ST_CONNECTING;
                nextTry = now;
            }
        } else {
            sleep(1);
        }

        now = time(NULL);
        if (now - lastSave >= 5) {
            int full = now - lastFull >= 60;
            save(full);
            if (full) lastFull = now;
            lastSave = now;
        }
        if (now - lastScan >= 15) {
            lastScan = now;
            if (doors_present()) alone = 0;
            else if (!alone) {
                alone = now;
                logf_("nobody is using the door: stopping in %d min unless someone comes back", cfg.lingerMinutes);
            } else if (now - alone >= (time_t)cfg.lingerMinutes * 60) {
                logf_("nobody for %d min: stopping", cfg.lingerMinutes);
                break;
            }
        }
    }
    mqtt_close();
    g_head.status = LP_ST_STOPPED;
    save(1);
    logf_("stopped with %u nodes and %d chat lines saved", g_nNodes, g_chatN);
    return 0;
}
