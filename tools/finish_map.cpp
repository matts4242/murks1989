// Finishes a map baked from real data: adds A*-routed local roads between
// settlements (joining the real road network where it exists), adds rail
// where the source data has little, marks bridges, and prints statistics.
//
// Usage: finish_map <in.murksmap> <out.murksmap> [--rail]
#include "murks/generator.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace murks;

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: finish_map <in.murksmap> <out.murksmap> [--rail]\n");
        return 2;
    }
    bool rail = argc > 3 && std::strcmp(argv[3], "--rail") == 0;
    Map map;
    if (!map.load(argv[1])) {
        std::fprintf(stderr, "could not read %s\n", argv[1]);
        return 1;
    }
    GenStats before, after;
    count_map_stats(map, before);
    auto t0 = std::chrono::steady_clock::now();
    add_transport_network(map, true, rail, &after);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    count_map_stats(map, after);
    if (!map.save(argv[2])) {
        std::fprintf(stderr, "could not write %s\n", argv[2]);
        return 1;
    }
    int n = map.hex_count();
    std::printf("%s: %d x %d hexes (%d), %d settlements (%d of 60k+)\n", map.name.c_str(), map.cols(), map.rows(), n,
                after.settlements, after.cities);
    std::printf("terrain: water %.1f%%  forest %.1f%%  marsh %.1f%%  built-up %.1f%%  farmland %.1f%%  open %.1f%%\n",
                100.0 * after.water / n, 100.0 * after.forest / n, 100.0 * after.wetland / n, 100.0 * after.urban / n,
                100.0 * after.fields / n, 100.0 * after.open / n);
    std::printf("roads %d -> %d hexes, rail %d -> %d, rivers %d, bridges %d  (network built in %.0f ms)\n",
                before.road_hexes, after.road_hexes, before.rail_hexes, after.rail_hexes, after.river_hexes,
                after.bridges, ms);
    return 0;
}
