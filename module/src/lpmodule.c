// The Meshtastic Listening Post TRACE module: the picture half of the door.
//
// A live map of the US (or one state) with every Meshtastic node the public
// feed has heard, coloured by how recently, pulsing as new packets arrive; the
// chosen node's details; the latest public chat; and the numbers. Zoom and pan
// the map, pick a region, read the whole chat. Every action has a key, and
// anything a key does can be clicked.
//
// The door sends the nodes, chat and feed status (see protocol.h); the state
// outlines arrive once as an asset (states.bin, cached by TERMinator).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_api.h"
#include "protocol.h"
#include "gfx.h"
#include "geo.h"
#include "hwmodels.h"

// ---------------------------------------------------------------- colours ---

#define C_BG       0x050A12
#define C_BAR      0x0B1322
#define C_PANEL    0x0F1A2C
#define C_PANEL_HI 0x19294A
#define C_EDGE     0x22324F
#define C_EDGE_HI  0x37507C
#define C_TEXT     0xEAF2F4
#define C_TEXT2    0x9DB0C2
#define C_TEXT3    0x5D6E88
#define C_MESH     0x67EA94
#define C_ACCENT   0x4FD1FF
#define C_WARN     0xFFB547
#define C_ERR      0xFF6B6B
#define C_SEA      0x07101D
#define C_LAND     0x132338
#define C_LAND_DIM 0x0D1829
#define C_LINE     0x2C4668
#define C_LINE_DIM 0x172538

// ---------------------------------------------------------------- layout ---

#define BAR_H   28
#define MAP_X   0
#define MAP_Y   29
#define MAP_W   640
#define MAP_H   303
#define FOOT_Y  (MAP_Y + MAP_H)          /* the key and hints, under the map */
#define PANEL_Y 352
#define NODE_X  6
#define NODE_W  200
#define CHAT_X  212
#define CHAT_W  264
#define STAT_X  482
#define STAT_W  152
#define PANEL_H 122

typedef struct { float x, y, w, h; } Rect;
static int inside(Rect r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static const Rect R_CLOSE = { 610, 4, 24, 20 };
static Rect g_regionBtn = { 250, 4, 160, 20 };
static const Rect R_CHATPANEL = { CHAT_X, PANEL_Y, CHAT_W, PANEL_H };
static const Rect R_ZIN = { MAP_X + MAP_W - 34, MAP_Y + 10, 24, 24 };
static const Rect R_ZOUT = { MAP_X + MAP_W - 34, MAP_Y + 36, 24, 24 };
static const Rect R_ZRESET = { MAP_X + MAP_W - 34, MAP_Y + 62, 24, 24 };
static const Rect R_ZHOME = { MAP_X + MAP_W - 34, MAP_Y + 88, 24, 24 };
static Rect g_chipLinks = { MAP_X + 8, MAP_Y + 8, 60, 18 }, g_chipSensors = { MAP_X + 72, MAP_Y + 8, 70, 18 };
static const Rect R_STATSPANEL = { STAT_X, PANEL_Y, STAT_W, PANEL_H };
static const Rect R_STATSBOX = { 24, 40, 592, 412 };
static const Rect R_FINDBOX = { 150, 60, 340, 330 };
static const Rect R_PICKER = { 36, 44, 568, 392 };
static const Rect R_CHATBOX = { 24, 40, 592, 412 };

// ------------------------------------------------------------------ state ---

typedef struct {
    LpNodeWire w;
    float    bx, by;                // base (unzoomed) map position, logical px
    int      onMap;                 // has a position the current view shows
    int32_t  pulseAt;               // trace_time_ms() when an update arrived, 0 = none
} Node;

static Node    *g_nodes = NULL;
static int      g_n = 0, g_cap = 0;
static int32_t *g_idHash = NULL;    // node index by id
static int      g_idHashCap = 0;

// Radios that heard each other directly, and each end's node index (-1 = not known yet).
typedef struct { LpLinkWire w; int ia, ib; } Link;
static Link    *g_links = NULL;
static int      g_nLinks = 0, g_capLinks = 0, g_linksDirty = 1;
static int32_t *g_linkHash = NULL;  // link index by its two ends
static int      g_linkHashCap = 0;
static int      g_showLinks = 1, g_showSensors = 0;

// Traffic: unique packets per minute, oldest first.
static uint16_t g_hist[LP_HIST_MIN];
static int      g_histCount = 0;
static uint32_t g_histEnd = 0;

// The caller's town, from the Weather door.
static int      g_haveHome = 0, g_homeState = 0;
static float    g_homeLat, g_homeLon;
static char     g_homeName[40];

// A short message over the map.
static char     g_toast[120];
static int32_t  g_toastAt = -100000;
static int      g_sel = -1, g_hover = -1, g_haveAll = 0;
static int      g_projFor = -1;     // the region the bx/by were worked out for

typedef struct { uint32_t time; uint8_t state; char name[LP_WIRE_NAME]; char chan[20]; char text[240]; } Chat;
static Chat    *g_chat = NULL;
static int      g_nChat = 0, g_capChat = 0, g_chatBuild = 0;

static LpStats  g_stats;
static uint32_t g_doorNow = 0;
static int32_t  g_recvMs = 0;
static int      g_region = 0, g_nodeDays = 7;

static GeoMap   g_geo;
static int      g_haveGeo = 0;
static char     g_statesHash[65];
static GeoView  g_view;

// View: zoom and pan over the base projection.
static float    g_zoom = 1, g_panX = 0, g_panY = 0;
static int      g_dragging = 0, g_pressed = 0, g_pressedNode = 0;
static float    g_pressX, g_pressY, g_pressPanX, g_pressPanY;

// Overlays.
static int      g_picker = 0, g_pickSel = 0, g_chatOpen = 0, g_chatBack = 0;
static int      g_statsOpen = 0, g_helpOpen = 0;
static int      g_findOpen = 0, g_findLen = 0, g_findSel = 0, g_nFound = 0;
static char     g_find[24];
static int      g_found[12];
static int      g_hideChatter = 1, g_chanSel = -1;   // chat filter: -1 = every channel

static int      g_quitting = 0, g_started = 0;
static float    g_mx = -1, g_my = -1;
static int      g_mouseOver = 0, g_shift = 0;

#ifdef LP_DEMO
static void demo_fill(void);
#endif
static void resolve_links(void);
static int links_of(int node);
static float chip(float x, float y, const char *label, int on, Rect *out);

static uint32_t now_door(void)
{
    if (!g_doorNow) return 0;
    return g_doorNow + (uint32_t)((trace_time_ms() - g_recvMs) / 1000);
}

// ------------------------------------------------------------- door link ---

static void send_msg(uint8_t type, const void *payload, uint32_t len)
{
    uint8_t buf[sizeof(LpMsgHeader) + 16];
    if (len > 16) return;
    LpMsgHeader h = { type, 0, 1, len };
    memcpy(buf, &h, sizeof h);
    if (payload && len) memcpy(buf + sizeof h, payload, len);
    uint32_t total = sizeof h + len;
    if ((uint32_t)trace_send_room() >= total) trace_send(buf, (int32_t)total);
}

// ------------------------------------------------------------ helpers ---

static const char *region_name(int r)
{
    if (r <= 0 || !g_haveGeo || r > g_geo.count) return "All US";
    return g_geo.st[r - 1].name;
}

static const char *state_abbr(int st)
{
    if (st <= 0 || !g_haveGeo || st > g_geo.count) return "";
    return g_geo.st[st - 1].abbr;
}

static int in_region(int st) { return g_region == 0 || st == g_region; }

static void ago(uint32_t t, char *out, size_t n)
{
    uint32_t now = now_door(), d = now > t ? now - t : 0;
    if (!t) snprintf(out, n, "never");
    else if (d < 60) snprintf(out, n, "just now");
    else if (d < 3600) snprintf(out, n, "%u min ago", d / 60);
    else if (d < 86400) snprintf(out, n, "%u h ago", d / 3600);
    else snprintf(out, n, "%u d ago", d / 86400);
}

static uint32_t heard_colour(uint32_t lastHeard)
{
    uint32_t d = now_door() - lastHeard;
    if (d < 900) return C_MESH;
    if (d < 3600) return 0x3FC6B8;
    if (d < 86400) return 0x3A86D0;
    return 0x55608A;
}

// What to call a node: its long name, else its short name, else its number (!1a2b3c4d).
static const char *node_label(const LpNodeWire *w)
{
    static char buf[4][32];
    static int k = 0;
    char *out = buf[k++ & 3];
    if (w->name[0]) snprintf(out, 32, "%s", w->name);
    else if (w->shortName[0]) snprintf(out, 32, "%s", w->shortName);
    else snprintf(out, 32, "!%08x", w->id);
    return out;
}

static uint32_t hash_id(uint32_t id) { id ^= id >> 16; id *= 0x7feb352d; id ^= id >> 15; id *= 0x846ca68b; id ^= id >> 16; return id; }

static void id_rehash(void)
{
    int cap = 8192;
    while (cap < g_n * 2 + 16) cap *= 2;
    int32_t *h = malloc(sizeof *h * (size_t)cap);
    if (!h) return;
    for (int i = 0; i < cap; i++) h[i] = -1;
    for (int i = 0; i < g_n; i++) {
        uint32_t k = hash_id(g_nodes[i].w.id) & (uint32_t)(cap - 1);
        while (h[k] >= 0) k = (k + 1) & (uint32_t)(cap - 1);
        h[k] = i;
    }
    free(g_idHash);
    g_idHash = h;
    g_idHashCap = cap;
}

static int node_index(uint32_t id)
{
    if (!g_idHashCap) return -1;
    uint32_t k = hash_id(id) & (uint32_t)(g_idHashCap - 1);
    while (g_idHash[k] >= 0) {
        if (g_nodes[g_idHash[k]].w.id == id) return g_idHash[k];
        k = (k + 1) & (uint32_t)(g_idHashCap - 1);
    }
    return -1;
}

static Node *find_node(uint32_t id)
{
    int i = node_index(id);
    return i >= 0 ? &g_nodes[i] : NULL;
}

static void toast(const char *msg)
{
    snprintf(g_toast, sizeof g_toast, "%s", msg);
    g_toastAt = trace_time_ms();
}

static double km_between(double lat1, double lon1, double lat2, double lon2)
{
    const double r = 0.017453292519943295;
    double a = sin((lat2 - lat1) * r / 2), b = sin((lon2 - lon1) * r / 2);
    double h = a * a + cos(lat1 * r) * cos(lat2 * r) * b * b;
    return 2 * 6371.0 * asin(sqrt(h < 1 ? h : 1));
}

static double node_km(const LpNodeWire *w)
{
    if (!g_haveHome || !(w->flags & 1)) return -1;
    return km_between(g_homeLat, g_homeLon, w->lat1e5 / 1e5, w->lon1e5 / 1e5);
}

// Temperature for people in the US: Fahrenheit.
static int temp_f(int16_t dC) { return (int)lroundf(dC / 10.0f * 9 / 5 + 32); }

// ------------------------------------------------------------ the view ---

static void to_screen(float bx, float by, float *sx, float *sy)
{
    float cx = MAP_X + MAP_W / 2.0f, cy = MAP_Y + MAP_H / 2.0f;
    *sx = (bx - cx) * g_zoom + cx + g_panX;
    *sy = (by - cy) * g_zoom + cy + g_panY;
}

static void from_screen(float sx, float sy, float *bx, float *by)
{
    float cx = MAP_X + MAP_W / 2.0f, cy = MAP_Y + MAP_H / 2.0f;
    *bx = (sx - cx - g_panX) / g_zoom + cx;
    *by = (sy - cy - g_panY) / g_zoom + cy;
}

static void project_node(Node *n)
{
    n->onMap = 0;
    if (!g_haveGeo || !(n->w.flags & 1) || !in_region(n->w.state)) return;
    n->onMap = geo_project(&g_view, &g_geo, n->w.state - 1, n->w.lat1e5 / 1e5f, n->w.lon1e5 / 1e5f, &n->bx, &n->by);
}

static void project_all(void)
{
    if (!g_haveGeo) return;
    geo_view(&g_view, &g_geo, g_region, MAP_X, MAP_Y + 4, MAP_W, MAP_H - 8);
    for (int i = 0; i < g_n; i++) project_node(&g_nodes[i]);
    g_projFor = g_region;
}

static void clamp_view(void)
{
    if (g_zoom < 1) g_zoom = 1;
    if (g_zoom > 24) g_zoom = 24;
    float mx = MAP_W * (g_zoom - 1) / 2 + MAP_W * 0.25f, my = MAP_H * (g_zoom - 1) / 2 + MAP_H * 0.25f;
    if (g_zoom == 1) mx = my = 0;
    if (g_panX > mx) g_panX = mx;
    if (g_panX < -mx) g_panX = -mx;
    if (g_panY > my) g_panY = my;
    if (g_panY < -my) g_panY = -my;
}

static void reset_view(void) { g_zoom = 1; g_panX = g_panY = 0; }

// Zooms by f keeping the map point under (ax, ay) where it is.
static void zoom_at(float f, float ax, float ay)
{
    float bx, by;
    from_screen(ax, ay, &bx, &by);
    g_zoom *= f;
    clamp_view();
    float sx, sy;
    to_screen(bx, by, &sx, &sy);
    g_panX += ax - sx;
    g_panY += ay - sy;
    clamp_view();
}

static void set_region(int r)
{
    if (r < 0 || (g_haveGeo && r > g_geo.count)) return;
    if (r != g_region) {
        g_region = r;
        uint8_t b = (uint8_t)r;
        send_msg(LP_OUT_REGION, &b, 1);
    }
    reset_view();
    g_sel = g_hover = -1;
    project_all();
    g_chatBack = 0;
}

// ------------------------------------------------------------ base map ---

static uint32_t *g_base = NULL;      // the land and lines at device size
static int       g_baseW = 0, g_baseH = 0, g_baseRegion = -2, g_baseS = 0;
static float     g_baseZoom = -1, g_basePanX = 0, g_basePanY = 0;

// Fills one state (all its rings, even-odd) into the base, at device pixels.
static void fill_state(int s, const GeoPane *p, uint32_t col, const Rect *only)
{
    const GeoState *st = &g_geo.st[s];
    static float *xs = NULL, *ys = NULL;
    static int cap = 0;
    int total = 0;
    for (int r = st->firstRing; r < st->firstRing + st->rings; r++) total += g_geo.ringLen[r];
    if (total > cap) {
        free(xs); free(ys);
        xs = malloc(sizeof *xs * (size_t)total);
        ys = malloc(sizeof *ys * (size_t)total);
        cap = xs && ys ? total : 0;
        if (!cap) return;
    }
    float minY = 1e9f, maxY = -1e9f, minX = 1e9f, maxX = -1e9f;
    int k = 0;
    for (int r = st->firstRing; r < st->firstRing + st->rings; r++) {
        const GeoPt *pt = g_geo.pts + g_geo.ringStart[r];
        for (int j = 0; j < g_geo.ringLen[r]; j++, k++) {
            float sx, sy;
            geo_to_screen(p, pt[j].lat, pt[j].lon, &sx, &sy);
            if (!only) to_screen(sx, sy, &sx, &sy);
            xs[k] = (sx - MAP_X) * g_S;
            ys[k] = (sy - MAP_Y) * g_S;
            if (ys[k] < minY) minY = ys[k];
            if (ys[k] > maxY) maxY = ys[k];
            if (xs[k] < minX) minX = xs[k];
            if (xs[k] > maxX) maxX = xs[k];
        }
    }
    if (maxX < 0 || minX > g_baseW || maxY < 0 || minY > g_baseH) return;
    int y0 = minY < 0 ? 0 : (int)minY, y1 = maxY >= g_baseH ? g_baseH - 1 : (int)maxY;
    int ox0 = 0, ox1 = g_baseW, oy0 = 0, oy1 = g_baseH;
    if (only) {
        ox0 = (int)((only->x - MAP_X) * g_S); ox1 = (int)((only->x + only->w - MAP_X) * g_S);
        oy0 = (int)((only->y - MAP_Y) * g_S); oy1 = (int)((only->y + only->h - MAP_Y) * g_S);
    }
    static float cross[1024];
    for (int y = y0; y <= y1; y++) {
        if (y < oy0 || y >= oy1) continue;
        float py = y + 0.5f;
        int nc = 0, base = 0;
        for (int r = st->firstRing; r < st->firstRing + st->rings; r++) {
            int n = g_geo.ringLen[r];
            for (int a = 0, b = n - 1; a < n && nc < 1024; b = a++)
                if ((ys[base + a] > py) != (ys[base + b] > py))
                    cross[nc++] = xs[base + a] + (py - ys[base + a]) * (xs[base + b] - xs[base + a]) / (ys[base + b] - ys[base + a]);
            base += n;
        }
        for (int i = 1; i < nc; i++)
            for (int j = i; j > 0 && cross[j - 1] > cross[j]; j--) { float t = cross[j]; cross[j] = cross[j - 1]; cross[j - 1] = t; }
        uint32_t *row = g_base + y * g_baseW;
        for (int i = 0; i + 1 < nc; i += 2) {
            int xa = (int)ceilf(cross[i] - 0.5f), xb = (int)ceilf(cross[i + 1] - 0.5f);
            if (xa < ox0) xa = ox0;
            if (xb > ox1) xb = ox1;
            if (xa < 0) xa = 0;
            if (xb > g_baseW) xb = g_baseW;
            for (int x = xa; x < xb; x++) row[x] = col;
        }
    }
}

// State lines: where a pixel's right or lower neighbour belongs to another state.
// Works on an id buffer so lines are one pixel wide at any zoom.
static uint8_t *g_ids = NULL;

static void render_base(void)
{
    int W = D(MAP_X + MAP_W) - D(MAP_X), H = D(MAP_Y + MAP_H) - D(MAP_Y);
    if (W != g_baseW || H != g_baseH || !g_base) {
        free(g_base);
        free(g_ids);
        g_base = malloc((size_t)W * H * 4);
        g_ids = malloc((size_t)W * H);
        g_baseW = W;
        g_baseH = H;
    }
    if (!g_base || !g_ids) return;
    for (int i = 0; i < W * H; i++) g_base[i] = 0;
    if (g_haveGeo) {
        // Fill with state ids (index + 1) packed in the colour buffer, then colour.
        for (int s = 0; s < g_geo.count; s++) {
            int pane = geo_pane_for_state(&g_view, &g_geo, s);
            const GeoPane *p = &g_view.pane[pane];
            if (g_view.npanes > 1 && pane > 0) {
                // An inset: drawn at its own place, not zoomed with the lower 48.
                Rect r = { p->x, p->y, p->w, p->h };
                if (g_zoom == 1) fill_state(s, p, (uint32_t)(s + 1), &r);
            } else {
                fill_state(s, p, (uint32_t)(s + 1), NULL);
            }
        }
    }
    for (int i = 0; i < W * H; i++) g_ids[i] = (uint8_t)g_base[i];
    int sel = g_region;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int s = g_ids[y * W + x];
            uint32_t c;
            if (!s) {
                c = C_SEA;
                // A soft graticule of dots so the sea isn't a flat slab.
                if (((x / g_S) % 24 == 0) && ((y / g_S) % 24 == 0)) c = 0x0E1A2C;
            } else {
                int mine = !sel || s == sel;
                c = mine ? C_LAND : C_LAND_DIM;
                int r = x + 1 < W ? g_ids[y * W + x + 1] : s, d = y + 1 < H ? g_ids[(y + 1) * W + x] : s;
                int l = x > 0 ? g_ids[y * W + x - 1] : s, u = y > 0 ? g_ids[(y - 1) * W + x] : s;
                if ((r && r != s) || (d && d != s)) c = mine ? C_LINE : C_LINE_DIM;
                else if (!r || !d || !l || !u) c = mine ? 0x2A5074 : 0x16283E;      // the coast
            }
            g_base[y * W + x] = c | 0xFF000000u;
        }
    // Frames around the insets (All US, unzoomed).
    g_baseRegion = g_region;
    g_baseZoom = g_zoom;
    g_basePanX = g_panX;
    g_basePanY = g_panY;
    g_baseS = g_S;
}

static void draw_base(void)
{
    if (!g_base || g_baseRegion != g_region || g_baseZoom != g_zoom || g_basePanX != g_panX || g_basePanY != g_panY ||
        g_baseS != g_S || g_baseW != D(MAP_X + MAP_W) - D(MAP_X))
        render_base();
    if (!g_base) return;
    int X0 = D(MAP_X), Y0 = D(MAP_Y);
    for (int y = 0; y < g_baseH && Y0 + y < g_H; y++)
        memcpy(g_frame + (Y0 + y) * g_W + X0, g_base + y * g_baseW, (size_t)g_baseW * 4);
    if (g_view.npanes > 1 && g_zoom == 1) {
        for (int i = 1; i < g_view.npanes; i++) {
            const GeoPane *p = &g_view.pane[i];
            rrect_stroke_l(p->x + 2, p->y + 2, p->w - 4, p->h - 4, 6, 1, C_EDGE, 200);
        }
    }
}

// ------------------------------------------------------------ nodes ---

static void node_screen(const Node *n, float *x, float *y)
{
    int pane = g_view.npanes > 1 ? geo_pane_for_state(&g_view, &g_geo, n->w.state - 1) : 0;
    if (pane > 0) { *x = n->bx; *y = n->by; return; }   // insets don't zoom
    to_screen(n->bx, n->by, x, y);
}

static int node_visible(const Node *n)
{
    if (!n->onMap) return 0;
    if (g_view.npanes > 1 && g_zoom != 1 && geo_pane_for_state(&g_view, &g_geo, n->w.state - 1) > 0) return 0;
    uint32_t cut = now_door() - (uint32_t)g_nodeDays * 86400u;
    return n->w.lastHeard >= cut;
}

// Sensor labels: placed greedily, skipping any that would cover one already placed.
static Rect g_labels[160];
static int  g_nLabels = 0;

static int label_free(Rect r)
{
    for (int i = 0; i < g_nLabels; i++) {
        const Rect *o = &g_labels[i];
        if (r.x < o->x + o->w && o->x < r.x + r.w && r.y < o->y + o->h && o->y < r.y + r.h) return 0;
    }
    return g_nLabels < 160;
}

static void draw_nodes(void)
{
    int32_t t = trace_time_ms();
    g_nLabels = 0;
    float r = g_zoom < 2 ? 2.1f : g_zoom < 6 ? 2.8f : 3.6f;
    // Two passes: the old ones underneath, the fresh ones on top.
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_n; i++) {
            Node *n = &g_nodes[i];
            if (!node_visible(n)) continue;
            uint32_t age = now_door() - n->w.lastHeard;
            if ((pass == 0) != (age >= 3600)) continue;
            float x, y;
            node_screen(n, &x, &y);
            if (x < MAP_X - 4 || y < MAP_Y - 4 || x > MAP_X + MAP_W + 4 || y > MAP_Y + MAP_H + 4) continue;
            uint32_t col = heard_colour(n->w.lastHeard);
            int a = age < 86400 ? 255 : 170;
            int sensor = (n->w.flags & 8) && n->w.temp_dC != -32768;
            if (g_showSensors && !sensor) a = 60;
            if (age < 3600 && a > 60) circle_l(x, y, r + 2.4f, col, 45);
            circle_l(x, y, r, col, a);
            if (g_showSensors && sensor) {
                char t[16];
                snprintf(t, sizeof t, "%d\xb0", temp_f(n->w.temp_dC));
                float tw = text_w(F_SMALL, t, 0) + 8;
                int f = temp_f(n->w.temp_dC);
                uint32_t tc = f >= 85 ? 0xFF8A5C : f >= 65 ? 0xFFD166 : f >= 45 ? 0x8FE3B0 : 0x7FC8FF;
                Rect lr = { x + 4, y - 7, tw, 13 };
                if (label_free(lr)) {
                    g_labels[g_nLabels++] = lr;
                    rrect_l(x + 4, y - 7, tw, 13, 6, 0x000000, 170);
                    text(F_SMALL, x + 8, y - 7, t, tc);
                } else {
                    ring_l(x, y, r + 1.5f, 1, tc, 200);          // a reading too crowded to label: a ring in its colour
                }
            }
            if (n->pulseAt) {
                int32_t d = t - n->pulseAt;
                if (d > 1600) n->pulseAt = 0;
                else {
                    float k = d / 1600.0f;
                    ring_l(x, y, r + 2 + 14 * k, 1.5f, 0xC8FFD8, (int)(230 * (1 - k)));
                    circle_l(x, y, r + 0.6f, 0xFFFFFF, (int)(255 * (1 - k)));
                }
            }
        }
    // The chosen one: a ring and its name.
    if (g_sel >= 0 && g_sel < g_n && node_visible(&g_nodes[g_sel])) {
        Node *n = &g_nodes[g_sel];
        float x, y;
        node_screen(n, &x, &y);
        float pulse = 0.5f + 0.5f * sinf(t / 260.0f);
        glow_l(x, y, 14, 0xFFFFFF, (int)(50 + 40 * pulse));
        ring_l(x, y, 7 + 2 * pulse, 1.5f, 0xFFFFFF, 230);
        circle_l(x, y, 2.6f, 0xFFFFFF, 255);
        const char *nm = node_label(&n->w);
        float w = text_w(F_BOLD, nm, 0) + 10;
        float lx = x + 12, ly = y - 8;
        if (lx + w > MAP_X + MAP_W - 40) lx = x - 12 - w;
        if (ly < MAP_Y + 4) ly = y + 8;
        rrect_l(lx, ly, w, 17, 4, 0x000000, 160);
        text(F_BOLD, lx + 5, ly + 1, nm, 0xFFFFFF);
    }
    if (g_hover >= 0 && g_hover != g_sel && g_hover < g_n) {
        Node *n = &g_nodes[g_hover];
        float x, y;
        node_screen(n, &x, &y);
        char when[32], buf[96];
        ago(n->w.lastHeard, when, sizeof when);
        if (n->w.shortName[0] && n->w.name[0])
            snprintf(buf, sizeof buf, "%s (%s)  \xb7  %s", n->w.name, n->w.shortName, when);
        else
            snprintf(buf, sizeof buf, "%s  \xb7  %s", node_label(&n->w), when);
        float w = text_w(F_SMALL, buf, 0) + 10;
        float lx = x + 10, ly = y + 6;
        if (lx + w > MAP_X + MAP_W - 4) lx = x - 10 - w;
        if (ly > MAP_Y + MAP_H - 20) ly = y - 20;
        ring_l(x, y, 6, 1.2f, 0xFFFFFF, 160);
        rrect_l(lx, ly, w, 15, 4, 0x000000, 180);
        text(F_SMALL, lx + 5, ly + 2, buf, C_TEXT);
    }
}

// Link colour by signal-to-noise: green is a strong link, red a marginal one.
static uint32_t snr_colour(int snr_q)
{
    float db = snr_q / 4.0f;
    if (db >= 5) return C_MESH;
    if (db >= 0) return 0xC6E35A;
    if (db >= -7) return C_WARN;
    return C_ERR;
}

static void draw_links(void)
{
    if (!g_showLinks) return;
    resolve_links();
    uint32_t now = now_door();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_nLinks; i++) {
            const Link *l = &g_links[i];
            if (l->ia < 0 || l->ib < 0) continue;
            uint32_t age = now - l->w.lastHeard;
            if (age > 86400) continue;
            int mine = g_sel >= 0 && (l->ia == g_sel || l->ib == g_sel);
            if ((pass == 1) != mine) continue;                       // the chosen node's links on top
            const Node *a = &g_nodes[l->ia], *b = &g_nodes[l->ib];
            if (!node_visible(a) || !node_visible(b)) continue;
            float x0, y0, x1, y1;
            node_screen(a, &x0, &y0);
            node_screen(b, &x1, &y1);
            int alpha = age < 3600 ? 235 : age < 6 * 3600 ? 180 : 120;
            if (g_sel >= 0 && !mine) alpha /= 2;
            line_l(x0, y0, x1, y1, mine ? 2.2f : 1.5f, snr_colour(l->w.snr_q), mine ? 255 : alpha);
        }
}

static void home_marker(void)
{
    if (!g_haveHome || !g_haveGeo) return;
    float bx, by;
    if (!geo_project(&g_view, &g_geo, g_homeState - 1, g_homeLat, g_homeLon, &bx, &by)) return;
    int pane = g_view.npanes > 1 ? geo_pane_for_state(&g_view, &g_geo, g_homeState - 1) : 0;
    float x = bx, y = by;
    if (pane == 0) to_screen(bx, by, &x, &y);
    else if (g_zoom != 1) return;
    if (x < MAP_X || y < MAP_Y || x > MAP_X + MAP_W || y > MAP_Y + MAP_H) return;
    // A small house.
    float p[] = { x - 6, y - 1, x, y - 7, x + 6, y - 1 };
    poly_l(p, 3, 0xFF5F7A, 255);
    fill_l(x - 4, y - 1, 8, 6, 0xFF5F7A, 255);
    fill_l(x - 1, y + 1, 2.5f, 4, 0x2A0A12, 255);
    if (g_zoom >= 2 || g_region) text(F_SMALL, x + 8, y - 6, "home", 0xFF8FA2);
}

// The node nearest the pointer, within 9 pixels.
static int node_at(float mx, float my)
{
    int best = -1;
    float bd = 81;
    for (int i = 0; i < g_n; i++) {
        if (!node_visible(&g_nodes[i])) continue;
        float x, y;
        node_screen(&g_nodes[i], &x, &y);
        float dx = x - mx, dy = y - my, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// Which state is under the pointer (All US, unzoomed), from the id buffer.
static int state_at(float mx, float my)
{
    if (!g_ids || mx < MAP_X || my < MAP_Y || mx >= MAP_X + MAP_W || my >= MAP_Y + MAP_H) return 0;
    int x = D(mx) - D(MAP_X), y = D(my) - D(MAP_Y);
    if (x < 0 || y < 0 || x >= g_baseW || y >= g_baseH) return 0;
    return g_ids[y * g_baseW + x];
}

// Tab: the next node on the map, newest first.
static void step_node(int dir)
{
    int count = 0;
    static int *order = NULL;
    static int cap = 0;
    if (g_n > cap) { free(order); order = malloc(sizeof *order * (size_t)g_n); cap = order ? g_n : 0; }
    if (!order) return;
    for (int i = 0; i < g_n; i++) if (node_visible(&g_nodes[i])) order[count++] = i;
    if (!count) return;
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && g_nodes[order[j - 1]].w.lastHeard < g_nodes[order[j]].w.lastHeard; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    int at = -1;
    for (int i = 0; i < count; i++) if (order[i] == g_sel) at = i;
    at = at < 0 ? 0 : (at + dir + count) % count;
    g_sel = order[at];
    // Keep it on screen.
    float x, y;
    node_screen(&g_nodes[g_sel], &x, &y);
    if (x < MAP_X + 30 || x > MAP_X + MAP_W - 50 || y < MAP_Y + 20 || y > MAP_Y + MAP_H - 20) {
        g_panX += MAP_X + MAP_W / 2.0f - x;
        g_panY += MAP_Y + MAP_H / 2.0f - y;
        clamp_view();
    }
}

// ------------------------------------------------------------- panels ---

static void panel(float x, float w, const char *head, uint32_t headCol)
{
    rrect_l(x, PANEL_Y, w, PANEL_H, 8, C_PANEL, 255);
    rrect_stroke_l(x, PANEL_Y, w, PANEL_H, 8, 1, C_EDGE, 255);
    if (head) label(x + 10, PANEL_Y + 8, head, headCol);
}

static void kv(float x, float y, const char *k, const char *v, uint32_t vc)
{
    label(x, y + 2, k, C_TEXT3);
    text(F_SMALL, x + 62, y + 1, v, vc);
}

static void draw_node_panel(void)
{
    panel(NODE_X, NODE_W, NULL, 0);
    if (g_sel < 0 || g_sel >= g_n) {
        label(NODE_X + 10, PANEL_Y + 8, "NODE", C_TEXT3);
        text(F_SMALL, NODE_X + 10, PANEL_Y + 40, "Click a node on the map,", C_TEXT2);
        text(F_SMALL, NODE_X + 10, PANEL_Y + 56, "or press Tab to step", C_TEXT2);
        text(F_SMALL, NODE_X + 10, PANEL_Y + 72, "through them, newest first.", C_TEXT2);
        return;
    }
    const LpNodeWire *w = &g_nodes[g_sel].w;
    char nm[48], buf[64];
    fit(F_BOLD, node_label(w), NODE_W - 20, nm, sizeof nm);
    text(F_BOLD, NODE_X + 10, PANEL_Y + 5, nm, w->name[0] ? C_TEXT : C_TEXT2);
    // Its short name and number, as other Meshtastic maps and apps show them.
    float sx = NODE_X + 10;
    if (w->shortName[0]) {
        float bw = text_w(F_SMALL, w->shortName, 0) + 14;
        rrect_l(sx, PANEL_Y + 23, bw, 17, 8, C_MESH, 40);
        text(F_SMALL, sx + 7, PANEL_Y + 26, w->shortName, C_MESH);
        sx += bw + 6;
    }
    snprintf(buf, sizeof buf, "!%08x", w->id);
    text(F_SMALL, sx, PANEL_Y + 26, buf, C_TEXT3);
    if ((w->flags & 8) && w->temp_dC != -32768) {
        // A sensor: its reading, top right.
        if (w->humidity != 255) snprintf(buf, sizeof buf, "%d\xb0" "F  %u%%", temp_f(w->temp_dC), w->humidity);
        else snprintf(buf, sizeof buf, "%d\xb0" "F", temp_f(w->temp_dC));
        float bw = text_w(F_SMALL, buf, 0) + 14;
        rrect_l(NODE_X + NODE_W - 10 - bw, PANEL_Y + 23, bw, 17, 8, 0xFFD166, 40);
        text(F_SMALL, NODE_X + NODE_W - 3 - bw, PANEL_Y + 26, buf, 0xFFD166);
    }
    float y = PANEL_Y + 43;
    kv(NODE_X + 10, y, "Radio", w->hw ? lp_hw_name(w->hw) : "not reported", w->hw ? C_TEXT : C_TEXT3); y += 15;
    if (w->battery == 255) snprintf(buf, sizeof buf, "not reported");
    else if (w->battery > 100) snprintf(buf, sizeof buf, "on external power");
    else if (w->voltage_cV) snprintf(buf, sizeof buf, "%u%%  (%.2f V)", w->battery, w->voltage_cV / 100.0);
    else snprintf(buf, sizeof buf, "%u%%", w->battery);
    kv(NODE_X + 10, y, "Battery", buf, w->battery == 255 ? C_TEXT3 : C_TEXT); y += 15;
    ago(w->lastHeard, buf, sizeof buf);
    kv(NODE_X + 10, y, "Heard", buf, heard_colour(w->lastHeard)); y += 15;
    double km = node_km(w);
    char where[64], fitted[64];
    const char *st = w->state ? region_name(w->state) : "unknown";
    if (km >= 0) snprintf(where, sizeof where, "%s \xb7 %.0f mi", st, km * 0.621371);
    else snprintf(where, sizeof where, "%s", st);
    fit(F_SMALL, where, NODE_W - 82, fitted, sizeof fitted);
    kv(NODE_X + 10, y, "Where", fitted, w->state ? C_TEXT2 : C_TEXT3); y += 15;
    int nl = links_of(g_sel);
    if (w->flags & 1)
        snprintf(buf, sizeof buf, "~%.2f\xb0%c %.2f\xb0%c%s", fabs(w->lat1e5 / 1e5), w->lat1e5 >= 0 ? 'N' : 'S',
                 fabs(w->lon1e5 / 1e5), w->lon1e5 >= 0 ? 'E' : 'W', "");
    else snprintf(buf, sizeof buf, "no position");
    float bx2 = text(F_SMALL, NODE_X + 72, y, buf, C_TEXT3);
    if (nl) {
        snprintf(buf, sizeof buf, "  \xb7 %d link%s", nl, nl == 1 ? "" : "s");
        text(F_SMALL, bx2, y, buf, C_MESH);
    }
}

// Test chatter: "test", "hi", "check", a script's repeats. Hidden unless the caller asks (F).
static int is_chatter_text(const char *t)
{
    char k[40];
    int n = 0;
    for (const char *p = t; *p && n < 38; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) k[n++] = c;
    }
    k[n] = 0;
    if (n == 0) return 1;
    static const char *const WORDS[] = { "test", "testing", "tests", "testtest", "hi", "hello", "hey", "ping", "pong",
        "check", "checkcheck", "checkonetwothree", "ack", "ok", "okay", "yes", "123", "1234", "receivedinatl", "received",
        "cmd", "radiocheck", "copy", "rogerthat", "roger", "hellomesh", "hellotest", "testmsg", "test1", "test2", "test123" };
    for (unsigned i = 0; i < sizeof WORDS / sizeof *WORDS; i++) if (!strcmp(k, WORDS[i])) return 1;
    return 0;
}

// Whether chat line i is chatter: a test word, or a repeat of the same sender's same words in the last 3 hours.
static int is_chatter(int i)
{
    const Chat *c = &g_chat[i];
    if (is_chatter_text(c->text)) return 1;
    for (int j = i - 1; j >= 0; j--) {
        if (c->time - g_chat[j].time > 3 * 3600) break;
        if (!strcmp(g_chat[j].text, c->text) && !strcmp(g_chat[j].name, c->name)) return 1;
    }
    return 0;
}

// The channels in the chat, for the filter (up to 8).
static int chat_channels(char out[][20], int max)
{
    int n = 0;
    for (int i = 0; i < g_nChat && n < max; i++) {
        int seen = 0;
        for (int k = 0; k < n; k++) if (!strcmp(out[k], g_chat[i].chan)) seen = 1;
        if (!seen && g_chat[i].chan[0]) snprintf(out[n++], 20, "%s", g_chat[i].chan);
    }
    return n;
}

static int chat_shown(int i)
{
    const Chat *c = &g_chat[i];
    if (!in_region(c->state)) return 0;
    if (g_hideChatter && is_chatter(i)) return 0;
    if (g_chanSel >= 0) {
        char chans[8][20];
        int nc = chat_channels(chans, 8);
        if (g_chanSel < nc && strcmp(c->chan, chans[g_chanSel])) return 0;
    }
    return 1;
}

static void chat_time(uint32_t t, char *out, size_t n)
{
    // Clock time in UTC offset-free form: minutes ago is clearer than a zone guess.
    uint32_t now = now_door(), d = now > t ? now - t : 0;
    if (d < 60) snprintf(out, n, "now");
    else if (d < 3600) snprintf(out, n, "%um", d / 60);
    else if (d < 86400) snprintf(out, n, "%uh", d / 3600);
    else snprintf(out, n, "%ud", d / 86400);
}

static void draw_chat_panel(void)
{
    int hot = !g_picker && !g_chatOpen && g_mouseOver && inside(R_CHATPANEL, g_mx, g_my);
    panel(CHAT_X, CHAT_W, "PUBLIC CHAT", C_ACCENT);
    if (hot) rrect_stroke_l(CHAT_X, PANEL_Y, CHAT_W, PANEL_H, 8, 1, C_EDGE_HI, 255);
    text_r(F_SMALL, CHAT_X + CHAT_W - 10, PANEL_Y + 7, "C: all of it", hot ? C_TEXT2 : C_TEXT3);
    int shown = 0;
    float y = PANEL_Y + PANEL_H - 22;
    for (int i = g_nChat - 1; i >= 0 && shown < 6; i--) {
        const Chat *c = &g_chat[i];
        if (!chat_shown(i)) continue;
        char when[16], line[300], out[300];
        chat_time(c->time, when, sizeof when);
        text_r(F_SMALL, CHAT_X + 34, y, when, C_TEXT3);
        const char *who = c->name[0] ? c->name : "(no name yet)";
        char whoFit[40];
        fit(F_SBOLD, who, 90, whoFit, sizeof whoFit);
        float x = text(F_SBOLD, CHAT_X + 40, y, whoFit, c->name[0] ? C_MESH : C_TEXT3);
        snprintf(line, sizeof line, "%s", c->text);
        fit(F_SMALL, line, CHAT_X + CHAT_W - 10 - (x + 6), out, sizeof out);
        text(F_SMALL, x + 6, y, out, C_TEXT);
        y -= 15;
        shown++;
    }
    if (!shown) {
        text(F_BODY, CHAT_X + 12, PANEL_Y + 44, g_region ? "Nothing said here yet." : "Nothing said yet.", C_TEXT2);
        text(F_SMALL, CHAT_X + 12, PANEL_Y + 64, "Chat is rare: most radios only report in.", C_TEXT3);
    }
}

static void draw_stats_panel(void)
{
    int hot = !g_picker && !g_chatOpen && !g_statsOpen && g_mouseOver && inside(R_STATSPANEL, g_mx, g_my);
    panel(STAT_X, STAT_W, "NODES HEARD", C_ACCENT);
    if (hot) rrect_stroke_l(STAT_X, PANEL_Y, STAT_W, PANEL_H, 8, 1, C_EDGE_HI, 255);
    text_r(F_SMALL, STAT_X + STAT_W - 10, PANEL_Y + 7, "S: more", hot ? C_TEXT2 : C_TEXT3);
    uint32_t now = now_door(), hour = 0, day = 0, week = 0;
    int perState[GEO_MAX_STATES + 1] = { 0 };
    static int perHw[256];
    memset(perHw, 0, sizeof perHw);
    for (int i = 0; i < g_n; i++) {
        const LpNodeWire *w = &g_nodes[i].w;
        if (now - w->lastHeard > (uint32_t)g_nodeDays * 86400u) continue;
        if (now - w->lastHeard < 86400 && w->state <= GEO_MAX_STATES) perState[w->state]++;
        if (!in_region(w->state)) continue;
        week++;
        if (now - w->lastHeard < 86400) { day++; if (w->hw) perHw[w->hw]++; }
        if (now - w->lastHeard < 3600) hour++;
    }
    char buf[32];
    float y = PANEL_Y + 22;
    struct { const char *k; uint32_t v; } rows[3] = { { "Last hour", hour }, { "Today", day }, { "", week } };
    char days[16];
    snprintf(days, sizeof days, "%d days", g_nodeDays);
    rows[2].k = days;
    for (int i = 0; i < 3; i++) {
        label(STAT_X + 10, y + 2, rows[i].k, C_TEXT3);
        snprintf(buf, sizeof buf, "%u", rows[i].v);
        text_r(i == 0 ? F_BOLD : F_BODY, STAT_X + STAT_W - 10, y, buf, i == 0 ? C_MESH : C_TEXT);
        y += 14;
    }
    y += 2;
    fill_l(STAT_X + 10, y, STAT_W - 20, 1, C_EDGE, 255);
    y += 4;
    label(STAT_X + 10, y, g_region ? "RADIOS TODAY" : "BUSIEST TODAY", C_TEXT3);
    y += 12;
    for (int k = 0; k < 3; k++) {
        int bi = 0, bv = 0;
        if (g_region) {
            for (int h = 1; h < 256; h++) if (perHw[h] > bv) { bv = perHw[h]; bi = h; }
            if (!bv) break;
            perHw[bi] = 0;
        } else {
            for (int s = 1; s <= g_geo.count && s <= GEO_MAX_STATES; s++) if (perState[s] > bv) { bv = perState[s]; bi = s; }
            if (!bv) break;
            perState[bi] = 0;
        }
        char nm[32];
        fit(F_SMALL, g_region ? lp_hw_name((unsigned)bi) : region_name(bi), STAT_W - 60, nm, sizeof nm);
        text(F_SMALL, STAT_X + 10, y, nm, C_TEXT2);
        snprintf(buf, sizeof buf, "%d", bv);
        text_r(F_SMALL, STAT_X + STAT_W - 10, y, buf, C_TEXT2);
        y += 12;
    }
}

// ------------------------------------------------------------ chrome ---

static void mesh_logo(float cx, float cy)
{
    float t = trace_time_ms() / 1000.0f;
    float px[3] = { cx - 8, cx + 8, cx }, py[3] = { cy + 5, cy + 5, cy - 7 };
    for (int i = 0; i < 3; i++) line_l(px[i], py[i], px[(i + 1) % 3], py[(i + 1) % 3], 1.3f, C_MESH, 150);
    for (int i = 0; i < 3; i++) {
        float p = 0.5f + 0.5f * sinf(t * 3 + i * 2.1f);
        circle_l(px[i], py[i], 2.6f + p * 0.8f, C_MESH, 255);
    }
}

static void spinner(float cx, float cy, float r, uint32_t col)
{
    float t = trace_time_ms() / 1000.0f;
    for (int i = 0; i < 8; i++) {
        float ang = i * PI_F / 4 + t * 5;
        circle_l(cx + cosf(ang) * r, cy + sinf(ang) * r, r * 0.24f, col, 60 + (int)(195 * (i / 7.0f)));
    }
}

static void pin_icon(float cx, float cy, float s, uint32_t col)
{
    circle_l(cx, cy - s * 0.12f, s * 0.3f, col, 255);
    float p[] = { cx - s * 0.26f, cy - s * 0.02f, cx, cy + s * 0.48f, cx + s * 0.26f, cy - s * 0.02f };
    poly_l(p, 3, col, 255);
    circle_l(cx, cy - s * 0.12f, s * 0.11f, C_BAR, 255);
}

static void draw_bar(void)
{
    fill_l(0, 0, LW, BAR_H, C_BAR, 255);
    fill_l(0, BAR_H, LW, 1, C_EDGE, 255);
    mesh_logo(20, 15);
    float x = text(F_HEAD, 36, 5, "Meshtastic", C_MESH);
    x = text(F_HEAD, x + 6, 5, "Listening Post", C_TEXT);

    // The region button.
    char name[40];
    fit(F_BODY, region_name(g_region), 150, name, sizeof name);
    float bw = text_w(F_BODY, name, 0) + 42;
    g_regionBtn = (Rect){ x + 14, 4, bw, 20 };
    int hot = !g_picker && !g_chatOpen && g_mouseOver && inside(g_regionBtn, g_mx, g_my);
    rrect_l(g_regionBtn.x, 4, bw, 20, 10, hot ? C_PANEL_HI : C_PANEL, 255);
    rrect_stroke_l(g_regionBtn.x, 4, bw, 20, 10, 1, hot ? C_MESH : C_EDGE, 255);
    pin_icon(g_regionBtn.x + 13, 14, 12, C_MESH);
    text(F_BODY, g_regionBtn.x + 22, 7, name, C_TEXT);
    float chx = g_regionBtn.x + bw - 12;
    line_l(chx - 3, 12, chx, 15, 1.4f, C_TEXT2, 255);
    line_l(chx, 15, chx + 3, 12, 1.4f, C_TEXT2, 255);

    // The feed, right of it.
    float rx = R_CLOSE.x - 10;
    char st[48];
    uint32_t col;
    if (g_stats.feed == LP_FEED_LIVE) { snprintf(st, sizeof st, "LIVE  %u/min", g_stats.pktPerMin); col = C_MESH; }
    else if (g_stats.feed == LP_FEED_OFFLINE) { snprintf(st, sizeof st, "OFFLINE"); col = C_ERR; }
    else { snprintf(st, sizeof st, "LISTENING"); col = C_WARN; }
    float sw = text_w(F_BOLD, st, 0);
    text(F_BOLD, rx - sw, 7, st, col);
    if (g_stats.feed == LP_FEED_LIVE) {
        float p = 0.5f + 0.5f * sinf(trace_time_ms() / 300.0f);
        circle_l(rx - sw - 9, 14, 3.4f, col, 255);
        ring_l(rx - sw - 9, 14, 5 + p * 2.5f, 1.2f, col, (int)(180 * (1 - p)));
    } else if (g_stats.feed == LP_FEED_OFFLINE) {
        circle_l(rx - sw - 9, 14, 3.4f, col, 255);
    } else {
        spinner(rx - sw - 11, 14, 5, col);
    }

    int hotC = !g_picker && !g_chatOpen && g_mouseOver && inside(R_CLOSE, g_mx, g_my);
    if (hotC) rrect_l(R_CLOSE.x, R_CLOSE.y, R_CLOSE.w, R_CLOSE.h, 6, 0x5A2030, 255);
    float r = 20 * 0.22f, cx = R_CLOSE.x + 12, cy = R_CLOSE.y + 10;
    line_l(cx - r, cy - r, cx + r, cy + r, 1.8f, hotC ? 0xFFFFFF : C_TEXT2, 255);
    line_l(cx - r, cy + r, cx + r, cy - r, 1.8f, hotC ? 0xFFFFFF : C_TEXT2, 255);
}

// The key hints can be clicked: each one drawn this frame is kept with the key it stands for and the layer it was
// drawn on (the map, or the overlay on top), and a click on one presses that key.
enum { HL_MAP, HL_PICKER, HL_CHAT, HL_STATS, HL_FIND, HL_HELP };
typedef struct { Rect r; int sc, shift, layer; } Hint;
static Hint g_hints[32];
static int  g_nHints = 0, g_hintLayer = HL_MAP;

static int hint_layer_now(void);

// The scancode for a hint's key, or 0 for one that names several keys ("Arrows", "Up/Dn").
static int hint_scancode(const char *k, int *shift)
{
    static const char lower[0x40] =
        "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0\0\0 ";
    *shift = 0;
    if (!strcmp(k, "Esc")) return 0x01;
    if (!strcmp(k, "Enter")) return 0x1C;
    if (!strcmp(k, "Tab")) return 0x0F;
    if (!k[0] || k[1]) return 0;
    char ch = k[0];
    if (ch == '?') { ch = '/'; *shift = 1; }
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    for (int sc = 1; sc < 0x40; sc++) if (lower[sc] == ch) return sc;
    return 0;
}

static void keycap_hint(float *x, float y, const char *k, const char *what)
{
    float w = text_w(F_SMALL, k, 0) + 8;
    if (w < 14) w = 14;
    int shift, sc = hint_scancode(k, &shift);
    float x0 = *x, end = text_w(F_SMALL, what, 0) + x0 + w + 4;
    Rect r = { x0 - 2, y - 2, end - x0 + 4, 17 };
    int hot = sc && g_mouseOver && g_hintLayer == hint_layer_now() && inside(r, g_mx, g_my);
    rrect_l(*x, y, w, 13, 3, hot ? C_MESH : C_PANEL_HI, hot ? 70 : 255);
    rrect_stroke_l(*x, y, w, 13, 3, 1, hot ? C_MESH : C_EDGE_HI, 255);
    text_c(F_SMALL, *x + w / 2, y + 1, k, hot ? 0xFFFFFF : C_TEXT2);
    *x = text(F_SMALL, *x + w + 4, y + 1, what, hot ? C_TEXT : C_TEXT3) + 12;
    if (sc && g_nHints < (int)(sizeof g_hints / sizeof *g_hints))
        g_hints[g_nHints++] = (Hint){ r, sc, shift, g_hintLayer };
}

static void map_button(Rect r, int kind)
{
    int hot = !g_picker && !g_chatOpen && g_mouseOver && inside(r, g_mx, g_my);
    rrect_l(r.x, r.y, r.w, r.h, 6, hot ? C_PANEL_HI : 0x000000, hot ? 255 : 150);
    rrect_stroke_l(r.x, r.y, r.w, r.h, 6, 1, hot ? C_MESH : C_EDGE_HI, 255);
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    uint32_t c = hot ? 0xFFFFFF : C_TEXT;
    if (kind == 0) { line_l(cx - 5, cy, cx + 5, cy, 1.8f, c, 255); line_l(cx, cy - 5, cx, cy + 5, 1.8f, c, 255); }
    else if (kind == 1) line_l(cx - 5, cy, cx + 5, cy, 1.8f, c, 255);
    else if (kind == 2) { ring_l(cx, cy, 5, 1.5f, c, 255); circle_l(cx, cy, 1.6f, c, 255); }
    else {   // home
        float p[] = { cx - 6, cy, cx, cy - 6, cx + 6, cy };
        poly_l(p, 3, g_haveHome ? 0xFF7A90 : c, 255);
        fill_l(cx - 4, cy, 8, 5, g_haveHome ? 0xFF7A90 : c, 255);
    }
}

// Under the map: the colour key and the keys; over its foot, the warm-up message.
static void draw_map_foot(void)
{
    fill_l(0, FOOT_Y, LW, 1, C_EDGE, 255);
    float x = MAP_X + 10, y = FOOT_Y + 4;
    label(x, y + 1, "HEARD", C_TEXT3);
    x += 40;
    static const struct { uint32_t col; const char *k; } KEY[] = {
        { C_MESH, "15 min" }, { 0x3FC6B8, "1 hour" }, { 0x3A86D0, "24 hours" }, { 0x55608A, "older" } };
    for (int i = 0; i < 4; i++) {
        circle_l(x + 3, y + 7, 3, KEY[i].col, 255);
        x = text(F_SMALL, x + 10, y + 1, KEY[i].k, C_TEXT2) + 12;
    }
    // Status: warm-up or trouble, centred, over the bottom of the map.
    char msg[160] = "";
    uint32_t col = C_WARN;
    char age[32];
    ago(g_stats.snapshotAt, age, sizeof age);
    uint32_t now = now_door();
    if (!g_haveAll) snprintf(msg, sizeof msg, "Loading what's been heard...");
    else if (g_stats.firstEver && !g_n) snprintf(msg, sizeof msg, "Listening... the map fills in as radios are heard. That takes a few minutes.");
    else if (g_stats.feed == LP_FEED_OFFLINE) { snprintf(msg, sizeof msg, "Can't reach the mesh right now. Showing what was heard until %s.", age); col = C_ERR; }
    else if (g_stats.feed != LP_FEED_LIVE) snprintf(msg, sizeof msg, "Listening to the mesh... new traffic can take a few minutes. Showing what was heard until %s.", age);
    else if (g_stats.startedAt && now - g_stats.startedAt < 180) snprintf(msg, sizeof msg, "Live. New traffic is arriving: the map fills in over a few minutes.");
    if (msg[0]) {
        float w = text_w(F_BODY, msg, 0) + 20;
        float mx = MAP_X + MAP_W / 2 - w / 2, my = MAP_Y + MAP_H - 44;
        rrect_l(mx, my, w, 20, 10, 0x000000, 185);
        rrect_stroke_l(mx, my, w, 20, 10, 1, col, 120);
        text(F_BODY, mx + 10, my + 3, msg, col);
    }
    float hx = 318, hy = FOOT_Y + 3;
    keycap_hint(&hx, hy, "/", "find");
    keycap_hint(&hx, hy, "C", "chat");
    keycap_hint(&hx, hy, "S", "stats");
    keycap_hint(&hx, hy, "L", "links");
    keycap_hint(&hx, hy, "?", "all keys");
}

// The layer chips over the map's top left, and a toast under them.
static void draw_map_chips(void)
{
    float x = MAP_X + 8;
    x = chip(x, MAP_Y + 8, "Links", g_showLinks, &g_chipLinks);
    chip(x, MAP_Y + 8, "Sensors", g_showSensors, &g_chipSensors);
    int32_t age = trace_time_ms() - g_toastAt;
    if (g_toast[0] && age < 4000) {
        int a = age > 3400 ? (int)(255 * (4000 - age) / 600) : 255;
        float w = text_w(F_BODY, g_toast, 0) + 20;
        float tx = MAP_X + MAP_W / 2 - w / 2, ty = MAP_Y + 36;
        rrect_l(tx, ty, w, 22, 11, 0x000000, a * 200 / 255);
        rrect_stroke_l(tx, ty, w, 22, 11, 1, C_MESH, a * 150 / 255);
        text_sp(F_BODY, tx + 10, ty + 4, g_toast, C_TEXT, a, 0);
    }
}

// ------------------------------------------------------------ home, find ---

static void go_to_node(int i, float zoom)
{
    if (i < 0 || i >= g_n) return;
    int st = g_nodes[i].w.state;
    if (!in_region(st)) set_region(st && g_haveGeo && st <= g_geo.count ? st : 0);
    g_sel = i;
    if (!node_visible(&g_nodes[i])) return;
    reset_view();
    int pane = g_view.npanes > 1 ? geo_pane_for_state(&g_view, &g_geo, st - 1) : 0;
    if (pane > 0) return;                                   // an inset: shown as it is
    g_zoom = zoom;
    clamp_view();
    float x, y;
    node_screen(&g_nodes[i], &x, &y);
    g_panX += MAP_X + MAP_W / 2.0f - x;
    g_panY += MAP_Y + MAP_H / 2.0f - y;
    clamp_view();
}

static void go_home(void)
{
    if (!g_haveHome) { toast("Set your town in the Weather door and your home shows up here."); return; }
    if (g_region && g_region != g_homeState) set_region(g_homeState ? g_homeState : 0);
    reset_view();
    float bx, by;
    if (geo_project(&g_view, &g_geo, g_homeState - 1, g_homeLat, g_homeLon, &bx, &by)) {
        int pane = g_view.npanes > 1 ? geo_pane_for_state(&g_view, &g_geo, g_homeState - 1) : 0;
        if (pane == 0) {
            g_zoom = g_region ? 2.5f : 5;
            clamp_view();
            float x, y;
            to_screen(bx, by, &x, &y);
            g_panX += MAP_X + MAP_W / 2.0f - x;
            g_panY += MAP_Y + MAP_H / 2.0f - y;
            clamp_view();
        }
    }
    int near = 0;
    uint32_t now = now_door();
    for (int i = 0; i < g_n; i++) {
        double km = node_km(&g_nodes[i].w);
        if (km >= 0 && km <= 80 && now - g_nodes[i].w.lastHeard < 86400) near++;
    }
    char msg[120], place[40];
    fit(F_BODY, g_homeName, 180, place, sizeof place);
    snprintf(msg, sizeof msg, "%s: %d radio%s within 50 miles heard today", place, near, near == 1 ? "" : "s");
    toast(msg);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && lower((unsigned char)p[k]) == lower((unsigned char)needle[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

static void run_find(void)
{
    g_nFound = 0;
    g_findSel = 0;
    if (!g_findLen) return;
    const char *q = g_find[0] == '!' ? g_find + 1 : g_find;
    // Newest first, up to 12.
    static int *cand = NULL;
    static int cap = 0;
    if (g_n > cap) { free(cand); cand = malloc(sizeof *cand * (size_t)g_n); cap = cand ? g_n : 0; }
    if (!cand) return;
    int nc = 0;
    for (int i = 0; i < g_n; i++) {
        const LpNodeWire *w = &g_nodes[i].w;
        char id[12];
        snprintf(id, sizeof id, "%08x", w->id);
        if (contains(w->name, q) || contains(w->shortName, q) || contains(id, q)) cand[nc++] = i;
    }
    for (int k = 0; k < 12 && k < nc; k++) {
        int best = k;
        for (int j = k + 1; j < nc; j++) if (g_nodes[cand[j]].w.lastHeard > g_nodes[cand[best]].w.lastHeard) best = j;
        int t = cand[k]; cand[k] = cand[best]; cand[best] = t;
        g_found[g_nFound++] = cand[k];
    }
}

static Rect find_row(int i) { return (Rect){ R_FINDBOX.x + 12, R_FINDBOX.y + 84 + i * 20, R_FINDBOX.w - 24, 19 }; }

static void draw_find(void)
{
    fill_d(0, 0, g_W, g_H, 0x02040A, 160);
    const Rect *P = &R_FINDBOX;
    rrect_l(P->x + 3, P->y + 5, P->w, P->h, 12, 0x000000, 110);
    rrect_l(P->x, P->y, P->w, P->h, 12, 0x101B30, 255);
    rrect_stroke_l(P->x, P->y, P->w, P->h, 12, 1, C_EDGE_HI, 255);
    text(F_HEAD, P->x + 16, P->y + 12, "Find a node", C_TEXT);
    text(F_SMALL, P->x + 16, P->y + 34, "by name, short name or ID (!1a2b3c4d)", C_TEXT3);
    float fx = P->x + 12, fy = P->y + 50, fw = P->w - 24;
    rrect_l(fx, fy, fw, 26, 8, 0x0B1124, 255);
    rrect_stroke_l(fx, fy, fw, 26, 8, 1.5f, C_MESH, 255);
    float tx = fx + 10;
    if (g_findLen) tx = text(F_BODY, tx, fy + 5, g_find, C_TEXT);
    else text(F_BODY, tx, fy + 5, "Start typing...", C_TEXT3);
    if ((trace_time_ms() / 530) % 2 == 0) fill_l(g_findLen ? tx + 1 : fx + 9, fy + 5, 1.5f, 16, C_MESH, 255);
    if (g_findLen && !g_nFound) text_c(F_BODY, P->x + P->w / 2, P->y + 130, "No node matches.", C_TEXT2);
    for (int i = 0; i < g_nFound; i++) {
        Rect r = find_row(i);
        const LpNodeWire *w = &g_nodes[g_found[i]].w;
        int on = i == g_findSel;
        if (on) rrect_l(r.x, r.y, r.w, r.h, 6, C_PANEL_HI, 255);
        char nm[40], when[24], id[12];
        fit(F_BODY, node_label(w), 150, nm, sizeof nm);
        text(F_BODY, r.x + 8, r.y + 2, nm, on ? C_TEXT : C_TEXT2);
        if (w->shortName[0] && w->name[0]) text(F_BOLD, r.x + 164, r.y + 2, w->shortName, C_MESH);
        snprintf(id, sizeof id, "!%08x", w->id);
        text(F_SMALL, r.x + 206, r.y + 4, id, C_TEXT3);
        ago(w->lastHeard, when, sizeof when);
        text_r(F_SMALL, r.x + r.w - 6, r.y + 4, state_abbr(w->state), C_TEXT3);
        (void)when;
    }
    float hx = P->x + 14, hy = P->y + P->h - 22;
    fill_l(P->x + 1, hy - 8, P->w - 2, 1, C_EDGE, 255);
    keycap_hint(&hx, hy, "Enter", "go");
    keycap_hint(&hx, hy, "Up/Dn", "move");
    keycap_hint(&hx, hy, "Esc", "close");
}

// ------------------------------------------------------------ stats page ---

static void bar_list(float x, float y, float w, const char *title, const char *const *names, const int *vals, int n, uint32_t col)
{
    label(x, y, title, C_TEXT3);
    y += 16;
    int max = 1;
    for (int i = 0; i < n; i++) if (vals[i] > max) max = vals[i];
    for (int i = 0; i < n; i++) {
        char nm[40], v[16];
        fit(F_SMALL, names[i], 110, nm, sizeof nm);
        text(F_SMALL, x, y, nm, C_TEXT2);
        float bw = (w - 160) * vals[i] / max;
        rrect_l(x + 116, y + 2, bw < 3 ? 3 : bw, 10, 3, col, 200);
        snprintf(v, sizeof v, "%d", vals[i]);
        text(F_SMALL, x + 120 + bw, y, v, C_TEXT3);
        y += 15;
    }
    if (!n) text(F_SMALL, x, y, "Nothing yet.", C_TEXT3);
}

static void draw_stats_box(void)
{
    fill_d(0, 0, g_W, g_H, 0x02040A, 175);
    const Rect *P = &R_STATSBOX;
    rrect_l(P->x + 3, P->y + 5, P->w, P->h, 12, 0x000000, 110);
    rrect_l(P->x, P->y, P->w, P->h, 12, 0x0E1829, 255);
    rrect_stroke_l(P->x, P->y, P->w, P->h, 12, 1, C_EDGE_HI, 255);
    text(F_HEAD, P->x + 16, P->y + 12, "The mesh in numbers", C_TEXT);
    text(F_SMALL, P->x + 16 + text_w(F_HEAD, "The mesh in numbers", 0) + 10, P->y + 16, region_name(g_region), C_TEXT3);

    // Traffic: packets per minute over the last 3 hours.
    float gx = P->x + 16, gy = P->y + 44, gw = P->w - 32, gh = 70;
    label(gx, gy, "PACKETS PER MINUTE, ALL US, LAST 3 HOURS", C_TEXT3);
    gy += 16;
    rrect_l(gx, gy, gw, gh, 6, 0x0A1322, 255);
    int max = 1;
    for (int i = 0; i < g_histCount; i++) if (g_hist[i] > max) max = g_hist[i];
    if (g_histCount > 1) {
        float px = 0, py = 0;
        for (int i = 0; i < g_histCount; i++) {
            float x = gx + 6 + (gw - 12) * i / (LP_HIST_MIN - 1) + (gw - 12) * (LP_HIST_MIN - g_histCount) / (LP_HIST_MIN - 1);
            float y = gy + gh - 6 - (gh - 14) * g_hist[i] / max;
            if (g_hist[i]) fill_l(x, y, 1.5f, gy + gh - 6 - y, C_MESH, 70);
            if (i) line_l(px, py, x, y, 1.4f, C_MESH, 230);
            px = x;
            py = y;
        }
        char m[64];
        uint32_t lagMin = now_door() / 60 > g_histEnd ? now_door() / 60 - g_histEnd : 0;
        if (lagMin > 5) snprintf(m, sizeof m, "peak %d/min  \xb7  last run ended %u min ago", max, lagMin);
        else snprintf(m, sizeof m, "peak %d/min", max);
        text_r(F_SMALL, gx + gw - 8, gy + 4, m, C_TEXT3);
        text(F_SMALL, gx + 8, gy + 4, "3 h ago", C_TEXT3);
    } else {
        text_c(F_SMALL, gx + gw / 2, gy + gh / 2 - 6, "The graph fills in while the listening post runs.", C_TEXT3);
    }

    // The counts, over the region.
    uint32_t now = now_door();
    int perHw[256] = { 0 }, perState[GEO_MAX_STATES + 1] = { 0 }, sensors = 0, near = 0, day = 0, week = 0, links = 0;
    for (int i = 0; i < g_n; i++) {
        const LpNodeWire *w = &g_nodes[i].w;
        if (!in_region(w->state)) continue;
        uint32_t age = now - w->lastHeard;
        if (age > (uint32_t)g_nodeDays * 86400u) continue;
        week++;
        if (w->state <= GEO_MAX_STATES) perState[w->state]++;
        if (age < 86400) { day++; if (w->hw) perHw[w->hw]++; }
        if ((w->flags & 8) && w->temp_dC != -32768) sensors++;
        double km = node_km(w);
        if (km >= 0 && km <= 80 && age < 86400) near++;
    }
    for (int i = 0; i < g_nLinks; i++)
        if (now - g_links[i].w.lastHeard < 86400 && g_links[i].ia >= 0 && g_links[i].ib >= 0 &&
            (in_region(g_nodes[g_links[i].ia].w.state) || in_region(g_nodes[g_links[i].ib].w.state))) links++;
    const char *names[8];
    int vals[8], n = 0;
    float cy = gy + gh + 12, colW = (P->w - 48) / 3;
    // Radios.
    for (n = 0; n < 8; n++) {
        int bi = 0, bv = 0;
        for (int h = 1; h < 256; h++) if (perHw[h] > bv) { bv = perHw[h]; bi = h; }
        if (!bv) break;
        perHw[bi] = 0;
        names[n] = lp_hw_name((unsigned)bi);
        vals[n] = bv;
    }
    bar_list(P->x + 16, cy, colW, "RADIOS HEARD TODAY", names, vals, n, C_ACCENT);
    // States (All US) or the busiest nodes here.
    if (!g_region) {
        for (n = 0; n < 8; n++) {
            int bi = 0, bv = 0;
            for (int s2 = 1; s2 <= g_geo.count && s2 <= GEO_MAX_STATES; s2++) if (perState[s2] > bv) { bv = perState[s2]; bi = s2; }
            if (!bv) break;
            perState[bi] = 0;
            names[n] = region_name(bi);
            vals[n] = bv;
        }
        bar_list(P->x + 24 + colW, cy, colW, "BUSIEST STATES, THIS WEEK", names, vals, n, C_MESH);
    }
    // The chattiest nodes (most packets heard).
    static int taken[16];
    int nt = 0;
    for (n = 0; n < 8; n++) {
        int bi = -1;
        uint32_t bv = 0;
        for (int i = 0; i < g_n; i++) {
            const LpNodeWire *w = &g_nodes[i].w;
            if (!in_region(w->state) || w->packets <= bv) continue;
            int dup = 0;
            for (int k = 0; k < nt; k++) if (taken[k] == i) dup = 1;
            if (!dup) { bv = w->packets; bi = i; }
        }
        if (bi < 0) break;
        taken[nt++] = bi;
        names[n] = node_label(&g_nodes[bi].w);
        vals[n] = (int)bv;
    }
    bar_list(g_region ? P->x + 24 + colW : P->x + 32 + 2 * colW, cy, colW, "MOST ACTIVE NODES (PACKETS)", names, vals, n, 0xFFD166);

    // The totals line.
    char tot[200];
    if (g_haveHome)
        snprintf(tot, sizeof tot, "%d heard today  \xb7  %d this week  \xb7  %d links  \xb7  %d sensors  \xb7  %d within 50 mi of %s",
                 day, week, links, sensors, near, g_homeName);
    else
        snprintf(tot, sizeof tot, "%d heard today  \xb7  %d this week  \xb7  %d links in the last day  \xb7  %d sensors reporting",
                 day, week, links, sensors);
    char tf[200];
    fit(F_BODY, tot, P->w - 32, tf, sizeof tf);
    text(F_BODY, P->x + 16, P->y + P->h - 56, tf, C_TEXT);
    float hx = P->x + 18, hy = P->y + P->h - 24;
    fill_l(P->x + 1, hy - 8, P->w - 2, 1, C_EDGE, 255);
    keycap_hint(&hx, hy, "Esc", "close");
    keycap_hint(&hx, hy, "R", "region");
}

// ------------------------------------------------------------ help ---

static void draw_help(void)
{
    fill_d(0, 0, g_W, g_H, 0x02040A, 175);
    Rect P = { 110, 50, 420, 380 };
    rrect_l(P.x + 3, P.y + 5, P.w, P.h, 12, 0x000000, 110);
    rrect_l(P.x, P.y, P.w, P.h, 12, 0x101B30, 255);
    rrect_stroke_l(P.x, P.y, P.w, P.h, 12, 1, C_EDGE_HI, 255);
    text(F_HEAD, P.x + 16, P.y + 12, "Keys", C_TEXT);
    text(F_SMALL, P.x + 60, P.y + 16, "everything here can also be clicked", C_TEXT3);
    static const char *const K[][2] = {
        { "Tab / Shift+Tab", "step through nodes, newest first" }, { "click", "pick a node, or a state on All US" },
        { "/  or  F", "find a node by name, short name or ID" }, { "H", "go to your town (from the Weather door)" },
        { "R", "choose a region" }, { "+  -  wheel", "zoom" }, { "arrows, drag", "move the map" },
        { "Home  0", "whole region again" }, { "L", "show or hide the links between radios" },
        { "E", "show sensor readings (temperature)" }, { "C", "the public chat" }, { "S", "the mesh in numbers" },
        { "Esc", "step back: node, zoom, region" }, { "Q", "quit" } };
    float y = P.y + 44;
    for (unsigned i = 0; i < sizeof K / sizeof *K; i++, y += 22) {
        text(F_BOLD, P.x + 20, y, K[i][0], C_MESH);
        text(F_BODY, P.x + 150, y, K[i][1], C_TEXT2);
    }
}

// ---------------------------------------------------------- overlays ---

static int picker_cols(void) { return 4; }
static int picker_rows(void) { return 14; }

static Rect picker_cell(int i)
{
    int col = i / picker_rows(), row = i % picker_rows();
    return (Rect){ R_PICKER.x + 16 + col * 136, R_PICKER.y + 52 + row * 22, 132, 20 };
}

static void draw_picker(void)
{
    fill_d(0, 0, g_W, g_H, 0x02040A, 175);
    const Rect *P = &R_PICKER;
    rrect_l(P->x + 3, P->y + 5, P->w, P->h, 12, 0x000000, 110);
    rrect_l(P->x, P->y, P->w, P->h, 12, 0x101B30, 255);
    rrect_stroke_l(P->x, P->y, P->w, P->h, 12, 1, C_EDGE_HI, 255);
    pin_icon(P->x + 26, P->y + 24, 18, C_MESH);
    text(F_HEAD, P->x + 42, P->y + 12, "Choose a region", C_TEXT);
    text(F_SMALL, P->x + 42 + text_w(F_HEAD, "Choose a region", 0) + 10, P->y + 16, "nodes are placed by where they are", C_TEXT3);
    int n = g_haveGeo ? g_geo.count + 1 : 1;
    for (int i = 0; i < n; i++) {
        Rect r = picker_cell(i);
        int on = i == g_pickSel, cur = i == g_region;
        if (on) {
            rrect_l(r.x, r.y, r.w, r.h, 6, C_PANEL_HI, 255);
            rrect_stroke_l(r.x, r.y, r.w, r.h, 6, 1, C_MESH, 200);
        }
        char nm[32];
        if (!i) {
            text(F_BOLD, r.x + 8, r.y + 3, "All US", on ? C_TEXT : C_TEXT2);
        } else {
            text(F_BOLD, r.x + 6, r.y + 3, g_geo.st[i - 1].abbr, cur ? C_MESH : C_TEXT3);
            fit(F_BODY, g_geo.st[i - 1].name, r.w - 34, nm, sizeof nm);
            text(F_BODY, r.x + 28, r.y + 3, nm, on ? C_TEXT : C_TEXT2);
        }
        if (cur) circle_l(r.x + r.w - 9, r.y + 10, 3, C_MESH, 255);
    }
    float hx = P->x + 18, hy = P->y + P->h - 24;
    fill_l(P->x + 1, hy - 8, P->w - 2, 1, C_EDGE, 255);
    keycap_hint(&hx, hy, "Arrows", "move");
    keycap_hint(&hx, hy, "A-Z", "jump");
    keycap_hint(&hx, hy, "Enter", "choose");
    keycap_hint(&hx, hy, "Esc", "cancel");
    text_r(F_SMALL, P->x + P->w - 16, hy + 1, "or click a state on the All US map", C_TEXT3);
}

static int picker_at(float mx, float my)
{
    int n = g_haveGeo ? g_geo.count + 1 : 1;
    for (int i = 0; i < n; i++) if (inside(picker_cell(i), mx, my)) return i;
    return -1;
}

static int chat_rows_visible(void) { return 20; }
static Rect g_chanChip[9], g_chatterChip;
static int  g_nChanChips = 0;

// A pill that can be clicked: on = filled.
static float chip(float x, float y, const char *label, int on, Rect *out)
{
    float w = text_w(F_BOLD, label, 0) + 16;
    int hot = g_mouseOver && inside((Rect){ x, y, w, 18 }, g_mx, g_my);
    rrect_l(x, y, w, 18, 9, on ? C_MESH : 0x000000, on ? 230 : hot ? 200 : 140);
    rrect_stroke_l(x, y, w, 18, 9, 1, on ? C_MESH : hot ? C_TEXT2 : C_EDGE_HI, 255);
    text(F_BOLD, x + 8, y + 2, label, on ? 0x04140A : hot ? C_TEXT : C_TEXT2);
    if (out) *out = (Rect){ x, y, w, 18 };
    return x + w + 6;
}

// The region's chat lines, oldest first, into idx; returns the count.
static int chat_indexes(int *idx, int max)
{
    int n = 0;
    for (int i = 0; i < g_nChat && n < max; i++) if (chat_shown(i)) idx[n++] = i;
    return n;
}

static void draw_chat_box(void)
{
    fill_d(0, 0, g_W, g_H, 0x02040A, 175);
    const Rect *P = &R_CHATBOX;
    rrect_l(P->x + 3, P->y + 5, P->w, P->h, 12, 0x000000, 110);
    rrect_l(P->x, P->y, P->w, P->h, 12, 0x0E1829, 255);
    rrect_stroke_l(P->x, P->y, P->w, P->h, 12, 1, C_EDGE_HI, 255);
    text(F_HEAD, P->x + 16, P->y + 12, "Public chat", C_TEXT);
    char sub[80];
    snprintf(sub, sizeof sub, "%s  \xb7  channels anyone can read", region_name(g_region));
    text(F_SMALL, P->x + 16 + text_w(F_HEAD, "Public chat", 0) + 10, P->y + 16, sub, C_TEXT3);
    // The channel chips (Tab), and the chatter toggle (F).
    char chans[8][20];
    int nc = chat_channels(chans, 8);
    float cx = P->x + 16;
    cx = chip(cx, P->y + 38, "All channels", g_chanSel < 0, &g_chanChip[0]);
    for (int i = 0; i < nc && cx < P->x + P->w - 220; i++) cx = chip(cx, P->y + 38, chans[i], g_chanSel == i, &g_chanChip[i + 1]);
    g_nChanChips = nc + 1;
    const char *cl = g_hideChatter ? "Test chatter hidden" : "Showing test chatter";
    chip(P->x + P->w - 16 - (text_w(F_BOLD, cl, 0) + 16), P->y + 38, cl, 0, &g_chatterChip);
    static int idx[4096];
    int n = chat_indexes(idx, 4096), rows = chat_rows_visible();
    if (g_chatBack > n - rows) g_chatBack = n - rows > 0 ? n - rows : 0;
    if (!n) {
        text_c(F_BODY, P->x + P->w / 2, P->y + 170, "Nothing said yet.", C_TEXT2);
        text_c(F_SMALL, P->x + P->w / 2, P->y + 190, g_hideChatter ? "Test chatter is hidden: press F to show it."
                                                                 : "It can be quiet for a long while: most radios only report in.", C_TEXT3);
    }
    float y = P->y + 66 + (rows - 1) * 15;
    for (int k = 0; k < rows && n - 1 - g_chatBack - k >= 0; k++) {
        const Chat *c = &g_chat[idx[n - 1 - g_chatBack - k]];
        char when[16], who[40], out[300], line[300];
        chat_time(c->time, when, sizeof when);
        text_r(F_SMALL, P->x + 46, y, when, C_TEXT3);
        text(F_SMALL, P->x + 52, y, state_abbr(c->state), C_TEXT3);
        fit(F_SBOLD, c->name[0] ? c->name : "(no name yet)", 130, who, sizeof who);
        float x = text(F_SBOLD, P->x + 74, y, who, c->name[0] ? C_MESH : C_TEXT3);
        snprintf(line, sizeof line, "%s", c->text);
        fit(F_SMALL, line, P->x + P->w - 16 - (x + 8), out, sizeof out);
        text(F_SMALL, x + 8, y, out, C_TEXT);
        y -= 15;
    }
    float hx = P->x + 18, hy = P->y + P->h - 24;
    fill_l(P->x + 1, hy - 8, P->w - 2, 1, C_EDGE, 255);
    keycap_hint(&hx, hy, "Up/Dn", "scroll");
    keycap_hint(&hx, hy, "PgUp/PgDn", "page");
    keycap_hint(&hx, hy, "Tab", "channel");
    keycap_hint(&hx, hy, "F", "chatter");
    keycap_hint(&hx, hy, "Esc", "close");
    if (n) {
        char pos[48];
        snprintf(pos, sizeof pos, "%d lines", n);
        text_r(F_SMALL, P->x + P->w - 16, hy + 1, pos, C_TEXT3);
    }
}

// --------------------------------------------------------- TRACE API ---

static void set_scale(int s)
{
    if (s == g_S && g_frame) return;
    uint32_t *nf = malloc((size_t)(LW * s) * (size_t)(LH * s) * 4);
    if (!nf) { if (g_frame) return; s = 1; nf = malloc(LW * LH * 4); }
    free(g_frame);
    g_frame = nf;
    g_S = s;
    g_W = LW * s;
    g_H = LH * s;
    clip_all();
}

static uint8_t *read_asset(const char *hash, int32_t *size)
{
    *size = trace_asset_size(hash);
    if (*size <= 0) return NULL;
    uint8_t *buf = malloc((size_t)*size);
    for (int32_t off = 0; buf && off < *size; ) {
        int32_t got = trace_asset_read(hash, off, buf + off, *size - off);
        if (got <= 0) { free(buf); return NULL; }
        off += got;
    }
    return buf;
}

static void load_states(void)
{
    int32_t size;
    uint8_t *raw = read_asset(g_statesHash, &size);
    if (!raw) return;
    g_haveGeo = geo_load(&g_geo, raw, (size_t)size);
    free(raw);
    project_all();
}

int32_t trace_init(void)
{
    set_scale(1);
    trace_mouse_mode(TRACE_MOUSE_POINTER);
    trace_set_tick(30);
#ifdef LP_DEMO
    demo_fill();
#endif
    return 0;
}

void *trace_alloc(int32_t size) { return size > 0 ? malloc((size_t)size) : NULL; }
void  trace_free(void *ptr)     { free(ptr); }

void trace_on_resize(int32_t width, int32_t height) { set_display(width, height); }

static void add_node(const LpNodeWire *w, int pulse)
{
    Node *n = find_node(w->id);
    if (!n) {
        if (g_n == g_cap) {
            int cap = g_cap ? g_cap * 2 : 4096;
            Node *nn = realloc(g_nodes, sizeof *nn * (size_t)cap);
            if (!nn) return;
            g_nodes = nn;
            g_cap = cap;
        }
        n = &g_nodes[g_n++];
        memset(n, 0, sizeof *n);
        n->w.id = w->id;
        if (g_n * 2 > g_idHashCap) id_rehash();
        else {
            uint32_t k = hash_id(w->id) & (uint32_t)(g_idHashCap - 1);
            while (g_idHash[k] >= 0) k = (k + 1) & (uint32_t)(g_idHashCap - 1);
            g_idHash[k] = g_n - 1;
        }
        g_linksDirty = 1;
    }
    n->w = *w;
    n->w.name[LP_WIRE_NAME - 1] = 0;
    if (pulse) n->pulseAt = trace_time_ms();
    project_node(n);
}

static uint32_t link_key(uint32_t a, uint32_t b) { return hash_id(a * 2654435761u ^ hash_id(b)); }

static void link_rehash(void)
{
    int cap = 4096;
    while (cap < g_nLinks * 2 + 16) cap *= 2;
    int32_t *h = malloc(sizeof *h * (size_t)cap);
    if (!h) return;
    for (int i = 0; i < cap; i++) h[i] = -1;
    for (int i = 0; i < g_nLinks; i++) {
        uint32_t k = link_key(g_links[i].w.a, g_links[i].w.b) & (uint32_t)(cap - 1);
        while (h[k] >= 0) k = (k + 1) & (uint32_t)(cap - 1);
        h[k] = i;
    }
    free(g_linkHash);
    g_linkHash = h;
    g_linkHashCap = cap;
}

static void add_link(const LpLinkWire *w)
{
    if (!g_linkHashCap) link_rehash();
    if (!g_linkHashCap) return;
    uint32_t k = link_key(w->a, w->b) & (uint32_t)(g_linkHashCap - 1);
    while (g_linkHash[k] >= 0) {
        Link *o = &g_links[g_linkHash[k]];
        if (o->w.a == w->a && o->w.b == w->b) { o->w = *w; return; }
        k = (k + 1) & (uint32_t)(g_linkHashCap - 1);
    }
    if (g_nLinks == g_capLinks) {
        int cap = g_capLinks ? g_capLinks * 2 : 1024;
        Link *nl = realloc(g_links, sizeof *nl * (size_t)cap);
        if (!nl) return;
        g_links = nl;
        g_capLinks = cap;
    }
    Link *l = &g_links[g_nLinks];
    l->w = *w;
    l->ia = node_index(w->a);
    l->ib = node_index(w->b);
    g_linkHash[k] = g_nLinks++;
    if (g_nLinks * 2 > g_linkHashCap) link_rehash();
}

// Each link's ends, found again after new nodes arrive.
static void resolve_links(void)
{
    if (!g_linksDirty) return;
    for (int i = 0; i < g_nLinks; i++) {
        g_links[i].ia = node_index(g_links[i].w.a);
        g_links[i].ib = node_index(g_links[i].w.b);
    }
    g_linksDirty = 0;
}

static int links_of(int node)
{
    int n = 0;
    uint32_t cut = now_door() - 86400;
    for (int i = 0; i < g_nLinks; i++)
        if ((g_links[i].ia == node || g_links[i].ib == node) && g_links[i].w.lastHeard >= cut) n++;
    return n;
}

static void on_chat(const uint8_t *p, uint32_t bytes, int count, int replace)
{
    if (replace) g_chatBuild = 0;
    uint32_t o = 0;
    for (int i = 0; i < count && o + sizeof(LpChatWire) <= bytes; i++) {
        LpChatWire w;
        memcpy(&w, p + o, sizeof w);
        o += sizeof w;
        if (o + w.nameLen + w.chanLen + w.textLen > bytes) break;
        if (g_chatBuild == g_capChat) {
            int cap = g_capChat ? g_capChat * 2 : 256;
            Chat *nc = realloc(g_chat, sizeof *nc * (size_t)cap);
            if (!nc) return;
            g_chat = nc;
            g_capChat = cap;
        }
        Chat *c = &g_chat[g_chatBuild++];
        memset(c, 0, sizeof *c);
        c->time = w.time;
        c->state = w.state;
        memcpy(c->name, p + o, w.nameLen < LP_WIRE_NAME - 1 ? w.nameLen : LP_WIRE_NAME - 1);
        o += w.nameLen;
        memcpy(c->chan, p + o, w.chanLen < 19 ? w.chanLen : 19);
        o += w.chanLen;
        memcpy(c->text, p + o, w.textLen < 239 ? w.textLen : 239);
        o += w.textLen;
    }
    g_nChat = g_chatBuild;
}

void trace_on_data(const char *data, int32_t length)
{
    if (length <= 0) return;
    if (length > 7 && !memcmp(data, "states=", 7)) {
        int n = length - 7 > 64 ? 64 : length - 7;
        memcpy(g_statesHash, data + 7, (size_t)n);
        g_statesHash[n] = 0;
        if (!g_haveGeo) load_states();
        return;
    }
    if (length == 5 && !memcmp(data, "start", 5)) {
        if (!g_started) { g_started = 1; send_msg(LP_OUT_READY, NULL, 0); }
        return;
    }
    if (length == 4 && !memcmp(data, "quit", 4)) { g_quitting = 1; return; }

    const char *nl = memchr(data, '\n', (size_t)length);
    if (!nl) return;
    const uint8_t *body = (const uint8_t *)nl + 1;
    int32_t bodyLen = length - (int32_t)(body - (const uint8_t *)data);
    if (bodyLen < (int32_t)sizeof(LpMsgHeader)) return;
    LpMsgHeader h;
    memcpy(&h, body, sizeof h);
    const uint8_t *p = body + sizeof h;
    if ((uint32_t)(bodyLen - (int32_t)sizeof h) < h.bytes) return;

    switch (h.type) {
    case LP_IN_HELLO:
        if (h.bytes >= sizeof(LpHello)) {
            LpHello hello;
            memcpy(&hello, p, sizeof hello);
            g_doorNow = hello.doorNow;
            g_recvMs = trace_time_ms();
            g_nodeDays = hello.nodeDays ? hello.nodeDays : 7;
            g_region = hello.region;
            if (g_haveGeo && g_region > g_geo.count) g_region = 0;
            g_haveHome = hello.haveHome;
            g_homeState = hello.homeState;
            g_homeLat = hello.homeLat1e5 / 1e5f;
            g_homeLon = hello.homeLon1e5 / 1e5f;
            memcpy(g_homeName, hello.homeName, sizeof g_homeName);
            g_homeName[sizeof g_homeName - 1] = 0;
            g_pickSel = g_region;
            project_all();
        }
        break;
    case LP_IN_NODES: {
        if (h.flags & 1) { g_n = 0; g_sel = g_hover = -1; }
        int n = h.count;
        if ((uint32_t)n * sizeof(LpNodeWire) > h.bytes) n = (int)(h.bytes / sizeof(LpNodeWire));
        for (int i = 0; i < n; i++) {
            LpNodeWire w;
            memcpy(&w, p + i * sizeof w, sizeof w);
            add_node(&w, g_haveAll && !(h.flags & 1));
        }
        break;
    }
    case LP_IN_DONE:
        g_haveAll = 1;
        break;
    case LP_IN_LINKS: {
        if (h.flags & 1) { g_nLinks = 0; link_rehash(); }
        int n = h.count;
        if ((uint32_t)n * sizeof(LpLinkWire) > h.bytes) n = (int)(h.bytes / sizeof(LpLinkWire));
        for (int i = 0; i < n; i++) {
            LpLinkWire w;
            memcpy(&w, p + i * sizeof w, sizeof w);
            add_link(&w);
        }
        break;
    }
    case LP_IN_HISTORY:
        if (h.bytes >= sizeof(LpHistHead)) {
            LpHistHead hh;
            memcpy(&hh, p, sizeof hh);
            int n = hh.count > LP_HIST_MIN ? LP_HIST_MIN : hh.count;
            if (sizeof hh + (uint32_t)n * 2 > h.bytes) n = (int)((h.bytes - sizeof hh) / 2);
            memcpy(g_hist, p + sizeof hh, (size_t)n * 2);
            g_histCount = n;
            g_histEnd = hh.endMinute;
        }
        break;
    case LP_IN_CHAT:
        on_chat(p, h.bytes, h.count, h.flags & 1);
        break;
    case LP_IN_STATS:
        if (h.bytes >= sizeof(LpStats)) {
            memcpy(&g_stats, p, sizeof g_stats);
            g_doorNow = g_stats.doorNow;
            g_recvMs = trace_time_ms();
        }
        break;
    default:
        break;
    }
}

// Set-1 scancodes.
#define SC_ESC   0x01
#define SC_MINUS 0x0C
#define SC_EQUAL 0x0D
#define SC_BACK  0x0E
#define SC_TAB   0x0F
#define SC_ENTER 0x1C
#define SC_UP    0x48
#define SC_DOWN  0x50
#define SC_LEFT  0x4B
#define SC_RIGHT 0x4D
#define SC_HOME  0x47
#define SC_END   0x4F
#define SC_PGUP  0x49
#define SC_PGDN  0x51
#define SC_KPMINUS 0x4A
#define SC_KPPLUS  0x4E

static char scancode_char(int sc)
{
    static const char lower[0x40] =
        "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0\0\0 ";
    if (sc < 0 || sc >= 0x40) return 0;
    return lower[sc];
}

static void picker_key(int sc)
{
    int n = g_haveGeo ? g_geo.count + 1 : 1, rows = picker_rows();
    switch (sc) {
    case SC_ESC: g_picker = 0; return;
    case SC_ENTER: g_picker = 0; set_region(g_pickSel); return;
    case SC_UP:    if (g_pickSel > 0) g_pickSel--; return;
    case SC_DOWN:  if (g_pickSel < n - 1) g_pickSel++; return;
    case SC_LEFT:  if (g_pickSel - rows >= 0) g_pickSel -= rows; return;
    case SC_RIGHT: if (g_pickSel + rows < n) g_pickSel += rows; return;
    case SC_HOME:  g_pickSel = 0; return;
    case SC_END:   g_pickSel = n - 1; return;
    default: {
        char c = scancode_char(sc);
        if (c >= 'a' && c <= 'z' && g_haveGeo) {
            for (int step = 1; step <= n; step++) {
                int i = (g_pickSel + step) % n;
                if (i && (g_geo.st[i - 1].name[0] | 0x20) == c) { g_pickSel = i; break; }
            }
        }
    }
    }
}

static void chat_key(int sc)
{
    switch (sc) {
    case SC_ESC: g_chatOpen = 0; return;
    case SC_UP:   g_chatBack++; return;
    case SC_DOWN: if (g_chatBack > 0) g_chatBack--; return;
    case SC_PGUP: g_chatBack += chat_rows_visible() - 1; return;
    case SC_PGDN: g_chatBack -= chat_rows_visible() - 1; if (g_chatBack < 0) g_chatBack = 0; return;
    case SC_HOME: g_chatBack = 1 << 20; return;
    case SC_END:  g_chatBack = 0; return;
    case SC_TAB: {
        char chans[8][20];
        int nc = chat_channels(chans, 8);
        g_chanSel = g_chanSel + 1 >= nc ? -1 : g_chanSel + 1;
        g_chatBack = 0;
        return;
    }
    default: {
        char ch = scancode_char(sc);
        if (ch == 'c' || ch == 'q') g_chatOpen = 0;
        else if (ch == 'f') { g_hideChatter = !g_hideChatter; g_chatBack = 0; }
    }
    }
}

static void find_key(int sc, int shift)
{
    switch (sc) {
    case SC_ESC: g_findOpen = 0; return;
    case SC_ENTER: if (g_nFound) { g_findOpen = 0; go_to_node(g_found[g_findSel], 4); } return;
    case SC_UP: if (g_findSel > 0) g_findSel--; return;
    case SC_DOWN: if (g_findSel < g_nFound - 1) g_findSel++; return;
    case SC_BACK: if (g_findLen) { g_find[--g_findLen] = 0; run_find(); } return;
    default: {
        char ch = scancode_char(sc);
        if (shift && ch == '1') ch = '!';
        if (shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '!' ||
                 ch == '-' || ch == '.';
        if (ok && g_findLen < (int)sizeof g_find - 1 && !(ch == ' ' && !g_findLen)) {
            g_find[g_findLen++] = ch;
            g_find[g_findLen] = 0;
            run_find();
        }
    }
    }
}

static void open_find(void) { g_findOpen = 1; g_findLen = 0; g_find[0] = 0; g_nFound = 0; g_findSel = 0; }

static void toggle_links(void)
{
    g_showLinks = !g_showLinks;
    int n = 0;
    uint32_t now = now_door();
    for (int i = 0; i < g_nLinks; i++) if (now - g_links[i].w.lastHeard < 86400) n++;
    char m[96];
    if (g_showLinks) snprintf(m, sizeof m, "Links on: %d heard in the last day. Green is a strong signal, red a weak one.", n);
    else snprintf(m, sizeof m, "Links off");
    toast(m);
}

static void toggle_sensors(void)
{
    g_showSensors = !g_showSensors;
    int n = 0;
    for (int i = 0; i < g_n; i++) if ((g_nodes[i].w.flags & 8) && g_nodes[i].w.temp_dC != -32768 && in_region(g_nodes[i].w.state)) n++;
    char m[96];
    if (g_showSensors) snprintf(m, sizeof m, "%d sensor%s reporting here (temperature in \xb0" "F)", n, n == 1 ? "" : "s");
    else snprintf(m, sizeof m, "Sensors off");
    toast(m);
}

static void open_picker(void) { g_picker = 1; g_pickSel = g_region; }

static int hint_layer_now(void)
{
    return g_helpOpen ? HL_HELP : g_findOpen ? HL_FIND : g_picker ? HL_PICKER : g_chatOpen ? HL_CHAT
         : g_statsOpen ? HL_STATS : HL_MAP;
}

// The end of a press on the map, however TERMinator reports it: the button's release, or a position update that
// already says no button is held (which can arrive first). A press that didn't drag and didn't land on a node
// picks the state under it on the All US map, or clears the choice.
static void end_press(void)
{
    if (g_pressed && !g_dragging && !g_pressedNode) {
        if (!g_region && g_zoom == 1) {
            int s = state_at(g_pressX, g_pressY);
            if (s > 0) set_region(s);
            else g_sel = -1;
        } else {
            g_sel = -1;
        }
    }
    g_pressed = g_dragging = g_pressedNode = 0;
}

void trace_on_input(int32_t type, int32_t flags, int32_t a, int32_t b, int32_t c)
{
    switch (type) {
    case 1: {   // key
        if (a == 0x2A || a == 0x36) { g_shift = flags & 1; break; }
        if (!(flags & 1)) break;
        int shift = (flags & 4) != 0 || g_shift;
        if (g_helpOpen) { g_helpOpen = 0; break; }
        if (g_findOpen) { find_key(a, shift); break; }
        if (g_picker) { picker_key(a); break; }
        if (g_chatOpen) { chat_key(a); break; }
        if (g_statsOpen) {
            if (a == SC_ESC || scancode_char(a) == 's' || scancode_char(a) == 'q') g_statsOpen = 0;
            else if (scancode_char(a) == 'r') { g_statsOpen = 0; open_picker(); }
            break;
        }
        float pan = 60;
        switch (a) {
        case SC_ESC:
            if (g_sel >= 0) g_sel = -1;
            else if (g_zoom != 1) reset_view();
            else if (g_region) set_region(0);
            else g_quitting = 1;
            break;
        case SC_TAB:   step_node(shift ? -1 : 1); break;
        case SC_UP:    g_panY += pan; clamp_view(); break;
        case SC_DOWN:  g_panY -= pan; clamp_view(); break;
        case SC_LEFT:  g_panX += pan; clamp_view(); break;
        case SC_RIGHT: g_panX -= pan; clamp_view(); break;
        case SC_EQUAL: case SC_KPPLUS:  zoom_at(1.5f, MAP_X + MAP_W / 2.0f, MAP_Y + MAP_H / 2.0f); break;
        case SC_MINUS: case SC_KPMINUS: zoom_at(1 / 1.5f, MAP_X + MAP_W / 2.0f, MAP_Y + MAP_H / 2.0f); break;
        case SC_HOME:  reset_view(); break;
        default: {
            char ch = scancode_char(a);
            if (ch == 'q') g_quitting = 1;
            else if (ch == 'r') open_picker();
            else if (ch == 'c') { g_chatOpen = 1; g_chatBack = 0; }
            else if (ch == '0') reset_view();
            else if (ch == 'a') set_region(0);
            else if (ch == '/' && shift) g_helpOpen = 1;
            else if (ch == '/' || ch == 'f') open_find();
            else if (ch == 'h') go_home();
            else if (ch == 's') g_statsOpen = 1;
            else if (ch == 'l') toggle_links();
            else if (ch == 'e') toggle_sensors();
            break;
        }
        }
        break;
    }
    case TRACE_INPUT_MOUSE_POS:
        g_mx = mouse_lx(b);
        g_my = mouse_ly(c);
        g_mouseOver = flags & 1;
        if (g_picker) {
            int i = picker_at(g_mx, g_my);
            if (i >= 0) g_pickSel = i;
            g_hover = -1;
            break;
        }
        if (g_findOpen) {
            for (int i = 0; i < g_nFound; i++) if (inside(find_row(i), g_mx, g_my)) g_findSel = i;
            g_hover = -1;
            break;
        }
        if (g_chatOpen || g_statsOpen || g_helpOpen) { g_hover = -1; break; }
        if (g_pressed && (a & 1)) {
            float dx = g_mx - g_pressX, dy = g_my - g_pressY;
            if (!g_dragging && dx * dx + dy * dy > 16) g_dragging = 1;
            if (g_dragging) {
                g_panX = g_pressPanX + dx;
                g_panY = g_pressPanY + dy;
                clamp_view();
            }
        } else if (g_pressed && !(a & 1)) {
            end_press();
        }
        g_hover = (!g_dragging && g_my >= MAP_Y && g_my < MAP_Y + MAP_H) ? node_at(g_mx, g_my) : -1;
        break;
    case TRACE_INPUT_MOUSE_BUTTON: {
        int pressed = flags & 1, btn = a;
        if (btn == 4 || btn == 5) {
            if (!pressed) break;
            if (g_picker) {
                int n = g_haveGeo ? g_geo.count + 1 : 1;
                g_pickSel += btn == 4 ? -1 : 1;
                if (g_pickSel < 0) g_pickSel = 0;
                if (g_pickSel >= n) g_pickSel = n - 1;
            } else if (g_chatOpen) {
                if (btn == 4) g_chatBack += 3;
                else { g_chatBack -= 3; if (g_chatBack < 0) g_chatBack = 0; }
            } else if (g_my >= MAP_Y && g_my < MAP_Y + MAP_H) {
                zoom_at(btn == 4 ? 1.25f : 1 / 1.25f, g_mx, g_my);
            }
            break;
        }
        if (btn != 1 || !g_mouseOver) break;
        if (pressed && !g_pressed) {
            int layer = hint_layer_now();
            for (int i = 0; i < g_nHints; i++)
                if (g_hints[i].layer == layer && inside(g_hints[i].r, g_mx, g_my)) {
                    trace_on_input(1, 1 | (g_hints[i].shift ? 4 : 0), g_hints[i].sc, 0, 0);
                    return;
                }
        }
        if (g_helpOpen) { if (pressed) g_helpOpen = 0; break; }
        if (g_findOpen) {
            if (!pressed) break;
            for (int i = 0; i < g_nFound; i++)
                if (inside(find_row(i), g_mx, g_my)) { g_findOpen = 0; go_to_node(g_found[i], 4); return; }
            if (!inside(R_FINDBOX, g_mx, g_my)) g_findOpen = 0;
            break;
        }
        if (g_statsOpen) { if (pressed && !inside(R_STATSBOX, g_mx, g_my)) g_statsOpen = 0; break; }
        if (g_picker) {
            if (!pressed) break;
            int i = picker_at(g_mx, g_my);
            if (i >= 0) { g_picker = 0; set_region(i); }
            else if (!inside(R_PICKER, g_mx, g_my)) g_picker = 0;
            break;
        }
        if (g_chatOpen) {
            if (!pressed) break;
            if (!inside(R_CHATBOX, g_mx, g_my)) { g_chatOpen = 0; break; }
            if (inside(g_chatterChip, g_mx, g_my)) { g_hideChatter = !g_hideChatter; g_chatBack = 0; break; }
            for (int i = 0; i < g_nChanChips; i++)
                if (inside(g_chanChip[i], g_mx, g_my)) { g_chanSel = i - 1; g_chatBack = 0; break; }
            break;
        }
        if (pressed) {
            if (inside(R_CLOSE, g_mx, g_my)) { g_quitting = 1; break; }
            if (inside(g_regionBtn, g_mx, g_my)) { open_picker(); break; }
            if (inside(R_ZIN, g_mx, g_my)) { zoom_at(1.5f, MAP_X + MAP_W / 2.0f, MAP_Y + MAP_H / 2.0f); break; }
            if (inside(R_ZOUT, g_mx, g_my)) { zoom_at(1 / 1.5f, MAP_X + MAP_W / 2.0f, MAP_Y + MAP_H / 2.0f); break; }
            if (inside(R_ZRESET, g_mx, g_my)) { reset_view(); break; }
            if (inside(R_ZHOME, g_mx, g_my)) { go_home(); break; }
            if (inside(g_chipLinks, g_mx, g_my)) { toggle_links(); break; }
            if (inside(g_chipSensors, g_mx, g_my)) { toggle_sensors(); break; }
            if (inside(R_CHATPANEL, g_mx, g_my)) { g_chatOpen = 1; g_chatBack = 0; break; }
            if (inside(R_STATSPANEL, g_mx, g_my)) { g_statsOpen = 1; break; }
            if (g_my >= MAP_Y && g_my < MAP_Y + MAP_H) {
                // A node is picked the moment it's pressed; the press can still become a drag that pans.
                int hit = node_at(g_mx, g_my);
                if (hit >= 0) g_sel = hit;
                g_pressedNode = hit >= 0;
                g_pressed = 1;
                g_dragging = 0;
                g_pressX = g_mx;
                g_pressY = g_my;
                g_pressPanX = g_panX;
                g_pressPanY = g_panY;
            }
            break;
        }
        end_press();
        break;
    }
    case 6:
        g_quitting = 1;
        break;
    default:
        break;
    }
}

void trace_update(void)
{
    if (g_quitting) trace_quit(0);
    if (g_wantS != g_S) { set_scale(g_wantS); g_baseRegion = -2; }
    if (!g_frame) return;
    if (g_haveGeo && g_projFor != g_region) project_all();

    clip_all();
    g_nHints = 0;
    g_hintLayer = HL_MAP;
    fill_d(0, 0, g_W, g_H, C_BG, 255);
    draw_base();
    clip_l(MAP_X, MAP_Y, MAP_W, MAP_H);
    draw_links();
    draw_nodes();
    home_marker();
    clip_all();
    map_button(R_ZIN, 0);
    map_button(R_ZOUT, 1);
    map_button(R_ZRESET, 2);
    map_button(R_ZHOME, 3);
    draw_map_chips();
    draw_map_foot();
    draw_node_panel();
    draw_chat_panel();
    draw_stats_panel();
    draw_bar();
    if (g_picker) { g_hintLayer = HL_PICKER; draw_picker(); }
    if (g_chatOpen) { g_hintLayer = HL_CHAT; draw_chat_box(); }
    if (g_statsOpen) { g_hintLayer = HL_STATS; draw_stats_box(); }
    if (g_findOpen) { g_hintLayer = HL_FIND; draw_find(); }
    if (g_helpOpen) { g_hintLayer = HL_HELP; draw_help(); }
    present_fitted(TRACE_PRESENT_ASPECT_4_3);
}

#ifdef LP_DEMO
#include "demo.h"
#endif
