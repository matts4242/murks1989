// Headless checks of gun ranges by equipment and of the fire support net (6 Nov 2026):
// whole-word gun matching, ranges in hexes, guns firing beyond the 10 km hot zone, direct and
// relayed calls for fire, and units cut off from the net. Exit code 0 = all passed.
#include "murks/sim.hpp"

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

static World flat_world() {
    Map m;
    m.init(3, 2); // 192 x 128 hexes of flat open ground
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    m.name = "fire-support-flat";
    m.save("fs_flat.murksmap");
    std::ifstream f("fs_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("fs_flat.murksmap");
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(11);
    w.time_min = 10 * 60 - 1;
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
    w.formations.push_back({"Red", SIDE_HOSTILE, -1, false, 0, {}});
    return w;
}

static int add(World &w, int formation, UnitType t, const char *equip, int col, int row = 64) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = w.formations[formation].side;
    u.type = t;
    u.name = std::string(formation == 0 ? "Blue " : "Red ") + unit_type_info(t).name;
    u.equipment = equip;
    u.formation = formation;
    u.pos = u.prev_pos = u.dest = {col, row};
    u.elements_max = u.elements = World::element_count(u);
    u.roe = t == U_ARTILLERY ? ROE_FREE : ROE_HOLD; // only the guns fire
    u.brk = BC_NEVER;
    w.formations[formation].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

static Unit gun(const char *equip) {
    Unit u;
    u.type = U_ARTILLERY;
    u.equipment = equip;
    return u;
}

// Runs up to `minutes` and returns the minute of the battery's first round (-1 none).
static int first_round(World &w, int bty, int minutes) {
    for (int m = 0; m < minutes; ++m) {
        w.tick();
        if (w.units[bty].last_fired == w.time_min) return m;
    }
    return -1;
}

int main() {
    std::printf("Gun ranges by equipment:\n");
    {
        check(World::weapon_range(gun("4x 2S1 Gvozdika")) == 30, "2S1: 15.3 km -> 30 hexes");
        check(World::weapon_range(gun("D-30 howitzers")) == 30, "D-30: 15.4 km -> 30 hexes");
        check(World::weapon_range(gun("6x 2S3")) == 37, "2S3: 18.5 km -> 37 hexes");
        check(World::weapon_range(gun("6x 2S19")) == 49, "2S19 is not read as 2S1: 24.7 km");
        check(World::weapon_range(gun("2S12 Sani")) == 14, "2S12 is not read as 2S1: 7.1 km");
        check(World::weapon_range(gun("6x M109A2")) == 36 && World::weapon_range(gun("M109")) == 29,
              "M109A2 18.1 km, original M109 14.6 km");
        check(World::weapon_range(gun("18x BM-21 Grad")) == 40, "BM-21: 20.4 km");
        check(World::weapon_range(gun("4x 2S7 Pion")) == 75, "2S7: 37.5 km");
        check(World::weapon_range(gun("field guns")) == unit_type_info(U_ARTILLERY).weapon_range,
              "unknown guns keep the type's range");
        Unit tank;
        tank.type = U_TANK;
        tank.equipment = "2S1"; // nonsense, but direct-fire types never use the gun table
        check(World::weapon_range(tank) == unit_type_info(U_TANK).weapon_range, "direct fire keeps the type's range");
        const GunInfo *g = gun_info("4x 2S1 Gvozdika");
        check(g && std::string(g->label) == "122 mm SP howitzer 2S1", "label for the unit panel");
    }

    std::printf("Guns fire beyond the 10 km hot zone:\n");
    {
        World w = flat_world();
        int bty = add(w, 0, U_ARTILLERY, "6x 2S3", 10);
        add(w, 0, U_RECON, "2x BRDM-2", 40);
        int tgt = add(w, 1, U_INFANTRY, "33 men", 44); // 17 km from the guns, 2 km from the recon
        int m = first_round(w, bty, 30);
        std::printf("     2S3 at 17 km: first round after %d min (zone %d)\n", m, (int)w.units[bty].zone);
        check(m >= 0 && w.units[bty].last_target == tgt, "2S3 fires at 17 km (18.5 km range)");

        World v = flat_world();
        int b2 = add(v, 0, U_ARTILLERY, "6x 2S1", 10);
        add(v, 0, U_RECON, "2x BRDM-2", 40);
        add(v, 1, U_INFANTRY, "33 men", 44);
        check(first_round(v, b2, 30) < 0, "2S1 does not fire at 17 km (15.3 km range)");
    }

    std::printf("Fire support net:\n");
    {
        // A rifle platoon (manpack radio, 6 km) sees the target; the HQ 3 km behind it relays.
        World w = flat_world();
        int bty = add(w, 0, U_ARTILLERY, "6x 2S3", 20);
        int hq = add(w, 0, U_HQ, "UAZ-469", 44);
        int rifle = add(w, 0, U_INFANTRY, "33 men", 50);
        int tgt = add(w, 1, U_INFANTRY, "33 men", 53);
        w.tick();
        FireNetLink l = w.fire_net_link(w.units[rifle]);
        check(l.role == FN_CALLER && l.status == FN_RELAY && l.via == hq && l.battery == bty,
              "rifle platoon calls fire through the HQ");
        l = w.fire_net_link(w.units[hq]);
        check(l.role == FN_OBSERVER && l.status == FN_DIRECT && l.battery == bty && l.distance == 24,
              "HQ is on the fire net, 12 km from the guns");
        check(w.fire_net_link(w.units[bty]).role == FN_GUNS, "the battery is the guns");
        int obs = -1;
        check(w.fire_call(w.units[bty], w.units[tgt], &obs) == 1 && obs == rifle, "call for fire is relayed");
        int m = first_round(w, bty, 30);
        std::printf("     relayed fire mission: first round after %d min (battery 4 + relay %d)\n", m + 1,
                    FIRE_RELAY_MIN);
        check(m + 1 >= 4 + FIRE_RELAY_MIN && m + 1 <= 4 + FIRE_RELAY_MIN + 2, "the relay adds about 2 minutes");

        // The same platoon with the HQ 10 km behind it: out of manpack range, no one relays.
        World v = flat_world();
        int b2 = add(v, 0, U_ARTILLERY, "6x 2S3", 20);
        add(v, 0, U_HQ, "UAZ-469", 30);
        int r2 = add(v, 0, U_INFANTRY, "33 men", 50);
        int t2 = add(v, 1, U_INFANTRY, "33 men", 53);
        v.tick();
        l = v.fire_net_link(v.units[r2]);
        check(l.role == FN_CALLER && l.status == FN_CUT, "rifle platoon 10 km from the HQ is cut off");
        check(v.fire_call(v.units[b2], v.units[t2]) < 0, "no call for fire");
        check(first_round(v, b2, 30) < 0, "the guns stay silent: nobody who sees the target can call");

        // A recon section 25 km from the guns: beyond its own radio, relayed by the HQ in between.
        World x = flat_world();
        int b3 = add(x, 0, U_ARTILLERY, "4x 2S7", 20);
        int h3 = add(x, 0, U_HQ, "UAZ-469", 45);
        int rc = add(x, 0, U_RECON, "2x BRDM-2", 70);
        add(x, 1, U_INFANTRY, "33 men", 72);
        x.tick();
        l = x.fire_net_link(x.units[rc]);
        check(l.role == FN_OBSERVER && l.status == FN_RELAY && l.via == h3, "recon out of radio range: relayed");
        x.units[h3].alive = false;
        l = x.fire_net_link(x.units[rc]);
        check(l.status == FN_CUT && l.battery == b3 && l.distance == 50, "without the HQ the recon is cut off");

        // A formation without guns has no fire net.
        World y = flat_world();
        int lone = add(y, 0, U_RECON, "2x BRDM-2", 40);
        check(y.fire_net_link(y.units[lone]).role == FN_NONE, "no guns, no fire net");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
