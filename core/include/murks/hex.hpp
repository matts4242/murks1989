// Murks 1989 — hex grid maths.
//
// The map is a flat grid of pointy-top hexes, 500 m across the flats (the
// Flashpoint Campaigns scale), stored in "odd-r" offset coordinates
// (odd rows are shifted half a hex to the right). Distance and line maths
// convert to cube coordinates internally.
//
// World units are metres. Hex (0,0) has its centre at (HEX_WIDTH/2, HEX_SIZE).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace murks {

constexpr int CHUNK = 64;                       // hexes per chunk side
constexpr int CHUNK_SHIFT = 6;
constexpr int CHUNK_AREA = CHUNK * CHUNK;       // 4096
constexpr double SQRT3 = 1.7320508075688772;
constexpr double HEX_WIDTH = 500.0;             // metres, flat to flat
constexpr double HEX_SIZE = HEX_WIDTH / SQRT3;  // centre to corner, ~288.7 m
constexpr double ROW_SPACING = HEX_SIZE * 1.5;  // ~433.0 m between row centres

struct HexCoord {
    int col = 0;
    int row = 0;
    bool operator==(const HexCoord &o) const { return col == o.col && row == o.row; }
    bool operator!=(const HexCoord &o) const { return !(*this == o); }
};

struct Cube {
    int x, y, z;
};

inline Cube to_cube(HexCoord h) {
    int x = h.col - (h.row - (h.row & 1)) / 2;
    int z = h.row;
    return {x, -x - z, z};
}

inline HexCoord from_cube(Cube c) {
    int col = c.x + (c.z - (c.z & 1)) / 2;
    return {col, c.z};
}

inline int hex_distance(HexCoord a, HexCoord b) {
    Cube ca = to_cube(a), cb = to_cube(b);
    int dx = std::abs(ca.x - cb.x), dy = std::abs(ca.y - cb.y), dz = std::abs(ca.z - cb.z);
    int m = dx > dy ? dx : dy;
    return m > dz ? m : dz;
}

// Neighbour offsets for odd-r layout, in direction order E, NE, NW, W, SW, SE.
inline HexCoord neighbor(HexCoord h, int dir) {
    static const int even[6][2] = {{+1, 0}, {0, -1}, {-1, -1}, {-1, 0}, {-1, +1}, {0, +1}};
    static const int odd[6][2] = {{+1, 0}, {+1, -1}, {0, -1}, {-1, 0}, {0, +1}, {+1, +1}};
    const int(*t)[2] = (h.row & 1) ? odd : even;
    return {h.col + t[dir][0], h.row + t[dir][1]};
}

struct Vec2 {
    double x = 0, y = 0;
};

inline Vec2 hex_center(HexCoord h) {
    return {HEX_WIDTH * (h.col + 0.5 * (h.row & 1)) + HEX_WIDTH * 0.5, ROW_SPACING * h.row + HEX_SIZE};
}

// Corner i of a pointy-top hex (0 = upper right, clockwise in screen space).
inline Vec2 hex_corner_offset(int i) {
    double ang = (60.0 * i - 30.0) * 3.14159265358979323846 / 180.0;
    return {HEX_SIZE * std::cos(ang), HEX_SIZE * std::sin(ang)};
}

inline Cube cube_round(double fx, double fy, double fz) {
    int rx = (int)std::lround(fx), ry = (int)std::lround(fy), rz = (int)std::lround(fz);
    double dx = std::fabs(rx - fx), dy = std::fabs(ry - fy), dz = std::fabs(rz - fz);
    if (dx > dy && dx > dz) rx = -ry - rz;
    else if (dy > dz) ry = -rx - rz;
    else rz = -rx - ry;
    return {rx, ry, rz};
}

inline HexCoord world_to_hex(double wx, double wy) {
    double px = wx - HEX_WIDTH * 0.5, py = wy - HEX_SIZE;
    double q = (SQRT3 / 3.0 * px - 1.0 / 3.0 * py) / HEX_SIZE;
    double r = (2.0 / 3.0 * py) / HEX_SIZE;
    return from_cube(cube_round(q, -q - r, r));
}

// Hexes on the straight line from a to b, inclusive of both ends.
inline void hex_line(HexCoord a, HexCoord b, std::vector<HexCoord> &out) {
    out.clear();
    int n = hex_distance(a, b);
    Cube ca = to_cube(a), cb = to_cube(b);
    if (n == 0) {
        out.push_back(a);
        return;
    }
    // Small nudge so lines along hex edges resolve consistently.
    const double ex = 1e-6, ey = 2e-6, ez = -3e-6;
    for (int i = 0; i <= n; ++i) {
        double t = (double)i / n;
        out.push_back(from_cube(cube_round(ca.x + (cb.x - ca.x) * t + ex, ca.y + (cb.y - ca.y) * t + ey,
                                           ca.z + (cb.z - ca.z) * t + ez)));
    }
}

} // namespace murks
