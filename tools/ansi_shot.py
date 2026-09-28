#!/usr/bin/env python3
"""Runs the door on a pty as an ANSI caller, types a script of keys, and renders
the screen to a PNG with a small 24-bit ANSI / CP437 terminal emulator -- so the
ANSI view can be looked at without a BBS or a terminal program.

    python3 ansi_shot.py out.png [keys...]

Each key argument is sent after the screen settles: plain text is typed, and
@ENTER @ESC @UP @DOWN @LEFT @RIGHT @WAIT are special. Set LPDIR to run the door from another folder. The door is run with no
drop file (local mode), so TRACE is never detected and the menu offers ANSI.
"""
import os
import pty
import re
import select
import sys
import time

from PIL import Image, ImageDraw, ImageFont

COLS, ROWS, CW, CH = 80, 25, 9, 18
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"


class Screen:
    def __init__(self):
        self.cells = [[(" ", (200, 200, 200), (0, 0, 0)) for _ in range(COLS)] for _ in range(ROWS)]
        self.r = self.c = 0
        self.fg, self.bg = (170, 170, 170), (0, 0, 0)
        self.saved = (0, 0)

    def sgr(self, params):
        p = [int(x) if x else 0 for x in params.split(";")] if params else [0]
        i = 0
        base = [(0, 0, 0), (170, 0, 0), (0, 170, 0), (170, 85, 0), (0, 0, 170), (170, 0, 170), (0, 170, 170), (170, 170, 170)]
        bright = [(85, 85, 85), (255, 85, 85), (85, 255, 85), (255, 255, 85), (85, 85, 255), (255, 85, 255), (85, 255, 255), (255, 255, 255)]
        bold = False
        while i < len(p):
            v = p[i]
            if v == 0: self.fg, self.bg = (170, 170, 170), (0, 0, 0)
            elif v == 1: bold = True
            elif 30 <= v <= 37: self.fg = (bright if bold else base)[v - 30]
            elif 40 <= v <= 47: self.bg = base[v - 40]
            elif v in (38, 48) and i + 4 < len(p) + 0 and p[i + 1] == 2:
                col = (p[i + 2], p[i + 3], p[i + 4])
                if v == 38: self.fg = col
                else: self.bg = col
                i += 4
            i += 1

    def feed(self, data):
        # An escape sequence can be split across two reads: keep the tail.
        data = getattr(self, "rest", b"") + data
        self.rest = b""
        cut = data.rfind(b"\x1b")
        if cut >= 0 and not re.match(rb"\x1b\[[?0-9;]*[A-Za-z]", data[cut:]) and len(data) - cut < 32:
            self.rest = data[cut:]
            data = data[:cut]
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0x1B:
                m = re.match(rb"\x1b\[([?0-9;]*)([A-Za-z])", data[i:])
                if not m:
                    m2 = re.match(rb"\x1b_.*?\x1b\\\\", data[i:], re.S)
                    i += m2.end() if m2 else 1
                    continue
                params, cmd = m.group(1).decode(), m.group(2).decode()
                i += m.end()
                if cmd == "H":
                    nums = [int(x) for x in params.split(";") if x.isdigit()] or [1, 1]
                    self.r, self.c = nums[0] - 1, (nums[1] - 1 if len(nums) > 1 else 0)
                elif cmd == "J" and params == "2":
                    self.cells = [[(" ", self.fg, self.bg) for _ in range(COLS)] for _ in range(ROWS)]
                elif cmd == "m": self.sgr(params)
                elif cmd == "C": self.c += int(params or 1)
                elif cmd == "s": self.saved = (self.r, self.c)
                elif cmd == "u": self.r, self.c = self.saved
                continue
            i += 1
            if b == 13: self.c = 0
            elif b == 10: self.r = min(self.r + 1, ROWS - 1)
            elif b == 8: self.c = max(0, self.c - 1)
            elif b >= 32:
                if 0 <= self.r < ROWS and 0 <= self.c < COLS:
                    self.cells[self.r][self.c] = (bytes([b]).decode("cp437"), self.fg, self.bg)
                self.c += 1
                if self.c >= COLS: self.c = 0; self.r = min(self.r + 1, ROWS - 1)

    def png(self, path):
        img = Image.new("RGB", (COLS * CW, ROWS * CH))
        d = ImageDraw.Draw(img)
        font = ImageFont.truetype(FONT, 15)
        for r in range(ROWS):
            for c in range(COLS):
                ch, fg, bg = self.cells[r][c]
                x, y = c * CW, r * CH
                d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=bg)
                # Block elements drawn as shapes, so they tile like a real terminal.
                if ch == "█": d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=fg)
                elif ch == "▀": d.rectangle([x, y, x + CW - 1, y + CH // 2 - 1], fill=fg)
                elif ch == "▄": d.rectangle([x, y + CH // 2, x + CW - 1, y + CH - 1], fill=fg)
                elif ch != " ": d.text((x, y + 1), ch, font=font, fill=fg)
        img.save(path)


def main():
    out = sys.argv[1]
    keys = sys.argv[2:]
    here = os.environ.get("LPDIR") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "door")
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(here)
        os.execv("./lpdoor", ["./lpdoor"])
    scr = Screen()

    def pump(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return
                scr.feed(data)

    pump(3.5)
    special = {"@ENTER": b"\r", "@ESC": b"\x1b", "@UP": b"\x1b[A", "@DOWN": b"\x1b[B",
               "@LEFT": b"\x1b[D", "@RIGHT": b"\x1b[C", "@BACK": b"\x7f"}
    for k in keys:
        if k == "@WAIT": pump(3); continue
        os.write(fd, special.get(k, k.encode()))
        pump(1.2 if k in special else 0.6)
    pump(1.5)
    scr.png(out)
    os.kill(pid, 9)
    print("wrote", out)


if __name__ == "__main__":
    main()
