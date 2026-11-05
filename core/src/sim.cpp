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
    // Six Ural-4320 trucks: 0.4 L/km each (1.2 L a hex for the column of 1,800 L), drivers' rifles only.
    {"Supply Column", M_WHEELED, 6, 1, 0.3f, 0.05f, false, false, 3, 0.067f, 1800, 0.1f, 12},
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
    holdings.clear();
    route = SupplyRoute{};
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
    holdings.clear();
    route = SupplyRoute{};
    events_.clear();
    stock = CompanyStock{};
    stock.cash = START_CASH + CONTRACT_ADVANCE;
    stock.next_payday = 7 * 1440 + 18 * 60; // Friday 21 April 1989, 18:00, then weekly
    outcome_ = OUT_ONGOING;
    outcome_text_.clear();
    default_standoff = 0;
    trains_announced_ = -1;
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

    // ---- The home depot and the supply column ------------------------------------------
    // The company's stocks are split: a few days' worth with the trains, the rest in the depot
    // at the billet, whose market also sells it food. Six trucks run between the two.
    {
        Holding h;
        h.name = S[player_home].name;
        h.settlement = player_home;
        h.pos = home;
        h.side = SIDE_PLAYER;
        h.diesel_l = 18000;
        h.ammo_t = 36;
        h.food_kg = 4000;
        h.food_per_day = std::max(150.0, std::min(1500.0, S[player_home].population * 0.02));
        holdings.push_back(h);
        formations.push_back({"Murks Supply Column", SIDE_PLAYER, player_home, false, 0, {}});
        std::vector<int> tk;
        // The column takes no random draw, so the rest of the scenario rolls out as it did before it.
        const uint64_t rs = rng_.state();
        int cid = spawn((int)formations.size() - 1, SIDE_PLAYER, U_SUPPLY, "Supply Column", "6x Ural-4320", home, tk);
        rng_.set_state(rs);
        units[cid].roe = ROE_HOLD;   // drivers don't give their trucks away by shooting
        units[cid].standoff = 0;     // the route decides where it goes, not the company stand-off
        route.convoy = cid;
        route.dest = home;
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
    static const int kDefault[U_TYPE_COUNT] = {2, 3, 3, 3, 2, 4, 6}; // HQ, tank, mech, infantry, recon, artillery, supply
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

int World::dead_hq(int formation) const {
    if (formation < 0 || formation >= (int)formations.size()) return -1;
    int dead = -1;
    for (int id : formations[formation].members) {
        if (units[id].type != U_HQ) continue;
        if (units[id].alive) return -1;
        dead = id;
    }
    return dead;
}

int World::acting_hq(int formation, int64_t at) const {
    if (formation < 0 || formation >= (int)formations.size()) return -1;
    if (at < 0) at = time_min;
    const auto &m = formations[formation].members;
    for (int id : m)
        if (units[id].alive && units[id].type == U_HQ) return id;
    const int hq = dead_hq(formation);
    if (hq < 0) return -1; // the formation never had an HQ
    // The HQ died with its last element, so its last hit is when it was lost.
    if (at - units[hq].last_hit < HANDOVER_MIN) return -1;
    // The senior platoon still able to fight: first in the order of battle.
    for (int id : m) {
        const Unit &u = units[id];
        if (u.alive && (u.type == U_TANK || u.type == U_MECH || u.type == U_INFANTRY) && u.strength >= 30.0f)
            return id;
    }
    for (int id : m)
        if (units[id].alive) return id;
    return -1;
}

int World::handover_minutes_left(int formation) const {
    const int hq = dead_hq(formation);
    if (hq < 0) return 0;
    int64_t left = units[hq].last_hit + HANDOVER_MIN - time_min;
    if (left > 0) return (int)left;
    return acting_hq(formation) >= 0 ? 0 : -1;
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
    // The column runs its route; an HQ digging in stays put until it is done or called off.
    if (!u.alive || !map.in_bounds(target) || u.type == U_SUPPLY || u.order == O_DIGIN) return -1;
    if (map.at(target).terrain == T_WATER) return -1;
    int delay = command_delay(u);
    u.pending = {true, O_MOVE, target, time_min, time_min + delay, mode};
    return delay;
}

int World::order_fall_back(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    Unit &u = units[unit_id];
    if (u.type == U_SUPPLY || u.order == O_DIGIN) return -1;
    int delay = command_delay(u);
    u.pending = {true, O_RETREAT, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

int World::order_regroup(int unit_id, bool rest) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    Unit &u = units[unit_id];
    if (u.type == U_SUPPLY || u.order == O_DIGIN) return -1;
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
        if (!u.alive || u.order == O_RETREAT || u.order == O_DIGIN || u.mode == MV_ASSAULT) continue;
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
    // On the HQ, or on whoever has taken over from it.
    int hq = acting_hq(u.formation);
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
    if (!u.alive || u.type == U_SUPPLY || u.order == O_DIGIN) return -1;
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
        if (u.pending.kind == O_DIGIN) {
            // The HQ halts where the order finds it and digs in, if that is still in a town.
            start_path(u, u.pos, O_HOLD, false);
            u.mode = MV_HASTY;
            u.resting = false;
            const std::string why = dig_in_problem(u.id);
            if (!why.empty()) {
                if (u.side == SIDE_PLAYER)
                    push_event(EV_INFO, u.id, true, fmt("%s halts %s and cannot dig in: %s", u.name.c_str(),
                                                        place_name(u.pos).c_str(), why.c_str()));
                continue;
            }
            u.order = O_DIGIN;
            u.dig_town = town_at(u.pos);
            const int mins = dig_in_minutes(u);
            u.dig_until = time_min + mins;
            if (u.side == SIDE_PLAYER)
                push_event(EV_ORDER, u.id, false,
                           fmt("%s is digging in at %s: the FOB is ready in %d min if the HQ is left in peace.",
                               u.name.c_str(), map.settlements[u.dig_town].name.c_str(), mins));
            continue;
        }
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
    update_convoy();
    if (time_min % 5 == 0) {
        rebuild_buckets();
        update_zones();
    }
    update_spotting(time_min % 5 == 0);
    update_combat();
    update_dig_in(); // after combat: a hit this minute abandons the dig-in
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
        if (id < 0 || id >= (int)units.size() || !units[id].alive || units[id].type == U_SUPPLY) return -1;
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
            if (!m.follow || m.order == O_RETREAT || m.order == O_DIGIN || m.pending.active) continue;
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
            if (t.type == U_SUPPLY && kills > 0) convoy_hit(t, kills);
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
                           t.side != SIDE_PLAYER
                               ? fmt("Hostile %s destroyed by %s.", tt.name, u.name.c_str())
                           : t.type == U_HQ
                               ? fmt("%s has been destroyed %s. Orders will be slow, and the trains need about "
                                     "%d minutes to report to a new commander.",
                                     t.name.c_str(), place_name(t.pos).c_str(), HANDOVER_MIN)
                               : fmt("%s has been destroyed %s.", t.name.c_str(), place_name(t.pos).c_str()));
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
        if (!u.moving() && !engaged) {
            // Halted at a FOB: billets, hot food and a perimeter, so crews recover faster.
            float fob = 1.0f;
            if (u.side == SIDE_PLAYER) {
                const int d = depot_near(u.pos);
                if (d >= 0 && holdings[d].kind == H_FOB) fob = FOB_READINESS_RATE;
            }
            u.readiness = std::min(100.0f, u.readiness + 0.04f * rest * fob);
        }
        // Standing orders: break contact when hurt or shaken. No command delay.
        float s_thr = u.brk == BC_LIGHT ? 60.0f : u.brk == BC_NEVER ? 0.0f : 30.0f;
        float m_thr = u.brk == BC_LIGHT ? 45.0f : u.brk == BC_NEVER ? 8.0f : 25.0f;
        if (u.mode == MV_ASSAULT && u.moving()) {
            s_thr *= 0.6f;
            m_thr *= 0.6f;
        }
        if (u.type == U_HQ || u.type == U_SUPPLY) { // headquarters and trucks displace early rather than fight it out
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
            const bool was_digging = u.order == O_DIGIN;
            u.pending.active = false;
            u.mode = MV_HASTY;
            u.resting = false;
            start_path(u, away, O_RETREAT, false);
            if (!u.moving() && was_digging) u.order = O_DIGIN; // nowhere to go: it keeps digging
            if (u.moving()) {
                u.order = O_RETREAT;
                if (was_digging) {
                    u.dig_until = 0;
                    u.dig_town = -1;
                    if (u.side == SIDE_PLAYER)
                        push_event(EV_INFO, u.id, false, fmt("%s abandons digging in to break contact.", u.name.c_str()));
                }
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
    // The trains work for the HQ, or once it is lost and the handover is done, for the acting
    // commander, more slowly: nobody is left whose job is to run them.
    const int hq = acting_hq(0);
    const bool acting = hq >= 0 && units[hq].type != U_HQ;
    if (acting && hq != trains_announced_)
        push_event(EV_SUPPLY, hq, true,
                   fmt("%s has taken over the company trains %s. Units halted near it resupply at %d%% of the "
                       "usual rate.",
                       units[hq].name.c_str(), place_name(units[hq].pos).c_str(), (int)(ACTING_SUPPLY_RATE * 100)));
    trains_announced_ = acting ? hq : -1;
    raid_depots();
    // Occupation: every hour hostiles spend in a FOB's territory costs a 24th of its daily income
    // (the locals know where they are, seen or not).
    for (auto &h : holdings) {
        if (h.kind != H_FOB || h.lost || h.side != SIDE_PLAYER) continue;
        bool contested = false;
        for_units_near(h.pos, FOB_TERRITORY, [&](const Unit &o) {
            if (o.alive && o.side != SIDE_PLAYER && hex_distance(o.pos, h.pos) <= FOB_TERRITORY) contested = true;
        });
        if (contested) h.contested_hours++;
    }

    // The trains load straight from a depot while the HQ (or whoever runs them) is halted at it.
    if (hq >= 0 && !units[hq].moving()) {
        const int d = depot_near(units[hq].pos);
        if (d >= 0) {
            Holding &h = holdings[d];
            double di = std::max(0.0, std::min(stock.diesel_cap - stock.diesel_l, h.diesel_l));
            double am = std::max(0.0, std::min(stock.ammo_cap - stock.ammo_t, h.ammo_t));
            double fo = std::max(0.0, std::min(stock.food_cap - stock.food_kg, h.food_kg));
            const double w = di * DIESEL_T_PER_L + am + fo / 1000.0;
            const double k = w > DEPOT_LOAD_T_PER_H ? DEPOT_LOAD_T_PER_H / w : 1.0;
            di *= k, am *= k, fo *= k;
            h.diesel_l -= di, h.ammo_t -= am, h.food_kg -= fo;
            stock.diesel_l += di, stock.ammo_t += am, stock.food_kg += fo;
        }
    }

    stock.personnel = 0;
    for (auto &u : units) {
        if (!u.alive || u.side != SIDE_PLAYER) continue;
        const auto &ti = unit_type_info(u.type);
        stock.personnel += (int)std::round(ti.crew * u.strength / 100.0f);
        if (u.type == U_SUPPLY) continue; // the column fills its own tanks at its depot
        // Resupply: halted and out of the fight for 30 minutes (enemies merely being nearby
        // doesn't stop the trucks), within reach of a depot or FOB (its own staff hand out the
        // stores, faster at a FOB) or else of the company trains.
        bool fighting = time_min - u.last_fired < 30 || time_min - u.last_hit < 30 || time_min - u.last_targeted < 30;
        if (fighting || u.moving()) continue;
        double *diesel = nullptr, *ammo = nullptr;
        float source_rate = 1.0f;
        const int d = depot_near(u.pos);
        if (d >= 0) {
            diesel = &holdings[d].diesel_l;
            ammo = &holdings[d].ammo_t;
            if (holdings[d].kind == H_FOB) source_rate = FOB_SUPPLY_RATE;
        } else if (hq >= 0 && hex_distance(u.pos, units[hq].pos) <= SUPPLY_RADIUS) {
            diesel = &stock.diesel_l;
            ammo = &stock.ammo_t;
            source_rate = acting ? ACTING_SUPPLY_RATE : 1.0f;
        } else {
            continue;
        }
        float rate = (u.resting ? 50.0f : 25.0f) * source_rate;
        float want_fuel = std::min(rate, 100.0f - u.fuel);
        double litres = want_fuel / 100.0 * ti.fuel_litres;
        if (litres > 0 && *diesel > 0) {
            double got = std::min(litres, *diesel);
            *diesel -= got;
            u.fuel += (float)(got / ti.fuel_litres * 100.0);
            u.out_of_fuel_reported = false;
        }
        float want_ammo = std::min(rate, 100.0f - u.ammo);
        double tonnes = want_ammo / 100.0 * ti.ammo_tonnes;
        if (tonnes > 0 && *ammo > 0) {
            double got = std::min(tonnes, *ammo);
            *ammo -= got;
            u.ammo += (float)(got / ti.ammo_tonnes * 100.0);
        }
    }

    // Food at 06:00: the town's market delivers to its depot, then everyone eats, from the depot
    // when they are at it, otherwise from the trains.
    if ((time_min % 1440) == 6 * 60) {
        for (auto &h : holdings)
            if (h.side == SIDE_PLAYER && !h.lost && time_min - h.raided_at >= 6 * 60) h.food_kg += h.food_per_day;
        fob_income();
        std::vector<double> at_depot(holdings.size(), 0.0);
        double with_trains = 0;
        for (const auto &u : units) {
            if (!u.alive || u.side != SIDE_PLAYER) continue;
            const double kg = std::round(unit_type_info(u.type).crew * u.strength / 100.0f) * 1.5;
            const int d = depot_near(u.pos);
            if (d >= 0) at_depot[d] += kg;
            else with_trains += kg;
        }
        auto go_hungry = [&](int depot) {
            for (auto &u : units)
                if (u.alive && u.side == SIDE_PLAYER && depot_near(u.pos) == depot)
                    u.morale = std::max(0.0f, u.morale - 10.0f);
        };
        if (with_trains > 0) {
            if (stock.food_kg >= with_trains) {
                stock.food_kg -= with_trains;
            } else {
                stock.food_kg = 0;
                go_hungry(-1);
                push_event(EV_SUPPLY, -1, true, "The trains are out of food. Morale is falling.");
            }
        }
        for (size_t d = 0; d < holdings.size(); ++d) {
            if (at_depot[d] <= 0) continue;
            if (holdings[d].food_kg >= at_depot[d]) {
                holdings[d].food_kg -= at_depot[d];
            } else {
                holdings[d].food_kg = 0;
                go_hungry((int)d);
                push_event(EV_SUPPLY, -1, true,
                           fmt("The depot at %s is out of food. Morale is falling.", holdings[d].name.c_str()));
            }
        }
    }
    if (time_min >= stock.next_payday) pay_day();

    const bool column_busy = route.state != CV_PARKED && route.convoy >= 0 && units[route.convoy].alive;
    if (stock.diesel_l < 0.25 * stock.diesel_cap && stock.diesel_l > 0 && time_min % 720 == 0 && !column_busy)
        push_event(EV_SUPPLY, -1, true, fmt("Diesel with the trains is running low: %.0f litres left.", stock.diesel_l));
    if (stock.ammo_t < 0.25 * stock.ammo_cap && time_min % 720 == 0 && !column_busy)
        push_event(EV_SUPPLY, -1, true, fmt("Ammunition with the trains is running low: %.1f t left.", stock.ammo_t));
}

// ------------------------------------------------------------------------------------
// Supply line: depots, the supply column, raids and pay
// ------------------------------------------------------------------------------------

namespace {
std::string money(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", std::max(0.0, v));
    std::string s(buf), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return "$" + out;
}
} // namespace

int World::depot_near(HexCoord h, int radius) const {
    int best = -1, bd = radius + 1;
    for (int i = 0; i < (int)holdings.size(); ++i) {
        if (holdings[i].side != SIDE_PLAYER || holdings[i].lost) continue;
        const int d = hex_distance(h, holdings[i].pos);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

double World::trains_need_t() const {
    return std::max(0.0, stock.diesel_cap - stock.diesel_l) * DIESEL_T_PER_L + std::max(0.0, stock.ammo_cap - stock.ammo_t) +
           std::max(0.0, stock.food_cap - stock.food_kg) / 1000.0;
}

double World::column_capacity_t() const {
    if (route.convoy < 0 || route.convoy >= (int)units.size() || !units[route.convoy].alive) return 0;
    return units[route.convoy].elements * TRUCK_LOAD_T;
}

double World::cargo_t() const { return route.diesel_l * DIESEL_T_PER_L + route.ammo_t + route.food_kg / 1000.0; }

double World::weekly_bill() const { return stock.personnel * WAGE_PER_MAN_WEEK; }

double World::food_need_per_day(bool at_depot) const {
    double kg = 0;
    for (const auto &u : units) {
        if (!u.alive || u.side != SIDE_PLAYER) continue;
        if ((depot_near(u.pos) >= 0) != at_depot) continue;
        kg += std::round(unit_type_info(u.type).crew * u.strength / 100.0f) * 1.5;
    }
    return kg;
}

void World::set_supply_route(bool active) {
    route.active = active;
    route.until = 0; // look again at once
    if (active || route.convoy < 0 || route.convoy >= (int)units.size() || !units[route.convoy].alive) return;
    if (route.holding < 0 || route.holding >= (int)holdings.size()) return;
    Unit &c = units[route.convoy];
    // Recalled: a column still loading unloads again; one on its way out turns round.
    if (route.state == CV_LOADING) convoy_unload_at_depot(c);
    else if (route.state == CV_OUTBOUND) convoy_set_off(c, holdings[route.holding].pos, CV_RETURNING);
}

void World::set_convoy_turn_back(bool turn_back) { route.turn_back = turn_back; }

void World::convoy_set_off(Unit &c, HexCoord to, uint8_t state) {
    c.mode = MV_ROAD;
    c.resting = false;
    c.pending.active = false;
    start_path(c, to, O_MOVE, false);
    route.state = state;
    route.dest = to;
    route.until = time_min + 15; // next route check
}

void World::convoy_unload_at_depot(Unit &c) {
    Holding &h = holdings[route.holding];
    h.diesel_l += route.diesel_l;
    h.ammo_t += route.ammo_t;
    h.food_kg += route.food_kg;
    route.diesel_l = route.ammo_t = route.food_kg = 0;
    // The drivers fill their own tanks at the depot: the column's running costs.
    const auto &ti = unit_type_info(c.type);
    const double litres = std::min((100.0 - c.fuel) / 100.0 * ti.fuel_litres, h.diesel_l);
    if (litres > 0) {
        h.diesel_l -= litres;
        c.fuel = std::min(100.0f, c.fuel + (float)(litres / ti.fuel_litres * 100.0));
        c.out_of_fuel_reported = false;
    }
    route.state = CV_PARKED;
}

void World::convoy_hit(Unit &c, int lost) {
    if (c.id != route.convoy || lost <= 0) return;
    const int before = c.elements + lost;
    const double keep = (double)c.elements / before;
    const double lost_t = cargo_t() * (1.0 - keep);
    route.diesel_l *= keep;
    route.ammo_t *= keep;
    route.food_kg *= keep;
    if (c.elements <= 0) { // the destroyed event follows
        route.state = CV_PARKED;
        return;
    }
    push_event(EV_SUPPLY, c.id, true,
               fmt("Supply column loses %d truck%s %s%s. %d left.", lost, lost == 1 ? "" : "s",
                   place_name(c.pos).c_str(), lost_t >= 0.05 ? fmt(" and %.1f t of cargo", lost_t).c_str() : "",
                   c.elements));
}

// The supply column's round: load at the depot what the trains are short of, drive to the
// trains on road march, unload, drive back. It re-routes when the trains move on or when it is
// halted short of them, and falls back on its own like any unit when it is shot up.
void World::update_convoy() {
    if (route.convoy < 0 || route.convoy >= (int)units.size()) return;
    if (route.holding < 0 || route.holding >= (int)holdings.size()) return;
    Unit &c = units[route.convoy];
    if (!c.alive) return;
    if (c.order == O_RETREAT && c.moving()) return; // breaking contact on its own
    Holding &h = holdings[route.holding];
    const int hq = acting_hq(0);
    // Low on its own fuel on the road: the drivers fill up from the diesel aboard.
    if (c.fuel < 20.0f && route.diesel_l > 0) {
        const auto &ti = unit_type_info(c.type);
        const double litres = std::min((100.0 - c.fuel) / 100.0 * ti.fuel_litres, route.diesel_l);
        route.diesel_l -= litres;
        c.fuel = std::min(100.0f, c.fuel + (float)(litres / ti.fuel_litres * 100.0));
        c.out_of_fuel_reported = false;
    }
    // Known enemies within 3 km of the depot (seen there in the last two hours: once the column
    // has left, nobody may be watching them, and raids are reported hourly): a column at the
    // depot drives off, and one away from it doesn't drive back into them. `threat_at` gets the
    // nearest one's position.
    HexCoord threat_at;
    auto depot_threatened = [&]() {
        int best = 1 << 30;
        for_units_near(h.pos, 6, [&](const Unit &o) {
            if (!o.alive || o.side == SIDE_PLAYER) return;
            HexCoord p;
            if (seen_by(o, SIDE_PLAYER)) p = o.pos;
            else if (time_min - o.last_seen[SIDE_PLAYER] <= 120) p = o.last_seen_pos[SIDE_PLAYER];
            else return;
            const int d = hex_distance(p, h.pos);
            if (d <= 6 && d < best) {
                best = d;
                threat_at = p;
            }
        });
        return best <= 6;
    };
    // At the depot with the enemy coming, or driving back to it: drive off (or turn off) with
    // whatever is aboard, towards the trains if they are away from the town, else straight away
    // from the enemy, and wait there about 6 km from the depot.
    const int from_depot = hex_distance(c.pos, h.pos);
    const bool idle_at_depot =
        (route.state == CV_PARKED || route.state == CV_LOADING) && !c.moving() && from_depot <= SUPPLY_RADIUS;
    const bool heading_in = route.state == CV_RETURNING && route.dest == h.pos && from_depot <= 10;
    if ((idle_at_depot || heading_in) && time_min >= route.evac_next && depot_threatened()) {
        route.evac_next = time_min + 15;
        const Vec2 a = hex_center(h.pos);
        Vec2 b = hex_center(threat_at);
        double dx = a.x - b.x, dy = a.y - b.y;
        if ((hq >= 0 && hex_distance(units[hq].pos, h.pos) > 12) || std::hypot(dx, dy) < 1.0) {
            const Vec2 t = hex_center(hq >= 0 ? units[hq].pos : c.pos);
            dx = t.x - a.x, dy = t.y - a.y;
            if (std::hypot(dx, dy) < 1.0) dx = -1.0, dy = 0.0; // nowhere better: west
        }
        const double l = std::max(1.0, std::hypot(dx, dy));
        HexCoord to = world_to_hex(a.x + dx / l * 6000.0, a.y + dy / l * 6000.0);
        if (!map.in_bounds(to) || map.at(to).terrain == T_WATER || !reachable(c.pos, to))
            to = hq >= 0 ? units[hq].pos : c.pos;
        const uint8_t was = route.state;
        const int64_t was_until = route.until;
        convoy_set_off(c, to, CV_RETURNING); // it heads back to the depot once the enemy is gone
        if (!c.moving()) { // nowhere to go: stay put and look again in a while
            route.state = was;
            route.until = was_until;
            return;
        }
        push_event(EV_SUPPLY, c.id, false,
                   fmt("Supply column %s %s with %.1f t aboard: enemies %s. It comes back once they are gone.",
                       idle_at_depot ? "drives out of" : "turns away from", h.name.c_str(), cargo_t(),
                       place_name(threat_at).c_str()));
        return;
    }
    auto route_minutes = [&]() {
        if (!c.moving()) return 0;
        float m = c.step_cost - c.progress;
        for (size_t i = c.path_i + 1; i < c.path.size(); ++i) m += std::max(0.0f, move_cost(c, c.path[i - 1], c.path[i]));
        return (int)m;
    };
    // Where it delivers: the trains (with whoever runs them) or a FOB, which must be away from
    // the depot it loads at.
    const Dest dst = route_dest();
    const std::string to = route_target_name();
    const bool dest_away = dst.ok && hex_distance(dst.pos, h.pos) > SUPPLY_RADIUS;
    switch (route.state) {
        case CV_PARKED: {
            if (!route.active || !dest_away) return;
            if (time_min < route.until) return;
            route.until = time_min + 15;
            if (hex_distance(c.pos, h.pos) > SUPPLY_RADIUS) { // away from its depot (after breaking contact)
                if (!c.moving() && !depot_threatened()) convoy_set_off(c, h.pos, CV_RETURNING);
                return;
            }
            // Load what the destination is short of, as far as the depot has it and the trucks can carry it.
            double di = std::max(0.0, std::min(dst.diesel_cap - *dst.diesel, h.diesel_l));
            double am = std::max(0.0, std::min(dst.ammo_cap - *dst.ammo, h.ammo_t));
            double fo = std::max(0.0, std::min(dst.food_cap - *dst.food, h.food_kg));
            double w = di * DIESEL_T_PER_L + am + fo / 1000.0;
            const double cap = column_capacity_t();
            const bool urgent = *dst.diesel < 0.5 * dst.diesel_cap || *dst.ammo < 0.5 * dst.ammo_cap ||
                                *dst.food < 0.5 * dst.food_cap;
            if (w < 1.0 || (w < 0.25 * cap && !urgent)) return; // not worth a trip yet
            if (w > cap) {
                const double k = cap / w;
                di *= k, am *= k, fo *= k;
            }
            h.diesel_l -= di, h.ammo_t -= am, h.food_kg -= fo;
            route.diesel_l += di, route.ammo_t += am, route.food_kg += fo;
            route.state = CV_LOADING;
            route.until = time_min + CONVOY_LOAD_MIN;
            return;
        }
        case CV_LOADING: {
            if (!route.active || !dest_away) {
                convoy_unload_at_depot(c); // nobody out there to deliver to any more
                return;
            }
            if (time_min < route.until) return;
            convoy_set_off(c, dst.pos, CV_OUTBOUND);
            if (!c.moving()) {
                convoy_unload_at_depot(c);
                route.until = time_min + 60;
                push_event(EV_SUPPLY, c.id, true,
                           fmt("Supply column: no road from %s to %s %s.", h.name.c_str(), to.c_str(),
                               place_name(dst.pos).c_str()));
                return;
            }
            const int mins = route_minutes();
            push_event(EV_SUPPLY, c.id, false,
                       fmt("Supply column leaves %s with %.1f t for %s: %.0f km, about %dh%02d.", h.name.c_str(),
                           cargo_t(), to.c_str(), (c.path.size() - c.path_i) * 0.5, mins / 60, mins % 60));
            return;
        }
        case CV_OUTBOUND: {
            if (!dst.ok || !route.active) { // nobody to deliver to, or recalled
                convoy_set_off(c, h.pos, CV_RETURNING);
                return;
            }
            if (route.turn_back && time_min - c.last_targeted <= 1) {
                convoy_set_off(c, h.pos, CV_RETURNING);
                push_event(EV_SUPPLY, c.id, true,
                           fmt("Supply column under fire %s: turning back to %s.", place_name(c.pos).c_str(), h.name.c_str()));
                return;
            }
            const HexCoord tp = dst.pos;
            if (hex_distance(c.pos, tp) <= SUPPLY_RADIUS) {
                c.path.clear();
                c.path_i = 0;
                c.progress = 0;
                c.order = O_HOLD;
                c.dest = c.pos;
                c.mode = MV_HASTY;
                route.state = CV_UNLOADING;
                route.until = time_min + CONVOY_LOAD_MIN;
                return;
            }
            if (time_min < route.until) return;
            route.until = time_min + (c.moving() ? 15 : 10);
            // Re-route when the trains have moved on, or when the column was halted short of them.
            if (!c.moving() || hex_distance(tp, route.dest) > 4) convoy_set_off(c, tp, CV_OUTBOUND);
            return;
        }
        case CV_UNLOADING: {
            if (!dst.ok) {
                convoy_set_off(c, h.pos, CV_RETURNING);
                return;
            }
            if (hex_distance(c.pos, dst.pos) > SUPPLY_RADIUS + 1) { // the trains moved off: follow them
                route.state = CV_OUTBOUND;
                route.until = 0;
                return;
            }
            if (time_min < route.until) return;
            const double di = std::max(0.0, std::min(route.diesel_l, dst.diesel_cap - *dst.diesel));
            const double am = std::max(0.0, std::min(route.ammo_t, dst.ammo_cap - *dst.ammo));
            const double fo = std::max(0.0, std::min(route.food_kg, dst.food_cap - *dst.food));
            *dst.diesel += di, *dst.ammo += am, *dst.food += fo;
            route.diesel_l -= di, route.ammo_t -= am, route.food_kg -= fo;
            const double w = di * DIESEL_T_PER_L + am + fo / 1000.0;
            route.trips++;
            route.delivered_t += w;
            push_event(EV_SUPPLY, c.id, false,
                       fmt("Supply column delivered %.1f t to %s%s: %.0f L diesel, %.1f t ammo, %.0f kg food.", w,
                           to.c_str(), route.target < 0 ? (" " + place_name(c.pos)).c_str() : "", di, am, fo));
            convoy_set_off(c, h.pos, CV_RETURNING);
            return;
        }
        case CV_RETURNING: {
            if (!c.moving() && hex_distance(c.pos, h.pos) <= SUPPLY_RADIUS) {
                convoy_unload_at_depot(c);
                route.until = time_min;
                return;
            }
            if (!c.moving() && time_min >= route.until) {
                route.until = time_min + 15;
                // Halted short of the depot (blocked, or after breaking contact): try again, unless
                // the enemy is at the depot, in which case wait where it is.
                if (!depot_threatened()) convoy_set_off(c, h.pos, CV_RETURNING);
            }
            return;
        }
        default: route.state = CV_PARKED; return;
    }
}

// Every holding is a target: hostiles next to a depot with no own unit near it carry off part
// of every store each hour. Locals report who it was, so the raiders are known. A FOB that is
// still unguarded at the next hour's check is overrun: the town is lost with what was left.
void World::raid_depots() {
    for (int hi = 0; hi < (int)holdings.size(); ++hi) {
        Holding &h = holdings[hi];
        if (h.side != SIDE_PLAYER || h.lost) continue;
        if (h.kind != H_FOB && h.diesel_l + h.ammo_t + h.food_kg <= 0.5) continue;
        int raider = -1;
        bool guarded = false;
        for_units_near(h.pos, GUARD_RADIUS, [&](const Unit &o) {
            if (!o.alive) return;
            const int d = hex_distance(o.pos, h.pos);
            if (o.side == SIDE_PLAYER) {
                if (o.type != U_SUPPLY && d <= GUARD_RADIUS) guarded = true;
            } else if (d <= RAID_RADIUS && (raider < 0 || o.id < raider)) {
                raider = o.id;
            }
        });
        if (raider < 0 || guarded) continue;
        Unit &r = units[raider];
        r.fuel = 100.0f; // they help themselves
        r.ammo = 100.0f;
        r.seen_until[SIDE_PLAYER] = std::max(r.seen_until[SIDE_PLAYER], time_min + 5);
        r.last_seen[SIDE_PLAYER] = time_min;
        r.last_seen_pos[SIDE_PLAYER] = r.pos;
        if (h.kind == H_FOB && time_min - h.raided_at <= 60) {
            h.lost = true;
            h.diesel_l = h.ammo_t = h.food_kg = 0;
            h.raided_at = time_min;
            if (route.target == hi) route.target = -1; // the column goes back to serving the trains
            push_event(EV_SUPPLY, raider, true,
                       fmt("FOB %s has been overrun by a hostile %s: the town and the stores in it are lost.",
                           h.name.c_str(), unit_type_info(r.type).name));
            continue;
        }
        const double di = h.diesel_l * RAID_SHARE, am = h.ammo_t * RAID_SHARE, fo = h.food_kg * RAID_SHARE;
        h.diesel_l -= di, h.ammo_t -= am, h.food_kg -= fo;
        const bool fresh = time_min - h.raided_at > 120; // only a new raid pauses the game
        h.raided_at = time_min;
        if (h.kind == H_FOB)
            push_event(EV_SUPPLY, raider, true,
                       fmt("Hostile %s is plundering FOB %s, which nobody guards: %.0f L diesel, %.1f t ammo and %.0f "
                           "kg food carried off. Still unguarded in an hour, the FOB is lost.",
                           unit_type_info(r.type).name, h.name.c_str(), di, am, fo));
        else
            push_event(EV_SUPPLY, raider, fresh,
                       fmt("Hostile %s is plundering the unguarded depot at %s: %.0f L diesel, %.1f t ammo and %.0f kg "
                           "food carried off.",
                           unit_type_info(r.type).name, h.name.c_str(), di, am, fo));
    }
}

// Weekly wages from the company chest. Paid in full, morale rises a little; short, it falls,
// and more so when it happens again.
void World::pay_day() {
    stock.next_payday += 7 * 1440;
    const double bill = weekly_bill();
    if (bill <= 0) return;
    if (stock.cash >= bill) {
        stock.cash -= bill;
        stock.missed_paydays = 0;
        for (auto &u : units)
            if (u.alive && u.side == SIDE_PLAYER) u.morale = std::min(90.0f, u.morale + 3.0f);
        push_event(EV_SUPPLY, -1, false,
                   fmt("Pay day: %s paid to %d men. %s left in the chest.", money(bill).c_str(), stock.personnel,
                       money(stock.cash).c_str()));
        return;
    }
    const double paid = stock.cash;
    stock.cash = 0;
    stock.missed_paydays++;
    const float hit = 15.0f * (float)(1.0 - paid / bill) * (stock.missed_paydays >= 2 ? 1.5f : 1.0f);
    for (auto &u : units)
        if (u.alive && u.side == SIDE_PLAYER) u.morale = std::max(0.0f, u.morale - hit);
    push_event(EV_SUPPLY, -1, true,
               fmt("Pay day: only %s of the %s due could be paid. Morale falls.", money(paid).c_str(),
                   money(bill).c_str()));
}

std::string World::convoy_status() const {
    if (route.convoy < 0 || route.convoy >= (int)units.size() || holdings.empty()) return "No supply column.";
    const Unit &c = units[route.convoy];
    const char *depot = holdings[route.holding].name.c_str();
    if (!c.alive) return "The supply column has been destroyed. The trains get no more deliveries.";
    const double km = (c.path.size() - c.path_i) * 0.5;
    if (c.order == O_RETREAT && c.moving())
        return fmt("Breaking contact %s with %.1f t aboard.", place_name(c.pos).c_str(), cargo_t());
    const std::string to = route_target_name();
    const char *dest = to.c_str();
    switch (route.state) {
        case CV_LOADING:
            return fmt("Loading %.1f t at %s for %s, leaves in %d min.", cargo_t(), depot, dest,
                       (int)std::max<int64_t>(0, route.until - time_min));
        case CV_OUTBOUND:
            return c.moving() ? fmt("On the road to %s with %.1f t, %.1f km to go.", dest, cargo_t(), km)
                              : fmt("Halted %s with %.1f t for %s; finding another way.", place_name(c.pos).c_str(), cargo_t(), dest);
        case CV_UNLOADING:
            return fmt("Unloading %.1f t at %s, %d min left.", cargo_t(), dest, (int)std::max<int64_t>(0, route.until - time_min));
        case CV_RETURNING:
            return c.moving() ? fmt("Returning to %s, %.1f km to go.", depot, km)
                              : fmt("Waiting %s to drive back to %s.", place_name(c.pos).c_str(), depot);
        default: break;
    }
    if (!route.active) return fmt("Route stopped: parked at %s.", depot);
    const Holding &h = holdings[route.holding];
    if (route.target < 0) {
        const int hq = acting_hq(0);
        if (hq < 0) return fmt("Waiting at %s: nobody is running the trains.", depot);
        if (hex_distance(units[hq].pos, h.pos) <= SUPPLY_RADIUS)
            return fmt("Parked at %s: the trains load at the depot themselves.", depot);
    }
    if (h.diesel_l * DIESEL_T_PER_L + h.ammo_t + h.food_kg / 1000.0 < 1.0)
        return fmt("Waiting at %s: the depot is empty.", depot);
    return fmt("Waiting at %s until %s %s supplies.", depot, dest, route.target < 0 ? "need" : "needs");
}

World::Dest World::route_dest() {
    Dest d;
    if (route.target >= 0) {
        if (route.target >= (int)holdings.size() || route.target == route.holding) return d;
        Holding &f = holdings[route.target];
        if (f.lost || f.side != SIDE_PLAYER) return d;
        d.ok = true;
        d.pos = f.pos;
        d.diesel = &f.diesel_l, d.ammo = &f.ammo_t, d.food = &f.food_kg;
        d.diesel_cap = f.diesel_cap, d.ammo_cap = f.ammo_cap, d.food_cap = f.food_cap;
        return d;
    }
    const int hq = acting_hq(0);
    if (hq < 0) return d;
    d.ok = true;
    d.pos = units[hq].pos;
    d.diesel = &stock.diesel_l, d.ammo = &stock.ammo_t, d.food = &stock.food_kg;
    d.diesel_cap = stock.diesel_cap, d.ammo_cap = stock.ammo_cap, d.food_cap = stock.food_cap;
    return d;
}

std::string World::route_target_name() const {
    if (route.target >= 0 && route.target < (int)holdings.size()) return "FOB " + holdings[route.target].name;
    return "the trains";
}

bool World::set_route_target(int t) {
    if (t >= 0 && (t >= (int)holdings.size() || t == route.holding || holdings[t].lost || holdings[t].kind != H_FOB ||
                   holdings[t].side != SIDE_PLAYER))
        return false;
    if (t < 0) t = -1;
    if (t == route.target) return true;
    route.target = t;
    route.until = 0; // look again at once (a column on its way re-routes at its next check)
    return true;
}

// ------------------------------------------------------------------------------------
// Forward operating bases
// ------------------------------------------------------------------------------------

int World::town_at(HexCoord h) const {
    if (!map.in_bounds(h)) return -1;
    const uint16_t s = map.at(h).settlement;
    return s == NO_SETTLEMENT || s >= map.settlements.size() ? -1 : (int)s;
}

int World::holding_at_settlement(int s) const {
    if (s < 0) return -1;
    for (int i = 0; i < (int)holdings.size(); ++i)
        if (!holdings[i].lost && holdings[i].side == SIDE_PLAYER && holdings[i].settlement == s) return i;
    return -1;
}

int World::dig_in_minutes(const Unit &u) const {
    const double worn = std::max(0.0, std::min(1.0, (100.0 - u.readiness) / 100.0));
    return (int)std::lround(DIG_IN_MIN_FRESH + (DIG_IN_MIN_MAX - DIG_IN_MIN_FRESH) * worn);
}

std::string World::dig_in_problem(int unit_id) const {
    if (unit_id < 0 || unit_id >= (int)units.size()) return "No such unit.";
    const Unit &u = units[unit_id];
    if (!u.alive || u.side != SIDE_PLAYER) return "Only your own units can set up a FOB.";
    if (u.type != U_HQ) return "Only an HQ can dig in and set up a FOB.";
    if (u.order == O_DIGIN) return fmt("%s is already digging in.", u.name.c_str());
    const int s = town_at(u.pos);
    if (s < 0) return "A FOB needs a town: move the HQ into a built-up area first.";
    const int held = holding_at_settlement(s);
    if (held >= 0)
        return holdings[held].kind == H_FOB ? fmt("%s already has a FOB.", map.settlements[s].name.c_str())
                                            : fmt("%s is your home depot already.", map.settlements[s].name.c_str());
    return "";
}

int World::order_dig_in(int unit_id) {
    if (!dig_in_problem(unit_id).empty()) return -1;
    Unit &u = units[unit_id];
    const int delay = command_delay(u);
    u.pending = {true, O_DIGIN, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

bool World::cancel_dig_in(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size()) return false;
    Unit &u = units[unit_id];
    if (u.pending.active && u.pending.kind == O_DIGIN) {
        u.pending.active = false;
        return true;
    }
    if (u.order != O_DIGIN) return false;
    u.order = O_HOLD;
    u.dig_until = 0;
    u.dig_town = -1;
    if (u.side == SIDE_PLAYER)
        push_event(EV_INFO, u.id, false, fmt("%s stops digging in %s.", u.name.c_str(), place_name(u.pos).c_str()));
    return true;
}

void World::abandon_dig_in(Unit &u, const char *why) {
    if (u.order != O_DIGIN) return;
    u.order = O_HOLD;
    u.dig_until = 0;
    u.dig_town = -1;
    if (u.side == SIDE_PLAYER)
        push_event(EV_INFO, u.id, true,
                   fmt("%s abandons digging in %s: %s.", u.name.c_str(), place_name(u.pos).c_str(), why));
}

// HQs digging in: under fire they give it up; when the time is up the town is a FOB.
void World::update_dig_in() {
    for (auto &u : units) {
        if (!u.alive || u.order != O_DIGIN) continue;
        if (u.last_hit == time_min) {
            abandon_dig_in(u, "it is under fire");
            continue;
        }
        if (time_min < u.dig_until) continue;
        const int s = u.dig_town;
        u.order = O_HOLD;
        u.dig_until = 0;
        u.dig_town = -1;
        if (s < 0 || s >= (int)map.settlements.size() || holding_at_settlement(s) >= 0) continue;
        const Settlement &S = map.settlements[s];
        Holding f;
        f.name = S.name;
        f.settlement = s;
        f.pos = u.pos;
        f.side = SIDE_PLAYER;
        f.kind = H_FOB;
        f.diesel_cap = FOB_DIESEL_CAP, f.ammo_cap = FOB_AMMO_CAP, f.food_cap = FOB_FOOD_CAP;
        f.income_per_day = std::max(FOB_INCOME_MIN, std::min(FOB_INCOME_MAX, S.population * FOB_INCOME_PER_HEAD));
        f.established_at = time_min;
        holdings.push_back(f);
        push_event(EV_SUPPLY, u.id, true,
                   fmt("FOB %s is set up. It holds up to %.0f L diesel, %.0f t ammo and %.0f kg food, and the town "
                       "pays %s a day while you hold it. Send it supplies from the supply panel (I).",
                       S.name.c_str(), FOB_DIESEL_CAP, FOB_AMMO_CAP, FOB_FOOD_CAP, money(f.income_per_day).c_str()));
    }
}

double World::fob_income_per_day() const {
    double d = 0;
    for (const auto &h : holdings)
        if (h.kind == H_FOB && !h.lost && h.side == SIDE_PLAYER) d += h.income_per_day;
    return d;
}

// Occupation income at 06:00, less the hours of the past day that hostiles spent in each
// FOB's territory (5 km). Counted hourly in update_logistics.
void World::fob_income() {
    double total = 0;
    int n = 0;
    for (auto &h : holdings) {
        if (h.kind != H_FOB || h.lost || h.side != SIDE_PLAYER) continue;
        const double earned = h.income_per_day * (24 - std::min(24, h.contested_hours)) / 24.0;
        h.contested_hours = 0;
        total += earned;
        ++n;
    }
    if (n == 0) return;
    stock.cash += total;
    push_event(EV_SUPPLY, -1, false,
               fmt("Income from %d occupied town%s: %s.", n, n == 1 ? "" : "s", money(total).c_str()));
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
        if (u.alive && u.type != U_HQ && u.type != U_ARTILLERY && u.type != U_SUPPLY && u.strength >= 30.0f) ++n;
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
        stock.cash += CONTRACT_PAYMENT;
        outcome_text_ = formations[sf].name + " is broken. The contract is fulfilled: " + money(CONTRACT_PAYMENT) +
                        " paid on completion.";
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
constexpr char kSaveMagic[8] = {'M', 'U', 'R', 'K', 'S', 'S', 'V', '5'};

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

// Orders and stances are written field by field: their padding bytes are indeterminate (they
// are set from brace temporaries), and saves must be byte-identical for the replay checks.
template <typename A, typename P> void io_pending(A &a, P &p) {
    a.pod(p.active); a.pod(p.kind); a.pod(p.target); a.pod(p.issued_at); a.pod(p.activates_at); a.pod(p.mode);
}

template <typename A, typename P> void io_stance(A &a, P &p) {
    a.pod(p.active); a.pod(p.roe); a.pod(p.brk); a.pod(p.activates_at); a.pod(p.standoff);
}

template <typename A, typename U> void io_unit(A &a, U &u) {
    a.pod(u.id); a.pod(u.side); a.pod(u.type); a.str(u.name); a.str(u.equipment);
    a.pod(u.formation); a.pod(u.slot); a.pod(u.pos); a.pod(u.prev_pos); a.pod(u.dest);
    a.ints(u.path);
    uint64_t pi = u.path_i;
    a.pod(pi);
    u.path_i = (size_t)pi;
    a.pod(u.progress); a.pod(u.step_cost); a.pod(u.order); io_pending(a, u.pending); a.pod(u.mode);
    a.pod(u.roe); a.pod(u.brk); io_stance(a, u.pending_stance); a.pod(u.resting); a.pod(u.last_targeted);
    a.pod(u.elements_max); a.pod(u.elements); a.pod(u.acquired); a.pod(u.group); a.pod(u.follow);
    a.pod(u.standoff); a.pod(u.standoff_next);
    a.pod(u.strength); a.pod(u.morale); a.pod(u.fuel); a.pod(u.ammo); a.pod(u.readiness);
    a.pod(u.zone); a.pod(u.hot_until); a.pod(u.fire_cooldown); a.pod(u.last_fired); a.pod(u.last_hit);
    a.pod(u.last_target); a.pod(u.alive); a.pod(u.out_of_fuel_reported);
    a.pod(u.seen_until); a.pod(u.last_seen); a.pod(u.last_seen_pos); a.pod(u.ai_next);
    a.pod(u.dig_until); a.pod(u.dig_town);
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

template <typename A, typename H> void io_holding(A &a, H &h) {
    a.str(h.name); a.pod(h.settlement); a.pod(h.pos); a.pod(h.side);
    a.pod(h.diesel_l); a.pod(h.ammo_t); a.pod(h.food_kg); a.pod(h.food_per_day); a.pod(h.raided_at);
    a.pod(h.kind); a.pod(h.lost); a.pod(h.diesel_cap); a.pod(h.ammo_cap); a.pod(h.food_cap);
    a.pod(h.income_per_day); a.pod(h.contested_hours); a.pod(h.established_at);
}

template <typename A, typename R> void io_route(A &a, R &r) {
    a.pod(r.active); a.pod(r.turn_back); a.pod(r.holding); a.pod(r.convoy); a.pod(r.state); a.pod(r.until);
    a.pod(r.diesel_l); a.pod(r.ammo_t); a.pod(r.food_kg); a.pod(r.dest); a.pod(r.trips); a.pod(r.delivered_t);
    a.pod(r.evac_next); a.pod(r.target);
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
    w.pod((uint32_t)holdings.size());
    for (const auto &h : holdings) io_holding(w, const_cast<Holding &>(h));
    io_route(w, const_cast<SupplyRoute &>(route));
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
    std::vector<Holding> hs(r.count(4096));
    for (auto &h : hs) io_holding(r, h);
    SupplyRoute rt;
    io_route(r, rt);
    if (!r.ok) return fail("save is truncated or corrupt");
    if (rt.convoy < -1 || rt.convoy >= (int)us.size() || rt.state > CV_RETURNING ||
        (!hs.empty() && (rt.holding < 0 || rt.holding >= (int)hs.size())) || rt.target < -1 ||
        rt.target >= (int)hs.size())
        return fail("save is corrupt");
    const int n_towns = (int)map.settlements.size();
    for (const auto &h : hs)
        if (!map.in_bounds(h.pos) || h.settlement >= n_towns || h.kind > H_FOB) return fail("save is corrupt");
    for (const auto &u : us)
        if (u.dig_town < -1 || u.dig_town >= n_towns || u.order > O_DIGIN) return fail("save is corrupt");
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
    holdings = std::move(hs);
    route = rt;
    events_.clear();
    rebuild_buckets();
    // As of the last hourly logistics pass, so a load doesn't announce the acting commander again.
    {
        int a = acting_hq(0, time_min - time_min % 60);
        trains_announced_ = a >= 0 && units[a].type != U_HQ ? a : -1;
    }
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
