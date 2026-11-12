#!/usr/bin/env python3
"""Bakes contour lines and spot heights for a Murks 1989 map (work board MRK-23).

Reads the same Copernicus DEM GLO-90 tiles as bake_region.py, warps them onto a 90 m
raster in the map's own frame (EPSG:3035, x east and y south of the map's north-west
corner, in metres), smooths out single-pixel noise, and traces a contour line every
20 m. The lines are simplified, cut to the 64 x 64-hex chunks the game streams, and
written next to the map as <map>.murkc. The game shows every line close in and only
every 100 m further out.

Usage (same bounding box and cache as the map bake):
  python3 tools/bake_contours.py --bbox 10 49 24.2 55 --out game/data/poland_1989.murkc --cache ~/murks_cache
Needs: numpy rasterio pyproj shapely contourpy scipy.

File format (little endian), stored like the map inside a "MURKSZ01" zlib container:
  magic "MURKSCT1", u32 chunks_x, u32 chunks_y, u32 interval_m, u32 reserved
  chunks_x * chunks_y entries (row by row): u32 offset, u32 line_count, u32 spot_count
  per chunk at its offset:
    lines: i16 level_m, u16 point_count, then per point two varints (dx, dy): the step
           from the point before (the first from the chunk origin), zigzag-coded
    spots: i16 x, i16 y, i16 elevation_m
  coordinates: metres from the chunk origin / 4, so four-metre steps
"""
import argparse
import math
import os
import struct
import sys
import time
import zlib

import numpy as np
import shapely
from contourpy import contour_generator, LineType
from rasterio.enums import Resampling
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake_region as br

CONTOUR_PIXEL = 90.0   # raster step in metres: the source data's own resolution
INTERVAL = 20          # metres between contour lines
SMOOTH_SIGMA = 1.2     # pixels; GLO-90 is a surface model, so woods and towns add noise
SIMPLIFY_M = 40.0      # Douglas-Peucker tolerance
MIN_LENGTH_M = 900.0   # shorter lines are noise at this scale
SPOT_WINDOW_M = 5000.0 # a spot height is the highest point within this distance ...
SPOT_PROMINENCE_M = 40.0  # ... standing at least this far above the lowest ground in it
SPOTS_PER_CHUNK = 6
UNIT_M = 4.0           # coordinate step in the file


def varints(values):
    """Zigzag varint coding of a 1-D int array: small steps take one byte."""
    v = values.astype(np.int64)
    z = ((v << 1) ^ (v >> 63)).astype(np.uint64)
    out = bytearray()
    for n in z.tolist():
        while n >= 0x80:
            out.append((n & 0x7F) | 0x80)
            n >>= 7
        out.append(n)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", type=float, nargs=4, default=[10.0, 49.0, 24.2, 55.0], metavar=("LON0", "LAT0", "LON1", "LAT1"))
    ap.add_argument("--out", default="game/data/poland_1989.murkc")
    ap.add_argument("--cache", default=os.path.expanduser("~/murks_cache"))
    args = ap.parse_args()
    os.makedirs(args.cache, exist_ok=True)
    os.environ.setdefault("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR")
    os.environ.setdefault("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif")
    t0 = time.time()

    br.PIXEL = CONTOUR_PIXEL  # the grid and the warp read this module global
    grid = br.Grid(args.bbox)
    br.log(f"grid: {grid.chunks_x} x {grid.chunks_y} chunks, raster {grid.px_w} x {grid.px_h} @ {CONTOUR_PIXEL:.0f} m")
    tiles = br.tile_list(grid.geo_bounds, 1)
    br.log(f"elevation: {len(tiles)} Copernicus GLO-90 tiles")
    elev = br.warp_tiles(grid, tiles, br.DEM_URL, 1, 1, Resampling.bilinear, np.float32, np.nan, args.cache, "dem")
    elev[np.isnan(elev)] = 0.0  # open sea
    elev = ndimage.gaussian_filter(elev, SMOOTH_SIGMA, mode="nearest")
    br.log(f"elevation {elev.min():.0f} .. {elev.max():.0f} m")

    # Spot heights: local summits that stand clear of the ground around them.
    win = int(SPOT_WINDOW_M / CONTOUR_PIXEL) | 1
    small = elev[::2, ::2]  # half resolution is plenty for finding summits
    hi = ndimage.maximum_filter(small, size=win // 2 | 1, mode="nearest")
    lo = ndimage.minimum_filter(small, size=win // 2 | 1, mode="nearest")
    si, sj = np.nonzero((small == hi) & (hi - lo >= SPOT_PROMINENCE_M) & (small > 1.0))
    spot_x = (sj * 2 + 0.5) * CONTOUR_PIXEL
    spot_y = (si * 2 + 0.5) * CONTOUR_PIXEL
    spot_e = small[si, sj]
    spot_p = (hi - lo)[si, sj]
    del hi, lo, small
    br.log(f"spot heights: {len(spot_x)} candidates")

    chunk_w = br.CHUNK * br.HEX_WIDTH
    chunk_h = br.CHUNK * br.ROW_SPACING
    blocks, table = [], []
    n_lines = n_pts = n_spots = 0
    for cy in range(grid.chunks_y):
        for cx in range(grid.chunks_x):
            ox, oy = cx * chunk_w, cy * chunk_h
            # A chunk's hexes reach half a hex past its origin grid on the right and below.
            x1, y1 = ox + chunk_w + br.HEX_WIDTH * 0.5, oy + chunk_h + br.HEX_SIZE * 0.5
            j0, j1 = max(0, int(ox / CONTOUR_PIXEL) - 1), min(grid.px_w, int(x1 / CONTOUR_PIXEL) + 3)
            i0, i1 = max(0, int(oy / CONTOUR_PIXEL) - 1), min(grid.px_h, int(y1 / CONTOUR_PIXEL) + 3)
            out = bytearray()
            count = 0
            if j1 - j0 >= 2 and i1 - i0 >= 2:
                z = elev[i0:i1, j0:j1]
                xs = (np.arange(j0, j1) + 0.5) * CONTOUR_PIXEL
                ys = (np.arange(i0, i1) + 0.5) * CONTOUR_PIXEL
                zmin, zmax = float(z.min()), float(z.max())
                first = int(math.floor(zmin / INTERVAL) + 1) * INTERVAL
                if first <= zmax:
                    gen = contour_generator(xs, ys, z, line_type=LineType.Separate)
                    for level in range(max(first, INTERVAL), int(zmax) + 1, INTERVAL):
                        lines = [l for l in gen.lines(float(level)) if len(l) >= 2]
                        if not lines:
                            continue
                        geoms = np.array([shapely.LineString(l) for l in lines], dtype=object)
                        geoms = geoms[shapely.length(geoms) >= MIN_LENGTH_M]
                        if len(geoms) == 0:
                            continue
                        geoms = shapely.simplify(geoms, SIMPLIFY_M, preserve_topology=False)
                        geoms = shapely.clip_by_rect(geoms, ox, oy, x1, y1)
                        parts = shapely.get_parts(geoms)
                        for g in parts:
                            if shapely.get_type_id(g) != 1 or shapely.length(g) < 200.0:  # 1 = LineString
                                continue
                            c = shapely.get_coordinates(g)
                            q = np.rint((c - (ox, oy)) / UNIT_M).astype(np.int64)
                            keep = np.ones(len(q), dtype=bool)
                            keep[1:] = np.any(q[1:] != q[:-1], axis=1)
                            q = q[keep]
                            for k0 in range(0, len(q), 60000):  # u16 point count
                                seg = q[max(0, k0 - 1):k0 + 60000]
                                if len(seg) < 2:
                                    continue
                                d = np.diff(seg, axis=0, prepend=np.zeros((1, 2), dtype=np.int64))
                                out += struct.pack("<hH", level, len(seg)) + varints(d.reshape(-1))
                                count += 1
                                n_pts += len(seg)
            sel = np.nonzero((spot_x >= ox) & (spot_x < ox + chunk_w) & (spot_y >= oy) & (spot_y < oy + chunk_h))[0]
            sel = sel[np.argsort(-spot_p[sel])][:SPOTS_PER_CHUNK]
            for k in sel:
                out += struct.pack("<hhh", int(round((spot_x[k] - ox) / UNIT_M)),
                                   int(round((spot_y[k] - oy) / UNIT_M)), int(round(spot_e[k])))
            table.append((count, len(sel)))
            blocks.append(bytes(out))
            n_lines += count
            n_spots += len(sel)
        br.log(f"  chunk row {cy + 1}/{grid.chunks_y}: {n_lines} lines, {n_pts} points")

    header = b"MURKSCT1" + struct.pack("<4I", grid.chunks_x, grid.chunks_y, INTERVAL, 0)
    offset = len(header) + 12 * len(blocks)
    index = bytearray()
    for (lines, spots), block in zip(table, blocks):
        index += struct.pack("<3I", offset, lines, spots)
        offset += len(block)
    raw = header + bytes(index) + b"".join(blocks)
    packed = b"MURKSZ01" + struct.pack("<Q", len(raw)) + zlib.compress(raw, 9)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(packed)
    br.log(f"wrote {args.out}: {n_lines} lines, {n_pts} points, {n_spots} spot heights, "
           f"{len(raw) / 1e6:.1f} MB raw, {len(packed) / 1e6:.1f} MB packed, {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
