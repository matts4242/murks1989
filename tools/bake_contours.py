#!/usr/bin/env python3
"""Bakes the relief data for a Murks 1989 map's topographic mode (work board MRK-23).

Reads the same Copernicus DEM GLO-90 tiles as bake_region.py, warps them onto a 90 m
raster in the map's own frame (EPSG:3035, x east and y south of the map's north-west
corner, in metres) and smooths out single-pixel noise. From that it writes, next to the
map as <map>.murkc:

  - a height raster at 180 m, which the game draws as stepped height bands: the terrain
    itself changes tone at every 20 m (or 100 m) step, so the border between two bands
    is the contour line;
  - a height figure for every 100 m contour long enough to carry one, cut to the
    64 x 64-hex chunks the game streams;
  - spot heights on summits.

Usage (same bounding box and cache as the map bake):
  python3 tools/bake_contours.py --bbox 10 49 24.2 55 --out game/data/poland_1989.murkc --cache ~/murks_cache
Needs: numpy rasterio pyproj shapely contourpy scipy.

File format (little endian), stored like the map inside a "MURKSZ01" zlib container:
  magic "MURKSCT2", u32 chunks_x, u32 chunks_y, u32 raster_w, u32 raster_h,
  f32 raster_pixel_m, f32 height_unit_m, f32 height_offset_m, u32 raster_offset
  chunks_x * chunks_y entries (row by row): u32 offset, u32 label_count, u32 spot_count
  per chunk at its offset:
    labels: i16 x, i16 y, i16 level_m, i16 angle (1/100 rad, direction of the contour there)
    spots:  i16 x, i16 y, i16 elevation_m
    coordinates: metres from the chunk origin / 4
  at raster_offset: raster_w * raster_h u16, row by row. Each value is the difference
    from the plane through its left, upper and upper-left neighbours (missing ones 0),
    modulo 65536; height = value * height_unit_m - height_offset_m. Pixel (i, j) has its
    centre at ((j + 0.5) * pixel, (i + 0.5) * pixel) metres.
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
LABEL_INTERVAL = 100   # a height figure on every contour of this spacing
RASTER_STEP = 2        # the height raster keeps every 2nd pixel: 180 m
HEIGHT_UNIT_M = 1.0    # height step in the file
HEIGHT_OFFSET_M = 100.0
LABEL_MIN_LENGTH_M = 3000.0
SMOOTH_SIGMA = 1.2     # pixels; GLO-90 is a surface model, so woods and towns add noise
SPOT_WINDOW_M = 5000.0 # a spot height is the highest point within this distance ...
SPOT_PROMINENCE_M = 40.0  # ... standing at least this far above the lowest ground in it
SPOTS_PER_CHUNK = 6
UNIT_M = 4.0           # coordinate step in the file


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
    n_labels = n_spots = 0
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
                first = max(LABEL_INTERVAL, int(math.floor(zmin / LABEL_INTERVAL) + 1) * LABEL_INTERVAL)
                if first <= zmax:
                    gen = contour_generator(xs, ys, z, line_type=LineType.Separate)
                    for level in range(first, int(zmax) + 1, LABEL_INTERVAL):
                        for line in gen.lines(float(level)):
                            if len(line) < 2:
                                continue
                            g = shapely.clip_by_rect(shapely.LineString(line), ox, oy, x1, y1)
                            for part in shapely.get_parts(g):
                                if shapely.get_type_id(part) != 1 or shapely.length(part) < LABEL_MIN_LENGTH_M:
                                    continue  # 1 = LineString
                                # The figure sits at the middle of the line, along its direction there.
                                half = shapely.length(part) * 0.5
                                a = shapely.get_coordinates(shapely.line_interpolate_point(part, half - 200.0))[0]
                                m = shapely.get_coordinates(shapely.line_interpolate_point(part, half))[0]
                                b = shapely.get_coordinates(shapely.line_interpolate_point(part, half + 200.0))[0]
                                ang = math.atan2(b[1] - a[1], b[0] - a[0])
                                if ang > math.pi / 2:
                                    ang -= math.pi
                                if ang < -math.pi / 2:
                                    ang += math.pi
                                out += struct.pack("<4h", int(round((m[0] - ox) / UNIT_M)), int(round((m[1] - oy) / UNIT_M)),
                                                   level, int(round(ang * 100)))
                                count += 1
            sel = np.nonzero((spot_x >= ox) & (spot_x < ox + chunk_w) & (spot_y >= oy) & (spot_y < oy + chunk_h))[0]
            sel = sel[np.argsort(-spot_p[sel])][:SPOTS_PER_CHUNK]
            for k in sel:
                out += struct.pack("<hhh", int(round((spot_x[k] - ox) / UNIT_M)),
                                   int(round((spot_y[k] - oy) / UNIT_M)), int(round(spot_e[k])))
            table.append((count, len(sel)))
            blocks.append(bytes(out))
            n_labels += count
            n_spots += len(sel)
        br.log(f"  chunk row {cy + 1}/{grid.chunks_y}: {n_labels} height figures")

    # Height raster: 2 x 2 block means of the smoothed 90 m raster, plane-predicted so that
    # zlib sees small numbers.
    s = RASTER_STEP
    hh, ww = (elev.shape[0] // s) * s, (elev.shape[1] // s) * s
    coarse = elev[:hh, :ww].reshape(hh // s, s, ww // s, s).mean(axis=(1, 3))
    q = np.clip(np.rint((coarse + HEIGHT_OFFSET_M) / HEIGHT_UNIT_M), 0, 65535).astype(np.int64)
    pad = np.zeros((q.shape[0] + 1, q.shape[1] + 1), dtype=np.int64)
    pad[1:, 1:] = q
    resid = (pad[1:, 1:] - pad[1:, :-1] - pad[:-1, 1:] + pad[:-1, :-1]) & 0xFFFF
    raster = resid.astype("<u2").tobytes()

    header_size = 8 + 4 * 4 + 3 * 4 + 4
    offset = header_size + 12 * len(blocks)
    index = bytearray()
    for (labels, spots), block in zip(table, blocks):
        index += struct.pack("<3I", offset, labels, spots)
        offset += len(block)
    header = b"MURKSCT2" + struct.pack("<4I3fI", grid.chunks_x, grid.chunks_y, q.shape[1], q.shape[0],
                                       CONTOUR_PIXEL * s, HEIGHT_UNIT_M, HEIGHT_OFFSET_M, offset)
    assert len(header) == header_size
    raw = header + bytes(index) + b"".join(blocks) + raster
    packed = b"MURKSZ01" + struct.pack("<Q", len(raw)) + zlib.compress(raw, 9)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(packed)
    br.log(f"wrote {args.out}: height raster {q.shape[1]} x {q.shape[0]} @ {CONTOUR_PIXEL * s:.0f} m, {n_labels} height figures, "
           f"{n_spots} spot heights, {len(raw) / 1e6:.1f} MB raw, {len(packed) / 1e6:.1f} MB packed, {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
