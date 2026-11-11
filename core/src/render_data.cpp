#include "murks/render_data.hpp"

#include "murks/noise.hpp"

#include <algorithm>
#include <cmath>

namespace murks {

namespace {

struct RGB {
    double r, g, b;
};

// Flashpoint-style palette: natural, saturated enough to tell ground types apart at a
// glance. One terrain type per hex; the shade varies a little from hex to hex so that
// large areas don't look flat.
RGB base_color(const Hex &h, int c, int r) {
    int jitter = (int)(hash2(c, r, 77) % 9) - 4;
    switch (h.terrain) {
        case T_WATER: {
            double depth = std::min(1.0, std::max(0.0, -h.elev / 120.0));
            return {92 - 30 * depth, 138 - 30 * depth, 178 - 22 * depth};
        }
        case T_FIELDS: {
            // Parcels a few hexes across: ripe grain, stubble, green crop, ploughed earth.
            static const RGB tone[4] = {{214, 198, 132}, {198, 186, 118}, {176, 184, 112}, {186, 160, 116}};
            double n = fbm(c * 0.2, r * 0.2, 4201, 2);
            int k = n < -0.16 ? 3 : n < 0.0 ? 1 : n < 0.18 ? 0 : 2;
            return {tone[k].r + jitter, tone[k].g + jitter, tone[k].b + jitter};
        }
        case T_FOREST:
            return h.cover >= 20 ? RGB{74.0 + jitter, 108.0 + jitter, 66.0 + jitter}
                                 : RGB{92.0 + jitter, 126.0 + jitter, 76.0 + jitter};
        case T_WETLAND: return {126.0 + jitter, 156.0 + jitter, 136.0 + jitter};
        case T_URBAN: return h.cover >= 20 ? RGB{124, 112, 106} : RGB{164, 152, 138};
        default: return {172.0 + jitter, 182.0 + jitter, 122.0 + jitter}; // pasture and rough ground
    }
}

double elev_at(const Map &map, HexCoord h, double fallback) {
    if (!map.in_bounds(h)) return fallback;
    const Hex &x = map.at(h);
    return x.terrain == T_WATER ? std::max<double>(0, x.elev) : x.elev;
}

void push_quad(ChunkGeometry &g, double ax, double ay, double bx, double by, double half_w, uint32_t col) {
    double dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
    if (len < 1e-6) return;
    dx /= len;
    dy /= len;
    // Extend each end by half the width so joins at hex centres overlap cleanly.
    ax -= dx * half_w;
    ay -= dy * half_w;
    bx += dx * half_w;
    by += dy * half_w;
    double nx = -dy * half_w, ny = dx * half_w;
    int32_t base = (int32_t)(g.xy.size() / 2);
    const double pts[4][2] = {{ax + nx, ay + ny}, {bx + nx, by + ny}, {bx - nx, by - ny}, {ax - nx, ay - ny}};
    for (auto &p : pts) {
        g.xy.push_back((float)p[0]);
        g.xy.push_back((float)p[1]);
        g.rgba.push_back(col);
    }
    const int32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (int32_t i : idx) g.indices.push_back(base + i);
}


// A filled convex polygon of n points around (x, y): a fan from the first vertex.
void push_blob(ChunkGeometry &g, double x, double y, double rad, int n, double rot, uint32_t col) {
    int32_t base = (int32_t)(g.xy.size() / 2);
    for (int i = 0; i < n; ++i) {
        double a = rot + 6.283185307179586 * i / n;
        g.xy.push_back((float)(x + rad * std::cos(a)));
        g.xy.push_back((float)(y + rad * std::sin(a)));
        g.rgba.push_back(col);
    }
    for (int i = 1; i + 1 < n; ++i) {
        g.indices.push_back(base);
        g.indices.push_back(base + i);
        g.indices.push_back(base + i + 1);
    }
}

// A rectangle of half-size (hw, hh) turned by `rot`.
void push_rect(ChunkGeometry &g, double x, double y, double hw, double hh, double rot, uint32_t col) {
    double cs = std::cos(rot), sn = std::sin(rot);
    int32_t base = (int32_t)(g.xy.size() / 2);
    const double pts[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (auto &p : pts) {
        g.xy.push_back((float)(x + p[0] * cs - p[1] * sn));
        g.xy.push_back((float)(y + p[0] * sn + p[1] * cs));
        g.rgba.push_back(col);
    }
    const int32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (int32_t i : idx) g.indices.push_back(base + i);
}

// A line with square ends (push_quad extends its ends for joins; patterns must not).
void push_line(ChunkGeometry &g, double ax, double ay, double bx, double by, double half_w, uint32_t col) {
    double mx = 0.5 * (ax + bx), my = 0.5 * (ay + by);
    push_rect(g, mx, my, 0.5 * std::hypot(bx - ax, by - ay), half_w, std::atan2(by - ay, bx - ax), col);
}

inline double unit(uint32_t h) { return (h & 0xFFFFu) * (1.0 / 65535.0); }

} // namespace

void chunk_origin(int cx, int cy, double &ox, double &oy) {
    ox = cx * CHUNK * HEX_WIDTH;
    oy = cy * CHUNK * ROW_SPACING;
}

uint32_t hex_color(const Map &map, int c, int r) {
    const Hex &h = map.at(c, r);
    RGB col = base_color(h, c, r);
    if (h.terrain != T_WATER) {
        // Highland tint.
        double hi = smoothstep(250, 1300, h.elev) * 0.55;
        col.r += (176 - col.r) * hi;
        col.g += (152 - col.g) * hi;
        col.b += (118 - col.b) * hi;
        double snow = smoothstep(1500, 2100, h.elev) * 0.7;
        col.r += (236 - col.r) * snow;
        col.g += (236 - col.g) * snow;
        col.b += (232 - col.b) * snow;
        // Hillshade, light from the north-west.
        HexCoord hc{c, r};
        double e = h.elev;
        double eE = elev_at(map, neighbor(hc, 0), e), eW = elev_at(map, neighbor(hc, 3), e);
        double eN = 0.5 * (elev_at(map, neighbor(hc, 1), e) + elev_at(map, neighbor(hc, 2), e));
        double eS = 0.5 * (elev_at(map, neighbor(hc, 4), e) + elev_at(map, neighbor(hc, 5), e));
        double dzdx = (eE - eW) / (2 * HEX_WIDTH), dzdy = (eS - eN) / (2 * ROW_SPACING);
        const double ex = 6.0;
        double nx = -dzdx * ex, ny = -dzdy * ex, nz = 1.0;
        double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        const double lx = -0.577, ly = -0.577, lz = 0.577;
        double shade = (nx * lx + ny * ly + nz * lz) / nl; // 0.577 on flat ground
        double k = std::max(0.62, std::min(1.25, 1.0 + (shade - 0.577) * 1.6));
        col.r *= k;
        col.g *= k;
        col.b *= k;
    }
    return rgba8((int)col.r, (int)col.g, (int)col.b);
}

void build_chunk_geometry(const Map &map, int cx, int cy, bool with_grid, ChunkGeometry &g) {
    g.xy.clear();
    g.rgba.clear();
    g.indices.clear();
    g.grid_xy.clear();
    chunk_origin(cx, cy, g.origin_x, g.origin_y);
    const int c0 = cx * CHUNK, r0 = cy * CHUNK;
    g.xy.reserve(CHUNK_AREA * 12 + 4096);
    g.rgba.reserve(CHUNK_AREA * 6 + 2048);
    g.indices.reserve(CHUNK_AREA * 12 + 4096);

    Vec2 corner[6];
    for (int i = 0; i < 6; ++i) corner[i] = hex_corner_offset(i);

    // Terrain hexes: 6 vertices, 4 triangles each.
    for (int r = r0; r < r0 + CHUNK; ++r) {
        for (int c = c0; c < c0 + CHUNK; ++c) {
            Vec2 ctr = hex_center({c, r});
            double lx = ctr.x - g.origin_x, ly = ctr.y - g.origin_y;
            uint32_t col = hex_color(map, c, r);
            int32_t base = (int32_t)(g.xy.size() / 2);
            for (int i = 0; i < 6; ++i) {
                // Tiny overlap hides hairline seams between hexes.
                g.xy.push_back((float)(lx + corner[i].x * 1.004));
                g.xy.push_back((float)(ly + corner[i].y * 1.004));
                g.rgba.push_back(col);
            }
            const int32_t tri[12] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5};
            for (int32_t t : tri) g.indices.push_back(base + t);
            if (with_grid) {
                // Each hex draws its top-right, right and bottom-right edges.
                const int e[4] = {5, 0, 1, 2};
                for (int k = 0; k < 3; ++k) {
                    g.grid_xy.push_back((float)(lx + corner[e[k]].x));
                    g.grid_xy.push_back((float)(ly + corner[e[k]].y));
                    g.grid_xy.push_back((float)(lx + corner[e[k + 1]].x));
                    g.grid_xy.push_back((float)(ly + corner[e[k + 1]].y));
                }
            }
        }
    }

    // Linear features, drawn as quads between neighbouring hex centres.
    // Directions E, NE, NW cover every neighbouring pair exactly once.
    // Each pass draws one network at one width, so a casing pass followed by a fill pass
    // gives roads and rivers a clean outline, junctions included.
    struct Pass {
        Network net;
        uint16_t major_flag;
        double half_w, half_w_major;
        uint32_t col, col_major;
    };
    const Pass passes[] = {
        {NET_RIVER, F_MAJOR_RIVER, 54, 92, rgba8(58, 100, 150), rgba8(52, 94, 146)},   // banks
        {NET_RIVER, F_MAJOR_RIVER, 40, 76, rgba8(98, 148, 196), rgba8(84, 136, 188)},  // water
        {NET_ROAD, F_MAJOR_ROAD, 40, 64, rgba8(84, 66, 46), rgba8(96, 40, 28)},        // road casing
        {NET_ROAD, F_MAJOR_ROAD, 26, 46, rgba8(236, 222, 176), rgba8(222, 104, 58)},   // road surface
        {NET_RAIL, 0, 18, 18, rgba8(34, 34, 38), rgba8(34, 34, 38)},
    };
    for (const Pass &L : passes) {
        for (int r = r0; r < r0 + CHUNK; ++r) {
            for (int c = c0; c < c0 + CHUNK; ++c) {
                const Hex &h = map.at(c, r);
                if (!(h.features & kNetFlag[L.net])) continue;
                Vec2 a = hex_center({c, r});
                for (int d = 0; d < 3; ++d) {
                    if (!((h.features >> (kNetShift[L.net] + d)) & 1)) continue;
                    HexCoord n = neighbor({c, r}, d);
                    if (!map.in_bounds(n)) continue;
                    const Hex &hn = map.at(n);
                    // A river running through a lake or the sea is already drawn as water hexes.
                    if (L.net == NET_RIVER && h.terrain == T_WATER && hn.terrain == T_WATER) continue;
                    bool major = L.major_flag && (h.features & L.major_flag) && (hn.features & L.major_flag);
                    Vec2 b = hex_center(n);
                    push_quad(g, a.x - g.origin_x, a.y - g.origin_y, b.x - g.origin_x, b.y - g.origin_y,
                              major ? L.half_w_major : L.half_w, major ? L.col_major : L.col);
                }
            }
        }
    }
}

// Close-zoom detail drawn over the terrain colour and under the hex grid: a pattern per
// terrain type inside each hex (tree crowns, building blocks, furrows, reed marks), rail
// sleepers and bridge marks. Everything stays inside its own hex, so hexes remain crisp,
// and everything is placed by a hash of the hex coordinates, so it never changes.
void build_chunk_detail(const Map &map, int cx, int cy, ChunkGeometry &g) {
    g.xy.clear();
    g.rgba.clear();
    g.indices.clear();
    g.grid_xy.clear();
    chunk_origin(cx, cy, g.origin_x, g.origin_y);
    const int c0 = cx * CHUNK, r0 = cy * CHUNK;
    g.xy.reserve(CHUNK_AREA * 110);
    g.rgba.reserve(CHUNK_AREA * 55);
    g.indices.reserve(CHUNK_AREA * 90);
    const double PI = 3.14159265358979323846;

    for (int r = r0; r < r0 + CHUNK; ++r) {
        for (int c = c0; c < c0 + CHUNK; ++c) {
            const Hex &h = map.at(c, r);
            Vec2 ctr = hex_center({c, r});
            const double x0 = ctr.x - g.origin_x, y0 = ctr.y - g.origin_y;
            const bool lined = (h.features & (F_ROAD | F_RAIL | F_RIVER)) != 0;
            switch (h.terrain) {
                case T_FOREST: {
                    // The stand changes over a few kilometres (conifer, broadleaf, mixed, young
                    // plantation), and every hex varies in tree count, size and extras
                    // (clearings, rides, undergrowth). Shadows fall to the south-east.
                    const bool dense = h.cover >= 20;
                    const uint32_t hx = hash2(c, r, 4409);
                    const double stand_n = fbm(c * 0.11, r * 0.11, 733, 2);
                    // 0 conifer, 1 broadleaf, 2 mixed, 3 young plantation in rows
                    int stand = stand_n < -0.14 ? 0 : stand_n < 0.02 ? 2 : stand_n < 0.3 ? 1 : 0;
                    if (!dense && (hash2(c >> 1, r >> 1, 97) % 7) == 0) stand = 3;
                    const uint32_t shadow = rgba8(26, 46, 28, 125);
                    static const int conifer[3][3] = {{40, 78, 56}, {48, 86, 60}, {34, 70, 54}};
                    static const int broad[4][3] = {{72, 116, 58}, {88, 128, 62}, {62, 104, 54}, {104, 138, 70}};
                    // A ride (forest track) through some hexes; its direction is shared nearby.
                    if ((hx & 7) == 0 && !lined) {
                        double ang = (hash2(c >> 2, r >> 2, 613) % 6) * PI / 6.0 + 0.2;
                        double dx = std::cos(ang), dy = std::sin(ang);
                        push_line(g, x0 - dx * 205, y0 - dy * 205, x0 + dx * 205, y0 + dy * 205, 7.0, rgba8(150, 150, 104, 150));
                    }
                    // A clearing in some light-forest hexes: a patch of grass with fewer trees.
                    const bool clearing = !dense && ((hx >> 3) & 15) == 0;
                    if (clearing) push_blob(g, x0 + 20, y0 - 10, 95, 7, unit(hx >> 8) * 3.0, rgba8(150, 170, 104, 200));
                    if (stand == 3) {
                        // Young plantation: small crowns in straight rows.
                        double ang = (hash2(c >> 1, r >> 1, 331) % 6) * PI / 6.0;
                        double dx = std::cos(ang), dy = std::sin(ang);
                        for (int row = -1; row <= 1; ++row) {
                            for (int k = -2; k <= 2; ++k) {
                                double u = k * 78.0 + (row & 1) * 30.0, v = row * 105.0;
                                if (u * u + v * v > 200.0 * 200.0) continue;
                                if (lined && std::abs(v) < 60.0 && std::abs(u) < 60.0) continue;
                                uint32_t hs = hash2(c * 7 + k, r * 13 + row, 911);
                                int t = (int)(hs % 3);
                                push_blob(g, x0 + u * dx - v * dy, y0 + u * dy + v * dx, 24.0 + 8.0 * unit(hs >> 8), 4, 0.785,
                                          rgba8(74 + t * 8, 118 + t * 7, 66 + t * 4));
                            }
                        }
                        break;
                    }
                    int n = (dense ? 5 : 4) + (int)((hx >> 9) % 3) - (clearing ? 2 : 0);
                    for (int i = 0; i < n; ++i) {
                        uint32_t hs = hash2(c * 7 + i, r * 13 + i * 3, 911);
                        double ang = 2 * PI * (i + unit(hs) * 0.8) / n, rad = (i == 0 ? 25.0 * unit(hs >> 20) : 80.0 + 110.0 * unit(hs >> 8));
                        double tx = x0 + std::cos(ang) * rad, ty = y0 + std::sin(ang) * rad;
                        if (lined && rad < 60.0) continue; // keep the road or river through the hex readable
                        if (clearing && rad < 90.0) continue;
                        bool con = stand == 0 || (stand == 2 && ((hs >> 5) & 1));
                        double size = (dense ? 50.0 : 42.0) + 34.0 * unit(hs >> 16) * unit(hs >> 12);
                        if (con) {
                            // Conifer: a dark pointed crown (two squares turned against each other).
                            size *= 0.82;
                            const int *k3 = conifer[(hs >> 4) % 3];
                            uint32_t col = rgba8(k3[0], k3[1], k3[2]);
                            push_blob(g, tx + 15, ty + 17, size, 4, 0.785, shadow);
                            push_blob(g, tx, ty, size, 4, 0.0, col);
                            push_blob(g, tx, ty, size, 4, 0.785, col);
                            if (!dense || (hs & 128))
                                push_blob(g, tx - size * 0.12, ty - size * 0.12, size * 0.34, 4, 0.4, rgba8(k3[0] + 22, k3[1] + 24, k3[2] + 16));
                        } else {
                            // Broadleaf: a round crown, lit on its north-west side.
                            const int *k3 = broad[(hs >> 4) % 4];
                            int d = dense ? -8 : 0;
                            push_blob(g, tx + 16, ty + 18, size, 4, 0.785, shadow);
                            push_blob(g, tx, ty, size, 6, unit(hs >> 3), rgba8(k3[0] + d, k3[1] + d, k3[2] + d));
                            push_blob(g, tx - size * 0.24, ty - size * 0.26, size * 0.5, 4, unit(hs >> 7),
                                      rgba8(k3[0] + d + 16, k3[1] + d + 16, k3[2] + d + 8));
                        }
                    }
                    // Undergrowth: a few bushes between the trees.
                    for (int i = 0; i < 2; ++i) {
                        uint32_t hs = hash2(c * 11 + i, r * 3 + i * 5, 2741);
                        if ((hs & 3) == 0) continue;
                        double ang = 2 * PI * unit(hs >> 4), rad = 60.0 + 140.0 * unit(hs >> 14);
                        if (lined && rad < 80.0) continue;
                        push_blob(g, x0 + std::cos(ang) * rad, y0 + std::sin(ang) * rad, 15.0 + 8.0 * unit(hs >> 22), 4, 0.3,
                                  rgba8(96, 130, 72, 220));
                    }
                    break;
                }
                case T_URBAN: {
                    // Each quarter (2 x 2 hexes) has a character; every hex varies inside it.
                    // Dense: courtyard blocks, slab housing estates, works, old town with a
                    // church and market square. Light: village street, houses with gardens,
                    // farmsteads, allotments. Decorative: the map data has no building types.
                    const bool dense = h.cover >= 20;
                    const uint32_t hq = hash2(c >> 1, r >> 1, 5511), hx = hash2(c, r, 6007);
                    double rot = (unit(hq) - 0.5) * 1.0 + (unit(hx >> 6) - 0.5) * 0.12;
                    const double cs = std::cos(rot), sn = std::sin(rot);
                    static const int roofs[6][3] = {{170, 84, 62}, {150, 70, 56}, {98, 94, 96}, {72, 72, 80}, {190, 180, 164}, {134, 108, 84}};
                    const uint32_t shade = rgba8(34, 28, 28, 120);
                    // A building in the quarter's own frame, with its shadow.
                    auto bld = [&](double u, double v, double hw, double hh, double turn, uint32_t col) {
                        double bx = x0 + u * cs - v * sn, by = y0 + u * sn + v * cs;
                        push_rect(g, bx + 9, by + 10, hw, hh, rot + turn, shade);
                        push_rect(g, bx, by, hw, hh, rot + turn, col);
                    };
                    auto flat = [&](double u, double v, double hw, double hh, uint32_t col) {
                        push_rect(g, x0 + u * cs - v * sn, y0 + u * sn + v * cs, hw, hh, rot, col);
                    };
                    auto tree = [&](double u, double v, double size) {
                        double bx = x0 + u * cs - v * sn, by = y0 + u * sn + v * cs;
                        push_blob(g, bx + 8, by + 9, size, 4, 0.785, rgba8(30, 40, 30, 110));
                        push_blob(g, bx, by, size, 6, 0.4, rgba8(84, 124, 70));
                    };
                    auto roof = [&](uint32_t hs, int from, int count) {
                        const int *k3 = roofs[from + (int)(hs % (uint32_t)count)];
                        int j = (int)((hs >> 9) % 13) - 6;
                        return rgba8(k3[0] + j, k3[1] + j, k3[2] + j);
                    };
                    const int kind = (int)((hq >> 8) % 100);
                    if (dense && kind < 38) {
                        // Courtyard blocks: a ring of buildings around a yard, on a street grid.
                        for (int j = -1; j <= 1; ++j) {
                            for (int i = -1; i <= 1; ++i) {
                                uint32_t hs = hash2(c * 5 + i, r * 5 + j, 3301);
                                double u = i * 138.0, v = j * 128.0;
                                if (u * u + v * v > 200.0 * 200.0) continue;
                                if (lined && i == 0 && j == 0) continue;
                                if ((hs & 15) == 0) { // a small square with trees instead of a block
                                    flat(u, v, 50, 44, rgba8(150, 168, 112));
                                    tree(u - 18, v - 8, 20);
                                    tree(u + 20, v + 12, 17);
                                    continue;
                                }
                                double hw = 50.0 + 8.0 * unit(hs >> 8), hh = 44.0 + 8.0 * unit(hs >> 16);
                                bld(u, v, hw, hh, 0, roof(hs >> 3, 0, 4));
                                if ((hs & 7) != 1) flat(u, v, hw - 24, hh - 22, (hs & 16) ? rgba8(128, 146, 100) : rgba8(144, 134, 124));
                            }
                        }
                    } else if (dense && kind < 58) {
                        // Slab housing estate: long pale blocks in rows, grass and trees between.
                        flat(0, 0, 170, 150, rgba8(152, 160, 124, 150));
                        const bool turned = (hx >> 3) & 1;
                        for (int j = -1; j <= 1; ++j) {
                            uint32_t hs = hash2(c * 5 + j, r * 5, 3307);
                            double off = (unit(hs) - 0.5) * 70.0, len = 78.0 + 30.0 * unit(hs >> 8);
                            if (lined && j == 0) continue;
                            int tone = (int)((hs >> 12) % 3) * 8;
                            if (turned) bld(j * 118.0, off, 15, len, 0, rgba8(204 - tone, 200 - tone, 188 - tone));
                            else bld(off, j * 112.0, len, 15, 0, rgba8(204 - tone, 200 - tone, 188 - tone));
                            double tu = (unit(hs >> 16) - 0.5) * 240.0, tv = j * 112.0 + 56.0;
                            if (turned) tree(tv, tu, 18);
                            else tree(tu, tv, 18);
                        }
                    } else if (dense && kind < 78) {
                        // Works: big sheds with a ridge line, a yard, tanks and a chimney.
                        flat(0, 0, 165, 140, rgba8(132, 126, 120, 170));
                        for (int i = 0; i < 3; ++i) {
                            uint32_t hs = hash2(c * 5 + i, r * 5, 3311);
                            double u = (i - 1) * 118.0 + (unit(hs) - 0.5) * 20.0, v = (unit(hs >> 8) - 0.5) * 120.0;
                            if (lined && i == 1) continue;
                            double hw = 44.0 + 12.0 * unit(hs >> 14), hh = 62.0 + 34.0 * unit(hs >> 20);
                            int j = (int)((hs >> 4) % 3);
                            static const int shed[3][3] = {{120, 128, 138}, {146, 142, 134}, {108, 104, 104}};
                            bld(u, v, hw, hh, 0, rgba8(shed[j][0], shed[j][1], shed[j][2]));
                            flat(u, v, 4, hh, rgba8(shed[j][0] + 34, shed[j][1] + 34, shed[j][2] + 34));
                        }
                        double tu = (unit(hx >> 8) - 0.5) * 220.0, tv = 150.0 * ((hx & 1) ? 1 : -1);
                        for (int k = 0; k < 2; ++k) {
                            double bx = x0 + (tu + k * 46) * cs - tv * sn, by = y0 + (tu + k * 46) * sn + tv * cs;
                            push_blob(g, bx + 7, by + 8, 20, 8, 0, shade);
                            push_blob(g, bx, by, 20, 8, 0, rgba8(196, 196, 190));
                        }
                        double ch_u = -tu * 0.6, ch_v = -tv * 0.9;
                        double bx = x0 + ch_u * cs - ch_v * sn, by = y0 + ch_u * sn + ch_v * cs;
                        push_line(g, bx, by, bx + 46, by + 52, 6, shade); // a tall chimney throws a long shadow
                        push_blob(g, bx, by, 13, 6, 0, rgba8(150, 82, 64));
                        push_blob(g, bx, by, 6, 6, 0, rgba8(40, 34, 34));
                    } else if (dense) {
                        // Old town: small houses packed along crooked streets, a market square
                        // and a church with a tower.
                        const bool church = ((hx >> 5) % 3) == 0 && !lined;
                        for (int j = -2; j <= 2; ++j) {
                            for (int i = -2; i <= 2; ++i) {
                                uint32_t hs = hash2(c * 5 + i, r * 5 + j, 3313);
                                double u = i * 84.0 + (unit(hs >> 4) - 0.5) * 26.0, v = j * 84.0 + (unit(hs >> 12) - 0.5) * 26.0;
                                if (u * u + v * v > 205.0 * 205.0) continue;
                                if (i == 0 && j == 0) continue; // market square, or the road through town
                                if (church && i == 1 && j == 0) continue;
                                if ((hs & 15) == 0) continue;
                                bld(u, v, 24.0 + 12.0 * unit(hs >> 8), 20.0 + 10.0 * unit(hs >> 16), (unit(hs >> 20) - 0.5) * 0.5,
                                    roof(hs >> 3, 0, 3));
                            }
                        }
                        if (!lined) flat(0, 0, 34, 34, rgba8(196, 186, 166));
                        if (church) {
                            bld(92, 0, 40, 17, 0.1, rgba8(92, 86, 90));
                            bld(48, 0, 13, 13, 0.1, rgba8(96, 140, 122)); // copper-green tower roof
                        }
                    } else if (kind < 36) {
                        // Village street: houses on both sides of a lane, garden strips behind.
                        flat(0, 0, 200, 7, rgba8(210, 198, 164));
                        for (int i = -2; i <= 2; ++i) {
                            for (int sd = -1; sd <= 1; sd += 2) {
                                uint32_t hs = hash2(c * 5 + i, r * 5 + sd, 3317);
                                if ((hs & 7) < 2) continue;
                                double u = i * 78.0 + (unit(hs >> 4) - 0.5) * 20.0;
                                if (lined && i == 0) continue;
                                if (u * u + 110.0 * 110.0 < 195.0 * 195.0)
                                    flat(u, sd * 105.0, 22, 44, (hs & 64) ? rgba8(136, 164, 98, 200) : rgba8(176, 168, 110, 200));
                                bld(u, sd * 40.0, 22.0 + 8.0 * unit(hs >> 8), 15.0 + 5.0 * unit(hs >> 14), 0, roof(hs >> 3, 0, 3));
                            }
                        }
                    } else if (kind < 66) {
                        // Houses with gardens on a loose grid.
                        for (int j = -1; j <= 1; ++j) {
                            for (int i = -1; i <= 1; ++i) {
                                uint32_t hs = hash2(c * 5 + i, r * 5 + j, 3301);
                                if ((hs & 7) < 2) continue;
                                if (lined && i == 0 && j == 0) continue;
                                double u = i * 120.0 + (unit(hs >> 20) - 0.5) * 24.0, v = j * 116.0 + (unit(hs >> 24) - 0.5) * 24.0;
                                if (u * u + v * v > 200.0 * 200.0) continue;
                                flat(u, v, 44, 40, rgba8(142, 166, 104, 190));
                                bld(u - 10, v - 6, 22.0 + 8.0 * unit(hs >> 8), 17.0 + 7.0 * unit(hs >> 16), 0, roof(hs >> 3, 0, 5));
                                if (hs & 256) tree(u + 26, v + 20, 14);
                            }
                        }
                    } else if (kind < 86) {
                        // Farmsteads: house and barns around a yard, an orchard beside them.
                        for (int f = -1; f <= 1; f += 2) {
                            uint32_t hs = hash2(c * 5 + f, r * 5, 3323);
                            double u = f * 98.0, v = (unit(hs) - 0.5) * 120.0;
                            if ((hs & 7) == 0) continue;
                            flat(u, v, 46, 42, rgba8(188, 176, 146, 220));
                            bld(u, v - 46, 46, 14, 0, roof(hs >> 3, 0, 2));
                            bld(u - 44, v + 6, 14, 38, 0, rgba8(134, 108, 84));
                            bld(u + 44, v + 6, 14, 38, 0, rgba8(120, 98, 80));
                            for (int k = 0; k < 4; ++k) tree(u - 30.0 + (k % 2) * 40.0, v + 78.0 + (k / 2) * 34.0, 12);
                        }
                    } else {
                        // Allotments and edge of town: small plots, sheds, trees and a few houses.
                        for (int j = -1; j <= 1; ++j) {
                            for (int i = -2; i <= 2; ++i) {
                                uint32_t hs = hash2(c * 5 + i, r * 5 + j, 3329);
                                double u = i * 76.0, v = j * 96.0;
                                if (u * u + v * v > 195.0 * 195.0) continue;
                                if (lined && i == 0 && j == 0) continue;
                                static const int plot[3][3] = {{138, 166, 100}, {170, 172, 108}, {150, 134, 102}};
                                int t = (int)(hs % 3);
                                flat(u, v, 32, 42, rgba8(plot[t][0], plot[t][1], plot[t][2], 210));
                                if ((hs & 24) == 0) bld(u, v - 24, 18, 13, 0, roof(hs >> 5, 0, 5));
                                else if (hs & 32) flat(u + 14, v - 26, 8, 7, rgba8(92, 76, 64));
                                if ((hs & 192) == 0) tree(u - 12, v + 18, 13);
                            }
                        }
                    }
                    break;
                }
                case T_FIELDS: {
                    // Furrows: parallel lines whose direction is shared by a parcel of hexes.
                    uint32_t hp = hash2((int)std::floor(c * 0.34), (int)std::floor(r * 0.34), 8087);
                    double ang = (hp % 6) * PI / 6.0 + 0.12;
                    double dx = std::cos(ang), dy = std::sin(ang);
                    const double R = 205.0;
                    for (int k = -1; k <= 1; ++k) {
                        double off = k * 120.0 + (unit(hash2(c, r, 61)) - 0.5) * 20.0;
                        double half = std::sqrt(std::max(0.0, R * R - off * off));
                        double mx = x0 - dy * off, my = y0 + dx * off;
                        push_line(g, mx - dx * half, my - dy * half, mx + dx * half, my + dy * half, 5.0,
                                  rgba8(96, 84, 44, 70));
                    }
                    break;
                }
                case T_WETLAND: {
                    // Marsh marks: a short stretch of standing water with reeds above it.
                    for (int i = 0; i < 5; ++i) {
                        uint32_t hs = hash2(c * 3 + i, r * 11 + i, 2203);
                        double ang = 2 * PI * (i + unit(hs) * 0.6) / 5.0, rad = (i == 0 ? 0.0 : 110.0 + 70.0 * unit(hs >> 8));
                        double tx = x0 + std::cos(ang) * rad, ty = y0 + std::sin(ang) * rad;
                        if (lined && rad < 60.0) continue;
                        push_rect(g, tx, ty, 46, 6, 0, rgba8(70, 118, 160, 210));
                        for (int k = -1; k <= 1; ++k)
                            push_rect(g, tx + k * 22.0, ty - 22.0 - (k == 0 ? 8.0 : 0.0), 4, 14 + (k == 0 ? 8.0 : 0.0), k * 0.25,
                                      rgba8(58, 96, 62, 220));
                    }
                    break;
                }
                default: {
                    // Pasture and rough ground: a few grass tufts, sparse enough to stay calm.
                    for (int i = 0; i < 3; ++i) {
                        uint32_t hs = hash2(c * 9 + i, r * 5 + i * 7, 1409);
                        if (hs & 1) continue;
                        double ang = 2 * PI * unit(hs >> 4), rad = 60.0 + 130.0 * unit(hs >> 12);
                        double tx = x0 + std::cos(ang) * rad, ty = y0 + std::sin(ang) * rad;
                        for (int k = -1; k <= 1; ++k)
                            push_rect(g, tx + k * 11.0, ty, 2.5, 11, k * 0.4, rgba8(96, 118, 70, 130));
                    }
                    break;
                }
                case T_WATER: break;
            }
        }
    }

    // Rail sleepers: short cross marks along every rail link.
    for (int r = r0; r < r0 + CHUNK; ++r) {
        for (int c = c0; c < c0 + CHUNK; ++c) {
            const Hex &h = map.at(c, r);
            if (!(h.features & F_RAIL)) continue;
            Vec2 a = hex_center({c, r});
            for (int d = 0; d < 3; ++d) {
                if (!((h.features >> (kNetShift[NET_RAIL] + d)) & 1)) continue;
                HexCoord n = neighbor({c, r}, d);
                if (!map.in_bounds(n)) continue;
                Vec2 b = hex_center(n);
                double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
                dx /= len;
                dy /= len;
                for (int k = 0; k < 4; ++k) {
                    double t = (k + 0.5) * len / 4.0;
                    double mx = a.x + dx * t - g.origin_x, my = a.y + dy * t - g.origin_y;
                    push_line(g, mx + dy * 42, my - dx * 42, mx - dy * 42, my + dx * 42, 9, rgba8(34, 34, 38));
                }
            }
        }
    }

    // Bridges: a pale deck with dark parapets along the road (or rail) where it crosses.
    for (int r = r0; r < r0 + CHUNK; ++r) {
        for (int c = c0; c < c0 + CHUNK; ++c) {
            const Hex &h = map.at(c, r);
            if (!(h.features & F_BRIDGE)) continue;
            HexCoord hc{c, r};
            Network net = (h.features & F_ROAD) ? NET_ROAD : NET_RAIL;
            int dir = -1;
            for (int d = 0; d < 6 && dir < 0; ++d)
                if (map.linked(hc, d, net)) dir = d;
            Vec2 ctr = hex_center(hc);
            double x0 = ctr.x - g.origin_x, y0 = ctr.y - g.origin_y;
            double dx = 1.0, dy = 0.0;
            if (dir >= 0) {
                Vec2 nb = hex_center(neighbor(hc, dir));
                double len = std::hypot(nb.x - ctr.x, nb.y - ctr.y);
                dx = (nb.x - ctr.x) / len;
                dy = (nb.y - ctr.y) / len;
            }
            const double L = 150.0, W = (h.features & F_MAJOR_ROAD) ? 62.0 : 46.0;
            if (net == NET_ROAD) push_line(g, x0 - dx * L, y0 - dy * L, x0 + dx * L, y0 + dy * L, W - 12, rgba8(240, 236, 222));
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                double ox = -dy * W * sgn, oy = dx * W * sgn;
                push_line(g, x0 - dx * L + ox, y0 - dy * L + oy, x0 + dx * L + ox, y0 + dy * L + oy, 9, rgba8(30, 28, 26));
                // Wing walls turned outward at each end.
                for (int e = -1; e <= 1; e += 2) {
                    double ex = x0 + dx * L * e + ox, ey = y0 + dy * L * e + oy;
                    push_line(g, ex, ey, ex + (dx * e - dy * sgn) * 34, ey + (dy * e + dx * sgn) * 34, 9, rgba8(30, 28, 26));
                }
            }
        }
    }
}

void build_overview_rgba(const Map &map, std::vector<uint8_t> &rgba) {
    const int W = map.cols(), H = map.rows();
    rgba.resize((size_t)W * H * 4);
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            const Hex &h = map.at(c, r);
            uint32_t col = hex_color(map, c, r);
            auto blend = [&](uint32_t over, double a) {
                double r0 = col & 255, g0 = (col >> 8) & 255, b0 = (col >> 16) & 255;
                double r1 = over & 255, g1 = (over >> 8) & 255, b1 = (over >> 16) & 255;
                col = rgba8((int)(r0 + (r1 - r0) * a), (int)(g0 + (g1 - g0) * a), (int)(b0 + (b1 - b0) * a));
            };
            if (h.features & F_MAJOR_RIVER) blend(rgba8(70, 120, 176), 1.0);
            else if (h.features & F_RIVER) blend(rgba8(84, 132, 182), 0.8);
            if (h.features & F_MAJOR_ROAD) blend(rgba8(214, 92, 52), 0.85);
            else if (h.features & F_ROAD) blend(rgba8(120, 96, 66), 0.5);
            if (h.features & F_RAIL) blend(rgba8(38, 38, 40), 0.6);
            size_t o = ((size_t)r * W + c) * 4;
            rgba[o + 0] = col & 255;
            rgba[o + 1] = (col >> 8) & 255;
            rgba[o + 2] = (col >> 16) & 255;
            rgba[o + 3] = 255;
        }
    }
}

} // namespace murks
