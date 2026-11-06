// Headless checks of suppression (6 Nov 2026): it builds under fire whether or not the fire
// hits, holds while the fire goes on, fades once it stops, slows engagement, pins soft units
// (they stop moving and spot at half range), and armour and cover shield. Exit code 0 = passed.
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

static World flat_world() {
    Map m;
    m.init(2, 2);
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = 100;
            h.terrain = c >= 90 ? T_FOREST : T_OPEN; // woods from column 90 east
            h.cover = 0;
            h.features = 0;
        }
    m.name = "supp-flat";
    m.save("supp_flat.murksmap");
    std::ifstream f("supp_flat.murksmap", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    std::remove("supp_flat.murksmap");
    World w;
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(9);
    w.time_min = 10 * 60 - 1;
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
    u.brk = BC_NEVER;
    w.formations[formation].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

// Peak suppression of `target` under `minutes` of fire from a hostile battery observed by
// recon; the target holds its fire.
struct Run {
    float peak = 0;
    bool pinned = false;
};
static Run under_battery(UnitType t, const char *equip, int col, int minutes, World *out = nullptr) {
    World w = flat_world();
    int tgt = add(w, 0, t, equip, {col, 64});
    w.units[tgt].roe = ROE_HOLD;
    add(w, 1, U_ARTILLERY, "6x 2S3", {col - 24, 64});
    int obs = add(w, 1, U_RECON, "2x BRDM-2", {col - 4, 64});
    w.units[obs].roe = ROE_HOLD;
    Run r;
    for (int m = 0; m < minutes; ++m) {
        w.tick();
        r.peak = std::max(r.peak, w.units[tgt].suppression);
        r.pinned = r.pinned || w.units[tgt].pinned;
    }
    if (out) *out = w;
    return r;
}

int main() {
    std::printf("Weights:\n");
    {
        Unit g;
        g.type = U_ARTILLERY;
        g.equipment = "6x 2S3";
        check(std::fabs(World::suppression_weight(g) - 152.0f / 155.0f) < 1e-4f, "152 mm gun ~ one 155 mm round");
        g.equipment = "6x 2S1";
        check(std::fabs(World::suppression_weight(g) - 122.0f / 155.0f) < 1e-4f, "122 mm: 0.79");
        g.equipment = "BM-21";
        check(World::suppression_weight(g) > 3.0f, "rocket launcher ripples weigh 4x");
        Unit u;
        u.suppression = 0;
        World::suppress(u, 3.0f, 0);
        check(std::fabs(u.suppression - 63.2f) < 0.1f, "S = S0 takes 63% of what is left");
        World::suppress(u, 3.0f, 0);
        check(u.suppression > 85.0f && u.suppression < 87.0f, "and saturates toward 100");
    }

    std::printf("Rifle platoon in the open under a 6-gun 152 mm battery:\n");
    World w;
    Run open = under_battery(U_INFANTRY, "33 men", 60, 30, &w);
    std::printf("     peak suppression %.0f, pinned %s\n", open.peak, open.pinned ? "yes" : "no");
    check(open.peak >= 60 && open.pinned, "suppressed and pinned");
    {
        int tgt = 0;
        const float s = w.units[tgt].suppression;
        if (w.units[tgt].alive) {
            auto et = w.engagement_time(w.units[tgt]);
            check(std::fabs(et.f_supp - (1.0f + s / 50.0f)) < 1e-4f, "engagement slowed by 1 + s/50");
            if (w.units[tgt].pinned)
                check(w.effective_spot_range(w.units[tgt]) <= unit_type_info(U_INFANTRY).spot_range * 0.5f + 1e-4f,
                      "pinned: spots at half range");
        }
        // Silence the guns and watch it fade.
        for (auto &u : w.units)
            if (u.type == U_ARTILLERY) u.roe = ROE_HOLD;
        const int64_t hold = w.units[tgt].supp_hold_until;
        while (w.time_min < hold) w.tick();
        const float s0 = w.units[tgt].suppression;
        w.tick();
        w.tick();
        const float s2 = w.units[tgt].suppression;
        int unpinned = -1;
        for (int m = 1; m <= 20 && w.units[tgt].suppression > 0; ++m) {
            w.tick();
            if (unpinned < 0 && !w.units[tgt].pinned) unpinned = m + 2;
        }
        std::printf("     after the fire: %.0f -> %.0f in 2 min, up again after %d min, gone after 22 min: %s\n", s0,
                    s2, unpinned, w.units[tgt].suppression == 0 ? "yes" : "no");
        check(s2 < s0 * 0.4f, "two minutes after the fire stops most of it has gone");
        check(!w.units[tgt].pinned && w.units[tgt].suppression == 0, "fully recovered");
    }

    std::printf("Moving under fire:\n");
    {
        World v = flat_world();
        int tgt = add(v, 0, U_INFANTRY, "33 men", {60, 64});
        v.units[tgt].roe = ROE_HOLD;
        v.tick();
        v.order_move(tgt, {80, 64}, MV_HASTY);
        while (!v.units[tgt].moving()) v.tick();
        for (int m = 0; m < 10; ++m) v.tick(); // under way
        // Fire comes down: pinned, with the fire going on for 5 more minutes.
        v.units[tgt].suppression = 80;
        v.units[tgt].supp_hold_until = v.time_min + 5;
        v.units[tgt].pinned = true;
        const HexCoord at = v.units[tgt].pos;
        const float prog = v.units[tgt].progress;
        int still = 0, moved_at = -1;
        for (int m = 1; m <= 30 && moved_at < 0; ++m) {
            v.tick();
            if (v.units[tgt].pos == at && v.units[tgt].progress == prog) ++still;
            else moved_at = m;
        }
        std::printf("     went to ground for %d min, moving again after %d min\n", still, moved_at);
        check(still >= 6 && v.units[tgt].moving(), "no progress while pinned, the move goes on once it fades");
        check(moved_at > 0 && moved_at < 15, "gets up a few minutes after the fire stops");
    }

    std::printf("Armour and cover:\n");
    {
        Run tank = under_battery(U_TANK, "3x T-55A", 60, 30);
        Run woods = under_battery(U_INFANTRY, "33 men", 100, 30);
        std::printf("     peaks: rifles in the open %.0f, rifles in woods %.0f, tanks %.0f (pinned %s)\n", open.peak,
                    woods.peak, tank.peak, tank.pinned ? "yes" : "no");
        check(tank.peak < open.peak && !tank.pinned, "tanks suppressed less and never pinned");
        check(woods.peak < open.peak, "cover shields");
    }

    std::printf("Save/load while suppressed:\n");
    {
        World s;
        under_battery(U_INFANTRY, "33 men", 60, 12, &s);
        std::vector<uint8_t> b = s.save_state();
        for (int k = 0; k < 20; ++k) s.tick();
        std::vector<uint8_t> after = s.save_state();
        std::string err;
        check(s.load_state(b.data(), b.size(), &err), "load ok");
        for (int k = 0; k < 20; ++k) s.tick();
        check(after == s.save_state(), "replay identical");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
