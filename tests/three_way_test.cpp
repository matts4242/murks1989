// The three-way (1v1v1) scenario: Company Murks, the 7th Guards Remnant and Kessler's Free Corps,
// each at war with the other two. Checks the setup, the hostiles and the rivals fighting each
// other, kills and event reports, the hunters' targets, holdings, the outcome, and save/load
// (byte-identical replay). Exit code 0 = all passed.
#include "murks/sim.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

static std::vector<uint8_t> flat_map_bytes() {
    Map m;
    m.init(1, 1); // 64 x 64 hexes of flat open ground
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    m.name = "three-way-flat";
    const char *path = "three_way_flat.murksmap";
    m.save(path);
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(f), {});
    std::remove(path);
    return bytes;
}

static Unit make_tank(World &w, int formation, int8_t side, HexCoord at) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = side;
    u.type = U_TANK;
    u.name = side == SIDE_PLAYER ? "Blue" : side == SIDE_HOSTILE ? "Red" : "Green";
    u.equipment = "3x T-72M";
    u.formation = formation;
    u.pos = u.prev_pos = u.dest = at;
    for (auto &p : u.last_seen_pos) p = at;
    u.brk = BC_NEVER;
    u.elements_max = u.elements = World::element_count(u);
    w.formations[formation].members.push_back(u.id);
    return u;
}

static int formation_of_side(const World &w, int side, bool hunter) {
    for (int i = 0; i < (int)w.formations.size(); ++i)
        if (w.formations[i].side == side && w.formations[i].hunts_player == hunter) return i;
    return -1;
}

static void break_formation(World &w, int f) {
    for (int id : w.formations[f].members) {
        Unit &u = w.units[id];
        if (u.type == U_HQ || u.type == U_ARTILLERY) continue;
        u.alive = false;
        u.elements = 0;
        u.strength = 0;
    }
}

int main() {
    // ---- 1. Hostiles and rivals fight each other; the company is not credited -------------
    std::printf("Hostile against rival (flat open ground, 1.5 km, daylight):\n");
    {
        const std::vector<uint8_t> bytes = flat_map_bytes();
        int fought = 0, both = 0, damaged = 0, credited = 0, leaked = 0, runs = 6;
        for (int run = 0; run < runs; ++run) {
            World w;
            w.load_map(bytes.data(), bytes.size());
            w.scenario = SC_THREE_WAY;
            w.time_min = 10 * 60 - 1;
            w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
            w.formations.push_back({"Red", SIDE_HOSTILE, -1, false, 0, {}});
            w.formations.push_back({"Green", SIDE_RIVAL, -1, false, 0, {}});
            w.units.push_back(make_tank(w, 0, SIDE_PLAYER, {5, 5}));   // far out of sight (> 25 km)
            w.units.push_back(make_tank(w, 1, SIDE_HOSTILE, {40, 50}));
            w.units.push_back(make_tank(w, 2, SIDE_RIVAL, {43, 50}));
            w.reseed_rng(4000 + run * 7919);
            ticks(w, 60);
            const Unit &r = w.units[1], &g = w.units[2];
            if (r.last_target == 2 || g.last_target == 1) ++fought;
            if (r.last_target == 2 && g.last_target == 1) ++both;
            if (r.elements < r.elements_max || g.elements < g.elements_max) ++damaged;
            if (w.stats().kills != 0 || w.stats().losses != 0) ++credited;
            for (const Event &e : w.drain_events())
                if (e.kind == EV_DESTROYED || e.kind == EV_CONTACT || e.kind == EV_UNDER_FIRE) ++leaked;
        }
        char msg[160];
        std::snprintf(msg, sizeof msg, "they open fire on each other (%d of %d runs; both of them in %d)", fought, runs, both);
        check(fought == runs && both >= runs / 2, msg);
        std::snprintf(msg, sizeof msg, "and knocks out vehicles (%d of %d runs)", damaged, runs);
        check(damaged >= runs - 1, msg);
        check(credited == 0, "their kills are not the company's kills, and are not its losses");
        check(leaked == 0, "a fight the company cannot see sends it no contact, fire or kill reports");
    }
    std::printf("All three in one place:\n");
    {
        const std::vector<uint8_t> bytes = flat_map_bytes();
        World w;
        w.load_map(bytes.data(), bytes.size());
        w.scenario = SC_THREE_WAY;
        w.time_min = 10 * 60 - 1;
        w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
        w.formations.push_back({"Red", SIDE_HOSTILE, -1, false, 0, {}});
        w.formations.push_back({"Green", SIDE_RIVAL, -1, false, 0, {}});
        w.units.push_back(make_tank(w, 0, SIDE_PLAYER, {30, 30}));
        w.units.push_back(make_tank(w, 1, SIDE_HOSTILE, {33, 30}));
        w.units.push_back(make_tank(w, 2, SIDE_RIVAL, {31, 33}));
        w.reseed_rng(77);
        bool rival_contact = false, rival_word = false;
        bool seen[SIDE_COUNT][SIDE_COUNT] = {};
        for (int m = 0; m < 20; ++m) {
            w.tick();
            for (int a = 0; a < SIDE_COUNT; ++a)
                for (int b = 0; b < SIDE_COUNT; ++b)
                    if (a != b && w.seen_by(w.units[a], b)) seen[a][b] = true;
            for (const Event &e : w.drain_events()) {
                if (e.kind == EV_CONTACT && e.text.find("rival") != std::string::npos) rival_contact = true;
                if (e.text.find("rival") != std::string::npos) rival_word = true;
            }
        }
        bool all = true;
        for (int a = 0; a < SIDE_COUNT; ++a)
            for (int b = 0; b < SIDE_COUNT; ++b)
                if (a != b && !seen[a][b]) all = false;
        check(all, "every side spots both of the others");
        check(rival_contact && rival_word, "the company's reports name the Free Corps' units as rival");
        check(std::string(w.side_word(SIDE_HOSTILE)) == "hostile" && std::string(w.side_word(SIDE_RIVAL)) == "rival",
              "side words: hostile, rival");
    }

    // ---- 2. Setup on the generated map --------------------------------------------------
    std::printf("Setup:\n");
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(4, SC_THREE_WAY);
    check(w.scenario == SC_THREE_WAY, "the scenario is the three-way");
    const int fh = formation_of_side(w, SIDE_HOSTILE, true), fr = formation_of_side(w, SIDE_RIVAL, true);
    check(w.formations[0].side == SIDE_PLAYER && fh == 1 && fr == 2 &&
              w.formations[fh].name == "7th Guards Remnant" && w.formations[fr].name == "Kessler's Free Corps",
          "Company Murks, the 7th Guards Remnant and Kessler's Free Corps, both of them hunting");
    check(w.hunter_formations() == std::vector<int>({1, 2}) && w.strike_formation() == 1, "two hunters, hostile first");
    check(w.formations[fh].start_combat == 6 && w.formations[fr].start_combat == 6,
          "each of them has six fighting platoons at the start");
    {
        int rival_units = 0, rival_squads = 0;
        for (const auto &u : w.units)
            if (u.formation == fr) {
                ++rival_units;
                if (u.carrier >= 0 && u.embarked) ++rival_squads;
            }
        check(rival_units == 10 && rival_squads == 2, "the Free Corps: 8 platoons, with squads aboard its BMP-1s and BTR-70s");
        int h_bands = 0, r_bands = 0;
        for (const auto &f : w.formations)
            if (!f.hunts_player && f.side != SIDE_PLAYER) (f.side == SIDE_RIVAL ? r_bands : h_bands)++;
        check(h_bands == 2 && r_bands == 2, "the patrolling bands take sides, two each");
        const HexCoord home = w.units[0].pos, hp = w.units[w.formations[fh].members[0]].pos,
                       rp = w.units[w.formations[fr].members[0]].pos;
        const int a = hex_distance(home, hp), b = hex_distance(home, rp), c = hex_distance(hp, rp);
        char msg[200];
        std::snprintf(msg, sizeof msg, "the three start roughly in a triangle (%.0f, %.0f and %.0f km apart)", a * 0.5,
                      b * 0.5, c * 0.5);
        check(a > 60 && b > 60 && c > 60 && a < 260 && b < 260 && c < 260 && w.reachable(rp, home) && w.reachable(rp, hp),
              msg);
    }
    {
        World c;
        c.generate(1989, 24, 22);
        c.setup_scenario(4);
        check(c.scenario == SC_CONTRACT && c.hunter_formations().size() == 1 && formation_of_side(c, SIDE_RIVAL, true) < 0,
              "the contract still has one hunter and no rival side");
        bool any_rival = false;
        for (const auto &u : c.units) any_rival |= u.side == SIDE_RIVAL;
        check(!any_rival && std::string(c.side_word(SIDE_RIVAL)) == "hostile", "and no rival units");
    }

    // ---- 3. Hunters hunt both of the others ----------------------------------------------
    std::printf("Hunters:\n");
    ticks(w, 1);
    check(w.formations[fh].has_target && w.formations[fr].has_target, "both hunters have word of a target at once");
    {
        // A fresh sighting of a rival unit by the hostiles pulls the 7th Guards towards it.
        Unit &r = w.units[w.formations[fr].members[1]];
        r.last_seen[SIDE_HOSTILE] = w.time_min + 1; // seen in the coming minute
        r.last_seen_pos[SIDE_HOSTILE] = r.pos;
        ticks(w, 1);
        check(w.formations[fh].target == r.pos, "the 7th Guards head for the Free Corps unit they have just seen");
        Unit &p = w.units[1];
        p.last_seen[SIDE_RIVAL] = w.time_min + 1;
        p.last_seen_pos[SIDE_RIVAL] = p.pos;
        ticks(w, 1);
        check(w.formations[fr].target == p.pos, "and the Free Corps for the company unit they have just seen");
    }

    // ---- 4. Save and load ----------------------------------------------------------------
    std::printf("Save and load:\n");
    ticks(w, 240);
    {
        std::vector<uint8_t> blob = w.save_state();
        World r;
        r.generate(1989, 24, 22);
        r.setup_scenario(0);
        std::string err;
        const bool ok = r.load_state(blob.data(), blob.size(), &err);
        int rivals = 0;
        for (const auto &u : r.units) rivals += u.side == SIDE_RIVAL;
        check(ok && r.scenario == SC_THREE_WAY && rivals > 10, "a three-way save loads as a three-way game, rivals and all");
        check(r.save_state() == blob, "it saves back byte for byte");
        World a, b;
        a.generate(1989, 24, 22);
        b.generate(1989, 24, 22);
        a.load_state(blob.data(), blob.size());
        b.load_state(blob.data(), blob.size());
        ticks(a, 300);
        ticks(b, 300);
        check(a.save_state() == b.save_state(), "two loads of it play out identically (300 min)");
        std::vector<uint8_t> old = blob;
        old[7] = 'A';
        check(!r.load_state(old.data(), old.size(), &err) && err.find("older") != std::string::npos,
              "a save from before the third side is refused as older");
        std::vector<uint8_t> bad = blob;
        bad[bad.size() - 1] = SC_COUNT;
        check(!r.load_state(bad.data(), bad.size(), &err), "an unknown scenario kind is refused");
    }

    // ---- 5. Holdings: the rivals can take a town --------------------------------------------
    std::printf("Holdings:\n");
    {
        World h;
        h.generate(1989, 24, 22);
        h.setup_scenario(0, SC_THREE_WAY);
        h.hostile_ai = false;
        int town = -1;
        for (int i = 1; i < (int)h.holdings.size() && town < 0; ++i)
            if (h.holdings[i].owner < 0 && h.holdings[i].kind == HK_TOWN) {
                bool clear = true;
                for (const auto &u : h.units)
                    if (u.alive && hex_distance(u.pos, h.holdings[i].pos) <= 30) clear = false;
                if (clear) town = i;
            }
        check(town > 0, "an independent town with nobody near it");
        int rifle = -1;
        for (int id : h.formations[fr].members)
            if (h.units[id].type == U_INFANTRY && h.units[id].carrier < 0) rifle = id;
        Unit &u = h.units[rifle];
        u.pos = u.prev_pos = u.dest = h.holdings[town].pos;
        ticks(h, 200);
        check(h.holdings[town].owner == SIDE_RIVAL, "a Free Corps platoon alone in it for two hours takes it");
        int tank = -1;
        for (int id : h.formations[fh].members)
            if (h.units[id].type == U_TANK) tank = id;
        Unit &t = h.units[tank];
        t.pos = t.prev_pos = t.dest = h.holdings[town].pos;
        t.roe = ROE_HOLD;
        u.roe = ROE_HOLD; // both hold fire, so the place stays contested
        ticks(h, 200);
        check(h.holdings[town].owner == SIDE_RIVAL, "with the 7th Guards in it too, it does not change hands");
    }

    // ---- 6. The outcome ----------------------------------------------------------------------
    std::printf("Outcome:\n");
    {
        World o;
        o.generate(1989, 24, 22);
        o.setup_scenario(0, SC_THREE_WAY);
        o.hostile_ai = false;
        o.drain_events();
        break_formation(o, fh);
        ticks(o, 5);
        bool told = false;
        for (const Event &e : o.drain_events())
            told |= e.text == "7th Guards Remnant is broken. Kessler's Free Corps is still in the field.";
        check(o.outcome() == OUT_ONGOING && told, "one broken is not enough: the company is told the other is still out there");
        ticks(o, 30);
        int repeats = 0;
        for (const Event &e : o.drain_events()) repeats += e.text.find("is broken") != std::string::npos;
        check(repeats == 0, "and told only once");
        break_formation(o, fr);
        ticks(o, 5);
        check(o.outcome() == OUT_VICTORY && o.outcome_text().find("both broken") != std::string::npos,
              "both broken: victory");
        World d;
        d.generate(1989, 24, 22);
        d.setup_scenario(0, SC_THREE_WAY);
        d.hostile_ai = false;
        break_formation(d, 0);
        ticks(d, 5);
        check(d.outcome() == OUT_DEFEAT, "the company broken first: defeat");
    }

    std::printf(failures ? "three_way_test: %d FAILED\n" : "three_way_test: all passed\n", failures);
    return failures ? 1 : 0;
}
