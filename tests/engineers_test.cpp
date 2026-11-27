// Headless check of engineers: an own town with two militia squads gives them up to a 24-hour
// course that turns out an engineer platoon (or breaks up if the town is lost); engineers halted
// in a hex build a field garage or a supply depot there in 24 hours, and moving off stops the
// work; requested reinforcements join a unit at a garage as at the trains; units at a depot
// resupply from its stores and supply routes can carry to it; an enemy in the hex of a garage or
// depot with nobody of ours wrecks it; and all of it is saved (v13 saves are refused) and
// replays byte-identically after a load. Exit code 0 = all passed. Usage: engineers_test
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

static int count_events(World &w, const std::string &needle) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) ++n;
    return n;
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

static void place(World &w, int id, HexCoord h) {
    Unit &u = w.units[id];
    u.pos = u.prev_pos = u.dest = h;
    u.path.clear();
    u.path_i = 0;
    u.pending.active = false;
    u.order = O_HOLD;
}

// The nearest land hex to `h` that is not inside any place.
static HexCoord open_land(const World &w, HexCoord h) {
    for (int r = 0; r < 12; ++r)
        for (int dc = -r; dc <= r; ++dc)
            for (int dr = -r; dr <= r; ++dr) {
                HexCoord n{h.col + dc, h.row + dr};
                if (w.map.in_bounds(n) && w.map.at(n).terrain != T_WATER && w.holding_at(n) < 0) return n;
            }
    return h;
}

static int find_type(const World &w, int side, UnitType t) {
    for (const auto &u : w.units)
        if (u.alive && u.side == side && u.type == t && !u.militia) return u.id;
    return -1;
}

// The company at home with no enemy about, and the home town's militia grown to two squads.
static World with_militia() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0);
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER) u.alive = false;
    Holding &home = w.holdings[0];
    home.loyalty = 70;
    home.manpower = 100;
    w.set_training(0, true);
    for (int i = 0; i < 7 * 1440 && w.militia_squads(w.holdings[0]) < 2; ++i) w.tick();
    w.set_training(0, false);
    w.drain_events();
    return w;
}

int main() {
    const int HOME = 0;
    World base = with_militia();
    std::printf("Training engineers:\n");
    {
        World w = base;
        check(w.militia_squads(w.holdings[HOME]) >= 2, "the home town has two militia squads to give");
        const int militia_before = w.militia_squads(w.holdings[HOME]);
        check(w.train_engineers(HOME).empty(), "an engineer course starts");
        check(w.militia_squads(w.holdings[HOME]) == militia_before - ENGINEER_MILITIA_SQUADS, "it takes two squads off the garrison");
        check(!w.train_engineers(HOME).empty(), "one course at a time");
        check(!w.train_engineers(9999).empty(), "no such place: refused");
        const int units_before = (int)w.units.size();
        ticks(w, ENGINEER_TRAIN_MIN - 61);
        check(find_type(w, SIDE_PLAYER, U_ENGINEER) < 0, "nothing before the day is out");
        ticks(w, 61 + 60);
        const int eng = find_type(w, SIDE_PLAYER, U_ENGINEER);
        check(eng >= units_before && w.units[eng].name == "1st Engineers", "after 24 hours 1st Engineers joins");
        check(eng >= 0 && w.units[eng].elements == 2 && unit_type_info(U_ENGINEER).mobility == M_WHEELED, "2 squads on trucks, wheeled");
        check(eng >= 0 && hex_distance(w.units[eng].pos, w.holdings[HOME].pos) <= 2, "it appears at the town");
        check(eng >= 0 && w.orderable(w.units[eng]) && !w.units[eng].militia, "and takes orders like any platoon of the company");
        check(count_events(w, "engineer platoon joins") == 1, "reported");
    }
    {
        World w = base;
        World none = base;
        none.holdings[HOME].militia_unit = -1; // a town with no militia
        check(!none.train_engineers(HOME).empty(), "a town without two militia squads cannot train them");
        w.train_engineers(HOME);
        w.holdings[HOME].owner = SIDE_HOSTILE; // the town is lost during the course
        ticks(w, ENGINEER_TRAIN_MIN + 60);
        check(find_type(w, SIDE_PLAYER, U_ENGINEER) < 0 && count_events(w, "broken up") == 1, "losing the town breaks the course up");
    }

    // Engineers ready to work, away from the town.
    World eng_world = base;
    eng_world.train_engineers(HOME);
    ticks(eng_world, ENGINEER_TRAIN_MIN + 60);
    const int ENG = find_type(eng_world, SIDE_PLAYER, U_ENGINEER);
    const int HQ = find_type(eng_world, SIDE_PLAYER, U_HQ);
    const int TANK = find_type(eng_world, SIDE_PLAYER, U_TANK);
    const HexCoord site = open_land(eng_world, {eng_world.units[HQ].pos.col + 30, eng_world.units[HQ].pos.row});
    place(eng_world, ENG, site);
    eng_world.drain_events();

    std::printf("Building:\n");
    {
        World w = eng_world;
        check(!w.build(TANK, HK_GARAGE).empty(), "only engineers build");
        check(!w.build(ENG, HK_TOWN).empty(), "they build garages and depots, not towns");
        World intown = w;
        place(intown, ENG, intown.holdings[HOME].pos);
        check(!intown.build(ENG, HK_GARAGE).empty(), "not inside an existing place");
        check(w.build(ENG, HK_GARAGE).empty(), "a garage is started");
        check(!w.build(ENG, HK_DEPOT).empty(), "one thing at a time");
        const size_t places = w.holdings.size();
        ticks(w, ENGINEER_BUILD_MIN - 61);
        check(w.holdings.size() == places, "not done early");
        ticks(w, 61 + 60);
        check(w.holdings.size() == places + 1, "after 24 hours there is a new place");
        const Holding &g = w.holdings.back();
        check(g.kind == HK_GARAGE && g.owner == SIDE_PLAYER && g.pos == site && w.holding_at(site) == (int)places, "a field garage of the company's, where they stood");
        check(count_events(w, "has finished a field garage") == 1, "reported");
        check(w.units[ENG].build_kind == 0, "the engineers are free again");
    }
    {
        World w = eng_world;
        w.build(ENG, HK_DEPOT);
        ticks(w, 300);
        w.order_move(ENG, {site.col + 6, site.row});
        ticks(w, 120);
        check(w.units[ENG].build_kind == 0 && count_events(w, "work is abandoned") == 1, "moving off abandons the work");
        ticks(w, ENGINEER_BUILD_MIN);
        check(w.holding_at(site) < 0, "and nothing is built");
    }

    // A garage and a depot built by the engineers.
    World built = eng_world;
    built.build(ENG, HK_GARAGE);
    ticks(built, ENGINEER_BUILD_MIN + 60);
    const int GARAGE = (int)built.holdings.size() - 1;
    const HexCoord site2 = open_land(built, {site.col, site.row + 8});
    place(built, ENG, site2);
    built.build(ENG, HK_DEPOT);
    ticks(built, ENGINEER_BUILD_MIN + 60);
    const int DEPOT = (int)built.holdings.size() - 1;
    built.drain_events();

    std::printf("The garage:\n");
    {
        check(built.holdings[GARAGE].kind == HK_GARAGE && built.holdings[DEPOT].kind == HK_DEPOT, "a garage and a depot stand");
        World w = built, ctl = built;
        for (World *x : {&w, &ctl}) {
            Unit &t = x->units[TANK];
            t.elements -= 1;
            t.strength = 100.0f * t.elements / t.elements_max;
        }
        place(w, TANK, built.holdings[GARAGE].pos);
        place(ctl, TANK, open_land(ctl, {site.col - 8, site.row - 8})); // away from both trains and garage
        const int d = w.request_reinforcements(TANK);
        ctl.request_reinforcements(TANK);
        check(d > 0 && !w.at_trains(w.units[TANK]) && w.at_garage(w.units[TANK]), "a tank short of a vehicle stands at the garage, far from the trains");
        ticks(w, d + 130);
        ticks(ctl, d + 130);
        check(w.units[TANK].elements == w.units[TANK].elements_max, "its reinforcement joins at the garage");
        check(ctl.units[TANK].elements < ctl.units[TANK].elements_max, "away from trains and garage it waits");
    }

    std::printf("The depot:\n");
    {
        World w = built;
        Holding &dp = w.holdings[DEPOT];
        dp.diesel_l = 5000;
        dp.ammo_t = 5;
        place(w, TANK, dp.pos);
        w.units[TANK].fuel = 40;
        w.units[TANK].ammo = 40;
        const double stock_l = w.stock.diesel_l, stock_t = w.stock.ammo_t;
        ticks(w, 120);
        check(w.depot_at(w.units[TANK]) == DEPOT && !w.at_trains(w.units[TANK]), "a tank stands at the depot, far from the trains");
        check(w.units[TANK].fuel > 40.0f && w.units[TANK].ammo > 40.0f, "it refuels and rearms there");
        check(w.holdings[DEPOT].diesel_l < 5000 && w.holdings[DEPOT].ammo_t < 5, "from the depot's own stores");
        check(w.stock.diesel_l == stock_l && w.stock.ammo_t == stock_t, "not from the company's");
        std::string err;
        check(w.set_supply_route(HOME, DEPOT, &err) >= 0, "a supply route can run to the depot");
    }

    std::printf("Wrecking:\n");
    {
        World w = built;
        int foe = -1;
        for (const auto &u : w.units)
            if (u.side == SIDE_HOSTILE && u.type == U_TANK) { foe = u.id; break; }
        Unit &f = w.units[foe];
        f.alive = true;
        f.elements = f.elements_max;
        f.ai_next = 1ll << 40;
        f.roe = ROE_HOLD;
        place(w, foe, w.holdings[DEPOT].pos);
        place(w, ENG, open_land(w, {site.col - 12, site.row})); // the builders have moved on
        World guarded = w;
        place(guarded, TANK, guarded.holdings[DEPOT].pos);
        ticks(w, 61);
        ticks(guarded, 61);
        check(w.holdings[DEPOT].wrecked && w.holdings[DEPOT].owner < 0 && w.holding_at(w.holdings[DEPOT].pos) < 0, "an enemy in the depot's hex wrecks it");
        check(count_events(w, "wrecked") == 1, "reported");
        check(!guarded.holdings[DEPOT].wrecked, "not while a unit of ours holds the hex");
        std::string err;
        check(w.set_supply_route(HOME, DEPOT, &err) < 0, "no route to a wrecked depot");
    }

    std::printf("Save and load:\n");
    {
        World a = base;
        a.train_engineers(HOME);
        World b0 = built;
        b0.build(ENG, HK_GARAGE); // ENG stands at site2, inside the depot: refused
        place(b0, ENG, open_land(b0, {site.col + 10, site.row + 10}));
        check(b0.build(ENG, HK_GARAGE).empty(), "a second garage is started elsewhere");
        ticks(b0, 300);
        const std::vector<uint8_t> bytes = b0.save_state();
        World c = base;
        std::string err;
        const bool loaded = c.load_state(bytes.data(), bytes.size(), &err);
        if (!loaded) std::printf("     load error: %s\n", err.c_str());
        check(loaded, "a save with engineers at work loads");
        check(c.units[ENG].build_kind == HK_GARAGE && c.units[ENG].build_done == b0.units[ENG].build_done, "the work in progress is saved");
        check(c.holdings[GARAGE].kind == HK_GARAGE && c.holdings[DEPOT].kind == HK_DEPOT, "the garage and depot are saved");
        const std::vector<uint8_t> course = a.save_state();
        World d = base;
        check(d.load_state(course.data(), course.size(), &err) && d.holdings[HOME].engineers_ready == a.holdings[HOME].engineers_ready, "a course under way is saved");
        for (int i = 0; i < ENGINEER_BUILD_MIN + 60; ++i) {
            b0.tick();
            c.tick();
        }
        check(b0.save_state() == c.save_state(), "the replay after loading is byte-identical");
        std::vector<uint8_t> old = bytes;
        old[7] = 'D'; // v13's magic
        check(!d.load_state(old.data(), old.size(), &err), "a v13 save is refused");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
