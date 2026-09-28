/*
 * ansi_lp.c - the Listening Post as a 24-bit ANSI screen. See ansi_lp.h.
 *
 * Four pages, all keyboard:
 *   Map    the US (or one state) in half blocks, 56x34 pixels: the land, the
 *          state lines, and every node heard, coloured by how recently. Beside
 *          it the numbers; under it the latest public chat.
 *   Nodes  everything heard in the region, newest first.
 *   Chat   the public chat, newest at the bottom.
 *   Region a list of All US and the states.
 * Laid out in 79x24 so nothing lands in the screen's last column or row, and
 * sent as a diff so a frame costs only the cells that changed.
 */
#include "ansi_lp.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "door.h"
#include "hwmodels.h"

#define ROWS 24
#define COLS 79
#define CSI "\033["

#define C_BG       0x060B14
#define C_BAR      0x0C1424
#define C_PANEL    0x101a2e
#define C_PANEL_HI 0x1c2c4c
#define C_EDGE     0x2a3a5e
#define C_TEXT     0xEAF2F4
#define C_TEXT2    0xA2B2C4
#define C_TEXT3    0x62728E
#define C_MESH     0x67EA94            /* Meshtastic green */
#define C_ACCENT   0x4FD1FF
#define C_WARN     0xFFB547
#define C_ERR      0xFF6B6B
#define C_SEA      0x07101E
#define C_LAND     0x14263A
#define C_LAND_HI  0x1D3752
#define C_BORDER   0x35547A

typedef struct { unsigned char ch; uint32_t fg, bg; } Cell;

static Cell g_scr[ROWS][COLS];
static Cell g_sent[ROWS][COLS];
static bool g_sentValid = false;

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (uint32_t)((int)(ar + (br - ar) * t) << 16 | (int)(ag + (bg - ag) * t) << 8 | (int)(ab + (bb - ab) * t));
}

static void clear(uint32_t bg)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) g_scr[r][c] = (Cell){ ' ', C_TEXT, bg };
}

static void fill(int r, int c, int h, int w, uint32_t bg)
{
    for (int y = r; y < r + h && y < ROWS; y++)
        for (int x = c; x < c + w && x < COLS; x++)
            if (y >= 0 && x >= 0) g_scr[y][x] = (Cell){ ' ', C_TEXT, bg };
}

/* Latin-1 (what the collector stores) to CP437 (what ANSI terminals draw). */
static unsigned char cp437(unsigned char c)
{
    static const unsigned char HI[96] = {
        0xFF, 0xAD, 0x9B, 0x9C, '?',  0x9D, '|',  0x15, '"',  'c',  0xA6, 0xAE, 0xAA, '-',  'r',  '-',
        0xF8, 0xF1, 0xFD, '3',  '\'', 0xE6, 0x14, 0xFA, ',',  '1',  0xA7, 0xAF, 0xAC, 0xAB, '?',  0xA8,
        'A',  'A',  'A',  'A',  0x8E, 0x8F, 0x92, 0x80, 'E',  0x90, 'E',  'E',  'I',  'I',  'I',  'I',
        'D',  0xA5, 'O',  'O',  'O',  'O',  0x99, 'x',  'O',  'U',  'U',  'U',  0x9A, 'Y',  'P',  0xE1,
        0x85, 0xA0, 0x83, 'a',  0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
        'd',  0xA4, 0x95, 0xA2, 0x93, 'o',  0x94, 0xF6, 'o',  0x97, 0xA3, 0x96, 0x81, 'y',  'p',  0x98,
    };
    if (c < 0x20) return ' ';
    if (c < 0x7F) return c;
    if (c >= 0xA0) return HI[c - 0xA0];
    return '?';
}

/* Writes Latin-1 text at (r, c); bg KEEP keeps what's there. Returns the column after it. */
#define KEEP 0xFFFFFFFFu
static int put(int r, int c, const char *s, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS) return c;
    for (; *s; s++, c++) {
        if (c < 0) continue;
        if (c >= COLS) break;
        g_scr[r][c].ch = cp437((unsigned char)*s);
        g_scr[r][c].fg = fg;
        if (bg != KEEP) g_scr[r][c].bg = bg;
    }
    return c;
}

/* The same, but the bytes are already CP437 (box and block characters). */
static int putraw(int r, int c, const char *s, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS) return c;
    for (; *s; s++, c++) {
        if (c < 0) continue;
        if (c >= COLS) break;
        g_scr[r][c].ch = (unsigned char)*s;
        g_scr[r][c].fg = fg;
        if (bg != KEEP) g_scr[r][c].bg = bg;
    }
    return c;
}

static int putf(int r, int c, uint32_t fg, uint32_t bg, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return put(r, c, buf, fg, bg);
}

static void put_r(int r, int cEnd, const char *s, uint32_t fg, uint32_t bg)
{
    put(r, cEnd - (int)strlen(s), s, fg, bg);
}

static void put_c(int r, int c0, int w, const char *s, uint32_t fg, uint32_t bg)
{
    put(r, c0 + (w - (int)strlen(s)) / 2, s, fg, bg);
}

static void setch(int r, int c, unsigned char ch, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    g_scr[r][c].ch = ch;
    g_scr[r][c].fg = fg;
    if (bg != KEEP) g_scr[r][c].bg = bg;
}

/* Sends what changed since the last flush. */
static void flush(void)
{
    static char out[96 * 1024];
    size_t n = 0;
    uint32_t fg = KEEP, bg = KEEP;
    int cr = -1, cc = -1;
#define EMIT(...) (n += (size_t)snprintf(out + n, sizeof out - n, __VA_ARGS__))
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            Cell *want = &g_scr[r][c];
            if (g_sentValid && !memcmp(want, &g_sent[r][c], sizeof *want)) continue;
            if (n == 0) EMIT(CSI "?25l");
            if (cr != r || cc != c) EMIT(CSI "%d;%dH", r + 1, c + 1);
            if (want->bg != bg) { EMIT(CSI "48;2;%u;%u;%um", (want->bg >> 16) & 255, (want->bg >> 8) & 255, want->bg & 255); bg = want->bg; }
            if (want->fg != fg && want->ch != ' ') { EMIT(CSI "38;2;%u;%u;%um", (want->fg >> 16) & 255, (want->fg >> 8) & 255, want->fg & 255); fg = want->fg; }
            out[n++] = (char)want->ch;
            g_sent[r][c] = *want;
            cr = r;
            cc = c + 1;
            if (n > sizeof out - 256) { door_write_raw(out, n); n = 0; }
        }
    g_sentValid = true;
    if (n) door_write_raw(out, n);
#undef EMIT
}

static void invalidate(void) { g_sentValid = false; }

/* ------------------------------------------------------------- state --- */

enum { PAGE_MAP, PAGE_NODES, PAGE_CHAT, PAGE_STATS };

#define MAP_PW 56                   /* map pixels: 56 x 34, half blocks in 17 rows */
#define MAP_PH 34
#define MAP_R  1
#define SIDE_C 57
#define SIDE_W 21

static LpUserPrefs *g_prefs;
static int      g_page = PAGE_MAP;
static int      g_listTop = 0, g_listSel = 0, g_chatBack = 0;
static uint8_t  g_landAt[MAP_PW * MAP_PH];    /* state index + 1 per pixel, 0 = sea */
static int      g_landFor = -1;
static GeoView  g_view;
static uint32_t *g_order = NULL;              /* node indexes in the region, newest first */
static uint32_t g_nOrder = 0;
static char     g_filter[24];                 /* the Nodes page's find text (/) */
static int      g_filterLen = 0, g_typing = 0;
static bool     g_sensorsOnly = false;        /* the Nodes page shows sensors and their readings (E) */
static bool     g_hideChatter = true;         /* test chatter hidden (F) */
static int      g_chanSel = -1;               /* the chat's channel, -1 = all (Tab) */
static char     g_note[120];                  /* a message on the status line, for a few seconds */
static time_t   g_noteUntil = 0;
static uint16_t *g_linkCount = NULL;          /* per node index: links heard in the last day */
static uint32_t g_linkCountFor = 0;

static void note(const char *m) { snprintf(g_note, sizeof g_note, "%s", m); g_noteUntil = time(NULL) + 5; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && lower((unsigned char)p[k]) == lower((unsigned char)needle[k])) k++;
        if (k == n) return true;
    }
    return false;
}

static bool matches(const LpNode *n)
{
    if (g_sensorsOnly && !(n->flags & LP_NODE_ENV)) return false;
    if (!g_filterLen) return true;
    const char *q = g_filter[0] == '!' ? g_filter + 1 : g_filter;
    char id[12];
    snprintf(id, sizeof id, "%08x", n->id);
    return contains(n->name, q) || contains(n->shortName, q) || contains(id, q);
}

/* How many radios each node has been heard talking to directly, in the last day. */
static void count_links(void)
{
    if (g_linkCountFor == lp_nNodes + lp_nLinks * 7919u && g_linkCount) return;
    free(g_linkCount);
    g_linkCount = calloc(lp_nNodes + 1, sizeof *g_linkCount);
    if (!g_linkCount) return;
    uint32_t now = (uint32_t)time(NULL);
    for (uint32_t i = 0; i < lp_nLinks; i++) {
        if (now - lp_links[i].lastHeard > 86400) continue;
        const LpNode *a = lp_find(lp_links[i].a), *b = lp_find(lp_links[i].b);
        if (a && g_linkCount[a - lp_nodes] < 65535) g_linkCount[a - lp_nodes]++;
        if (b && g_linkCount[b - lp_nodes] < 65535) g_linkCount[b - lp_nodes]++;
    }
    g_linkCountFor = lp_nNodes + lp_nLinks * 7919u;
}

static double node_km(const LpNode *n)
{
    if (!g_prefs->haveHome || !(n->flags & LP_NODE_POS)) return -1;
    return lp_km(g_prefs->homeLat, g_prefs->homeLon, n->lat1e5 / 1e5, n->lon1e5 / 1e5);
}

static int temp_f(int16_t dC) { return (int)lroundf(dC / 10.0f * 9 / 5 + 32); }

/* Test chatter: "test", "hi", "check"... and a sender's repeats within 3 hours. */
static bool chatter_text(const char *t)
{
    char k[40];
    int n = 0;
    for (const char *p = t; *p && n < 38; p++) {
        char c = (char)lower((unsigned char)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) k[n++] = c;
    }
    k[n] = 0;
    if (!n) return true;
    static const char *const W[] = { "test", "testing", "tests", "testtest", "hi", "hello", "hey", "ping", "pong", "check",
        "checkcheck", "checkonetwothree", "ack", "ok", "okay", "yes", "123", "1234", "receivedinatl", "received", "cmd",
        "radiocheck", "copy", "rogerthat", "roger", "hellomesh", "hellotest", "testmsg", "test1", "test2", "test123" };
    for (unsigned i = 0; i < sizeof W / sizeof *W; i++) if (!strcmp(k, W[i])) return true;
    return false;
}

static bool is_chatter(uint32_t i)
{
    const LpChat *c = &lp_chat[i];
    if (chatter_text(c->text)) return true;
    for (int j = (int)i - 1; j >= 0; j--) {
        if (c->time - lp_chat[j].time > 3 * 3600) break;
        if (lp_chat[j].from == c->from && !strcmp(lp_chat[j].text, c->text)) return true;
    }
    return false;
}

static int chat_channels(char out[][LP_CHAN_LEN], int max)
{
    int n = 0;
    for (uint32_t i = 0; i < lp_nChat && n < max; i++) {
        bool seen = false;
        for (int k = 0; k < n; k++) if (!strcmp(out[k], lp_chat[i].channel)) seen = true;
        if (!seen && lp_chat[i].channel[0]) snprintf(out[n++], LP_CHAN_LEN, "%s", lp_chat[i].channel);
    }
    return n;
}

static bool chat_shown(uint32_t i)
{
    const LpChat *c = &lp_chat[i];
    if (!(g_prefs->region == 0 || c->state == g_prefs->region)) return false;
    if (g_hideChatter && is_chatter(i)) return false;
    if (g_chanSel >= 0) {
        char ch[8][LP_CHAN_LEN];
        int nc = chat_channels(ch, 8);
        if (g_chanSel < nc && strcmp(c->channel, ch[g_chanSel])) return false;
    }
    return true;
}

static const char *region_name(int region)
{
    if (region <= 0 || !lp_haveGeo || region > lp_geo.count) return "All US";
    return lp_geo.st[region - 1].name;
}

static bool in_region(int st)
{
    return g_prefs->region == 0 || st == g_prefs->region;
}

static void ago(uint32_t t, char *out, size_t n)
{
    uint32_t now = (uint32_t)time(NULL), d = now > t ? now - t : 0;
    if (!t) snprintf(out, n, "never");
    else if (d < 60) snprintf(out, n, "now");
    else if (d < 3600) snprintf(out, n, "%um ago", d / 60);
    else if (d < 86400) snprintf(out, n, "%uh ago", d / 3600);
    else snprintf(out, n, "%ud ago", d / 86400);
}

/* How a node looks by how recently it was heard. */
static uint32_t heard_colour(uint32_t lastHeard)
{
    uint32_t d = (uint32_t)time(NULL) - lastHeard;
    if (d < 15) return 0xC8FFD8;                  /* just now */
    if (d < 900) return C_MESH;
    if (d < 3600) return 0x3FC6B8;
    if (d < 86400) return 0x2F7FC0;
    return 0x4A5378;
}

static int cmp_heard(const void *a, const void *b)
{
    uint32_t x = lp_nodes[*(const uint32_t *)a].lastHeard, y = lp_nodes[*(const uint32_t *)b].lastHeard;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* The region's nodes, newest first. */
static void build_order(void)
{
    free(g_order);
    g_order = lp_nNodes ? malloc(sizeof *g_order * lp_nNodes) : NULL;
    g_nOrder = 0;
    for (uint32_t i = 0; g_order && i < lp_nNodes; i++)
        if (in_region(lp_node_state(&lp_nodes[i])) && matches(&lp_nodes[i])) g_order[g_nOrder++] = i;
    if (g_nOrder > 1) qsort(g_order, g_nOrder, sizeof *g_order, cmp_heard);
    if (g_listSel >= (int)g_nOrder) g_listSel = g_nOrder ? (int)g_nOrder - 1 : 0;
}

/* The land for this region, filled per pixel from the projected state outlines. */
static void build_land(void)
{
    if (g_landFor == g_prefs->region) return;
    g_landFor = g_prefs->region;
    memset(g_landAt, 0, sizeof g_landAt);
    if (!lp_haveGeo) return;
    geo_view(&g_view, &lp_geo, g_prefs->region, 0, 0, MAP_PW, MAP_PH);
    static float xs[8192], ys[8192];
    for (int s = 0; s < lp_geo.count; s++) {
        int pane = geo_pane_for_state(&g_view, &lp_geo, s);
        if (g_prefs->region && s != g_prefs->region - 1) {
            /* A state view shows its neighbours too, faintly: project everything with its one pane. */
            pane = 0;
        }
        const GeoPane *p = &g_view.pane[pane];
        const GeoState *st = &lp_geo.st[s];
        for (int y = 0; y < MAP_PH; y++) {
            float py = y + 0.5f;
            float cross[256];
            int nc = 0;
            for (int r = st->firstRing; r < st->firstRing + st->rings; r++) {
                int n = lp_geo.ringLen[r];
                if (n > 8192) continue;
                const GeoPt *pt = lp_geo.pts + lp_geo.ringStart[r];
                for (int k = 0; k < n; k++) geo_to_screen(p, pt[k].lat, pt[k].lon, &xs[k], &ys[k]);
                for (int a = 0, b = n - 1; a < n && nc < 256; b = a++)
                    if ((ys[a] > py) != (ys[b] > py))
                        cross[nc++] = xs[a] + (py - ys[a]) * (xs[b] - xs[a]) / (ys[b] - ys[a]);
            }
            for (int i = 1; i < nc; i++)
                for (int j = i; j > 0 && cross[j - 1] > cross[j]; j--) { float t = cross[j]; cross[j] = cross[j - 1]; cross[j - 1] = t; }
            for (int i = 0; i + 1 < nc; i += 2)
                for (int x = (int)ceilf(cross[i] - 0.5f); x < (int)ceilf(cross[i + 1] - 0.5f); x++)
                    if (x >= 0 && x < MAP_PW) {
                        /* In the All US view each inset only draws its own state. */
                        if (!g_prefs->region && pane > 0) {
                            const GeoPane *ip = &g_view.pane[pane];
                            if (x < ip->x || x >= ip->x + ip->w || y < ip->y || y >= ip->y + ip->h) continue;
                        }
                        g_landAt[y * MAP_PW + x] = (uint8_t)(s + 1);
                    }
        }
    }
}

/* ----------------------------------------------------------- drawing --- */

static void draw_header(void)
{
    fill(0, 0, 1, COLS, C_BAR);
    int c = put(0, 1, "Meshtastic", C_MESH, C_BAR);
    c = put(0, c + 1, "Listening Post", C_TEXT, C_BAR);
    putraw(0, c + 1, "\xB3", C_EDGE, C_BAR);
    char reg[40];
    snprintf(reg, sizeof reg, "%s", region_name(g_prefs->region));
    put(0, c + 3, reg, C_TEXT2, C_BAR);
    char st[48];
    uint32_t col;
    switch (lp_feed_state()) {
    case LP_FEED_LIVE:    snprintf(st, sizeof st, "\xFE LIVE %u/min", lp_head.pktPerMin); col = C_MESH; break;
    case LP_FEED_OFFLINE: snprintf(st, sizeof st, "\xFE OFFLINE"); col = C_ERR; break;
    default:              snprintf(st, sizeof st, "\xFE LISTENING"); col = C_WARN; break;
    }
    int w = (int)strlen(st);
    putraw(0, COLS - 1 - w, st, col, C_BAR);
}

static void draw_map(void)
{
    build_land();
    static uint32_t px[MAP_PW * MAP_PH];
    int sel = g_prefs->region;
    for (int i = 0; i < MAP_PW * MAP_PH; i++) {
        int s = g_landAt[i];
        px[i] = !s ? C_SEA : (!sel || s == sel) ? C_LAND : mix(C_LAND, C_SEA, 0.55f);
    }
    /* State lines, faintly: land pixels next to a different state (the coast needs no line). At this size a
     * brighter line would outweigh the land itself. */
    for (int y = 0; y < MAP_PH; y++)
        for (int x = 0; x < MAP_PW; x++) {
            int s = g_landAt[y * MAP_PW + x];
            if (!s) continue;
            int r = x + 1 < MAP_PW ? g_landAt[y * MAP_PW + x + 1] : 0, d = y + 1 < MAP_PH ? g_landAt[(y + 1) * MAP_PW + x] : 0;
            if ((r && r != s) || (d && d != s)) px[y * MAP_PW + x] = (!sel || s == sel) ? C_LAND_HI : mix(C_LAND_HI, C_SEA, 0.55f);
        }
    /* The nodes: the freshest one on each pixel sets its colour. */
    static uint32_t best[MAP_PW * MAP_PH];
    memset(best, 0, sizeof best);
    for (uint32_t i = 0; i < lp_nNodes; i++) {
        const LpNode *n = &lp_nodes[i];
        if (!(n->flags & LP_NODE_POS)) continue;
        int st = lp_node_state(n);
        if (!in_region(st)) continue;
        float sx, sy;
        if (!lp_haveGeo || !geo_project(&g_view, &lp_geo, st - 1, n->lat1e5 / 1e5f, n->lon1e5 / 1e5f, &sx, &sy)) continue;
        int x = (int)sx, y = (int)sy;
        if (x < 0 || y < 0 || x >= MAP_PW || y >= MAP_PH) continue;
        if (n->lastHeard > best[y * MAP_PW + x]) best[y * MAP_PW + x] = n->lastHeard;
    }
    for (int i = 0; i < MAP_PW * MAP_PH; i++)
        if (best[i]) px[i] = heard_colour(best[i]);
    for (int r = 0; r < MAP_PH / 2; r++)
        for (int x = 0; x < MAP_PW; x++)
            setch(MAP_R + r, x, 0xDF, px[(r * 2) * MAP_PW + x], px[(r * 2 + 1) * MAP_PW + x]);
    /* The key, bottom left of the map. */
    if (!g_prefs->region) return;
    put(MAP_R + MAP_PH / 2 - 1, 1, region_name(g_prefs->region), C_TEXT3, KEEP);
}

static void draw_side(void)
{
    fill(MAP_R, SIDE_C - 1, MAP_PH / 2, SIDE_W + 2, C_PANEL);
    uint32_t now = (uint32_t)time(NULL);
    uint32_t hour = 0, day = 0, week = 0, pos = 0;
    int perState[GEO_MAX_STATES + 1] = { 0 };
    int perHw[256] = { 0 };
    for (uint32_t i = 0; i < lp_nNodes; i++) {
        const LpNode *n = &lp_nodes[i];
        int st = lp_node_state(n);
        if (now - n->lastHeard < 86400 && st <= GEO_MAX_STATES) perState[st]++;
        if (!in_region(st)) continue;
        week++;
        if (now - n->lastHeard < 86400) day++;
        if (now - n->lastHeard < 3600) hour++;
        if (n->flags & LP_NODE_POS) pos++;
        if (n->hw && now - n->lastHeard < 86400) perHw[n->hw]++;
    }
    int r = MAP_R;
    put(r++, SIDE_C, "NODES HEARD", C_ACCENT, C_PANEL);
    putf(r++, SIDE_C, C_TEXT, C_PANEL, "Last hour   %6u", hour);
    putf(r++, SIDE_C, C_TEXT, C_PANEL, "Today       %6u", day);
    char days[16];
    snprintf(days, sizeof days, "%d days", lp_config.nodeDays);
    putf(r++, SIDE_C, C_TEXT2, C_PANEL, "%-11s %6u", days, week);
    putf(r++, SIDE_C, C_TEXT3, C_PANEL, "On the map  %6u", pos);
    if (g_prefs->haveHome) {
        int near = 0;
        for (uint32_t i = 0; i < lp_nNodes; i++) {
            double km = node_km(&lp_nodes[i]);
            if (km >= 0 && km <= 80 && now - lp_nodes[i].lastHeard < 86400) near++;
        }
        putf(r++, SIDE_C, C_MESH, C_PANEL, "Near you    %6d", near);
    } else {
        r++;
    }
    if (!g_prefs->region) {
        put(r++, SIDE_C, "BUSIEST STATES, 24H", C_ACCENT, C_PANEL);
        for (int k = 0; k < 7; k++) {
            int bi = 0, bv = 0;
            for (int s = 1; s <= lp_geo.count && s <= GEO_MAX_STATES; s++)
                if (perState[s] > bv) { bv = perState[s]; bi = s; }
            if (!bv) break;
            perState[bi] = -perState[bi] - 1;          /* taken */
            putf(r++, SIDE_C, C_TEXT2, C_PANEL, "%-14.14s %5d", lp_geo.st[bi - 1].name, bv);
        }
    } else {
        put(r++, SIDE_C, "RADIOS, 24H", C_ACCENT, C_PANEL);
        for (int k = 0; k < 7; k++) {
            int bi = 0, bv = 0;
            for (int h = 1; h < 256; h++)
                if (perHw[h] > bv) { bv = perHw[h]; bi = h; }
            if (!bv) break;
            perHw[bi] = 0;
            putf(r++, SIDE_C, C_TEXT2, C_PANEL, "%-15.15s %4d", lp_hw_name((unsigned)bi), bv);
        }
    }
    /* The colour key along the panel's foot. */
    int kr = MAP_R + MAP_PH / 2 - 1;
    int c = SIDE_C;
    static const struct { uint32_t col; const char *k; } KEY[] = {
        { C_MESH, "15m" }, { 0x3FC6B8, "1h" }, { 0x2F7FC0, "24h" }, { 0x4A5378, "older" } };
    for (int i = 0; i < 4; i++) {
        setch(kr, c, 0xFE, KEY[i].col, C_PANEL);
        c = put(kr, c + 1, KEY[i].k, C_TEXT3, C_PANEL) + 1;
    }
}

static const char *chat_name(const LpChat *m)
{
    static char label[LP_NAME_LEN];
    lp_label(lp_find(m->from), m->from, label, sizeof label);
    return label;
}

static void chat_line(int r, const LpChat *m, int width)
{
    char when[16], line[320];
    time_t t = (time_t)m->time;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(when, sizeof when, "%H:%M", &tm);
    int c = put(r, 1, when, C_TEXT3, KEEP);
    const char *ab = m->state && lp_haveGeo && m->state <= lp_geo.count ? lp_geo.st[m->state - 1].abbr : "  ";
    c = put(r, c + 1, ab, C_TEXT3, KEEP);
    char name[24];
    snprintf(name, sizeof name, "%.18s", chat_name(m));
    c = put(r, c + 1, name, C_MESH, KEEP);
    snprintf(line, sizeof line, " %.*s", width - c > 0 ? width - c : 0, m->text);
    put(r, c, line, C_TEXT, KEEP);
}

static void draw_ticker(void)
{
    const int r0 = MAP_R + MAP_PH / 2;
    fill(r0, 0, 4, COLS, C_BG);
    put(r0, 1, "PUBLIC CHAT", C_ACCENT, C_BG);
    int shown = 0;
    for (int i = (int)lp_nChat - 1; i >= 0 && shown < 3; i--) {
        if (!chat_shown((uint32_t)i)) continue;
        chat_line(r0 + 3 - shown, &lp_chat[i], COLS - 1);
        shown++;
    }
    if (!shown) put(r0 + 2, 3, g_prefs->region ? "Nothing said here yet. Chat is rare: most radios only report in."
                                               : "Nothing said yet. Chat is rare: most radios only report in.", C_TEXT3, C_BG);
}

static void draw_nodes(void)
{
    const int r0 = MAP_R, rows = 18;
    fill(r0, 0, rows + 1, COLS, C_BG);
    count_links();
    putf(r0, 1, C_ACCENT, C_BG, "%-21s %-5s %-9s %-2s %-15s %-8s %-7s %s", "NODE", "SHORT", "ID", "", "RADIO",
         g_sensorsOnly ? "SENSOR" : "BATTERY", "HEARD", g_prefs->haveHome ? "  MI" : "LNKS");
    if (!g_nOrder) {
        put(r0 + 3, 3, g_filterLen ? "No node matches." : g_sensorsOnly ? "No sensors heard here yet." : "Nothing heard here yet.",
            C_TEXT3, C_BG);
        return;
    }
    if (g_listSel < g_listTop) g_listTop = g_listSel;
    if (g_listSel >= g_listTop + rows) g_listTop = g_listSel - rows + 1;
    for (int k = 0; k < rows && g_listTop + k < (int)g_nOrder; k++) {
        const LpNode *n = &lp_nodes[g_order[g_listTop + k]];
        bool on = g_listTop + k == g_listSel;
        uint32_t bg = on ? C_PANEL_HI : C_BG;
        if (on) fill(r0 + 1 + k, 0, 1, COLS, C_PANEL_HI);
        char heard[16], batt[12], name[22], shortName[8], id[12];
        ago(n->lastHeard, heard, sizeof heard);
        if (g_sensorsOnly && (n->flags & LP_NODE_ENV) && n->temp_dC != LP_NO_TEMP) {
            if (n->humidity != 255) snprintf(batt, sizeof batt, temp_f(n->temp_dC) > 99 || temp_f(n->temp_dC) < -9 ? "%dF%u%%" : "%dF %u%%",
                                             temp_f(n->temp_dC), n->humidity);
            else snprintf(batt, sizeof batt, "%dF", temp_f(n->temp_dC));
        }
        else if (n->battery == 255) snprintf(batt, sizeof batt, "-");
        else if (n->battery > 100) snprintf(batt, sizeof batt, "powered");
        else snprintf(batt, sizeof batt, "%u%%", n->battery);
        snprintf(name, sizeof name, "%.21s", n->name[0] ? n->name : "-");
        snprintf(shortName, sizeof shortName, "%.5s", n->shortName[0] ? n->shortName : "-");
        snprintf(id, sizeof id, "!%08x", n->id);
        int st = lp_node_state(n);
        const char *ab = st && lp_haveGeo && st <= lp_geo.count ? lp_geo.st[st - 1].abbr : "";
        putraw(r0 + 1 + k, 0, on ? "\xAF" : " ", C_ACCENT, bg);
        put(r0 + 1 + k, 1, name, n->name[0] ? (on ? C_TEXT : C_TEXT2) : C_TEXT3, bg);
        put(r0 + 1 + k, 23, shortName, n->shortName[0] ? C_MESH : C_TEXT3, bg);
        put(r0 + 1 + k, 29, id, C_TEXT3, bg);
        put(r0 + 1 + k, 39, ab, C_TEXT3, bg);
        char radio[16];
        snprintf(radio, sizeof radio, "%.15s", n->hw ? lp_hw_name(n->hw) : "-");
        put(r0 + 1 + k, 42, radio, C_TEXT2, bg);
        put(r0 + 1 + k, 58, batt, g_sensorsOnly ? 0xFFD166 : C_TEXT2, bg);
        char h7[8];
        snprintf(h7, sizeof h7, "%.7s", heard);
        put(r0 + 1 + k, 67, h7, heard_colour(n->lastHeard) == 0x4A5378 ? C_TEXT3 : heard_colour(n->lastHeard), bg);
        char last[8] = "";
        if (g_prefs->haveHome) {
            double km = node_km(n);
            if (km >= 0) snprintf(last, sizeof last, "%4.0f", km * 0.621371 > 9999 ? 9999 : km * 0.621371);
        } else if (g_linkCount && g_linkCount[g_order[g_listTop + k]]) {
            snprintf(last, sizeof last, "%4u", g_linkCount[g_order[g_listTop + k]]);
        }
        put(r0 + 1 + k, 75, last, C_TEXT3, bg);
    }
    char foot[120];
    snprintf(foot, sizeof foot, "%d of %u in %s%s%s%s", g_listSel + 1, g_nOrder, region_name(g_prefs->region),
             g_sensorsOnly ? "  \xb7  sensors only" : "", g_filterLen ? "  \xb7  matching " : "", g_filterLen ? g_filter : "");
    put(r0 + rows + 1, 1, foot, C_TEXT3, C_BG);
    put_r(r0 + rows + 1, COLS - 1, "/ find   E sensors   Esc clear", C_TEXT3, C_BG);
}

static void draw_chat_page(void)
{
    const int r0 = MAP_R, rows = 20;
    fill(r0, 0, rows + 1, COLS, C_BG);
    put(r0, 1, "PUBLIC CHAT", C_ACCENT, C_BG);
    char ch[8][LP_CHAN_LEN], hdr[80];
    int nc = chat_channels(ch, 8);
    snprintf(hdr, sizeof hdr, "%s  \xb7  %s", g_chanSel >= 0 && g_chanSel < nc ? ch[g_chanSel] : "all channels",
             g_hideChatter ? "test chatter hidden" : "showing test chatter");
    put_r(r0, COLS - 1, hdr, C_TEXT3, C_BG);
    int idx[2000], n = 0;
    for (uint32_t i = 0; i < lp_nChat && n < 2000; i++)
        if (chat_shown(i)) idx[n++] = (int)i;
    if (!n) {
        put(r0 + 3, 3, g_hideChatter ? "Nothing but test chatter here. Press F to show it."
                                     : "Nothing said yet. It can be quiet for a long while: most radios only report in.", C_TEXT3, C_BG);
        put_r(r0 + rows - 1, COLS - 1, "F test chatter   Tab channel", C_TEXT3, C_BG);
        return;
    }
    if (g_chatBack > n - (rows - 2)) g_chatBack = n - (rows - 2) > 0 ? n - (rows - 2) : 0;
    int last = n - 1 - g_chatBack;
    for (int k = 0; k < rows - 2 && last - k >= 0; k++) chat_line(r0 + rows - 2 - k, &lp_chat[idx[last - k]], COLS - 1);
    put_r(r0 + rows - 1, COLS - 1, "F test chatter   Tab channel   Up/Dn scroll", C_TEXT3, C_BG);
}

/* The mesh in numbers: traffic over three hours, the radios, the states, the busiest nodes. */
static void draw_stats_page(void)
{
    const int r0 = MAP_R;
    fill(r0, 0, 21, COLS, C_BG);
    put(r0, 1, "THE MESH IN NUMBERS", C_ACCENT, C_BG);
    put(r0, 21, region_name(g_prefs->region), C_TEXT3, C_BG);
    /* Traffic: 60 columns of 3 minutes, 5 rows of half blocks. */
    int gmax = 1, cols = 60;
    int v[60] = { 0 };
    for (int c = 0; c < cols; c++) {
        int sum = 0, k = 0;
        for (int m = c * 3; m < c * 3 + 3; m++) {
            int at = (int)lp_histCount - LP_HIST_MIN + m;
            if (at >= 0 && at < (int)lp_histCount) { sum += lp_hist[at]; k++; }
        }
        v[c] = k ? sum / k : 0;
        if (v[c] > gmax) gmax = v[c];
    }
    putf(r0 + 1, 1, C_TEXT3, C_BG, "Packets per minute, all US, last 3 hours%s", lp_histCount ? "" : ": fills in while it runs");
    if (lp_histCount) putf(r0 + 1, 62, C_TEXT3, C_BG, "peak %d/min", gmax);
    for (int c = 0; c < cols; c++) {
        int h = v[c] * 10 / gmax;                          /* 0-10 half cells */
        for (int row = 0; row < 5; row++) {
            int lo = (4 - row) * 2;                        /* half cells below this row */
            int fillH = h - lo;
            unsigned char ch = fillH >= 2 ? 0xDB : fillH == 1 ? 0xDC : ' ';
            setch(r0 + 2 + row, 2 + c, ch, C_MESH, 0x0A1322);
        }
    }
    /* Counts over the region. */
    uint32_t now = (uint32_t)time(NULL);
    int perHw[256] = { 0 }, perState[GEO_MAX_STATES + 1] = { 0 }, sensors = 0, day = 0, week = 0;
    for (uint32_t i = 0; i < lp_nNodes; i++) {
        const LpNode *n = &lp_nodes[i];
        int st = lp_node_state(n);
        if (!in_region(st)) continue;
        uint32_t age = now - n->lastHeard;
        week++;
        if (st <= GEO_MAX_STATES) perState[st]++;
        if (age < 86400) { day++; if (n->hw) perHw[n->hw]++; }
        if (n->flags & LP_NODE_ENV) sensors++;
    }
    int r = r0 + 8;
    put(r, 1, "RADIOS TODAY", C_ACCENT, C_BG);
    put(r, 28, g_prefs->region ? "MOST ACTIVE HERE" : "BUSIEST STATES, WEEK", C_ACCENT, C_BG);
    put(r, 55, g_prefs->region ? "" : "MOST ACTIVE NODES", C_ACCENT, C_BG);
    for (int k = 0; k < 9; k++) {
        int bi = 0, bv = 0;
        for (int h = 1; h < 256; h++) if (perHw[h] > bv) { bv = perHw[h]; bi = h; }
        if (!bv) break;
        perHw[bi] = 0;
        putf(r + 1 + k, 1, C_TEXT2, C_BG, "%-18.18s %5d", lp_hw_name((unsigned)bi), bv);
    }
    if (!g_prefs->region) {
        for (int k = 0; k < 9; k++) {
            int bi = 0, bv = 0;
            for (int st = 1; st <= lp_geo.count && st <= GEO_MAX_STATES; st++) if (perState[st] > bv) { bv = perState[st]; bi = st; }
            if (!bv) break;
            perState[bi] = 0;
            putf(r + 1 + k, 28, C_TEXT2, C_BG, "%-18.18s %5d", lp_geo.st[bi - 1].name, bv);
        }
    }
    /* The nodes with the most packets heard. */
    int col = g_prefs->region ? 28 : 55;
    uint32_t taken[9];
    int nt = 0;
    for (int k = 0; k < 9; k++) {
        int bi = -1;
        uint32_t bv = 0;
        for (uint32_t i = 0; i < lp_nNodes; i++) {
            const LpNode *n = &lp_nodes[i];
            if (!in_region(lp_node_state(n)) || n->packets <= bv) continue;
            bool dup = false;
            for (int t = 0; t < nt; t++) if (taken[t] == i) dup = true;
            if (!dup) { bv = n->packets; bi = (int)i; }
        }
        if (bi < 0) break;
        taken[nt++] = (uint32_t)bi;
        char lab[LP_NAME_LEN];
        lp_label(&lp_nodes[bi], lp_nodes[bi].id, lab, sizeof lab);
        putf(r + 1 + k, col, C_TEXT2, C_BG, "%-16.16s %6u", lab, bv);
    }
    count_links();
    uint32_t links = 0;
    for (uint32_t i = 0; i < lp_nLinks; i++) if (now - lp_links[i].lastHeard < 86400) links++;
    char tot[120];
    snprintf(tot, sizeof tot, "%d heard today  \xb7  %d in %d days  \xb7  %u links heard  \xb7  %d sensors",
             day, week, lp_config.nodeDays, links, sensors);
    put(r0 + 20, 1, tot, C_TEXT, C_BG);
}

static void draw_keys(void)
{
    fill(ROWS - 2, 0, 1, COLS, C_BG);
    static const struct { int page; const char *k, *what; } K[] = {
        { PAGE_MAP, "M", "Map" }, { PAGE_NODES, "N", "Nodes" }, { PAGE_CHAT, "C", "Chat" }, { PAGE_STATS, "S", "Stats" },
        { -1, "R", "Region" }, { -3, "H", "Home" }, { -2, "Q", "Quit" } };
    int c = 1;
    for (int i = 0; i < 7; i++) {
        bool on = K[i].page == g_page;
        c = put(ROWS - 2, c, " ", C_TEXT, on ? C_MESH : C_PANEL_HI);
        c = put(ROWS - 2, c, K[i].k, on ? 0x000000 : C_TEXT, on ? C_MESH : C_PANEL_HI);
        c = put(ROWS - 2, c, " ", C_TEXT, on ? C_MESH : C_PANEL_HI);
        c = put(ROWS - 2, c + 1, K[i].what, on ? C_TEXT : C_TEXT3, C_BG) + 1;
    }
    (void)c;
    /* The bottom line: where the data stands. */
    fill(ROWS - 1, 0, 1, COLS, C_BAR);
    char msg[160], age[16];
    ago(lp_head.savedAt, age, sizeof age);
    int feed = lp_feed_state();
    uint32_t col = C_TEXT3;
    if (g_typing)
        { snprintf(msg, sizeof msg, "Find: %s_   (name, short name or ID; Enter to keep, Esc to clear)", g_filter); col = C_MESH; }
    else if (g_note[0] && time(NULL) < g_noteUntil)
        { snprintf(msg, sizeof msg, "%s", g_note); col = C_MESH; }
    else if (lp_firstEver && !lp_nNodes)
        { snprintf(msg, sizeof msg, "Listening... the map fills in as radios are heard. That takes a few minutes."); col = C_WARN; }
    else if (feed == LP_FEED_OFFLINE)
        { snprintf(msg, sizeof msg, "Can't reach the mesh right now. Showing what was heard until %s.", age); col = C_ERR; }
    else if (feed != LP_FEED_LIVE)
        { snprintf(msg, sizeof msg, "Listening to the mesh... Showing what was heard until %s.", age); col = C_WARN; }
    else if (time(NULL) - lp_head.startedAt < 180)
        { snprintf(msg, sizeof msg, "Live. New traffic is arriving: the map fills in over a few minutes."); col = C_WARN; }
    else
        snprintf(msg, sizeof msg, "Live from the public Meshtastic feed. Positions rounded to about 2 km.");
    put(ROWS - 1, 1, msg, col, C_BAR);
}

/* The region picker: a box of All US + the states, four columns. Returns the
 * region chosen, or -1. */
enum { K_NONE = -1, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_ENTER, K_ESC, K_BACK };

static int read_key(int timeout_ms)
{
    int c = door_read_char_timeout(timeout_ms);
    if (c < 0) return K_NONE;
    if (c == 27) {
        int c2 = door_read_char_timeout(60);
        if (c2 < 0) return K_ESC;
        if (c2 == '[' || c2 == 'O') {
            int c3 = door_read_char_timeout(60);
            switch (c3) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            default: {
                int num = 0;
                while (c3 >= '0' && c3 <= '9') { num = num * 10 + (c3 - '0'); c3 = door_read_char_timeout(60); }
                while (c3 >= 0 && c3 != '~' && !(c3 >= 'A' && c3 <= 'Z') && !(c3 >= 'a' && c3 <= 'z'))
                    c3 = door_read_char_timeout(60);
                if (c3 == '~') {
                    if (num == 5) return K_PGUP;
                    if (num == 6) return K_PGDN;
                    if (num == 1 || num == 7) return K_HOME;
                    if (num == 4 || num == 8) return K_END;
                }
                return K_NONE;
            }
            }
        }
        return K_ESC;
    }
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 8 || c == 127) return K_BACK;
    return c;
}

static void draw_all(void);

static int pick_region(void)
{
    int n = lp_haveGeo ? lp_geo.count + 1 : 1, sel = g_prefs->region;
    const int r0 = 2, c0 = 3, h = 19, w = 73, perCol = 14;
    for (;;) {
        draw_all();
        fill(r0, c0, h, w, 0x14203E);
        for (int x = c0; x < c0 + w; x++) setch(r0, x, 0xDF, C_MESH, 0x14203E);
        put(r0 + 1, c0 + 2, "Choose a region", C_TEXT, KEEP);
        put(r0 + 1, c0 + 19, "(nodes are placed by where they are)", C_TEXT3, KEEP);
        for (int i = 0; i < n; i++) {
            int col = i / perCol, row = i % perCol;
            int x = c0 + 2 + col * 18, y = r0 + 3 + row;
            bool on = i == sel;
            char label[24];
            if (!i) snprintf(label, sizeof label, "   All US");
            else snprintf(label, sizeof label, "%s %.13s", lp_geo.st[i - 1].abbr, lp_geo.st[i - 1].name);
            if (on) fill(y, x - 1, 1, 18, C_PANEL_HI);
            put(y, x, label, on ? C_TEXT : C_TEXT2, on ? C_PANEL_HI : KEEP);
        }
        int c = put(r0 + h - 1, c0 + 2, " Arrows ", C_TEXT, C_PANEL_HI);
        c = put(r0 + h - 1, c + 1, "move", C_TEXT3, KEEP);
        c = put(r0 + h - 1, c + 3, " A-Z ", C_TEXT, C_PANEL_HI);
        c = put(r0 + h - 1, c + 1, "jump", C_TEXT3, KEEP);
        c = put(r0 + h - 1, c + 3, " Enter ", C_TEXT, C_PANEL_HI);
        c = put(r0 + h - 1, c + 1, "choose", C_TEXT3, KEEP);
        c = put(r0 + h - 1, c + 3, " Esc ", C_TEXT, C_PANEL_HI);
        put(r0 + h - 1, c + 1, "cancel", C_TEXT3, KEEP);
        flush();
        int k = read_key(300000);
        if (k == K_NONE || k == K_ESC || k == 'q' || k == 'Q') return -1;
        if (k == K_ENTER) return sel;
        if (k == K_UP && sel > 0) sel--;
        else if (k == K_DOWN && sel < n - 1) sel++;
        else if (k == K_LEFT && sel - perCol >= 0) sel -= perCol;
        else if (k == K_RIGHT && sel + perCol < n) sel += perCol;
        else if (k == K_HOME) sel = 0;
        else if (k == K_END) sel = n - 1;
        else if (k < 127 && isalpha(k)) {
            /* The next state starting with that letter, after the current one. */
            int up = toupper(k);
            for (int step = 1; step <= n; step++) {
                int i = (sel + step) % n;
                if (i && toupper((unsigned char)lp_geo.st[i - 1].name[0]) == up) { sel = i; break; }
            }
        }
    }
}

static void draw_all(void)
{
    clear(C_BG);
    draw_header();
    if (g_page == PAGE_MAP) {
        draw_map();
        draw_side();
        draw_ticker();
    } else if (g_page == PAGE_NODES) {
        draw_nodes();
    } else if (g_page == PAGE_STATS) {
        draw_stats_page();
    } else {
        draw_chat_page();
    }
    draw_keys();
}

/* -------------------------------------------------------------- main --- */

void ansi_lp_run(LpUserPrefs *prefs)
{
    g_prefs = prefs;
    invalidate();
    door_write(CSI "0m" CSI "2J" CSI "?25l");
    build_order();
    time_t lastPoll = time(NULL);
    for (;;) {
        draw_all();
        flush();
        int k = read_key(1000);
        time_t now = time(NULL);
        lp_touch();
        if (now - lastPoll >= 2) {
            lastPoll = now;
            LpChanges c = lp_refresh();
            if (c.nodes && g_page == PAGE_NODES) build_order();
        }
        if (k == K_NONE) {
            if (door_time_remaining() <= 0) break;
            continue;
        }
        if (g_typing) {
            /* Typing into the Nodes page's find. */
            if (k == K_ENTER) g_typing = 0;
            else if (k == K_ESC) { g_typing = 0; g_filterLen = 0; g_filter[0] = 0; }
            else if (k == K_BACK) { if (g_filterLen) g_filter[--g_filterLen] = 0; }
            else if (k > 31 && k < 127 && g_filterLen < (int)sizeof g_filter - 1 && (isalnum(k) || k == ' ' || k == '!' || k == '-' || k == '.')) {
                g_filter[g_filterLen++] = (char)k;
                g_filter[g_filterLen] = 0;
            }
            g_listSel = g_listTop = 0;
            build_order();
            continue;
        }
        if (k == 'q' || k == 'Q') break;
        if (k == K_ESC) {
            if (g_page == PAGE_NODES && (g_filterLen || g_sensorsOnly)) {
                g_filterLen = 0; g_filter[0] = 0; g_sensorsOnly = false; build_order();
                continue;
            }
            if (g_page != PAGE_MAP) { g_page = PAGE_MAP; continue; }
            break;
        }
        switch (k) {
        case 'm': case 'M': g_page = PAGE_MAP; break;
        case 'n': case 'N': g_page = PAGE_NODES; build_order(); break;
        case 'c': case 'C': g_page = PAGE_CHAT; g_chatBack = 0; break;
        case 's': case 'S': g_page = PAGE_STATS; break;
        case '/':
            g_page = PAGE_NODES;
            g_typing = 1;
            g_filterLen = 0;
            g_filter[0] = 0;
            g_listSel = g_listTop = 0;
            build_order();
            break;
        case 'e': case 'E':
            g_page = PAGE_NODES;
            g_sensorsOnly = !g_sensorsOnly;
            g_listSel = g_listTop = 0;
            build_order();
            note(g_sensorsOnly ? "Sensors only: temperature (F) and humidity. E again for every node." : "Every node again.");
            break;
        case 'f': case 'F':
            if (g_page == PAGE_CHAT) { g_hideChatter = !g_hideChatter; g_chatBack = 0; }
            break;
        case '\t':
            if (g_page == PAGE_CHAT) {
                char ch[8][LP_CHAN_LEN];
                int nc = chat_channels(ch, 8);
                g_chanSel = g_chanSel + 1 >= nc ? -1 : g_chanSel + 1;
                g_chatBack = 0;
            }
            break;
        case 'h': case 'H': {
            if (!prefs->haveHome) { note("Set your town in the Weather door and your home shows up here."); break; }
            if (prefs->homeState && prefs->region != prefs->homeState) {
                prefs->region = prefs->homeState;
                lp_save_prefs(prefs);
                build_order();
            }
            int near = 0;
            uint32_t tnow = (uint32_t)time(NULL);
            for (uint32_t i = 0; i < lp_nNodes; i++) {
                double km = node_km(&lp_nodes[i]);
                if (km >= 0 && km <= 80 && tnow - lp_nodes[i].lastHeard < 86400) near++;
            }
            char m[120];
            snprintf(m, sizeof m, "%.40s: %d radio%s within 50 miles heard today", prefs->homeName, near, near == 1 ? "" : "s");
            note(m);
            break;
        }
        case 'r': case 'R': {
            int r = pick_region();
            if (r >= 0 && r != prefs->region) {
                prefs->region = r;
                lp_save_prefs(prefs);
                g_listSel = g_listTop = g_chatBack = 0;
                build_order();
            }
            invalidate();
            door_write(CSI "0m" CSI "2J");
            break;
        }
        case K_UP:
            if (g_page == PAGE_NODES && g_listSel > 0) g_listSel--;
            else if (g_page == PAGE_CHAT) g_chatBack++;
            break;
        case K_DOWN:
            if (g_page == PAGE_NODES && g_listSel < (int)g_nOrder - 1) g_listSel++;
            else if (g_page == PAGE_CHAT && g_chatBack > 0) g_chatBack--;
            break;
        case K_PGUP:
            if (g_page == PAGE_NODES) { g_listSel -= 18; if (g_listSel < 0) g_listSel = 0; }
            else if (g_page == PAGE_CHAT) g_chatBack += 18;
            break;
        case K_PGDN:
            if (g_page == PAGE_NODES) { g_listSel += 18; if (g_listSel >= (int)g_nOrder) g_listSel = g_nOrder ? (int)g_nOrder - 1 : 0; }
            else if (g_page == PAGE_CHAT) { g_chatBack -= 18; if (g_chatBack < 0) g_chatBack = 0; }
            break;
        case K_HOME: if (g_page == PAGE_NODES) g_listSel = 0; break;
        case K_END:  if (g_page == PAGE_NODES && g_nOrder) g_listSel = (int)g_nOrder - 1; break;
        default: break;
        }
    }
    free(g_order);
    g_order = NULL;
    door_write(CSI "0m" CSI "2J" CSI "H" CSI "?25h");
}
