// Headless check of the planning map (work board MRK-25): its figures must be the rules'
// own. Going per hex equals the mean of move_cost - the pathfinder's step cost - for real
// units of each mobility stepping in from each land neighbour; line of sight from a unit's
// hex equals the spotting model's own visible_hexes; the tints and keys exist for every
// mode. Exit code 0 = all passed.
// Usage: planning_test
#include "murks/planning.hpp"
#include "murks/render_data.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0);
    const Map &m = w.map;

    // A unit of each mobility (tank: tracked, recon: wheeled, rifles: foot), moving normally.
    const int types[3] = {U_TANK, U_RECON, U_INFANTRY};
    int probe[3] = {-1, -1, -1};
    for (const Unit &u : w.units)
        for (int k = 0; k < 3; ++k)
            if (probe[k] < 0 && u.type == types[k]) probe[k] = u.id;
    check(probe[0] >= 0 && probe[1] >= 0 && probe[2] >= 0, "the scenario has a tracked, a wheeled and a foot unit");

    // 1. Going agrees with move_cost on every land hex that has no road or rail at all
    //    (so every step into it is an off-road step), over a sample of the map.
    int tested = 0, bad = 0;
    double worst = 0;
    for (int idx = 0; idx < m.hex_count() && tested < 20000; idx += 97) {
        const Hex &h = m.at_index(idx);
        if (h.terrain == T_WATER || (h.features & (F_ROAD | F_RAIL))) continue;
        const HexCoord c = m.coord_of(idx);
        for (int k = 0; k < 3; ++k) {
            Unit &u = w.units[probe[k]];
            u.mode = MV_HASTY;
            double sum = 0;
            int n = 0;
            for (int d = 0; d < 6; ++d) {
                HexCoord nb = neighbor(c, d);
                if (!m.in_bounds(nb) || m.at(nb).terrain == T_WATER) continue;
                sum += w.move_cost(u, m.index(nb), idx);
                ++n;
            }
            if (n == 0) continue;
            const double want = sum / n, got = hex_going(w, idx, unit_type_info(u.type).mobility);
            worst = std::max(worst, std::fabs(want - got));
            if (std::fabs(want - got) > 1e-4) ++bad;
            ++tested;
        }
    }
    std::printf("  going: %d hex x mobility samples, worst difference %.6f min\n", tested, worst);
    check(tested > 1000 && bad == 0, "going per hex equals the mean of move_cost from its land neighbours");
    check(hex_going(w, m.index(w.units[probe[0]].pos), M_TRACKED) > 0, "going is positive on land");
    {
        int water = -1;
        for (int idx = 0; idx < m.hex_count() && water < 0; ++idx)
            if (m.at_index(idx).terrain == T_WATER) water = idx;
        check(water >= 0 && hex_going(w, water, M_WHEELED) < 0, "water is impassable in the going mode");
    }

    // 2. Line of sight from a unit's hex at the spotting heights and its spot range equals the
    //    spotting model's visible_hexes.
    for (int k = 0; k < 2; ++k) {
        const Unit &u = w.units[probe[k]];
        std::vector<HexCoord> vis;
        w.visible_hexes(u.id, vis);
        std::set<std::pair<int, int>> want;
        for (HexCoord h : vis) want.insert({h.col, h.row});
        Viewshed vs;
        const int r = (int)w.effective_spot_range(u);
        viewshed(w, u.pos, SPOT_EYE_M, SPOT_TARGET_M, r, vs);
        std::set<std::pair<int, int>> got;
        for (int j = 0; j < vs.h; ++j)
            for (int i = 0; i < vs.w; ++i) {
                const uint8_t *px = &vs.rgba[((size_t)j * vs.w + i) * 4];
                uint32_t t = px[0] | (px[1] << 8) | (px[2] << 16) | ((uint32_t)px[3] << 24);
                if (t == sight_tint(0)) got.insert({vs.origin.col + i, vs.origin.row + j});
            }
        char msg[160];
        std::snprintf(msg, sizeof msg, "line of sight from %s matches the spotting model (%zu of %d hexes seen)",
                      u.name.c_str(), got.size(), vs.total);
        check(got == want && (int)got.size() == vs.seen, msg);
    }

    // 3. A higher eye never sees less.
    {
        const HexCoord from = w.units[probe[0]].pos;
        Viewshed low, high;
        viewshed(w, from, 2.0f, SPOT_TARGET_M, SIGHT_RADIUS_HEXES, low);
        viewshed(w, from, 30.0f, SPOT_TARGET_M, SIGHT_RADIUS_HEXES, high);
        bool superset = true;
        for (size_t i = 0; i < low.rgba.size(); i += 4) {
            uint32_t a = low.rgba[i] | (low.rgba[i + 1] << 8) | (low.rgba[i + 2] << 16) | ((uint32_t)low.rgba[i + 3] << 24);
            uint32_t b = high.rgba[i] | (high.rgba[i + 1] << 8) | (high.rgba[i + 2] << 16) | ((uint32_t)high.rgba[i + 3] << 24);
            if (a == sight_tint(0) && b != sight_tint(0)) superset = false;
        }
        char msg[160];
        std::snprintf(msg, sizeof msg, "a 30 m eye sees everything a 2 m eye sees, and more (%d vs %d hexes)", high.seen, low.seen);
        check(superset && high.seen >= low.seen, msg);
    }

    // 4. Every whole-map mode has a tint image and a key; slope tints follow slope_class.
    for (int mode = PLAN_GOING_TRACKED; mode <= PLAN_SLOPE; ++mode) {
        std::vector<uint8_t> rgba;
        build_plan_rgba(w, mode, rgba);
        std::vector<PlanKey> key;
        plan_key(mode, key);
        size_t tinted = 0;
        for (size_t i = 3; i < rgba.size(); i += 4) tinted += rgba[i] > 0;
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s: %zu hexes tinted, %zu key rows", plan_mode_name(mode), tinted, key.size());
        check(rgba.size() == (size_t)m.cols() * m.rows() * 4 && tinted > 0 && key.size() >= 3, msg);
    }
    {
        std::vector<uint8_t> rgba;
        build_plan_rgba(w, PLAN_SLOPE, rgba);
        bool ok = true;
        for (int idx = 0; idx < m.hex_count(); idx += 131) {
            HexCoord c = m.coord_of(idx);
            int dir;
            int cls = slope_class(hex_slope(m, c.col, c.row, dir));
            uint8_t a = rgba[((size_t)c.row * m.cols() + c.col) * 4 + 3];
            if ((cls == 0) != (a == 0)) ok = false;
        }
        check(ok, "slope tints follow the slope classes of the status-bar readout");
    }
    std::vector<PlanKey> key;
    plan_key(PLAN_SIGHT, key);
    check(key.size() >= 3, "the line-of-sight mode has a key");

    std::printf(failures ? "planning_test: %d FAILED\n" : "planning_test: all passed\n", failures);
    return failures ? 1 : 0;
}
