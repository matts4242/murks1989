// Headless check of reinforcements (work board MRK-07): lost vehicles, guns and trucks are not
// bought but asked for, and arrive at the trains two to three days later; they are handed over
// once the unit is there, with fresh crews mixed in; one request a unit at a time; infantry
// (whose men are the replacements) and hostiles cannot ask; a unit wiped out in the meantime
// loses the request; the request is saved, an older save is refused, and a replay after loading
// is byte-identical. Exit code 0 = all passed. Usage: reinforcements_test
#include "murks/sim.hpp"

#include <algorithm>
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

static void place(World &w, int id, HexCoord h) {
    Unit &u = w.units[id];
    u.pos = u.prev_pos = u.dest = h;
    u.path.clear();
    u.path_i = 0;
    u.pending.active = false;
    u.order = O_HOLD;
}

static void knock_out(World &w, int id, int n) {
    Unit &u = w.units[id];
    u.elements -= n;
    u.strength = 100.0f * u.elements / u.elements_max;
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

// A scenario with no fighting in the way: the hostiles are gone and the company has had time to
// settle (nobody fired or was hit in the last 30 minutes).
static World quiet_world() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0);
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER) u.alive = false;
    ticks(w, 40);
    w.drain_events();
    return w;
}

int main() {
    // Order of battle: 0 HQ, 1 1st Tank, 2 2nd Tank, 3 1st Mech, ..., 6 1st Rifle.
    const int HQ = 0, TANK1 = 1, TANK2 = 2, MECH1 = 3, RIFLE1 = 6;

    std::printf("Who can ask, and for what:\n");
    {
        World w = quiet_world();
        check(w.units[TANK1].elements_max >= 3, "a tank platoon has several tanks");
        check(w.vehicles_short(w.units[TANK1]) == 0, "nothing is short at full strength");
        std::string why;
        check(w.request_reinforcements(TANK1, &why) < 0 && !why.empty(), "nothing missing: nothing to ask for");
        knock_out(w, TANK1, 2);
        check(w.vehicles_short(w.units[TANK1]) == 2, "two tanks lost: two short");
        knock_out(w, RIFLE1, 1);
        check(w.vehicles_short(w.units[RIFLE1]) == 0 && w.request_reinforcements(RIFLE1) < 0, "infantry gets men, not vehicles: the replacements");
        const int carrier_squads = w.units[MECH1].dismounts;
        check(carrier_squads >= 0 && w.vehicles_short(w.units[carrier_squads]) == 0, "an APC platoon's squads are men too");
        knock_out(w, MECH1, 1);
        check(w.vehicles_short(w.units[MECH1]) == 1, "but its carriers are vehicles");
        Unit h; // a hostile unit
        for (const auto &u : w.units) if (u.side != SIDE_PLAYER) { h = u; break; }
        check(w.vehicles_short(h) == 0, "the hostiles are not part of the company");
        check(w.request_reinforcements(-1) < 0 && w.request_reinforcements(9999) < 0, "no such unit: refused");
    }

    std::printf("The wait:\n");
    {
        World w = quiet_world();
        knock_out(w, TANK1, 2);
        const double cash = w.stock.cash;
        const int replacements = w.stock.replacements;
        const int d = w.request_reinforcements(TANK1);
        std::printf("     asked at minute %lld: %d min (%.1f days)\n", (long long)w.time_min, d, d / 1440.0);
        check(d >= REINF_MIN_DELAY_MIN && d <= REINF_MIN_DELAY_MIN + REINF_EXTRA_DELAY_MIN, "between two and three days");
        check(w.stock.cash == cash && w.stock.replacements == replacements, "it costs nothing: only the wait");
        check(w.vehicles_short(w.units[TANK1]) == 0, "while it is on the way the unit is not 'short' again");
        const Reinforcement *rq = w.reinforcement_for(TANK1);
        check(rq && rq->count == 2 && rq->due == w.time_min + d, "the request holds the count and the due time");
        check(w.request_reinforcements(TANK1) < 0, "one request at a time");
        check(count_events(w, "requested from the rear") == 1, "the request is reported");
        // The delay is drawn: many requests spread over the whole span, none outside it.
        int lo = 1 << 30, hi = 0;
        for (int seed = 1; seed <= 60; ++seed) {
            World v = quiet_world();
            v.reseed_rng(seed);
            knock_out(v, TANK1, 1);
            const int x = v.request_reinforcements(TANK1);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
        std::printf("     60 requests: %.2f to %.2f days\n", lo / 1440.0, hi / 1440.0);
        check(lo >= 2 * 1440 && hi <= 3 * 1440 && hi - lo > 700, "60 requests spread over the two to three days, none outside");
    }

    std::printf("Arrival at the trains:\n");
    {
        World w = quiet_world();
        place(w, TANK1, w.units[HQ].pos);
        knock_out(w, TANK1, 2);
        w.units[TANK1].morale = 90;
        w.units[TANK1].fuel = 50;
        const int before = w.units[TANK1].elements;
        const int d = w.request_reinforcements(TANK1);
        ticks(w, d - 60);
        check(w.units[TANK1].elements == before, "nothing arrives early");
        ticks(w, 61 + 60);
        const Unit &u = w.units[TANK1];
        std::printf("     after %d min: %d of %d, morale %.1f, fuel %.1f\n", d + 61, u.elements, u.elements_max, u.morale, u.fuel);
        check(u.elements == u.elements_max, "both tanks joined once due and at the trains");
        check(std::fabs(u.strength - 100.0f) < 0.01f, "strength follows the vehicles");
        check(u.morale < 90.0f && u.morale > 75.0f, "the fresh crews' morale (70) mixes in");
        check(u.fuel > 50.0f, "the new vehicles come with fuel");
        check(w.reinforcement_for(TANK1) == nullptr, "the request is done");
        check(count_events(w, "reinforcement") >= 1, "the arrival is reported");
    }
    {
        World w = quiet_world();
        const HexCoord far{w.units[HQ].pos.col + 40, w.units[HQ].pos.row};
        place(w, TANK2, far);
        knock_out(w, TANK2, 1);
        const int d = w.request_reinforcements(TANK2);
        ticks(w, d + 130);
        check(w.units[TANK2].elements == w.units[TANK2].elements_max - 1 && w.reinforcement_for(TANK2), "away from the trains the reinforcements wait for the unit");
        place(w, TANK2, w.units[HQ].pos);
        ticks(w, 40 + 60);
        check(w.units[TANK2].elements == w.units[TANK2].elements_max && !w.reinforcement_for(TANK2), "and join when it comes in and has stood out of the fight");
    }
    {
        World w = quiet_world();
        place(w, TANK1, w.units[HQ].pos);
        knock_out(w, TANK1, 2);
        const int d = w.request_reinforcements(TANK1);
        w.units[TANK1].elements = w.units[TANK1].elements_max - 1; // a tank came back some other way
        ticks(w, d + 130);
        check(w.units[TANK1].elements == w.units[TANK1].elements_max, "only what is still missing is delivered");
    }
    {
        World w = quiet_world();
        place(w, TANK1, w.units[HQ].pos);
        knock_out(w, TANK1, 2);
        const int d = w.request_reinforcements(TANK1);
        w.units[TANK1].alive = false;
        w.units[TANK1].elements = 0;
        w.drain_events();
        ticks(w, d + 130);
        check(!w.units[TANK1].alive && w.reinforcement_for(TANK1) == nullptr, "a platoon wiped out in the meantime is not raised again");
        check(count_events(w, "no unit to go to") == 1, "and the request is closed with a report");
    }

    std::printf("Save and load:\n");
    {
        World a = quiet_world();
        place(a, TANK1, a.units[HQ].pos);
        knock_out(a, TANK1, 2);
        const int d = a.request_reinforcements(TANK1);
        ticks(a, 600);
        const std::vector<uint8_t> bytes = a.save_state();
        World b = quiet_world();
        std::string err;
        check(b.load_state(bytes.data(), bytes.size(), &err), "a save with a request loads");
        const Reinforcement *rq = b.reinforcement_for(TANK1);
        check(rq && rq->count == 2 && rq->due == a.reinforcement_for(TANK1)->due, "the request is saved");
        for (int i = 0; i < d + 200; ++i) {
            a.tick();
            b.tick();
        }
        check(a.units[TANK1].elements == a.units[TANK1].elements_max && b.units[TANK1].elements == b.units[TANK1].elements_max, "both received the reinforcements");
        check(a.save_state() == b.save_state(), "the replay after loading is byte-identical");
        std::vector<uint8_t> old = bytes;
        old[7] = 'C'; // v12's magic
        World c = quiet_world();
        check(!c.load_state(old.data(), old.size(), &err), "a v12 save is refused rather than misread");
        std::vector<uint8_t> bad = bytes;
        bad.resize(bad.size() - 5);
        check(!c.load_state(bad.data(), bad.size(), &err), "a truncated save is refused");
        World n = quiet_world();
        n.setup_scenario(0);
        check(n.reinforcements.empty(), "a new game starts with no requests");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
