// Headless checks of the Historical Values rules (5 Nov 2026): fire cycles and fire-mission
// times, the night factor, command zones and radio ranges, element defaults, battery
// displacement, shift fire, deployment from road march and from column, and save/load.
// Exit code 0 = all passed. Usage: rules_test
#include "murks/sim.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

// A flat 128 x 128-hex map of open ground with one east-west road along row 64.
static World flat_world() {
    Map m;
    m.init(2, 2);
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    for (int c = 0; c + 1 < m.cols(); ++c) m.link({c, 64}, {c + 1, 64}, NET_ROAD);
    m.name = "rules-flat";
    m.save("rules_flat.murksmap");
    std::ifstream f("rules_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("rules_flat.murksmap");
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(5);
    w.time_min = 10 * 60 - 1; // the first tick (10:00) builds the zones
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
    w.formations[formation].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

static int count(World &w, const char *needle) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) ++n;
    return n;
}

int main() {
    std::printf("Engagement times:\n");
    {
        World w = flat_world();
        int tank = add(w, 0, U_TANK, "3x T-72M", {10, 10});
        int bty = add(w, 0, U_ARTILLERY, "6x 2S1", {12, 10});
        int plt = add(w, 0, U_ARTILLERY, "3x 2S1", {14, 10});
        int def = add(w, 0, U_ARTILLERY, "D-30 howitzers", {16, 10});
        w.tick();
        auto et = w.engagement_time(w.units[tank]);
        check(et.base_acquire == 1.0f && et.base_cycle == 1.0f, "tank: acquire 1 min, fire cycle 1 min");
        check(unit_type_info(U_TANK).rounds_per_cycle == 2, "tank: 2 aimed rounds per tank per cycle");
        et = w.engagement_time(w.units[bty]);
        check(et.base_acquire == 4.0f && et.base_cycle == 4.0f, "battery: fire mission 4 min, cycle 4 min");
        check(w.engagement_time(w.units[plt]).base_acquire == 1.25f, "3-gun platoon: fire mission 1.25 min");
        check(w.units[def].elements_max == 6, "battery without a count has 6 guns");
        check(w.engagement_time(w.units[tank]).f_night == 1.0f, "no night factor by day");
        w.time_min = 23 * 60;
        et = w.engagement_time(w.units[tank]);
        check(std::fabs(et.f_night - 1.2f) < 1e-6f && std::fabs(et.factor - 1.2f) < 1e-6f, "night: x1.2");
    }

    std::printf("Command zones and radio:\n");
    {
        World w = flat_world();
        int hq = add(w, 0, U_HQ, "UAZ-469", {10, 10});
        int tank = add(w, 0, U_TANK, "3x T-55A", {12, 10});
        int rifle = add(w, 0, U_INFANTRY, "33 men", {14, 10});
        int leader = add(w, 0, U_MECH, "3x BMP-1", {20, 20});
        check(w.command_zone(w.units[hq]) == 8, "HQ zone 4 km");
        check(w.command_zone(w.units[leader]) == 2, "team leader zone 1 km");
        check(World::radio_range(w.units[tank]) == 40 && World::radio_range(w.units[rifle]) == 12,
              "radio: vehicles 20 km, on foot 6 km");
        w.units[rifle].pos = {10 + 15, 10}; // 7.5 km: inside vehicle range, beyond the manpack's
        w.units[tank].pos = {10 + 15, 11};
        const int d_rifle = w.command_delay(w.units[rifle]), d_tank = w.command_delay(w.units[tank]);
        std::printf("     delays at 7.5 km: rifle %d min, tank %d min\n", d_rifle, d_tank);
        check(d_rifle > d_tank + 5, "rifle platoon 7.5 km out needs a courier, the tank platoon does not");
    }

    std::printf("Battery displacement and shift fire:\n");
    {
        World w = flat_world();
        int bty = add(w, 0, U_ARTILLERY, "6x 2S1", {28, 40}); // 8 km: guns only fire inside the hot zone (10 km)
        int obs = add(w, 0, U_RECON, "2x BRDM-2", {40, 40});
        int t1 = add(w, 1, U_INFANTRY, "33 men", {44, 40});
        int t2 = add(w, 1, U_INFANTRY, "33 men", {45, 40});
        w.units[t1].brk = w.units[t2].brk = BC_NEVER;
        w.units[obs].roe = ROE_HOLD;
        int64_t first = -1, displaced_at = -1;
        HexCoord fired_from = w.units[bty].pos;
        for (int m = 0; m < 60 && displaced_at < 0; ++m) {
            w.tick();
            if (first < 0 && w.units[bty].last_fired == w.time_min) first = w.time_min;
            if (count(w, "displaces to a new firing position") > 0) displaced_at = w.time_min;
        }
        std::printf("     first round %lld min after the start, displaced after %lld min of fire\n",
                    (long long)(first - 599), (long long)(displaced_at - first));
        check(first > 0 && first - 599 >= 4 && first - 599 <= 6, "fire mission on an unplanned target takes ~4 min");
        check(displaced_at > 0 && displaced_at - first >= 15 && displaced_at - first <= 21,
              "battery displaces after 15-20 min of fire");
        while (w.units[bty].moving()) w.tick();
        const int moved = hex_distance(w.units[bty].pos, fired_from);
        std::printf("     moved %d hexes\n", moved);
        check(moved >= 2 && moved <= 3, "new position 1-1.5 km away");
        check(hex_distance(w.units[bty].pos, w.units[t1].pos) >= hex_distance(fired_from, w.units[t1].pos),
              "moves away from the enemy");
        // Shift fire: kill the present target outright; the next one is in the adjacent hex.
        int64_t fired = -1;
        for (int m = 0; m < 30 && fired < 0; ++m) {
            w.tick();
            if (w.units[bty].last_fired == w.time_min) fired = w.time_min;
        }
        int tgt = w.units[bty].last_target, other = tgt == t1 ? t2 : t1;
        w.units[tgt].alive = false;
        w.units[tgt].elements = 0;
        int64_t shifted = -1;
        for (int m = 0; m < 10 && shifted < 0; ++m) {
            w.tick();
            if (w.units[bty].last_fired == w.time_min && w.units[bty].last_target == other) shifted = w.time_min;
        }
        const float f = w.engagement_time(w.units[bty]).factor;
        std::printf("     next round on the adjacent target %lld min after the last (cycle 4 + shift 2, x%.2f)\n",
                    (long long)(shifted - fired), f);
        check(shifted > 0 && shifted - fired <= (int)std::ceil(6 * f) + 1 && shifted - fired < 8 * f,
              "shift fire to an adjacent target takes 2 min, not a new 4-min mission");
    }

    std::printf("Deployment from road march and from column:\n");
    {
        World w = flat_world();
        int tank = add(w, 0, U_TANK, "3x T-55A", {10, 64});
        int enemy = add(w, 1, U_INFANTRY, "33 men", {60, 64});
        w.units[enemy].roe = ROE_HOLD;
        w.tick();
        w.order_move(tank, {100, 64}, MV_ROAD);
        int deployed_at = -1;
        for (int m = 0; m < 300 && deployed_at < 0; ++m) {
            w.tick();
            if (count(w, "leaves road march") > 0) deployed_at = hex_distance(w.units[tank].pos, w.units[enemy].pos);
        }
        std::printf("     road march deployed %d hexes (%.1f km) from the enemy\n", deployed_at, deployed_at * 0.5);
        check(deployed_at > 3 && deployed_at <= World::DEPLOY_COLUMN_HEXES && w.units[tank].mode == MV_HASTY,
              "road march deploys once the enemy is known within 5 km (seen at the tanks' 4 km sight)");

        World v = flat_world();
        int l = add(v, 0, U_TANK, "3x T-55A", {10, 64});
        int m1 = add(v, 0, U_TANK, "3x T-55A", {8, 64});
        int e = add(v, 1, U_INFANTRY, "33 men", {40, 64});
        v.units[e].roe = ROE_HOLD;
        v.units[l].roe = v.units[m1].roe = ROE_HOLD;
        int g = v.create_group({l, m1}, l);
        v.set_group_shape(g, FS_COLUMN, 1);
        v.tick();
        v.units[e].seen_until[SIDE_PLAYER] = -1;
        v.order_group_move(g, {39, 64}, MV_HASTY);
        int line_at = -1;
        for (int m = 0; m < 400 && line_at < 0; ++m) {
            v.tick();
            if (count(v, "deploys from column into line") > 0) line_at = hex_distance(v.units[l].pos, v.units[e].pos);
        }
        std::printf("     column deployed into line %d hexes from the enemy\n", line_at);
        check(line_at >= 0 && line_at <= World::DEPLOY_LINE_HEXES && v.groups[g].shape == FS_LINE,
              "a team in column deploys into line at 1.5 km");
    }

    std::printf("Save/load:\n");
    {
        World w = flat_world();
        add(w, 0, U_ARTILLERY, "6x 2S1", {28, 40});
        add(w, 0, U_RECON, "2x BRDM-2", {40, 40});
        add(w, 1, U_INFANTRY, "33 men", {44, 40});
        for (int m = 0; m < 12; ++m) w.tick();
        std::vector<uint8_t> s = w.save_state();
        for (int m = 0; m < 30; ++m) w.tick();
        std::vector<uint8_t> a = w.save_state();
        std::string err;
        check(w.load_state(s.data(), s.size(), &err), "load ok");
        for (int m = 0; m < 30; ++m) w.tick();
        check(a == w.save_state(), "replay after load is identical (battery mid fire assault)");
        std::vector<uint8_t> old = s;
        old[7] = '3';
        check(!w.load_state(old.data(), old.size(), &err) && err.find("older") != std::string::npos,
              "v3 saves are refused");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
