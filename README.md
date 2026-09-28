# Meshtastic Listening Post

A BBS door that listens to the public Meshtastic network: a live map of the US with every node the public feed has
heard, coloured by how recently, the radios they are, and the public chat. No radio needed.

- **TRACE** callers (TERMinator 1.1.3+) get a smooth map they can zoom and pan, with the links between radios that
  heard each other drawn by signal strength, node details (long and short name, ID, radio, battery, sensor reading,
  distance from home), the chat with a test-chatter filter, a find box, a sensor layer and a stats page with a
  3-hour traffic graph. Every action has a key, and anything a key does can be clicked, including the key hints
  along the bottom of the map and each box (? lists the keys).
- **ANSI** callers get the same map in 24-bit half blocks at 79x24, a node list with find (/), a sensor view (E),
  distances or link counts, the chat (F filter, Tab channel) and a stats page (S). Keyboard only.
- **Home:** a caller's town comes from the Weather door's saves (H goes there); without one, H says how to set it.
- **News screen:** `news/newsstats.py` shows "Mesh: N radios" on row 24 when the listening post ran in the last day.

## How it works

| Piece | Where | What |
|---|---|---|
| `lpcollect` | the BBS, in the background | Listens to `mqtt.meshtastic.org` (`msh/US/#`) and keeps `data/snapshot.bin` + `data/live.bin`. Plain C: MQTT, protobuf and AES-128-CTR are built in. |
| `lpdoor` | the BBS | Start menu, the ANSI view, and the pipe to the module. Starts the collector. |
| `lp.wasm` | the caller's TERMinator | The TRACE picture. |
| `states.bin` | both; sent to TERMinator once, cached | US state outlines (Census cartographic boundaries, public domain), from `tools/make_states.py`. |

**The collector runs only while the door is in use.** The first caller to open the door starts it. Each open door
keeps a file in `data/doors/` fresh; when none is left, the collector keeps going for `linger_minutes` (15) and then
saves and exits. A second collector started while one runs sees the lock (`data/collector.lock`) and leaves. The
snapshot stays on disk, so the next caller starts with the last map, marked with its age, while the feed warms up.

**What it keeps.** Per node: its long and short names, ID, hardware, battery, a sensor's temperature and humidity,
when it was first and last heard, and its position rounded to a 0.02 degree grid (about 2 km) -- some map reports
carry exact positions, and the collector never keeps them. Broadcast text on channels readable with the default key.
Links: two radios that heard each other directly, with the signal (SNR), for a day -- mostly from the gateways
themselves (a packet uplinked with no hops used was heard straight off the air), plus neighbour reports and
traceroute answers when readable (rare: answers are direct messages, usually encrypted). Three hours of unique
packets per minute. Routing and duplicate copies from other gateways are dropped. Private channels and direct
messages can't be read and only count as "heard". Nodes not heard for `node_days` (7) drop out.

**What the feed looks like** (measured 2026-09-28, whole US, 10 minutes): about 3,300 nodes, 500 with positions, but
only ~21 nodes sending text, mostly "test". So it is a map first, with a chat ticker. About 90% of the raw MQTT
messages are empty stubs from one gateway (0-byte payloads); the collector drops them.

States: a node is placed by its position; one that never sends a position gets the state its gateway's topic names
(`msh/US/GA/...`), if any.

## Build and install

```
make -C collector                        # lpcollect (gcc; runs an AES self-test)
make -C door                             # lpdoor (gcc)
make -C module                           # lp.wasm (wasi-sdk 34+ in ~/tools)
make -C door bundle                      # snapshot the shared headers into door/native for the BBS
```

On the BBS, `/home/user/mystic/doors/listenpost/` holds `lpdoor`, `lpcollect`, `lp.wasm`, `states.bin` and `lp.ini`
(from `lp.ini.example`), with write access for `data/` and `saves/`. The collector needs outbound TCP 1883.

Mystic menu command: `D3` with data `./doors/listenpost/lpdoor /home/user/mystic/temp%3`.

## Keys

| | TRACE | ANSI |
|---|---|---|
| Pick a node | click it, Tab / Shift+Tab (newest first) | N (node list), Up/Down |
| Find a node (name, short name, ID) | / or F | / on the node list |
| Home (from the Weather door) | H, the house button | H |
| Links between radios | L, the Links chip | (a count in the node list) |
| Sensor readings | E, the Sensors chip | E (node list shows sensors) |
| Stats | S, click the stats panel | S |
| Chat filter | F (chatter), Tab (channel), the chips | F, Tab on the chat page |
| All the keys | ? | the key bar |
| Zoom | wheel, + / -, the buttons | (pick a region) |
| Pan | drag, arrows | |
| Reset the view | Home, 0, the target button | |
| Region | R, the region button, click a state on the All US map | R |
| Chat | C, click the chat panel or the C hint | C |
| Back / quit | Esc steps back (node, zoom, region), Q or the X quits | Esc, Q |

## Testing

```
python3 door/test_door.py            # fake TERMinator: outlines + module, nodes, chat, live updates, region, collector start
python3 door/test_door.py --bin      # binary frames
collector/lpcollect --selftest       # AES (FIPS-197)
LPDIR=<folder> python3 tools/ansi_shot.py out.png 2 @WAIT n @WAIT     # the ANSI view, rendered to a PNG
make -C module lpdemo.wasm           # the module with made-up nodes; render with the TRACE probe:
TR=../TERMinator-Windows/trace/bin-linux-x86_64
$TR/gamesandbox_probe $TR/gamesandbox module/lpdemo.wasm -seconds 2 -size 1280x960 -shot shot.bmp
```

`tools/make_states.py` and `tools/make_hw.py` regenerate `data/states.bin` and `common/hwmodels.h` (see their
headers for the Python packages they need).
