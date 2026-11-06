// Attrition harness: two platoons face each other on flat open ground in daylight, both halted
// and fighting it out, and the kill rate is measured as elements knocked out per firing
// element-minute (a "firing element" is a vehicle still in the fight on a side that has a
// target in range). The Historical Values doc puts tank against tank at 1.5 km in the open at
// about 0.09-0.13 kills per tank-minute (Soviet planning norms degraded to real combat).
//
// Usage: attrition_test [runs] [range_hexes] [minutes] [shooter_type] [target_type]
//   types: tank | mech | rifle | recon      (default: tank against tank, 3 hexes = 1.5 km)
//   attrition_test calibrate   prints the tank table and exits 0 if 1.5 km is inside the band.
#include "murks/sim.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace murks;

static std::vector<uint8_t> flat_map_bytes() {
    static std::vector<uint8_t> bytes;
    if (!bytes.empty()) return bytes;
    Map m;
    m.init(1, 1); // 64 x 64 hexes of flat open ground
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    m.name = "attrition-flat";
    const char *path = "attrition_flat.murksmap";
    m.save(path);
    std::ifstream f(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(f), {});
    std::remove(path);
    return bytes;
}

static UnitType parse_type(const char *s) {
    if (!std::strcmp(s, "mech")) return U_MECH;
    if (!std::strcmp(s, "rifle")) return U_INFANTRY;
    if (!std::strcmp(s, "recon")) return U_RECON;
    return U_TANK;
}

static const char *equip_for(UnitType t) {
    switch (t) {
        case U_MECH: return "3x BMP-2";
        case U_INFANTRY: return "33 men";
        case U_RECON: return "2x BRDM-2";
        default: return "3x T-72M";
    }
}

static Unit make_unit(World &w, int formation, int8_t side, UnitType t, HexCoord at) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = side;
    u.type = t;
    u.name = side == SIDE_PLAYER ? "Blue" : "Red";
    u.equipment = equip_for(t);
    u.formation = formation;
    u.pos = u.prev_pos = u.dest = at;
    u.brk = BC_NEVER; // fight it out: the measurement is the exchange, not who breaks first
    u.elements_max = u.elements = World::element_count(u);
    w.formations[formation].members.push_back(u.id);
    return u;
}

struct Result {
    double kills = 0, firing_minutes = 0;
};

// One duel. Blue (`shooter`) and Red (`target`) each fire at the other; both sides' kills count.
static void duel(Result &res, uint32_t seed, int range, int minutes, UnitType shooter, UnitType target) {
    const std::vector<uint8_t> &bytes = flat_map_bytes();
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.seed = seed;
    w.time_min = 10 * 60 - 1; // daylight; the first tick (10:00) builds the zones
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
    w.formations.push_back({"Red", SIDE_HOSTILE, -1, false, 0, {}});
    w.units.push_back(make_unit(w, 0, SIDE_PLAYER, shooter, {20, 32}));
    w.units.push_back(make_unit(w, 1, SIDE_HOSTILE, target, {20 + range, 32}));
    w.reseed_rng(seed);
    Unit &a = w.units[0], &b = w.units[1];
    for (int m = 0; m < minutes; ++m) {
        const int ka = a.elements, kb = b.elements;
        const bool both = a.alive && b.alive;
        w.tick();
        if (!both) break;
        // Each side's in-the-fight elements at the start of the minute count as firing.
        res.firing_minutes += ka + kb;
        res.kills += (ka - a.elements) + (kb - b.elements);
        if (!a.alive || !b.alive) break;
    }
}

static double rate(int runs, int range, int minutes, UnitType s, UnitType t) {
    Result r;
    for (int i = 0; i < runs; ++i) duel(r, 1000 + i * 7919, range, minutes, s, t);
    return r.firing_minutes > 0 ? r.kills / r.firing_minutes : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "calibrate")) {
        const int runs = 400, minutes = 30;
        std::printf("tank vs tank, flat open, daylight, both halted (%d duels each, up to %d min)\n", runs, minutes);
        std::printf("  range    kills per tank-minute\n");
        double at15 = 0;
        for (int r = 1; r <= 5; ++r) {
            double k = rate(runs, r, minutes, U_TANK, U_TANK);
            if (r == 3) at15 = k;
            std::printf("  %.1f km   %.3f%s\n", r * 0.5, k, r == 3 ? "   <- target band 0.09-0.13" : "");
        }
        std::printf("other pairings at 1.5 km (kills per firing element-minute, both sides):\n");
        const char *names[] = {"tank", "mech", "rifle", "recon"};
        const UnitType types[] = {U_TANK, U_MECH, U_INFANTRY, U_RECON};
        for (int i = 1; i < 4; ++i)
            std::printf("  tank vs %-5s %.3f   %s vs %-5s %.3f\n", names[i], rate(runs, 3, minutes, U_TANK, types[i]),
                        names[i], names[i], rate(runs, std::min(3, unit_type_info(types[i]).weapon_range), minutes,
                                                 types[i], types[i]));
        const bool ok = at15 >= 0.09 && at15 <= 0.13;
        std::printf("%s\n", ok ? "inside the band" : "OUTSIDE the band");
        return ok ? 0 : 1;
    }
    const int runs = argc > 1 ? std::atoi(argv[1]) : 400;
    const int range = argc > 2 ? std::atoi(argv[2]) : 3;
    const int minutes = argc > 3 ? std::atoi(argv[3]) : 30;
    const UnitType s = parse_type(argc > 4 ? argv[4] : "tank"), t = parse_type(argc > 5 ? argv[5] : "tank");
    std::printf("%.3f kills per firing element-minute at %.1f km (%d duels)\n", rate(runs, range, minutes, s, t),
                range * 0.5, runs);
    return 0;
}
