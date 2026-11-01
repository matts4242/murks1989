#include "murks/generator.hpp"

#include "murks/noise.hpp"
#include "murks/pathfinder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <unordered_set>

namespace murks {

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Position of a hex in kilometres.
inline void hex_km(int c, int r, double &x, double &y) {
    x = (c + 0.5 * (r & 1)) * HEX_WIDTH / 1000.0;
    y = r * ROW_SPACING / 1000.0;
}

std::string make_name(Rng &rng, bool german) {
    static const char *de_pre[] = {"Alt", "Neu", "Wend", "Kott", "Fried", "Lauch", "Ross", "Wust", "Grun", "Mark",
                                   "Brand", "Hohen", "Lieb", "Senft", "Elster", "Bern", "Zieg", "Forst", "Peitz",
                                   "Schwed", "Anger", "Witt", "Lucken", "Strau", "Prenz", "Templ", "Oder", "Kirch"};
    static const char *de_suf[] = {"au", "itz", "burg", "stadt", "dorf", "berg", "ow", "in", "hausen", "walde",
                                   "feld", "witz", "hagen", "werder"};
    static const char *pl_pre[] = {"Kra", "Wol", "Bors", "Lub", "Star", "Biel", "Zel", "Ost", "Ros", "Dob",
                                   "Glin", "Pas", "Wit", "Kor", "Mal", "Sos", "Brz", "Tor", "Gniez", "Rad",
                                   "Pul", "Sier", "Ciech", "Lom", "Zam", "Prz", "Chel", "Kut", "Plo", "Skar",
                                   "Grod", "Myszk", "Wlod", "Gost", "Kal", "Leg", "Mog", "Choj"};
    static const char *pl_suf[] = {"ow", "ice", "in", "awa", "sk", "ewo", "yn", "ka", "owo", "ec", "no", "owice",
                                   "ia", "any", "ze"};
    std::string n;
    if (german) {
        n = de_pre[rng.next() % (sizeof(de_pre) / sizeof(*de_pre))];
        n += de_suf[rng.next() % (sizeof(de_suf) / sizeof(*de_suf))];
    } else {
        n = pl_pre[rng.next() % (sizeof(pl_pre) / sizeof(*pl_pre))];
        n += pl_suf[rng.next() % (sizeof(pl_suf) / sizeof(*pl_suf))];
        if (rng.chance(0.08)) n = (rng.chance(0.5) ? "Nowy " : "Stary ") + n;
    }
    return n;
}

void trace_river(Map &map, HexCoord from, HexCoord to, uint32_t seed, bool major) {
    const int W = map.cols(), H = map.rows();
    auto clamp_h = [&](HexCoord h) {
        h.col = std::max(0, std::min(W - 1, h.col));
        h.row = std::max(0, std::min(H - 1, h.row));
        return h;
    };
    HexCoord cur = clamp_h(from);
    to = clamp_h(to);
    std::unordered_set<int> visited;
    double along = 0;
    HexCoord prev{-1, -1};
    for (int steps = 0; steps < 8000; ++steps) {
        Hex &h = map.at(cur);
        int idx = map.index(cur);
        if (h.terrain == T_WATER) break;
        bool joined = steps > 30 && (h.features & F_RIVER) && !visited.count(idx);
        if (prev.col >= 0) map.link(prev, cur, NET_RIVER);
        if (joined) break; // flows into another river
        h.features |= F_RIVER | (major ? F_MAJOR_RIVER : 0);
        visited.insert(idx);
        if (hex_distance(cur, to) <= 1) break;

        Vec2 a = hex_center(cur), b = hex_center(to);
        double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        dx /= len;
        dy /= len;
        // Meander: rotate the heading by a slowly varying angle.
        double ang = 2.8 * fbm(along / 9000.0, 0.37, seed, 4);
        double rx = dx * std::cos(ang) - dy * std::sin(ang), ry = dx * std::sin(ang) + dy * std::cos(ang);

        int best = -1;
        double best_dot = -2;
        for (int d = 0; d < 6; ++d) {
            HexCoord n = neighbor(cur, d);
            if (!map.in_bounds(n) || visited.count(map.index(n))) continue;
            Vec2 c = hex_center(n);
            double ndx = c.x - a.x, ndy = c.y - a.y, nl = std::hypot(ndx, ndy);
            double dot = (ndx * rx + ndy * ry) / nl;
            if (dot > best_dot) {
                best_dot = dot;
                best = d;
            }
        }
        if (best < 0) break;
        prev = cur;
        cur = neighbor(cur, best);
        along += HEX_WIDTH;
    }
}

} // namespace

void add_transport_network(Map &map, bool roads, bool rail, GenStats *st) {
    Pathfinder pf;
    pf.attach(&map);
    std::vector<int> path;

    auto connect = [&](int min_pop, int max_dist, auto neighbours_for, const StepCost &cost, Network net,
                       uint16_t major_flag, int major_pop) {
        std::set<std::pair<int, int>> done;
        const auto &S = map.settlements;
        for (int i = 0; i < (int)S.size(); ++i) {
            if (S[i].population < min_pop) continue;
            std::vector<std::pair<int, int>> near; // (dist, j)
            for (int j = 0; j < (int)S.size(); ++j) {
                if (j == i || S[j].population < min_pop) continue;
                int d = hex_distance(S[i].pos, S[j].pos);
                if (d <= max_dist) near.push_back({d, j});
            }
            std::sort(near.begin(), near.end());
            int k = neighbours_for(S[i].population);
            for (int n = 0; n < (int)near.size() && n < k; ++n) {
                int j = near[n].second;
                auto key = std::make_pair(std::min(i, j), std::max(i, j));
                if (done.count(key)) continue;
                done.insert(key);
                PathResult pr = pf.find(S[i].pos, S[j].pos, cost, 0.3f, path, 250000, 1.6f);
                if (!pr.found) continue;
                bool major = S[i].population >= major_pop && S[j].population >= major_pop;
                HexCoord prev = S[i].pos;
                if (major) map.at(prev).features |= major_flag;
                for (int idx : path) {
                    HexCoord h = map.coord_of(idx);
                    map.link(prev, h, net);
                    if (major) map.at(h).features |= major_flag;
                    prev = h;
                }
            }
        }
    };

    auto t0 = Clock::now();
    StepCost road_cost = [&](int from, int to) -> float {
        const Hex &a = map.at_index(from), &b = map.at_index(to);
        if (b.terrain == T_WATER) return -1;
        float c = 1.0f;
        switch (b.terrain) {
            case T_FOREST: c = 1.8f; break;
            case T_WETLAND: c = 3.5f; break;
            case T_URBAN: c = 0.9f; break;
            default: break;
        }
        if ((b.features & F_RIVER) && !(b.features & F_BRIDGE)) c += 5.0f;
        c += std::abs(b.elev - a.elev) * 0.06f;
        if (b.features & F_MAJOR_ROAD) c *= 0.3f;
        else if (b.features & F_ROAD) c *= 0.4f;
        return c;
    };
    if (roads)
        connect(0, 70, [](int pop) { return pop >= 60000 ? 5 : pop >= 15000 ? 3 : 2; }, road_cost, NET_ROAD,
                F_MAJOR_ROAD, 40000);
    if (st) st->ms_roads = ms_since(t0);

    t0 = Clock::now();
    StepCost rail_cost = [&](int from, int to) -> float {
        const Hex &a = map.at_index(from), &b = map.at_index(to);
        if (b.terrain == T_WATER) return -1;
        float c = 1.0f;
        if (b.terrain == T_FOREST) c = 1.5f;
        else if (b.terrain == T_WETLAND) c = 4.0f;
        if ((b.features & F_RIVER) && !(b.features & F_BRIDGE)) c += 6.0f;
        c += std::abs(b.elev - a.elev) * 0.2f;
        if (b.features & F_RAIL) c *= 0.35f;
        return c;
    };
    if (rail) connect(25000, 220, [](int pop) { return pop >= 100000 ? 3 : 2; }, rail_cost, NET_RAIL, 0, 1 << 30);
    if (st) st->ms_rail = ms_since(t0);
    // Wherever a road now crosses a river there is a bridge.
    for (int i = 0; i < map.hex_count(); ++i) {
        Hex &h = map.at_index(i);
        if ((h.features & F_ROAD) && (h.features & F_RIVER)) h.features |= F_BRIDGE;
    }

}

void count_map_stats(const Map &map, GenStats &st) {
    st.water = st.forest = st.wetland = st.urban = st.fields = st.open = 0;
    st.road_hexes = st.rail_hexes = st.river_hexes = st.bridges = 0;
    st.settlements = (int)map.settlements.size();
    st.cities = 0;
    for (const auto &S : map.settlements)
        if (S.population >= 60000) st.cities++;
    for (int i = 0; i < map.hex_count(); ++i) {
        const Hex &h = map.at_index(i);
        switch (h.terrain) {
            case T_WATER: st.water++; break;
            case T_FOREST: st.forest++; break;
            case T_WETLAND: st.wetland++; break;
            case T_URBAN: st.urban++; break;
            case T_FIELDS: st.fields++; break;
            default: st.open++; break;
        }
        if (h.features & F_ROAD) st.road_hexes++;
        if (h.features & F_RAIL) st.rail_hexes++;
        if (h.features & F_RIVER) st.river_hexes++;
        if (h.features & F_BRIDGE) st.bridges++;
    }
}

GenStats generate_map(Map &map, const GenParams &p) {
    GenStats st;
    auto t_all = Clock::now();
    map.init(p.chunks_x, p.chunks_y);
    const int W = map.cols(), H = map.rows();
    double Wkm, Hkm;
    hex_km(W, H, Wkm, Hkm);
    const uint32_t s = p.seed * 2654435761u;

    // ---- 1. Elevation, sea and lakes ------------------------------------------------
    auto t0 = Clock::now();
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            double x, y;
            hex_km(c, r, x, y);
            double ny = y / Hkm;
            Hex &h = map.at(c, r);

            double base = fbm(x / 140.0, y / 140.0, s + 1, 5);
            double detail = fbm(x / 16.0, y / 16.0, s + 2, 4);
            double elev = 110 + 110 * base + 28 * detail + 110 * ny;

            // Southern uplands and mountains (Sudetes / Carpathians stand-in).
            double south = smoothstep(0.74, 0.97, ny + 0.06 * fbm(x / 70.0, y / 70.0, s + 3, 3));
            double ridge = 1.0 - std::fabs(fbm(x / 38.0, y / 38.0, s + 4, 5)) * 1.7;
            if (ridge < 0) ridge = 0;
            elev += south * (180 + 1500 * ridge * ridge);
            // Scattered central uplands.
            double up = smoothstep(0.18, 0.42, fbm(x / 70.0, y / 70.0, s + 5, 4));
            elev += up * 420 * ridge;

            // Baltic coast along the north edge.
            double coast = 0.085 + 0.05 * fbm(x / 90.0, 0.5, s + 6, 3) + 0.018 * fbm(x / 12.0, y / 12.0, s + 7, 3);
            if (ny < coast) {
                h.terrain = T_WATER;
                h.elev = (int16_t)std::max(-200.0, -5.0 - (coast - ny) * 900.0);
                continue;
            }
            // Lake districts in the northern third.
            double lake = fbm(x / 7.0, y / 7.0, s + 8, 3);
            double band = smoothstep(0.10, 0.17, ny) * (1.0 - smoothstep(0.30, 0.42, ny));
            if (band > 0.3 && lake > 0.34 && base < 0.25) {
                h.terrain = T_WATER;
                h.elev = (int16_t)std::max(1.0, elev - 5);
                continue;
            }
            h.elev = (int16_t)std::max(1.0, std::min(2400.0, elev));
            h.terrain = T_OPEN;
        }
    }
    st.ms_terrain = ms_since(t0);

    // ---- 2. Rivers ---------------------------------------------------------------------
    t0 = Clock::now();
    auto at_n = [&](double nx, double ny) {
        return HexCoord{(int)(nx * (W - 1)), (int)(ny * (H - 1))};
    };
    struct Leg { double ax, ay, bx, by; bool major; };
    const Leg legs[] = {
        {0.36, 1.00, 0.25, 0.00, true},  // "Oder"
        {0.74, 1.00, 0.48, 0.55, true},  // "Vistula", upper
        {0.48, 0.55, 0.56, 0.00, true},  // "Vistula", lower
        {0.02, 0.80, 0.12, 0.00, true},  // "Elbe"
        {0.62, 0.62, 0.33, 0.45, true},  // "Warta"
        {0.99, 0.55, 0.58, 0.40, true},  // "Bug"
        {0.56, 0.30, 0.33, 0.36, false}, // "Notec"
        {0.18, 0.76, 0.10, 0.45, false}, // "Spree"
        {0.64, 0.80, 0.51, 0.60, false}, // "Pilica"
        {0.90, 0.20, 0.57, 0.18, false}, // "Narew"
        {0.47, 0.95, 0.36, 0.70, false}, // "Nysa"
    };
    int li = 0;
    for (const Leg &l : legs) trace_river(map, at_n(l.ax, l.ay), at_n(l.bx, l.by), s + 100 + li++, l.major);
    // Rivers cut shallow valleys.
    for (int i = 0; i < map.hex_count(); ++i) {
        Hex &h = map.at_index(i);
        if (h.features & F_RIVER) h.elev = (int16_t)std::max(1, h.elev - 12);
    }
    // Distance-to-river mask (0..2 hexes) for wet ground and town siting.
    std::vector<uint8_t> near_river(map.hex_count(), 0);
    for (int i = 0; i < map.hex_count(); ++i) {
        if (!(map.at_index(i).features & F_RIVER)) continue;
        HexCoord h = map.coord_of(i);
        for (int d = 0; d < 6; ++d) {
            HexCoord n1 = neighbor(h, d);
            if (!map.in_bounds(n1)) continue;
            near_river[map.index(n1)] = std::max<uint8_t>(near_river[map.index(n1)], 2);
            for (int e = 0; e < 6; ++e) {
                HexCoord n2 = neighbor(n1, e);
                if (map.in_bounds(n2)) near_river[map.index(n2)] = std::max<uint8_t>(near_river[map.index(n2)], 1);
            }
        }
        near_river[i] = 3;
    }
    st.ms_rivers = ms_since(t0);

    // ---- 3. Land cover -------------------------------------------------------------
    t0 = Clock::now();
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            Hex &h = map.at(c, r);
            if (h.terrain == T_WATER) continue;
            double x, y;
            hex_km(c, r, x, y);
            double moist = fbm(x / 25.0, y / 25.0, s + 10, 4) + 0.12 * near_river[map.index(c, r)] +
                           0.15 * smoothstep(0.75, 1.0, x / Wkm);
            double hill = smoothstep(280, 700, h.elev);
            double forest = 0.7 * fbm(x / 11.0, y / 11.0, s + 11, 5) + 0.5 * fbm(x / 60.0, y / 60.0, s + 12, 3);
            double field = fbm(x / 3.0, y / 3.0, s + 13, 2);
            if (h.elev < 170 && moist > 0.36 && !(h.features & F_RIVER)) {
                h.terrain = T_WETLAND;
                h.cover = 2;
            } else if (forest > 0.24 - 0.22 * hill) {
                h.terrain = T_FOREST;
                h.cover = (uint8_t)(14 + (hash2(c, r, s + 14) % 10));
            } else if (field > -0.12 && h.elev < 900) {
                h.terrain = T_FIELDS;
            } else {
                h.terrain = T_OPEN;
            }
        }
    }
    st.ms_terrain += ms_since(t0);

    // ---- 4. Settlements -------------------------------------------------------------
    t0 = Clock::now();
    Rng rng(p.seed * 7919ull + 13);
    const int cell = 20;
    std::unordered_set<std::string> used_names;
    for (int gy = 0; gy * cell < H; ++gy) {
        for (int gx = 0; gx * cell < W; ++gx) {
            int c = gx * cell + rng.range_int(0, cell - 1), r = gy * cell + rng.range_int(0, cell - 1);
            if (!map.in_bounds(c, r)) continue;
            Hex &h = map.at(c, r);
            if (h.terrain == T_WATER || h.terrain == T_WETLAND || h.elev > 750) continue;
            double pr = 0.32 + 0.06 * near_river[map.index(c, r)] - (h.elev > 400 ? 0.12 : 0.0);
            if (!rng.chance(pr)) continue;
            double u = std::max(1e-6, rng.uniform());
            int pop = (int)std::min(1800000.0, 1500.0 / std::pow(u, 0.9));
            double x, y;
            hex_km(c, r, x, y);
            bool german = (x / Wkm) < 0.30 + 0.05 * fbm(x / 50.0, y / 50.0, s + 15, 2);
            std::string name;
            for (int tries = 0; tries < 20; ++tries) {
                name = make_name(rng, german);
                if (!used_names.count(name)) break;
            }
            used_names.insert(name);
            Settlement st_;
            st_.name = name;
            st_.pos = {c, r};
            st_.population = pop;
            st_.radius = pop >= 600000 ? 5 : pop >= 200000 ? 3 : pop >= 60000 ? 2 : pop >= 15000 ? 1 : 0;
            map.settlements.push_back(st_);
        }
    }
    // Biggest first: they claim footprint and names first.
    std::sort(map.settlements.begin(), map.settlements.end(),
              [](const Settlement &a, const Settlement &b) { return a.population > b.population; });
    if (map.settlements.size() > 0xFFFE) map.settlements.resize(0xFFFE);
    for (int i = 0; i < (int)map.settlements.size(); ++i) {
        const Settlement &S = map.settlements[i];
        for (int dr = -S.radius; dr <= S.radius; ++dr) {
            for (int dc = -S.radius - 1; dc <= S.radius + 1; ++dc) {
                HexCoord h{S.pos.col + dc, S.pos.row + dr};
                if (!map.in_bounds(h)) continue;
                int d = hex_distance(h, S.pos);
                if (d > S.radius) continue;
                Hex &hx = map.at(h);
                if (hx.terrain == T_WATER || hx.settlement != NO_SETTLEMENT) continue;
                double x, y;
                hex_km(h.col, h.row, x, y);
                if (d >= 2 && d == S.radius && fbm(x / 1.5, y / 1.5, s + 16, 2) < 0) continue; // ragged edge
                hx.terrain = T_URBAN;
                hx.cover = (uint8_t)(S.radius >= 2 && d <= 1 ? 20 : 10);
                hx.settlement = (uint16_t)i;
            }
        }
        if (S.population >= 60000) st.cities++;
    }
    st.settlements = (int)map.settlements.size();
    st.ms_settlements = ms_since(t0);

    // ---- 5. Roads and rail ------------------------------------------------------------
    add_transport_network(map, true, true, &st);
    count_map_stats(map, st);
    st.ms_total = ms_since(t_all);
    return st;
}

} // namespace murks
