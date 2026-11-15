// Sandbox mode (work board MRK-31): placing and removing units for either side, the start
// time, the hostile AI switch, no victory or defeat, and save/load staying byte-identical.
// Exit code 0 = all passed.
#include "murks/sim.hpp"

#include <cstdio>
#include <string>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

static int alive_count(const World &w, int side) {
    int n = 0;
    for (const auto &u : w.units)
        if (u.alive && u.side == side) ++n;
    return n;
}

static bool occupied_by_other(const World &w, int id) {
    for (const auto &u : w.units)
        if (u.alive && u.id != id && u.pos == w.units[id].pos) return true;
    return false;
}

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0);
    const HexCoord home = w.units[0].pos;
    const int units_before = (int)w.units.size();

    std::printf("Outside the sandbox:\n");
    check(w.sandbox_place(SIDE_PLAYER, World::SB_TANK, home) < 0, "placing is refused in a normal game");
    check(!w.sandbox_remove(1), "removing is refused in a normal game");
    check(!w.set_start_time(15, 12), "the start time is fixed in a normal game");

    w.sandbox = true;
    std::printf("Start time:\n");
    check(w.set_start_time(16, 22) && w.time_min == 2 * 1440 + 22 * 60, "16 April 22:00 before the clock moves");
    check(!w.set_start_time(13, 6) && !w.set_start_time(21, 6) && !w.set_start_time(15, 24), "days 14-20, hours 0-23 only");
    check(w.set_start_time(14, 6) && w.time_min == 360, "back to 14 April 06:00");

    std::printf("Placing:\n");
    const int t = w.sandbox_place(SIDE_PLAYER, World::SB_TANK, home);
    check(t == units_before && w.units[t].alive && w.units[t].side == SIDE_PLAYER && w.units[t].type == U_TANK,
          "an own tank platoon is put on the map");
    check(w.units[t].equipment == "4x T-55A" && w.units[t].elements == 4, "with the scenario's kit (4x T-55A, 4 elements)");
    check(w.units[t].formation == 0, "it joins Company Murks");
    check(!occupied_by_other(w, t) && w.map.at(w.units[t].pos).terrain != T_WATER, "on a free land hex near the click");
    const int m = w.sandbox_place(SIDE_PLAYER, World::SB_MECH, home);
    check(m >= 0 && w.units[m].dismounts >= 0 && w.units[w.units[m].dismounts].embarked, "a mech platoon gets its squads aboard");
    const int h1 = w.sandbox_place(SIDE_HOSTILE, World::SB_TANK, {home.col + 20, home.row});
    const int hf = w.sandbox_hostile_formation();
    check(h1 >= 0 && w.units[h1].side == SIDE_HOSTILE && hf >= 0 && w.units[h1].formation == hf,
          "a hostile tank platoon joins the Sandbox Hostiles formation");
    check(w.formations[hf].objective < 0 && !w.formations[hf].hunts_player, "which has no objective and does not hunt");
    check(w.units[h1].equipment == "3x T-72M", "hostile kit (3x T-72M)");
    const int h2 = w.sandbox_place(SIDE_HOSTILE, World::SB_RIFLE, {home.col + 20, home.row});
    check(h2 >= 0 && w.units[h2].formation == hf && w.units[h2].pos != w.units[h1].pos, "a second hostile goes beside the first");
    check(w.sandbox_place(SIDE_HOSTILE, World::SB_SUPPLY, home) < 0, "no hostile supply columns (their trains are not modelled)");
    check(w.sandbox_place(SIDE_PLAYER, 99, home) < 0 && w.sandbox_place(5, World::SB_TANK, home) < 0, "bad kind or side refused");

    std::printf("Removing:\n");
    const int sq = w.units[m].dismounts;
    const int alive_own = alive_count(w, SIDE_PLAYER);
    check(w.sandbox_remove(m) && w.units[m].removed && w.units[sq].removed && !w.units[sq].embarked,
          "a mech platoon goes with its squads");
    check(alive_count(w, SIDE_PLAYER) == alive_own - 1, "one unit fewer on the map");
    bool listed = false;
    for (int id : w.formations[0].members) listed = listed || id == m || id == sq;
    check(!listed, "they leave the order of battle");
    check(!w.sandbox_remove(m), "a removed unit cannot be removed again");
    // Team: removing a member keeps the team; removing the leader disbands it.
    const int g = w.create_group({1, 2, t}, 1);
    check(g >= 0 && w.sandbox_remove(t) && w.groups[g].alive && w.groups[g].members.size() == 1 &&
              w.groups[g].keep_dx.size() == 1, "a removed member leaves its team");
    check(w.sandbox_remove(1) && !w.groups[g].alive && w.units[2].group < 0, "a removed leader disbands the team");
    // Supply column: its route ends.
    int col = -1;
    for (const auto &u : w.units)
        if (u.alive && u.type == U_TRUCK && u.side == SIDE_PLAYER) col = u.id;
    int depot = -1;
    for (int i = 0; i < (int)w.holdings.size(); ++i)
        if (w.holdings[i].owner == SIDE_PLAYER) depot = i;
    const int rid = col >= 0 && depot >= 0 ? w.set_supply_route(depot, -1) : -1;
    if (rid >= 0 && w.routes[rid].convoy >= 0) col = w.routes[rid].convoy;
    check(rid >= 0 && w.routes[rid].active, "the supply column works a route");
    check(rid >= 0 && w.sandbox_remove(col) && !w.routes[rid].active && w.routes[rid].convoy < 0, "removing the column ends its route");
    check(w.sandbox_remove(h2), "a hostile can be removed too");

    const World pre = w; // before the clock first moves
    std::printf("Ticks:\n");
    const int kills0 = w.stats().kills, losses0 = w.stats().losses;
    ticks(w, 1);
    check(w.clock_started() && !w.set_start_time(15, 6), "the start time locks once the clock moves");
    check(w.stats().kills == kills0 && w.stats().losses == losses0, "removals are neither kills nor losses");

    std::printf("Hostile AI off:\n");
    {
        World a = pre;
        a.hostile_ai = false;
        ticks(a, 600);
        bool any_moved = false;
        for (const auto &u : a.units)
            if (u.alive && u.side == SIDE_HOSTILE && (u.moving() || u.pending.active || u.pos != pre.units[u.id].pos)) any_moved = true;
        check(!any_moved, "with the hostile AI off no hostile has moved or been ordered after 10 h");
        World b = pre;
        ticks(b, 600);
        bool any = false;
        for (const auto &u : b.units)
            if (u.alive && u.side == SIDE_HOSTILE && (u.moving() || u.pending.active || u.pos != pre.units[u.id].pos)) any = true;
        check(any, "with it on, hostiles move");
    }

    std::printf("No victory or defeat:\n");
    {
        World d = w;
        for (auto &u : d.units)
            if (u.side == SIDE_PLAYER && u.alive && u.id != 0) d.sandbox_remove(u.id);
        ticks(d, 30);
        check(d.outcome() == OUT_ONGOING, "a company with no platoons left is not defeated in the sandbox");
        World e = w;
        e.sandbox = false;
        for (auto &u : e.units)
            if (u.side == SIDE_PLAYER && u.alive && u.id != 0) e.sandbox = true, e.sandbox_remove(u.id), e.sandbox = false;
        ticks(e, 30);
        check(e.outcome() == OUT_DEFEAT, "the same company outside the sandbox is defeated");
    }

    std::printf("Save/load:\n");
    {
        std::vector<uint8_t> s = w.save_state();
        ticks(w, 240);
        std::vector<uint8_t> after = w.save_state();
        World l = w;
        std::string err;
        check(l.load_state(s.data(), s.size(), &err) && l.sandbox && l.clock_started(), "load ok, still a sandbox");
        check(l.units[m].removed && !l.units[m].alive, "removed units stay removed");
        ticks(l, 240);
        check(after == l.save_state(), "replay 4 h after load is byte-identical");
        std::vector<uint8_t> old = s;
        old[7] = '9';
        check(!l.load_state(old.data(), old.size(), &err), "a v9 save is refused");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
