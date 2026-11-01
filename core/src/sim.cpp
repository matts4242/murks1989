#include "murks/sim.hpp"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace murks {

namespace {

constexpr int BUCKET = 32;          // hexes per spatial bucket side
constexpr int HOT_RANGE = 20;       // hexes (10 km)
constexpr int WARM_RANGE = 80;      // hexes (40 km)
constexpr int HOT_HOLD_MIN = 30;    // minutes a zone stays hot after contact ends
constexpr int RADIO_RANGE = 40;     // hexes (20 km) from HQ before couriers are needed
constexpr int MAX_PATHS_PER_TICK = 10;
constexpr int CONTACT_RANGE = 10;   // hexes (5 km): a hostile in sight this close caps fast-forward

const UnitTypeInfo kTypes[U_TYPE_COUNT] = {
    // name        mobility    spot wpn  fire  anti-arm armd  indir intv fuel/hex litres ammo_t crew
    {"HQ", M_WHEELED, 8, 2, 1.0f, 0.1f, false, false, 3, 0.10f, 600, 0.4f, 25},
    {"Tank Plt", M_TRACKED, 8, 5, 9.0f, 1.0f, true, false, 3, 0.15f, 4000, 6.0f, 16},
    {"Mech Plt", M_TRACKED, 8, 4, 6.0f, 0.8f, true, false, 3, 0.12f, 1800, 3.0f, 30},
    {"Rifle Plt", M_FOOT, 6, 2, 4.0f, 0.5f, false, false, 3, 0.0f, 0, 1.0f, 35},
    {"Recon Sect", M_WHEELED, 14, 3, 2.5f, 0.25f, false, false, 3, 0.08f, 500, 0.5f, 15},
    {"Arty Bty", M_TRACKED, 6, 30, 12.0f, 0.6f, false, true, 8, 0.12f, 2400, 18.0f, 30},
};

// Minutes per hex by mobility and terrain (water, open, fields, forest, wetland, urban).
const float kTerrainCost[3][T_COUNT] = {
    {-1, 2.5f, 3.0f, 6.0f, 14.0f, 3.0f},  // tracked
    {-1, 4.0f, 5.0f, 10.0f, 30.0f, 2.0f}, // wheeled
    {-1, 7.0f, 8.0f, 10.0f, 16.0f, 7.0f}, // foot
};
const float kRoadCost[3] = {1.2f, 0.9f, 6.0f};
const float kRiverPenalty[3] = {25.0f, 60.0f, 20.0f};

const char *kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

float cover_mod(const Hex &h, bool soft_target) {
    switch (h.terrain) {
        case T_FOREST: return soft_target ? 0.5f : 0.7f;
        case T_URBAN: return soft_target ? 0.4f : 0.6f;
        case T_WETLAND: return 0.9f;
        default: return 1.0f;
    }
}

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

} // namespace

const UnitTypeInfo &unit_type_info(int type) { return kTypes[type < 0 || type >= U_TYPE_COUNT ? 0 : type]; }

World::World() : rng_(1989) {}

GenStats World::generate(uint32_t s, int chunks_x, int chunks_y) {
    seed = s;
    GenParams p;
    p.seed = s;
    p.chunks_x = chunks_x;
    p.chunks_y = chunks_y;
    GenStats st = generate_map(map, p);
    attach_map();
    return st;
}

bool World::load_map(const uint8_t *data, size_t size) {
    if (!map.load_from_memory(data, size)) return false;
    attach_map();
    return true;
}

void World::attach_map() {
    pf_.attach(&map);
    bucket_cols_ = (map.cols() + BUCKET - 1) / BUCKET;
    bucket_rows_ = (map.rows() + BUCKET - 1) / BUCKET;
    buckets_.assign((size_t)bucket_cols_ * bucket_rows_, {});
    units.clear();
    formations.clear();
    label_land_regions();
}

// Every connected piece of land gets its own number, so a route to an island or across
// the sea is known to be impossible without running the pathfinder into the ground.
void World::label_land_regions() {
    const int n = map.cols() * map.rows();
    region_.assign((size_t)n, -1);
    std::vector<int> stack;
    int next = 0;
    for (int start = 0; start < n; ++start) {
        if (region_[start] >= 0 || map.at_index(start).terrain == T_WATER) continue;
        region_[start] = next;
        stack.push_back(start);
        while (!stack.empty()) {
            int idx = stack.back();
            stack.pop_back();
            HexCoord h = map.coord_of(idx);
            for (int d = 0; d < 6; ++d) {
                HexCoord nb = neighbor(h, d);
                if (!map.in_bounds(nb)) continue;
                int ni = map.index(nb);
                if (region_[ni] >= 0 || map.at_index(ni).terrain == T_WATER) continue;
                region_[ni] = next;
                stack.push_back(ni);
            }
        }
        ++next;
    }
}

bool World::reachable(HexCoord a, HexCoord b) const {
    if (!map.in_bounds(a) || !map.in_bounds(b)) return false;
    int ra = region_[map.index(a)], rb = region_[map.index(b)];
    return ra >= 0 && ra == rb;
}

// ------------------------------------------------------------------------------------
// Scenario
// ------------------------------------------------------------------------------------

int World::spawn(int formation, int8_t side, UnitType type, const std::string &name, const std::string &equip,
                 HexCoord near, std::vector<int> &taken) {
    // Nearest free land hex, searching outward ring by ring.
    HexCoord best = near;
    bool found = false;
    for (int radius = 0; radius < 12 && !found; ++radius) {
        for (int dr = -radius; dr <= radius && !found; ++dr) {
            for (int dc = -radius - 1; dc <= radius + 1 && !found; ++dc) {
                HexCoord h{near.col + dc, near.row + dr};
                if (!map.in_bounds(h) || hex_distance(h, near) != radius) continue;
                if (map.at(h).terrain == T_WATER) continue;
                int idx = map.index(h);
                if (std::find(taken.begin(), taken.end(), idx) != taken.end()) continue;
                best = h;
                found = true;
                taken.push_back(idx);
            }
        }
    }
    Unit u;
    u.id = (int)units.size();
    u.side = side;
    u.type = type;
    u.name = name;
    u.equipment = equip;
    u.formation = formation;
    u.slot = (int)formations[formation].members.size();
    u.pos = u.prev_pos = u.dest = best;
    u.last_seen_pos[0] = u.last_seen_pos[1] = best;
    u.morale = side == SIDE_PLAYER ? 75 : 65;
    u.elements_max = u.elements = element_count(u);
    u.ai_next = rng_.range_int(0, 30);
    formations[formation].members.push_back(u.id);
    units.push_back(u);
    return u.id;
}

void World::setup_scenario(int extra_groups) {
    units.clear();
    formations.clear();
    groups.clear();
    events_.clear();
    stock = CompanyStock{};
    outcome_ = OUT_ONGOING;
    outcome_text_.clear();
    default_standoff = 0;
    stats_.kills = stats_.losses = 0;
    time_min = 6 * 60; // 14 April 1989, 06:00
    rng_.reseed(seed * 31ull + 7);
    const auto &S = map.settlements;
    if (S.empty()) return;

    auto settlement_near = [&](double nx, double ny, int min_pop, int max_pop) {
        HexCoord want{(int)(nx * map.cols()), (int)(ny * map.rows())};
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < (int)S.size(); ++i) {
            if (S[i].population < min_pop || S[i].population > max_pop) continue;
            int d = hex_distance(want, S[i].pos);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        return best;
    };

    // ---- The player's company -------------------------------------------------------
    player_home = settlement_near(0.38, 0.52, 8000, 250000);
    HexCoord home = S[player_home].pos;
    formations.push_back({"Company Murks", SIDE_PLAYER, player_home, false, 0, {}});
    std::vector<int> taken;
    spawn(0, SIDE_PLAYER, U_HQ, "Murks HQ", "UAZ-469, R-145 radio", home, taken);
    spawn(0, SIDE_PLAYER, U_TANK, "1st Tank", "4x T-55A", {home.col + 3, home.row}, taken);
    spawn(0, SIDE_PLAYER, U_TANK, "2nd Tank", "4x T-55A", {home.col + 3, home.row + 2}, taken);
    spawn(0, SIDE_PLAYER, U_MECH, "1st Mech", "3x BMP-1", {home.col + 2, home.row - 2}, taken);
    spawn(0, SIDE_PLAYER, U_MECH, "2nd Mech", "3x BMP-1", {home.col + 4, home.row + 1}, taken);
    spawn(0, SIDE_PLAYER, U_MECH, "3rd Mech", "3x BTR-60PB", {home.col + 1, home.row + 3}, taken);
    spawn(0, SIDE_PLAYER, U_INFANTRY, "1st Rifle", "35 men, RPG-7", {home.col, home.row - 1}, taken);
    spawn(0, SIDE_PLAYER, U_INFANTRY, "2nd Rifle", "35 men, RPG-7", {home.col - 1, home.row + 1}, taken);
    spawn(0, SIDE_PLAYER, U_RECON, "1st Recon", "2x BRDM-2", {home.col + 6, home.row - 1}, taken);
    spawn(0, SIDE_PLAYER, U_RECON, "2nd Recon", "2x BRDM-2", {home.col + 6, home.row + 3}, taken);
    spawn(0, SIDE_PLAYER, U_ARTILLERY, "Battery", "4x 2S1 Gvozdika", {home.col - 3, home.row}, taken);

    // ---- Hostile strike group heading for the company's town ---------------------------
    int a_town = settlement_near(0.38 + 120.0 / map.cols() * 1.0, 0.50, 2000, 400000);
    {
        HexCoord p = S[a_town].pos;
        formations.push_back({"7th Guards Remnant", SIDE_HOSTILE, player_home, true, 0, {}});
        int f = (int)formations.size() - 1;
        std::vector<int> tk;
        spawn(f, SIDE_HOSTILE, U_HQ, "7G HQ", "BTR-60PU", p, tk);
        spawn(f, SIDE_HOSTILE, U_TANK, "7G 1st Tank", "3x T-72M", {p.col - 2, p.row}, tk);
        spawn(f, SIDE_HOSTILE, U_TANK, "7G 2nd Tank", "3x T-72M", {p.col - 2, p.row + 2}, tk);
        spawn(f, SIDE_HOSTILE, U_MECH, "7G 1st Mech", "3x BMP-2", {p.col - 1, p.row - 2}, tk);
        spawn(f, SIDE_HOSTILE, U_MECH, "7G 2nd Mech", "3x BMP-2", {p.col - 3, p.row + 1}, tk);
        spawn(f, SIDE_HOSTILE, U_INFANTRY, "7G Rifles", "30 men", {p.col, p.row + 1}, tk);
        spawn(f, SIDE_HOSTILE, U_RECON, "7G Recon", "BRDM-2", {p.col - 4, p.row}, tk);
        spawn(f, SIDE_HOSTILE, U_ARTILLERY, "7G Battery", "3x 2S1", {p.col + 2, p.row}, tk);
    }

    // ---- Patrolling bands across the map (also the scale stress test) ----------------
    static const char *band_names[] = {"Black Eagle Militia", "Free Corps Brandt", "Wolny Legion", "Red Banner Remnant",
                                       "River Wolves", "Kowal's Raiders", "Grey Coats", "Iron Road Band",
                                       "Northern Levy", "Lake Rangers", "3rd Rifle Remnant", "Ostmark Guard"};
    for (int g = 0; g < extra_groups; ++g) {
        int town = -1;
        for (int tries = 0; tries < 50; ++tries) {
            int i = rng_.range_int(0, (int)S.size() - 1);
            if (hex_distance(S[i].pos, home) >= 60) {
                town = i;
                break;
            }
        }
        if (town < 0) continue;
        std::string fname = band_names[g % 12];
        if (g >= 12) fname += fmt(" %d", g / 12 + 1);
        formations.push_back({fname, SIDE_HOSTILE, town, false, 0, {}});
        int f = (int)formations.size() - 1;
        std::vector<int> tk;
        HexCoord p = S[town].pos;
        int n = rng_.range_int(6, 10);
        for (int k = 0; k < n; ++k) {
            double roll = rng_.uniform();
            UnitType t = k == 0 ? U_HQ : roll < 0.2 ? U_TANK : roll < 0.45 ? U_MECH : roll < 0.8 ? U_INFANTRY
                                       : roll < 0.92 ? U_RECON : U_ARTILLERY;
            const char *equip = t == U_TANK ? "T-55" : t == U_MECH ? "BTR-70" : t == U_RECON ? "technicals"
                              : t == U_ARTILLERY ? "D-30 howitzers" : t == U_HQ ? "trucks" : "riflemen";
            spawn(f, SIDE_HOSTILE, t, fmt("%.10s %d", fname.c_str(), k + 1), equip,
                  {p.col + rng_.range_int(-3, 3), p.row + rng_.range_int(-3, 3)}, tk);
        }
    }

    for (auto &u : units) {
        if (u.side == SIDE_PLAYER) stock.personnel += unit_type_info(u.type).crew;
    }
    for (int f = 0; f < (int)formations.size(); ++f) formations[f].start_combat = effective_combat_units(f);
    push_event(EV_INFO, -1, false,
               fmt("Company Murks is billeted at %s. The 7th Guards Remnant is moving on the town from the east: "
                   "break it before it breaks you.", S[player_home].name.c_str()));
    rebuild_buckets();
    update_zones();
}

// ------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------

void World::push_event(EventKind k, int unit, bool pause, const std::string &text) {
    events_.push_back({time_min, k, unit, pause, text});
    if (pause) alert_ = true;
    if (events_.size() > 500) events_.erase(events_.begin(), events_.begin() + 100);
}

std::vector<Event> World::drain_events() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

std::string World::place_name(HexCoord h) const {
    int s = map.nearest_settlement(h, 60);
    if (s < 0) return "in open country";
    const Settlement &S = map.settlements[s];
    int d = hex_distance(h, S.pos);
    if (d <= S.radius + 1) return "in " + S.name;
    Vec2 a = hex_center(S.pos), b = hex_center(h);
    double ang = std::atan2(-(b.y - a.y), b.x - a.x) * 180.0 / 3.14159265358979323846; // 0 = east, 90 = north
    static const char *dirs[8] = {"E", "NE", "N", "NW", "W", "SW", "S", "SE"};
    int k = (int)std::floor((ang + 360.0 + 22.5) / 45.0) % 8;
    return fmt("%d km %s of %s", (d + 1) / 2, dirs[k], S.name.c_str());
}

bool World::is_night() const {
    int hour = (int)((time_min / 60) % 24);
    return hour >= 21 || hour < 5;
}

std::string World::time_string_at(int64_t minutes) {
    int64_t day = 13 + minutes / 1440; // 0-based day of April
    int month = 3;
    while (day >= kMonthDays[month % 12]) {
        day -= kMonthDays[month % 12];
        month++;
    }
    int year = 1989 + month / 12;
    int hm = (int)(minutes % 1440);
    return fmt("%02d %s %d  %02d:%02d", (int)day + 1, kMonths[month % 12], year, hm / 60, hm % 60);
}

int World::element_count(const Unit &u) {
    // "4x T-55A" -> 4 vehicles; "35 men" -> 3 squads of about 11; otherwise the type's usual size.
    const std::string &e = u.equipment;
    size_t i = 0;
    int n = 0;
    while (i < e.size() && e[i] >= '0' && e[i] <= '9') n = n * 10 + (e[i++] - '0');
    if (n > 0 && i < e.size() && e[i] == 'x') return n;
    if (n > 0 && e.find(" men", i) != std::string::npos) return std::max(1, (int)std::lround(n / 11.0));
    static const int kDefault[U_TYPE_COUNT] = {2, 3, 3, 3, 2, 4}; // HQ, tank, mech, infantry, recon, artillery
    return kDefault[u.type < U_TYPE_COUNT ? u.type : 0];
}

EngagementTime World::engagement_time(const Unit &u) const {
    const auto &ti = unit_type_info(u.type);
    EngagementTime e;
    e.base_cycle = (float)ti.fire_interval;
    e.base_acquire = ti.indirect ? 4.0f : 1.0f; // a fire mission is plotted and laid; direct fire picks up the target
    e.f_readiness = 1.0f + (100.0f - u.readiness) / 100.0f;
    e.f_morale = 1.0f + std::max(0.0f, 60.0f - u.morale) / 60.0f;
    e.f_ammo = u.ammo >= 25.0f ? 1.0f : 1.0f + (25.0f - u.ammo) / 25.0f;
    e.f_fire = time_min - u.last_targeted <= 5 ? 1.3f : 1.0f;
    e.factor = e.f_readiness * e.f_morale * e.f_ammo * e.f_fire;
    e.acquire = e.base_acquire * e.factor;
    e.cycle = e.base_cycle * e.factor;
    return e;
}

int World::hq_of(int side) const {
    for (const auto &u : units)
        if (u.alive && u.side == side && u.type == U_HQ) return u.id;
    return -1;
}

bool World::road_step(int from_idx, int to_idx) const {
    const Hex &a = map.at_index(from_idx), &b = map.at_index(to_idx);
    if (!(a.features & F_ROAD) || !(b.features & F_ROAD)) return false;
    HexCoord ha = map.coord_of(from_idx), hb = map.coord_of(to_idx);
    int dir = Map::direction_to(ha, hb);
    return dir >= 0 && map.linked(ha, dir, NET_ROAD);
}

float World::move_cost(const Unit &u, int from_idx, int to_idx, bool planning) const {
    const Hex &a = map.at_index(from_idx), &b = map.at_index(to_idx);
    if (b.terrain == T_WATER) return -1;
    const int mob = unit_type_info(u.type).mobility;
    const bool road = road_step(from_idx, to_idx);
    float c = road ? kRoadCost[mob] : kTerrainCost[mob][b.terrain];
    if (road && (a.features & F_MAJOR_ROAD) && (b.features & F_MAJOR_ROAD)) c *= 0.8f;
    if (!road && (b.features & F_RIVER)) c += kRiverPenalty[mob];
    int dz = b.elev - a.elev;
    c += (dz > 0 ? dz : 0) * (mob == M_FOOT ? 0.04f : 0.03f) + std::abs(dz) * 0.004f;
    switch (u.mode) {
        case MV_ROAD: c *= road ? 0.85f : (planning ? 1.8f : 1.0f); break;
        case MV_DELIBERATE: c *= 1.5f; break;
        case MV_ASSAULT: c *= 1.15f; break;
        default: break;
    }
    return c;
}

bool World::line_of_sight(HexCoord a, HexCoord b, float eye, float target) const {
    ++los_checks_;
    thread_local std::vector<HexCoord> line;
    hex_line(a, b, line);
    const int n = (int)line.size() - 1;
    if (n <= 1) return true;
    auto ground = [&](HexCoord h) { return (float)std::max<int>(0, map.at(h).elev); };
    float ea = ground(a) + eye, eb = ground(b) + target;
    for (int i = 1; i < n; ++i) {
        if (!map.in_bounds(line[i])) return false;
        const Hex &h = map.at(line[i]);
        float obstacle = std::max<int>(0, h.elev) + h.cover;
        float ray = ea + (eb - ea) * ((float)i / n);
        if (obstacle > ray) return false;
    }
    return true;
}

float World::effective_spot_range(const Unit &u) const {
    float r = (float)unit_type_info(u.type).spot_range;
    if (is_night()) r = std::max(2.0f, r * 0.35f);
    return r;
}

void World::rebuild_buckets() {
    for (auto &b : buckets_) b.clear();
    for (const auto &u : units) {
        if (!u.alive) continue;
        int bx = u.pos.col / BUCKET, by = u.pos.row / BUCKET;
        buckets_[(size_t)by * bucket_cols_ + bx].push_back(u.id);
    }
}

template <typename F> void World::for_units_near(HexCoord h, int radius, F &&fn) const {
    int br = (radius + BUCKET - 1) / BUCKET;
    int bx0 = h.col / BUCKET, by0 = h.row / BUCKET;
    for (int by = std::max(0, by0 - br); by <= std::min(bucket_rows_ - 1, by0 + br); ++by)
        for (int bx = std::max(0, bx0 - br); bx <= std::min(bucket_cols_ - 1, bx0 + br); ++bx)
            for (int id : buckets_[(size_t)by * bucket_cols_ + bx]) fn(units[id]);
}

// ------------------------------------------------------------------------------------
// Orders
// ------------------------------------------------------------------------------------

int World::hq_above(const Unit &u) const {
    if (u.group >= 0 && u.group < (int)groups.size() && groups[u.group].alive && groups[u.group].leader != u.id) {
        int l = groups[u.group].leader;
        if (l >= 0 && units[l].alive) return l;
    }
    if (u.type == U_HQ) return -1; // the company HQ answers to the client, not to an HQ on the map
    for (int id : formations[u.formation].members)
        if (units[id].alive && units[id].type == U_HQ) return id;
    return -1;
}

int World::command_zone(const Unit &hq) const {
    return hq.type == U_HQ ? HQ_COMMAND_ZONE : LEADER_COMMAND_ZONE;
}

int World::command_delay(const Unit &u) const {
    float d = u.side == SIDE_PLAYER ? 6.0f : 14.0f;
    const bool member = u.group >= 0 && u.group < (int)groups.size() && groups[u.group].alive &&
                        groups[u.group].leader != u.id;
    if (u.type != U_HQ || member) {
        d += member ? 7.0f : 4.0f; // one step down the chain; a team member is one step further
        int hq = hq_above(u);
        if (hq < 0) {
            d += 25.0f; // no HQ: orders improvised
        } else {
            int dist = hex_distance(u.pos, units[hq].pos);
            if (dist > RADIO_RANGE) d += (dist * 0.5f) / 40.0f * 60.0f; // courier at 40 km/h
            else if (dist > command_zone(units[hq])) d *= 1.5f;     // outside the zone of command: relayed
        }
    }
    if (u.zone == Z_HOT && (time_min - u.last_fired < 10 || time_min - u.last_hit < 10)) d *= 1.5f;
    d *= 1.0f + (100.0f - u.readiness) / 200.0f;
    return (int)std::ceil(d);
}

int World::order_move(int unit_id, HexCoord target, MoveMode mode) {
    if (unit_id < 0 || unit_id >= (int)units.size()) return -1;
    Unit &u = units[unit_id];
    if (!u.alive || !map.in_bounds(target)) return -1;
    if (map.at(target).terrain == T_WATER) return -1;
    int delay = command_delay(u);
    u.pending = {true, O_MOVE, target, time_min, time_min + delay, mode};
    return delay;
}

int World::order_fall_back(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    Unit &u = units[unit_id];
    int delay = command_delay(u);
    u.pending = {true, O_RETREAT, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

int World::order_regroup(int unit_id, bool rest) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    Unit &u = units[unit_id];
    int delay = command_delay(u);
    u.pending = {true, rest ? O_REST : O_REGROUP, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

int World::set_stance(int unit_id, int roe, int brk) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    Unit &u = units[unit_id];
    int delay = command_delay(u);
    // -1 keeps the current (or already ordered) value, so ROE and break-contact can be set separately.
    uint8_t cur_roe = u.pending_stance.active ? u.pending_stance.roe : u.roe;
    uint8_t cur_brk = u.pending_stance.active ? u.pending_stance.brk : u.brk;
    if (!u.pending_stance.active) u.pending_stance.standoff = u.standoff;
    u.pending_stance.roe = (uint8_t)(roe < 0 ? cur_roe : std::min(roe, 2));
    u.pending_stance.brk = (uint8_t)(brk < 0 ? cur_brk : std::min(brk, 2));
    u.pending_stance.active = true;
    u.pending_stance.activates_at = time_min + delay;
    return delay;
}

int World::set_standoff(int unit_id, int standoff) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    Unit &u = units[unit_id];
    int delay = command_delay(u);
    if (!u.pending_stance.active) {
        u.pending_stance.roe = u.roe;
        u.pending_stance.brk = u.brk;
    }
    u.pending_stance.standoff = (uint8_t)(standoff == STANDOFF_DEFAULT || standoff == STANDOFF_WEAPONS
                                              ? standoff
                                              : std::max(0, std::min(standoff, 20)));
    u.pending_stance.active = true;
    u.pending_stance.activates_at = time_min + delay;
    return delay;
}

int World::standoff_hexes(const Unit &u) const {
    uint8_t v = u.standoff == STANDOFF_DEFAULT ? (u.side == SIDE_PLAYER ? default_standoff : 0) : u.standoff;
    if (v == STANDOFF_DEFAULT) v = 0;
    if (v == STANDOFF_WEAPONS) return unit_type_info(u.type).weapon_range;
    return v;
}

// Stand-off (a standing order, so no command delay): a unit set to keep its distance halts
// before it closes inside that range of a known enemy, and pulls back out to it when an enemy
// comes closer. An assault ignores it; team members go by their leader's.
void World::update_standoff() {
    for (auto &u : units) {
        if (!u.alive || u.order == O_RETREAT || u.mode == MV_ASSAULT) continue;
        if (u.follow && u.group >= 0) continue;
        const int S = standoff_hexes(u);
        if (S <= 0) continue;
        // Nearest enemy this side knows about (seen now, or seen in the last 10 minutes).
        int best = -1, bd = 1 << 30;
        HexCoord bpos;
        for_units_near(u.pos, S + 8, [&](const Unit &o) {
            if (!o.alive || o.side == u.side) return;
            HexCoord at;
            if (seen_by(o, u.side)) at = o.pos;
            else if (time_min - o.last_seen[u.side] <= 10) at = o.last_seen_pos[u.side];
            else return;
            int d = hex_distance(u.pos, at);
            if (d < bd) {
                bd = d;
                best = o.id;
                bpos = at;
            }
        });
        if (best < 0) continue;
        // Moving in: stop at the stand-off line.
        if (u.moving() && u.order == O_MOVE) {
            HexCoord next = map.coord_of(u.path[u.path_i]);
            if (hex_distance(next, bpos) < S && hex_distance(next, bpos) < bd) {
                u.path.clear();
                u.path_i = 0;
                u.progress = 0;
                u.order = O_HOLD;
                u.dest = u.pos;
                if (u.side == SIDE_PLAYER)
                    push_event(EV_INFO, u.id, false,
                               fmt("%s halts at its %.1f km stand-off from a hostile %s.", u.name.c_str(), S * 0.5,
                                   unit_type_info(units[best].type).name));
            }
            continue;
        }
        // Enemy inside the stand-off: pull back out to it (checked every 5 minutes).
        if (bd >= S || u.moving() || time_min < u.standoff_next) continue;
        u.standoff_next = time_min + 5;
        Vec2 a = hex_center(u.pos), b = hex_center(bpos);
        double dx = a.x - b.x, dy = a.y - b.y, l = std::max(1.0, std::hypot(dx, dy));
        double back_m = (S - bd + 1) * 500.0;
        HexCoord t = world_to_hex(a.x + dx / l * back_m, a.y + dy / l * back_m);
        if (!map.in_bounds(t) || map.at(t).terrain == T_WATER || !reachable(u.pos, t)) continue;
        MoveMode keep = u.mode;
        u.mode = MV_HASTY;
        start_path(u, t, O_MOVE, false);
        if (!u.moving()) {
            u.mode = keep;
            continue;
        }
        if (u.side == SIDE_PLAYER)
            push_event(EV_INFO, u.id, false,
                       fmt("%s pulls back to keep %.1f km from a hostile %s.", u.name.c_str(), S * 0.5,
                           unit_type_info(units[best].type).name));
    }
}

HexCoord World::regroup_point(const Unit &u) const {
    static const int ring[12][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1},
                                    {1, -1}, {-1, -1}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
    int hq = -1;
    for (int id : formations[u.formation].members)
        if (units[id].alive && units[id].type == U_HQ) hq = id;
    if (hq < 0 || hq == u.id) return u.pos;
    HexCoord base = units[hq].pos;
    // Where the HQ is heading counts, so the company gathers where the HQ will be.
    if (units[hq].moving()) base = units[hq].dest;
    const int *o = ring[u.slot % 12];
    HexCoord t{base.col + o[0], base.row + o[1]};
    if (!map.in_bounds(t) || map.at(t).terrain == T_WATER) t = base;
    return t;
}

HexCoord World::fall_back_point(const Unit &u) const {
    int nearest = -1, nd = 1 << 30;
    for_units_near(u.pos, 24, [&](const Unit &o) {
        if (o.side == u.side || !o.alive || time_min - o.last_seen[u.side] > 60) return;
        int d = hex_distance(u.pos, o.last_seen_pos[u.side]);
        if (d < nd) {
            nd = d;
            nearest = o.id;
        }
    });
    if (nearest < 0) return regroup_point(u);
    Vec2 a = hex_center(u.pos), b = hex_center(units[nearest].last_seen_pos[u.side]);
    double dx = a.x - b.x, dy = a.y - b.y, l = std::max(1.0, std::hypot(dx, dy));
    HexCoord t = world_to_hex(a.x + dx / l * 6000.0, a.y + dy / l * 6000.0);
    if (!map.in_bounds(t) || map.at(t).terrain == T_WATER) return regroup_point(u);
    return t;
}

int World::order_hold(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size()) return -1;
    Unit &u = units[unit_id];
    if (!u.alive) return -1;
    int delay = command_delay(u);
    u.pending = {true, O_HOLD, u.pos, time_min, time_min + delay};
    return delay;
}

void World::start_path(Unit &u, HexCoord target, OrderKind kind, bool announce) {
    u.path.clear();
    u.path_i = 0;
    u.progress = 0;
    u.order = kind;
    if (kind == O_HOLD || target == u.pos) {
        u.order = O_HOLD;
        u.dest = u.pos;
        return;
    }
    const auto &ti = unit_type_info(u.type);
    float min_step = kRoadCost[ti.mobility] * 0.8f * (u.mode == MV_ROAD ? 0.85f : 1.0f);
    if (!reachable(u.pos, target)) {
        u.order = O_HOLD;
        u.dest = u.pos;
        if (announce)
            push_event(EV_INFO, u.id, false, fmt("%s: no route to the objective (across water).", u.name.c_str()));
        return;
    }
    // Hexes held by enemies this side knows about are closed to anything but an assault. The
    // destination stays open so an order onto an enemy brings the unit up to it (it then halts
    // in the hex before, see update_movement).
    std::vector<int> closed;
    if (u.mode != MV_ASSAULT) {
        const int tgt_idx = map.index(target);
        for (const auto &o : units) {
            if (!o.alive || o.side == u.side) continue;
            HexCoord at;
            if (seen_by(o, u.side)) at = o.pos;
            else if (time_min - o.last_seen[u.side] <= 30) at = o.last_seen_pos[u.side];
            else continue;
            if (!map.in_bounds(at) || hex_distance(at, u.pos) > 400) continue;
            int idx = map.index(at);
            if (idx != tgt_idx) closed.push_back(idx);
        }
    }
    PathResult pr = pf_.find(
        u.pos, target,
        [&](int a, int b) {
            for (int c : closed)
                if (c == b) return -1.0f;
            return move_cost(u, a, b, true);
        },
        min_step, u.path, 400000, 1.3f);
    ++paths_this_tick_;
    if (!pr.found || u.path.empty()) {
        u.order = O_HOLD;
        u.dest = u.pos;
        if (announce) push_event(EV_INFO, u.id, false, fmt("%s: no route to the objective.", u.name.c_str()));
        return;
    }
    u.dest = target;
    deploy_if_off_road(u);
    u.step_cost = move_cost(u, map.index(u.pos), u.path[0]);
    if (announce) {
        float cost = move_cost(u, map.index(u.pos), u.path[0]);
        for (size_t i = 1; i < u.path.size(); ++i) cost += std::max(0.0f, move_cost(u, u.path[i - 1], u.path[i]));
        int mins = (int)cost;
        push_event(EV_ORDER, u.id, false,
                   fmt("%s moving: %.0f km, about %dh%02d.", u.name.c_str(), u.path.size() * 0.5, mins / 60, mins % 60));
    }
}

void World::update_orders() {
    for (auto &u : units) {
        if (!u.alive) continue;
        if (u.pending_stance.active && time_min >= u.pending_stance.activates_at) {
            u.pending_stance.active = false;
            u.roe = u.pending_stance.roe;
            u.brk = u.pending_stance.brk;
            u.standoff = u.pending_stance.standoff;
        }
        if (!u.pending.active || time_min < u.pending.activates_at) continue;
        if (paths_this_tick_ >= MAX_PATHS_PER_TICK) continue; // spread heavy route-finding over ticks
        u.pending.active = false;
        u.mode = u.pending.mode;
        u.resting = false;
        OrderKind kind = u.pending.kind;
        HexCoord target = u.pending.target;
        if (kind == O_REGROUP || kind == O_REST) target = regroup_point(u);
        else if (kind == O_RETREAT) target = fall_back_point(u);
        start_path(u, target, kind == O_REGROUP || kind == O_REST ? O_MOVE : kind, u.side == SIDE_PLAYER);
        if (kind == O_REGROUP || kind == O_REST) {
            if (u.moving()) u.order = kind;
            u.resting = kind == O_REST;
        }
    }
}

// ------------------------------------------------------------------------------------
// Tick
// ------------------------------------------------------------------------------------

int World::advance(int ticks, double budget_ms) {
    auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    for (; done < ticks; ++done) {
        alert_ = false;
        tick();
        if (alert_) {
            ++done;
            break;
        }
        if ((done & 3) == 3) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > budget_ms) {
                ++done;
                break;
            }
        }
    }
    return done;
}

void World::tick() {
    auto t0 = std::chrono::steady_clock::now();
    los_checks_ = 0;
    paths_this_tick_ = 0;
    ++time_min;

    update_orders();
    update_groups();
    update_movement();
    if (time_min % 5 == 0) {
        rebuild_buckets();
        update_zones();
    }
    update_spotting(time_min % 5 == 0);
    update_combat();
    update_morale_and_sop();
    update_standoff();
    update_ai();
    if (time_min % 60 == 0) update_logistics();
    if (time_min % 5 == 0) check_outcome();

    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    stats_.avg_tick_us = stats_.ticks == 0 ? us : stats_.avg_tick_us * 0.98 + us * 0.02;
    stats_.max_tick_us = std::max(stats_.max_tick_us * 0.999, us);
    stats_.los_checks_last_tick = los_checks_;
    stats_.paths_last_tick = paths_this_tick_;
    ++stats_.ticks;
}

// ------------------------------------------------------------------------------------
// Teams and formations
// ------------------------------------------------------------------------------------

int World::create_group(const std::vector<int> &ids, int leader) {
    if (ids.empty() || std::find(ids.begin(), ids.end(), leader) == ids.end()) return -1;
    for (int id : ids) {
        if (id < 0 || id >= (int)units.size() || !units[id].alive) return -1;
        if (units[id].side != units[leader].side || units[id].formation != units[leader].formation) return -1;
    }
    // Units leave any team they were in; a team losing its leader this way is disbanded.
    for (int id : ids) {
        int og = units[id].group;
        if (og < 0 || og >= (int)groups.size() || !groups[og].alive) continue;
        Group &o = groups[og];
        if (o.leader == id) {
            disband_group(og);
            continue;
        }
        for (size_t k = 0; k < o.members.size(); ++k) {
            if (o.members[k] != id) continue;
            o.members.erase(o.members.begin() + k);
            o.keep_dx.erase(o.keep_dx.begin() + k);
            o.keep_dy.erase(o.keep_dy.begin() + k);
            break;
        }
        units[id].group = -1;
        units[id].follow = false;
        if (o.members.empty()) disband_group(og);
    }
    int gid = -1;
    for (int i = 0; i < (int)groups.size(); ++i)
        if (!groups[i].alive) {
            gid = i;
            break;
        }
    if (gid < 0) {
        gid = (int)groups.size();
        groups.emplace_back();
    }
    bool used[8] = {};
    for (const auto &g : groups)
        if (g.alive) used[g.color % 8] = true;
    int color = 0;
    while (color < 7 && used[color]) ++color;
    Group g;
    g.alive = true;
    g.side = units[leader].side;
    g.formation = units[leader].formation;
    g.leader = leader;
    g.color = (uint8_t)color;
    g.name = std::string("Team ") + (char)('A' + color);
    Vec2 lc = hex_center(units[leader].pos);
    for (int id : ids) {
        units[id].group = gid;
        units[id].formation_wait = false;
        if (id == leader) {
            units[id].follow = false;
            continue;
        }
        units[id].follow = true;
        g.members.push_back(id);
        Vec2 mc = hex_center(units[id].pos);
        g.keep_dx.push_back((float)(mc.x - lc.x));
        g.keep_dy.push_back((float)(mc.y - lc.y));
    }
    groups[gid] = g;
    return gid;
}

bool World::disband_group(int gid) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return false;
    Group &g = groups[gid];
    auto release = [&](int id) {
        if (id < 0 || id >= (int)units.size() || units[id].group != gid) return;
        units[id].group = -1;
        units[id].follow = false;
        units[id].formation_wait = false;
    };
    release(g.leader);
    for (int id : g.members) release(id);
    g = Group{};
    return true;
}

bool World::set_group_shape(int gid, int shape, int spacing) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return false;
    Group &g = groups[gid];
    if (shape >= 0 && shape < FS_COUNT) g.shape = (uint8_t)shape;
    if (spacing > 0) g.spacing = std::min(spacing, 8);
    if (g.shape == FS_KEEP) {
        // "Keep layout" locks the team's present arrangement around its leader.
        Vec2 lc = hex_center(units[g.leader].pos);
        for (size_t k = 0; k < g.members.size(); ++k) {
            Vec2 mc = hex_center(units[g.members[k]].pos);
            g.keep_dx[k] = (float)(mc.x - lc.x);
            g.keep_dy[k] = (float)(mc.y - lc.y);
        }
    }
    for (int id : g.members) units[id].follow = true;
    g.has_dest = false; // members take up their new slots
    return true;
}

int World::order_group_move(int gid, HexCoord target, MoveMode mode) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return -1;
    Group &g = groups[gid];
    for (int id : g.members) {
        units[id].follow = true;
        units[id].pending.active = false; // the team order replaces anything still on its way
    }
    return order_move(g.leader, target, mode);
}

int World::order_group_hold(int gid) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return -1;
    Group &g = groups[gid];
    for (int id : g.members) {
        units[id].follow = true;
        units[id].pending.active = false;
    }
    return order_hold(g.leader);
}

void World::detach(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size()) return;
    Unit &u = units[unit_id];
    if (u.group < 0 || u.group >= (int)groups.size() || groups[u.group].leader == unit_id) return;
    u.follow = false;
    u.formation_wait = false;
}

HexCoord World::slot_for(const Group &g, int k, HexCoord center, float heading) const {
    const Vec2 c = hex_center(center);
    const double fx = std::cos(heading), fy = std::sin(heading); // forward
    const double rx = -fy, ry = fx;                              // right
    const double S = g.spacing * 500.0;
    const int i = k / 2 + 1;
    const double side = (k % 2 == 0) ? 1.0 : -1.0;
    double dx = 0, dy = 0;
    switch (g.shape) {
        case FS_COLUMN: dx = -(k + 1) * S * fx; dy = -(k + 1) * S * fy; break;
        case FS_LINE: dx = side * i * S * rx; dy = side * i * S * ry; break;
        case FS_WEDGE: dx = side * i * S * rx - i * S * fx; dy = side * i * S * ry - i * S * fy; break;
        default:
            if (k < (int)g.keep_dx.size()) {
                dx = g.keep_dx[k];
                dy = g.keep_dy[k];
            }
            break;
    }
    HexCoord h = world_to_hex(c.x + dx, c.y + dy);
    if (!map.in_bounds(h) || map.at(h).terrain == T_WATER || !reachable(center, h)) return center;
    return h;
}

void World::update_groups() {
    for (int gid = 0; gid < (int)groups.size(); ++gid) {
        Group &g = groups[gid];
        if (!g.alive) continue;
        // Drop the dead; if the leader is gone, the next in line takes over (an HQ first).
        for (size_t k = 0; k < g.members.size();) {
            if (units[g.members[k]].alive) {
                ++k;
                continue;
            }
            units[g.members[k]].group = -1;
            g.members.erase(g.members.begin() + k);
            g.keep_dx.erase(g.keep_dx.begin() + k);
            g.keep_dy.erase(g.keep_dy.begin() + k);
        }
        if (!units[g.leader].alive) {
            units[g.leader].group = -1;
            if (g.members.empty()) {
                g = Group{};
                continue;
            }
            size_t pick = 0;
            for (size_t k = 0; k < g.members.size(); ++k)
                if (units[g.members[k]].type == U_HQ) pick = k;
            int nl = g.members[pick];
            float ox = g.keep_dx[pick], oy = g.keep_dy[pick];
            g.members.erase(g.members.begin() + pick);
            g.keep_dx.erase(g.keep_dx.begin() + pick);
            g.keep_dy.erase(g.keep_dy.begin() + pick);
            for (size_t k = 0; k < g.members.size(); ++k) {
                g.keep_dx[k] -= ox;
                g.keep_dy[k] -= oy;
            }
            g.leader = nl;
            units[nl].follow = false;
            g.has_dest = false;
            if (g.side == SIDE_PLAYER)
                push_event(EV_INFO, nl, true, fmt("%s takes command of %s.", units[nl].name.c_str(), g.name.c_str()));
        }
        if (g.members.empty()) {
            disband_group(gid);
            continue;
        }
        Unit &L = units[g.leader];
        L.formation_wait = false;
        for (int id : g.members) units[id].formation_wait = false;
        if (L.order == O_RETREAT) continue; // a leader breaking contact doesn't drag the team along

        // The leader's track: the hexes it has driven through (a column follows them exactly,
        // and anyone squeezing through a defile files in along them).
        const int lidx = map.index(L.pos);
        if (g.trail.empty() || g.trail.back() != lidx) {
            g.trail.push_back(lidx);
            if (g.trail.size() > 200) g.trail.erase(g.trail.begin());
        }
        // A new leader destination (an order arrived, or it halted) re-lays the formation now.
        HexCoord ld = L.moving() ? L.dest : L.pos;
        bool reslot = false;
        if (!g.has_dest || !(ld == g.dest)) {
            g.dest = ld;
            g.has_dest = true;
            reslot = true;
        }
        // The formation faces along the leader's route a few hexes ahead, so it wheels with
        // the road instead of pointing straight at the destination.
        if (L.moving()) {
            size_t ahead = std::min(L.path.size() - 1, L.path_i + 4);
            HexCoord a2 = map.coord_of(L.path[ahead]);
            if (!(a2 == L.pos)) {
                Vec2 a = hex_center(L.pos), b = hex_center(a2);
                g.heading = (float)std::atan2(b.y - a.y, b.x - a.x);
            }
        }
        auto trail_hex = [&](int rank, HexCoord &out) {
            const size_t back = (size_t)rank * (size_t)g.spacing;
            if (g.trail.size() <= back) return false;
            out = map.coord_of(g.trail[g.trail.size() - 1 - back]);
            return true;
        };
        auto enemy_held = [&](const Unit &m, HexCoord h) {
            bool held = false;
            for_units_near(h, 1, [&](const Unit &o) {
                if (o.alive && o.side != m.side && o.pos == h) held = true;
            });
            return held;
        };
        // Formation pathfinding rules:
        //  1. Column: each member follows the leader's own track, rank x spacing hexes behind it.
        //  2. Line, wedge, keep: each member takes its place beside or behind the leader's present
        //     hex, laid out along the leader's route heading, and re-takes it whenever the place
        //     has moved 1 km from where it is heading (keeping its progress into the next hex).
        //  3. If that place is water, cut off, or held by the enemy, the member files in on the
        //     leader's track instead (the formation closes up, e.g. onto a bridge).
        //  4. Defiles: if the member's own route to its place would take more than twice as long
        //     as the leader's going (a river without a bridge, a marsh, deep forest beside a road),
        //     it files in on the leader's track and opens out again once past.
        //  5. The team keeps the pace of its slowest member: the leader halts while anyone who can
        //     move is more than (3 + spacing / 2) hexes from its place.
        const int tol = 3 + g.spacing / 2;
        for (size_t k = 0; k < g.members.size(); ++k) {
            Unit &m = units[g.members[k]];
            if (!m.follow || m.order == O_RETREAT || m.pending.active) continue;
            const int rank = (int)k + 1;
            HexCoord slot;
            bool filed = false;
            if (g.shape == FS_COLUMN) {
                if (!trail_hex(rank, slot)) slot = slot_for(g, (int)k, L.pos, g.heading);
                filed = true;
            } else {
                slot = slot_for(g, (int)k, L.pos, g.heading);
                HexCoord t;
                if ((slot == L.pos || (m.mode != MV_ASSAULT && enemy_held(m, slot))) && trail_hex(rank, t)) {
                    slot = t;
                    filed = true;
                }
            }
            if (m.mode != MV_ASSAULT && enemy_held(m, slot)) continue; // stay put rather than walk into the enemy
            HexCoord alt; // its place on the leader's track, which also counts as in place (rule 3/4)
            const bool has_alt = trail_hex(rank, alt);
            bool need;
            if (!m.moving()) need = !(m.pos == slot) && !(has_alt && m.pos == alt && L.moving());
            else need = hex_distance(slot, m.dest) >= 2 && !(has_alt && hex_distance(alt, m.dest) < 2);
            if (reslot && !(m.pos == slot)) need = true;
            if (need) {
                // Re-routing keeps the progress already made into the next hex when the new route
                // starts the same way, so frequent corrections don't stall the member.
                const bool was_moving = m.moving();
                const int old_next = was_moving ? m.path[m.path_i] : -1;
                const float old_progress = m.progress;
                m.mode = L.mode;
                m.resting = false;
                start_path(m, slot, O_MOVE, false);
                auto keep_progress = [&]() {
                    if (was_moving && m.moving() && m.path[m.path_i] == old_next) m.progress = old_progress;
                };
                keep_progress();
                if (!filed && m.moving()) {
                    float cost = 0;
                    int from = map.index(m.pos);
                    for (size_t i = m.path_i; i < m.path.size(); ++i) {
                        cost += std::max(0.0f, move_cost(m, from, m.path[i]));
                        from = m.path[i];
                    }
                    const float leader_rate = std::max(2.5f, L.moving() ? L.step_cost : 2.5f);
                    const float par = leader_rate * (float)hex_distance(m.pos, slot);
                    HexCoord t;
                    if (cost > 2.0f * par + 10.0f && trail_hex(rank, t) && !(t == m.pos)) {
                        start_path(m, t, O_MOVE, false);
                        keep_progress();
                        slot = t;
                    }
                }
            }
            if (has_alt && m.moving() && hex_distance(alt, m.dest) < 2) slot = alt; // filed in: judge it by its track place
            const bool stuck = (!m.moving() && !(m.pos == slot)) ||
                               (unit_type_info(m.type).mobility != M_FOOT && m.fuel <= 0.0f);
            if (L.moving() && !stuck && hex_distance(m.pos, slot) > tol) L.formation_wait = true;
        }
    }
}

// A road column that has no more road ahead deploys: it leaves road-march mode for the
// rest of the move (normal off-road speed, normal shooting, no longer a bunched target).
void World::deploy_if_off_road(Unit &u) const {
    if (u.mode != MV_ROAD || !u.moving()) return;
    int from = map.index(u.pos);
    for (size_t i = u.path_i; i < u.path.size(); ++i) {
        if (road_step(from, u.path[i])) return;
        from = u.path[i];
    }
    u.mode = MV_HASTY;
}

void World::update_movement() {
    for (auto &u : units) {
        if (!u.alive || !u.moving()) continue;
        if (u.formation_wait) continue; // keeping formation: the rest of the team catches up
        const auto &ti = unit_type_info(u.type);
        if (ti.mobility != M_FOOT && u.fuel <= 0) {
            if (!u.out_of_fuel_reported && u.side == SIDE_PLAYER) {
                push_event(EV_SUPPLY, u.id, true, fmt("%s is out of fuel %s.", u.name.c_str(),
                                                     place_name(u.pos).c_str()));
            }
            u.out_of_fuel_reported = true;
            continue;
        }
        float speed = 0.6f + 0.4f * u.readiness / 100.0f;
        u.progress += speed;
        bool halted_by_enemy = false;
        while (u.moving() && u.progress >= u.step_cost) {
            // Only an assault may move into a hex the enemy holds. Anyone else halts in front
            // of it - and has found the enemy, seen or not.
            if (u.mode != MV_ASSAULT) {
                const HexCoord next = map.coord_of(u.path[u.path_i]);
                int blocker = -1;
                for_units_near(next, 1, [&](const Unit &o) {
                    if (blocker < 0 && o.alive && o.side != u.side && o.pos == next) blocker = o.id;
                });
                if (blocker >= 0) {
                    Unit &e = units[blocker];
                    const bool was_known = time_min - e.last_seen[u.side] <= 120;
                    e.seen_until[u.side] = std::max(e.seen_until[u.side], time_min + 5);
                    e.last_seen[u.side] = time_min;
                    e.last_seen_pos[u.side] = e.pos;
                    u.seen_until[e.side] = std::max(u.seen_until[e.side], time_min + 5);
                    u.last_seen[e.side] = time_min;
                    u.last_seen_pos[e.side] = u.pos;
                    if (u.side == SIDE_PLAYER)
                        push_event(was_known ? EV_INFO : EV_CONTACT, u.id, true,
                                   fmt("%s halts %s: hostile %s in the next hex. Only an assault can go in.",
                                       u.name.c_str(), place_name(u.pos).c_str(), unit_type_info(e.type).name));
                    else if (e.side == SIDE_PLAYER && !was_known)
                        push_event(EV_CONTACT, u.id, true,
                                   fmt("Contact: hostile %s halts in front of %s %s.", unit_type_info(u.type).name,
                                       e.name.c_str(), place_name(e.pos).c_str()));
                    u.path.clear();
                    u.path_i = 0;
                    u.progress = 0;
                    halted_by_enemy = true;
                    break;
                }
            }
            u.progress -= u.step_cost;
            u.prev_pos = u.pos;
            u.pos = map.coord_of(u.path[u.path_i++]);
            u.fuel = std::max(0.0f, u.fuel - ti.fuel_per_hex);
            u.readiness = std::max(20.0f, u.readiness - 0.08f);
            if (u.moving()) {
                deploy_if_off_road(u);
                u.step_cost = move_cost(u, map.index(u.pos), u.path[u.path_i]);
                if (u.step_cost < 0) {
                    u.path.clear();
                    u.path_i = 0;
                }
            }
        }
        if (!u.moving()) {
            bool was_retreat = u.order == O_RETREAT;
            u.order = u.resting ? O_REST : O_HOLD;
            u.mode = MV_HASTY; // the march is over: the column deploys where it halts
            u.progress = 0;
            u.path.clear();
            u.path_i = 0;
            if (u.side == SIDE_PLAYER && !was_retreat && !u.follow && !halted_by_enemy) // team members keeping station don't report
                push_event(EV_ARRIVED, u.id, false, fmt("%s has arrived %s.", u.name.c_str(), place_name(u.pos).c_str()));
        }
    }
}

void World::update_zones() {
    stats_.hot = stats_.warm = stats_.cold = stats_.alive = 0;
    for (auto &u : units) {
        if (!u.alive) continue;
        int best = 1 << 30;
        for_units_near(u.pos, WARM_RANGE, [&](const Unit &o) {
            if (o.side == u.side || !o.alive) return;
            int d = hex_distance(u.pos, o.pos);
            if (d < best) best = d;
        });
        if (best <= HOT_RANGE) u.hot_until = time_min + HOT_HOLD_MIN;
        if (time_min < u.hot_until) u.zone = Z_HOT;
        else if (best <= WARM_RANGE) u.zone = Z_WARM;
        else u.zone = Z_COLD;
        stats_.alive++;
        if (u.zone == Z_HOT) stats_.hot++;
        else if (u.zone == Z_WARM) stats_.warm++;
        else stats_.cold++;
    }
}

void World::update_spotting(bool full_pass) {
    for (auto &obs : units) {
        if (!obs.alive || obs.zone == Z_COLD) continue;
        if (obs.zone == Z_WARM && !full_pass) continue;
        const int hold = obs.zone == Z_HOT ? 1 : 5;
        const float range = effective_spot_range(obs);
        const int irange = (int)range;
        for_units_near(obs.pos, irange, [&](const Unit &tc) {
            if (tc.side == obs.side || !tc.alive) return;
            Unit &t = units[tc.id];
            int d = hex_distance(obs.pos, t.pos);
            if (d > irange) return;
            if (time_min < t.seen_until[obs.side] && t.seen_until[obs.side] >= time_min + hold) return; // known
            // Units sitting still in cover are only found up close.
            const Hex &th = map.at(t.pos);
            bool concealed = (th.terrain == T_FOREST || th.terrain == T_URBAN) &&
                             (!t.moving() || t.mode == MV_DELIBERATE) && time_min - t.last_fired > 3;
            if (concealed && d > (obs.type == U_RECON ? 4 : 2)) {
                // Beyond close range a hidden unit is found only by chance (movement, a glint,
                // a careless radio) - unless this side already has eyes on it.
                bool tracked = time_min - t.last_seen[obs.side] <= 10;
                if (!tracked) {
                    double p = 0.03 * (1.0 - (double)d / (range + 1.0)) * (obs.type == U_RECON ? 2.0 : 1.0) * hold;
                    if (!rng_.chance(p)) return;
                }
            }
            if (!line_of_sight(obs.pos, t.pos, 3.0f, 2.5f)) return;
            bool was_known = time_min - t.last_seen[obs.side] <= 120;
            t.seen_until[obs.side] = time_min + hold;
            t.last_seen[obs.side] = time_min;
            t.last_seen_pos[obs.side] = t.pos;
            if (!was_known && obs.side == SIDE_PLAYER) {
                push_event(EV_CONTACT, t.id, true,
                           fmt("Contact: hostile %s %s, spotted by %s.", unit_type_info(t.type).name,
                               place_name(t.pos).c_str(), obs.name.c_str()));
            }
        });
    }
}

void World::update_combat() {
    for (auto &u : units) {
        if (!u.alive || u.zone != Z_HOT) continue;
        if (u.fire_cooldown > 0) {
            --u.fire_cooldown;
            continue;
        }
        if (u.ammo <= 0) continue;
        if (u.roe == ROE_HOLD) continue;
        if (u.roe == ROE_RETURN && time_min - u.last_targeted > 5) continue;
        const auto &ti = unit_type_info(u.type);
        int best = -1, best_d = 1 << 30;
        for_units_near(u.pos, ti.weapon_range, [&](const Unit &t) {
            if (t.side == u.side || !t.alive || !seen_by(t, u.side)) return;
            int d = hex_distance(u.pos, t.pos);
            if (d > ti.weapon_range || d >= best_d) return;
            // Same sight line as spotting: whatever a unit can see in range, it can shoot at.
            if (!ti.indirect && !line_of_sight(u.pos, t.pos, 3.0f, 2.5f)) return;
            best = t.id;
            best_d = d;
        });
        if (best < 0) {
            u.acquired = -1;
            continue;
        }
        // A new target has to be acquired first: found, identified and laid on (or, for guns,
        // a fire mission plotted). Crew condition sets how long that takes.
        const EngagementTime et = engagement_time(u);
        auto minutes = [&](float x) {
            int m = (int)x;
            if (rng_.chance(x - m)) ++m;
            return std::max(0, m);
        };
        if (best != u.acquired) {
            u.acquired = best;
            u.fire_cooldown = minutes(et.acquire);
            if (u.fire_cooldown > 0) continue;
        }
        Unit &t = units[best];
        const auto &tt = unit_type_info(t.type);
        const Hex &th = map.at(t.pos);
        bool soft = !tt.armored;
        float p;
        if (ti.indirect) {
            p = 0.35f * (soft ? cover_mod(th, true) * 1.4f : 0.8f);
        } else {
            p = 0.42f * (1.0f - 0.5f * best_d / (ti.weapon_range + 1.0f)) * cover_mod(th, soft) *
                (t.moving() ? 0.8f : 1.0f);
        }
        if (u.moving() && !ti.indirect) {
            switch (u.mode) {
                case MV_DELIBERATE: break;
                case MV_ASSAULT: p *= 0.85f; break;
                case MV_ROAD: p *= 0.5f; break;
                default: p *= 0.75f; break;
            }
        }
        if (t.moving() && t.mode == MV_ROAD) p *= 1.3f; // a column on the road is bunched up
        t.last_targeted = time_min;
        u.fire_cooldown = std::max(1, minutes(et.cycle));
        u.ammo = std::max(0.0f, u.ammo - (ti.indirect ? 2.5f : 1.2f));
        u.last_fired = time_min;
        u.last_target = t.id;
        u.readiness = std::max(20.0f, u.readiness - 0.1f);
        // Direct fire gives the shooter away.
        if (!ti.indirect && best_d <= std::max(8, tt.spot_range)) {
            u.seen_until[t.side] = std::max(u.seen_until[t.side], time_min + 2);
            if (time_min - u.last_seen[t.side] > 30 && t.side == SIDE_PLAYER)
                push_event(EV_UNDER_FIRE, t.id, true,
                           fmt("%s under fire from a hostile %s.", t.name.c_str(), ti.name));
            u.last_seen[t.side] = time_min;
            u.last_seen_pos[t.side] = u.pos;
        }
        t.morale = std::max(0.0f, t.morale - 0.7f); // suppression
        if (rng_.chance(p)) {
            // Fire weight in strength points (as before), scaled by how many of the shooter's
            // elements are still in the fight, then resolved as whole elements knocked out:
            // each point is a 1-in-(100 / elements_max) chance of one element, so the average
            // loss per hit is unchanged but every element is either in the fight or not.
            float dmg = ti.firepower * (float)rng_.range(0.6, 1.4) * u.elements / (float)u.elements_max;
            if (tt.armored) dmg *= ti.anti_armor;
            const float per_element = 100.0f / t.elements_max;
            float expect = dmg / per_element;
            int kills = (int)expect;
            if (rng_.chance(expect - kills)) ++kills;
            kills = std::min(kills, t.elements);
            t.elements -= kills;
            t.strength = 100.0f * t.elements / t.elements_max;
            t.morale = std::max(0.0f, t.morale - kills * per_element * 0.6f);
            t.last_hit = time_min;
            if (t.elements <= 0) {
                t.strength = 0;
                t.alive = false;
                t.path.clear();
                t.path_i = 0;
                t.pending.active = false;
                if (t.side == SIDE_PLAYER) stats_.losses++;
                else stats_.kills++;
                push_event(EV_DESTROYED, t.id, t.side == SIDE_PLAYER,
                           t.side == SIDE_PLAYER
                               ? fmt("%s has been destroyed %s.", t.name.c_str(), place_name(t.pos).c_str())
                               : fmt("Hostile %s destroyed by %s.", tt.name, u.name.c_str()));
            }
        }
    }
}

void World::update_morale_and_sop() {
    for (auto &u : units) {
        if (!u.alive) continue;
        bool engaged = time_min - u.last_hit < 15 || time_min - u.last_fired < 15;
        float rest = (u.resting && !u.moving()) ? 2.0f : 1.0f;
        if (!engaged) u.morale = std::min(90.0f, u.morale + 0.03f * rest);
        if (!u.moving() && !engaged) u.readiness = std::min(100.0f, u.readiness + 0.04f * rest);
        // Standing orders: break contact when hurt or shaken. No command delay.
        float s_thr = u.brk == BC_LIGHT ? 60.0f : u.brk == BC_NEVER ? 0.0f : 30.0f;
        float m_thr = u.brk == BC_LIGHT ? 45.0f : u.brk == BC_NEVER ? 8.0f : 25.0f;
        if (u.mode == MV_ASSAULT && u.moving()) {
            s_thr *= 0.6f;
            m_thr *= 0.6f;
        }
        if (u.type == U_HQ) { // a headquarters displaces early rather than fight it out
            s_thr = std::max(s_thr, 60.0f);
            m_thr = std::max(m_thr, 45.0f);
        }
        if (u.order != O_RETREAT && (u.strength < s_thr || u.morale < m_thr) && engaged) {
            HexCoord away = u.pos;
            int nearest = -1, nd = 1 << 30;
            for_units_near(u.pos, 12, [&](const Unit &o) {
                if (o.side == u.side || !o.alive) return;
                int d = hex_distance(u.pos, o.pos);
                if (d < nd) {
                    nd = d;
                    nearest = o.id;
                }
            });
            if (nearest >= 0) {
                Vec2 a = hex_center(u.pos), b = hex_center(units[nearest].pos);
                double dx = a.x - b.x, dy = a.y - b.y, l = std::max(1.0, std::hypot(dx, dy));
                away = world_to_hex(a.x + dx / l * 6000.0, a.y + dy / l * 6000.0);
            }
            if (!map.in_bounds(away) || map.at(away).terrain == T_WATER) away = u.pos;
            u.pending.active = false;
            u.mode = MV_HASTY;
            u.resting = false;
            start_path(u, away, O_RETREAT, false);
            if (u.moving()) {
                u.order = O_RETREAT;
                if (u.side == SIDE_PLAYER)
                    push_event(EV_RETREAT, u.id, true,
                               fmt("%s is falling back (strength %.0f%%, morale %.0f).", u.name.c_str(), u.strength,
                                   u.morale));
            }
        }
    }
}

void World::update_ai() {
    static const int slot_offsets[12][2] = {{0, 0}, {2, 0}, {-2, 0}, {1, 2}, {-1, 2}, {1, -2},
                                            {-1, -2}, {4, 0}, {-4, 0}, {0, 4}, {0, -4}, {3, 3}};
    // Formation objectives: patrol from town to town, the strike group hunts the company.
    update_hunters();
    for (auto &f : formations) {
        if (f.side == SIDE_PLAYER || f.objective < 0 || f.hunts_player) continue;
        HexCoord obj = map.settlements[f.objective].pos;
        int arrived = 0, alive = 0;
        for (int id : f.members) {
            if (!units[id].alive) continue;
            ++alive;
            if (hex_distance(units[id].pos, obj) <= 5) ++arrived;
        }
        if (alive == 0) continue;
        if (arrived * 2 >= alive) {
            if (f.dwell_until == 0) f.dwell_until = time_min + 180 + rng_.range_int(0, 240);
            else if (time_min >= f.dwell_until) {
                // Next town within ~60 km.
                std::vector<int> options;
                for (int i = 0; i < (int)map.settlements.size(); ++i) {
                    int d = hex_distance(map.settlements[i].pos, obj);
                    if (d >= 20 && d <= 120 && reachable(map.settlements[i].pos, obj)) options.push_back(i);
                }
                if (!options.empty()) f.objective = options[rng_.range_int(0, (int)options.size() - 1)];
                f.dwell_until = 0;
            }
        }
    }

    for (auto &u : units) {
        if (!u.alive || u.side == SIDE_PLAYER || time_min < u.ai_next) continue;
        u.ai_next = time_min + 15 + rng_.range_int(0, 5);
        if (u.pending.active || (u.order == O_RETREAT && u.moving())) continue;
        const auto &ti = unit_type_info(u.type);
        const Formation &f = formations[u.formation];
        // Shaken, shot-up or empty units don't press forward: they pull back from anything
        // close and sit to recover (and rearm from their own trains, see update_logistics).
        if (u.morale < 35 || u.strength < 25 || u.ammo < 10) {
            bool threatened = false;
            for_units_near(u.pos, 8, [&](const Unit &o) {
                if (o.side != u.side && o.alive && seen_by(o, u.side) && hex_distance(u.pos, o.pos) <= 8)
                    threatened = true;
            });
            if (threatened && u.order != O_RETREAT) order_fall_back(u.id);
            else if (!threatened && u.moving() && u.order != O_RETREAT) order_hold(u.id);
            continue;
        }

        // React to known enemies within 12 km.
        int tgt = -1, td = 1 << 30;
        for_units_near(u.pos, 24, [&](const Unit &o) {
            if (o.side == u.side || !o.alive || !seen_by(o, u.side)) return;
            int d = hex_distance(u.pos, o.pos);
            if (d < td && d <= 24) {
                td = d;
                tgt = o.id;
            }
        });
        if (tgt >= 0) {
            if (u.type == U_ARTILLERY || u.type == U_HQ || u.type == U_RECON) {
                if (u.moving() && td < 10) order_hold(u.id);
                continue;
            }
            if (td > ti.weapon_range - 1) {
                if (!(u.moving() && hex_distance(u.dest, units[tgt].pos) <= 3)) order_move(u.id, units[tgt].pos);
            } else if (u.moving()) {
                order_hold(u.id);
            }
            continue;
        }

        if (f.objective < 0 && !f.has_target) continue;
        HexCoord obj = f.hunts_player && f.has_target ? f.target : map.settlements[f.objective].pos;
        const int *off = slot_offsets[u.slot % 12];
        HexCoord goal{obj.col + off[0], obj.row + off[1]};
        if (!map.in_bounds(goal) || map.at(goal).terrain == T_WATER) goal = obj;
        if (f.hunts_player && ((u.type == U_ARTILLERY && hex_distance(u.pos, obj) <= 16) ||
                               (u.type == U_HQ && hex_distance(u.pos, obj) <= 10))) {
            // Guns stand off about 8 km and fire from there; the HQ stays out of the fight.
            if (u.moving()) order_hold(u.id);
            continue;
        }
        if (!reachable(u.pos, goal)) continue;
        if (hex_distance(u.pos, goal) > 2 && !(u.moving() && u.dest == goal)) order_move(u.id, goal);
    }
}

void World::update_logistics() {
    // Hostile forces live off their own trains: out of the fight they slowly rearm and refuel.
    for (auto &u : units) {
        if (!u.alive || u.side == SIDE_PLAYER || u.moving()) continue;
        if (time_min - u.last_fired < 60 || time_min - u.last_hit < 60) continue;
        u.ammo = std::min(100.0f, u.ammo + 8.0f);
        u.fuel = std::min(100.0f, u.fuel + 8.0f);
    }
    int hq = -1;
    for (int id : formations[0].members)
        if (units[id].alive && units[id].type == U_HQ) hq = id;
    stock.personnel = 0;
    for (auto &u : units) {
        if (!u.alive || u.side != SIDE_PLAYER) continue;
        const auto &ti = unit_type_info(u.type);
        stock.personnel += (int)std::round(ti.crew * u.strength / 100.0f);
        // Resupply from the company train: halted near the HQ and out of the fight for 30 minutes
        // (enemies merely being nearby doesn't stop the trucks).
        bool fighting = time_min - u.last_fired < 30 || time_min - u.last_hit < 30 || time_min - u.last_targeted < 30;
        if (hq < 0 || fighting || u.moving() || hex_distance(u.pos, units[hq].pos) > 3) continue;
        float rate = u.resting ? 50.0f : 25.0f;
        float want_fuel = std::min(rate, 100.0f - u.fuel);
        double litres = want_fuel / 100.0 * ti.fuel_litres;
        if (litres > 0 && stock.diesel_l > 0) {
            double got = std::min(litres, stock.diesel_l);
            stock.diesel_l -= got;
            u.fuel += (float)(got / ti.fuel_litres * 100.0);
            u.out_of_fuel_reported = false;
        }
        float want_ammo = std::min(rate, 100.0f - u.ammo);
        double tonnes = want_ammo / 100.0 * ti.ammo_tonnes;
        if (tonnes > 0 && stock.ammo_t > 0) {
            double got = std::min(tonnes, stock.ammo_t);
            stock.ammo_t -= got;
            u.ammo += (float)(got / ti.ammo_tonnes * 100.0);
        }
    }
    if ((time_min % 1440) == 6 * 60) {
        double need = stock.personnel * 1.5;
        if (stock.food_kg >= need) {
            stock.food_kg -= need;
        } else {
            stock.food_kg = 0;
            for (auto &u : units)
                if (u.alive && u.side == SIDE_PLAYER) u.morale = std::max(0.0f, u.morale - 10.0f);
            push_event(EV_SUPPLY, -1, true, "The company is out of food. Morale is falling.");
        }
    }
    if (stock.diesel_l < 3000 && stock.diesel_l > 0 && time_min % 720 == 0)
        push_event(EV_SUPPLY, -1, true, fmt("Diesel is running low: %.0f litres left.", stock.diesel_l));
}

// The strike group hunts the company: it heads for the freshest sighting any hostile has
// made, and every six hours or so gets word of where the company HQ is (locals talk).
void World::update_hunters() {
    for (auto &f : formations) {
        if (!f.hunts_player) continue;
        int64_t best_t = -100000;
        HexCoord best_pos;
        for (const auto &p : units) {
            if (!p.alive || p.side != SIDE_PLAYER) continue;
            if (p.last_seen[SIDE_HOSTILE] > best_t) {
                best_t = p.last_seen[SIDE_HOSTILE];
                best_pos = p.last_seen_pos[SIDE_HOSTILE];
            }
        }
        if (best_t > f.target_time && time_min - best_t <= 180) {
            f.target = best_pos;
            f.target_time = best_t;
            f.has_target = true;
        }
        if (time_min >= f.intel_next) {
            f.intel_next = time_min + 360 + rng_.range_int(0, 120);
            // Word comes in about some part of the company, not necessarily its HQ.
            std::vector<int> own;
            for (const auto &p : units)
                if (p.alive && p.side == SIDE_PLAYER) own.push_back(p.id);
            if (!own.empty() && time_min - f.target_time > 120) {
                HexCoord h = units[own[rng_.range_int(0, (int)own.size() - 1)]].pos;
                HexCoord guess{h.col + rng_.range_int(-4, 4), h.row + rng_.range_int(-4, 4)};
                if (!map.in_bounds(guess) || map.at(guess).terrain == T_WATER) guess = h;
                f.target = guess;
                f.target_time = time_min;
                f.has_target = true;
            }
        }
    }
}

int World::strike_formation() const {
    for (int i = 0; i < (int)formations.size(); ++i)
        if (formations[i].hunts_player) return i;
    return -1;
}

int World::effective_combat_units(int formation) const {
    if (formation < 0 || formation >= (int)formations.size()) return 0;
    int n = 0;
    for (int id : formations[formation].members) {
        const Unit &u = units[id];
        if (u.alive && u.type != U_HQ && u.type != U_ARTILLERY && u.strength >= 30.0f) ++n;
    }
    return n;
}

void World::check_outcome() {
    if (outcome_ != OUT_ONGOING || formations.empty()) return;
    // Losing the HQ is not the end by itself (orders get slow and the trucks stop coming),
    // but a company with fewer than three fighting platoons is finished.
    if (effective_combat_units(0) < 3) {
        outcome_ = OUT_DEFEAT;
        outcome_text_ = hq_of(SIDE_PLAYER) < 0
                            ? "Company Murks is no longer combat effective, and its HQ is gone."
                            : "Company Murks is no longer combat effective.";
        push_event(EV_INFO, -1, true, outcome_text_);
        return;
    }
    int sf = strike_formation();
    if (sf >= 0 && formations[sf].start_combat > 0 && effective_combat_units(sf) == 0) {
        outcome_ = OUT_VICTORY;
        outcome_text_ = formations[sf].name + " is broken. The contract is fulfilled.";
        push_event(EV_INFO, -1, true, outcome_text_);
    }
}

bool World::player_in_contact() const {
    for (const auto &u : units) {
        if (!u.alive || u.side != SIDE_PLAYER) continue;
        if (time_min - u.last_fired < 15 || time_min - u.last_hit < 15 || time_min - u.last_targeted < 15) return true;
    }
    for (const auto &t : units) {
        if (!t.alive || t.side == SIDE_PLAYER || !seen_by(t, SIDE_PLAYER)) continue;
        bool near = false;
        for_units_near(t.pos, CONTACT_RANGE, [&](const Unit &o) {
            if (o.alive && o.side == SIDE_PLAYER && hex_distance(o.pos, t.pos) <= CONTACT_RANGE) near = true;
        });
        if (near) return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------
// Quick save / load
// ------------------------------------------------------------------------------------

namespace {
constexpr char kSaveMagic[8] = {'M', 'U', 'R', 'K', 'S', 'S', 'V', '3'};

struct SaveWriter {
    std::vector<uint8_t> b;
    template <typename T> void pod(const T &v) {
        static_assert(std::is_trivially_copyable<T>::value, "pod");
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    void str(const std::string &v) {
        pod((uint32_t)v.size());
        b.insert(b.end(), v.begin(), v.end());
    }
    void ints(const std::vector<int> &v) {
        pod((uint32_t)v.size());
        for (int x : v) pod(x);
    }
};

struct SaveReader {
    const uint8_t *p, *e;
    bool ok = true;
    template <typename T> void pod(T &v) {
        if (!ok || (size_t)(e - p) < sizeof(T)) {
            ok = false;
            return;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
    }
    uint32_t count(size_t limit) {
        uint32_t n = 0;
        pod(n);
        if (n > limit) ok = false;
        return ok ? n : 0;
    }
    void str(std::string &v) {
        uint32_t n = count(1 << 16);
        if (!ok || (size_t)(e - p) < n) {
            ok = false;
            return;
        }
        v.assign(reinterpret_cast<const char *>(p), n);
        p += n;
    }
    void ints(std::vector<int> &v) {
        uint32_t n = count(1u << 26);
        v.resize(n);
        for (auto &x : v) pod(x);
    }
};

template <typename A, typename U> void io_unit(A &a, U &u) {
    a.pod(u.id); a.pod(u.side); a.pod(u.type); a.str(u.name); a.str(u.equipment);
    a.pod(u.formation); a.pod(u.slot); a.pod(u.pos); a.pod(u.prev_pos); a.pod(u.dest);
    a.ints(u.path);
    uint64_t pi = u.path_i;
    a.pod(pi);
    u.path_i = (size_t)pi;
    a.pod(u.progress); a.pod(u.step_cost); a.pod(u.order); a.pod(u.pending); a.pod(u.mode);
    a.pod(u.roe); a.pod(u.brk); a.pod(u.pending_stance); a.pod(u.resting); a.pod(u.last_targeted);
    a.pod(u.elements_max); a.pod(u.elements); a.pod(u.acquired); a.pod(u.group); a.pod(u.follow);
    a.pod(u.standoff); a.pod(u.standoff_next);
    a.pod(u.strength); a.pod(u.morale); a.pod(u.fuel); a.pod(u.ammo); a.pod(u.readiness);
    a.pod(u.zone); a.pod(u.hot_until); a.pod(u.fire_cooldown); a.pod(u.last_fired); a.pod(u.last_hit);
    a.pod(u.last_target); a.pod(u.alive); a.pod(u.out_of_fuel_reported);
    a.pod(u.seen_until); a.pod(u.last_seen); a.pod(u.last_seen_pos); a.pod(u.ai_next);
}

template <typename A> void io_floats(A &a, std::vector<float> &v) {
    uint32_t n = (uint32_t)v.size();
    a.pod(n);
    if (n > (1u << 16)) n = 0;
    v.resize(n);
    for (auto &x : v) a.pod(x);
}

template <typename A, typename G> void io_group(A &a, G &g) {
    a.pod(g.alive); a.str(g.name); a.pod(g.side); a.pod(g.formation); a.pod(g.leader);
    a.ints(g.members);
    io_floats(a, g.keep_dx); io_floats(a, g.keep_dy);
    a.pod(g.shape); a.pod(g.spacing); a.pod(g.color); a.pod(g.has_dest); a.pod(g.dest); a.pod(g.heading);
    a.ints(g.trail);
}

template <typename A, typename F> void io_formation(A &a, F &f) {
    a.str(f.name); a.pod(f.side); a.pod(f.objective); a.pod(f.hunts_player); a.pod(f.dwell_until);
    a.ints(f.members);
    a.pod(f.has_target); a.pod(f.target); a.pod(f.target_time); a.pod(f.intel_next); a.pod(f.start_combat);
}
} // namespace

std::vector<uint8_t> World::save_state() const {
    SaveWriter w;
    w.b.insert(w.b.end(), kSaveMagic, kSaveMagic + 8);
    w.str(map.name);
    w.pod((int32_t)map.cols());
    w.pod((int32_t)map.rows());
    w.pod(time_min); w.pod(seed); w.pod(player_home);
    w.pod(rng_.state());
    w.pod(stock);
    w.pod(stats_.kills); w.pod(stats_.losses);
    w.pod(outcome_); w.str(outcome_text_);
    w.pod(default_standoff);
    w.pod((uint32_t)formations.size());
    for (const auto &f : formations) io_formation(w, const_cast<Formation &>(f));
    w.pod((uint32_t)units.size());
    for (const auto &u : units) io_unit(w, const_cast<Unit &>(u));
    w.pod((uint32_t)groups.size());
    for (const auto &g : groups) io_group(w, const_cast<Group &>(g));
    return w.b;
}

bool World::load_state(const uint8_t *data, size_t size, std::string *error) {
    auto fail = [&](const char *why) {
        if (error) *error = why;
        return false;
    };
    if (size >= 8 && std::memcmp(data, "MURKSSV", 7) == 0 && std::memcmp(data, kSaveMagic, 8) != 0)
        return fail("save is from an older version of the game");
    if (size < 8 || std::memcmp(data, kSaveMagic, 8) != 0) return fail("not a Murks 1989 save");
    SaveReader r{data + 8, data + size};
    std::string map_name;
    int32_t cols = 0, rows = 0;
    r.str(map_name);
    r.pod(cols);
    r.pod(rows);
    if (!r.ok) return fail("save is truncated");
    if (map_name != map.name || cols != map.cols() || rows != map.rows())
        return fail("save was made on a different map");
    int64_t t = 0;
    uint32_t sd = 0;
    int home = -1;
    uint64_t rs = 0;
    CompanyStock st;
    int kills = 0, losses = 0;
    Outcome oc = OUT_ONGOING;
    std::string ot;
    r.pod(t); r.pod(sd); r.pod(home); r.pod(rs); r.pod(st); r.pod(kills); r.pod(losses); r.pod(oc); r.str(ot);
    uint8_t dso = 0;
    r.pod(dso);
    std::vector<Formation> fs(r.count(4096));
    for (auto &f : fs) io_formation(r, f);
    std::vector<Unit> us(r.count(1 << 20));
    for (auto &u : us) io_unit(r, u);
    std::vector<Group> gs(r.count(4096));
    for (auto &g : gs) io_group(r, g);
    if (!r.ok) return fail("save is truncated or corrupt");
    for (const auto &g : gs) {
        if (!g.alive) continue;
        if (g.leader < 0 || g.leader >= (int)us.size() || g.keep_dx.size() != g.members.size() ||
            g.keep_dy.size() != g.members.size())
            return fail("save is corrupt");
        for (int id : g.members)
            if (id < 0 || id >= (int)us.size()) return fail("save is corrupt");
    }
    for (const auto &u : us)
        if (u.group >= (int)gs.size()) return fail("save is corrupt");
    const int n_cells = map.cols() * map.rows();
    for (int i = 0; i < (int)us.size(); ++i) {
        const Unit &u = us[i];
        if (u.id != i || u.formation < 0 || u.formation >= (int)fs.size() || !map.in_bounds(u.pos) ||
            u.type >= U_TYPE_COUNT || u.path_i > u.path.size() || u.elements_max <= 0 || u.elements < 0 ||
            u.elements > u.elements_max)
            return fail("save is corrupt");
        for (int idx : u.path)
            if (idx < 0 || idx >= n_cells) return fail("save is corrupt");
    }
    for (const auto &f : fs)
        for (int id : f.members)
            if (id < 0 || id >= (int)us.size()) return fail("save is corrupt");
    for (const auto &g : gs)
        for (int idx : g.trail)
            if (idx < 0 || idx >= n_cells) return fail("save is corrupt");
    time_min = t;
    seed = sd;
    player_home = home;
    rng_.set_state(rs);
    stock = st;
    stats_.kills = kills;
    stats_.losses = losses;
    outcome_ = oc;
    outcome_text_ = ot;
    default_standoff = dso;
    formations = std::move(fs);
    units = std::move(us);
    groups = std::move(gs);
    events_.clear();
    rebuild_buckets();
    return true;
}

bool World::player_in_hot_zone() const {
    for (const auto &u : units)
        if (u.alive && u.side == SIDE_PLAYER && u.zone == Z_HOT) return true;
    return false;
}

void World::visible_hexes(int unit_id, std::vector<HexCoord> &out) const {
    out.clear();
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return;
    const Unit &u = units[unit_id];
    int r = (int)effective_spot_range(u);
    for (int dr = -r; dr <= r; ++dr) {
        for (int dc = -r - 1; dc <= r + 1; ++dc) {
            HexCoord h{u.pos.col + dc, u.pos.row + dr};
            if (!map.in_bounds(h) || hex_distance(h, u.pos) > r) continue;
            if (line_of_sight(u.pos, h, 3.0f, 2.5f)) out.push_back(h);
        }
    }
}

} // namespace murks
