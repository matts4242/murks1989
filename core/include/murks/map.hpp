// Murks 1989 — the one map.
//
// Hexes are stored chunk by chunk (64 x 64 hexes per chunk, 8 bytes per hex),
// so a chunk is one contiguous 32 KB block. That layout is what later lets the
// full Atlantic-to-Urals map be paged from disk chunk by chunk.
#pragma once

#include "hex.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace murks {

enum Terrain : uint8_t {
    T_WATER = 0,
    T_OPEN,
    T_FIELDS,
    T_FOREST,
    T_WETLAND,
    T_URBAN,
    T_COUNT
};

enum Feature : uint16_t {
    F_ROAD = 1 << 0,
    F_MAJOR_ROAD = 1 << 1,
    F_RAIL = 1 << 2,
    F_RIVER = 1 << 3,
    F_BRIDGE = 1 << 4,
    F_MAJOR_RIVER = 1 << 5,
    // Bits 6-14: which hex edges each network crosses. A hex stores its E, NE
    // and NW edges; the other three belong to the neighbour on that side.
};

// Linear networks with per-edge connections.
enum Network : int { NET_ROAD = 0, NET_RAIL = 1, NET_RIVER = 2 };
constexpr int kNetShift[3] = {6, 9, 12};
constexpr uint16_t kNetFlag[3] = {F_ROAD, F_RAIL, F_RIVER};

constexpr uint16_t NO_SETTLEMENT = 0xFFFF;

#pragma pack(push, 1)
struct Hex {
    int16_t elev = 0;        // metres above sea level
    uint8_t terrain = T_OPEN;
    uint8_t cover = 0;       // height of cover (trees, buildings) in metres
    uint16_t features = 0;   // Feature bits
    uint16_t settlement = NO_SETTLEMENT;
};
#pragma pack(pop)
static_assert(sizeof(Hex) == 8, "Hex must stay 8 bytes");

struct Settlement {
    std::string name;
    HexCoord pos;
    int population = 0;
    int radius = 0;   // urban footprint in hexes
    int owner = -1;   // faction id, -1 = independent
};

const char *terrain_name(uint8_t t);

class Map {
public:
    void init(int chunks_x, int chunks_y);

    int cols() const { return chunks_x_ * CHUNK; }
    int rows() const { return chunks_y_ * CHUNK; }
    int chunks_x() const { return chunks_x_; }
    int chunks_y() const { return chunks_y_; }
    int hex_count() const { return (int)hexes_.size(); }

    bool in_bounds(int c, int r) const { return c >= 0 && r >= 0 && c < cols() && r < rows(); }
    bool in_bounds(HexCoord h) const { return in_bounds(h.col, h.row); }

    // Chunk-major linear index.
    int index(int c, int r) const {
        return ((((r >> CHUNK_SHIFT) * chunks_x_) + (c >> CHUNK_SHIFT)) << 12) + ((r & (CHUNK - 1)) << CHUNK_SHIFT) +
               (c & (CHUNK - 1));
    }
    int index(HexCoord h) const { return index(h.col, h.row); }
    HexCoord coord_of(int idx) const {
        int chunk = idx >> 12, local = idx & (CHUNK_AREA - 1);
        int cy = chunk / chunks_x_, cx = chunk % chunks_x_;
        return {cx * CHUNK + (local & (CHUNK - 1)), cy * CHUNK + (local >> CHUNK_SHIFT)};
    }

    Hex &at(int c, int r) { return hexes_[index(c, r)]; }
    const Hex &at(int c, int r) const { return hexes_[index(c, r)]; }
    Hex &at(HexCoord h) { return hexes_[index(h)]; }
    const Hex &at(HexCoord h) const { return hexes_[index(h)]; }
    Hex &at_index(int i) { return hexes_[i]; }
    const Hex &at_index(int i) const { return hexes_[i]; }

    // Width and height of the whole map in metres.
    double width_m() const { return HEX_WIDTH * (cols() + 0.5); }
    double height_m() const { return ROW_SPACING * (rows() - 1) + 2 * HEX_SIZE; }

    size_t memory_bytes() const { return hexes_.size() * sizeof(Hex); }

    int nearest_settlement(HexCoord h, int max_dist = 1 << 30) const;

    // Network edges. `dir` is a neighbour direction 0-5 (E, NE, NW, W, SW, SE).
    bool linked(HexCoord h, int dir, Network net) const {
        if (dir < 3) return (at(h).features >> (kNetShift[net] + dir)) & 1;
        HexCoord n = neighbor(h, dir);
        return in_bounds(n) && ((at(n).features >> (kNetShift[net] + dir - 3)) & 1);
    }
    // Joins two neighbouring hexes on a network (also sets the presence flag on both).
    void link(HexCoord a, HexCoord b, Network net) {
        int dir = direction_to(a, b);
        if (dir < 0) return;
        at(a).features |= kNetFlag[net];
        at(b).features |= kNetFlag[net];
        if (dir < 3) at(a).features |= (uint16_t)(1u << (kNetShift[net] + dir));
        else at(b).features |= (uint16_t)(1u << (kNetShift[net] + dir - 3));
    }
    static int direction_to(HexCoord a, HexCoord b) {
        for (int d = 0; d < 6; ++d)
            if (neighbor(a, d) == b) return d;
        return -1;
    }
    // Older map files had presence flags only: join every neighbouring pair.
    void derive_edges_from_flags();

    // Map files: "MURKSMAP" v2 (georeferenced) or v1. See map.cpp for the layout.
    bool save(const std::string &path) const;
    bool load(const std::string &path);
    bool load_from_memory(const uint8_t *data, size_t size);

    std::vector<Settlement> settlements;

    // Georeference: projected coordinates (metres) of world point (0,0), with
    // y pointing south in the game and north in the projection.
    // epsg 0 = not georeferenced (procedural map).
    std::string name = "Procedural";
    int epsg = 0;
    double geo_origin_e = 0, geo_origin_n = 0;

private:
    int chunks_x_ = 0, chunks_y_ = 0;
    std::vector<Hex> hexes_;
};

} // namespace murks
