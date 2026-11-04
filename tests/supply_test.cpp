// Headless check of the supply line: the home depot, the company trains, the supply column
// between them, depot raids, food by location and weekly wages. Exit code 0 = all passed.
// Usage: supply_test
#include "murks/sim.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
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
    u.order = O_HOLD;
}
// A land hex `dist` hexes from `from`, reachable by land, on a road if there is one.
static HexCoord land_hex_at(const World &w, HexCoord from, int dist) {
    HexCoord fallback = from;
    for (int dr = -dist; dr <= dist; ++dr)
        for (int dc = -dist - 1; dc <= dist + 1; ++dc) {
            HexCoord h{from.col + dc, from.row + dr};
            if (!w.map.in_bounds(h) || hex_distance(h, from) != dist || !w.reachable(from, h)) continue;
            if (w.map.at(h).features & F_ROAD) return h;
            fallback = h;
        }
    return fallback;
}
// No enemies: the supply line on its own.
static void clear_hostiles(World &w) {
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER) {
            u.alive = false;
            u.elements = 0;
            u.strength = 0;
        }
}
static double ammo_total(const World &w) { return w.holdings[0].ammo_t + w.stock.ammo_t + w.route.ammo_t; }
static double diesel_total(const World &w) { return w.holdings[0].diesel_l + w.stock.diesel_l + w.route.diesel_l; }

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0); // 06:00
    const int HQ = 0, TANK1 = 1, TANK2 = 2;
    const int C = w.route.convoy;

    std::printf("Start:\n");
    check(w.holdings.size() == 1 && w.holdings[0].pos == w.units[HQ].pos, "home depot at the billet");
    check(near(w.holdings[0].diesel_l + w.stock.diesel_l, 30000) && near(w.holdings[0].ammo_t + w.stock.ammo_t, 60) &&
              near(w.holdings[0].food_kg + w.stock.food_kg, 6000),
          "stocks split between depot and trains, same totals as before");
    check(near(w.stock.ammo_cap, 24) && near(w.stock.diesel_cap, 12000) && near(w.stock.food_cap, 2000), "trains capacity");
    check(C >= 0 && w.units[C].type == U_SUPPLY && w.units[C].elements == 6 && w.units[C].formation != 0,
          "supply column of 6 trucks, outside the company's order of battle");
    check(w.column_capacity_t() == 6 * TRUCK_LOAD_T, "column carries 27 t");
    check(w.route.active && w.route.state == CV_PARKED, "route running, column parked");
    check(w.order_move(C, w.units[TANK1].pos) < 0 && w.order_hold(C) < 0 && w.create_group({C, TANK1}, TANK1) < 0,
          "the column takes no direct orders");
    check(w.effective_combat_units(w.units[C].formation) == 0, "the column is no fighting unit");
    check(near(w.stock.cash, START_CASH + CONTRACT_ADVANCE), "chest holds the advance");
    std::printf("     %s\n", w.convoy_status().c_str());

    std::printf("Trains at the depot:\n");
    const std::vector<uint8_t> start = w.save_state();
    clear_hostiles(w);
    w.stock.ammo_t = 4;
    double tot = ammo_total(w), depot0 = w.holdings[0].ammo_t;
    run_to_next_logistics(w);
    std::printf("     trains loaded %.1f t of ammo in one hour\n", w.stock.ammo_t - 4);
    check(near(w.stock.ammo_t - 4, DEPOT_LOAD_T_PER_H), "trains load 15 t an hour at the depot");
    check(near(ammo_total(w), tot) && near(depot0 - w.holdings[0].ammo_t, DEPOT_LOAD_T_PER_H), "ammo conserved");
    check(w.route.state == CV_PARKED, "no convoy while the trains are at the depot");

    std::printf("Units at the depot draw from it:\n");
    place(w, TANK2, w.holdings[0].pos);
    w.units[TANK2].fuel = 50;
    double trains_d = w.stock.diesel_l, depot_d = w.holdings[0].diesel_l;
    run_to_next_logistics(w);
    check(near(w.units[TANK2].fuel, 75.0f) && near(w.stock.diesel_l, trains_d),
          "a tank at the depot refuels from the depot, not the trains");
    check(near(depot_d - w.holdings[0].diesel_l, 0.25 * unit_type_info(U_TANK).fuel_litres), "1,000 L from the depot");

    std::printf("Food by location at 06:00:\n");
    while (w.time_min % 1440 != 6 * 60 - 1) w.tick();
    const double at_depot = w.food_need_per_day(true), with_trains = w.food_need_per_day(false);
    const double fd0 = w.holdings[0].food_kg, ft0 = w.stock.food_kg;
    w.tick();
    std::printf("     %.0f kg eaten at the depot, %.0f kg from the trains, %.0f kg bought in town\n", at_depot,
                with_trains, w.holdings[0].food_per_day);
    check(at_depot > 0 && with_trains > 0, "some units at the depot, some away from it");
    check(near(w.holdings[0].food_kg, fd0 + w.holdings[0].food_per_day - at_depot), "depot: town delivers, units there eat");
    check(near(w.stock.food_kg, ft0 - with_trains), "trains feed everyone else");

    std::printf("Supply column round trip:\n");
    w.load_state(start.data(), start.size());
    clear_hostiles(w);
    const HexCoord home = w.holdings[0].pos;
    const HexCoord field = land_hex_at(w, home, 20);
    place(w, HQ, field);
    w.stock.ammo_t = 6; // under half: urgent
    const double ammo0 = ammo_total(w), diesel0 = diesel_total(w);
    const double depot_ammo0 = w.holdings[0].ammo_t;
    bool seen[5] = {};
    int64_t t_left = -1, t_back = -1;
    const int64_t t0 = w.time_min;
    for (int i = 0; i < 12 * 60 && !(seen[CV_RETURNING] && w.route.state == CV_PARKED); ++i) {
        w.tick();
        seen[w.route.state] = true;
        if (w.route.state == CV_OUTBOUND && t_left < 0) t_left = w.time_min;
        if (seen[CV_RETURNING] && w.route.state == CV_PARKED) t_back = w.time_min;
    }
    std::printf("     trains %d km out; left after %lld min, back after %lld min; %s\n",
                hex_distance(home, field) / 2, (long long)(t_left - t0), (long long)(t_back - t0),
                w.convoy_status().c_str());
    check(seen[CV_LOADING] && seen[CV_OUTBOUND] && seen[CV_UNLOADING] && seen[CV_RETURNING] && t_back > 0,
          "load, drive out, unload, drive back, park");
    check(w.route.trips == 1 && near(w.stock.ammo_t, w.stock.ammo_cap), "one trip fills the trains' ammo");
    check(near(depot_ammo0 - w.holdings[0].ammo_t, 18.0, 1e-4), "18 t came out of the depot");
    check(near(ammo_total(w), ammo0, 1e-9), "ammo conserved over the trip");
    const double burned = diesel0 - diesel_total(w);
    std::printf("     column burned %.0f L of diesel on the round trip\n", burned);
    check(burned > 0 && burned < 200, "the column's own fuel comes out of the depot");
    check(hex_distance(w.units[C].pos, home) <= SUPPLY_RADIUS, "column back at the depot");
    check(count_events(w, "Supply column delivered") == 1, "delivery reported");

    std::printf("Trains move on while the column is out:\n");
    w.stock.ammo_t = 6;
    w.route.until = 0;
    for (int i = 0; i < 120 && w.route.state != CV_OUTBOUND; ++i) w.tick();
    const HexCoord field2 = land_hex_at(w, field, 8);
    place(w, HQ, field2);
    for (int i = 0; i < 8 * 60 && w.route.state != CV_UNLOADING; ++i) w.tick();
    check(w.route.state == CV_UNLOADING && hex_distance(w.units[C].pos, field2) <= SUPPLY_RADIUS,
          "the column follows the trains to where they went");

    std::printf("Ambush on the road:\n");
    w.load_state(start.data(), start.size());
    clear_hostiles(w);
    place(w, HQ, field);
    w.stock.ammo_t = 6;
    for (int i = 0; i < 120 && w.route.state != CV_OUTBOUND; ++i) w.tick();
    for (int i = 0; i < 5; ++i) w.tick();
    const double cargo0 = w.cargo_t(), tot0 = ammo_total(w);
    w.drain_events();
    w.units[C].elements -= 3;
    w.convoy_hit(w.units[C], 3);
    std::printf("     cargo %.1f t -> %.1f t\n", cargo0, w.cargo_t());
    check(near(w.cargo_t(), cargo0 * 0.5) && near(tot0 - ammo_total(w), w.route.ammo_t), "3 of 6 trucks lost: half the cargo gone");
    check(w.column_capacity_t() == 3 * TRUCK_LOAD_T, "capacity shrinks with the trucks");
    check(count_events(w, "Supply column loses 3 trucks") == 1, "loss reported");

    std::printf("Standing order: turn back under fire:\n");
    w.set_convoy_turn_back(true);
    w.units[C].last_targeted = w.time_min;
    w.tick();
    check(w.route.state == CV_RETURNING && count_events(w, "turning back") == 1, "column under fire turns back");
    for (int i = 0; i < 4 * 60 && w.route.state != CV_PARKED; ++i) w.tick();
    check(w.route.state == CV_PARKED && w.cargo_t() == 0, "cargo back in the depot");

    std::printf("Route stopped:\n");
    w.load_state(start.data(), start.size());
    clear_hostiles(w);
    place(w, HQ, field);
    w.stock.ammo_t = 6;
    const double tot1 = ammo_total(w);
    for (int i = 0; i < 120 && w.route.state != CV_OUTBOUND; ++i) w.tick();
    for (int i = 0; i < 5; ++i) w.tick();
    w.set_supply_route(false);
    check(w.route.state == CV_RETURNING, "recalled column turns round");
    for (int i = 0; i < 4 * 60 && w.route.state != CV_PARKED; ++i) w.tick();
    ticks(w, 120);
    check(w.route.state == CV_PARKED && near(ammo_total(w), tot1) && w.route.trips == 0, "parked, nothing lost, nothing sent");
    std::printf("     %s\n", w.convoy_status().c_str());

    std::printf("Save / load mid-trip:\n");
    w.set_supply_route(true);
    for (int i = 0; i < 120 && w.route.state != CV_OUTBOUND; ++i) w.tick();
    ticks(w, 7);
    const std::vector<uint8_t> mid = w.save_state();
    ticks(w, 240);
    const std::vector<uint8_t> a = w.save_state();
    std::string err;
    check(w.load_state(mid.data(), mid.size(), &err), "load ok");
    ticks(w, 240);
    check(a == w.save_state(), "replay after load is identical");
    std::vector<uint8_t> old = mid;
    old[7] = '3';
    check(!w.load_state(old.data(), old.size(), &err) && err.find("older version") != std::string::npos,
          "a save from the previous version is refused politely");

    std::printf("Column keeps out of a depot the enemy holds:\n");
    w.load_state(start.data(), start.size());
    check(w.units[C].roe == ROE_HOLD, "drivers hold their fire (no giveaway)");
    {
        int foe = -1;
        for (auto &u : w.units)
            if (u.side != SIDE_PLAYER && u.alive && foe < 0) foe = u.id;
        for (auto &u : w.units)
            if (u.side != SIDE_PLAYER && u.id != foe) u.alive = false;
        for (auto &u : w.units) // the company is out in the field: nobody at home shoots the intruder
            if (u.side == SIDE_PLAYER && u.type != U_SUPPLY) place(w, u.id, field);
        place(w, C, land_hex_at(w, home, 10)); // broke contact and ended up away from its depot
        place(w, foe, land_hex_at(w, home, 2));
        w.units[foe].ai_next = 1 << 30;
        w.units[foe].seen_until[SIDE_PLAYER] = w.time_min + 100000;
        w.units[foe].last_seen[SIDE_PLAYER] = w.time_min;
        w.units[foe].last_seen_pos[SIDE_PLAYER] = w.units[foe].pos;
        w.route.until = w.time_min + 6; // after the next 5-minute spatial index rebuild (we teleported units)
        ticks(w, 30);
        check(!w.units[C].moving() && hex_distance(w.units[C].pos, home) == 10, "waits while known enemies are at the depot");
        w.units[foe].alive = false;
        for (int i = 0; i < 3 * 60 && hex_distance(w.units[C].pos, home) > SUPPLY_RADIUS; ++i) w.tick();
        check(hex_distance(w.units[C].pos, home) <= SUPPLY_RADIUS, "drives back once they are gone");
        for (int i = 0; i < 60 && w.route.state != CV_PARKED; ++i) w.tick();
        // The enemy comes to the depot while the column is parked there: it drives out and waits.
        // The enemy comes to the depot while the column is parked there: it drives out and waits.
        // (The enemy is pinned to its hex: it would otherwise chase the column or fall back.)
        const HexCoord spot = land_hex_at(w, home, 2);
        w.units[foe].alive = true;
        auto pin = [&]() {
            place(w, foe, spot);
            w.units[foe].pending.active = false;
        };
        pin();
        w.drain_events();
        for (int i = 0; i < 20 && !w.units[C].moving(); ++i) {
            w.tick();
            pin();
        }
        check(w.units[C].moving() && w.route.state == CV_RETURNING && count_events(w, "drives out of") == 1,
              "a parked column drives out when the enemy comes to the depot");
        for (int i = 0; i < 90; ++i) {
            w.tick();
            pin();
        }
        std::printf("     %d hexes from the depot: %s\n", hex_distance(w.units[C].pos, home), w.convoy_status().c_str());
        check(hex_distance(w.units[C].pos, home) > 6, "and waits more than 3 km away");
    }

    std::printf("Depot raid:\n");
    w.load_state(start.data(), start.size());
    int raider = -1;
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER && u.alive && raider < 0) raider = u.id;
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER && u.id != raider) u.alive = false;
    for (auto &u : w.units)
        if (u.side == SIDE_PLAYER && u.type != U_SUPPLY) place(w, u.id, land_hex_at(w, home, 12));
    place(w, raider, w.holdings[0].pos);
    w.units[raider].pending.active = false;
    w.units[raider].ai_next = 1 << 30;
    const double d0 = w.holdings[0].diesel_l, a0 = w.holdings[0].ammo_t;
    w.drain_events();
    run_to_next_logistics(w);
    check(near(w.holdings[0].ammo_t, a0 * (1 - RAID_SHARE)) && near(w.holdings[0].diesel_l, d0 * (1 - RAID_SHARE)),
          "unguarded depot loses a quarter of its stores in an hour");
    check(count_events(w, "plundering the unguarded depot") == 1 && w.seen_by(w.units[raider], SIDE_PLAYER),
          "raid reported, raiders known");
    place(w, TANK1, land_hex_at(w, home, 2));
    const double a1 = w.holdings[0].ammo_t;
    run_to_next_logistics(w);
    check(near(w.holdings[0].ammo_t, a1), "a guard stops the plundering");

    std::printf("Wages:\n");
    w.load_state(start.data(), start.size());
    clear_hostiles(w);
    run_to_next_logistics(w);
    const double bill = w.weekly_bill();
    std::printf("     weekly bill %.0f for %d men\n", bill, w.stock.personnel);
    check(near(bill, w.stock.personnel * WAGE_PER_MAN_WEEK) && bill > 0, "50 dollars a man a week");
    w.stock.next_payday = w.time_min + 60;
    w.stock.cash = bill + 100;
    for (auto &u : w.units) u.morale = 60;
    w.drain_events();
    run_to_next_logistics(w);
    check(near(w.stock.cash, 100) && w.stock.missed_paydays == 0 && count_events(w, "Pay day: $") == 1, "paid in full");
    check(w.units[TANK1].morale > 62.9f && w.stock.next_payday == w.time_min + 7 * 1440, "morale +3, next pay day in a week");
    w.stock.next_payday = w.time_min + 60;
    w.stock.cash = bill * 0.5;
    const float m0 = w.units[TANK1].morale;
    run_to_next_logistics(w);
    std::printf("     half pay: morale %.1f -> %.1f\n", m0, w.units[TANK1].morale);
    check(w.stock.cash == 0 && w.stock.missed_paydays == 1 && count_events(w, "could be paid") == 1, "short pay reported");
    // Half pay costs 7.5 morale; the hour's ordinary recovery (60 x 0.03) gives some back.
    check(std::fabs(w.units[TANK1].morale - (m0 - 7.5f + 60 * 0.03f)) < 0.05f, "unpaid men lose heart");

    std::printf("Contract payment:\n");
    w.load_state(start.data(), start.size());
    const double cash0 = w.stock.cash;
    for (int id : w.formations[w.strike_formation()].members) {
        w.units[id].alive = false;
        w.units[id].strength = 0;
    }
    ticks(w, 5);
    check(w.outcome() == OUT_VICTORY && near(w.stock.cash, cash0 + CONTRACT_PAYMENT), "fulfilled contract pays on completion");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
