// demo.h - made-up nodes and chat, and the state outlines compiled in, so the
// module can be looked at without a BBS (make lpdemo.wasm, then the probe).
// Nothing here is real: names, positions and messages are invented.
#pragma once
#include "states_blob.h"

static uint32_t demo_rand(void)
{
    static uint32_t s = 0x2545F491u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static float demo_f(void) { return (demo_rand() & 0xFFFFFF) / (float)0x1000000; }

static void demo_fill(void)
{
    g_haveGeo = geo_load(&g_geo, k_states_blob, sizeof k_states_blob);
    g_doorNow = 1790600000u;
    g_recvMs = 0;
    g_nodeDays = 7;
    static const char *const NAMES[] = { "Ridge Relay", "Peachtree Base", "Kite Hill", "Rover 7", "Solar Router",
        "Blue Heron", "Mountain Echo", "Lakeside", "Attic Node", "Porch Radio", "Tower Two", "Creekside" };
    static const uint8_t HW[] = { 43, 43, 9, 4, 31, 50, 110, 37, 43, 9 };
    // Clusters where meshes are busy, plus a scatter everywhere.
    static const float HOT[][2] = { { 33.75f, -84.39f }, { 37.77f, -122.42f }, { 28.54f, -81.38f }, { 42.33f, -83.05f },
        { 39.95f, -75.16f }, { 30.27f, -97.74f }, { 47.61f, -122.33f }, { 41.88f, -87.63f }, { 40.76f, -111.89f } };
    for (int i = 0; i < 1800; i++) {
        LpNodeWire w;
        memset(&w, 0, sizeof w);
        w.id = 0x10000 + (uint32_t)i;
        float lat, lon;
        int st = -1;
        for (int tries = 0; tries < 20 && st < 0; tries++) {
            if (i % 3) {
                const float *h = HOT[demo_rand() % 9];
                lat = h[0] + (demo_f() - 0.5f) * 2.2f;
                lon = h[1] + (demo_f() - 0.5f) * 2.8f;
            } else {
                lat = 25 + demo_f() * 24;
                lon = -124 + demo_f() * 57;
            }
            st = geo_state_at(&g_geo, lat, lon);
        }
        if (st < 0) continue;
        w.lat1e5 = (int32_t)lroundf(lat * 1e5f);
        w.lon1e5 = (int32_t)lroundf(lon * 1e5f);
        w.state = (uint8_t)(st + 1);
        w.flags = 1 | ((i % 4) ? 2 : 0) | ((i % 5) ? 4 : 0);
        float r = demo_f();
        uint32_t age = r < 0.15f ? (uint32_t)(demo_f() * 900) : r < 0.35f ? 900 + (uint32_t)(demo_f() * 2700)
                     : r < 0.7f ? 3600 + (uint32_t)(demo_f() * 80000) : 86400 + (uint32_t)(demo_f() * 500000);
        w.lastHeard = g_doorNow - age;
        w.firstHeard = w.lastHeard - (uint32_t)(demo_f() * 400000);
        w.hw = HW[demo_rand() % 10];
        w.battery = (uint8_t)(demo_rand() % 5 == 0 ? 101 : 20 + demo_rand() % 80);
        w.voltage_cV = (uint16_t)(360 + demo_rand() % 60);
        if (w.flags & 2) snprintf(w.name, sizeof w.name, "%s %d", NAMES[demo_rand() % 12], i % 97);
        if (i % 3) snprintf(w.shortName, sizeof w.shortName, "%c%c%02d", 'A' + (int)(demo_rand() % 26), 'A' + (int)(demo_rand() % 26), i % 100);
        w.packets = 1 + demo_rand() % (i % 7 ? 60 : 900);
        w.temp_dC = -32768;
        w.humidity = 255;
        if (i % 17 == 0) { w.flags |= 8; w.temp_dC = (int16_t)(100 + demo_rand() % 220); w.humidity = (uint8_t)(20 + demo_rand() % 70); }
        add_node(&w, 0);
    }
    // Links between nearby nodes, as gateways hearing their neighbours.
    for (int k = 0; k < 60000; k++) {
        int i = (int)(demo_rand() % (uint32_t)g_n), j = i + 1 + (int)(demo_rand() % 60);
        if (j >= g_n) continue;
        const LpNodeWire *a = &g_nodes[i].w, *b = &g_nodes[j].w;
        float dl = (a->lat1e5 - b->lat1e5) / 1e5f, dn = (a->lon1e5 - b->lon1e5) / 1e5f;
        if (dl * dl + dn * dn > 0.35f) continue;
        LpLinkWire l;
        memset(&l, 0, sizeof l);
        l.a = a->id < b->id ? a->id : b->id;
        l.b = a->id < b->id ? b->id : a->id;
        l.lastHeard = g_doorNow - demo_rand() % 60000;
        l.snr_q = (int16_t)((int)(demo_rand() % 120) - 70);
        l.source = 3;
        add_link(&l);
    }
    for (int m = 0; m < LP_HIST_MIN; m++) g_hist[m] = (uint16_t)(420 + 90 * sinf(m / 17.0f) + demo_rand() % 60);
    g_histCount = LP_HIST_MIN;
    g_histEnd = g_doorNow / 60;
    g_haveHome = 1;
    g_homeLat = 33.749f;
    g_homeLon = -84.388f;
    g_homeState = geo_state_at(&g_geo, g_homeLat, g_homeLon) + 1;
    snprintf(g_homeName, sizeof g_homeName, "Atlanta, Georgia");
    static const char *const LINES[][3] = {
        { "Ridge Relay 12", "LongFast", "Good morning mesh! Signal check from the ridge." },
        { "Rover 7 40", "LongFast", "Heard you 3 hops out, nice." },
        { "Kite Hill 5", "MediumFast", "Anyone near the lake this weekend?" },
        { "Tower Two 61", "LongFast", "New solar node is up on the tower." },
        { "Blue Heron 18", "LongFast", "test" },
        { "Porch Radio 77", "LongFast", "Weather's clearing, range is great today." },
        { "Blue Heron 18", "LongFast", "Test" },
        { "Rover 7 40", "MediumFast", "Hi" },
        { "Kite Hill 5", "MediumFast", "Anyone near the lake this weekend?" },
    };
    for (int i = 0; i < 9; i++) {
        Chat *c;
        if (!g_capChat) { g_capChat = 64; g_chat = realloc(g_chat, sizeof *g_chat * 64); }
        c = &g_chat[g_chatBuild++];
        memset(c, 0, sizeof *c);
        c->time = g_doorNow - (uint32_t)(9 - i) * 420;
        c->state = (uint8_t)(1 + (i * 7) % 50);
        snprintf(c->name, sizeof c->name, "%s", LINES[i][0]);
        snprintf(c->chan, sizeof c->chan, "%s", LINES[i][1]);
        snprintf(c->text, sizeof c->text, "%s", LINES[i][2]);
    }
    g_nChat = g_chatBuild;
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.doorNow = g_doorNow;
    g_stats.feed = LP_FEED_LIVE;
    g_stats.pktPerMin = 212;
    g_stats.startedAt = g_doorNow - 900;
    g_stats.snapshotAt = g_doorNow;
    g_haveAll = 1;
    g_started = 1;
    project_all();
    for (int i = 0; i < g_n; i++)
        if (g_nodes[i].onMap && g_nodes[i].w.name[0] && now_door() - g_nodes[i].w.lastHeard < 600) { g_sel = i; break; }
}
