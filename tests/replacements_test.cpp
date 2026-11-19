// Headless check of replacements (work board MRK-10): men levied from the holdings fill
// knocked-out infantry squads at the trains, a whole squad at a time, one an hour (two while
// resting, half under an acting commander), mounted squads only up to one a vehicle, never
// vehicles or guns, the new men's morale mixes in, nothing happens away from the trains or in a
// fight, and save/load stays deterministic. Exit code 0 = all passed.
// Usage: replacements_test
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

static void run_to_next_logistics(World &w) {
    do w.tick();
    while (w.time_min % 60 != 0);
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
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

// Knocks out `n` elements of a unit (as fire would, without the fight).
static void knock_out(World &w, int id, int n) {
    Unit &u = w.units[id];
    u.elements -= n;
    u.strength = 100.0f * u.elements / u.elements_max;
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
    w.drain_events();
    // Order of battle: 0 HQ, 1 1st Tank, 2 2nd Tank, 3 1st Mech (3x BMP-1), ..., 6 1st Rifle.
    const int HQ = 0, TANK1 = 1, MECH1 = 3, RIFLE1 = 6, RIFLE2 = 7;
    const int SQUADS = w.units[MECH1].dismounts;
    const HexCoord trains = w.units[HQ].pos;
    const std::vector<uint8_t> start = w.save_state();
    const int rifle_men = World::men_per_squad(w.units[RIFLE1]);

    std::printf("What a squad costs:\n");
    std::printf("     rifle platoon (35 men, %d squads): %d men a squad; BMP-1 squads %d men\n",
                w.units[RIFLE1].elements_max, rifle_men, World::men_per_squad(w.units[SQUADS]));
    check(w.units[RIFLE1].elements_max == 3 && rifle_men == 12, "a 35-man rifle platoon has 3 squads of 12");
    check(World::men_per_squad(w.units[SQUADS]) == 8, "a BMP-1 squad is 8 men");
    check(w.replacement_squads(w.units[MECH1]) == SQUADS, "an APC/IFV platoon's replacements go to its squads aboard");
    check(w.replacement_squads(w.units[TANK1]) == -1 && w.replacement_squads(w.units[HQ]) == -1,
          "tanks and the HQ take no replacements");

    std::printf("A rifle platoon down to 1 of 3 squads, at the trains:\n");
    place(w, RIFLE1, trains);
    knock_out(w, RIFLE1, 2);
    w.units[RIFLE1].morale = 80;
    check(w.squads_short(w.units[RIFLE1]) == 2, "2 squads short");
    check(w.at_trains(w.units[RIFLE1]), "it stands at the trains");
    run_to_next_logistics(w);
    check(w.units[RIFLE1].elements == 1, "no replacements in the pool: nothing changes");
    w.drain_events();
    w.stock.replacements = 30;
    const int men_before = w.stock.personnel;
    run_to_next_logistics(w);
    std::printf("     after one hour: %d squads, %d men left in the pool, morale %.1f\n", w.units[RIFLE1].elements,
                w.stock.replacements, w.units[RIFLE1].morale);
    check(w.units[RIFLE1].elements == 2, "one squad back in the first hour");
    check(w.stock.replacements == 30 - rifle_men, "the squad's 12 men leave the pool");
    check(std::fabs(w.units[RIFLE1].strength - 200.0f / 3.0f) < 0.01f, "strength follows the squads (67%)");
    // Morale: (80 x 1 + 50 x 1) / 2, then an hour's recovery out of the fight (+0.03 a minute).
    check(w.units[RIFLE1].morale > 64.9f && w.units[RIFLE1].morale < 67.0f,
          "the new men's morale (50) mixes in: about 65");
    check(w.stock.personnel > men_before + 10, "the men count on the rolls");
    check(count_events(w, "1st Rifle: 1 squad of 12 men filled from the replacements") == 1, "the refill is reported");
    run_to_next_logistics(w);
    check(w.units[RIFLE1].elements == 3 && w.stock.replacements == 30 - 2 * rifle_men, "the third squad an hour later");
    run_to_next_logistics(w);
    check(w.units[RIFLE1].elements == 3 && w.stock.replacements == 6, "a full platoon takes no more");
    check(w.squads_short(w.units[RIFLE1]) == 0, "nothing short");

    std::printf("Only whole squads:\n");
    knock_out(w, RIFLE1, 1);
    run_to_next_logistics(w);
    check(w.units[RIFLE1].elements == 2 && w.stock.replacements == 6, "6 men in the pool make no squad of 12");

    std::printf("Resting: two squads an hour:\n");
    w.load_state(start.data(), start.size());
    place(w, RIFLE1, trains);
    knock_out(w, RIFLE1, 2);
    w.units[RIFLE1].resting = true;
    w.stock.replacements = 100;
    run_to_next_logistics(w);
    check(w.units[RIFLE1].elements == 3 && w.stock.replacements == 100 - 2 * rifle_men, "both squads back in one hour");

    std::printf("Away from the trains or in a fight: nothing:\n");
    w.load_state(start.data(), start.size());
    w.stock.replacements = 100;
    {
        // 2nd Rifle 5 hexes out; 1st Rifle at the trains but hit 10 minutes before the pass.
        HexCoord far = trains;
        for (int dc = 4; dc < 12; ++dc) {
            HexCoord h{trains.col + dc, trains.row};
            if (w.map.in_bounds(h) && w.map.at(h).terrain != T_WATER && hex_distance(h, trains) > 3) {
                far = h;
                break;
            }
        }
        check(hex_distance(far, trains) > 3, "a land hex more than 3 hexes from the HQ");
        place(w, RIFLE2, far);
        knock_out(w, RIFLE2, 1);
        place(w, RIFLE1, trains);
        knock_out(w, RIFLE1, 1);
        ticks(w, 50);
        w.units[RIFLE1].last_hit = w.time_min;
        run_to_next_logistics(w);
        check(w.units[RIFLE2].elements == 2, "a platoon 2.5+ km from the HQ gets none");
        check(w.units[RIFLE1].elements == 2, "a platoon in a fight in the last 30 minutes gets none");
        check(w.stock.replacements == 100, "the pool is untouched");
    }

    std::printf("Mounted squads, up to one a vehicle:\n");
    w.load_state(start.data(), start.size());
    place(w, MECH1, trains);
    knock_out(w, MECH1, 1);    // 2 BMPs left
    knock_out(w, SQUADS, 2);   // 1 squad aboard
    w.stock.replacements = 100;
    check(w.squads_short(w.units[MECH1]) == 1, "2 vehicles, 1 squad aboard: 1 squad short");
    run_to_next_logistics(w);
    run_to_next_logistics(w);
    check(w.units[SQUADS].elements == 2 && w.units[SQUADS].embarked, "filled to 2 squads aboard 2 vehicles");
    check(w.stock.replacements == 100 - 8, "8 men from the pool");
    check(w.units[MECH1].elements == 2, "the knocked-out BMP is not replaced");
    // All squads lost aboard: the vehicles left still take new squads.
    knock_out(w, SQUADS, 2);
    check(w.can_dismount(w.units[MECH1]) == false, "no squads aboard: nothing to dismount");
    run_to_next_logistics(w);
    check(w.units[SQUADS].elements == 1 && w.can_dismount(w.units[MECH1]), "new squads join the vehicles left");
    {
        // Squads on foot next to the trains are filled where they stand, up to the vehicles left.
        check(w.order_dismount(MECH1) >= 0, "dismount ordered");
        for (int i = 0; i < 120 && w.units[SQUADS].embarked; ++i) w.tick();
        check(w.units[SQUADS].alive && !w.units[SQUADS].embarked, "the squads are out");
        place(w, SQUADS, trains);
        place(w, MECH1, trains);
        run_to_next_logistics(w);
        check(w.units[SQUADS].elements == 2, "squads on foot filled too, to one a vehicle");
    }

    std::printf("Vehicles and guns are not replaced:\n");
    w.load_state(start.data(), start.size());
    place(w, TANK1, trains);
    knock_out(w, TANK1, 2);
    w.stock.replacements = 100;
    run_to_next_logistics(w);
    run_to_next_logistics(w);
    check(w.units[TANK1].elements == 2 && w.squads_short(w.units[TANK1]) == 0, "a tank platoon stays at 2 of 4");
    check(w.stock.replacements == 100, "and takes no men");

    std::printf("Acting commander: a squad every other hour:\n");
    w.load_state(start.data(), start.size());
    kill(w, HQ);
    ticks(w, World::HANDOVER_MIN + 5);
    const int acting = w.acting_hq(0);
    check(acting >= 0 && acting != HQ, "someone has taken over the trains");
    place(w, RIFLE1, w.units[acting].pos);
    knock_out(w, RIFLE1, 2);
    w.stock.replacements = 100;
    run_to_next_logistics(w);
    run_to_next_logistics(w);
    std::printf("     after two hours: %d squads\n", w.units[RIFLE1].elements);
    check(w.units[RIFLE1].elements == 2, "one squad in two hours");

    std::printf("Hostiles take none:\n");
    {
        w.load_state(start.data(), start.size());
        int hostile = -1;
        for (auto &u : w.units)
            if (u.alive && u.side != SIDE_PLAYER && u.type == U_INFANTRY) hostile = u.id;
        check(hostile >= 0 && w.replacement_squads(w.units[hostile]) == -1, "a hostile rifle unit has no pool to draw on");
    }

    std::printf("Save/load:\n");
    {
        w.load_state(start.data(), start.size());
        place(w, RIFLE1, trains);
        knock_out(w, RIFLE1, 2);
        place(w, MECH1, trains);
        knock_out(w, SQUADS, 2);
        w.stock.replacements = 40;
        run_to_next_logistics(w);
        const std::vector<uint8_t> mid = w.save_state();
        ticks(w, 180);
        const std::vector<uint8_t> a = w.save_state();
        World w2;
        w2.generate(1989, 24, 22);
        check(w2.load_state(mid.data(), mid.size()), "the save loads");
        ticks(w2, 180);
        check(w2.save_state() == a, "replaying from the save gives the same state");
    }

    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures;
}
