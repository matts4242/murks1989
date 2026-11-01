// Murks 1989 — procedural stand-in for the real-data map pipeline.
//
// The proof of concept generates a Poland-and-East-Germany-sized region
// (~770 x 610 km, ~2.2 million 500 m hexes) with coast, lakes, uplands,
// rivers, towns, roads and rail. The real pipeline (Copernicus elevation,
// WorldCover land cover, OSM roads, 1989 corrections) will write the same
// Map structure, so nothing downstream changes when real data replaces this.
#pragma once

#include "map.hpp"

#include <cstdint>

namespace murks {

struct GenParams {
    uint32_t seed = 1989;
    int chunks_x = 24; // 24 x 64 = 1536 hex columns (~768 km)
    int chunks_y = 22; // 22 x 64 = 1408 hex rows (~610 km)
};

struct GenStats {
    double ms_total = 0, ms_terrain = 0, ms_rivers = 0, ms_settlements = 0, ms_roads = 0, ms_rail = 0;
    int settlements = 0, cities = 0;
    int water = 0, forest = 0, wetland = 0, urban = 0, fields = 0, open = 0;
    int road_hexes = 0, rail_hexes = 0, river_hexes = 0, bridges = 0;
};

GenStats generate_map(Map &map, const GenParams &params);

// Connects settlements with A*-routed local roads (joining existing roads where
// it can) and, optionally, rail between towns of 25,000+. Used both by the
// procedural generator and to finish maps baked from real data.
void add_transport_network(Map &map, bool roads, bool rail, GenStats *stats = nullptr);
void count_map_stats(const Map &map, GenStats &stats);

} // namespace murks
