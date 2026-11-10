// Headless check of holdings, militia and supply routes (Work Board MRK-08 / MRK-09): the home
// town starts as the company's, places change hands after two hours of unopposed presence
// (militia block that), loyalty scales the daily yield, the trains draw from a depot they park
// at, requisitions move goods and cost loyalty, a town raises militia that takes no orders and
// counts for nothing, and a supply column shuttles goods from a depot to the trains, loses cargo
// with its trucks, suspends the route when destroyed, and comes home when the route is cancelled.
// Save/load stays deterministic through all of it. Exit code 0 = all passed.
// Usage: holdings_test
#include "murks/sim.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

static void kill(World &w, int id) {
    Unit &u = w.units[id];
    u.alive = false;
    u.elements = 0;
    u.strength = 0;
    u.last_hit = w.time_min;
    u.path.clear();
    u.path_i = 0;
    u.pending.active = false;
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

static void run_to_next_logistics(World &w) {
    do w.tick();
    while (w.time_min % 60 != 0);
}

// Puts a unit down, halted, at a hex.
static void place(World &w, int id, HexCoord h) {
    Unit &u = w.units[id];
    u.pos = u.prev_pos = u.dest = h;
    u.path.clear();
    u.path_i = 0;
    u.pending.active = false;
    u.order = O_HOLD;
}

static int count_events(World &w, const std::string &needle, bool *paused = nullptr) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) {
            ++n;
            if (paused) *paused = e.pause_worthy;
        }
    return n;
}

static int find_holding(const World &w, int kind, int owner) {
    for (int i = 0; i < (int)w.holdings.size(); ++i)
        if (w.holdings[i].kind == kind && w.holdings[i].owner == owner) return i;
    return -1;
}

static int find_unit(const World &w, const std::string &name) {
    for (const auto &u : w.units)
        if (u.name == name) return u.id;
    return -1;
}

// A hex of open land next to the place (for parking the trains beside a depot).
static HexCoord beside(const World &w, HexCoord h) {
    for (int d = 0; d < 6; ++d) {
        HexCoord n = neighbor(h, d);
        if (w.map.in_bounds(n) && w.map.at(n).terrain != T_WATER) return n;
    }
    return h;
}

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0); // 14 April 1989, 06:00
    const int HQ = 0, TANK1 = 1, TANK2 = 2;
    const int TRUCK = find_unit(w, "Supply Column");
    const int HOME = 0;

    std::printf("Setup:\n");
    check(!w.holdings.empty() && w.holdings[HOME].owner == SIDE_PLAYER && w.holdings[HOME].kind == HK_TOWN,
          "the home town is the company's");
    check(std::fabs(w.holdings[HOME].loyalty - 50.0f) < 0.01f, "home loyalty starts at 50");
    const int FUEL = find_holding(w, HK_FUEL_DEPOT, -1), AMMO = find_holding(w, HK_AMMO_DUMP, -1);
    const int FARM = find_holding(w, HK_FARM, -1);
    check(FUEL >= 0 && AMMO >= 0 && FARM >= 0, "a fuel depot, an ammo dump and a farm collective exist, independent");
    check(FUEL >= 0 && std::fabs(w.holdings[FUEL].diesel_l - 40000.0) < 0.01, "the fuel depot holds 40,000 L");
    check(AMMO >= 0 && std::fabs(w.holdings[AMMO].ammo_t - 60.0) < 0.01, "the ammo dump holds 60 t");
    int other_town = -1;
    for (int i = 1; i < (int)w.holdings.size(); ++i)
        if (w.holdings[i].kind == HK_TOWN && w.holdings[i].owner < 0 && w.holdings[i].population >= 10000) {
            other_town = i;
            break;
        }
    check(other_town >= 0, "an independent town of 10,000+ exists");
    check(TRUCK >= 0 && w.units[TRUCK].type == U_TRUCK && w.units[TRUCK].elements == 6,
          "the company has a supply column of 6 trucks");
    check(TRUCK >= 0 && w.orderable(w.units[TRUCK]), "an idle column takes orders");
    const int combat_at_start = w.effective_combat_units(0);
    int platoons = 0;
    for (const auto &u : w.units)
        if (u.alive && u.side == SIDE_PLAYER && u.carrier < 0 &&
            (u.type == U_TANK || u.type == U_MECH || u.type == U_INFANTRY || u.type == U_RECON))
            ++platoons;
    check(combat_at_start == platoons, "the column does not count as a fighting platoon");
    check(w.holding_at(w.holdings[HOME].pos) == HOME, "holding_at finds the home town");
    if (TRUCK < 0 || other_town < 0 || FUEL < 0) return 1;
    const std::vector<uint8_t> start = w.save_state();

    // Isolate: the hostile strike group stays out of these checks except where it is used.
    auto hostiles = [&](bool alive) {
        for (auto &u : w.units)
            if (u.side == SIDE_HOSTILE) {
                if (!alive && u.alive) kill(w, u.id);
            }
    };
    hostiles(false);
    // Nothing of the company's moves on its own; park the company in the open away from any town.
    for (auto &u : w.units)
        if (u.side == SIDE_PLAYER) u.roe = ROE_HOLD;

    std::printf("Capture (one side alone in a place for %d min):\n", CAPTURE_MIN);
    {
        Holding &t = w.holdings[other_town];
        place(w, TANK1, t.pos);
        run_to_next_logistics(w);
        check(t.contested_since >= 0 && t.owner < 0, "presence noted at the first hourly pass; not taken yet");
        ticks(w, 60);
        check(t.owner < 0, "still not taken after one hour");
        ticks(w, 60);
        bool paused = false;
        check(t.owner == SIDE_PLAYER, "taken after two hours of unopposed presence");
        check(t.loyalty >= 20.0f && t.loyalty < 21.0f, "loyalty of a taken town is 20 (plus the hour's drift)");
        check(count_events(w, "Company Murks takes " + t.name, &paused) == 1 && paused, "capture is a pause-worthy event");
        check(t.known_owner == SIDE_PLAYER, "the player knows it is theirs");
    }

    std::printf("Hostile forces take an undefended player town:\n");
    {
        Holding &t = w.holdings[other_town];
        place(w, TANK1, w.holdings[HOME].pos); // the platoon leaves
        // One hostile tank platoon, revived, sits in the town with its guns silent.
        int h = -1;
        for (auto &u : w.units)
            if (u.side == SIDE_HOSTILE && u.type == U_TANK) {
                h = u.id;
                break;
            }
        Unit &e = w.units[h];
        e.alive = true;
        e.elements = e.elements_max;
        e.strength = 100;
        e.roe = ROE_HOLD;
        e.ai_next = w.time_min + 100000; // no AI moves
        place(w, h, t.pos);
        ticks(w, 180);
        bool paused = false;
        check(t.owner == SIDE_HOSTILE, "the town is hostile after two hours");
        check(count_events(w, t.name + " has been taken by hostile forces", &paused) == 1 && paused,
              "loss of a holding is a pause-worthy event");
        kill(w, h);
    }

    std::printf("Daily yield scales with loyalty:\n");
    {
        Holding &home = w.holdings[HOME];
        double c50, f50, c100, f100;
        home.loyalty = 50;
        w.daily_yield(home, c50, f50);
        home.loyalty = 100;
        w.daily_yield(home, c100, f100);
        check(c50 > 0 && std::fabs(c100 - 2 * c50) < 1e-6 && std::fabs(f100 - 2 * f50) < 1e-6,
              "cash and food at loyalty 100 are twice those at 50");
        check(std::fabs(c100 - home.population * 0.5) < 1e-6, "0.5 DM a head at full loyalty");
        home.loyalty = 50;
        const double cash_before = home.cash, food_before = home.food_kg;
        // To the next 06:00 (the holdings' day).
        while (w.time_min % 1440 != 6 * 60) w.tick();
        // Loyalty drifts up a little with the company at home, so the yield is a shade over c50.
        check(home.cash - cash_before >= c50 - 1e-3 && home.cash - cash_before < c50 * 1.05,
              "taxes land in the town's stock at 06:00");
        check(home.food_kg > food_before, "and so does the food surplus");
        check(count_events(w, "The holdings' day") == 1, "the day is reported");
    }

    std::printf("The trains at a depot:\n");
    {
        Holding &d = w.holdings[FUEL];
        d.owner = d.known_owner = SIDE_PLAYER;
        d.loyalty = 40;
        w.stock.diesel_l = 5000;
        place(w, HQ, beside(w, d.pos));
        const double depot_before = d.diesel_l;
        run_to_next_logistics(w);
        check(std::fabs(w.stock.diesel_l - 15000.0) < 0.01, "the trains draw 10,000 L an hour from the depot");
        check(std::fabs(depot_before - d.diesel_l - 10000.0) < 0.01, "which the depot loses");
        run_to_next_logistics(w);
        run_to_next_logistics(w);
        check(std::fabs(w.stock.diesel_l - 30000.0) < 0.01, "and stop at the want level (30,000 L)");
        place(w, HQ, w.holdings[HOME].pos);
    }

    std::printf("Requisition (levy):\n");
    {
        Holding &home = w.holdings[HOME];
        home.loyalty = 50;
        check(!w.levy(other_town, LV_FOOD).empty(), "no levy at a town the company does not hold");
        place(w, HQ, beside(w, beside(w, home.pos)));
        // Nobody halted inside the place (the HQ is two hexes out): refused.
        bool nobody_in = true;
        for (auto &u : w.units)
            if (u.alive && u.side == SIDE_PLAYER && w.in_place(home, u.pos)) nobody_in = false;
        if (nobody_in) check(!w.levy(HOME, LV_FUEL).empty(), "a unit must be halted in the place");
        place(w, HQ, home.pos);
        const double fuel_before = home.diesel_l;
        const float loy_before = home.loyalty;
        check(w.levy(HOME, LV_FUEL).empty(), "fuel levy accepted with the HQ in town");
        check(std::fabs(home.diesel_l - fuel_before - home.population * 0.5) < 0.01, "0.5 L a head into the town's stores");
        check(std::fabs(loy_before - home.loyalty - 10.0f) < 0.01f, "loyalty falls by 10");
        const std::string again = w.levy(HOME, LV_FUEL);
        check(again.find("less than a day ago") != std::string::npos, "a second fuel levy is refused for 24 h");
        const int men = home.manpower;
        check(men > 0 && w.levy(HOME, LV_MEN).empty(), "men levy accepted");
        check(w.stock.replacements == men && home.manpower == 0, "the whole pool joins the company's replacements");
        check(std::fabs(loy_before - home.loyalty - 25.0f) < 0.01f, "men cost 15 loyalty more");
        check(count_events(w, "Requisition in " + home.name) == 2, "both requisitions reported");
        check(!w.levy(FUEL, LV_FOOD).empty(), "nothing to requisition at a depot");
    }

    std::printf("Militia:\n");
    {
        Holding &home = w.holdings[HOME];
        check(!w.set_training(other_town, true), "no militia at a town the company does not hold");
        home.loyalty = 20;
        check(!w.set_training(HOME, true), "no militia under loyalty 25");
        home.loyalty = 40; // under 50: no volunteers join the pool meanwhile
        home.manpower = 40;
        check(w.set_training(HOME, true) && home.training, "training starts");
        check(w.militia_max_squads(home) == std::min(MILITIA_MAX_SQUADS, home.population / MILITIA_PER_POP),
              "one squad per 2,000 people, up to 6");
        const size_t units_before = w.units.size();
        ticks(w, MILITIA_TRAIN_MIN + 60);
        check(w.militia_squads(home) == 1 && w.units.size() == units_before + 1, "a squad of 10 is ready after two days");
        check(count_events(w, "a militia squad of 10 is ready") == 1, "reported");
        const int m = home.militia_unit;
        check(m >= 0 && w.units[m].militia && w.units[m].side == SIDE_PLAYER && w.units[m].type == U_INFANTRY &&
                  w.units[m].pos == home.pos,
              "it is a rifle unit of the company's side standing in the town");
        check(w.units[m].name == home.name + " Militia", "named after the town");
        check(!w.orderable(w.units[m]) && w.order_move(m, beside(w, home.pos)) < 0 && w.order_hold(m) < 0 &&
                  w.set_stance(m, ROE_HOLD, BC_HEAVY) < 0,
              "it takes no orders");
        check(w.effective_combat_units(0) == combat_at_start, "it does not count as a fighting platoon");
        check(home.manpower == 30, "ten men left the pool");
        ticks(w, MILITIA_TRAIN_MIN);
        check(w.militia_squads(home) == 2 && w.units.size() == units_before + 1 && w.units[m].elements == 2,
              "the second squad joins the same unit");
        // Capture is blocked while the militia stands: a hostile platoon in town, guns silent.
        for (auto &u : w.units)
            if (u.alive && u.side == SIDE_PLAYER && !u.militia) place(w, u.id, beside(w, w.holdings[FUEL].pos));
        int h = -1;
        for (auto &u : w.units)
            if (u.side == SIDE_HOSTILE && u.type == U_RECON) {
                h = u.id;
                break;
            }
        Unit &e = w.units[h];
        e.alive = true;
        e.elements = e.elements_max;
        e.strength = 100;
        e.roe = ROE_HOLD;
        e.ai_next = w.time_min + 100000;
        place(w, h, home.pos);
        w.units[m].roe = ROE_HOLD;
        ticks(w, 240);
        check(home.owner == SIDE_PLAYER && home.contested_since < 0, "a garrison blocks capture (four hours, still ours)");
        kill(w, h);
        check(w.acting_hq(0) == HQ, "the HQ still runs the trains");
        kill(w, HQ);
        ticks(w, World::HANDOVER_MIN);
        check(w.acting_hq(0) != m && w.acting_hq(0) >= 0, "militia never takes over the trains");
        w.units[HQ].alive = true;
        w.units[HQ].elements = w.units[HQ].elements_max;
        w.units[HQ].strength = 100;
        check(w.acting_hq(0) == HQ, "HQ back");
        for (auto &u : w.units)
            if (u.alive && u.side == SIDE_PLAYER && !u.militia) place(w, u.id, home.pos);
    }

    std::printf("Save/load with holdings and militia:\n");
    {
        std::vector<uint8_t> s = w.save_state();
        ticks(w, 180);
        std::vector<uint8_t> a = w.save_state();
        std::string err;
        check(w.load_state(s.data(), s.size(), &err), "load ok");
        ticks(w, 180);
        check(a == w.save_state(), "replay after load is identical");
        check(w.militia_squads(w.holdings[HOME]) == 2, "militia survive the load");
    }

    std::printf("Supply route: fuel depot -> the trains (%d hexes):\n", hex_distance(w.holdings[FUEL].pos, w.holdings[HOME].pos));
    {
        Holding &d = w.holdings[FUEL];
        d.diesel_l = 40000;
        w.holdings[HOME].diesel_l = 0; // or the trains at home would draw the town's garages dry instead
        w.stock.diesel_l = 5000;
        Unit &c = w.units[TRUCK];
        c.fuel = 100;
        place(w, TRUCK, beside(w, w.holdings[HOME].pos));
        std::string err;
        check(w.set_supply_route(FUEL, FUEL, &err) < 0, "source and destination must differ");
        check(w.set_supply_route(other_town, -1, &err) < 0 && err.find("does not hold") != std::string::npos,
              "no route from a place the company does not hold");
        const int rid = w.set_supply_route(FUEL, -1, &err);
        check(rid >= 0, ("route set: " + err).c_str());
        if (rid < 0) return 1;
        check(c.convoy_route == rid && c.convoy_state == CV_TO_SOURCE && !w.orderable(c), "the column is on its way to the depot and takes no orders");
        check(w.set_supply_route(FUEL, HOME, &err) < 0 && err.find("no supply column is free") != std::string::npos,
              "no second route without a free column");
        const double depot_before = d.diesel_l, stock_before = w.stock.diesel_l;
        int t = 0;
        for (; t < 2 * 1440 && w.routes[rid].trips < 1; ++t) w.tick();
        const SupplyRoute &r = w.routes[rid];
        std::printf("     first delivery after %dh%02d: %.0f L; round trip %d min\n", t / 60, t % 60, r.delivered_l,
                    r.last_round_trip_min);
        check(r.trips == 1, "a load arrives within two days");
        check(std::fabs(r.delivered_l - 25000.0) < 1.0,
              "the column carries what the trains are short of (25,000 L, under its 27 t)");
        check(std::fabs((w.stock.diesel_l - stock_before) - r.delivered_l) < 400,
              "company diesel rises by the delivery (less the trucks' own tank)");
        check(depot_before - d.diesel_l >= r.delivered_l, "the depot lost the cargo and the trucks' fill-up");
        check(r.last_round_trip_min > 0, "round-trip time recorded");
        check(c.fuel > 90, "the trucks refuelled on the way");
        check(c.convoy_state == CV_TO_SOURCE && r.active, "and go back for more");
        check(count_events(w, "delivered") >= 1, "delivery reported");
        // Half the trucks gone: half the load. (The depot is topped up: trip one took 25,000 L of its 40,000.)
        c.elements = 3;
        c.strength = 50;
        d.diesel_l = 40000;
        w.stock.diesel_l = 1000;
        for (t = 0; t < 2 * 1440 && w.routes[rid].trips < 2; ++t) w.tick();
        check(r.trips == 2, "second trip arrives");
        const double last = w.stock.diesel_l - 1000;
        std::printf("     second delivery: about %.0f L with 3 trucks (29,000 L wanted)\n", last);
        check(last > 15000 && last <= 3 * TRUCK_LOAD_T * DIESEL_L_PER_T + 1, "three trucks carry 13.5 t, not the 29,000 L wanted");
    }

    std::printf("Save/load with a route running:\n");
    {
        ticks(w, 45);
        check(w.units[TRUCK].convoy_route >= 0 && w.units[TRUCK].moving(), "the column is on the road");
        std::vector<uint8_t> s = w.save_state();
        ticks(w, 240);
        std::vector<uint8_t> a = w.save_state();
        std::string err;
        check(w.load_state(s.data(), s.size(), &err), "load ok");
        ticks(w, 240);
        check(a == w.save_state(), "replay after load is identical");
    }

    std::printf("Cancel a route:\n");
    {
        const int rid = w.units[TRUCK].convoy_route;
        check(rid >= 0 && w.cancel_supply_route(rid), "cancelled");
        check(!w.routes[rid].active, "route inactive");
        int t = 0;
        for (; t < 1440 && w.units[TRUCK].convoy_route >= 0; ++t) w.tick();
        check(w.units[TRUCK].convoy_route < 0 && w.units[TRUCK].convoy_state == CV_IDLE, "the column comes back to the trains");
        check(hex_distance(w.units[TRUCK].pos, w.units[w.acting_hq(0)].pos) <= 3, "and parks by the HQ");
        check(count_events(w, "back with the trains") == 1, "reported");
        check(w.orderable(w.units[TRUCK]), "and takes orders again");
    }

    std::printf("A column destroyed on the road:\n");
    {
        std::string err;
        const int rid = w.set_supply_route(FUEL, -1, &err);
        check(rid >= 0, "route set again");
        ticks(w, 30);
        kill(w, TRUCK);
        w.tick();
        bool paused = false;
        check(!w.routes[rid].active && w.routes[rid].convoy < 0 && w.routes[rid].suspended_at >= 0, "the route is suspended");
        check(count_events(w, "suspended", &paused) == 1 && paused, "suspension is a pause-worthy event");
        check(w.free_convoy(SIDE_PLAYER) < 0 && w.set_supply_route(FUEL, -1, &err) < 0, "no column left to run one");
    }

    std::printf("Route between holdings:\n");
    {
        w.load_state(start.data(), start.size());
        hostiles(false);
        Holding &d = w.holdings[FUEL];
        d.owner = d.known_owner = SIDE_PLAYER;
        w.holdings[HOME].diesel_l = 0;
        std::string err;
        const int rid = w.set_supply_route(FUEL, HOME, &err);
        check(rid >= 0, ("route depot -> home town: " + err).c_str());
        int t = 0;
        for (; t < 2 * 1440 && rid >= 0 && w.routes[rid].trips < 1; ++t) w.tick();
        check(rid >= 0 && w.routes[rid].trips == 1 && w.holdings[HOME].diesel_l > 20000,
              "the town's stores fill from the depot");
        // Losing the source ends the route and sends the column home.
        d.owner = SIDE_HOSTILE;
        ticks(w, 5);
        check(!w.routes[rid].active, "a source lost to the enemy ends the route");
        for (t = 0; t < 1440 && w.units[TRUCK].convoy_route >= 0; ++t) w.tick();
        check(w.units[TRUCK].convoy_route < 0, "the column comes home");
    }

    std::printf("Performance:\n");
    {
        w.load_state(start.data(), start.size());
        ticks(w, 2 * 1440);
        std::printf("     2 days with the full scenario: avg %.1f us/tick, max %.1f us\n", w.stats().avg_tick_us, w.stats().max_tick_us);
        check(w.stats().avg_tick_us < 2000, "average tick under 2 ms");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
