// lpdata.h - what the collector keeps on disk for the doors, and how both sides
// read and write it. Shared by lpcollect and lpdoor (not the module).
//
//   data/snapshot.bin  every node heard in the last node_days, the chat, the mesh
//                      links heard in the last day and the traffic history.
//                      Rewritten every minute and when the collector stops, so a
//                      caller who opens the door with the collector off still
//                      sees what was heard last time.
//   data/live.bin      the same, but only the nodes and links heard in the last
//                      two minutes. Rewritten every 5 seconds while the collector
//                      runs: this is how the doors see traffic live.
//   data/collector.lock  held (flock) by the running collector.
//   data/doors/<pid>   one per open door, touched every 30 s: "someone is here".
//
// Both files are written to a temporary name and renamed, so a reader never sees
// half of one. Positions are already blurred to a 0.02 degree grid (about 2 km)
// before they are stored: the collector never keeps anything finer.
//
// Versions: 1 (nodes 76 bytes, no short name), 2 (84, short name), 3 (92, sensor
// readings; links and history after the chat). Readers take all three.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LP_NAME_LEN   40
#define LP_CHAN_LEN   16
#define LP_TEXT_LEN   232
#define LP_BLUR_1E5   2000          // 0.02 degrees in 1e-5 degree units
#ifndef LP_HIST_MIN
#define LP_HIST_MIN   180           // minutes of traffic history kept (3 hours)
#endif
#define LP_NO_TEMP    (-32768)

enum { LP_ST_STOPPED = 0, LP_ST_CONNECTING = 1, LP_ST_LIVE = 2, LP_ST_OFFLINE = 3 };

enum { LP_NODE_POS = 1, LP_NODE_NAME = 2, LP_NODE_TELEM = 4, LP_NODE_ENV = 8 };

enum { LP_LINK_NEIGHBOR = 1, LP_LINK_TRACE = 2, LP_LINK_HEARD = 3 };   // HEARD: a gateway heard it directly

typedef struct {
    uint32_t id;
    int32_t  lat1e5, lon1e5;        // blurred; valid when flags & LP_NODE_POS
    uint32_t firstHeard, lastHeard, lastPos;   // unix seconds
    uint32_t packets;               // unique packets heard from it
    uint16_t voltage_cV;            // centivolts, 0 = unknown
    uint8_t  hw;                    // Meshtastic HardwareModel (see hwmodels.h), 0 = unknown
    uint8_t  battery;               // 0-100 %, 101 = on external power, 255 = unknown
    uint8_t  state;                 // 1..52 = states.bin index + 1, 0 = none known
    uint8_t  flags;                 // LP_NODE_*
    uint8_t  role;
    uint8_t  topicState;            // the state its gateway's topic names, when it has no position
    char     name[LP_NAME_LEN];     // its long name (Latin-1)
    char     shortName[8];          // its short name (Latin-1; emoji ones come out empty)
    int16_t  temp_dC;               // a sensor's temperature, tenths of a degree C (LP_NO_TEMP = none)
    uint8_t  humidity;              // %, 255 = none
    uint8_t  pad;
    uint32_t envAt;                 // when the sensor reading came
} LpNode;                           // 92 bytes (84 in version 2, 76 in version 1)

typedef struct {
    uint32_t time, from;
    char     channel[LP_CHAN_LEN];
    char     text[LP_TEXT_LEN];     // Latin-1
    uint8_t  state;                 // the sender's state when it spoke, 0 = unknown
    uint8_t  pad[3];
} LpChat;                           // 260 bytes

// Two nodes that heard each other: from a neighbour report, or a hop in a traceroute.
typedef struct {
    uint32_t a, b;                  // a < b
    uint32_t lastHeard;
    int16_t  snr_q;                 // signal-to-noise in quarter dB (as Meshtastic sends it)
    uint8_t  source;                // LP_LINK_*
    uint8_t  pad;
} LpLink;                           // 16 bytes

typedef struct {
    char     magic[4];              // "LPS1"
    uint32_t version;               // 3
    uint32_t nodes, chats;
    uint32_t savedAt, startedAt, lastPacketAt;
    uint32_t pktPerMin, uniqueTotal;
    uint8_t  status;                // LP_ST_*
    uint8_t  pad[3];
    uint32_t links;                 // version 3
    uint32_t histCount;             // version 3: minutes of history that follow the links
    uint32_t histEndMin;            // version 3: unix minute of the last history entry
    uint32_t reserved;
} LpHead;                           // 56 bytes

typedef struct {
    LpHead    h;
    LpNode   *nodes;
    LpChat   *chats;
    LpLink   *links;
    uint16_t *hist;                 // unique packets per minute, oldest first
} LpData;

static void lp_data_free(LpData *d)
{
    free(d->nodes);
    free(d->chats);
    free(d->links);
    free(d->hist);
    memset(d, 0, sizeof *d);
}

// Writes it all to path, atomically. The counts come from d->h. Returns 1 on success.
static int lp_write_data(const char *path, LpData *d)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    LpHead *h = &d->h;
    memcpy(h->magic, "LPS1", 4);
    h->version = 3;
    int ok = fwrite(h, sizeof *h, 1, f) == 1 &&
             (!h->nodes || fwrite(d->nodes, sizeof *d->nodes, h->nodes, f) == h->nodes) &&
             (!h->chats || fwrite(d->chats, sizeof *d->chats, h->chats, f) == h->chats) &&
             (!h->links || fwrite(d->links, sizeof *d->links, h->links, f) == h->links) &&
             (!h->histCount || fwrite(d->hist, sizeof *d->hist, h->histCount, f) == h->histCount);
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return 0; }
    return rename(tmp, path) == 0;
}

// Reads a file of any version into d (everything malloc'd; free with
// lp_data_free). Returns 1 on success; on failure d is left empty.
static int lp_read_data(const char *path, LpData *d)
{
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    LpHead *h = &d->h;
    int ok = fread(h, sizeof *h, 1, f) == 1 && !memcmp(h->magic, "LPS1", 4) && h->version >= 1 && h->version <= 3 &&
             h->nodes <= 200000 && h->chats <= 10000;
    if (h->version < 3) { h->links = h->histCount = h->histEndMin = 0; }
    ok = ok && h->links <= 400000 && h->histCount <= 100000;
    if (ok && h->nodes) {
        size_t size = h->version == 3 ? sizeof(LpNode) : h->version == 2 ? 84 : 76;
        d->nodes = calloc(h->nodes, sizeof *d->nodes);
        for (uint32_t i = 0; ok && d->nodes && i < h->nodes; i++) {
            ok = fread(&d->nodes[i], size, 1, f) == 1;
            if (h->version < 3) { d->nodes[i].temp_dC = LP_NO_TEMP; d->nodes[i].humidity = 255; }
        }
        ok = ok && d->nodes;
    }
    if (ok && h->chats) {
        d->chats = malloc(sizeof *d->chats * h->chats);
        ok = d->chats && fread(d->chats, sizeof *d->chats, h->chats, f) == h->chats;
    }
    if (ok && h->links) {
        d->links = malloc(sizeof *d->links * h->links);
        ok = d->links && fread(d->links, sizeof *d->links, h->links, f) == h->links;
    }
    if (ok && h->histCount) {
        d->hist = malloc(sizeof *d->hist * h->histCount);
        ok = d->hist && fread(d->hist, sizeof *d->hist, h->histCount, f) == h->histCount;
    }
    fclose(f);
    if (!ok) { lp_data_free(d); return 0; }
    for (uint32_t i = 0; i < h->nodes; i++) {
        d->nodes[i].name[LP_NAME_LEN - 1] = 0;
        d->nodes[i].shortName[7] = 0;
    }
    for (uint32_t i = 0; i < h->chats; i++) {
        d->chats[i].text[LP_TEXT_LEN - 1] = 0;
        d->chats[i].channel[LP_CHAN_LEN - 1] = 0;
    }
    return 1;
}
