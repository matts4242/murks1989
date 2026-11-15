#include "murks/planning.hpp"

#include "murks/render_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace murks {

namespace {

inline uint32_t tint8(int r, int g, int b, int a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

// Going is shown against open ground for the same mobility: the bands are ratios of the
// hex's minutes to open ground's. Upper bounds; the last band takes the rest.
const float kGoingBand[5] = {0.9f, 1.15f, 1.6f, 2.6f, 5.0f};
const uint32_t kGoingTint[6] = {
    tint8(40, 150, 190, 120),  // faster than open ground (streets for wheels)
    tint8(60, 170, 70, 110),   // as good as open ground
    tint8(170, 200, 50, 120),  // up to 1.6x slower
    tint8(245, 190, 40, 140),  // up to 2.6x
    tint8(235, 110, 30, 155),  // up to 5x
    tint8(190, 25, 35, 170),   // slower still
};
const uint32_t kImpassable = tint8(30, 45, 95, 150);

int going_band(float ratio) {
    int k = 0;
    while (k < 5 && ratio > kGoingBand[k]) ++k;
    return k;
}

// Cover: the better the protection, the stronger and cooler the tint; open ground gets a
// light warm tint (exposed).
uint32_t cover_tint(const Hex &h) {
    const bool tall = h.cover >= 20;
    switch (h.terrain) {
        case T_URBAN: return tall ? tint8(120, 50, 190, 175) : tint8(140, 80, 200, 140);
        case T_FOREST: return tall ? tint8(20, 100, 200, 165) : tint8(50, 130, 210, 130);
        case T_WETLAND: return tint8(230, 200, 90, 90);
        case T_OPEN:
        case T_FIELDS: return tint8(225, 90, 60, 70);
        default: return 0;
    }
}

const uint32_t kSlopeTint[5] = {0, tint8(240, 222, 90, 110), tint8(242, 160, 45, 140), tint8(220, 75, 35, 165),
                                tint8(140, 15, 60, 185)};

std::string kmh(float minutes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", 30.0f / minutes);
    return b;
}

} // namespace

const char *plan_mode_name(int mode) {
    static const char *names[PLAN_COUNT] = {"Off", "Going: tracked", "Going: wheeled", "Going: on foot",
                                            "Cover and concealment", "Slope", "Line of sight from a point"};
    return names[mode < 0 || mode >= PLAN_COUNT ? 0 : mode];
}

int plan_mobility(int mode) {
    switch (mode) {
        case PLAN_GOING_TRACKED: return M_TRACKED;
        case PLAN_GOING_WHEELED: return M_WHEELED;
        case PLAN_GOING_FOOT: return M_FOOT;
        default: return -1;
    }
}

float hex_going(const World &w, int idx, int mobility) {
    const Map &m = w.map;
    if (m.at_index(idx).terrain == T_WATER) return -1.0f;
    const HexCoord h = m.coord_of(idx);
    float sum = 0.0f;
    int n = 0;
    for (int d = 0; d < 6; ++d) {
        HexCoord nb = neighbor(h, d);
        if (!m.in_bounds(nb) || m.at(nb).terrain == T_WATER) continue;
        sum += w.step_minutes(mobility, MV_HASTY, m.index(nb), idx, false);
        ++n;
    }
    if (n == 0) return w.step_minutes(mobility, MV_HASTY, idx, idx, false);
    return sum / (float)n;
}

void build_plan_rgba(const World &w, int mode, std::vector<uint8_t> &rgba) {
    const Map &m = w.map;
    const int W = m.cols(), H = m.rows();
    rgba.assign((size_t)W * H * 4, 0);
    const int mob = plan_mobility(mode);
    const float open = mob >= 0 ? terrain_minutes(mob, T_OPEN) : 1.0f;
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            const int idx = m.index({c, r});
            const Hex &hx = m.at_index(idx);
            uint32_t t = 0;
            if (mob >= 0) {
                float g = hex_going(w, idx, mob);
                t = g < 0 ? kImpassable : kGoingTint[going_band(g / open)];
            } else if (mode == PLAN_COVER) {
                t = cover_tint(hx);
            } else if (mode == PLAN_SLOPE) {
                int dir;
                t = kSlopeTint[slope_class(hex_slope(m, c, r, dir))];
            }
            uint8_t *o = &rgba[((size_t)r * W + c) * 4];
            o[0] = t & 255;
            o[1] = (t >> 8) & 255;
            o[2] = (t >> 16) & 255;
            o[3] = (t >> 24) & 255;
        }
    }
}

void plan_key(int mode, std::vector<PlanKey> &out) {
    out.clear();
    const int mob = plan_mobility(mode);
    if (mob >= 0) {
        // km/h ranges per band, from open ground's minutes for this mobility.
        const float open = terrain_minutes(mob, T_OPEN);
        static const char *what[6] = {"Faster than open ground", "As good as open ground", "Up to 1.6x slower",
                                      "Up to 2.6x slower", "Up to 5x slower", "Slower still"};
        const int types[5] = {T_OPEN, T_FIELDS, T_FOREST, T_WETLAND, T_URBAN};
        static const char *tname[6] = {"water", "open ground", "fields", "forest", "wetland", "towns"};
        for (int k = 0; k < 6; ++k) {
            std::string speed;
            if (k == 0) speed = "over " + kmh(open * kGoingBand[0]) + " km/h";
            else if (k == 5) speed = "under " + kmh(open * kGoingBand[4]) + " km/h";
            else speed = kmh(open * kGoingBand[k]) + "-" + kmh(open * kGoingBand[k - 1]) + " km/h";
            std::string terrains;
            for (int t : types) {
                if (going_band(terrain_minutes(mob, t) / open) != k) continue;
                terrains += (terrains.empty() ? "" : ", ") + std::string(tname[t]);
            }
            if (k == 5) terrains += std::string(terrains.empty() ? "" : ", ") + "river hexes off the road";
            if (k == 0 && terrains.empty()) continue; // only wheels go faster in towns
            out.push_back({kGoingTint[k], std::string(what[k]) + ", " + speed,
                           terrains.empty() ? "Steep climbs push ground into a slower band." : "Level " + terrains + "."});
        }
        out.push_back({kImpassable, "Impassable", "Water: cross at bridges."});
        const float rd = road_minutes(mob);
        out.push_back({0, "On roads: " + kmh(rd) + " km/h, main roads " + kmh(rd * 0.8f) + " km/h",
                       "At full readiness; climbing adds time. Road march is faster still on roads."});
        return;
    }
    if (mode == PLAN_COVER) {
        char b[160];
        std::snprintf(b, sizeof b, "Hit chance x%.1f vehicles, x%.1f infantry. Hides units that sit still.",
                      terrain_cover_factor(T_URBAN, false), terrain_cover_factor(T_URBAN, true));
        out.push_back({tint8(120, 50, 190, 175), "Town: the best cover", b});
        std::snprintf(b, sizeof b, "Hit chance x%.1f vehicles, x%.1f infantry. Hides units that sit still.",
                      terrain_cover_factor(T_FOREST, false), terrain_cover_factor(T_FOREST, true));
        out.push_back({tint8(20, 100, 200, 165), "Forest: good cover", b});
        std::snprintf(b, sizeof b, "Hit chance x%.1f. Does not hide.", terrain_cover_factor(T_WETLAND, false));
        out.push_back({tint8(230, 200, 90, 90), "Wetland: little cover", b});
        out.push_back({tint8(225, 90, 60, 70), "Open ground and fields: exposed", "No cover. Seen at full range."});
        std::snprintf(b, sizeof b, "Hidden units are found only within %d km (%d km by recon), beyond that by chance.",
                      CONCEAL_SPOT_HEXES / 2, CONCEAL_SPOT_HEXES_RECON / 2);
        out.push_back({0, "Sitting still and not firing", b});
        out.push_back({0, "Darker: tall cover (20 m or more)", "Tall woods and town blocks also block line of sight."});
        return;
    }
    if (mode == PLAN_SLOPE) {
        out.push_back({kSlopeTint[1], "Gentle, 3-7%", "Steepest grade to a neighbouring hex."});
        out.push_back({kSlopeTint[2], "Moderate, 7-15%", ""});
        out.push_back({kSlopeTint[3], "Steep, 15-30%", ""});
        out.push_back({kSlopeTint[4], "Very steep, over 30%", ""});
        out.push_back({0, "Level ground is left clear", "Climbing costs 0.03 min a metre (vehicles), 0.04 (foot)."});
        return;
    }
    if (mode == PLAN_SIGHT) {
        char b[160];
        std::snprintf(b, sizeof b, "A vehicle-high target (%.1f m) there can be seen from the point.", SPOT_TARGET_M);
        out.push_back({sight_tint(0), "Seen", b});
        out.push_back({sight_tint(1), "Dead ground", "Hidden from the point by ground, woods or buildings."});
        std::snprintf(b, sizeof b, "Beyond %d km", SIGHT_RADIUS_HEXES / 2);
        out.push_back({sight_tint(2), b, "Not worked out."});
        out.push_back({0, "Right-click the map > Look from here", "Sight is checked as the rules do: ground and cover height."});
    }
}

uint32_t sight_tint(int kind) {
    switch (kind) {
        case 0: return tint8(255, 236, 170, 46);
        case 1: return tint8(18, 26, 60, 150);
        default: return tint8(55, 55, 60, 110);
    }
}

void viewshed(const World &w, HexCoord from, float eye_m, float target_m, int radius, Viewshed &out) {
    const Map &m = w.map;
    out.origin = {from.col - radius - 1, from.row - radius};
    out.w = 2 * radius + 3;
    out.h = 2 * radius + 1;
    out.rgba.assign((size_t)out.w * out.h * 4, 0);
    out.seen = out.total = 0;
    for (int j = 0; j < out.h; ++j) {
        for (int i = 0; i < out.w; ++i) {
            HexCoord h{out.origin.col + i, out.origin.row + j};
            uint32_t t;
            if (!m.in_bounds(h) || hex_distance(h, from) > radius) {
                t = sight_tint(2);
            } else {
                ++out.total;
                bool seen = w.line_of_sight(from, h, eye_m, target_m);
                if (seen) ++out.seen;
                t = sight_tint(seen ? 0 : 1);
            }
            uint8_t *o = &out.rgba[((size_t)j * out.w + i) * 4];
            o[0] = t & 255;
            o[1] = (t >> 8) & 255;
            o[2] = (t >> 16) & 255;
            o[3] = (t >> 24) & 255;
        }
    }
}

// --- Planning tools (MRK-26) ---

static void finish_profile(const World &w, const std::vector<HexCoord> &hexes, Profile &out) {
    out.pts.clear();
    out.climb_m = out.descent_m = out.steepest_pct = 0;
    double km = 0;
    for (size_t i = 0; i < hexes.size(); ++i) {
        const HexCoord c = hexes[i];
        if (!w.map.in_bounds(c)) break;
        if (i > 0) {
            Vec2 p = hex_center(hexes[i - 1]), q = hex_center(c);
            km += std::hypot(q.x - p.x, q.y - p.y) / 1000.0;
        }
        const Hex &h = w.map.at(c);
        ProfilePoint pp;
        pp.hex = c;
        pp.km = (float)km;
        pp.ground = std::max<int>(0, h.elev);
        pp.cover = h.cover;
        pp.terrain = h.terrain;
        if (!out.pts.empty()) {
            const ProfilePoint &b = out.pts.back();
            const int dz = pp.ground - b.ground;
            if (dz > 0) out.climb_m += dz;
            else out.descent_m -= dz;
            const float run = (pp.km - b.km) * 1000.0f;
            if (run > 1.0f) out.steepest_pct = std::max(out.steepest_pct, 100.0f * std::abs(dz) / run);
        }
        out.pts.push_back(pp);
    }
}

void profile_line(const World &w, HexCoord a, HexCoord b, float eye_m, float target_m, Profile &out) {
    std::vector<HexCoord> line;
    hex_line(a, b, line);
    finish_profile(w, line, out);
    out.eye_m = eye_m;
    out.target_m = target_m;
    out.block = out.pts.size() == line.size() ? w.sight_block(a, b, eye_m, target_m) : -2;
}

void profile_route(const World &w, const std::vector<HexCoord> &hexes, Profile &out) {
    finish_profile(w, hexes, out);
    out.eye_m = out.target_m = 0;
    out.block = -3;
}

void grid_origin(const Map &m, double &e0, double &n0) {
    if (m.epsg == 3035) {
        e0 = m.geo_origin_e;
        n0 = m.geo_origin_n;
    } else {
        e0 = 0;
        n0 = m.height_m();
    }
}

std::string grid_ref(const Map &m, double wx, double wy) {
    double e0, n0;
    grid_origin(m, e0, n0);
    const long long e = (long long)std::floor((e0 + wx) / 100.0), n = (long long)std::floor((n0 - wy) / 100.0);
    if (e < 0 || n < 0) return "";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%02lld/%02lld %03lld %03lld", (e / 1000) % 100, (n / 1000) % 100, e % 1000, n % 1000);
    return buf;
}

double grid_bearing(double ax, double ay, double bx, double by) {
    double deg = std::atan2(bx - ax, -(by - ay)) * 180.0 / 3.14159265358979323846;
    if (deg < 0) deg += 360.0;
    if (deg >= 360.0) deg -= 360.0;
    return deg;
}

} // namespace murks
