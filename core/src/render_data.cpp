#include "murks/render_data.hpp"

#include <algorithm>
#include <cmath>

namespace murks {

namespace {

struct RGB {
    double r, g, b;
};

// Average colour of each painted ground type, for the far-zoom overview (one pixel per
// hex) and for flat-colour mode. Keep in step with the materials in
// game/shaders/terrain.gdshader: these are what its patterns average to.
RGB base_color(const Hex &h, int, int) {
    switch (h.terrain) {
        case T_WATER: {
            double depth = std::min(1.0, std::max(0.0, -h.elev / 40.0));
            return {102 - 34 * depth, 143 - 34 * depth, 179 - 30 * depth};
        }
        case T_FIELDS: return {139, 137, 88};
        case T_FOREST: return h.cover >= 20 ? RGB{44, 72, 40} : RGB{58, 91, 47};
        case T_WETLAND: return {118, 137, 109};
        case T_URBAN: return h.cover >= 20 ? RGB{135, 129, 109} : RGB{124, 132, 88};
        default: return {124, 149, 77}; // pasture and rough ground
    }
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

// A band along a polyline (world coordinates) with mitred joins, as triangles.
void push_strip(ChunkGeometry &g, const Vec2 *pts, size_t n, double half_w, uint32_t col) {
    if (n < 2) return;
    int32_t base = (int32_t)(g.xy.size() / 2);
    for (size_t i = 0; i < n; ++i) {
        const Vec2 &a = pts[i > 0 ? i - 1 : 0], &b = pts[i + 1 < n ? i + 1 : n - 1];
        double tx = b.x - a.x, ty = b.y - a.y, tl = std::hypot(tx, ty);
        if (tl < 1e-9) tx = 1, ty = 0, tl = 1;
        tx /= tl;
        ty /= tl;
        double w = half_w;
        if (i > 0 && i + 1 < n) { // mitre: keep the band its full width through a bend
            double sx = pts[i].x - pts[i - 1].x, sy = pts[i].y - pts[i - 1].y, sl = std::hypot(sx, sy);
            if (sl > 1e-9) w /= std::max(0.5, (sx * tx + sy * ty) / sl);
        }
        const double nx = -ty * w, ny = tx * w;
        g.xy.push_back((float)(pts[i].x - g.origin_x + nx));
        g.xy.push_back((float)(pts[i].y - g.origin_y + ny));
        g.xy.push_back((float)(pts[i].x - g.origin_x - nx));
        g.xy.push_back((float)(pts[i].y - g.origin_y - ny));
        g.rgba.push_back(col);
        g.rgba.push_back(col);
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        const int32_t a = base + (int32_t)(2 * i);
        const int32_t idx[6] = {a, a + 1, a + 2, a + 1, a + 3, a + 2};
        for (int32_t k : idx) g.indices.push_back(k);
    }
}

// The path a road, railway or river takes through one hex. Links leave each hex through the
// middle of an edge; a hex with two links joins them with a curve (a quadratic Bezier with
// the hex centre as control point), so a line through many hexes bends smoothly instead of
// zig-zagging between hex centres. Junctions and line ends run straight to the centre.
struct HexPath {
    Vec2 ctr;
    int n = 0;          // links
    int dir[6];         // directions, as `neighbor`
    bool major[6];
    Vec2 mid[6];        // edge midpoints
};

bool hex_path(const Map &map, int c, int r, Network net, uint16_t major_flag, HexPath &p) {
    const Hex &h = map.at(c, r);
    if (!(h.features & kNetFlag[net])) return false;
    p.ctr = hex_center({c, r});
    p.n = 0;
    for (int d = 0; d < 6; ++d) {
        if (!map.linked({c, r}, d, net)) continue;
        HexCoord nb = neighbor({c, r}, d);
        if (!map.in_bounds(nb)) continue;
        const Hex &hn = map.at(nb);
        // A river running through a lake or the sea is already drawn as water.
        if (net == NET_RIVER && h.terrain == T_WATER && hn.terrain == T_WATER) continue;
        Vec2 o = hex_center(nb);
        p.dir[p.n] = d;
        p.major[p.n] = major_flag && (h.features & major_flag) && (hn.features & major_flag);
        p.mid[p.n] = {0.5 * (p.ctr.x + o.x), 0.5 * (p.ctr.y + o.y)};
        ++p.n;
    }
    return p.n > 0;
}

const int kCurveSteps = 8;

inline Vec2 bezier(const Vec2 &a, const Vec2 &c, const Vec2 &b, double t) {
    const double u = 1.0 - t;
    return {u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y};
}

// Draws one hex's share of a network at one width (casing or fill).
void draw_hex_path(ChunkGeometry &g, const HexPath &p, double half_w, double half_w_major, uint32_t col,
                   uint32_t col_major) {
    if (p.n == 2) {
        Vec2 pts[kCurveSteps + 1];
        for (int k = 0; k <= kCurveSteps; ++k) pts[k] = bezier(p.mid[0], p.ctr, p.mid[1], (double)k / kCurveSteps);
        if (p.major[0] == p.major[1]) {
            push_strip(g, pts, kCurveSteps + 1, p.major[0] ? half_w_major : half_w, p.major[0] ? col_major : col);
        } else { // a main road meeting a minor one changes width halfway
            const int h = kCurveSteps / 2;
            push_strip(g, pts, h + 1, p.major[0] ? half_w_major : half_w, p.major[0] ? col_major : col);
            push_strip(g, pts + h, kCurveSteps - h + 1, p.major[1] ? half_w_major : half_w, p.major[1] ? col_major : col);
        }
        return;
    }
    bool any_major = false;
    for (int i = 0; i < p.n; ++i) {
        const Vec2 pts[2] = {p.ctr, p.mid[i]};
        push_strip(g, pts, 2, p.major[i] ? half_w_major : half_w, p.major[i] ? col_major : col);
        any_major = any_major || p.major[i];
    }
    // A round hub where lines meet (or end).
    push_blob(g, p.ctr.x - g.origin_x, p.ctr.y - g.origin_y, any_major ? half_w_major : half_w, 12, 0.0,
              any_major ? col_major : col);
}

// Points along a hex's path (for rail sleepers): one polyline per link pair or spoke.
void hex_path_polylines(const HexPath &p, std::vector<std::vector<Vec2>> &out) {
    out.clear();
    if (p.n == 2) {
        std::vector<Vec2> v;
        for (int k = 0; k <= kCurveSteps * 2; ++k) v.push_back(bezier(p.mid[0], p.ctr, p.mid[1], k / (2.0 * kCurveSteps)));
        out.push_back(v);
        return;
    }
    for (int i = 0; i < p.n; ++i) out.push_back({p.ctr, p.mid[i]});
}

// Every network of a chunk, smoothed: river banks and water, bridge decks, road casings
// and surfaces, railways with sleepers, then bridge parapets.
void append_lines(const Map &map, int c0, int r0, ChunkGeometry &g) {
    struct Pass {
        Network net;
        uint16_t major_flag;
        double half_w, half_w_major;
        uint32_t col, col_major;
    };
    const Pass rivers[] = {
        {NET_RIVER, F_MAJOR_RIVER, 46, 84, rgba8(70, 110, 148), rgba8(64, 104, 144)},  // banks
        {NET_RIVER, F_MAJOR_RIVER, 33, 70, rgba8(110, 156, 196), rgba8(98, 146, 190)}, // water
    };
    const Pass roads[] = {
        {NET_ROAD, F_MAJOR_ROAD, 36, 58, rgba8(104, 82, 58), rgba8(112, 46, 30)},     // casing
        {NET_ROAD, F_MAJOR_ROAD, 23, 41, rgba8(242, 230, 192), rgba8(230, 112, 58)},  // surface
    };
    HexPath p;
    const int c1 = std::min(c0 + CHUNK, map.cols()), r1 = std::min(r0 + CHUNK, map.rows());
    auto run = [&](const Pass &L) {
        for (int r = r0; r < r1; ++r)
            for (int c = c0; c < c1; ++c)
                if (hex_path(map, c, r, L.net, L.major_flag, p)) draw_hex_path(g, p, L.half_w, L.half_w_major, L.col, L.col_major);
    };
    for (const Pass &L : rivers) run(L);

    // Bridges: where a road or railway crosses a river, a pale deck a little wider than the
    // road, then dark parapets on top of everything (drawn last, below).
    struct Bridge {
        Vec2 at, along;
        double half_w;
    };
    std::vector<Bridge> bridges;
    for (int r = r0; r < r1; ++r) {
        for (int c = c0; c < c1; ++c) {
            const Hex &h = map.at(c, r);
            if (!(h.features & F_BRIDGE)) continue;
            Network net = (h.features & F_ROAD) ? NET_ROAD : NET_RAIL;
            if (!hex_path(map, c, r, net, net == NET_ROAD ? F_MAJOR_ROAD : 0, p)) continue;
            Bridge b;
            b.at = p.ctr;
            Vec2 a = p.ctr, z = p.mid[0];
            if (p.n == 2) {
                b.at = bezier(p.mid[0], p.ctr, p.mid[1], 0.5);
                a = p.mid[0];
                z = p.mid[1];
            }
            double dx = z.x - a.x, dy = z.y - a.y, dl = std::hypot(dx, dy);
            b.along = dl > 1e-9 ? Vec2{dx / dl, dy / dl} : Vec2{1, 0};
            bool major = false;
            for (int i = 0; i < p.n; ++i) major = major || p.major[i];
            b.half_w = (net == NET_RAIL ? 30.0 : major ? 58.0 : 36.0) + 16.0;
            bridges.push_back(b);
        }
    }
    const double kBridgeHalfLen = 130.0;
    for (const Bridge &b : bridges)
        push_rect(g, b.at.x - g.origin_x, b.at.y - g.origin_y, kBridgeHalfLen, b.half_w,
                  std::atan2(b.along.y, b.along.x), rgba8(232, 226, 210));

    for (const Pass &L : roads) run(L);

    // Railways: a dark line with sleepers across it every ~60 m.
    std::vector<std::vector<Vec2>> lines;
    for (int r = r0; r < r1; ++r) {
        for (int c = c0; c < c1; ++c) {
            if (!hex_path(map, c, r, NET_RAIL, 0, p)) continue;
            draw_hex_path(g, p, 10, 10, rgba8(44, 42, 46), rgba8(44, 42, 46));
            hex_path_polylines(p, lines);
            for (const auto &v : lines) {
                double len = 0;
                for (size_t i = 1; i < v.size(); ++i) len += std::hypot(v[i].x - v[i - 1].x, v[i].y - v[i - 1].y);
                const int n = std::max(1, (int)std::lround(len / 60.0));
                for (int k = 0; k < n; ++k) {
                    double s = (k + 0.5) * len / n, run_len = 0;
                    for (size_t i = 1; i < v.size(); ++i) {
                        double sl = std::hypot(v[i].x - v[i - 1].x, v[i].y - v[i - 1].y);
                        if (run_len + sl >= s && sl > 1e-9) {
                            double t = (s - run_len) / sl, tx = (v[i].x - v[i - 1].x) / sl, ty = (v[i].y - v[i - 1].y) / sl;
                            double mx = v[i - 1].x + (v[i].x - v[i - 1].x) * t - g.origin_x;
                            double my = v[i - 1].y + (v[i].y - v[i - 1].y) * t - g.origin_y;
                            push_line(g, mx - ty * 28, my + tx * 28, mx + ty * 28, my - tx * 28, 6, rgba8(44, 42, 46));
                            break;
                        }
                        run_len += sl;
                    }
                }
            }
        }
    }

    for (const Bridge &b : bridges) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const double ox = -b.along.y * b.half_w * sgn, oy = b.along.x * b.half_w * sgn;
            const double ax = b.at.x - b.along.x * kBridgeHalfLen + ox - g.origin_x, ay = b.at.y - b.along.y * kBridgeHalfLen + oy - g.origin_y;
            const double zx = b.at.x + b.along.x * kBridgeHalfLen + ox - g.origin_x, zy = b.at.y + b.along.y * kBridgeHalfLen + oy - g.origin_y;
            push_line(g, ax, ay, zx, zy, 8, rgba8(34, 30, 28));
            // Wing walls turned outward at both ends.
            const double wx = -b.along.y * sgn * 30, wy = b.along.x * sgn * 30;
            push_line(g, ax, ay, ax - b.along.x * 24 + wx, ay - b.along.y * 24 + wy, 8, rgba8(34, 30, 28));
            push_line(g, zx, zy, zx + b.along.x * 24 + wx, zy + b.along.y * 24 + wy, 8, rgba8(34, 30, 28));
        }
    }
}

inline double unit(uint32_t h) { return (h & 0xFFFFu) * (1.0 / 65535.0); }

} // namespace

void chunk_origin(int cx, int cy, double &ox, double &oy) {
    ox = cx * CHUNK * HEX_WIDTH;
    oy = cy * CHUNK * ROW_SPACING;
}

// Flat colour of a hex (the far-zoom overview). Hill shading and the highland tint are
// added by the terrain shader, from the height raster, at every zoom.
uint32_t hex_color(const Map &map, int c, int r) {
    RGB col = base_color(map.at(c, r), c, r);
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

    append_lines(map, c0, r0, g);
}

void build_chunk_lines(const Map &map, int cx, int cy, ChunkGeometry &g) {
    g.xy.clear();
    g.rgba.clear();
    g.indices.clear();
    g.grid_xy.clear();
    chunk_origin(cx, cy, g.origin_x, g.origin_y);
    append_lines(map, cx * CHUNK, cy * CHUNK, g);
}

double hex_slope(const Map &map, int c, int r, int &down_dir) {
    down_dir = -1;
    const Hex &h = map.at(c, r);
    if (h.terrain == T_WATER) return 0.0;
    double steepest = 0.0, fall = 0.0;
    for (int d = 0; d < 6; ++d) {
        HexCoord n = neighbor({c, r}, d);
        if (!map.in_bounds(n)) continue;
        double rise = (double)map.at(n).elev - (double)h.elev;
        steepest = std::max(steepest, std::fabs(rise));
        if (-rise > fall) {
            fall = -rise;
            down_dir = d;
        }
    }
    return steepest / HEX_WIDTH * 100.0;
}

int slope_class(double percent) {
    return percent < 3.0 ? 0 : percent < 7.0 ? 1 : percent < 15.0 ? 2 : percent < 30.0 ? 3 : 4;
}

void build_hex_data_rgba(const Map &map, std::vector<uint8_t> &rgba) {
    const int W = map.cols(), H = map.rows();
    rgba.resize((size_t)W * H * 4);
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            const Hex &h = map.at(c, r);
            uint8_t t = h.terrain & 7;
            if (h.cover >= 20 && (h.terrain == T_FOREST || h.terrain == T_URBAN)) t |= 8;
            const uint16_t e = (uint16_t)((int)h.elev + 32768);
            size_t o = ((size_t)r * W + c) * 4;
            rgba[o + 0] = t;
            rgba[o + 1] = (uint8_t)(e >> 8);
            rgba[o + 2] = (uint8_t)(e & 255);
            rgba[o + 3] = h.cover;
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
