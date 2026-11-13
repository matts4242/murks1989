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
// Close-zoom detail layer (terrain patterns, rail sleepers, bridges) as coloured triangles
// with alpha, drawn over the chunk mesh. Built only when the player zooms in.
void build_chunk_detail(const Map &map, int cx, int cy, ChunkGeometry &out);
// Slope of a hex from the per-hex heights the simulation itself uses: the steepest grade
// to a neighbouring hex, in percent (100 x rise / 500 m), and the direction (0..5, as
// `neighbor`) in which the ground falls most steeply (-1 on level ground and on water).
double hex_slope(const Map &map, int c, int r, int &down_dir);
// Slope class of a grade in percent: 0 level (< 3), 1 gentle (< 7), 2 moderate (< 15),
// 3 steep (< 30), 4 very steep.
int slope_class(double percent);
// One pixel per hex, rows top to bottom, RGBA8.
void build_overview_rgba(const Map &map, std::vector<uint8_t> &rgba);

} // namespace murks
