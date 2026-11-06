// Headless checks of mounted infantry (6 Nov 2026): dismounting, squads moving on their own,
// mounting up (also when the vehicles moved), squads lost with knocked-out vehicles, the
// two-squads-a-vehicle limit, the victory count, the hostile AI drill, and save/load.
// Exit code 0 = all passed. Usage: dismount_test
#include "murks/sim.hpp"

#include <cstdio>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

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
    m.name = "dismount-flat";
    m.save("dm_flat.murksmap");
    std::ifstream f("dm_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("dm_flat.murksmap");
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(3);
    w.time_min = 10 * 60 - 1;
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
    if (t == U_MECH) w.add_dismounts(u.id);
    return u.id;
}

static int run_until(World &w, int minutes, const std::function<bool()> &done) {
    for (int m = 1; m <= minutes; ++m) {
        w.tick();
        if (done()) return m;
    }
    return -1;
}

int main() {
    std::printf("Dismount:\n");
    World w = flat_world();
    int hq = add(w, 0, U_HQ, "UAZ-469", {20, 64});
    int c = add(w, 0, U_MECH, "3x BMP-1", {24, 64});
    int d = w.units[c].dismounts;
    add(w, 0, U_TANK, "3x T-55A", {22, 64});
    {
        check(d >= 0 && w.units[d].carrier == c && w.units[d].embarked && !w.units[d].alive,
              "BMP-1 platoon has its squads aboard, off the map");
        check(w.units[d].elements == 3 && w.units[d].squad_men == 8 && w.units[d].type == U_INFANTRY,
              "3 squads of 8 (one per vehicle)");
        check(w.can_dismount(w.units[c]) && !w.can_mount(w.units[c]), "can dismount, nothing to mount");
        check(w.effective_combat_units(0) == 2, "fighting platoons: tanks and mech (the squads aboard don't add one)");
        w.tick();
        w.order_move(c, {40, 64}, MV_HASTY);
        run_until(w, 60, [&] { return w.units[c].moving(); });
        const int delay = w.order_dismount(c);
        std::printf("     dismount order arrives in %d min\n", delay);
        check(delay > 0, "the order goes through the command delay");
        int m = run_until(w, 60, [&] { return w.units[d].alive; });
        check(m > 0 && !w.units[c].moving(), "the vehicles halt and the squads get out");
        check(w.units[d].pos == w.units[c].pos && w.units[d].elements == 3 && w.units[d].strength == 100,
              "squads on foot in the vehicles' hex");
        check(!w.can_dismount(w.units[c]) && w.can_mount(w.units[c]) && w.can_mount(w.units[d]),
              "now either can be ordered to mount up");
        check(w.effective_combat_units(0) == 2, "still two fighting platoons with the squads out");
        check(w.command_delay(w.units[d]) > 0, "squads take orders like any unit");
    }
    std::printf("Squads move on their own, then mount up:\n");
    {
        const HexCoord cpos = w.units[c].pos;
        w.order_move(d, {cpos.col, cpos.row + 4}, MV_HASTY);
        run_until(w, 120, [&] { return !w.units[d].moving() && w.units[d].pos != cpos; });
        std::printf("     squads 2 km off, vehicles stay at col %d\n", cpos.col);
        check(hex_distance(w.units[d].pos, cpos) == 4 && w.units[c].pos == cpos, "squads walk off, vehicles stay");
        // Mount order to the squads; once they're walking back the vehicles are sent 1.5 km on,
        // and the squads follow them.
        w.order_mount(d);
        run_until(w, 60, [&] { return w.units[d].mounting; });
        w.order_move(c, {cpos.col + 3, cpos.row}, MV_HASTY);
        int moved = -1;
        int m = run_until(w, 300, [&] {
            if (moved < 0 && w.units[c].pos != cpos) moved = (int)(w.time_min % 100000);
            return w.units[d].embarked;
        });
        std::printf("     mounted up after %d min, vehicles now at col %d (from %d)\n", m, w.units[c].pos.col, cpos.col);
        check(moved >= 0 && w.units[c].pos.col == cpos.col + 3, "the vehicles drove on 1.5 km");
        check(m > 0 && !w.units[d].alive && w.can_dismount(w.units[c]), "squads followed, walked up and mounted");
        check(w.units[d].mounting == false && w.units[d].mount_at < 0, "drill finished");
    }
    std::printf("Squads lost with knocked-out vehicles:\n");
    {
        World v = flat_world();
        int vc = add(v, 0, U_MECH, "3x BTR-60PB", {30, 64});
        int vd = v.units[vc].dismounts;
        v.units[vc].roe = ROE_HOLD;
        v.units[vc].brk = BC_NEVER;
        int e = add(v, 1, U_TANK, "3x T-72M", {33, 64});
        v.units[e].brk = BC_NEVER;
        int m = run_until(v, 120, [&] { return v.units[vc].elements < 3; });
        std::printf("     after %d min: %d vehicles, %d squads aboard\n", m, v.units[vc].elements, v.units[vd].elements);
        check(m > 0 && v.units[vd].elements == v.units[vc].elements, "one squad lost per knocked-out vehicle");
        // Two squads a vehicle at most.
        World x = flat_world();
        int xc = add(x, 0, U_MECH, "3x BMP-2", {30, 64});
        int xd = x.units[xc].dismounts;
        x.tick();
        x.order_dismount(xc);
        run_until(x, 60, [&] { return x.units[xd].alive; });
        x.units[xc].elements = 1;
        x.units[xc].strength = 33.4f;
        check(!x.can_mount(x.units[xc]) && x.order_mount(xd) < 0, "3 squads won't fit into 1 vehicle");
        x.units[xc].elements = 2;
        check(x.can_mount(x.units[xc]), "3 squads crowd into 2 vehicles");
        x.units[xc].elements = 0;
        x.units[xc].alive = false;
        check(x.effective_combat_units(0) == 1, "vehicles gone: the squads count as the platoon");
    }
    std::printf("Hostile AI drill:\n");
    {
        World a = flat_world();
        int ac = add(a, 1, U_MECH, "3x BMP-2", {30, 64});
        int ad = a.units[ac].dismounts;
        int bait = add(a, 0, U_INFANTRY, "33 men", {33, 64});
        a.units[bait].roe = ROE_HOLD;
        a.units[bait].brk = BC_NEVER;
        int m = run_until(a, 120, [&] { return a.units[ad].alive; });
        std::printf("     hostile squads out after %d min, 1.5 km from a known enemy\n", m);
        check(m > 0 && a.units[ad].pos == a.units[ac].pos, "halted near a known enemy: the squads get out");
        a.units[bait].alive = false; // gone
        int m2 = run_until(a, 240, [&] { return a.units[ad].embarked; });
        std::printf("     mounted up again %d min after the enemy was gone\n", m2);
        check(m2 > 0, "nothing in sight: they mount up again");
    }
    std::printf("Save/load with squads walking back:\n");
    {
        World s = flat_world();
        int sc = add(s, 0, U_MECH, "3x BMP-1", {30, 64});
        int sd = s.units[sc].dismounts;
        s.tick();
        s.order_dismount(sc);
        run_until(s, 60, [&] { return s.units[sd].alive; });
        s.order_move(sd, {30, 70}, MV_HASTY);
        run_until(s, 120, [&] { return !s.units[sd].moving() && s.units[sd].pos != s.units[sc].pos; });
        s.order_mount(sd);
        run_until(s, 60, [&] { return s.units[sd].mounting; });
        std::vector<uint8_t> b = s.save_state();
        for (int k = 0; k < 90; ++k) s.tick();
        std::vector<uint8_t> after = s.save_state();
        std::string err;
        check(s.load_state(b.data(), b.size(), &err) && s.units[sd].mounting, "load ok, still mounting");
        for (int k = 0; k < 90; ++k) s.tick();
        check(after == s.save_state() && s.units[sd].embarked, "replay identical, mounted up");
        std::vector<uint8_t> old = b;
        old[7] = '4';
        check(!s.load_state(old.data(), old.size(), &err), "v4 saves are refused");
    }
    (void)hq;
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
