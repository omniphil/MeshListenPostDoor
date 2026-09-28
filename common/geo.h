// geo.h - the Listening Post's map: US state outlines (states.bin), which state
// a point is in, and the projections the TRACE and ANSI maps draw with.
//
// Header-only, shared by the collector (state lookup), the door (ANSI map) and
// the module (TRACE map). No allocation beyond one block per map.
//
// Regions: 0 = All US, 1..count = the states in states.bin order (by name).
// All US is drawn as the usual atlas page: the lower 48 in an Albers equal-area
// projection, with Alaska, Hawaii and Puerto Rico as insets along the bottom.
// A single state gets an Albers projection centred on it.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GEO_MAX_STATES 64
#define GEO_ALL_US     0

typedef struct { float lat, lon; } GeoPt;

typedef struct {
    char   abbr[4];
    char   name[24];
    float  minLat, minLon, maxLat, maxLon;
    int    rings, firstRing;              // rings index into GeoMap.ringStart/ringLen
} GeoState;

typedef struct {
    int       count;
    GeoState  st[GEO_MAX_STATES];
    int       nRings, nPts;
    int      *ringStart, *ringLen;        // into pts
    GeoPt    *pts;
    void     *block;                      // the one allocation
} GeoMap;

static uint32_t geo_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float    geo_f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

static void geo_free(GeoMap *g)
{
    free(g->block);
    memset(g, 0, sizeof *g);
}

// Parses states.bin. Returns 0 on anything malformed.
static int geo_load(GeoMap *g, const uint8_t *b, size_t len)
{
    memset(g, 0, sizeof *g);
    if (len < 8 || memcmp(b, "STS1", 4)) return 0;
    uint32_t n = geo_u32(b + 4);
    if (n == 0 || n > GEO_MAX_STATES) return 0;
    // First pass: count, and check every length before trusting it.
    size_t o = 8;
    uint32_t rings = 0, pts = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (o + 52 > len) return 0;
        uint32_t r = geo_u32(b + o + 44);
        o += 52;
        for (uint32_t k = 0; k < r; k++) {
            if (o + 4 > len) return 0;
            uint32_t c = geo_u32(b + o);
            if (c > 100000 || o + 4 + (size_t)c * 8 > len) return 0;
            o += 4 + (size_t)c * 8;
            pts += c;
        }
        rings += r;
    }
    size_t bytes = (size_t)rings * 2 * sizeof(int) + (size_t)pts * sizeof(GeoPt);
    g->block = malloc(bytes ? bytes : 1);
    if (!g->block) return 0;
    g->ringStart = (int *)g->block;
    g->ringLen = g->ringStart + rings;
    g->pts = (GeoPt *)(g->ringLen + rings);
    o = 8;
    int ri = 0, pi = 0;
    for (uint32_t i = 0; i < n; i++) {
        GeoState *s = &g->st[i];
        memcpy(s->abbr, b + o, 4);
        s->abbr[3] = 0;
        memcpy(s->name, b + o + 4, 24);
        s->name[23] = 0;
        s->minLat = geo_f32(b + o + 28);
        s->minLon = geo_f32(b + o + 32);
        s->maxLat = geo_f32(b + o + 36);
        s->maxLon = geo_f32(b + o + 40);
        s->rings = (int)geo_u32(b + o + 44);
        s->firstRing = ri;
        o += 52;
        for (int k = 0; k < s->rings; k++) {
            int c = (int)geo_u32(b + o);
            o += 4;
            g->ringStart[ri] = pi;
            g->ringLen[ri] = c;
            ri++;
            for (int j = 0; j < c; j++, o += 8) {
                g->pts[pi].lat = geo_f32(b + o);
                g->pts[pi].lon = geo_f32(b + o + 4);
                pi++;
            }
        }
    }
    g->count = (int)n;
    g->nRings = ri;
    g->nPts = pi;
    return 1;
}

static int geo_find_abbr(const GeoMap *g, const char *abbr)
{
    for (int i = 0; i < g->count; i++)
        if (g->st[i].abbr[0] == abbr[0] && g->st[i].abbr[1] == abbr[1]) return i;
    return -1;
}

// Even-odd point-in-polygon over all of a state's rings (so lakes and holes work).
static int geo_in_state(const GeoMap *g, int i, float lat, float lon)
{
    const GeoState *s = &g->st[i];
    if (s->abbr[0] == 'A' && s->abbr[1] == 'K' && lon > 0) lon -= 360;
    if (lat < s->minLat || lat > s->maxLat || lon < s->minLon || lon > s->maxLon) return 0;
    int in = 0;
    for (int r = s->firstRing; r < s->firstRing + s->rings; r++) {
        const GeoPt *p = g->pts + g->ringStart[r];
        int n = g->ringLen[r];
        for (int a = 0, b = n - 1; a < n; b = a++) {
            if ((p[a].lat > lat) != (p[b].lat > lat) &&
                lon < (p[b].lon - p[a].lon) * (lat - p[a].lat) / (p[b].lat - p[a].lat) + p[a].lon)
                in = !in;
        }
    }
    return in;
}

// The state a point is in: 0-based index, or -1 (sea, Canada, Mexico...).
static int geo_state_at(const GeoMap *g, float lat, float lon)
{
    for (int i = 0; i < g->count; i++)
        if (geo_in_state(g, i, lat, lon)) return i;
    return -1;
}

// ------------------------------------------------------------ projection ---

typedef struct {
    double lat0, lon0, n, C, rho0;     // Albers constants (radians)
    double scale, ox, oy;              // projected -> screen
    float  x, y, w, h;                 // the pane on screen
    char   only[4];                    // an inset: only this state ("" = everything else)
} GeoPane;

typedef struct {
    int     region, npanes;
    GeoPane pane[4];
} GeoView;

#define GEO_D2R 0.017453292519943295
#define GEO_PI  3.14159265358979323846

static void geo_albers_init(GeoPane *p, double lat0, double lon0, double p1, double p2)
{
    lat0 *= GEO_D2R; lon0 *= GEO_D2R; p1 *= GEO_D2R; p2 *= GEO_D2R;
    p->lat0 = lat0;
    p->lon0 = lon0;
    p->n = (sin(p1) + sin(p2)) / 2;
    p->C = cos(p1) * cos(p1) + 2 * p->n * sin(p1);
    p->rho0 = sqrt(p->C - 2 * p->n * sin(lat0)) / p->n;
}

static void geo_albers(const GeoPane *p, double lat, double lon, double *x, double *y)
{
    lat *= GEO_D2R;
    lon *= GEO_D2R;
    double d = lon - p->lon0;
    while (d > GEO_PI) d -= 2 * GEO_PI;
    while (d < -GEO_PI) d += 2 * GEO_PI;
    double rho = sqrt(p->C - 2 * p->n * sin(lat)) / p->n, th = p->n * d;
    *x = rho * sin(th);
    *y = p->rho0 - rho * cos(th);
}

static void geo_to_screen(const GeoPane *p, double lat, double lon, float *sx, float *sy)
{
    double x, y;
    geo_albers(p, lat, lon, &x, &y);
    *sx = (float)(p->ox + x * p->scale);
    *sy = (float)(p->oy - y * p->scale);
}

// Fits a pane's projection to its rectangle around a lat/lon box (margin in pixels).
static void geo_fit(GeoPane *p, double minLat, double minLon, double maxLat, double maxLon, float margin)
{
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i <= 16; i++)
        for (int j = 0; j <= 16; j++) {
            double x, y;
            geo_albers(p, minLat + (maxLat - minLat) * i / 16, minLon + (maxLon - minLon) * j / 16, &x, &y);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    double w = p->w - 2 * margin, h = p->h - 2 * margin;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    double sx = w / (x1 - x0 > 1e-9 ? x1 - x0 : 1e-9), sy = h / (y1 - y0 > 1e-9 ? y1 - y0 : 1e-9);
    p->scale = sx < sy ? sx : sy;
    p->ox = p->x + p->w / 2 - (x0 + x1) / 2 * p->scale;
    p->oy = p->y + p->h / 2 + (y0 + y1) / 2 * p->scale;
}

static void geo_pane_rect(GeoPane *p, float x, float y, float w, float h, const char *only)
{
    p->x = x; p->y = y; p->w = w; p->h = h;
    memset(p->only, 0, sizeof p->only);
    if (only) memcpy(p->only, only, 2);
}

// Builds the view for a region over a rectangle.
static void geo_view(GeoView *v, const GeoMap *g, int region, float x, float y, float w, float h)
{
    memset(v, 0, sizeof *v);
    v->region = region;
    if (region > 0 && region <= g->count) {
        const GeoState *s = &g->st[region - 1];
        GeoPane *p = &v->pane[0];
        double la = (s->minLat + s->maxLat) / 2, span = s->maxLat - s->minLat;
        geo_albers_init(p, la, (s->minLon + s->maxLon) / 2, la - span / 4, la + span / 4);
        geo_pane_rect(p, x, y, w, h, NULL);
        geo_fit(p, s->minLat, s->minLon, s->maxLat, s->maxLon, h * 0.06f);
        v->npanes = 1;
        return;
    }
    // The lower 48 over the whole rectangle; the insets along the bottom.
    GeoPane *c = &v->pane[0];
    geo_albers_init(c, 37.5, -96, 29.5, 45.5);
    geo_pane_rect(c, x, y, w, h, NULL);
    geo_fit(c, 24.4, -124.8, 49.4, -66.9, h * 0.03f);
    GeoPane *ak = &v->pane[1];
    geo_albers_init(ak, 50, -154, 55, 65);
    geo_pane_rect(ak, x, y + h * 0.66f, w * 0.24f, h * 0.34f, "AK");
    geo_fit(ak, 51.2, -179.5, 71.4, -129.9, 2);
    GeoPane *hi = &v->pane[2];
    geo_albers_init(hi, 3, -157, 8, 18);
    geo_pane_rect(hi, x + w * 0.25f, y + h * 0.80f, w * 0.14f, h * 0.20f, "HI");
    geo_fit(hi, 18.9, -160.3, 22.3, -154.8, 2);
    GeoPane *pr = &v->pane[3];
    geo_albers_init(pr, 18, -66.5, 17.5, 18.5);
    geo_pane_rect(pr, x + w * 0.80f, y + h * 0.88f, w * 0.09f, h * 0.10f, "PR");
    geo_fit(pr, 17.9, -67.3, 18.6, -65.2, 1);
    v->npanes = 4;
}

// Which pane draws a state (All US), or -1 when the view doesn't show it.
static int geo_pane_for_state(const GeoView *v, const GeoMap *g, int st)
{
    if (v->npanes == 1 || st < 0) return 0;
    const char *ab = g->st[st].abbr;
    for (int i = 1; i < v->npanes; i++)
        if (v->pane[i].only[0] == ab[0] && v->pane[i].only[1] == ab[1]) return i;
    return 0;
}

// Where a point goes on screen. The state (or -1) picks the pane in the All US
// view; a point with no state is placed on the lower 48 only if it's near it.
// Returns 0 when the view doesn't show it.
static int geo_project(const GeoView *v, const GeoMap *g, int st, float lat, float lon, float *sx, float *sy)
{
    int pi = geo_pane_for_state(v, g, st);
    if (v->npanes > 1 && st < 0 && (lat < 23 || lat > 50 || lon < -128 || lon > -64)) return 0;
    const GeoPane *p = &v->pane[pi];
    geo_to_screen(p, lat, lon, sx, sy);
    if (v->npanes > 1 && pi > 0) {
        if (*sx < p->x || *sx > p->x + p->w || *sy < p->y || *sy > p->y + p->h) return 0;
    }
    return 1;
}
