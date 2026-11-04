// Headless check of the HQ-loss rules: after the company HQ is lost, the senior platoon still
// able to fight takes over the trains once the handover is done, resupply and regrouping go
// to it at the reduced rate, and save/load stays deterministic. Exit code 0 = all passed.
// Usage: hq_loss_test
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

static void run_to_next_logistics(World &w) {
    do w.tick();
    while (w.time_min % 60 != 0);
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

// Fuel gained by unit `id` over the next logistics pass.
static float fuel_gain(World &w, int id) {
    w.units[id].fuel = 50.0f;
    run_to_next_logistics(w);
    return w.units[id].fuel - 50.0f;
}

static int count_events(World &w, const std::string &needle) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) ++n;
    return n;
}

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0); // 06:00
    const std::vector<uint8_t> start = w.save_state();
    // Order of battle: 0 HQ, 1 1st Tank, 2 2nd Tank, 3 1st Mech, ...
    const int HQ = 0, TANK1 = 1, TANK2 = 2, MECH1 = 3;
    const char *TAKEOVER = "has taken over the company trains";

    std::printf("HQ alive (reference):\n");
    check(w.acting_hq(0) == HQ, "trains work for the HQ");
    check(w.handover_minutes_left(0) == 0, "no handover while the HQ lives");
    w.units[TANK2].pos = w.units[HQ].pos;
    const float full_rate = fuel_gain(w, TANK2);
    std::printf("     fuel gained in one pass at the HQ: %.1f\n", full_rate);
    check(std::fabs(full_rate - 25.0f) < 0.01f, "normal rate is 25 points an hour");

    std::printf("HQ lost at 06:30:\n");
    w.load_state(start.data(), start.size());
    ticks(w, 30);
    kill(w, HQ);
    w.units[TANK2].pos = w.units[TANK1].pos;
    check(w.acting_hq(0) == -1, "nobody runs the trains during the handover");
    check(w.handover_minutes_left(0) == World::HANDOVER_MIN, "handover takes HANDOVER_MIN minutes");
    check(fuel_gain(w, TANK2) == 0.0f, "no resupply at 07:00, before the handover is done");
    ticks(w, 30);
    check(w.acting_hq(0) == TANK1, "1st Tank (senior platoon) takes over at 07:30");
    check(w.handover_minutes_left(0) == 0, "handover done");
    const float acting_rate = fuel_gain(w, TANK2);
    std::printf("     fuel gained in one pass at the acting commander: %.1f\n", acting_rate);
    check(std::fabs(acting_rate - full_rate * World::ACTING_SUPPLY_RATE) < 0.01f,
          "acting commander resupplies at the reduced rate");
    run_to_next_logistics(w);
    check(count_events(w, TAKEOVER) == 1, "takeover announced once over two passes");

    w.order_regroup(MECH1, false);
    ticks(w, 1);
    for (int i = 0; i < 180 && (w.units[MECH1].pending.active || w.units[MECH1].moving()); ++i) w.tick();
    std::printf("     1st Mech ends %d hexes from 1st Tank\n", hex_distance(w.units[MECH1].pos, w.units[TANK1].pos));
    check(!w.units[MECH1].moving() && hex_distance(w.units[MECH1].pos, w.units[TANK1].pos) <= 2,
          "regroup forms up on the acting commander");

    std::printf("Acting commander lost:\n");
    w.drain_events();
    kill(w, TANK1);
    check(w.acting_hq(0) == TANK2, "2nd Tank takes over next");
    run_to_next_logistics(w);
    check(count_events(w, std::string("2nd Tank ") + TAKEOVER) == 1, "new acting commander announced");
    w.units[TANK2].elements = 1; // shattered (1 of 4 tanks): hands over
    w.units[TANK2].strength = 25.0f;
    check(w.acting_hq(0) == MECH1, "a platoon under 30% hands over to the next");
    run_to_next_logistics(w);
    check(count_events(w, std::string("1st Mech ") + TAKEOVER) == 1, "announced");

    std::printf("Save/load with the HQ gone:\n");
    ticks(w, 30);
    std::vector<uint8_t> s = w.save_state();
    ticks(w, 180);
    const int ann_a = count_events(w, TAKEOVER);
    std::vector<uint8_t> a = w.save_state();
    std::string err;
    check(w.load_state(s.data(), s.size(), &err), "load ok");
    ticks(w, 180);
    const int ann_b = count_events(w, TAKEOVER);
    check(ann_a == 0 && ann_b == 0, "no repeated announcement, before or after a load");
    check(a == w.save_state(), "replay after load is identical");

    std::printf("Hostile HQ lost (both sides): ");
    int sf = w.strike_formation();
    int hhq = -1;
    for (int id : w.formations[sf].members)
        if (w.units[id].type == U_HQ) hhq = id;
    kill(w, hhq);
    ticks(w, World::HANDOVER_MIN);
    int act = w.acting_hq(sf);
    std::printf("%s\n", act >= 0 ? w.units[act].name.c_str() : "none");
    check(act >= 0 && w.units[act].formation == sf, "hostile formation gets an acting commander too");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
