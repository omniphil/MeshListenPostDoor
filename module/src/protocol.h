// Wire protocol between the Listening Post door (on the BBS) and its TRACE module
// (on the caller's machine). Shared verbatim by both sides.
//
// Division of labour:
//   - The DOOR owns the data: it reads what the collector keeps (snapshot.bin,
//     then live.bin every few seconds) and sends the nodes, the chat and the
//     feed's status. The state outlines go once as an asset (states.bin, cached).
//   - The MODULE owns the picture: the map, the node details, the chat, the
//     region list, all of it driven by keyboard and mouse.
//
// Everything is little-endian. Every message starts with an LpMsgHeader.
#pragma once
#include <stdint.h>

#define LP_PROTO_VERSION 3
#define LP_MAX_PAYLOAD   4096       // one packet, under the 8 KB text-command limit once base64'd
#define LP_WIRE_NAME     24
#ifndef LP_HIST_MIN
#define LP_HIST_MIN      180        // minutes of traffic history (3 hours)
#endif

// Door -> module
enum LpMsgIn {
    LP_IN_HELLO  = 1,   // LpHello
    LP_IN_NODES  = 2,   // flags bit0 = the first batch of a full list (clear first); count * LpNodeWire.
                        //   Later batches update nodes by id or add them.
    LP_IN_DONE   = 3,   // no payload: the full list is complete
    LP_IN_CHAT   = 4,   // flags bit0 = replace the whole list; count * (LpChatWire + name + text)
    LP_IN_STATS  = 5,   // LpStats
    LP_IN_LINKS  = 6,   // flags bit0 = replace the whole list; count * LpLinkWire. Later batches update or add.
    LP_IN_HISTORY = 7,  // LpHistHead + count * uint16: unique packets per minute, oldest first
};

// Module -> door
enum LpMsgOut {
    LP_OUT_READY  = 1,  // no payload: send everything
    LP_OUT_REGION = 2,  // uint8 region (0 = All US, 1.. = states.bin order): remember it
};

// The feed, as the door sees it.
enum { LP_FEED_STOPPED = 0, LP_FEED_CONNECTING = 1, LP_FEED_LIVE = 2, LP_FEED_OFFLINE = 3 };

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t count;
    uint32_t bytes;
} LpMsgHeader;

typedef struct {
    uint32_t doorNow;           // the door's clock, so the module keeps time
    uint8_t  region;            // the caller's last region
    uint8_t  nodeDays;          // nodes older than this are gone
    uint8_t  haveHome;          // 1 = the caller's town is known (from the Weather door)
    uint8_t  homeState;         // its state, 0 = unknown
    int32_t  homeLat1e5, homeLon1e5;
    char     homeName[40];
} LpHello;                      // 56 bytes

typedef struct {
    uint32_t id;                // the node number: shown as !1a2b3c4d, and how updates are matched
    int32_t  lat1e5, lon1e5;    // blurred to ~2 km by the collector
    uint32_t firstHeard, lastHeard;
    uint16_t voltage_cV;
    uint8_t  hw, battery;       // battery 0-100, 101 = external power, 255 = unknown
    uint8_t  state, flags;      // state: 1.. = states.bin index + 1; flags: 1 position, 2 name, 4 telemetry
    uint8_t  role, humidity;    // humidity: a sensor's %, 255 = none
    char     name[LP_WIRE_NAME];  // long name
    char     shortName[8];
    uint32_t packets;           // unique packets heard from it
    int16_t  temp_dC;           // a sensor's temperature, tenths of a degree C (-32768 = none); flags 8 = has a sensor
    uint16_t links;             // how many radios it's been heard talking to directly
} LpNodeWire;                   // 68 bytes

// Two radios that heard each other directly (see lpdata.h LpLink).
typedef struct {
    uint32_t a, b;
    uint32_t lastHeard;
    int16_t  snr_q;             // quarter dB
    uint8_t  source;            // 1 neighbour report, 2 traceroute, 3 a gateway heard it
    uint8_t  pad;
} LpLinkWire;                   // 16 bytes

typedef struct {
    uint32_t endMinute;         // unix minute of the last value
    uint16_t count;
    uint16_t pad;
} LpHistHead;

typedef struct {
    uint32_t time;
    uint8_t  state;             // the sender's state, 0 = unknown
    uint8_t  nameLen, textLen, chanLen;
    // then name, channel, text (Latin-1)
} LpChatWire;

typedef struct {
    uint32_t doorNow;
    uint32_t startedAt;         // when the collector started (0 = not running)
    uint32_t lastPacketAt;      // the newest packet it heard
    uint32_t snapshotAt;        // when the data on screen was saved
    uint32_t pktPerMin;
    uint32_t nodesTotal;
    uint8_t  feed;              // LP_FEED_*
    uint8_t  firstEver;         // 1 = there's no snapshot at all yet
    uint8_t  reserved[2];
} LpStats;
