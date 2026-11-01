// Murks 1989 — engine-neutral geometry for drawing the map.
//
// The core builds plain arrays; the Godot layer only copies them into meshes
// and images. That keeps the heavy loops in C++ and the boundary crossings few.
#pragma once

#include "map.hpp"

#include <cstdint>
#include <vector>

namespace murks {

struct ChunkGeometry {
    double origin_x = 0, origin_y = 0; // world position of the chunk's local (0,0)
    // Filled triangles: terrain hexes, then rivers, roads and rail on top.
    std::vector<float> xy;         // local x,y pairs
    std::vector<uint32_t> rgba;    // one colour per vertex, 0xAABBGGRR
    std::vector<int32_t> indices;
    // Hex outlines as line pairs (local x,y).
    std::vector<float> grid_xy;
};

// Packs r,g,b,a into 0xAABBGGRR.
inline uint32_t rgba8(int r, int g, int b, int a = 255) {
    auto cl = [](int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    return cl(r) | (cl(g) << 8) | (cl(b) << 16) | (cl(a) << 24);
}

void chunk_origin(int cx, int cy, double &ox, double &oy);
uint32_t hex_color(const Map &map, int c, int r);
void build_chunk_geometry(const Map &map, int cx, int cy, bool with_grid, ChunkGeometry &out);
// One pixel per hex, rows top to bottom, RGBA8.
void build_overview_rgba(const Map &map, std::vector<uint8_t> &rgba);

} // namespace murks
