#!/usr/bin/env python3
"""Census state outlines -> states.bin, the Listening Post's map.

    pip install --target <dir> pyshp
    PYTHONPATH=<dir> python3 make_states.py <cb_2023_us_state_20m dir or .shp> [out]

Source: the US Census Bureau's cartographic boundary file for states, 1:20,000,000
(cb_2023_us_state_20m), which is public domain. 50 states, DC and Puerto Rico.

states.bin, little-endian:
  "STS1" u32 count
  per state, sorted by name:
    char abbr[4] (2 used, NUL padded)   char name[24] (NUL padded)
    f32 minLat, minLon, maxLat, maxLon  u32 rings  u32 points
    per ring: u32 n, then n x (f32 lat, f32 lon)
Alaska's Aleutians cross 180 degrees: its eastern-hemisphere longitudes are
stored minus 360, so every state is one unbroken range of longitude.
"""
import os
import struct
import sys

import shapefile


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "cb_2023_us_state_20m"
    if src.endswith(".shp"):
        src = src[:-4]
    if os.path.isdir(src):
        src = os.path.join(src, "cb_2023_us_state_20m")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "data", "states.bin")

    states = []
    for sr in shapefile.Reader(src).iterShapeRecords():
        rec = sr.record.as_dict()
        abbr, name = rec["STUSPS"], rec["NAME"]
        pts = sr.shape.points
        parts = list(sr.shape.parts) + [len(pts)]
        rings = []
        for a, b in zip(parts, parts[1:]):
            ring = []
            for lon, lat in pts[a:b]:
                if abbr == "AK" and lon > 0:
                    lon -= 360.0
                ring.append((lat, lon))
            if len(ring) >= 3:
                rings.append(ring)
        lats = [p[0] for r in rings for p in r]
        lons = [p[1] for r in rings for p in r]
        states.append((name, abbr, (min(lats), min(lons), max(lats), max(lons)), rings))
    states.sort(key=lambda s: s[0])

    blob = bytearray(b"STS1" + struct.pack("<I", len(states)))
    for name, abbr, bb, rings in states:
        blob += abbr.encode().ljust(4, b"\0") + name.encode()[:23].ljust(24, b"\0")
        blob += struct.pack("<ffffII", *bb, len(rings), sum(len(r) for r in rings))
        for r in rings:
            blob += struct.pack("<I", len(r))
            for lat, lon in r:
                blob += struct.pack("<ff", lat, lon)
    with open(out, "wb") as f:
        f.write(blob)
    print("%s: %d states, %d bytes" % (out, len(states), len(blob)))


if __name__ == "__main__":
    main()
