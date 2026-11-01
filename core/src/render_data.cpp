#include "murks/render_data.hpp"

#include "murks/noise.hpp"

#include <algorithm>
#include <cmath>

namespace murks {

namespace {

struct RGB {
    double r, g, b;
};

RGB base_color(const Hex &h, int c, int r) {
    int jitter = (int)(hash2(c, r, 77) % 9) - 4;
    switch (h.terrain) {
        case T_WATER: {
            double depth = std::min(1.0, std::max(0.0, -h.elev / 120.0));
            return {98 - 30 * depth, 142 - 30 * depth, 180 - 20 * depth};
        }
        case T_FIELDS: return {222.0 + jitter, 213.0 + jitter, 160.0 + jitter};
        case T_FOREST: return {116.0 + jitter, 146.0 + jitter, 94.0 + jitter};
        case T_WETLAND: return {150.0, 172.0, 150.0};
        case T_URBAN: return h.cover >= 20 ? RGB{128, 108, 104} : RGB{160, 140, 132};
        default: return {212.0 + jitter, 204.0 + jitter, 168.0 + jitter};
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
    struct Layer {
        Network net;
        uint16_t major_flag;
        double half_w, half_w_major;
        uint32_t col, col_major;
    };
    const Layer layers[] = {
        {NET_RIVER, F_MAJOR_RIVER, 45, 75, rgba8(84, 132, 182), rgba8(70, 120, 176)},
        {NET_ROAD, F_MAJOR_ROAD, 34, 55, rgba8(146, 100, 66), rgba8(196, 74, 46)},
        {NET_RAIL, 0, 20, 20, rgba8(38, 38, 40), rgba8(38, 38, 40)},
    };
    for (const Layer &L : layers) {
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
                    bool major = L.major_flag && (h.features & L.major_flag) && (hn.features & L.major_flag);
                    Vec2 b = hex_center(n);
                    push_quad(g, a.x - g.origin_x, a.y - g.origin_y, b.x - g.origin_x, b.y - g.origin_y,
                              major ? L.half_w_major : L.half_w, major ? L.col_major : L.col);
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
            if (h.features & F_MAJOR_ROAD) blend(rgba8(196, 74, 46), 0.85);
            else if (h.features & F_ROAD) blend(rgba8(150, 104, 70), 0.55);
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
