// Headless check of forward operating bases: an HQ digs in inside a town (immobile while it
// does), the town becomes a FOB with limited stores, faster resupply and readiness recovery and
// occupation income; the supply column can stock it; unguarded it is plundered, then overrun.
// Exit code 0 = all passed. Usage: fob_test
#include "murks/sim.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps * std::max(1.0, std::fabs(b)); }
static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}
static void run_to_next_logistics(World &w) {
    do w.tick();
    while (w.time_min % 60 != 0);
}
static int count_events(World &w, const std::string &needle) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) ++n;
    return n;
}
static void place(World &w, int id, HexCoord h) {
    Unit &u = w.units[id];
    u.pos = u.prev_pos = u.dest = h;
    u.path.clear();
    u.path_i = 0;
    u.progress = 0;
    if (u.order != O_DIGIN) u.order = O_HOLD;
}
static void clear_hostiles(World &w) {
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER) u.alive = false;
}
// A town 20-60 hexes from home, reachable by land, that is not `avoid`.
static int other_town(const World &w, int avoid) {
    const HexCoord home = w.holdings[0].pos;
    for (int i = 0; i < (int)w.map.settlements.size(); ++i) {
        const Settlement &s = w.map.settlements[i];
        const int d = hex_distance(s.pos, home);
        if (i != w.player_home && i != avoid && d >= 20 && d <= 60 && w.reachable(home, s.pos) && w.town_at(s.pos) == i)
            return i;
    }
    return -1;
}
static int first_hostile(const World &w) {
    for (const auto &u : w.units)
        if (u.side != SIDE_PLAYER) return u.id;
    return -1;
}
// Gives the order and plays through the command delay; returns minutes until the FOB is up.
static int start_dig_in(World &w, int id) {
    const int delay = w.order_dig_in(id);
    if (delay < 0) return -1;
    for (int i = 0; i <= delay + 1 && w.units[id].order != O_DIGIN; ++i) w.tick(); // to the minute it arrives
    return w.units[id].order == O_DIGIN ? (int)(w.units[id].dig_until - w.time_min) : -1;
}

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0); // 06:00
    clear_hostiles(w);
    const int HQ = 0, TANK1 = 1, TANK2 = 2;
    const HexCoord home = w.holdings[0].pos;
    const int town = other_town(w, -1);
    if (town < 0) {
        std::printf("no suitable town on this map\n");
        return 1;
    }
    const Settlement &T = w.map.settlements[town];
    std::printf("FOB town: %s (pop. %d), %d km from home\n", T.name.c_str(), T.population, hex_distance(T.pos, home) / 2);

    std::printf("Who can dig in, and where:\n");
    check(w.dig_in_problem(HQ).find("home depot") != std::string::npos, "not in the home town (it is the depot)");
    check(w.dig_in_problem(TANK1).find("Only an HQ") != std::string::npos, "only an HQ can");
    check(w.dig_in_problem(w.route.convoy).find("Only an HQ") != std::string::npos, "not the supply column");
    HexCoord open = T.pos;
    for (int r = 1; r < 40 && w.town_at(open) >= 0; ++r) open = {T.pos.col + r, T.pos.row};
    place(w, HQ, open);
    check(w.town_at(open) < 0 && w.dig_in_problem(HQ).find("needs a town") != std::string::npos, "not in open country");
    check(w.order_dig_in(HQ) < 0, "the order is refused there");
    place(w, HQ, T.pos);
    check(w.dig_in_problem(HQ).empty(), "an HQ in another town can");
    const std::vector<uint8_t> start = w.save_state();

    std::printf("Digging in:\n");
    w.units[HQ].readiness = 100;
    w.drain_events();
    const int mins = start_dig_in(w, HQ);
    std::printf("     fresh HQ digs in for %d min\n", mins);
    check(mins == DIG_IN_MIN_FRESH && count_events(w, "is digging in at") == 1, "30 minutes at full readiness");
    {
        Unit tired = w.units[HQ];
        tired.readiness = 20;
        check(w.dig_in_minutes(tired) == 54, "54 minutes for a worn-out HQ (readiness 20)");
        tired.readiness = 0;
        check(w.dig_in_minutes(tired) == DIG_IN_MIN_MAX, "never more than an hour");
    }
    check(w.order_move(HQ, home) < 0 && w.order_hold(HQ) < 0 && w.order_fall_back(HQ) < 0 &&
              w.order_regroup(HQ, false) < 0,
          "immobile: no move, hold, fall-back or regroup orders while digging in");
    check(!w.dig_in_problem(HQ).empty(), "can't dig in twice");
    ticks(w, mins - 1);
    check(w.holdings.size() == 1 && w.units[HQ].order == O_DIGIN, "not done a minute early");
    w.drain_events();
    w.tick();
    check(w.holdings.size() == 2 && w.units[HQ].order == O_HOLD, "the FOB is set up when the time is up");
    const int F = 1;
    const Holding &fob = w.holdings[F];
    check(fob.kind == H_FOB && fob.settlement == town && fob.pos == w.units[HQ].pos, "a FOB holding in that town");
    check(fob.diesel_cap == FOB_DIESEL_CAP && fob.ammo_cap == FOB_AMMO_CAP && fob.food_cap == FOB_FOOD_CAP &&
              fob.diesel_l == 0 && fob.ammo_t == 0,
          "limited stores, empty to begin with");
    const double income = std::max(FOB_INCOME_MIN, std::min(FOB_INCOME_MAX, T.population * FOB_INCOME_PER_HEAD));
    std::printf("     occupation income %.0f dollars a day\n", fob.income_per_day);
    check(near(fob.income_per_day, income), "income from the town's population");
    check(count_events(w, "is set up") == 1, "reported");
    check(w.order_move(HQ, home) >= 0, "the HQ can move again");
    check(w.dig_in_problem(HQ).find("already has a FOB") != std::string::npos, "one FOB per town");
    const std::vector<uint8_t> with_fob = w.save_state();

    std::printf("Called off and broken off:\n");
    w.load_state(start.data(), start.size());
    start_dig_in(w, HQ);
    check(w.cancel_dig_in(HQ) && w.units[HQ].order == O_HOLD && w.holdings.size() == 1, "cancel stops it at once");
    w.order_dig_in(HQ);
    check(w.cancel_dig_in(HQ) && !w.units[HQ].pending.active, "cancel also stops the order on its way");
    start_dig_in(w, HQ);
    w.drain_events();
    w.units[HQ].last_hit = w.time_min + 1; // hit in the next minute's fire
    w.tick();
    check(w.units[HQ].order == O_HOLD && count_events(w, "abandons digging in") == 1, "a hit abandons it");
    ticks(w, 60);
    check(w.holdings.size() == 1, "and no FOB comes of it");

    std::printf("FOB boosts:\n");
    w.load_state(with_fob.data(), with_fob.size());
    place(w, HQ, home); // the trains are at home: the FOB keeps its stores
    w.holdings[F].diesel_l = 5000;
    w.holdings[F].ammo_t = 10;
    place(w, TANK1, w.holdings[F].pos);
    place(w, TANK2, home);
    w.units[TANK1].fuel = w.units[TANK2].fuel = 50;
    run_to_next_logistics(w);
    std::printf("     fuel after an hour: %.1f at the FOB, %.1f at the depot\n", w.units[TANK1].fuel, w.units[TANK2].fuel);
    check(near(w.units[TANK1].fuel, 50 + 25 * FOB_SUPPLY_RATE) && near(w.units[TANK2].fuel, 75),
          "resupply at a FOB is 1.5 times as fast");
    check(near(w.holdings[F].diesel_l, 5000 - 0.25 * FOB_SUPPLY_RATE * unit_type_info(U_TANK).fuel_litres),
          "out of the FOB's own stores");
    w.units[TANK1].readiness = w.units[TANK2].readiness = 50;
    ticks(w, 60);
    std::printf("     readiness after an hour: %.1f at the FOB, %.1f at the depot\n", w.units[TANK1].readiness,
                w.units[TANK2].readiness);
    check(near(w.units[TANK1].readiness - 50, 2 * (w.units[TANK2].readiness - 50), 1e-3),
          "readiness recovers twice as fast at a FOB");

    std::printf("The supply column stocks the FOB:\n");
    w.load_state(with_fob.data(), with_fob.size());
    place(w, HQ, home);
    check(!w.set_route_target(0) && !w.set_route_target(7) && w.set_route_target(F), "deliveries can go to a FOB");
    check(w.route_target_name() == "FOB " + T.name, "named in the panel");
    const double depot_ammo = w.holdings[0].ammo_t;
    for (int i = 0; i < 12 * 60 && w.route.trips == 0; ++i) w.tick();
    for (int i = 0; i < 4 * 60 && w.route.state != CV_PARKED; ++i) w.tick();
    std::printf("     FOB now %.0f L, %.1f t, %.0f kg; %s\n", w.holdings[F].diesel_l, w.holdings[F].ammo_t,
                w.holdings[F].food_kg, w.convoy_status().c_str());
    check(w.route.trips == 1 && near(w.holdings[F].ammo_t, FOB_AMMO_CAP) && near(w.holdings[F].diesel_l, FOB_DIESEL_CAP) &&
              near(w.holdings[F].food_kg, FOB_FOOD_CAP),
          "one trip fills it to what it can hold");
    check(near(depot_ammo - w.holdings[0].ammo_t, FOB_AMMO_CAP), "out of the home depot");
    ticks(w, 120);
    check(w.route.trips == 1, "a full FOB gets no more");

    std::printf("Occupation income:\n");
    w.load_state(with_fob.data(), with_fob.size());
    while (w.time_min % 1440 != 6 * 60 - 1) w.tick();
    double cash = w.stock.cash;
    w.drain_events();
    w.tick();
    check(near(w.stock.cash - cash, income) && count_events(w, "Income from 1 occupied town") == 1,
          "paid in full at 06:00 when nobody contested it");
    // A hostile spends 6 hours in the FOB's territory (4 km out): a quarter of a day's income lost.
    w.load_state(start.data(), start.size());
    {
        const int foe = first_hostile(w); // all hostiles were cleared at the start: bring one back
        start_dig_in(w, HQ);
        while (w.units[HQ].order == O_DIGIN) w.tick();
        while (w.time_min % 1440 != 6 * 60 - 1) w.tick();
        w.tick(); // first income (the territory was quiet)
        HexCoord spot = w.holdings[F].pos;
        for (int r = 8; r >= 0; --r) {
            HexCoord h{spot.col + r, spot.row};
            if (w.map.in_bounds(h) && w.map.at(h).terrain != T_WATER) {
                spot = h;
                break;
            }
        }
        w.units[foe].alive = true;
        w.units[foe].ai_next = 1 << 30;
        w.units[foe].roe = ROE_HOLD;
        for (int i = 0; i < 6 * 60; ++i) {
            place(w, foe, spot);
            w.units[foe].pending.active = false;
            w.tick();
        }
        w.units[foe].alive = false;
        while (w.time_min % 1440 != 6 * 60 - 1) w.tick();
        cash = w.stock.cash;
        w.tick();
        std::printf("     contested %d hours: paid %.0f of %.0f\n", 6, w.stock.cash - cash, income);
        check(near(w.stock.cash - cash, income * 18.0 / 24.0, 1e-4), "each contested hour costs a 24th");
    }

    std::printf("Overrun:\n");
    w.load_state(start.data(), start.size());
    {
        const int foe = first_hostile(w); // all hostiles were cleared at the start: bring one back
        start_dig_in(w, HQ);
        while (w.units[HQ].order == O_DIGIN) w.tick();
        w.holdings[F].ammo_t = 8;
        w.set_route_target(F);
        for (auto &u : w.units) // the company leaves the FOB unguarded
            if (u.side == SIDE_PLAYER && u.type != U_SUPPLY) place(w, u.id, home);
        w.units[foe].alive = true;
        w.units[foe].ai_next = 1 << 30;
        place(w, foe, w.holdings[F].pos);
        w.drain_events();
        run_to_next_logistics(w);
        check(!w.holdings[F].lost && near(w.holdings[F].ammo_t, 8 * (1 - RAID_SHARE)) &&
                  count_events(w, "is plundering FOB") == 1,
              "first hour unguarded: plundered, with a warning");
        place(w, foe, w.holdings[F].pos);
        run_to_next_logistics(w);
        check(w.holdings[F].lost && w.holdings[F].ammo_t == 0 && count_events(w, "has been overrun") == 1,
              "still unguarded an hour later: overrun and lost");
        check(w.route.target == -1 && w.depot_near(w.holdings[F].pos) != F, "no longer a supply point or a target");
        w.units[foe].alive = false;
        place(w, HQ, T.pos);
        check(w.dig_in_problem(HQ).empty(), "the town can be taken again");
    }

    std::printf("Save / load:\n");
    w.load_state(start.data(), start.size());
    start_dig_in(w, HQ);
    ticks(w, 10);
    const std::vector<uint8_t> mid = w.save_state();
    ticks(w, 120);
    const std::vector<uint8_t> a = w.save_state();
    std::string err;
    check(w.load_state(mid.data(), mid.size(), &err), "load ok mid dig-in");
    check(w.units[HQ].order == O_DIGIN && w.units[HQ].dig_town == town, "still digging in after the load");
    ticks(w, 120);
    check(a == w.save_state(), "replay after load is identical");
    check(w.holdings.size() == 2 && w.holdings[1].kind == H_FOB, "and the FOB comes up as before");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
