// Headless check of MRK-22: a team member given its own order leaves the formation when that
// order arrives (not when it is given), goes where it was sent and stays there; a team move
// afterwards does not pull it along; Rejoin team puts it back in its place; save/load keeps it.
// Exit code 0 = all passed. Usage: team_detach_test
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
    m.init(2, 2);
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    m.name = "detach-flat";
    m.save("detach_flat.murksmap");
    std::ifstream f("detach_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("detach_flat.murksmap");
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(5);
    w.time_min = 10 * 60 - 1;
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
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

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

int main() {
    World w = flat_world();
    add(w, U_HQ, "2x BTR-60PB", {60, 64});
    const int l = add(w, U_TANK, "3x T-55A", {62, 64});
    const int a = add(w, U_TANK, "3x T-55A", {62, 66});
    const int b = add(w, U_TANK, "3x T-55A", {62, 62});
    w.tick();
    const int g = w.create_group({l, a, b}, l);
    w.set_group_shape(g, FS_LINE, 1);
    ticks(w, 5);
    check(g >= 0 && w.units[a].follow && w.units[b].follow, "a team of three: both members follow the leader");

    std::printf("Solo order to a member:\n");
    const HexCoord away = {62, 80}; // 7 km south of the team
    const int delay = w.order_move(a, away, MV_HASTY);
    check(delay > 0 && w.units[a].follow, "the member keeps station while its order travels");
    ticks(w, delay + 1);
    check(!w.units[a].follow && w.units[a].moving() && w.units[a].dest == away, "when the order arrives it detaches and goes");
    for (int m = 0; m < 300 && w.units[a].moving(); ++m) w.tick();
    check(w.units[a].pos == away, "it reaches the place it was sent to");
    ticks(w, 120);
    check(w.units[a].pos == away && !w.units[a].moving(), "and stays there (no snap back to the formation)");
    check(w.units[a].group == g && !w.units[a].follow, "it is still in the team, detached");

    std::printf("Team move leaves the detached member alone:\n");
    const int td = w.order_group_move(g, {40, 64}, MV_HASTY);
    ticks(w, td + 1);
    for (int m = 0; m < 400 && (w.units[l].moving() || w.units[b].moving()); ++m) w.tick();
    check(hex_distance(w.units[l].pos, {40, 64}) <= 1, "the leader arrives");
    check(hex_distance(w.units[b].pos, w.units[l].pos) <= 4, "the following member comes along");
    check(w.units[a].pos == away, "the detached member does not move");

    std::printf("Rejoin:\n");
    check(w.order_rejoin(l) < 0, "the leader has nothing to rejoin");
    check(w.order_rejoin(b) < 0, "a member in formation has nothing to rejoin");
    const int rd = w.order_rejoin(a);
    check(rd > 0, "the detached member gets a rejoin order (command delay)");
    ticks(w, rd + 1);
    check(w.units[a].follow, "when it arrives the member follows the leader again");
    for (int m = 0; m < 600 && hex_distance(w.units[a].pos, w.units[l].pos) > 3; ++m) w.tick();
    check(hex_distance(w.units[a].pos, w.units[l].pos) <= 3, "and takes its place by the leader");
    int rejoin_events = 0;
    for (auto &e : w.drain_events())
        if (e.text.find("rejoins") != std::string::npos) ++rejoin_events;
    check(rejoin_events == 1, "reported once");

    std::printf("Save/load:\n");
    {
        w.order_move(b, {40, 80}, MV_HASTY);
        ticks(w, 3);
        std::vector<uint8_t> s = w.save_state();
        ticks(w, 200);
        std::vector<uint8_t> after = w.save_state();
        std::string err;
        check(w.load_state(s.data(), s.size(), &err), "load ok");
        ticks(w, 200);
        check(after == w.save_state(), "replay after load is identical (detach on arrival included)");
        check(!w.units[b].follow && w.units[a].follow, "one member detached, one in formation, as before the load");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
