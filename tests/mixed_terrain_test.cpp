// Headless checks of mixed ground (hedgerows and copses; terrain look, card 2): its going sits
// between fields and forest for every mobility, its cover between open ground and forest, a
// unit sitting still in it is found only within 2 km (3 km by recon) instead of forest's 1 km,
// and the planning map's going and cover keys show it. Exit code 0 = all passed.
// Usage: mixed_terrain_test
#include "murks/planning.hpp"
#include "murks/sim.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

// 128 x 128 flat hexes of open ground; columns 60-69 are `strip` terrain (with mixed ground's
// 6 m cover where it is mixed, forest's 18 m for forest), the rest open. With `one_hex`, only
// hex (65, 64) is: so a sight line to it crosses open ground (the cover in between would
// block it, which is the sight-line rule, not concealment).
static std::vector<uint8_t> map_bytes(int strip, bool one_hex) {
    Map m;
    m.init(2, 2);
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            const bool in = one_hex ? (c == 65 && r == 64) : (c >= 60 && c < 70);
            h.terrain = in ? (uint8_t)strip : (uint8_t)T_OPEN;
            h.cover = h.terrain == T_MIXED ? 6 : (h.terrain == T_FOREST ? 18 : 0);
            h.features = 0;
        }
    m.name = "mixed";
    m.save("mixed_test.murksmap");
    std::ifstream f("mixed_test.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    f.close();
    std::remove("mixed_test.murksmap");
    return bytes;
}

static World world_with(int strip, bool one_hex = false) {
    std::vector<uint8_t> bytes = map_bytes(strip, one_hex);
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(11);
    w.time_min = 10 * 60 - 1; // daylight
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
    w.formations.push_back({"Red", SIDE_HOSTILE, -1, false, 0, {}});
    return w;
}

static int add(World &w, int formation, UnitType t, const char *equip, HexCoord at) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = w.formations[formation].side;
    u.type = t;
    u.name = std::string(formation == 0 ? "Blue " : "Red ") + unit_type_info(t).name;
    u.equipment = equip;
    u.formation = formation;
    u.pos = u.prev_pos = u.dest = at;
    u.elements_max = u.elements = World::element_count(u);
    u.ai_next = 1ll << 40;
    u.roe = ROE_HOLD;
    w.formations[formation].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

// Whether an observer of `type` this many hexes west of a still hostile rifle platoon in the
// strip (column 65) has spotted it after a few minutes.
static bool spotted(int strip, UnitType observer, int hexes) {
    World w = world_with(strip, true);
    add(w, 0, observer, observer == U_RECON ? "BRDM-2" : "3x T-72M", {65 - hexes, 64});
    const int target = add(w, 1, U_INFANTRY, "Rifle Plt", {65, 64});
    for (int i = 0; i < 3; ++i) w.tick();
    return w.seen_by(w.units[target], SIDE_PLAYER);
}

int main() {
    std::printf("The terrain:\n");
    check(std::string(terrain_name(T_MIXED)).find("Mixed") == 0, "it is called Mixed");
    bool between = true;
    for (int mob = 0; mob <= M_FOOT; ++mob)
        between &= terrain_minutes(mob, T_MIXED) >= terrain_minutes(mob, T_FIELDS) &&
                   terrain_minutes(mob, T_MIXED) <= terrain_minutes(mob, T_FOREST);
    check(between, "its going lies between fields and forest for tracks, wheels and feet");
    check(terrain_minutes(M_TRACKED, T_MIXED) == 3.5f && terrain_minutes(M_WHEELED, T_MIXED) == 6.0f &&
              terrain_minutes(M_FOOT, T_MIXED) == 8.0f,
          "3.5 / 6 / 8 minutes a hex");
    check(terrain_cover_factor(T_MIXED, false) == 0.85f && terrain_cover_factor(T_MIXED, true) == 0.75f, "hit chance x0.85 vehicles, x0.75 infantry");
    check(terrain_cover_factor(T_MIXED, false) < terrain_cover_factor(T_OPEN, false) &&
              terrain_cover_factor(T_MIXED, true) > terrain_cover_factor(T_FOREST, true),
          "cover between open ground and forest");
    check(terrain_conceals(T_MIXED) && conceal_spot_hexes(T_MIXED, false) == 4 && conceal_spot_hexes(T_MIXED, true) == 6 &&
              conceal_spot_hexes(T_FOREST, false) == 2,
          "it hides still units beyond 2 km (3 km by recon), forest beyond 1 km");

    std::printf("Spotting a still rifle platoon:\n");
    {
        const bool open3 = spotted(T_OPEN, U_TANK, 6), mixed3 = spotted(T_MIXED, U_TANK, 6), mixed15 = spotted(T_MIXED, U_TANK, 3);
        const bool forest15 = spotted(T_FOREST, U_TANK, 3), recon25 = spotted(T_MIXED, U_RECON, 5);
        std::printf("     at 3 km: open %d, mixed %d; at 1.5 km: mixed %d, forest %d; recon at 2.5 km in mixed %d\n", open3, mixed3,
                    mixed15, forest15, recon25);
        check(open3, "on open ground it is seen at 3 km");
        check(!mixed3, "in mixed ground it is not seen at 3 km");
        check(mixed15, "in mixed ground it is seen at 1.5 km");
        check(!forest15, "in forest it is still hidden at 1.5 km");
        check(recon25, "recon finds it in mixed ground at 2.5 km");
    }

    std::printf("Moving through it:\n");
    {
        World w = world_with(T_MIXED);
        const float mixed = w.step_minutes(M_TRACKED, MV_HASTY, w.map.index({64, 64}), w.map.index({65, 64}), false);
        World f = world_with(T_FIELDS);
        const float fields = f.step_minutes(M_TRACKED, MV_HASTY, f.map.index({64, 64}), f.map.index({65, 64}), false);
        check(mixed > fields && mixed == 3.5f, "a tank's step into mixed ground costs 3.5 min, more than fields");
        RouteEstimate across = w.route_estimate({55, 64}, {75, 64}, M_WHEELED, MV_HASTY);
        RouteEstimate open = world_with(T_OPEN).route_estimate({55, 64}, {75, 64}, M_WHEELED, MV_HASTY);
        std::printf("     wheels across 10 km with 5 km of mixed ground: %.0f min (all open: %.0f)\n", across.minutes, open.minutes);
        check(across.found && across.minutes > open.minutes, "crossing a belt of it takes longer than open ground");
    }

    std::printf("Planning map:\n");
    {
        World w = world_with(T_MIXED);
        const float g = hex_going(w, w.map.index({65, 64}), M_WHEELED);
        check(g > terrain_minutes(M_WHEELED, T_FIELDS) && g < terrain_minutes(M_WHEELED, T_FOREST), "going shows it between fields and forest");
        std::vector<PlanKey> key;
        plan_key(PLAN_COVER, key);
        bool listed = false;
        for (const auto &k : key) listed |= k.label.find("Mixed") != std::string::npos;
        check(listed, "the cover key lists mixed ground");
        plan_key(PLAN_GOING_TRACKED, key);
        bool going = false;
        for (const auto &k : key) going |= k.note.find("mixed ground") != std::string::npos;
        check(going, "the going key names mixed ground in its band");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
