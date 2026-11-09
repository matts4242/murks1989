// Headless checks of solo orders to team members and the Rejoin order (MRK-22, 9 Nov 2026):
// a member given its own order leaves the formation when the order arrives and stays where it
// was sent; team moves and formation changes leave it alone; Rejoin brings it back to its place;
// a member in formation takes over from a lost leader before a detached one; save/load keeps it.
// Exit code 0 = all passed. Usage: team_orders_test
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

// A flat 128 x 128-hex map of open ground.
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
    m.name = "team-flat";
    m.save("team_flat.murksmap");
    std::ifstream f("team_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("team_flat.murksmap");
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(7);
    w.time_min = 10 * 60 - 1;
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
    w.formations.push_back({"Red", SIDE_HOSTILE, -1, false, 0, {}});
    return w;
}

static int add(World &w, UnitType t, const char *equip, HexCoord at) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = SIDE_PLAYER;
    u.type = t;
    u.name = std::string("Blue ") + unit_type_info(t).name + " " + std::to_string(u.id);
    u.equipment = equip;
    u.formation = 0;
    u.pos = u.prev_pos = u.dest = at;
    u.elements_max = u.elements = World::element_count(u);
    w.formations[0].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

static void run(World &w, int minutes) {
    for (int m = 0; m < minutes; ++m) w.tick();
}

int main() {
    World w = flat_world();
    const int hq = add(w, U_HQ, "2x BRDM-2U", {20, 64}); // company HQ, outside the team
    const int L = add(w, U_TANK, "3x T-55A", {30, 64});
    const int m1 = add(w, U_TANK, "3x T-55A", {28, 64});
    const int m2 = add(w, U_TANK, "3x T-55A", {32, 64});
    (void)hq;
    const int g = w.create_group({L, m1, m2}, L);
    w.set_group_shape(g, FS_LINE, 2);
    run(w, 30); // form up

    std::printf("Solo order to a team member:\n");
    const HexCoord far{30, 76}; // 6 km off, well outside the leader's 1 km zone
    const int delay = w.order_move(m1, far, MV_HASTY);
    check(delay > 0, "the order goes through the command delay");
    run(w, delay - 1);
    check(w.units[m1].follow, "still in formation while the order is on its way");
    run(w, 2);
    check(!w.units[m1].follow, "detached once the order arrives");
    run(w, 240);
    check(w.units[m1].pos == far, "goes where it was sent");
    run(w, 120);
    check(w.units[m1].pos == far && !w.units[m1].follow, "and stays there (no snap back to the leader)");

    std::printf("Team orders leave a detached member alone:\n");
    const HexCoord dest{60, 64};
    w.order_group_move(g, dest, MV_HASTY);
    run(w, 400);
    check(hex_distance(w.units[L].pos, dest) <= 1, "the leader gets there");
    check(hex_distance(w.units[m2].pos, w.units[L].pos) <= 3, "the member in formation comes along");
    check(w.units[m1].pos == far && !w.units[m1].follow, "the detached member doesn't move");
    w.order_group_hold(g);
    run(w, 30);
    check(w.units[m1].pos == far && !w.units[m1].follow, "a team hold doesn't pull it back either");
    w.set_group_shape(g, FS_COLUMN, 1);
    w.set_group_shape(g, FS_LINE, 2); // what a column deploying into line does
    run(w, 30);
    check(w.units[m1].pos == far && !w.units[m1].follow, "nor does a formation change");

    std::printf("Rejoin:\n");
    check(w.order_rejoin(L) < 0, "the leader can't be told to rejoin");
    check(w.order_rejoin(m2) < 0, "a member already in formation can't either");
    const int rd = w.order_rejoin(m1);
    check(rd > 0, "the rejoin order goes through the command delay");
    run(w, rd - 1);
    check(!w.units[m1].follow && w.units[m1].pos == far, "detached until the order arrives");
    run(w, 2);
    check(w.units[m1].follow, "follows the leader again once it arrives");
    run(w, 400);
    check(hex_distance(w.units[m1].pos, w.units[L].pos) <= 3, "back in its place by the leader");
    const HexCoord dest2{60, 50};
    w.order_group_move(g, dest2, MV_HASTY);
    run(w, 400);
    check(hex_distance(w.units[L].pos, dest2) <= 1 && hex_distance(w.units[m1].pos, w.units[L].pos) <= 3,
          "and moves with the team again");

    std::printf("A team order replaces a solo order still on its way:\n");
    w.order_move(m2, {70, 40}, MV_HASTY);
    w.order_group_move(g, {50, 50}, MV_HASTY);
    check(!w.units[m2].pending.active && w.units[m2].follow, "the member stays in formation");
    run(w, 300);
    check(hex_distance(w.units[m2].pos, w.units[L].pos) <= 3, "and moves with the team");

    std::printf("Hold to a team member:\n");
    const int hd = w.order_hold(m2);
    w.order_group_move(g, {40, 30}, MV_HASTY); // replaces it: the member follows
    run(w, hd + 2);
    check(w.units[m2].follow, "a team order given after the hold replaces it");
    run(w, 200);
    const HexCoord stay = w.units[m2].pos;
    const int hd2 = w.order_hold(m2);
    run(w, hd2 + 1);
    const HexCoord held = w.units[m2].pos;
    w.order_group_move(g, {60, 30}, MV_HASTY);
    run(w, 300);
    check(!w.units[m2].follow && w.units[m2].pos == held, "a hold that arrived keeps it where it stopped");
    (void)stay;

    std::printf("Save/load:\n");
    {
        w.order_rejoin(m2);
        std::vector<uint8_t> s = w.save_state();
        run(w, 60);
        std::vector<uint8_t> a = w.save_state();
        std::string err;
        check(w.load_state(s.data(), s.size(), &err), "load ok");
        check(!w.units[m2].follow && w.units[m2].pending.active && w.units[m2].pending.kind == O_REJOIN,
              "detached state and the rejoin order on its way survive a save");
        run(w, 60);
        check(a == w.save_state(), "replay after load is identical");
        check(w.units[m2].follow, "the rejoin arrives after loading");
    }

    std::printf("Leader lost:\n");
    {
        World v = flat_world();
        const int vl = add(v, U_TANK, "3x T-55A", {30, 64});
        const int d = add(v, U_TANK, "3x T-55A", {28, 64}); // first in the team, but detached
        const int f = add(v, U_TANK, "3x T-55A", {32, 64});
        const int vg = v.create_group({vl, d, f}, vl);
        run(v, 10);
        const HexCoord away{30, 90};
        run(v, v.order_move(d, away, MV_HASTY) + 1);
        check(!v.units[d].follow, "first member detached on its own order");
        v.units[vl].alive = false;
        v.units[vl].elements = 0;
        v.units[vl].strength = 0;
        run(v, 1);
        check(v.groups[vg].alive && v.groups[vg].leader == f, "the member in formation takes command, not the detached one");
        check(!v.units[d].follow, "the detached member stays detached under the new leader");
        run(v, 300);
        check(v.units[d].pos == away, "and still goes where it was sent");
    }

    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "All checks passed.");
    return failures ? 1 : 0;
}
