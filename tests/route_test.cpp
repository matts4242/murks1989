// Headless checks of waypoint routes (MRK-20): waypoints visited in order with their halts, one
// command delay for the whole route, move mode per leg, route edits as adjustments (50% / 75% /
// full, at least 3 minutes; big changes and Assault are new orders), the x1.25 on-the-move
// factor, a route paused by an enemy-held hex or the stand-off line and resumed once clear,
// orders that drop the route, the 16-waypoint limit, leg planning for the map, and save/load
// replaying a route exactly. Exit code 0 = all passed. Usage: route_test
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

static std::vector<uint8_t> flat_map_bytes() {
    // A flat 128 x 128-hex map of open ground with one east-west road along row 64 and a
    // lake at columns 100-110, rows 100-110.
    Map m;
    m.init(2, 2);
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = (c >= 100 && c <= 110 && r >= 100 && r <= 110) ? T_WATER : T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    for (int c = 0; c + 1 < m.cols(); ++c) m.link({c, 64}, {c + 1, 64}, NET_ROAD);
    m.name = "route-flat";
    m.save("route_flat.murksmap");
    std::ifstream f("route_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("route_flat.murksmap");
    return bytes;
}

static World flat_world() {
    static const std::vector<uint8_t> bytes = flat_map_bytes();
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
    if (formation == 1) {
        u.ai_next = 1ll << 40; // the hostile AI stays out of these checks
        u.roe = ROE_HOLD;
    }
    w.formations[formation].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

static Waypoint wp(int c, int r, MoveMode mode = MV_HASTY, int halt = 0) {
    Waypoint w;
    w.at = {c, r};
    w.mode = mode;
    w.halt_min = (int16_t)halt;
    return w;
}

static int count(World &w, const char *needle) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) ++n;
    return n;
}

// Ticks until the unit has no route and is halted (or `limit` minutes pass).
static int run_route(World &w, int id, int limit) {
    for (int i = 0; i < limit; ++i) {
        w.tick();
        const Unit &u = w.units[id];
        if (!u.pending.active && !u.on_route() && !u.moving()) return i + 1;
    }
    return -1;
}

int main() {
    std::printf("Waypoints in order, halts, one command delay:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 30});
        std::vector<Waypoint> r{wp(20, 30, MV_HASTY, 10), wp(20, 40, MV_HASTY, 5), wp(30, 40), wp(30, 50)};
        const int expect = w.command_delay(w.units[t]);
        bool adj = true;
        const int d = w.order_route(t, r, &adj);
        check(d == expect && !adj, "a new route costs the unit's normal command delay");
        check(w.units[t].pending.route.size() == 4, "the whole route travels as one pending order");
        int idx = 0, activations = 0;
        int64_t arrive[4] = {-1, -1, -1, -1}, leave[4] = {-1, -1, -1, -1};
        bool was_pending = true;
        for (int i = 0; i < 900 && (w.units[t].on_route() || w.units[t].pending.active || w.units[t].moving()); ++i) {
            w.tick();
            const Unit &u = w.units[t];
            if (was_pending && !u.pending.active) ++activations;
            was_pending = u.pending.active;
            for (int k = 0; k < 4; ++k) {
                if (arrive[k] < 0 && u.pos == r[k].at) {
                    arrive[k] = w.time_min;
                    if (k == idx) ++idx;
                }
                if (arrive[k] >= 0 && leave[k] < 0 && (u.moving() || !(u.pos == r[k].at))) leave[k] = w.time_min;
            }
        }
        check(activations == 1, "the route arrives once");
        check(idx == 4, "all four waypoints are reached, in order");
        check(arrive[0] < arrive[1] && arrive[1] < arrive[2] && arrive[2] < arrive[3], "arrival times rise");
        std::printf("     halted at waypoint 1 for %lld min, waypoint 2 for %lld min, waypoint 3 for %lld min\n",
                    (long long)(leave[0] - arrive[0]), (long long)(leave[1] - arrive[1]),
                    (long long)(leave[2] - arrive[2]));
        check(leave[0] - arrive[0] == 10, "halts 10 min at waypoint 1");
        check(leave[1] - arrive[1] == 5, "halts 5 min at waypoint 2");
        check(leave[2] - arrive[2] <= 1, "no halt at waypoint 3 (the next leg starts the next minute)");
        check(w.units[t].pos == r[3].at && !w.units[t].on_route(), "ends at the last waypoint with no route left");
        const int arrived = count(w, "has arrived");
        check(arrived == 1, "only the last waypoint reports an arrival");
    }

    std::printf("Move mode per leg:\n");
    {
        World w = flat_world();
        int a = add(w, 0, U_TANK, "3x T-72M", {10, 64});
        int b = add(w, 0, U_TANK, "3x T-72M", {10, 63});
        w.order_route(a, {wp(60, 64, MV_ROAD)});
        w.order_route(b, {wp(60, 63, MV_DELIBERATE)});
        const int ta = run_route(w, a, 900);
        World v = flat_world();
        int c = add(v, 0, U_TANK, "3x T-72M", {10, 64});
        v.order_route(c, {wp(60, 64, MV_DELIBERATE)});
        const int tc = run_route(v, c, 1200);
        std::printf("     25 km on the road: road march %d min, deliberate %d min\n", ta, tc);
        check(ta > 0 && tc > 0 && ta < tc, "a road-march leg is faster on the road than a deliberate one");
        World x = flat_world();
        int d = add(x, 0, U_TANK, "3x T-72M", {10, 64});
        x.order_route(d, {wp(30, 64, MV_ROAD), wp(30, 74, MV_DELIBERATE)});
        bool road_first = false, deliberate_second = false;
        for (int i = 0; i < 900 && (x.units[d].on_route() || x.units[d].pending.active); ++i) {
            x.tick();
            const Unit &u = x.units[d];
            if (u.moving() && u.wp_i == 0 && u.mode == MV_ROAD) road_first = true;
            if (u.moving() && u.wp_i == 1 && u.mode == MV_DELIBERATE) deliberate_second = true;
        }
        check(road_first && deliberate_second, "each leg runs at its own waypoint's mode");
    }

    std::printf("A team on a route, formation per leg:\n");
    {
        World w = flat_world();
        int l = add(w, 0, U_TANK, "3x T-72M", {10, 64});
        int m1 = add(w, 0, U_TANK, "3x T-72M", {10, 66});
        int m2 = add(w, 0, U_TANK, "3x T-72M", {10, 62});
        const int gid = w.create_group({l, m1, m2}, l);
        check(gid >= 0, "team formed");
        Waypoint a = wp(35, 64, MV_ROAD), b = wp(35, 80);
        a.shape = FS_COLUMN;
        b.shape = FS_LINE;
        check(w.order_group_route(gid, {a, b}) > 0, "the team route goes to the leader");
        bool column_on_road = false, line_after = false;
        for (int i = 0; i < 900 && (w.units[l].on_route() || w.units[l].pending.active); ++i) {
            w.tick();
            const Unit &L = w.units[l];
            if (L.moving() && L.wp_i == 0 && w.groups[gid].shape == FS_COLUMN) column_on_road = true;
            if (L.moving() && L.wp_i == 1 && w.groups[gid].shape == FS_LINE) line_after = true;
        }
        for (int i = 0; i < 120; ++i) w.tick();
        check(column_on_road && line_after, "column for the road leg, line for the last leg");
        check(w.units[l].pos == b.at && hex_distance(w.units[m1].pos, b.at) <= 6 && hex_distance(w.units[m2].pos, b.at) <= 6,
              "the team arrives together");
    }

    std::printf("Command delay on the move and route adjustments:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 30});
        const int base = w.command_delay(w.units[t]);
        bool adj = false;
        int d = w.route_delay(t, {wp(13, 30)}, &adj);
        check(adj && d == std::max(ADJUST_MIN_DELAY, (int)std::ceil(base * 0.5f)),
              "halted, a move of 1.5 km is an adjustment at 50% (adjust position)");
        d = w.route_delay(t, {wp(20, 30)}, &adj);
        check(!adj && d == base, "halted, a 5 km move is a new order at full delay");
        d = w.route_delay(t, {wp(12, 30, MV_ASSAULT)}, &adj);
        check(!adj && d == base, "an assault is always a new order");

        w.order_route(t, {wp(40, 30), wp(40, 50)});
        for (int i = 0; i < 80 && !w.units[t].moving(); ++i) w.tick();
        check(w.units[t].moving(), "the route is under way");
        Unit halted = w.units[t];
        halted.path.clear();
        halted.path_i = 0;
        const int d_halt = w.command_delay(halted), d_move = w.command_delay(w.units[t]);
        std::printf("     same unit halted %d min, on the move %d min\n", d_halt, d_move);
        check(d_move > d_halt && std::abs(d_move - (int)std::ceil(d_halt * MOVING_ORDER_FACTOR)) <= 1,
              "a unit on the move gets orders at x1.25");
        // Edits: shift the second waypoint 1.5 km, then again, then again.
        const int mb = w.command_delay(w.units[t]);
        d = w.order_route(t, {wp(40, 30), wp(43, 50)}, &adj);
        check(adj && d == std::max(3, (int)std::ceil(mb * 0.5f)), "1st adjustment: 50%");
        check(w.units[t].on_route() && w.units[t].route[w.units[t].wp_i].at == HexCoord{40, 30} &&
                  w.units[t].moving(),
              "the unit keeps following its old route until the adjustment arrives");
        w.tick();
        d = w.order_route(t, {wp(40, 30), wp(42, 50)}, &adj);
        check(adj && d == std::max(3, (int)std::ceil(w.command_delay(w.units[t]) * 0.75f)),
              "2nd within 30 min: 75%");
        w.tick();
        d = w.order_route(t, {wp(40, 30), wp(41, 50)}, &adj);
        check(adj && d == w.command_delay(w.units[t]), "3rd: full delay");
        d = w.route_delay(t, {wp(40, 30), wp(40, 60)}, &adj);
        check(!adj, "moving the destination 5 km is a new order");
        d = w.route_delay(t, {wp(40, 30), wp(48, 40), wp(40, 50)}, &adj);
        check(!adj, "a new waypoint 4 km off the old route is a new order");
        d = w.route_delay(t, {wp(40, 30), wp(41, 40), wp(40, 50)}, &adj);
        check(adj, "a new waypoint 0.5 km off the old route is an adjustment");
        // Let the edit arrive and run a while: 30 quiet minutes later the streak starts again.
        for (int i = 0; i < 60; ++i) w.tick();
        d = w.route_delay(t, {wp(40, 30), wp(42, 50)}, &adj);
        check(adj && d == std::max(3, (int)std::ceil(w.command_delay(w.units[t]) * 0.5f)),
              "after 30 quiet minutes an adjustment is back at 50%");
        check(count(w, "adjusts its route") >= 1, "the adjustment is reported when it arrives");

        // The 3-minute floor: a unit with a short delay.
        World v = flat_world();
        add(v, 0, U_HQ, "UAZ-469", {10, 10});
        int s = add(v, 0, U_TANK, "3x T-72M", {11, 10});
        const int sb = v.command_delay(v.units[s]);
        d = v.route_delay(s, {wp(12, 10)}, &adj);
        std::printf("     next to the HQ: full %d min, adjustment %d min\n", sb, d);
        check(adj && d == std::max(ADJUST_MIN_DELAY, (int)std::ceil(sb * 0.5f)) && d >= ADJUST_MIN_DELAY,
              "an adjustment takes at least 3 minutes");
    }

    std::printf("Halted by an enemy-held hex, resumes once clear:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 80});
        w.units[t].roe = ROE_HOLD;
        int e = add(w, 1, U_INFANTRY, "33 men", {25, 80});
        w.units[e].pos = w.units[e].prev_pos = w.units[e].dest = {25, 80};
        w.order_route(t, {wp(40, 80)});
        bool paused = false;
        for (int i = 0; i < 300; ++i) {
            w.tick();
            if (w.units[t].route_pause == RP_ENEMY_HEX) {
                paused = true;
                break;
            }
        }
        check(paused && !w.units[t].moving() && w.units[t].on_route(),
              "halts in front of the enemy and keeps its route");
        for (int i = 0; i < 10; ++i) w.tick();
        check(w.units[t].route_pause == RP_ENEMY_HEX && !w.units[t].moving(), "waits while the hex is held");
        w.drain_events();
        w.units[e].pos = w.units[e].prev_pos = w.units[e].dest = {25, 120}; // the enemy moves off
        const int took = run_route(w, t, 300);
        check(took > 0 && w.units[t].pos == HexCoord{40, 80}, "resumes and reaches the end of its route");
        check(count(w, "resumes its route") == 1, "reports that it resumes");
    }

    std::printf("Halted at the stand-off line, resumes once clear:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 90});
        w.units[t].roe = ROE_HOLD;
        w.units[t].standoff = 6; // 3 km
        int e = add(w, 1, U_TANK, "3x T-72M", {35, 90});
        w.order_route(t, {wp(30, 90)});
        bool paused = false;
        for (int i = 0; i < 300; ++i) {
            w.tick();
            if (w.units[t].route_pause == RP_STANDOFF) {
                paused = true;
                break;
            }
        }
        check(paused && w.units[t].on_route(), "halts at its stand-off and keeps its route");
        const HexCoord held_at = w.units[t].pos;
        for (int i = 0; i < 12; ++i) w.tick();
        check(w.units[t].route_pause == RP_STANDOFF && hex_distance(w.units[t].pos, held_at) <= 1,
              "stays put while the enemy is inside the stand-off ahead");
        w.units[e].pos = w.units[e].prev_pos = w.units[e].dest = {35, 125}; // the enemy pulls out
        w.units[e].seen_until[SIDE_PLAYER] = -1;
        w.units[e].last_seen[SIDE_PLAYER] = -100000;
        const int took = run_route(w, t, 300);
        check(took > 0 && w.units[t].pos == HexCoord{30, 90}, "resumes once clear and reaches its waypoint");
    }

    std::printf("Orders that drop the route; limits:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 30});
        w.order_route(t, {wp(40, 30), wp(40, 50)});
        for (int i = 0; i < 80 && !w.units[t].moving(); ++i) w.tick();
        w.order_hold(t);
        for (int i = 0; i < 80 && w.units[t].pending.active; ++i) w.tick();
        check(!w.units[t].on_route() && !w.units[t].moving(), "Hold drops the route");
        w.order_route(t, {wp(40, 30), wp(40, 50)});
        for (int i = 0; i < 80 && !w.units[t].moving(); ++i) w.tick();
        w.order_fall_back(t);
        for (int i = 0; i < 80 && w.units[t].pending.active; ++i) w.tick();
        check(!w.units[t].on_route(), "Fall back drops the route");

        std::vector<Waypoint> r16, r17;
        for (int k = 0; k < 16; ++k) r16.push_back(wp(12 + k, 30));
        r17 = r16;
        r17.push_back(wp(40, 30));
        check(w.route_delay(t, r16) > 0, "16 waypoints are accepted");
        check(w.order_route(t, r17) == -1, "17 waypoints are refused");
        check(w.order_route(t, {wp(20, 30), wp(105, 105)}) == -1, "a waypoint on water is refused");
        check(w.order_route(t, {wp(20, 30, MV_HASTY, 61)}) == -1, "a halt over 60 min is refused");
        check(w.order_route(t, {}) == -1, "an empty route is refused");
    }

    std::printf("Leg planning for the map:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 64});
        const Unit before = w.units[t];
        std::vector<int> path;
        const float road = w.plan_leg(t, {10, 64}, {40, 64}, MV_ROAD, path);
        const size_t n = path.size();
        const float slow = w.plan_leg(t, {10, 64}, {40, 64}, MV_DELIBERATE, path);
        std::printf("     15 km: road march %.0f min, deliberate %.0f min, %zu hexes\n", road, slow, n);
        check(road > 0 && slow > road && n >= 30, "plan_leg gives the path and minutes per mode");
        check(w.units[t].pos == before.pos && !w.units[t].moving() && w.stats().paths_last_tick == 0,
              "planning moves no one and spends no path budget");
        check(w.plan_leg(t, {10, 64}, {105, 105}, MV_HASTY, path) < 0, "no leg onto water");
    }

    std::printf("Save and load mid-route:\n");
    {
        World w = flat_world();
        int t = add(w, 0, U_TANK, "3x T-72M", {10, 30});
        w.order_route(t, {wp(20, 30, MV_HASTY, 15), wp(20, 45, MV_DELIBERATE), wp(35, 45, MV_ROAD)});
        for (int i = 0; i < 70; ++i) w.tick();
        const std::vector<uint8_t> mid = w.save_state();
        const int wp_mid = w.units[t].wp_i;
        for (int i = 0; i < 240; ++i) w.tick();
        const std::vector<uint8_t> a = w.save_state();
        World v = flat_world();
        std::string err;
        const bool ok = v.load_state(mid.data(), mid.size(), &err);
        check(ok, "the save loads");
        check(ok && v.units[t].route.size() == 3 && v.units[t].wp_i == wp_mid, "the route and its progress are restored");
        for (int i = 0; i < 240; ++i) v.tick();
        check(ok && v.save_state() == a, "loading and playing on replays the route exactly");
        std::vector<uint8_t> old = mid;
        old[7] = '6';
        check(!v.load_state(old.data(), old.size(), &err), "a v6 save is refused");
    }

    if (failures) std::printf("%d check(s) failed\n", failures);
    else std::printf("all passed\n");
    return failures ? 1 : 0;
}
