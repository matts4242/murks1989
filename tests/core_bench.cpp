// Headless check of the Murks core: builds the map, runs the simulation and
// prints timings. Usage: core_bench [hostile_groups] [days]
#include "murks/render_data.hpp"
#include "murks/sim.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace murks;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

int main(int argc, char **argv) {
    int groups = argc > 1 ? std::atoi(argv[1]) : 24;
    int days = argc > 2 ? std::atoi(argv[2]) : 2;
    World w;
    auto t0 = Clock::now();
    GenStats g = w.generate(1989, 24, 22);
    std::printf("map: %d x %d = %d hexes, %.1f MB, generated in %.0f ms\n", w.map.cols(), w.map.rows(),
                w.map.hex_count(), w.map.memory_bytes() / 1e6, ms(t0));
    std::printf("     %d settlements (%d cities), %d road hexes, %d rail hexes, %d river hexes, %d bridges\n",
                g.settlements, g.cities, g.road_hexes, g.rail_hexes, g.river_hexes, g.bridges);

    t0 = Clock::now();
    ChunkGeometry geo;
    for (int i = 0; i < 32; ++i) build_chunk_geometry(w.map, i % 24, (i * 7) % 22, true, geo);
    std::printf("chunk mesh build: %.2f ms per chunk\n", ms(t0) / 32);

    w.setup_scenario(groups);
    std::printf("units: %zu in %zu formations\n", w.units.size(), w.formations.size());

    // Send the recon east and the tanks after them.
    HexCoord home = w.units[0].pos;
    w.order_move(8, {home.col + 60, home.row - 4});
    w.order_move(1, {home.col + 30, home.row});

    t0 = Clock::now();
    int contacts = 0, destroyed = 0, total_events = 0;
    for (int m = 0; m < days * 1440; ++m) {
        w.tick();
        for (auto &e : w.drain_events()) {
            ++total_events;
            if (e.kind == EV_CONTACT) ++contacts;
            if (e.kind == EV_DESTROYED) ++destroyed;
            if (total_events <= 25) std::printf("  [%s] %s\n", w.time_string().c_str(), e.text.c_str());
        }
    }
    double run = ms(t0);
    const SimStats &st = w.stats();
    std::printf("simulated %d days (%d ticks) in %.0f ms: %.1f us/tick avg, %.0f us max\n", days, days * 1440, run,
                st.avg_tick_us, st.max_tick_us);
    std::printf("zones now: hot %d, warm %d, cold %d (alive %d)\n", st.hot, st.warm, st.cold, st.alive);
    std::printf("events %d, contacts %d, destroyed %d; stock: %.0f L diesel, %.1f t ammo, %.0f kg food\n",
                total_events, contacts, destroyed, w.stock.diesel_l, w.stock.ammo_t, w.stock.food_kg);
    std::printf("time: %s\n", w.time_string().c_str());
    return 0;
}
