// Drawing for the News module: anti-aliased shapes and text in a 640x480
// logical space, rendered at 1x or 2x. The same primitives as the Weather
// module (BBSGames/Weather/module/src/wxmodule.c), which is where they came from.
#pragma once
// Not every door uses every primitive.
#pragma clang diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-function"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fonts.h"

#define LW 640                  // logical layout size
#define LH 480
#define PI_F 3.14159265f

// ------------------------------------------------------------------ frame ---

static uint32_t *g_frame = NULL;
static int g_S = 1, g_W = LW, g_H = LH;         // device scale and size
static int g_wantS = 1;                         // what the last resize asked for
static int g_clipX0, g_clipY0, g_clipX1, g_clipY1;

static inline int D(float v) { return (int)lroundf(v * (float)g_S); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

static inline uint32_t mix(uint32_t a, uint32_t b, int t)
{
    if (t <= 0) return a & 0xFFFFFF;
    if (t >= 255) return b & 0xFFFFFF;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (uint32_t)((ar + ((br - ar) * t) / 255) << 16 | (ag + ((bg - ag) * t) / 255) << 8 |
                      (ab + ((bb - ab) * t) / 255));
}

static inline uint32_t mixf(uint32_t a, uint32_t b, float t) { return mix(a, b, (int)(clampf(t, 0, 1) * 255)); }

static void clip_all(void) { g_clipX0 = 0; g_clipY0 = 0; g_clipX1 = g_W; g_clipY1 = g_H; }

static void clip_l(float x, float y, float w, float h)
{
    g_clipX0 = D(x); g_clipY0 = D(y); g_clipX1 = D(x + w); g_clipY1 = D(y + h);
    if (g_clipX0 < 0) g_clipX0 = 0;
    if (g_clipY0 < 0) g_clipY0 = 0;
    if (g_clipX1 > g_W) g_clipX1 = g_W;
    if (g_clipY1 > g_H) g_clipY1 = g_H;
}

static inline void blendp(int x, int y, uint32_t rgb, int a)
{
    if (a <= 0 || x < g_clipX0 || y < g_clipY0 || x >= g_clipX1 || y >= g_clipY1) return;
    uint32_t *p = &g_frame[y * g_W + x];
    *p = (a >= 255 ? rgb : mix(*p, rgb, a)) | 0xFF000000u;
}

static void fill_d(int x0, int y0, int x1, int y1, uint32_t rgb, int a)
{
    if (x0 < g_clipX0) x0 = g_clipX0;
    if (y0 < g_clipY0) y0 = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    if (a <= 0 || x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = g_frame + y * g_W;
        if (a >= 255) for (int x = x0; x < x1; x++) row[x] = rgb | 0xFF000000u;
        else          for (int x = x0; x < x1; x++) row[x] = mix(row[x], rgb, a) | 0xFF000000u;
    }
}

static void fill_l(float x, float y, float w, float h, uint32_t rgb, int a)
{
    fill_d(D(x), D(y), D(x + w), D(y + h), rgb, a);
}

// Vertical gradient; alpha may fade too (a0 at the top, a1 at the bottom).
static void vgrad_l(float x, float y, float w, float h, uint32_t top, uint32_t bot, int a0, int a1)
{
    int x0 = D(x), y0 = D(y), x1 = D(x + w), y1 = D(y + h);
    int n = y1 - y0;
    for (int yy = y0; yy < y1; yy++) {
        float t = n > 1 ? (float)(yy - y0) / (float)(n - 1) : 0;
        fill_d(x0, yy, x1, yy + 1, mixf(top, bot, t), (int)lerpf((float)a0, (float)a1, t));
    }
}

// Coverage of a rounded box (device units) at a pixel centre, from its signed distance.
static inline float rbox_cov(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
    return clampf(0.5f - d, 0, 1);
}

static void rrect_l(float x, float y, float w, float h, float r, uint32_t rgb, int a)
{
    int X0 = D(x), Y0 = D(y), X1 = D(x + w), Y1 = D(y + h);
    float R = r * g_S;
    int band = (int)ceilf(R);
    float cx = (X0 + X1) * 0.5f, cy = (Y0 + Y1) * 0.5f, hw = (X1 - X0) * 0.5f, hh = (Y1 - Y0) * 0.5f;
    for (int yy = Y0; yy < Y1; yy++) {
        if (yy >= Y0 + band && yy < Y1 - band) { fill_d(X0, yy, X1, yy + 1, rgb, a); continue; }
        fill_d(X0 + band, yy, X1 - band, yy + 1, rgb, a);
        for (int xx = X0; xx < X0 + band; xx++)
            blendp(xx, yy, rgb, (int)(a * rbox_cov(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, R)));
        for (int xx = X1 - band; xx < X1; xx++)
            blendp(xx, yy, rgb, (int)(a * rbox_cov(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, R)));
    }
}

static void rrect_stroke_l(float x, float y, float w, float h, float r, float width, uint32_t rgb, int a)
{
    int X0 = D(x), Y0 = D(y), X1 = D(x + w), Y1 = D(y + h);
    float R = r * g_S, W = width * g_S;
    float cx = (X0 + X1) * 0.5f, cy = (Y0 + Y1) * 0.5f, hw = (X1 - X0) * 0.5f, hh = (Y1 - Y0) * 0.5f;
    float Ri = R - W > 0 ? R - W : 0;
    int band = (int)ceilf(R > W ? R : W) + 1;
    for (int yy = Y0; yy < Y1; yy++) {
        int mid = yy >= Y0 + band && yy < Y1 - band;
        for (int xx = X0; xx < X1; xx++) {
            if (mid && xx == X0 + band) xx = X1 - band;
            float px = xx + 0.5f, py = yy + 0.5f;
            float c = rbox_cov(px, py, cx, cy, hw, hh, R) - rbox_cov(px, py, cx, cy, hw - W, hh - W, Ri);
            if (c > 0) blendp(xx, yy, rgb, (int)(a * c));
        }
    }
}

static void circle_d(float cx, float cy, float r, uint32_t rgb, int a)
{
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    if (x0 < g_clipX0) x0 = g_clipX0;
    if (y0 < g_clipY0) y0 = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    float inner = r - 0.7f > 0 ? (r - 0.7f) * (r - 0.7f) : 0;
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 <= inner) { blendp(x, y, rgb, a); continue; }
            float c = r - sqrtf(d2) + 0.5f;
            if (c > 0) blendp(x, y, rgb, (int)(a * clampf(c, 0, 1)));
        }
    }
}

static void circle_l(float cx, float cy, float r, uint32_t rgb, int a)
{
    circle_d(cx * g_S, cy * g_S, r * g_S, rgb, a);
}

// A soft glow: alpha falls off with distance (quadratically) out to r.
static void glow_l(float cx, float cy, float r, uint32_t rgb, int a)
{
    float CX = cx * g_S, CY = cy * g_S, R = r * g_S;
    int x0 = (int)(CX - R), x1 = (int)(CX + R) + 1, y0 = (int)(CY - R), y1 = (int)(CY + R) + 1;
    if (x0 < g_clipX0) x0 = g_clipX0;
    if (y0 < g_clipY0) y0 = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    float inv = 1.0f / (R * R);
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - CY;
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - CX, t = 1.0f - (dx * dx + dy * dy) * inv;
            if (t > 0) blendp(x, y, rgb, (int)(a * t * t));
        }
    }
}

static void ring_l(float cx, float cy, float r, float w, uint32_t rgb, int a)
{
    float CX = cx * g_S, CY = cy * g_S, R = r * g_S, hw = w * g_S * 0.5f;
    int x0 = (int)(CX - R - hw - 1), x1 = (int)(CX + R + hw + 2);
    int y0 = (int)(CY - R - hw - 1), y1 = (int)(CY + R + hw + 2);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            float c = hw - fabsf(sqrtf(dx * dx + dy * dy) - R) + 0.5f;
            if (c > 0) blendp(x, y, rgb, (int)(a * clampf(c, 0, 1)));
        }
}

// An anti-aliased line with round caps (device units).
static void line_d(float x0, float y0, float x1, float y1, float w, uint32_t rgb, int a)
{
    float hw = w * 0.5f;
    int bx0 = (int)floorf(fminf(x0, x1) - hw - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int by0 = (int)floorf(fminf(y0, y1) - hw - 1), by1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    if (bx0 < g_clipX0) bx0 = g_clipX0;
    if (by0 < g_clipY0) by0 = g_clipY0;
    if (bx1 > g_clipX1) bx1 = g_clipX1;
    if (by1 > g_clipY1) by1 = g_clipY1;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0, 1) : 0;
            float ex = px - t * dx, ey = py - t * dy;
            float c = hw - sqrtf(ex * ex + ey * ey) + 0.5f;
            if (c > 0) blendp(x, y, rgb, (int)(a * clampf(c, 0, 1)));
        }
}

static void line_l(float x0, float y0, float x1, float y1, float w, uint32_t rgb, int a)
{
    line_d(x0 * g_S, y0 * g_S, x1 * g_S, y1 * g_S, w * g_S, rgb, a);
}

// A filled polygon, anti-aliased by 4x4 supersampling (small shapes only: the bolt).
static void poly_l(const float *pts, int n, uint32_t rgb, int a)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        minx = fminf(minx, pts[i * 2] * g_S); maxx = fmaxf(maxx, pts[i * 2] * g_S);
        miny = fminf(miny, pts[i * 2 + 1] * g_S); maxy = fmaxf(maxy, pts[i * 2 + 1] * g_S);
    }
    for (int y = (int)miny; y <= (int)maxy; y++)
        for (int x = (int)minx; x <= (int)maxx; x++) {
            int hits = 0;
            for (int s = 0; s < 16; s++) {
                float px = x + (s % 4 + 0.5f) / 4, py = y + (s / 4 + 0.5f) / 4;
                int in = 0;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    float xi = pts[i * 2] * g_S, yi = pts[i * 2 + 1] * g_S;
                    float xj = pts[j * 2] * g_S, yj = pts[j * 2 + 1] * g_S;
                    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
                }
                hits += in;
            }
            if (hits) blendp(x, y, rgb, a * hits / 16);
        }
}

// ------------------------------------------------------------------- text ---

static const FontFace *face(int f) { return &g_fonts[g_S - 1][f]; }

static const FontGlyph *glyph(const FontFace *F, unsigned char ch)
{
    if (ch < F->first || ch >= F->first + F->count) return NULL;
    return &F->g[ch - F->first];
}

// Width in logical pixels; sp is extra letter spacing (logical).
static float text_w(int f, const char *s, float sp)
{
    const FontFace *F = face(f);
    int w = 0, spd = (int)lroundf(sp * g_S), n = 0;
    for (; *s; s++, n++) {
        const FontGlyph *g = glyph(F, (unsigned char)*s);
        if (g) w += g->adv;
        if (s[1]) w += spd;
    }
    (void)n;
    return (float)w / (float)g_S;
}

// Draws with the top of the line at y; returns the pen position after it.
static float text_sp(int f, float x, float y, const char *s, uint32_t rgb, int a, float sp)
{
    const FontFace *F = face(f);
    int pen = D(x), top = D(y), spd = (int)lroundf(sp * g_S);
    for (; *s; s++) {
        const FontGlyph *g = glyph(F, (unsigned char)*s);
        if (!g) continue;
        const uint8_t *src = F->alpha + g->off;
        int gx = pen + g->x, gy = top + g->y;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int c = src[j * g->w + i];
                if (c) blendp(gx + i, gy + j, rgb, c * a / 255);
            }
        pen += g->adv + spd;
    }
    return (float)pen / (float)g_S;
}

static float text(int f, float x, float y, const char *s, uint32_t rgb)
{
    return text_sp(f, x, y, s, rgb, 255, 0);
}

static void text_c(int f, float cx, float y, const char *s, uint32_t rgb)
{
    text(f, cx - text_w(f, s, 0) / 2, y, s, rgb);
}

static void text_r(int f, float rx, float y, const char *s, uint32_t rgb)
{
    text(f, rx - text_w(f, s, 0), y, s, rgb);
}

// Letter-spaced capitals, for the small labels.
static float label(float x, float y, const char *s, uint32_t rgb)
{
    return text_sp(F_SMALL, x, y, s, rgb, 255, 0.8f);
}

static void label_c(float cx, float y, const char *s, uint32_t rgb)
{
    label(cx - text_w(F_SMALL, s, 0.8f) / 2, y, s, rgb);
}

// Text with a soft drop shadow, for writing over the sky.
static float text_shadow(int f, float x, float y, const char *s, uint32_t rgb)
{
    float o = g_S == 1 ? 1.0f : 0.75f;
    text_sp(f, x + o, y + o, s, 0x000000, 110, 0);
    text_sp(f, x + o * 2, y + o * 2, s, 0x000000, 40, 0);
    return text(f, x, y, s, rgb);
}

// Copies s into out, shortened with "..." to fit maxw.
static void fit(int f, const char *s, float maxw, char *out, size_t size)
{
    snprintf(out, size, "%s", s);
    if (text_w(f, out, 0) <= maxw) return;
    size_t n = strlen(out);
    while (n > 0) {
        out[--n] = 0;
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%.100s...", out);
        if (text_w(f, tmp, 0) <= maxw) { memcpy(out, tmp, strlen(tmp) + 1 < size ? strlen(tmp) + 1 : size); out[size - 1] = 0; return; }
    }
}


// ------------------------------------------------------------ presenting ---
//
// TERMinator fits the picture into its window at 4:3 and scales it with
// nearest-neighbour. Shrinking that way drops whole rows and columns, so thin
// strokes vanish here and there and text looks nicked. So the module works out
// the exact size the picture will be shown at (trace_on_resize gives the
// window's device pixels), renders at 2x, and hands over a frame of exactly that
// size, shrunk with an area-averaging filter. Bigger than the 2x frame (1440x1080
// on a 1080p screen, say), it is grown to that size with a bilinear filter:
// left to TERMinator, a stretch of 1.125 doubles every 8th row and column, and
// text comes out jagged.

static int g_dispW = LW, g_dispH = LH;     // the picture on screen, device pixels
static int g_presW = LW, g_presH = LH;     // the frame last presented (mouse positions are in this)

// Call from trace_on_resize: the largest 4:3 rectangle in the window.
static void set_display(int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return;
    if ((int64_t)w * 3 > (int64_t)h * 4) { g_dispH = h; g_dispW = h * 4 / 3; }
    else { g_dispW = w; g_dispH = w * 3 / 4; }
    g_wantS = g_dispW > 700 ? 2 : 1;
}

// One axis of the area filter: for each output pixel, the source pixels it
// covers and how much of each (weights in 1/256, summing to 256).
typedef struct { int first, count; uint16_t w[4]; } Span;

static Span *spans(int src, int dst)
{
    Span *s = malloc(sizeof(Span) * (size_t)dst);
    if (!s) return NULL;
    if (dst > src) {    // growing: bilinear, the two source pixels either side of the centre
        for (int i = 0; i < dst; i++) {
            double c = (i + 0.5) * src / dst - 0.5;
            if (c < 0) c = 0;
            int first = (int)c;
            if (first >= src - 1) { s[i].first = src - 1; s[i].count = 1; s[i].w[0] = 256; continue; }
            int f = (int)lround((c - first) * 256);
            s[i].first = first;
            s[i].count = 2;
            s[i].w[0] = (uint16_t)(256 - f);
            s[i].w[1] = (uint16_t)f;
        }
        return s;
    }
    double scale = (double)src / dst;
    for (int i = 0; i < dst; i++) {
        double a = i * scale, b = (i + 1) * scale;
        int first = (int)a, last = (int)ceil(b) - 1;
        if (last >= src) last = src - 1;
        if (last - first + 1 > 4) last = first + 3;
        s[i].first = first;
        s[i].count = last - first + 1;
        int total = 0;
        for (int k = 0; k < s[i].count; k++) {
            double lo = first + k > a ? first + k : a, hi = first + k + 1 < b ? first + k + 1 : b;
            int wgt = (int)lround((hi - lo) / scale * 256);
            if (wgt < 0) wgt = 0;
            s[i].w[k] = (uint16_t)wgt;
            total += wgt;
        }
        s[i].w[0] = (uint16_t)(s[i].w[0] + 256 - total);   // rounding goes to the first
    }
    return s;
}

static void present_fitted(int32_t flags)
{
    static int32_t capW = 0, capH = 0;
    if (capW <= 0 || capH <= 0) {
        trace_frame_capacity(&capW, &capH);
        if (capW <= 0 || capH <= 0) { capW = g_W; capH = g_H; }
    }
    int dw = g_dispW, dh = g_dispH;
    if (dw > capW) { dw = capW; dh = dw * 3 / 4; }
    if (dh > capH) { dh = capH; dw = dh * 4 / 3; }
    if (dw == g_W || dw < 64) {
        g_presW = g_W;
        g_presH = g_H;
        trace_present(g_frame, g_W, g_H, flags);
        return;
    }
    static uint32_t *tmp = NULL, *out = NULL;
    static Span *sx = NULL, *sy = NULL;
    static int forW = 0, forH = 0, fromW = 0, fromH = 0;
    if (dw != forW || dh != forH || g_W != fromW || g_H != fromH) {
        free(tmp); free(out); free(sx); free(sy);
        tmp = malloc((size_t)dw * g_H * 4);
        out = malloc((size_t)dw * dh * 4);
        sx = spans(g_W, dw);
        sy = spans(g_H, dh);
        forW = dw; forH = dh; fromW = g_W; fromH = g_H;
        if (!tmp || !out || !sx || !sy) {
            free(tmp); free(out); free(sx); free(sy);
            tmp = out = NULL; sx = sy = NULL; forW = forH = 0;
            trace_present(g_frame, g_W, g_H, flags);
            return;
        }
    }
    // Across, then down; red and blue ride together in one word, green alone.
    for (int y = 0; y < g_H; y++) {
        const uint32_t *row = g_frame + y * g_W;
        uint32_t *o = tmp + y * dw;
        for (int x = 0; x < dw; x++) {
            const Span *s = &sx[x];
            uint32_t rb = 0, g = 0;
            for (int k = 0; k < s->count; k++) {
                uint32_t p = row[s->first + k], w = s->w[k];
                rb += (p & 0x00FF00FFu) * w;
                g  += (p & 0x0000FF00u) * w;
            }
            o[x] = ((rb >> 8) & 0x00FF00FFu) | ((g >> 8) & 0x0000FF00u) | 0xFF000000u;
        }
    }
    for (int y = 0; y < dh; y++) {
        const Span *s = &sy[y];
        uint32_t *o = out + y * dw;
        for (int x = 0; x < dw; x++) {
            uint32_t rb = 0, g = 0;
            for (int k = 0; k < s->count; k++) {
                uint32_t p = tmp[(s->first + k) * dw + x], w = s->w[k];
                rb += (p & 0x00FF00FFu) * w;
                g  += (p & 0x0000FF00u) * w;
            }
            o[x] = ((rb >> 8) & 0x00FF00FFu) | ((g >> 8) & 0x0000FF00u) | 0xFF000000u;
        }
    }
    g_presW = dw;
    g_presH = dh;
    trace_present(out, dw, dh, flags);
}

// A mouse position (in the presented frame's pixels) in layout coordinates.
static float mouse_lx(int32_t x) { return (float)x * LW / (g_presW ? g_presW : LW); }
static float mouse_ly(int32_t y) { return (float)y * LH / (g_presH ? g_presH : LH); }
