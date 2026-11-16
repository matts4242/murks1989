// Headless check of the planning map (work board MRK-25): its figures must be the rules'
// own. Going per hex equals the mean of move_cost - the pathfinder's step cost - for real
// units of each mobility stepping in from each land neighbour; line of sight from a unit's
// hex equals the spotting model's own visible_hexes; the tints and keys exist for every
// mode. The planning tools (MRK-26): the ruler's route estimate is the route and time a unit
// ordered there takes; the profile's sight line is line_of_sight; grid references and bearings;
// markings survive save/load and the simulation ignores them. Exit code 0 = all passed.
// Usage: planning_test
#include "murks/planning.hpp"
#include "murks/render_data.hpp"

#include <algorithm>
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

    // --- Planning tools (MRK-26) ---

    // 5. sight_block is the line-of-sight rule: clear exactly when line_of_sight is true, and a
    //    blocking hex really stands above the sight line.
    {
        int pairs = 0, bad5 = 0, blocked = 0;
        for (int i = 0; i < 4000; ++i) {
            HexCoord a = m.coord_of((int)(((uint64_t)i * 2654435761u) % (uint64_t)m.hex_count()));
            HexCoord b{a.col + (i * 7) % 49 - 24, a.row + (i * 13) % 49 - 24};
            if (!m.in_bounds(b)) continue;
            ++pairs;
            const int blk = w.sight_block(a, b, 3.0f, 2.5f);
            if ((blk == -1) != w.line_of_sight(a, b, 3.0f, 2.5f)) ++bad5;
            if (blk > 0) {
                ++blocked;
                Profile pf;
                profile_line(w, a, b, 3.0f, 2.5f, pf);
                const ProfilePoint &s0 = pf.pts.front(), &s1 = pf.pts.back(), &h = pf.pts[blk];
                const float n = (float)(pf.pts.size() - 1);
                const float ray = s0.ground + 3.0f + (s1.ground + 2.5f - s0.ground - 3.0f) * (blk / n);
                if (pf.block != blk || h.ground + h.cover <= ray) ++bad5;
            }
        }
        char msg[160];
        std::snprintf(msg, sizeof msg, "the profile's sight line is line_of_sight (%d lines, %d blocked)", pairs, blocked);
        check(pairs > 1000 && blocked > 0 && bad5 == 0, msg);
    }

    // 6. Route estimate = what an order does: a tank ordered from A to B takes the estimated
    //    path, and drives it at full readiness in the estimated time.
    {
        World v;
        v.generate(1989, 24, 22);
        v.setup_scenario(0);
        int tank = -1;
        for (const Unit &u : v.units)
            if (u.type == U_TANK && u.side == SIDE_PLAYER && u.group < 0) {
                tank = u.id;
                break;
            }
        check(tank >= 0, "a tank platoon outside any team");
        const HexCoord a = v.units[tank].pos;
        HexCoord b{-1, -1};
        for (int d = 30; d < 60 && b.col < 0; ++d) {
            HexCoord c{a.col + d, a.row + d / 3};
            if (v.map.in_bounds(c) && v.map.at(c).terrain != T_WATER && v.reachable(a, c)) b = c;
        }
        RouteEstimate est = v.route_estimate(a, b, M_TRACKED, MV_HASTY);
        int sum_ok = est.found ? 1 : 0;
        {
            double sum = 0;
            int from = v.map.index(a);
            for (int idx : est.path) {
                sum += v.move_cost(v.units[tank], from, idx);
                from = idx;
            }
            if (std::fabs(sum - est.minutes) > 1e-3) sum_ok = 0;
        }
        check(sum_ok == 1, "estimated minutes are the sum of the unit's move_cost along the route");
        const int delay = v.order_move(tank, b, MV_HASTY);
        int64_t started = -1, arrived = -1;
        bool same_path = false;
        for (int t = 0; t < 2000 && arrived < 0; ++t) {
            v.units[tank].readiness = 100.0f; // full readiness, as the estimate
            v.units[tank].fuel = std::max(v.units[tank].fuel, 100.0f);
            v.tick();
            if (started < 0 && v.units[tank].moving()) {
                started = v.time_min;
                same_path = v.units[tank].path == est.path;
            }
            if (started >= 0 && !v.units[tank].moving()) arrived = v.time_min;
        }
        const double took = (double)(arrived - started) + 1.0; // the first step is driven in the tick it starts
        char msg[200];
        std::snprintf(msg, sizeof msg, "a tank ordered there takes the estimated route and time (%.1f km, est. %.1f min, drove %.0f min, delay %d)",
                      est.km, est.minutes, took, delay);
        check(same_path && arrived > 0 && std::fabs(took - est.minutes) <= 2.0 + 0.02 * est.minutes, msg);
        RouteEstimate foot = v.route_estimate(a, b, M_FOOT, MV_HASTY);
        check(foot.found && foot.minutes > est.minutes, "on foot is slower than tracked over the same ground");
        int water = -1;
        for (int idx = 0; idx < v.map.hex_count() && water < 0; ++idx)
            if (v.map.at_index(idx).terrain == T_WATER) water = idx;
        check(!v.route_estimate(a, v.map.coord_of(water), M_TRACKED, MV_HASTY).found, "no route estimate into water");
    }

    // 7. Grid references and bearings.
    {
        Map g; // only the georeference matters here
        g.epsg = 3035;
        g.geo_origin_e = 4721000.0;
        g.geo_origin_n = 3268900.0;
        check(grid_ref(g, 437.0, 590.0) == "47/32 214 683", "grid reference 47/32 214 683 (100 km square, then 100 m)");
        check(std::fabs(grid_bearing(0, 0, 0, -1) - 0) < 1e-9 && std::fabs(grid_bearing(0, 0, 1, 0) - 90) < 1e-9 &&
                  std::fabs(grid_bearing(0, 0, 0, 1) - 180) < 1e-9 && std::fabs(grid_bearing(0, 0, -1, 0) - 270) < 1e-9,
              "grid bearings: north 0, east 90, south 180, west 270");
    }

    // 8. Markings: kept through save/load, refused when malformed, ignored by the simulation.
    {
        World v;
        v.generate(1989, 24, 22);
        v.setup_scenario(0);
        World plain;
        plain.generate(1989, 24, 22);
        plain.setup_scenario(0);
        Marking obj{MK_OBJECTIVE, "OBJ WOLF", {20000, 30000}};
        Marking pl{MK_PHASE_LINE, "PL RED", {10000, 10000, 15000, 12000, 21000, 11000}};
        Marking bad{MK_PHASE_LINE, "PL ONE POINT", {10000, 10000}};
        check(v.add_marking(obj) == 0 && v.add_marking(pl) == 1 && v.add_marking(bad) == -1 &&
                  v.add_marking(Marking{MK_NOTE, "off the map", {-5, 10}}) == -1,
              "markings are added; a one-point phase line and a point off the map are refused");
        check(v.rename_marking(0, "OBJ BEAR") && v.markings[0].name == "OBJ BEAR", "a marking can be renamed");
        std::vector<uint8_t> blob = v.save_state();
        World r;
        r.generate(1989, 24, 22);
        r.setup_scenario(0);
        std::string err;
        const bool loaded = r.load_state(blob.data(), blob.size(), &err);
        check(loaded && r.markings.size() == 2 && r.markings[1].name == "PL RED" && r.markings[1].xy == pl.xy &&
                  r.markings[0].kind == MK_OBJECTIVE,
              "markings survive save and load");
        std::vector<uint8_t> old = blob;
        old[7] = '7';
        check(!r.load_state(old.data(), old.size(), &err) && err.find("older") != std::string::npos,
              "a save from before the markings is refused as older");
        for (int t = 0; t < 600; ++t) {
            v.tick();
            plain.tick();
        }
        std::vector<uint8_t> a = v.save_state(), b = plain.save_state();
        // The same run with and without markings: everything up to the markings is identical, and
        // so are the sandbox flags and the scenario kind saved after them (v10, MRK-31: 3 bytes;
        // v11, three-way: 1 byte).
        const size_t tail = 4;
        size_t marks_bytes = 4;
        for (const Marking &mk : v.markings) marks_bytes += 1 + 4 + mk.name.size() + 4 + 4 * mk.xy.size();
        check(a.size() == b.size() - 4 + marks_bytes &&
                  std::equal(b.begin(), b.end() - 4 - tail, a.begin()) &&
                  std::equal(b.end() - tail, b.end(), a.end() - tail),
              "the simulation runs the same with markings as without (600 min)");
        check(v.remove_marking(0) && v.markings.size() == 1 && !v.remove_marking(5), "a marking can be deleted");
    }

    std::printf(failures ? "planning_test: %d FAILED\n" : "planning_test: all passed\n", failures);
    return failures ? 1 : 0;
}
