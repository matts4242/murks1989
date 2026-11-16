// Headless checks of slope in the movement rules (MRK-04): the slope factor by mobility (level
// = 1, climbs slower the steeper, descents gentler than climbs, Tobler's function on foot, roads
// taking a hill at a share of its grade), step costs on a synthetic hillside, driving times up
// and down it, routes going round a steep ridge through a pass, the planning map's going and
// slope key, and the cheapest step any grade allows. Exit code 0 = all
// passed. Usage: slope_test
#include "murks/planning.hpp"
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
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// 128 x 128 hexes of open ground at 100 m, with `shape` setting elevations. One east-west road
// along row 100.
template <class F> static std::vector<uint8_t> map_bytes(const char *name, F shape) {
    Map m;
    m.init(2, 2);
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            h.elev = (int16_t)shape(c, r);
            h.terrain = T_OPEN;
            h.cover = 0;
            h.features = 0;
        }
    for (int c = 0; c + 1 < m.cols(); ++c) m.link({c, 100}, {c + 1, 100}, NET_ROAD);
    m.name = name;
    const std::string file = std::string(name) + ".murksmap";
    m.save(file);
    std::ifstream f(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    f.close();
    std::remove(file.c_str());
    return bytes;
}

static void load(World &w, const std::vector<uint8_t> &bytes) {
    w.load_map(bytes.data(), bytes.size());
    w.reseed_rng(7);
    w.time_min = 10 * 60 - 1;
    w.formations.push_back({"Blue", SIDE_PLAYER, -1, false, 0, {}});
}

static int add(World &w, UnitType t, const char *equip, HexCoord at) {
    Unit u;
    u.id = (int)w.units.size();
    u.side = SIDE_PLAYER;
    u.type = t;
    u.name = std::string("Blue ") + unit_type_info(t).name;
    u.equipment = equip;
    u.formation = 0;
    u.pos = u.prev_pos = u.dest = at;
    u.elements_max = u.elements = World::element_count(u);
    w.formations[0].members.push_back(u.id);
    w.units.push_back(u);
    return u.id;
}

// The hillside: level at 100 m up to column 20, then rising 50 m a hex (10%) to column 40,
// level again beyond.
static int hillside(int c, int) { return 100 + 50 * std::min(20, std::max(0, c - 20)); }

int main() {
    std::printf("The slope factor:\n");
    {
        bool level = true;
        for (int mob = 0; mob <= M_FOOT; ++mob) level &= slope_factor(mob, 0.0f, false) == 1.0f;
        check(level, "level ground costs what it did (x1 for every mobility)");
        bool rising = true, descents = true, roads = true;
        for (int mob = 0; mob <= M_FOOT; ++mob) {
            float prev = 0.0f;
            for (float g = 0.0f; g <= 0.8f; g += 0.01f) {
                const float up = slope_factor(mob, g, false), down = slope_factor(mob, -g, false);
                if (up < prev - 1e-6f) rising = false;
                prev = up;
                if (down > up + 1e-6f) descents = false;
                // On foot the road's gentler grade only loses near Tobler's best, the -5% descent.
                const bool steep_down = mob != M_FOOT || g >= 0.10f;
                if (slope_factor(mob, g, true) > up + 1e-6f || (steep_down && slope_factor(mob, -g, true) > down + 1e-6f))
                    roads = false;
            }
        }
        check(rising, "the steeper the climb, the slower (never faster)");
        check(descents, "a descent is never slower than the same climb");
        check(roads, "a road never makes a climb or a steep descent slower than off-road");
        std::printf("     at +15%%: tracks x%.2f, wheels x%.2f, foot x%.2f; at +30%%: x%.2f, x%.2f, x%.2f\n",
                    slope_factor(M_TRACKED, 0.15f, false), slope_factor(M_WHEELED, 0.15f, false),
                    slope_factor(M_FOOT, 0.15f, false), slope_factor(M_TRACKED, 0.30f, false),
                    slope_factor(M_WHEELED, 0.30f, false), slope_factor(M_FOOT, 0.30f, false));
        check(near(slope_factor(M_TRACKED, 0.15f, false), 1.4f, 1e-4f) && near(slope_factor(M_WHEELED, 0.15f, false), 1.8f, 1e-4f),
              "a 15% climb: tracks x1.4, wheels x1.8");
        check(slope_factor(M_WHEELED, 0.3f, false) > slope_factor(M_TRACKED, 0.3f, false),
              "wheels suffer more than tracks on steep ground");
        check(slope_factor(M_TRACKED, -0.07f, false) < 1.05f && slope_factor(M_WHEELED, -0.30f, false) > 1.5f,
              "vehicles: gentle descents cost little (under x1.05 to -7%), steep ones slow them");
        check(near(slope_factor(M_TRACKED, -0.25f, false), slope_factor(M_TRACKED, 0.25f * SLOPE_DESCENT_SHARE, false), 1e-5f),
              "a vehicle's descent counts as a climb of 60% of the grade");
        // Tobler: 6 km/h at -5%, 5.04 km/h on the level.
        check(near(slope_factor(M_FOOT, -0.05f, false), slope_factor_min(M_FOOT), 1e-6f) &&
                  near(slope_factor(M_FOOT, -0.05f, false), 5.036f / 6.0f, 1e-3f),
              "on foot the fastest grade is a gentle descent (-5%, Tobler's 6 km/h against 5 on the level)");
        check(near(slope_factor(M_FOOT, 0.15f, false), std::exp(3.5f * 0.15f), 1e-4f), "on foot a 15% climb is Tobler's x1.7");
        check(slope_factor(M_FOOT, 2.0f, false) <= 8.0f && slope_factor(M_TRACKED, 2.0f, false) <= 10.0f &&
                  slope_factor(M_WHEELED, 2.0f, false) <= 20.0f,
              "nothing is impassable: the factor is capped (foot x8, tracks x10, wheels x20)");
        check(near(slope_factor(M_WHEELED, 0.10f, true), slope_factor(M_WHEELED, 0.10f * SLOPE_ROAD_SHARE, false), 1e-5f),
              "a road takes a hill at 70% of its grade");
    }

    std::printf("Step costs on a hillside:\n");
    {
        static const std::vector<uint8_t> bytes = map_bytes("slope-hillside", hillside);
        World w;
        load(w, bytes);
        const Map &m = w.map;
        const int a = m.index({25, 50}), b = m.index({26, 50}), f0 = m.index({5, 50}), f1 = m.index({6, 50});
        const float flat_t = w.step_minutes(M_TRACKED, MV_HASTY, f0, f1, false);
        check(near(flat_t, terrain_minutes(M_TRACKED, T_OPEN), 1e-5f), "a level step costs the terrain's minutes");
        check(near(w.step_minutes(M_TRACKED, MV_HASTY, a, b, false), flat_t * slope_factor(M_TRACKED, 0.1f, false), 1e-4f),
              "a 10% climb off-road costs the level minutes times the slope factor");
        check(near(w.step_minutes(M_WHEELED, MV_HASTY, b, a, false),
                   terrain_minutes(M_WHEELED, T_OPEN) * slope_factor(M_WHEELED, -0.1f, false), 1e-4f),
              "a 10% descent likewise");
        check(near(w.step_minutes(M_WHEELED, MV_HASTY, a, b, true),
                   road_minutes(M_WHEELED) * slope_factor(M_WHEELED, 0.1f, true), 1e-4f),
              "a road climb uses the road's minutes and the road's (gentler) factor");
        check(near(w.step_minutes(M_TRACKED, MV_DELIBERATE, a, b, false),
                   1.5f * w.step_minutes(M_TRACKED, MV_HASTY, a, b, false), 1e-4f),
              "move modes still scale the step (deliberate x1.5)");
    }

    std::printf("Driving up and down the hillside:\n");
    {
        static const std::vector<uint8_t> bytes = map_bytes("slope-hillside", hillside);
        World w;
        load(w, bytes);
        struct Case { int mob; const char *what; } cases[3] = {{M_TRACKED, "tracks"}, {M_WHEELED, "wheels"}, {M_FOOT, "foot"}};
        for (auto &cs : cases) {
            RouteEstimate up = w.route_estimate({20, 50}, {40, 50}, cs.mob, MV_HASTY);
            RouteEstimate down = w.route_estimate({40, 50}, {20, 50}, cs.mob, MV_HASTY);
            RouteEstimate flat = w.route_estimate({0, 30}, {20, 30}, cs.mob, MV_HASTY);
            std::printf("     %s, 10 km: level %.0f min, up 1,000 m %.0f min, down %.0f min\n", cs.what, flat.minutes,
                        up.minutes, down.minutes);
            const float want = slope_factor(cs.mob, 0.1f, false);
            char b[160];
            std::snprintf(b, sizeof b, "%s: climbing takes x%.2f the level time (x%.2f expected)", cs.what,
                          up.minutes / flat.minutes, want);
            check(up.found && flat.found && near(up.minutes / flat.minutes, want, 0.03f), b);
            std::snprintf(b, sizeof b, "%s: going down is quicker than going up", cs.what);
            check(down.found && down.minutes < up.minutes, b);
        }
        // A tank ordered up the hill drives the estimated time.
        int t = add(w, U_TANK, "3x T-72M", {20, 50});
        const float est = w.route_estimate({20, 50}, {40, 50}, M_TRACKED, MV_HASTY).minutes;
        const int delay = w.order_move(t, {40, 50});
        int started = -1, arrived = -1;
        for (int i = 0; i < 600 && arrived < 0; ++i) {
            w.tick();
            const Unit &u = w.units[t];
            if (started < 0 && u.moving()) started = (int)w.time_min;
            if (u.pos == HexCoord{40, 50} && !u.moving() && !u.pending.active) arrived = (int)w.time_min;
        }
        std::printf("     tank up the hill: est. %.1f min, drove %d min (delay %d)\n", est, arrived - started, delay);
        check(arrived > 0 && std::fabs((arrived - started) - est) <= est * 0.08f + 3.0f,
              "a tank ordered up the hill takes about the estimated time");
    }

    std::printf("Routes round a steep ridge:\n");
    {
        // A ridge along columns 60-63 (100, 300, 500, 300 m: 40% faces) with a level pass at rows 34-37.
        static const std::vector<uint8_t> bytes = map_bytes("slope-ridge", [](int c, int r) {
            static const int h[4] = {300, 500, 300, 100};
            if (c < 60 || c > 63 || (r >= 34 && r <= 37)) return 100;
            return h[c - 60];
        });
        World w;
        load(w, bytes);
        for (int mob = 0; mob <= M_FOOT; ++mob) {
            RouteEstimate r = w.route_estimate({40, 30}, {80, 30}, mob, MV_HASTY);
            int top = 0;
            for (int idx : r.path) top = std::max<int>(top, w.map.at_index(idx).elev);
            char b[160];
            std::snprintf(b, sizeof b, "%s ordered across the ridge goes through the pass (highest point %d m, %.0f min)",
                          mob == M_TRACKED ? "tracks" : mob == M_WHEELED ? "wheels" : "foot", top, r.minutes);
            check(r.found && top == 100, b);
        }
    }

    std::printf("Planning map:\n");
    {
        static const std::vector<uint8_t> bytes = map_bytes("slope-hillside", hillside);
        World w;
        load(w, bytes);
        const float level = hex_going(w, w.map.index({5, 50}), M_WHEELED);
        const float slope = hex_going(w, w.map.index({30, 50}), M_WHEELED);
        check(slope > level * 1.1f, "going shows a hillside slower than level ground");
        std::vector<PlanKey> key;
        plan_key(PLAN_SLOPE, key);
        bool says = key.size() >= 4;
        for (int k = 0; k < 4 && k < (int)key.size(); ++k) says &= key[k].note.find(" tracked, x") != std::string::npos;
        check(says, "the slope key gives each class's effect on movement");
    }

    std::printf("Cheapest step:\n");
    {
        // Rough ground: every step from -60% to +60% in all mobilities and move modes, on and off roads.
        static const std::vector<uint8_t> bytes = map_bytes("slope-rough", [](int c, int r) {
            return 400 + (int)(300 * std::sin(c * 1.7 + r * 0.9) * std::cos(c * 0.4 - r * 1.3));
        });
        World w;
        load(w, bytes);
        bool ok = true;
        for (int mob = 0; mob <= M_FOOT; ++mob)
            for (int mode = MV_HASTY; mode <= MV_ASSAULT; ++mode) {
                // The heuristic's minimum step (a main road), less the most a slope can take off.
                const float min_step = road_minutes(mob) * 0.8f * slope_factor_min(mob) * (mode == MV_ROAD ? 0.85f : 1.0f);
                for (int r = 1; r < 127; r += 3)
                    for (int c = 1; c < 127; c += 3)
                        for (int d = 0; d < 6; ++d) {
                            const int i = w.map.index({c, r}), j = w.map.index(neighbor({c, r}, d));
                            for (int road = 0; road < 2; ++road)
                                if (w.step_minutes(mob, mode, i, j, road != 0) < min_step - 1e-5f) ok = false;
                        }
            }
        check(ok, "no step on any grade is cheaper than a main road less the slope minimum (x0.84 on foot, x1 vehicles)");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
