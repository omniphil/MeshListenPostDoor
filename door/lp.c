/*
 * lp.c - the Listening Post door's data. See lp.h.
 */
#define _GNU_SOURCE
#include "lp.h"

#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "door.h"

LpConfig lp_config;
GeoMap   lp_geo;
bool     lp_haveGeo = false;

LpNode  *lp_nodes = NULL;
uint32_t lp_nNodes = 0;
LpChat  *lp_chat = NULL;
uint32_t lp_nChat = 0;
LpHead   lp_head;
bool     lp_firstEver = false;
uint32_t *lp_changed = NULL;
LpLink  *lp_links = NULL;
uint32_t lp_nLinks = 0;
uint32_t *lp_changedLinks = NULL;
uint16_t lp_hist[LP_HIST_MIN];
uint32_t lp_histCount = 0, lp_histEnd = 0;
static uint32_t g_capLinks = 0;
static int32_t *g_linkHash = NULL;
static uint32_t g_linkHashCap = 0;

static uint32_t g_cap = 0;
static int32_t *g_hash = NULL;
static uint32_t g_hashCap = 0;
static uint32_t g_liveSavedAt = 0;

/* ------------------------------------------------------------- paths --- */

const char *lp_beside_exe(const char *name, char *out, size_t size)
{
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
    if (len > 0) {
        path[len] = '\0';
        char *slash = strrchr(path, '/');
        if (slash) {
            slash[1] = '\0';
            snprintf(out, size, "%s%s", path, name);
            return out;
        }
    }
    snprintf(out, size, "%s", name);
    return out;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void read_kv(const char *path, void (*fn)(const char *k, const char *v, void *ctx), void *ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        if (line[0]) fn(line, eq + 1, ctx);
    }
    fclose(f);
}

static void write_file(const char *path, const void *data, size_t len)
{
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%.1200s.%d.tmp", path, (int)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    fwrite(data, 1, len, fp);
    fclose(fp);
    rename(tmp, path);
}

/* ------------------------------------------------------------ config --- */

static void config_kv(const char *k, const char *v, void *ctx)
{
    (void)ctx;
    if (!strcmp(k, "node_days")) lp_config.nodeDays = atoi(v);
    else if (!strcmp(k, "weather_saves")) snprintf(lp_config.weatherSaves, sizeof lp_config.weatherSaves, "%s", v);
    else if (!strcmp(k, "default_region")) {
        if (!strcasecmp(v, "all") || !strcasecmp(v, "all us") || !strcasecmp(v, "us")) lp_config.defaultRegion = 0;
        else if (lp_haveGeo) {
            int st = strlen(v) == 2 ? geo_find_abbr(&lp_geo, v) : -1;
            for (int i = 0; st < 0 && i < lp_geo.count; i++)
                if (!strcasecmp(lp_geo.st[i].name, v)) st = i;
            lp_config.defaultRegion = st >= 0 ? st + 1 : 0;
        }
    }
}

bool lp_load_states(void)
{
    char path[1100];
    FILE *f = fopen(lp_beside_exe("states.bin", path, sizeof path), "rb");
    if (!f) return false;
    static uint8_t blob[1 << 20];
    size_t n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    lp_haveGeo = geo_load(&lp_geo, blob, n);
    return lp_haveGeo;
}

void lp_load_config(void)
{
    memset(&lp_config, 0, sizeof lp_config);
    lp_config.nodeDays = 7;
    lp_config.defaultRegion = 0;
    char path[1100];
    /* The Weather door, beside this one, is where callers tell the board their town. */
    lp_beside_exe("../weather/saves", lp_config.weatherSaves, sizeof lp_config.weatherSaves);
    read_kv(lp_beside_exe("lp.ini", path, sizeof path), config_kv, NULL);
    if (lp_config.nodeDays < 1) lp_config.nodeDays = 1;
    if (lp_config.nodeDays > 60) lp_config.nodeDays = 60;
}

/* ------------------------------------------------------------- prefs --- */

/* <root>/<handle>-<usernum>/prefs, the same shape as the other doors. */
static const char *user_prefs(const char *root, char *out, size_t size, bool make)
{
    char who[MAX_USERNAME], dir[1300];
    snprintf(who, sizeof who, "%s", door_info.handle[0] ? door_info.handle : "Player");
    for (char *c = who; *c; c++)
        if (*c == '/' || *c == '\\' || *c == ' ' || *c == '.') *c = '_';
    if (make) mkdir(root, 0755);
    snprintf(dir, sizeof dir, "%s/%s-%d", root, who, door_info.user_record);
    if (make) mkdir(dir, 0755);
    snprintf(out, size, "%s/prefs", dir);
    return out;
}

static const char *prefs_path(char *out, size_t size)
{
    char root[1100];
    lp_beside_exe("saves", root, sizeof root);
    return user_prefs(root, out, size, true);
}

const char *lp_user_file(const char *name, char *out, size_t size)
{
    char path[1400];
    prefs_path(path, sizeof path);
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    snprintf(out, size, "%s/%s", path, name);
    return out;
}

/* The Weather door's prefs for this caller: only the town is taken. */
static void weather_kv(const char *k, const char *v, void *ctx)
{
    LpUserPrefs *p = ctx;
    if (!strcmp(k, "place")) { snprintf(p->homeName, sizeof p->homeName, "%s", v); p->haveHome = true; }
    else if (!strcmp(k, "lat")) p->homeLat = atof(v);
    else if (!strcmp(k, "lon")) p->homeLon = atof(v);
}

static void prefs_kv(const char *k, const char *v, void *ctx)
{
    LpUserPrefs *p = ctx;
    if (!strcmp(k, "region")) p->region = atoi(v);
    else if (!strcmp(k, "display")) p->display = atoi(v);
}

void lp_load_prefs(LpUserPrefs *p)
{
    memset(p, 0, sizeof *p);
    p->region = lp_config.defaultRegion;
    char path[1400];
    read_kv(prefs_path(path, sizeof path), prefs_kv, p);
    if (p->region < 0 || (lp_haveGeo && p->region > lp_geo.count)) p->region = 0;
    if (lp_config.weatherSaves[0]) {
        read_kv(user_prefs(lp_config.weatherSaves, path, sizeof path, false), weather_kv, p);
        if (p->haveHome && p->homeLat == 0 && p->homeLon == 0) p->haveHome = false;
        if (p->haveHome && lp_haveGeo) {
            int st = geo_state_at(&lp_geo, (float)p->homeLat, (float)p->homeLon);
            p->homeState = st >= 0 ? st + 1 : 0;
        }
    }
}

void lp_save_prefs(const LpUserPrefs *p)
{
    char path[1400], buf[120];
    prefs_path(path, sizeof path);
    int n = snprintf(buf, sizeof buf, "region = %d\ndisplay = %d\n", p->region, p->display);
    write_file(path, buf, (size_t)n);
}

/* --------------------------------------------------------- collector --- */

static char g_here[1300];
static time_t g_touchedAt = 0;

static void data_path(const char *name, char *out, size_t size)
{
    char dir[1100];
    lp_beside_exe("data", dir, sizeof dir);
    snprintf(out, size, "%s/%s", dir, name);
}

/* Starts lpcollect, fully detached: its own session, no terminal, none of the
 * door's descriptors (the caller's connection must not stay open in it). */
static void start_collector(void)
{
    char exe[1100];
    lp_beside_exe("lpcollect", exe, sizeof exe);
    if (access(exe, X_OK) != 0) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        for (int fd = 0; fd < 1024; fd++) close(fd);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        signal(SIGHUP, SIG_IGN);
        execl(exe, "lpcollect", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

/* Whether a collector holds the lock right now. */
static bool collector_running(void)
{
    char path[1200];
    data_path("collector.lock", path, sizeof path);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return false;
    bool running = flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!running) flock(fd, LOCK_UN);
    close(fd);
    return running;
}

void lp_arrive(void)
{
    char dir[1200];
    lp_beside_exe("data", dir, sizeof dir);
    mkdir(dir, 0755);
    data_path("doors", dir, sizeof dir);
    mkdir(dir, 0755);
    snprintf(g_here, sizeof g_here, "%s/%d", dir, (int)getpid());
    FILE *f = fopen(g_here, "w");
    if (f) fclose(f);
    g_touchedAt = time(NULL);
    if (!collector_running()) start_collector();
}

void lp_touch(void)
{
    time_t now = time(NULL);
    if (!g_here[0] || now - g_touchedAt < 30) return;
    g_touchedAt = now;
    FILE *f = fopen(g_here, "w");         /* recreated if the collector swept it away */
    if (f) fclose(f);
    /* A collector that died (or was never able to start) is started again. */
    if (!collector_running()) start_collector();
}

void lp_leave(void)
{
    if (g_here[0]) unlink(g_here);
    g_here[0] = 0;
}

/* -------------------------------------------------------------- nodes --- */

static uint32_t hash_id(uint32_t id) { id ^= id >> 16; id *= 0x7feb352d; id ^= id >> 15; id *= 0x846ca68b; id ^= id >> 16; return id; }

static void rehash(void)
{
    uint32_t cap = 8192;
    while (cap < lp_nNodes * 2 + 16) cap *= 2;
    int32_t *h = malloc(sizeof *h * cap);
    if (!h) return;
    for (uint32_t i = 0; i < cap; i++) h[i] = -1;
    for (uint32_t i = 0; i < lp_nNodes; i++) {
        uint32_t k = hash_id(lp_nodes[i].id) & (cap - 1);
        while (h[k] >= 0) k = (k + 1) & (cap - 1);
        h[k] = (int32_t)i;
    }
    free(g_hash);
    g_hash = h;
    g_hashCap = cap;
}

/* Adds or updates a node; returns its index, or -1. */
static int32_t upsert(const LpNode *n)
{
    if (g_hashCap) {
        uint32_t k = hash_id(n->id) & (g_hashCap - 1);
        while (g_hash[k] >= 0) {
            if (lp_nodes[g_hash[k]].id == n->id) { lp_nodes[g_hash[k]] = *n; return g_hash[k]; }
            k = (k + 1) & (g_hashCap - 1);
        }
    }
    if (lp_nNodes == g_cap) {
        uint32_t cap = g_cap ? g_cap * 2 : 4096;
        LpNode *nn = realloc(lp_nodes, sizeof *nn * cap);
        uint32_t *ch = realloc(lp_changed, sizeof *ch * cap);
        if (!nn || !ch) { if (nn) lp_nodes = nn; if (ch) lp_changed = ch; return -1; }
        lp_nodes = nn;
        lp_changed = ch;
        g_cap = cap;
    }
    lp_nodes[lp_nNodes++] = *n;
    if (lp_nNodes * 2 > g_hashCap) rehash();
    else {
        uint32_t k = hash_id(n->id) & (g_hashCap - 1);
        while (g_hash[k] >= 0) k = (k + 1) & (g_hashCap - 1);
        g_hash[k] = (int32_t)(lp_nNodes - 1);
    }
    return (int32_t)(lp_nNodes - 1);
}

const LpNode *lp_find(uint32_t id)
{
    if (!g_hashCap) return NULL;
    uint32_t k = hash_id(id) & (g_hashCap - 1);
    while (g_hash[k] >= 0) {
        if (lp_nodes[g_hash[k]].id == id) return &lp_nodes[g_hash[k]];
        k = (k + 1) & (g_hashCap - 1);
    }
    return NULL;
}

static void take_chat(LpChat *chats, uint32_t n)
{
    free(lp_chat);
    lp_chat = chats;
    lp_nChat = n;
}

/* ------------------------------------------------------------- links --- */

static uint32_t link_hash(uint32_t a, uint32_t b) { return hash_id(a * 2654435761u ^ hash_id(b)); }

static void link_rehash(void)
{
    uint32_t cap = 8192;
    while (cap < lp_nLinks * 2 + 16) cap *= 2;
    int32_t *h = malloc(sizeof *h * cap);
    if (!h) return;
    for (uint32_t i = 0; i < cap; i++) h[i] = -1;
    for (uint32_t i = 0; i < lp_nLinks; i++) {
        uint32_t k = link_hash(lp_links[i].a, lp_links[i].b) & (cap - 1);
        while (h[k] >= 0) k = (k + 1) & (cap - 1);
        h[k] = (int32_t)i;
    }
    free(g_linkHash);
    g_linkHash = h;
    g_linkHashCap = cap;
}

/* Adds or updates a link; returns its index if it changed, else -1. */
static int32_t link_upsert(const LpLink *l)
{
    if (!g_linkHashCap) link_rehash();
    if (!g_linkHashCap) return -1;
    uint32_t k = link_hash(l->a, l->b) & (g_linkHashCap - 1);
    while (g_linkHash[k] >= 0) {
        LpLink *o = &lp_links[g_linkHash[k]];
        if (o->a == l->a && o->b == l->b) {
            if (!memcmp(o, l, sizeof *l)) return -1;
            *o = *l;
            return g_linkHash[k];
        }
        k = (k + 1) & (g_linkHashCap - 1);
    }
    if (lp_nLinks == g_capLinks) {
        uint32_t cap = g_capLinks ? g_capLinks * 2 : 4096;
        LpLink *nl = realloc(lp_links, sizeof *nl * cap);
        uint32_t *ch = realloc(lp_changedLinks, sizeof *ch * cap);
        if (nl) lp_links = nl;
        if (ch) lp_changedLinks = ch;
        if (!nl || !ch) return -1;
        g_capLinks = cap;
    }
    lp_links[lp_nLinks] = *l;
    g_linkHash[k] = (int32_t)lp_nLinks;
    lp_nLinks++;
    if (lp_nLinks * 2 > g_linkHashCap) link_rehash();
    return (int32_t)(lp_nLinks - 1);
}

static void take_hist(const LpData *d)
{
    lp_histCount = d->h.histCount < LP_HIST_MIN ? d->h.histCount : LP_HIST_MIN;
    lp_histEnd = d->h.histEndMin;
    memcpy(lp_hist, d->hist + (d->h.histCount - lp_histCount), sizeof *lp_hist * lp_histCount);
}

double lp_km(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 0.017453292519943295;
    double a = sin((lat2 - lat1) * r / 2), b = sin((lon2 - lon1) * r / 2);
    double h = a * a + cos(lat1 * r) * cos(lat2 * r) * b * b;
    return 2 * 6371.0 * asin(sqrt(h < 1 ? h : 1));
}

void lp_load_snapshot(void)
{
    char path[1200];
    LpData d;
    data_path("snapshot.bin", path, sizeof path);
    if (lp_read_data(path, &d)) {
        uint32_t now = (uint32_t)time(NULL), cut = now - (uint32_t)lp_config.nodeDays * 86400u;
        for (uint32_t i = 0; i < d.h.nodes; i++)
            if (d.nodes[i].lastHeard >= cut) upsert(&d.nodes[i]);
        for (uint32_t i = 0; i < d.h.links; i++)
            if (now - d.links[i].lastHeard <= 86400) link_upsert(&d.links[i]);
        take_chat(d.chats, d.h.chats);
        d.chats = NULL;
        take_hist(&d);
        lp_head = d.h;
        lp_data_free(&d);
    } else {
        lp_firstEver = true;
    }
    lp_refresh();                          /* and whatever is newer in live.bin */
}

LpChanges lp_refresh(void)
{
    LpChanges c = { 0, 0, false, false };
    char path[1200];
    data_path("live.bin", path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) return c;
    LpData d;
    if (!lp_read_data(path, &d)) return c;
    if (d.h.savedAt == g_liveSavedAt) { lp_data_free(&d); return c; }
    g_liveSavedAt = d.h.savedAt;
    if (d.h.savedAt >= lp_head.savedAt) lp_head = d.h;
    for (uint32_t i = 0; i < d.h.nodes; i++) {
        const LpNode *old = lp_find(d.nodes[i].id);
        if (old && !memcmp(old, &d.nodes[i], sizeof *old)) continue;
        int32_t at = upsert(&d.nodes[i]);
        if (at >= 0 && lp_changed) lp_changed[c.nodes++] = (uint32_t)at;
    }
    for (uint32_t i = 0; i < d.h.links; i++) {
        int32_t at = link_upsert(&d.links[i]);
        if (at >= 0 && lp_changedLinks) lp_changedLinks[c.links++] = (uint32_t)at;
    }
    bool same = d.h.chats == lp_nChat && (!d.h.chats || !memcmp(d.chats, lp_chat, sizeof *d.chats * d.h.chats));
    if (!same) { take_chat(d.chats, d.h.chats); d.chats = NULL; c.chat = true; }
    if (d.h.histCount && (d.h.histEndMin != lp_histEnd || memcmp(lp_hist, d.hist + d.h.histCount - (d.h.histCount < LP_HIST_MIN ? d.h.histCount : LP_HIST_MIN),
                                                                  sizeof *lp_hist * lp_histCount))) {
        take_hist(&d);
        c.hist = true;
    }
    lp_data_free(&d);
    return c;
}

int lp_feed_state(void)
{
    uint32_t now = (uint32_t)time(NULL);
    if (!lp_head.savedAt || now - lp_head.savedAt > 20 || lp_head.status == LP_ST_STOPPED)
        return lp_head.status == LP_ST_OFFLINE && now - lp_head.savedAt <= 20 ? LP_FEED_OFFLINE : LP_FEED_CONNECTING;
    if (lp_head.status == LP_ST_LIVE) return LP_FEED_LIVE;
    if (lp_head.status == LP_ST_OFFLINE) return LP_FEED_OFFLINE;
    return LP_FEED_CONNECTING;
}
