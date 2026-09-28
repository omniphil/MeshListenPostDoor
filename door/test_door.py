#!/usr/bin/env python3
"""A fake TERMinator on a pty, so the Listening Post door can be tested without a BBS.

It builds a door folder in a temp directory with a made-up snapshot and a stub
collector (so no network is needed), answers Query, takes the state outlines and
the module and checks their hashes, plays the part of the module, and checks
what comes back: the nodes, the chat, the feed status, live updates as live.bin
changes, the region remembered, the collector started, and the door's
"I'm here" file made and removed.

    python3 test_door.py             base64 transport
    python3 test_door.py --bin       binary frames
"""
import base64
import hashlib
import os
import pty
import re
import select
import shutil
import struct
import sys
import tempfile
import time

MODULE = "listenpost"
APC = "\033_TERMinator:TRACE;"
ST = "\033\\"

IN_HELLO, IN_NODES, IN_DONE, IN_CHAT, IN_STATS, IN_LINKS, IN_HISTORY = range(1, 8)
OUT_READY, OUT_REGION = 1, 2

ESCAPED = {0x00, 0x0A, 0x0D, 0x11, 0x13, 0x18, 0x1B, ord('='), 0xFF}

fails = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails.append(what)


def unescape(data):
    out, i = bytearray(), 0
    while i < len(data):
        if data[i] == ord('=') and i + 1 < len(data):
            out.append((data[i + 1] - 64) & 0xFF)
            i += 2
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def escape(data):
    """Module -> door, as TERMinator does it: EVERY control byte is escaped,
    because the door reads through a pty that would act on them (0x03 is
    Ctrl-C). Escaping only the door's nine lets a message type of 3 kill it."""
    out = bytearray()
    for b in data:
        if b in ESCAPED or b < 0x20:
            out += bytes((ord('='), (b + 64) & 0xFF))
        else:
            out.append(b)
    return bytes(out)


class Fake:
    """Reads APC commands from the door and answers as TERMinator would."""

    def __init__(self, binary, mouse=True):
        self.binary = binary
        self.mouse = mouse
        self.module_sent = None
        self.buf = b""
        self.assets = {}        # sha256 -> bytes
        self.module = None
        self.pending = {}       # (kind, hash) -> bytearray
        self.from_door = []     # decoded module messages
        self.raw = b""          # plain terminal output, for spotting prompts

    # -- transport ---------------------------------------------------------
    def feed(self, chunk):
        self.buf += chunk
        self.raw = (self.raw + chunk)[-8192:]
        cmds = []
        while True:
            start = self.buf.find(b"\033_")
            if start < 0:
                self.buf = self.buf[-2:]        # keep a possible split ESC
                break
            end = self.buf.find(b"\033\\", start)
            if end < 0:
                break
            cmds.append(self.buf[start + 2:end])
            self.buf = self.buf[end + 2:]
        return cmds

    def send(self, fd, text):
        os.write(fd, (APC + text + ST).encode("latin-1"))

    def send_module_data(self, fd, payload):
        """Module -> door. TERMinator escapes every control byte on this path."""
        if self.binary:
            head = f"Data;module={MODULE}".encode()
            body = escape(head + b"\n" + payload)
            os.write(fd, b"\033_TERMinator:TRACE;Bin;" + body + b"\033\\")
        else:
            b64 = base64.b64encode(payload).decode()
            self.send(fd, f"Data;module={MODULE};b64={b64}")

    # -- command handling --------------------------------------------------
    def handle(self, fd, cmd):
        text = cmd.decode("latin-1", "replace")
        if not text.startswith("TERMinator:TRACE;"):
            return
        body = text[len("TERMinator:TRACE;"):]

        if body.startswith("Query"):
            caps = "Info;v=2;wasm=1;audio=1;assets=1;send=1;store=1;tick=1"
            if self.mouse:
                caps += ";text=1;mouse=1;pad=1;import=1"
            if self.binary:
                caps += ";bin=1"
            self.send(fd, caps)
            return

        if body.startswith("Asset;"):
            sha = re.search(r"sha256=([0-9a-f]{64})", body).group(1)
            if sha in self.assets:
                self.send(fd, f"Have;module={MODULE};sha256={sha}")
            else:
                self.pending[("asset", sha)] = bytearray()
                self.send(fd, f"NeedAsset;module={MODULE}")
            return

        if body.startswith("Open;"):
            sha = re.search(r"wasm=([0-9a-f]{64})", body).group(1)
            check("exclusive=1" in body, "Open asks for the whole screen (exclusive=1)")
            if self.module == sha:
                self.send(fd, f"Ready;module={MODULE}")
            else:
                self.pending[("module", sha)] = bytearray()
                self.send(fd, f"Need;module={MODULE}")
            return

        if body.startswith("Bin;"):
            # A binary frame carries its own header line, and it may be either
            # an upload chunk or a data message -- route on that, not on "Bin".
            payload = unescape(cmd[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head = payload[:nl].decode() if nl >= 0 else payload.decode()
            if head.startswith("Put;"):
                self.handle_put(fd, head, cmd)
            elif head.startswith("Data;"):
                # Strip the frame's own "Data;module=..." line; what follows is
                # the door's message, which dispatch() unwraps in turn.
                self.dispatch(payload[nl + 1:])
            return

        if body.startswith("Put;"):
            self.handle_put(fd, body, cmd)
            return

        if body.startswith("PutDone;"):
            sha = re.search(r"(?:asset|module)=([0-9a-f]{64})", body)
            if "asset=" in body:
                sha = re.search(r"asset=([0-9a-f]{64})", body).group(1)
                data = bytes(self.pending.pop(("asset", sha), b""))
                got = hashlib.sha256(data).hexdigest()
                check(got == sha, f"asset hash matches ({len(data)} bytes)")
                self.assets[sha] = data
                self.send(fd, f"Have;module={MODULE};sha256={sha}")
            else:
                key = next(k for k in self.pending if k[0] == "module")
                data = bytes(self.pending.pop(key))
                got = hashlib.sha256(data).hexdigest()
                check(got == key[1], f"module hash matches ({len(data)} bytes)")
                self.module = key[1]
                self.module_sent = data
                self.send(fd, f"Ready;module={MODULE}")
            return

        if body.startswith("Data;"):
            self.handle_data(body, cmd)
            return

        if body.startswith("Close;"):
            self.send(fd, f"Closed;module={MODULE}")

    def handle_put(self, fd, body, raw):
        if raw.startswith(b"TERMinator:TRACE;Bin;"):
            payload = unescape(raw[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head, data = payload[:nl].decode(), payload[nl + 1:]
        else:
            head, _, b64 = body.partition(";data=")
            data = base64.b64decode(b64)
        off = int(re.search(r"offset=(\d+)", head).group(1))
        kind = "asset" if "asset=" in head else "module"
        sha = re.search(r"(?:asset|module)=([0-9a-f]{64})", head)
        key = (kind, sha.group(1)) if sha else next(k for k in self.pending if k[0] == kind)
        buf = self.pending.setdefault(key, bytearray())
        if len(buf) < off + len(data):
            buf.extend(b"\0" * (off + len(data) - len(buf)))
        buf[off:off + len(data)] = data

    def handle_data(self, body, raw):
        if body.startswith("Bin;") or raw.startswith(b"TERMinator:TRACE;Bin;"):
            payload = unescape(raw[len("TERMinator:TRACE;Bin;"):])
            nl = payload.find(b"\n")
            head, data = payload[:nl].decode(), payload[nl + 1:]
            if head.startswith(f"Data;module={MODULE}"):
                self.dispatch(data)
            return
        if ";b64=" in body:
            data = base64.b64decode(body.split(";b64=", 1)[1])
            self.dispatch(data)
        else:
            # plain text, e.g. "Data;module=micropolis;tiles=<sha>"
            prefix = f"Data;module={MODULE};"
            if body.startswith(prefix):
                self.from_door.append(("text", body[len(prefix):]))

    def dispatch(self, data):
        nl = data.find(b"\n")
        if nl < 0:
            return
        payload = data[nl + 1:]
        if len(payload) < 8:
            return
        mtype, flags, count, nbytes = struct.unpack_from("<BBHI", payload, 0)
        self.from_door.append((mtype, count, payload[8:8 + nbytes]))



def packet(mtype, body=b""):
    return struct.pack("<BBHI", mtype, 0, 1, len(body)) + body


# --------------------------------------------------------- a made-up board ---

NODE1 = struct.Struct("<IiiIIIIHBBBBBB40s")     # LpNode in version 1 files, 76 bytes
NODE2 = struct.Struct("<IiiIIIIHBBBBBB40s8s")   # LpNode in version 2 files, 84 bytes (+ short name)
NODE = struct.Struct("<IiiIIIIHBBBBBB40s8shBxI") # LpNode, 92 bytes (version 3: + sensor reading)
CHAT = struct.Struct("<II16s232sB3x")           # LpChat, 260 bytes
LINK = struct.Struct("<IIIhBx")                 # LpLink, 16 bytes
HEAD = struct.Struct("<4sIIIIIIIIB3xIIII")      # LpHead, 56 bytes


def lp_file(path, nodes, chats, saved, status, version=3, links=(), hist=()):
    body = HEAD.pack(b"LPS1", version, len(nodes), len(chats), saved, saved - 600, saved - 5, 42, 9999, status,
                     len(links) if version == 3 else 0, len(hist) if version == 3 else 0,
                     saved // 60 if version == 3 else 0, 0)
    for n in nodes:
        body += NODE.pack(*n) if version == 3 else NODE2.pack(*n[:16]) if version == 2 else NODE1.pack(*n[:15])
    for c in chats:
        body += CHAT.pack(*c)
    if version == 3:
        for l in links:
            body += LINK.pack(*l)
        body += struct.pack("<%dH" % len(hist), *hist)
    with open(path + ".tmp", "wb") as f:
        f.write(body)
    os.replace(path + ".tmp", path)


def node(i, name, lat, lon, heard, state, hw=43, batt=80, short="", temp=None, hum=255):
    flags = 3 | (8 if temp is not None else 0)
    return (0x1000 + i, int(lat * 1e5), int(lon * 1e5), heard - 3600, heard, heard, 5, 390, hw, batt, state, flags, 0, 0,
            name.encode("latin-1"), short.encode("latin-1"), -32768 if temp is None else temp, hum, heard if temp else 0)


def wire_nodes(items):
    out = []
    for _, count, body in items:
        for i in range(count):
            rec = body[i * 68:(i + 1) * 68]
            out.append((struct.unpack_from("<I", rec, 0)[0], struct.unpack_from("<I", rec, 16)[0],
                        rec[26], rec[28:52].split(b"\0")[0].decode("latin-1"), rec[52:60].split(b"\0")[0].decode("latin-1"),
                        struct.unpack_from("<h", rec, 64)[0], rec[25], struct.unpack_from("<I", rec, 60)[0]))
    return out


def wire_links(items):
    out = []
    for _, count, body in items:
        for i in range(count):
            a, b, lh, snr, src = struct.unpack_from("<IIIhB", body, i * 16)
            out.append((a, b, snr, src))
    return out


def wire_chat(items):
    out = []
    for _, count, body in items:
        o = 0
        for _ in range(count):
            t, st, nl, tl, cl = struct.unpack_from("<IBBBB", body, o)
            o += 8
            name = body[o:o + nl].decode("latin-1"); o += nl
            chan = body[o:o + cl].decode("latin-1"); o += cl
            text = body[o:o + tl].decode("latin-1"); o += tl
            out.append((name, chan, text))
    return out


def main():
    binary = "--bin" in sys.argv
    print("Listening Post door test (%s)" % ("binary frames" if binary else "base64"))
    here = os.path.dirname(os.path.abspath(__file__))
    top = os.path.dirname(here)
    need = {"lpdoor": os.path.join(here, "lpdoor"), "lp.wasm": os.path.join(top, "module", "lp.wasm"),
            "states.bin": os.path.join(top, "data", "states.bin")}
    for k, v in need.items():
        if not os.path.exists(v):
            print(f"missing {v} -- build it first")
            return 1

    root = tempfile.mkdtemp(prefix="lptest-")
    board = os.path.join(root, "door")
    os.makedirs(os.path.join(board, "data"))
    for k, v in need.items():
        shutil.copy(v, os.path.join(board, k))
    # A stub collector: says it ran, then leaves (the real one needs the network).
    with open(os.path.join(board, "lpcollect"), "w") as f:
        f.write("#!/bin/sh\necho started >> \"$(dirname \"$0\")/data/stub_ran\"\n")
    os.chmod(os.path.join(board, "lpcollect"), 0o755)
    now = int(time.time())
    ga = 11                                      # Georgia in states.bin order
    nodes = [node(1, "Peach Relay", 33.76, -84.38, now - 30, ga), node(2, "Bay Watch", 37.78, -122.42, now - 4000, 5),
             node(3, "", 40.0, -100.0, now - 90000, 0, hw=0, batt=255)]
    chats = [(now - 300, 0x1001, b"LongFast", b"Morning from Atlanta", ga), (now - 200, 0x1002, b"MediumFast", b"caf\xe9 test", 5)]
    # The snapshot as the first collector wrote it (version 1, no short names): an upgrade must still read it.
    lp_file(os.path.join(board, "data", "snapshot.bin"), nodes, chats, now - 60, 0, version=1)
    # The caller told the Weather door their town: the door beside this one.
    os.makedirs(os.path.join(root, "weather", "saves", "Tester-1"))
    with open(os.path.join(root, "weather", "saves", "Tester-1", "prefs"), "w") as f:
        f.write("units = 0\nplace = Atlanta, Georgia\nlat = 33.7490\nlon = -84.3880\n")
    drop = os.path.join(root, "node1")
    os.makedirs(drop)
    with open(os.path.join(drop, "door32.sys"), "w") as f:
        f.write("0\n0\n38400\nTest BBS\n1\nTester Real\nTester\n50\n60\n1\n1\n")

    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(tempfile.gettempdir())          # a BBS never starts a door in its folder
        os.execv(os.path.join(board, "lpdoor"), ["lpdoor", drop])
        os._exit(1)

    fake = Fake(binary, True)
    deadline = time.time() + 60
    step = "menu"
    got = {}
    step_at = time.time()
    here_file = os.path.join(board, "data", "doors", str(pid))
    try:
        while time.time() < deadline and step != "done":
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                for cmd in fake.feed(chunk):
                    fake.handle(fd, cmd)
            for item in fake.from_door:
                got.setdefault(item[0], []).append(item[1] if item[0] == "text" else item)
            fake.from_door = []

            if step == "menu" and b"Choose how to listen" in fake.raw:
                check(b"DETECTED" in fake.raw, "the menu says TRACE was detected")
                check(os.path.exists(here_file), "the door made its 'I'm here' file before the menu")
                os.write(fd, b"1")
                step = "open"
            elif step == "open" and "start" in got.get("text", []):
                texts = got.get("text", [])
                states_hash = hashlib.sha256(open(need["states.bin"], "rb").read()).hexdigest()
                check(f"states={states_hash}" in texts, "the module was told the outlines' hash")
                fake.send_module_data(fd, packet(OUT_READY))
                step = "ready"
            elif step == "ready" and IN_STATS in got and IN_CHAT in got:
                hello = got.get(IN_HELLO, [])
                hb = hello[-1][2] if hello else b""
                check(len(hb) == 56 and hb[4] == 0 and hb[5] == 7, "hello: region All US, 7 days")
                check(len(hb) == 56 and hb[6] == 1 and hb[7] == ga and hb[16:56].split(b"\0")[0] == b"Atlanta, Georgia"
                      and abs(struct.unpack_from("<i", hb, 8)[0] - 3374900) < 2,
                      "hello: the caller's town from the Weather door, placed in Georgia")
                ns = wire_nodes(got[IN_NODES])
                names = sorted(n[3] for n in ns)
                check(names == ["", "Bay Watch", "Peach Relay"],
                      f"every node heard in the last 7 days, read from a version 1 snapshot ({names})")
                check(IN_DONE in got, "the full list is marked done")
                ch = wire_chat(got[IN_CHAT])
                check(ch == [("Peach Relay", "LongFast", "Morning from Atlanta"), ("Bay Watch", "MediumFast", "caf\xe9 test")],
                      f"chat, with the sender's long name, Latin-1 intact ({ch})")
                st = got[IN_STATS][-1][2]
                check(len(st) == 28 and st[24] in (0, 1), f"stats: the feed isn't live (stale snapshot), status {st[24]}")
                check(all(len(x[2]) <= 4096 for x in got[IN_NODES]), "every node packet fits the 4 KB payload limit")
                for k in (IN_NODES, IN_CHAT, IN_STATS, IN_DONE):
                    got.pop(k, None)
                # The collector writes live.bin: one node heard again, one new node, a new chat line, live.
                t = int(time.time())
                live_nodes = [node(1, "Peach Relay", 33.76, -84.38, t, ga, short="PCH1"),
                              node(4, "", 33.95, -83.37, t, ga, short="NK", temp=215, hum=40)]
                links = [(0x1001, 0x1004, t, 26, 3)]
                lp_file(os.path.join(board, "data", "live.bin"), live_nodes,
                        chats + [(t, 0x1004, b"LongFast", b"hello mesh", ga)], t, 2, links=links, hist=[5, 7, 9])
                step = "live"
                step_at = time.time()
            elif step == "live" and IN_CHAT in got and IN_NODES in got and IN_STATS in got and IN_LINKS in got and IN_HISTORY in got:
                ns = wire_nodes(got[IN_NODES])
                nk = [n for n in ns if n[4] == "NK"]
                check(nk and nk[0][5] == 215 and nk[0][6] & 8, f"a sensor's reading comes along ({nk[0][5] if nk else None})")
                check(wire_links(got[IN_LINKS]) == [(0x1001, 0x1004, 26, 3)], f"the new link arrives ({wire_links(got[IN_LINKS])})")
                hh = got[IN_HISTORY][-1][2]
                check(struct.unpack_from("<H", hh, 4)[0] == 3 and struct.unpack_from("<3H", hh, 8) == (5, 7, 9),
                      "the traffic history arrives")
                check(sorted((n[3], n[4], n[0]) for n in ns) == [("", "NK", 0x1004), ("Peach Relay", "PCH1", 0x1001)],
                      f"only live.bin's changed nodes arrive, with short names and numbers ({[(n[3], n[4]) for n in ns]})")
                ch = wire_chat(got[IN_CHAT])
                check(ch[-1] == ("NK", "LongFast", "hello mesh"), f"a sender with no long name goes by its short name ({ch[-1]})")
                st = got[IN_STATS][-1][2]
                check(st[24] == 2 and struct.unpack_from("<I", st, 16)[0] == 42, "stats: live, 42 packets a minute")
                fake.send_module_data(fd, packet(OUT_REGION, bytes([ga])))
                step = "region"
                step_at = time.time()
            elif step == "region" and time.time() - step_at > 1.5:
                prefs = open(os.path.join(board, "saves", "Tester-1", "prefs")).read()
                check("region = 11" in prefs and "display = 1" in prefs, "the region and TRACE choice are saved for the caller")
                check(os.path.exists(os.path.join(board, "data", "stub_ran")), "the door started the collector")
                fake.send(fd, f"Closed;module={MODULE};code=0")
                step = "closing"
                step_at = time.time()
            elif step == "closing" and time.time() - step_at > 3:
                break
            if step == "live" and time.time() - step_at > 10:
                break

        if step == "closing":
            for _ in range(30):
                if os.waitpid(pid, os.WNOHANG)[0]:
                    pid = -1
                    break
                time.sleep(0.1)
            check(pid == -1, "the door ends when the module closes")
            check(not os.path.exists(here_file), "the 'I'm here' file is gone when the door ends")
            step = "done"
        check(step == "done", f"got through every step (stopped at '{step}')")
        check(fake.module_sent == open(need["lp.wasm"], "rb").read(), "door sent lp.wasm")
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        if pid > 0:
            try:
                os.kill(pid, 9)
                os.waitpid(pid, 0)
            except (OSError, ChildProcessError):
                pass
        shutil.rmtree(root, ignore_errors=True)
    print()
    if fails:
        print(f"FAILED ({len(fails)}): " + "; ".join(fails))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
