#!/usr/bin/env python3
"""Bake a real-world region into a Murks map (500 m hexes, EPSG:3035).

Sources (all free for commercial use with attribution, see README "Map data credits"):
  - Elevation: Copernicus DEM GLO-90 (AWS open data, read in place as COGs)
  - Land cover: ESA WorldCover 2021 v200 (read from the COG overviews at ~80 m)
  - Rivers, roads, railways: Natural Earth 10m (public domain)
  - Settlements: GeoNames cities1000 (CC BY 4.0), with a few 1989 name fixes

Steps: build a 125 m raster grid in EPSG:3035 aligned with the hex grid, warp
elevation and land cover into it, aggregate pixels into hexes (mean elevation,
land-cover fractions), burn rivers/roads/rail as hex features, place towns,
write a MURKSMAP v2 file. Then run finish_map (adds local roads) and compress.

Usage:
  python3 tools/bake_region.py --name "Poland and East Germany 1989" \
      --bbox 10 49 24.2 55 --out game/data/poland_1989.murkz --cache ~/murks_cache
Needs: numpy rasterio pyproj shapely pyogrio pillow; the finish_map tool built from CMakeLists.txt.
"""
import argparse
import concurrent.futures as cf
import io
import math
import os
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
import zlib

import numpy as np
import pyogrio
import rasterio
import shapely
from pyproj import Transformer
from rasterio.enums import Resampling
from rasterio.transform import Affine
from rasterio.warp import reproject

# --- Hex grid (must match core/include/murks/hex.hpp) --------------------------------
CHUNK = 64
SQRT3 = math.sqrt(3.0)
HEX_WIDTH = 500.0
HEX_SIZE = HEX_WIDTH / SQRT3
ROW_SPACING = HEX_SIZE * 1.5
PIXEL = 125.0  # sampling raster, ~14 samples per hex

# Terrain codes (core/include/murks/map.hpp)
T_WATER, T_OPEN, T_FIELDS, T_FOREST, T_WETLAND, T_URBAN = range(6)
F_ROAD, F_MAJOR_ROAD, F_RAIL, F_RIVER, F_BRIDGE, F_MAJOR_RIVER = 1, 2, 4, 8, 16, 32
NO_SETTLEMENT = 0xFFFF
# Network edge bits (map.hpp kNetShift): a hex stores its E, NE and NW edges.
ROAD, RAIL, RIVER = (F_ROAD, 6), (F_RAIL, 9), (F_RIVER, 12)
EVEN_NB = [(1, 0), (0, -1), (-1, -1), (-1, 0), (-1, 1), (0, 1)]
ODD_NB = [(1, 0), (1, -1), (0, -1), (-1, 0), (0, 1), (1, 1)]

# WorldCover classes -> compact index
WC_CODES = {0: 0, 10: 1, 20: 2, 30: 3, 40: 4, 50: 5, 60: 6, 70: 7, 80: 8, 90: 9, 95: 10, 100: 11}
K = 12
WC_LUT = np.zeros(256, dtype=np.uint8)
for code, k in WC_CODES.items():
    WC_LUT[code] = k

# Names that were different in 1989.
NAMES_1989 = {
    "Chemnitz": "Karl-Marx-Stadt",
    "Lviv": "Lvov",
    "Hrodna": "Grodno",
    "Chervonohrad": "Chervonograd",
    "Novovolynsk": "Novovolynsk",
}
SKIP_FEATURE_CODES = {"PPLX", "PPLH", "PPLQ", "PPLW", "PPLCH"}

DEM_URL = "https://copernicus-dem-90m.s3.amazonaws.com/Copernicus_DSM_COG_30_{ns}{lat:02d}_00_{ew}{lon:03d}_00_DEM/Copernicus_DSM_COG_30_{ns}{lat:02d}_00_{ew}{lon:03d}_00_DEM.tif"
WC_URL = "https://esa-worldcover.s3.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_{ns}{lat:02d}{ew}{lon:03d}_Map.tif"
NE_URL = "https://naciscdn.org/naturalearth/10m/{kind}/{name}.zip"
GEONAMES_URL = "https://download.geonames.org/export/dump/cities1000.zip"


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def world_to_hex(x, y):
    """Vectorised world (metres) -> odd-r offset (col, row). Mirrors murks::world_to_hex."""
    px = x - HEX_WIDTH * 0.5
    py = y - HEX_SIZE
    q = (SQRT3 / 3.0 * px - py / 3.0) / HEX_SIZE
    r = (2.0 / 3.0 * py) / HEX_SIZE
    cx, cz = q, r
    cy = -cx - cz
    rx, ry, rz = np.rint(cx), np.rint(cy), np.rint(cz)
    dx, dy, dz = np.abs(rx - cx), np.abs(ry - cy), np.abs(rz - cz)
    m1 = (dx > dy) & (dx > dz)
    m2 = ~m1 & (dy > dz)
    m3 = ~m1 & ~m2
    rx = np.where(m1, -ry - rz, rx)
    rz = np.where(m3, -rx - ry, rz)
    row = rz.astype(np.int64)
    col = rx.astype(np.int64) + (row - (row & 1)) // 2
    return col, row


def hex_center(col, row):
    return HEX_WIDTH * (col + 0.5 * (row & 1)) + HEX_WIDTH * 0.5, ROW_SPACING * row + HEX_SIZE


def cube(col, row):
    x = col - (row - (row & 1)) // 2
    return x, -x - row, row


def hex_distance(a, b):
    ax, ay, az = cube(*a)
    bx, by, bz = cube(*b)
    return max(abs(ax - bx), abs(ay - by), abs(az - bz))


def direction_to(a, b):
    table = ODD_NB if a[1] & 1 else EVEN_NB
    for d, (dc, dr) in enumerate(table):
        if (a[0] + dc, a[1] + dr) == b:
            return d
    return -1


def link(features, a, b, net, extra=0):
    """Joins neighbouring hexes a and b (col, row) on a network, like Map::link."""
    flag, shift = net
    d = direction_to(a, b)
    if d < 0:
        return False
    features[a[1], a[0]] |= flag | extra
    features[b[1], b[0]] |= flag | extra
    if d < 3:
        features[a[1], a[0]] |= 1 << (shift + d)
    else:
        features[b[1], b[0]] |= 1 << (shift + d - 3)
    return True


def hex_line(a, b):
    n = hex_distance(a, b)
    ax, ay, az = cube(*a)
    bx, by, bz = cube(*b)
    out = []
    for i in range(1, n):
        t = i / n
        fx, fy, fz = ax + (bx - ax) * t + 1e-6, ay + (by - ay) * t + 2e-6, az + (bz - az) * t - 3e-6
        rx, ry, rz = round(fx), round(fy), round(fz)
        ddx, ddy, ddz = abs(rx - fx), abs(ry - fy), abs(rz - fz)
        if ddx > ddy and ddx > ddz:
            rx = -ry - rz
        elif ddy > ddz:
            ry = -rx - rz
        else:
            rz = -rx - ry
        out.append((rx + (rz - (rz & 1)) // 2, rz))
    return out


def url_exists(url):
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status == 200
    except urllib.error.HTTPError as e:
        if e.code in (403, 404):  # S3 answers 403 for missing keys in some buckets
            return False
        raise


def fetch(url, path):
    if os.path.exists(path):
        return path
    tmp = path + ".part"
    with urllib.request.urlopen(url, timeout=300) as r, open(tmp, "wb") as f:
        f.write(r.read())
    os.replace(tmp, path)
    return path


# --- Grid -----------------------------------------------------------------------------

class Grid:
    def __init__(self, bbox):
        lon0, lat0, lon1, lat1 = bbox
        self.to3035 = Transformer.from_crs("EPSG:4326", "EPSG:3035", always_xy=True)
        self.to4326 = Transformer.from_crs("EPSG:3035", "EPSG:4326", always_xy=True)
        lons = np.concatenate([np.linspace(lon0, lon1, 50), np.full(50, lon1), np.linspace(lon1, lon0, 50), np.full(50, lon0)])
        lats = np.concatenate([np.full(50, lat0), np.linspace(lat0, lat1, 50), np.full(50, lat1), np.linspace(lat1, lat0, 50)])
        e, n = self.to3035.transform(lons, lats)
        self.e0 = math.floor(e.min() / 1000.0) * 1000.0
        self.n0 = math.ceil(n.max() / 1000.0) * 1000.0
        width_m = e.max() - self.e0
        height_m = self.n0 - n.min()
        self.chunks_x = math.ceil(width_m / HEX_WIDTH / CHUNK)
        self.chunks_y = math.ceil(height_m / ROW_SPACING / CHUNK)
        self.cols = self.chunks_x * CHUNK
        self.rows = self.chunks_y * CHUNK
        # Raster covering the whole hex area plus a margin.
        self.px_w = int(math.ceil((self.cols + 1) * HEX_WIDTH / PIXEL))
        self.px_h = int(math.ceil((self.rows * ROW_SPACING + 2 * HEX_SIZE) / PIXEL))
        self.transform = Affine(PIXEL, 0, self.e0, 0, -PIXEL, self.n0)
        # Geographic bounds of the raster (for picking source tiles).
        ce = [self.e0, self.e0 + self.px_w * PIXEL]
        cn = [self.n0 - self.px_h * PIXEL, self.n0]
        es = np.concatenate([np.linspace(ce[0], ce[1], 60)] * 2 + [np.full(60, ce[0]), np.full(60, ce[1])])
        ns = np.concatenate([np.full(60, cn[0]), np.full(60, cn[1]), np.linspace(cn[0], cn[1], 60), np.linspace(cn[0], cn[1], 60)])
        glon, glat = self.to4326.transform(es, ns)
        self.geo_bounds = (glon.min(), glat.min(), glon.max(), glat.max())

    def window_for(self, lon0, lat0, lon1, lat1, pad=4):
        lons = np.concatenate([np.linspace(lon0, lon1, 20)] * 2 + [np.full(20, lon0), np.full(20, lon1)])
        lats = np.concatenate([np.full(20, lat0), np.full(20, lat1), np.linspace(lat0, lat1, 20), np.linspace(lat0, lat1, 20)])
        e, n = self.to3035.transform(lons, lats)
        c0 = max(0, int((e.min() - self.e0) / PIXEL) - pad)
        c1 = min(self.px_w, int((e.max() - self.e0) / PIXEL) + pad)
        r0 = max(0, int((self.n0 - n.max()) / PIXEL) - pad)
        r1 = min(self.px_h, int((self.n0 - n.min()) / PIXEL) + pad)
        if c1 <= c0 or r1 <= r0:
            return None
        return r0, r1, c0, c1


# --- Rasters ----------------------------------------------------------------------------

def tile_list(bounds, step):
    lon0, lat0, lon1, lat1 = bounds
    tiles = []
    for lat in range(int(math.floor(lat0 / step) * step), int(math.ceil(lat1)), step):
        for lon in range(int(math.floor(lon0 / step) * step), int(math.ceil(lon1)), step):
            tiles.append((lat, lon))
    return tiles


def fmt_tile(url, lat, lon):
    return url.format(ns="N" if lat >= 0 else "S", lat=abs(lat), ew="E" if lon >= 0 else "W", lon=abs(lon))


def warp_tiles(grid, tiles, url, step, decimate, resampling, dtype, nodata_out, cache, tag):
    """Reads each source tile (optionally from a COG overview) and warps it into the grid raster."""
    out = np.full((grid.px_h, grid.px_w), nodata_out, dtype=dtype)

    def load(tile):
        lat, lon = tile
        cpath = os.path.join(cache, f"{tag}_{lat}_{lon}_{decimate}.npz")
        if os.path.exists(cpath):
            z = np.load(cpath)
            if z["data"].size == 0:
                return tile, None, None
            return tile, z["data"], Affine(*z["transform"])
        url_t = fmt_tile(url, lat, lon)
        data = tr = None
        for attempt in range(5):
            try:
                with rasterio.open("/vsicurl/" + url_t) as ds:
                    h, w = ds.height // decimate, ds.width // decimate
                    rs = Resampling.mode if (decimate > 1 and tag == "wc") else Resampling.average
                    data = ds.read(1, out_shape=(h, w), resampling=rs)
                    tr = ds.transform * Affine.scale(ds.width / w, ds.height / h)
                break
            except rasterio.errors.RasterioIOError:
                if not url_exists(url_t):  # no tile here (open sea): remember that
                    np.savez(cpath, data=np.zeros(0), transform=np.zeros(6))
                    return tile, None, None
                time.sleep(1.5 * (attempt + 1))  # transient network error: retry
        if data is None:
            raise RuntimeError("could not read " + url_t)
        np.savez_compressed(cpath, data=data, transform=np.array(tr[:6]))
        return tile, data, tr

    done = 0
    with cf.ThreadPoolExecutor(max_workers=4) as ex:
        for tile, data, tr in ex.map(load, tiles):
            done += 1
            if data is None:
                continue
            lat, lon = tile
            win = grid.window_for(lon, lat, lon + step, lat + step)
            if win is None:
                continue
            r0, r1, c0, c1 = win
            dst = np.full((r1 - r0, c1 - c0), nodata_out, dtype=dtype)
            dst_tr = grid.transform * Affine.translation(c0, r0)
            src_nodata = 0 if tag == "wc" else -32767
            reproject(source=data, destination=dst, src_transform=tr, src_crs="EPSG:4326", dst_transform=dst_tr,
                      dst_crs="EPSG:3035", resampling=resampling, src_nodata=src_nodata, dst_nodata=nodata_out)
            view = out[r0:r1, c0:c1]
            mask = dst != nodata_out if not np.isnan(nodata_out) else ~np.isnan(dst)
            view[mask] = dst[mask]
            if done % 20 == 0:
                log(f"  {tag}: {done}/{len(tiles)} tiles")
    return out


# --- Aggregation -------------------------------------------------------------------------

def aggregate(grid, elev, lc):
    n_hex = grid.cols * grid.rows
    esum = np.zeros(n_hex, dtype=np.float64)
    ecnt = np.zeros(n_hex, dtype=np.int32)
    counts = np.zeros((n_hex, K), dtype=np.uint16)
    xs = (np.arange(grid.px_w) + 0.5) * PIXEL
    strip = 256
    for i0 in range(0, grid.px_h, strip):
        i1 = min(grid.px_h, i0 + strip)
        ys = (np.arange(i0, i1) + 0.5) * PIXEL
        X, Y = np.meshgrid(xs, ys)
        col, row = world_to_hex(X, Y)
        ok = (col >= 0) & (col < grid.cols) & (row >= 0) & (row < grid.rows)
        hid = (row * grid.cols + col)[ok]
        if hid.size == 0:
            continue
        lo, hi = hid.min(), hid.max() + 1
        e = elev[i0:i1][ok]
        good = ~np.isnan(e)
        esum[lo:hi] += np.bincount(hid[good] - lo, weights=e[good], minlength=hi - lo)
        ecnt[lo:hi] += np.bincount(hid[good] - lo, minlength=hi - lo).astype(np.int32)
        k = WC_LUT[lc[i0:i1][ok]].astype(np.int64)
        c = np.bincount((hid - lo) * K + k, minlength=(hi - lo) * K).reshape(hi - lo, K)
        counts[lo:hi] += c.astype(np.uint16)
    return esum, ecnt, counts


def classify(grid, esum, ecnt, counts):
    total = counts.sum(axis=1).astype(np.float32)
    total[total == 0] = 1
    frac = counts / total[:, None]
    water = frac[:, 0] + frac[:, 8]
    tree, crop, built, wet = frac[:, 1], frac[:, 4], frac[:, 5], frac[:, 9] + frac[:, 10]
    has_elev = ecnt > 0
    elev = np.where(has_elev, esum / np.maximum(ecnt, 1), -20.0)

    terrain = np.full(len(total), T_OPEN, dtype=np.uint8)
    cover = np.zeros(len(total), dtype=np.uint8)
    terrain[crop >= 0.40] = T_FIELDS
    m = (wet >= 0.30)
    terrain[m] = T_WETLAND
    cover[m] = 2
    m = (tree >= 0.45)
    terrain[m] = T_FOREST
    cover[m] = (14 + 10 * tree[m]).astype(np.uint8)
    m = (built >= 0.30)
    terrain[m] = T_URBAN
    cover[m] = np.where(built[m] >= 0.6, 20, 12)
    # Scattered woods and hedgerows still block sight a little.
    m = (terrain != T_FOREST) & (terrain != T_URBAN) & (tree >= 0.2)
    cover[m] = 6
    is_water = water >= 0.5
    terrain[is_water] = T_WATER
    cover[is_water] = 0
    sea = is_water & (~has_elev | (elev <= 0.5))
    elev = np.where(sea, -20.0, elev)
    return terrain, cover, np.clip(np.round(elev), -500, 9000).astype(np.int16)


# --- Vector features ------------------------------------------------------------------------

def burn_lines(grid, geoms, features, net, extra=0, step=120.0):
    """Traces each line through the hexes it crosses and links consecutive hexes on `net`."""
    links = 0
    for g in geoms:
        if g is None or g.is_empty:
            continue
        parts = g.geoms if hasattr(g, "geoms") else [g]
        for part in parts:
            coords = np.asarray(part.coords)
            if len(coords) < 2:
                continue
            e, n = grid.to3035.transform(coords[:, 0], coords[:, 1])
            x = np.asarray(e) - grid.e0
            y = grid.n0 - np.asarray(n)
            seg = np.hypot(np.diff(x), np.diff(y))
            pts_x, pts_y = [x[:1]], [y[:1]]
            for i in range(len(seg)):
                k = max(1, int(math.ceil(seg[i] / step)))
                t = np.arange(1, k + 1) / k
                pts_x.append(x[i] + (x[i + 1] - x[i]) * t)
                pts_y.append(y[i] + (y[i + 1] - y[i]) * t)
            px, py = np.concatenate(pts_x), np.concatenate(pts_y)
            col, row = world_to_hex(px, py)
            keep = np.ones(len(col), dtype=bool)
            keep[1:] = (col[1:] != col[:-1]) | (row[1:] != row[:-1])
            # Hex cells along the line, gaps filled, then detours removed: if the cells
            # before and after a cell already touch, that cell only makes a zigzag.
            cells = []
            for c, r in zip(col[keep].tolist(), row[keep].tolist()):
                if cells and hex_distance(cells[-1], (c, r)) > 1:
                    cells.extend(hex_line(cells[-1], (c, r)))
                cells.append((c, r))
            path = []
            for cell in cells:
                if path and path[-1] == cell:
                    continue
                while len(path) >= 2 and hex_distance(path[-2], cell) <= 1:
                    path.pop()
                if path and path[-1] == cell:
                    continue
                path.append(cell)
            for a, b in zip(path, path[1:]):
                if 0 <= a[0] < grid.cols and 0 <= a[1] < grid.rows and 0 <= b[0] < grid.cols and 0 <= b[1] < grid.rows:
                    links += link(features, a, b, net, extra)
    return links


def read_ne(cache, kind, name, bbox):
    path = fetch(NE_URL.format(kind=kind, name=name), os.path.join(cache, name + ".zip"))
    meta, _, geom, fields = pyogrio.raw.read(f"/vsizip/{path}", bbox=bbox)
    names = list(meta["fields"])
    return shapely.from_wkb(geom), {n: fields[i] for i, n in enumerate(names)}


# --- Settlements --------------------------------------------------------------------------

def read_places(cache, grid, min_pop):
    path = fetch(GEONAMES_URL, os.path.join(cache, "cities1000.zip"))
    lon0, lat0, lon1, lat1 = grid.geo_bounds
    out = []
    with zipfile.ZipFile(path) as z, z.open("cities1000.txt") as f:
        for line in io.TextIOWrapper(f, "utf-8"):
            p = line.rstrip("\n").split("\t")
            if p[6] != "P" or p[7] in SKIP_FEATURE_CODES:
                continue
            pop = int(p[14] or 0)
            if pop < min_pop:
                continue
            lat, lon = float(p[4]), float(p[5])
            if not (lat0 <= lat <= lat1 and lon0 <= lon <= lon1):
                continue
            out.append((pop, NAMES_1989.get(p[1], p[1]), lon, lat))
    out.sort(reverse=True)
    return out


def place_settlements(grid, places, terrain, cover):
    taken = np.zeros((grid.rows, grid.cols), dtype=bool)
    settle = np.full((grid.rows, grid.cols), NO_SETTLEMENT, dtype=np.uint16)
    result = []
    crowded = np.zeros((grid.rows, grid.cols), dtype=bool)  # within ~3 hexes of a placed settlement
    for pop, name, lon, lat in places:
        e, n = grid.to3035.transform(lon, lat)
        c, r = world_to_hex(np.array([e - grid.e0]), np.array([grid.n0 - n]))
        c, r = int(c[0]), int(r[0])
        if not (4 <= c < grid.cols - 4 and 3 <= r < grid.rows - 3):
            continue
        if terrain[r, c] == T_WATER:  # coastal coordinate: nearest land hex
            best = None
            for dr in range(-2, 3):
                for dc in range(-3, 4):
                    cc, rr = c + dc, r + dr
                    if terrain[rr, cc] != T_WATER and (best is None or hex_distance((c, r), (cc, rr)) < best[0]):
                        best = (hex_distance((c, r), (cc, rr)), cc, rr)
            if best is None:
                continue
            c, r = best[1], best[2]
        if taken[r, c]:
            continue
        if pop < 10000 and crowded[r, c]:
            continue
        radius = 6 if pop >= 1000000 else 5 if pop >= 400000 else 3 if pop >= 150000 else 2 if pop >= 50000 else 1 if pop >= 10000 else 0
        idx = len(result)
        if idx >= 0xFFFE:
            break
        for dr in range(-radius, radius + 1):
            for dc in range(-radius - 1, radius + 2):
                cc, rr = c + dc, r + dr
                if not (0 <= cc < grid.cols and 0 <= rr < grid.rows):
                    continue
                d = hex_distance((c, r), (cc, rr))
                if d > radius or settle[rr, cc] != NO_SETTLEMENT or terrain[rr, cc] == T_WATER:
                    continue
                if d == 0 or terrain[rr, cc] == T_URBAN or d < radius // 2:
                    terrain[rr, cc] = T_URBAN
                    cover[rr, cc] = max(cover[rr, cc], 20 if (radius >= 2 and d <= radius // 2) else 10)
                    settle[rr, cc] = idx
                    taken[rr, cc] = True
        result.append((name, c, r, pop, radius))
        crowded[max(0, r - 3):r + 4, max(0, c - 3):c + 4] = True
    return settle, result


# --- Output ------------------------------------------------------------------------------

def write_map(path, grid, name, elev, terrain, cover, features, settle, settlements):
    rec = np.zeros((grid.rows, grid.cols), dtype=np.dtype([("elev", "<i2"), ("terrain", "u1"), ("cover", "u1"),
                                                          ("features", "<u2"), ("settlement", "<u2")]))
    rec["elev"], rec["terrain"], rec["cover"], rec["features"], rec["settlement"] = elev, terrain, cover, features, settle
    chunked = rec.reshape(grid.chunks_y, CHUNK, grid.chunks_x, CHUNK).transpose(0, 2, 1, 3).reshape(-1)
    nb = name.encode("utf-8")
    with open(path, "wb") as f:
        f.write(b"MURKSMAP")
        f.write(struct.pack("<Iii", 3, grid.chunks_x, grid.chunks_y))
        f.write(struct.pack("<idd", 3035, grid.e0, grid.n0))
        f.write(struct.pack("<H", len(nb)) + nb)
        f.write(chunked.tobytes())
        f.write(struct.pack("<I", len(settlements)))
        for sname, c, r, pop, radius in settlements:
            b = sname.encode("utf-8")[:255]
            f.write(struct.pack("<iiiiH", c, r, pop, radius, len(b)) + b)


def compress(src, dst):
    raw = open(src, "rb").read()
    z = zlib.compress(raw, 9)
    with open(dst, "wb") as f:
        f.write(b"MURKSZ01")
        f.write(struct.pack("<Q", len(raw)))
        f.write(z)
    return len(raw), len(z)


def preview(path, terrain, elev, features):
    from PIL import Image
    pal = np.array([[98, 142, 180], [212, 204, 168], [222, 213, 160], [116, 146, 94], [150, 172, 150], [150, 128, 120]], np.uint8)
    img = pal[terrain].astype(np.float32)
    hi = np.clip((elev.astype(np.float32) - 250) / 1000, 0, 0.55)[..., None]
    img = img + (np.array([176, 152, 118]) - img) * hi * (terrain != T_WATER)[..., None]
    for flag, col in ((F_RIVER, (70, 120, 176)), (F_ROAD, (150, 104, 70)), (F_MAJOR_ROAD, (196, 74, 46)), (F_RAIL, (38, 38, 40))):  # noqa
        img[(features & flag) != 0] = col
    Image.fromarray(img.astype(np.uint8)).resize((terrain.shape[1], int(terrain.shape[0] * 0.866))).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="Poland and East Germany 1989")
    ap.add_argument("--bbox", type=float, nargs=4, default=[10.0, 49.0, 24.2, 55.0], metavar=("LON0", "LAT0", "LON1", "LAT1"))
    ap.add_argument("--out", default="game/data/poland_1989.murkz")
    ap.add_argument("--cache", default=os.path.expanduser("~/murks_cache"))
    ap.add_argument("--min-pop", type=int, default=2000)
    ap.add_argument("--finish-map", default="build/finish_map", help="path to the finish_map executable")
    ap.add_argument("--preview", default="", help="optional PNG preview path")
    args = ap.parse_args()
    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    os.environ.setdefault("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR")
    os.environ.setdefault("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif")
    t_all = time.time()

    grid = Grid(args.bbox)
    log(f"grid: {grid.cols} x {grid.rows} hexes ({grid.chunks_x} x {grid.chunks_y} chunks), raster {grid.px_w} x {grid.px_h} @ {PIXEL:.0f} m")
    log(f"source bounds lon {grid.geo_bounds[0]:.2f}..{grid.geo_bounds[2]:.2f}, lat {grid.geo_bounds[1]:.2f}..{grid.geo_bounds[3]:.2f}")

    dem_tiles = tile_list(grid.geo_bounds, 1)
    log(f"elevation: {len(dem_tiles)} Copernicus GLO-90 tiles")
    elev_r = warp_tiles(grid, dem_tiles, DEM_URL, 1, 1, Resampling.bilinear, np.float32, np.nan, args.cache, "dem")
    wc_tiles = tile_list(grid.geo_bounds, 3)
    log(f"land cover: {len(wc_tiles)} WorldCover tiles (overview 1/8)")
    lc_r = warp_tiles(grid, wc_tiles, WC_URL, 3, 8, Resampling.nearest, np.uint8, 0, args.cache, "wc")

    log("aggregating pixels into hexes")
    esum, ecnt, counts = aggregate(grid, elev_r, lc_r)
    del elev_r, lc_r
    terrain, cover, elev = classify(grid, esum, ecnt, counts)
    terrain = terrain.reshape(grid.rows, grid.cols)
    cover = cover.reshape(grid.rows, grid.cols)
    elev = elev.reshape(grid.rows, grid.cols)
    features = np.zeros((grid.rows, grid.cols), dtype=np.uint16)

    gb = grid.geo_bounds
    log("rivers, roads and railways (Natural Earth)")
    g, f = read_ne(args.cache, "physical", "ne_10m_rivers_lake_centerlines", gb)
    burn_lines(grid, g, features, RIVER, F_MAJOR_RIVER)
    g, f = read_ne(args.cache, "physical", "ne_10m_rivers_europe", gb)
    burn_lines(grid, g, features, RIVER)
    g, f = read_ne(args.cache, "cultural", "ne_10m_roads", gb)
    kind = np.array([str(t) for t in f["type"]])
    exp = np.array([int(v or 0) for v in f["expressway"]])
    burn_lines(grid, g[(kind == "Major Highway") | (exp == 1)], features, ROAD, F_MAJOR_ROAD)
    burn_lines(grid, g[(kind == "Secondary Highway") | (kind == "Road")], features, ROAD)
    g, f = read_ne(args.cache, "cultural", "ne_10m_railroads", gb)
    burn_lines(grid, g[np.array([str(c) == "Railroad" for c in f["featurecla"]])], features, RAIL)
    water = terrain == T_WATER
    # Rivers end where lakes and sea begin (edges from the bank into the water stay, so they meet the shore).
    features[water] &= ~np.uint16(F_RIVER | F_MAJOR_RIVER | (0x7 << 12))

    log("settlements (GeoNames)")
    places = read_places(args.cache, grid, args.min_pop)
    settle, settlements = place_settlements(grid, places, terrain, cover)
    log(f"  {len(settlements)} settlements placed from {len(places)} candidates")

    raw_path = os.path.join(args.cache, "baked_raw.murksmap")
    fin_path = os.path.join(args.cache, "baked_finished.murksmap")
    write_map(raw_path, grid, args.name, elev, terrain, cover, features, settle, settlements)
    if args.preview:
        preview(args.preview, terrain, elev, features)
    log("finish_map: local roads between settlements")
    subprocess.run([args.finish_map, raw_path, fin_path], check=True)
    n_raw, n_z = compress(fin_path, args.out)
    log(f"wrote {args.out}: {n_raw / 1e6:.1f} MB raw, {n_z / 1e6:.1f} MB compressed; total {time.time() - t_all:.0f} s")


if __name__ == "__main__":
    sys.exit(main())
