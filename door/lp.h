/*
 * lp.h - the Listening Post door's data: settings, each caller's choices, the
 * collector (started on demand), and the nodes and chat it has heard.
 */
#ifndef LP_H
#define LP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "geo.h"
#include "lpdata.h"
#include "protocol.h"

typedef struct {
    int  nodeDays;                   /* nodes not heard for this long are gone (the collector's setting too) */
    int  defaultRegion;              /* 0 = All US */
    char weatherSaves[1024];         /* the Weather door's saves folder: where a caller's town comes from */
} LpConfig;

typedef struct {
    int    region;                   /* 0 = All US, 1.. = states.bin order */
    int    display;                  /* start page: 1 TRACE, 2 ANSI, 0 none */
    bool   haveHome;                 /* the caller told the Weather door their town */
    double homeLat, homeLon;
    char   homeName[64];
    int    homeState;                /* 1.. = states.bin index + 1, 0 = unknown */
} LpUserPrefs;

extern LpConfig lp_config;
extern GeoMap   lp_geo;
extern bool     lp_haveGeo;

const char *lp_beside_exe(const char *name, char *out, size_t size);
void lp_load_config(void);
void lp_load_prefs(LpUserPrefs *p);
void lp_save_prefs(const LpUserPrefs *p);
bool lp_load_states(void);

/* "I'm here": makes this door's file in data/doors and starts the collector if
 * it isn't running. lp_touch() keeps the file fresh (call it every few seconds;
 * it only writes every 30); lp_leave() removes it. */
void lp_arrive(void);
void lp_touch(void);
void lp_leave(void);

/* The nodes, links, chat and traffic history as the door knows them: the
 * snapshot at first, then what live.bin adds. lp_refresh() re-reads live.bin
 * when it has changed and says what changed: node indexes in lp_changed[],
 * link indexes in lp_changedLinks[]. */
extern LpNode  *lp_nodes;
extern uint32_t lp_nNodes;
extern LpChat  *lp_chat;
extern uint32_t lp_nChat;
extern LpLink  *lp_links;
extern uint32_t lp_nLinks;
extern uint16_t lp_hist[LP_HIST_MIN];
extern uint32_t lp_histCount, lp_histEnd;
extern LpHead   lp_head;             /* the newest header seen */
extern bool     lp_firstEver;        /* no snapshot existed when the door opened */
extern uint32_t *lp_changed, *lp_changedLinks;

typedef struct { uint32_t nodes, links; bool chat, hist; } LpChanges;

void lp_load_snapshot(void);
const LpNode *lp_find(uint32_t id);
LpChanges lp_refresh(void);

/* Great-circle distance in km. */
double lp_km(double lat1, double lon1, double lat2, double lon2);

/* The feed right now: LP_FEED_*, worked out from how fresh live.bin is. */
int lp_feed_state(void);

/* What to call a node: its long name, else its short name, else its number as Meshtastic writes it (!1a2b3c4d). */
static inline void lp_label(const LpNode *n, uint32_t id, char *out, size_t size)
{
    if (n && n->name[0]) snprintf(out, size, "%s", n->name);
    else if (n && n->shortName[0]) snprintf(out, size, "%s", n->shortName);
    else snprintf(out, size, "!%08x", id);
}

/* A node's state for display: its position's state, else its gateway topic's. */
static inline int lp_node_state(const LpNode *n) { return n->state ? n->state : n->topicState; }

#endif
