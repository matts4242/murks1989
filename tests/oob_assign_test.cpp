// Headless check of MRK-35: moving units in the order of battle. A unit put under a second HQ
// takes its orders through that HQ (command delay from it); a team leader brings its team; a
// unit dropped on a platoon joins (or forms) its team; back under the company HQ; no loops in
// the chain; a lost HQ hands its units back to the company HQ; the order of battle order picks
// the company HQ; save/load keeps it all.
// Exit code 0 = all passed. Usage: oob_assign_test
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

static std::vector<uint8_t> map_bytes() {
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
    m.name = "oob-flat";
    m.save("oob_flat.murksmap");
    std::ifstream f("oob_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("oob_flat.murksmap");
    return bytes;
}

static World flat_world(const std::vector<uint8_t> &bytes) {
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(7);
    w.time_min = 10 * 60;
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
    return w;
}

static int add(World &w, UnitType t, const char *equip, HexCoord at, bool militia = false) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = SIDE_PLAYER;
    u.type = t;
    u.name = std::string("Blue ") + unit_type_info(t).name + " " + std::to_string(u.id);
    u.equipment = equip;
    u.formation = 0;
    u.militia = militia;
    u.pos = u.prev_pos = u.dest = at;
    u.elements_max = u.elements = World::element_count(u);
    w.formations[0].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

int main() {
    std::printf("oob_assign_test\n");
    const auto bytes = map_bytes();
    World w = flat_world(bytes);
    const int hq1 = add(w, U_HQ, "UAZ-469, R-145", {20, 20});
    const int hq2 = add(w, U_HQ, "UAZ-469, R-145", {60, 20}); // 20 km east
    const int t1 = add(w, U_TANK, "4x T-55A", {58, 20});
    const int t2 = add(w, U_TANK, "4x T-55A", {58, 22});
    const int t3 = add(w, U_TANK, "4x T-55A", {59, 23});
    const int m1 = add(w, U_MECH, "3x BMP-1", {22, 22});
    const int mil = add(w, U_INFANTRY, "30 men", {21, 21}, true);
    std::string why;

    // Defaults: the first HQ commands everyone; the second HQ answers to no one on the map.
    check(w.hq_above(w.units[t1]) == hq1 && w.hq_above(w.units[hq2]) == -1, "first HQ is the company HQ; a second HQ is a root");
    const int d_far = w.command_delay(w.units[t1]);

    // Under the second HQ: its orders now come through it (20 km closer, inside its zone).
    check(w.oob_assign({t1}, hq2, World::OA_HQ, &why) == 1, "tank put under the second HQ");
    check(w.hq_above(w.units[t1]) == hq2 && w.units[t1].attached_hq == hq2, "it answers to the second HQ");
    check(w.command_delay(w.units[t1]) < d_far, "command delay now counted from the second HQ (no courier)");

    // The second HQ under the first: a step down the chain for it too.
    const int d_hq2 = w.command_delay(w.units[hq2]);
    check(w.oob_assign({hq2}, hq1, World::OA_HQ, &why) == 1 && w.hq_above(w.units[hq2]) == hq1, "second HQ put under the first");
    check(w.command_delay(w.units[hq2]) > d_hq2, "an HQ under an HQ waits for its orders like any unit");
    check(w.is_above(hq1, t1), "the company HQ is above the tank, through the second HQ");

    // No loops.
    check(w.oob_assign({hq1}, hq2, World::OA_HQ, &why) == 0 && w.hq_above(w.units[hq1]) == -1, "an HQ can't go under its own subordinate");
    check(w.oob_assign({hq2}, hq2, World::OA_HQ, &why) == 0, "nor under itself");
    check(w.oob_assign({t2}, t1, World::OA_HQ, &why) == 0, "only an HQ takes units under it for command");
    check(w.oob_assign({mil}, hq2, World::OA_HQ, &why) == 0, "militia stay out of the chain");

    // A team: dropped on a platoon with no team, a team is formed with it as leader.
    check(w.oob_assign({t2}, t1, World::OA_TEAM, &why) == 1, "tank dropped on a platoon forms a team");
    const int gA = w.units[t1].group;
    check(gA >= 0 && w.groups[gA].leader == t1 && w.hq_above(w.units[t2]) == t1 && w.units[t2].follow,
          "led by that platoon, member following it");
    check(w.hq_above(w.units[t1]) == hq2, "the leader keeps its place under the second HQ");
    // Dropped on a member: into that member's team.
    check(w.oob_assign({t3}, t2, World::OA_TEAM, &why) == 1 && w.units[t3].group == gA && w.groups[gA].members.size() == 2,
          "dropped on a member: joins the same team");
    check(w.oob_assign({t3}, t1, World::OA_TEAM, &why) == 0, "already in that team: refused");

    // A leader goes under another HQ with its whole team.
    check(w.oob_assign({t1}, hq1, World::OA_HQ, &why) == 1, "team leader put under the company HQ");
    check(w.units[t1].attached_hq == -1 && w.hq_above(w.units[t1]) == hq1, "no explicit link needed: the company HQ is the default");
    check(w.hq_above(w.units[t2]) == t1 && w.hq_above(w.units[t3]) == t1, "its team stays with it");

    // A member put under an HQ leaves its team.
    check(w.oob_assign({t3}, hq2, World::OA_HQ, &why) == 1 && w.units[t3].group == -1 && !w.units[t3].follow &&
              w.hq_above(w.units[t3]) == hq2,
          "a member put under an HQ leaves its team");
    // Back to the company HQ.
    check(w.oob_assign({t2}, -1, World::OA_COMPANY, &why) == 1 && w.units[t2].group == -1 && w.hq_above(w.units[t2]) == hq1,
          "back under the company HQ, out of the team");
    check(w.units[t1].group == -1, "the team with no members left is disbanded");

    // A leader dropped into another team brings its members.
    w.oob_assign({t2}, t1, World::OA_TEAM, &why); // team: t1 leads t2
    check(w.oob_assign({t1}, m1, World::OA_TEAM, &why) == 2, "a leader joining another team brings its member");
    const int gB = w.units[m1].group;
    check(gB >= 0 && w.groups[gB].leader == m1 && w.units[t1].group == gB && w.units[t2].group == gB,
          "both in the new team under the mech platoon");

    // A loop through a team is broken: HQ2 (under HQ1) leads a team that HQ1 joins.
    World w2 = flat_world(bytes);
    const int a = add(w2, U_HQ, "UAZ", {10, 10});
    const int b = add(w2, U_HQ, "UAZ", {12, 10});
    w2.oob_assign({b}, a, World::OA_HQ, &why);
    w2.create_group({a, b}, b);
    check(!(w2.is_above(a, b) && w2.is_above(b, a)), "no loop left after a team closes one");

    // Order of battle order: the first HQ is the company HQ.
    check(w.oob_move(hq2, hq1, false), "second HQ moved before the first");
    w.oob_assign({hq2}, -1, World::OA_COMPANY, &why);
    check(w.hq_above(w.units[m1]) == hq2 && w.hq_above(w.units[hq1]) == -1, "the first HQ in the list commands the company");
    w.oob_move(hq2, hq1, true); // back after it
    check(w.formations[0].members[0] == hq1 && w.formations[0].members[1] == hq2, "and moved back after it");
    // Within a team: the team's own order too.
    w.oob_move(t2, t1, false);
    const auto &gm = w.groups[gB].members;
    check(gm.size() == 2 && gm[0] == t2 && gm[1] == t1, "moving within a team reorders the team");

    // Save/load keeps the links, and a replay after load is identical.
    std::vector<uint8_t> s = w.save_state();
    for (int i = 0; i < 30; ++i) w.tick();
    std::vector<uint8_t> after = w.save_state();
    std::string err;
    check(w.load_state(s.data(), s.size(), &err), "load ok");
    check(w.units[t3].attached_hq == hq2, "the HQ link survives save/load");
    for (int i = 0; i < 30; ++i) w.tick();
    check(after == w.save_state(), "replay after load is identical");

    // The second HQ lost: its units answer to the company HQ again.
    w.units[hq2].alive = false;
    w.units[hq2].elements = 0;
    check(w.hq_above(w.units[t3]) == hq1, "second HQ lost: back to the company HQ");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
