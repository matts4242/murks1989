#include "murks/sim.hpp"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace murks {

namespace {

constexpr int BUCKET = 32;          // hexes per spatial bucket side
constexpr int HOT_RANGE = 20;       // hexes (10 km)
constexpr int WARM_RANGE = 80;      // hexes (40 km)
constexpr int HOT_HOLD_MIN = 30;    // minutes a zone stays hot after contact ends
constexpr int RADIO_RANGE = 40;     // hexes (20 km): vehicle radio (R-123) before couriers are needed
constexpr int RADIO_RANGE_FOOT = 12; // hexes (6 km): manpack radio (R-107) for units on foot
constexpr int MAX_PATHS_PER_TICK = 10;
constexpr int CONTACT_RANGE = 10;   // hexes (5 km): a hostile in sight this close caps fast-forward

const UnitTypeInfo kTypes[U_TYPE_COUNT] = {
    // name        mobility    spot wpn  fire  anti-arm armd  indir intv fuel/hex litres ammo_t crew rnds %/rnd
    // Tanks: 1-minute cycle of 2 aimed rounds each (DIA 1976: 3 rounds in 100 s, 1 per 15-45 s
    // short halt), 40 rounds a load (T-55 43, T-72 44). Guns: 4-minute missions (FM 100-2-1).
    {"HQ", M_WHEELED, 8, 2, 1.0f, 0.1f, false, false, 3, 0.10f, 600, 0.4f, 25, 1, 0},
    {"Tank Plt", M_TRACKED, 8, 5, 9.0f, 1.0f, true, false, 1, 0.15f, 4000, 6.0f, 16, 2, 2.5f},
    {"Mech Plt", M_TRACKED, 8, 4, 6.0f, 0.8f, true, false, 3, 0.12f, 1800, 3.0f, 30, 1, 0},
    {"Rifle Plt", M_FOOT, 6, 2, 4.0f, 0.5f, false, false, 3, 0.0f, 0, 1.0f, 35, 1, 0},
    {"Recon Sect", M_WHEELED, 14, 3, 2.5f, 0.25f, false, false, 3, 0.08f, 500, 0.5f, 15, 1, 0},
    {"Arty Bty", M_TRACKED, 6, 30, 12.0f, 0.6f, false, true, 4, 0.12f, 2400, 18.0f, 30, 1, 0},
    // Supply column: 6 Ural-375D (4.5 t each), 2 men a truck with small arms. About 48 L/100 km
    // loaded: 1.4 L a hex for the column, 0.08% of its 1,800 L. First estimates (MRK-09).
    {"Supply Col", M_WHEELED, 6, 1, 0.5f, 0.05f, false, false, 3, 0.08f, 1800, 0.2f, 12, 1, 0},
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

float cover_mod(const Hex &h, bool soft_target) { return terrain_cover_factor(h.terrain, soft_target); }

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

} // namespace

float terrain_cover_factor(int terrain, bool soft_target) {
    switch (terrain) {
        case T_FOREST: return soft_target ? 0.5f : 0.7f;
        case T_URBAN: return soft_target ? 0.4f : 0.6f;
        case T_WETLAND: return 0.9f;
        default: return 1.0f;
    }
}

float terrain_minutes(int mobility, int terrain) { return kTerrainCost[mobility][terrain]; }
float road_minutes(int mobility) { return kRoadCost[mobility]; }
float river_penalty_minutes(int mobility) { return kRiverPenalty[mobility]; }

const UnitTypeInfo &unit_type_info(int type) { return kTypes[type < 0 || type >= U_TYPE_COUNT ? 0 : type]; }

const char *holding_kind_name(int kind) {
    static const char *names[HK_COUNT] = {"Town", "Farm collective", "Fuel depot", "Ammo dump"};
    return names[kind < 0 || kind >= HK_COUNT ? 0 : kind];
}

const char *levy_kind_name(int kind) {
    static const char *names[LV_COUNT] = {"food", "fuel", "cash", "men"};
    return names[kind < 0 || kind >= LV_COUNT ? 0 : kind];
}

const char *convoy_state_name(int state) {
    static const char *names[6] = {"With the trains", "To the source", "Loading", "To the destination",
                                   "Unloading", "Returning to the trains"};
    return names[state < 0 || state >= 6 ? 0 : state];
}

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

// Maximum range with the standard HE round, from FM 100-2-3 (1991), Jane's and the makers' data.
// Rocket-assisted rounds (2S1 21.9 km, 2S3 24 km, M109A2 23.5 km, ...) are left out: few were
// issued, and a 1989 company would not have them.
static const GunInfo kGuns[] = {
    // Warsaw Pact howitzers and guns
    {"2S1", "122 mm SP howitzer 2S1", 15.3},
    {"D-30", "122 mm howitzer D-30", 15.4},
    {"D-30A", "122 mm howitzer D-30A", 15.4},
    {"M-30", "122 mm howitzer M-30", 11.8},
    {"M1938", "122 mm howitzer M-30", 11.8},
    {"2S3", "152 mm SP howitzer 2S3", 18.5},
    {"D-20", "152 mm gun-howitzer D-20", 17.4},
    {"ML-20", "152 mm gun-howitzer ML-20", 17.2},
    {"D-1", "152 mm howitzer D-1", 12.4},
    {"DANA", "152 mm SP howitzer vz. 77 DANA", 18.7},
    {"2S19", "152 mm SP howitzer 2S19", 24.7},
    {"2A65", "152 mm howitzer 2A65", 24.7},
    {"2S5", "152 mm SP gun 2S5", 27.0},
    {"2A36", "152 mm gun 2A36", 27.0},
    {"2S7", "203 mm SP gun 2S7", 37.5},
    // Rocket launchers
    {"BM-21", "122 mm MRL BM-21", 20.4},
    {"RM-70", "122 mm MRL RM-70", 20.4},
    {"BM-27", "220 mm MRL BM-27", 35.0},
    // Mortars and gun-mortars
    {"2S9", "120 mm SP gun-mortar 2S9", 8.8},
    {"2S4", "240 mm SP mortar 2S4", 9.6},
    {"2S12", "120 mm mortar 2S12", 7.1},
    {"2B11", "120 mm mortar 2B11", 7.1},
    {"M-1943", "120 mm mortar M-1943", 5.7},
    {"PM-43", "120 mm mortar M-1943", 5.7},
    {"2B9", "82 mm automatic mortar 2B9", 4.3},
    {"2B14", "82 mm mortar 2B14", 4.3},
    {"M-37", "82 mm mortar M-37", 3.0},
    // NATO
    {"M109", "155 mm SP howitzer M109", 14.6},
    {"M109A1", "155 mm SP howitzer M109A1", 18.1},
    {"M109A2", "155 mm SP howitzer M109A2", 18.1},
    {"M109A3", "155 mm SP howitzer M109A3", 18.1},
    {"M110", "203 mm SP howitzer M110", 16.8},
    {"M110A2", "203 mm SP howitzer M110A2", 22.9},
    {"M198", "155 mm howitzer M198", 18.1},
    {"M114", "155 mm howitzer M114", 14.6},
    {"FH-70", "155 mm howitzer FH-70", 24.0},
    {"FH70", "155 mm howitzer FH-70", 24.0},
    {"M101", "105 mm howitzer M101", 11.3},
    {"M102", "105 mm howitzer M102", 11.5},
    {"L118", "105 mm light gun L118", 17.2},
    {"M270", "227 mm MLRS M270", 32.0},
    {"MLRS", "227 mm MLRS M270", 32.0},
    {"LARS", "110 mm MRL LARS", 14.0},
    {"L16", "81 mm mortar L16", 5.7},
    {"M252", "81 mm mortar M252", 5.6},
    {"M29", "81 mm mortar M29", 4.6},
    {"M30", "107 mm mortar M30", 6.8},
};

const GunInfo *gun_info(const std::string &e) {
    auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    for (const GunInfo &g : kGuns) {
        const size_t n = std::strlen(g.key);
        for (size_t at = e.find(g.key); at != std::string::npos; at = e.find(g.key, at + 1)) {
            const bool start = at == 0 || !alnum(e[at - 1]);
            const bool end = at + n == e.size() || !alnum(e[at + n]);
            if (start && end) return &g;
        }
    }
    return nullptr;
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
    routes.clear();
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
    routes.clear();
    events_.clear();
    stock = CompanyStock{};
    used_diesel_l_ = used_ammo_t_ = 0;
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
    {
        // The company trains' trucks: a supply column that works the supply routes.
        int tr = spawn(0, SIDE_PLAYER, U_TRUCK, "Supply Column", "6x Ural-375D", {home.col - 1, home.row + 1}, taken);
        units[tr].roe = ROE_RETURN;
        units[tr].brk = BC_LIGHT;
    }

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

    // Squads for every APC/IFV platoon, embarked. Added last so every other unit keeps its id.
    for (int i = 0, n = (int)units.size(); i < n; ++i)
        if (units[i].type == U_MECH) add_dismounts(i);
    for (auto &u : units) {
        if (u.side == SIDE_PLAYER && u.carrier < 0) stock.personnel += unit_type_info(u.type).crew;
    }
    for (int f = 0; f < (int)formations.size(); ++f) formations[f].start_combat = effective_combat_units(f);
    setup_holdings();
    push_event(EV_INFO, -1, false,
               fmt("Company Murks is billeted at %s. The 7th Guards Remnant is moving on the town from the east: "
                   "break it before it breaks you.", S[player_home].name.c_str()));
    rebuild_buckets();
    update_zones();
}

// ------------------------------------------------------------------------------------
// Holdings: towns, farms and depots the company can hold (MRK-08)
// ------------------------------------------------------------------------------------

// Towns of 5,000+ within about 120 km of home are holdings; the home town is the company's.
// Around it, deterministically from the settlement list: one fuel depot and one ammo dump at
// villages 15-50 km out (a remnant army's stores), and four farm collectives at villages in
// farmland within 60 km. All first estimates; everything else starts independent.
void World::setup_holdings() {
    holdings.clear();
    routes.clear();
    const auto &S = map.settlements;
    if (S.empty() || player_home < 0) return;
    const HexCoord home = S[player_home].pos;
    std::vector<bool> used(S.size(), false);
    auto add = [&](int si, HoldingKind kind, int8_t owner, float loyalty) {
        Holding h;
        h.settlement = si;
        h.name = S[si].name;
        h.pos = S[si].pos;
        h.radius = std::max(1, S[si].radius);
        h.kind = kind;
        h.owner = owner;
        h.known_owner = owner;
        h.loyalty = loyalty;
        h.population = S[si].population;
        h.manpower = owner >= 0 ? (int)(h.population * 0.005) : (int)(h.population * 0.002); // volunteers so far
        switch (kind) {
            case HK_TOWN: h.diesel_l = h.population * 1.0; break; // what the town's garages still hold
            case HK_FARM: h.food_kg = 2000; break;
            case HK_FUEL_DEPOT: h.diesel_l = 40000; break;
            case HK_AMMO_DUMP: h.ammo_t = 60; break;
            default: break;
        }
        used[si] = true;
        holdings.push_back(h);
    };
    add(player_home, HK_TOWN, SIDE_PLAYER, 50.0f);
    for (int i = 0; i < (int)S.size(); ++i) {
        if (used[i] || S[i].population < 5000) continue;
        if (hex_distance(S[i].pos, home) > 240 || !reachable(S[i].pos, home)) continue;
        add(i, HK_TOWN, -1, 30.0f);
    }
    // Depots: villages 15-50 km out, the first on rail (an army depot sits on a railway), then any.
    auto pick_village = [&](int min_d, int max_d, bool want_rail, bool want_fields) {
        int best = -1, bd = 1 << 30;
        for (int i = 0; i < (int)S.size(); ++i) {
            if (used[i] || S[i].population >= 5000 || S[i].population < 1000) continue;
            int d = hex_distance(S[i].pos, home);
            if (d < min_d || d > max_d || !reachable(S[i].pos, home)) continue;
            const Hex &x = map.at(S[i].pos);
            if (want_rail && !(x.features & F_RAIL)) continue;
            if (want_fields && x.terrain != T_FIELDS) {
                bool fields = false;
                for (int dir = 0; dir < 6 && !fields; ++dir) {
                    HexCoord n = neighbor(S[i].pos, dir);
                    fields = map.in_bounds(n) && map.at(n).terrain == T_FIELDS;
                }
                if (!fields) continue;
            }
            // Prefer the middle of the band so depots aren't right on top of the company.
            int score = std::abs(d - (min_d + max_d) / 2);
            if (score < bd) {
                bd = score;
                best = i;
            }
        }
        return best;
    };
    int fuel = pick_village(30, 100, true, false);
    if (fuel < 0) fuel = pick_village(30, 100, false, false);
    if (fuel >= 0) add(fuel, HK_FUEL_DEPOT, -1, 30.0f);
    int ammo = pick_village(30, 100, true, false);
    if (ammo < 0) ammo = pick_village(30, 100, false, false);
    if (ammo >= 0) add(ammo, HK_AMMO_DUMP, -1, 30.0f);
    for (int k = 0; k < 4; ++k) {
        int f = pick_village(8, 120, false, true);
        if (f < 0) break;
        add(f, HK_FARM, -1, 30.0f);
    }
}

int World::holding_at(HexCoord h) const {
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < (int)holdings.size(); ++i) {
        int d = hex_distance(holdings[i].pos, h);
        if (d <= holdings[i].radius && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

int World::present_in(const Holding &h, int side) const {
    int n = 0;
    for_units_near(h.pos, h.radius + 1, [&](const Unit &u) {
        if (!u.alive || u.side != side || u.type == U_TRUCK || u.embarked) return;
        if (in_place(h, u.pos)) ++n;
    });
    return n;
}

bool World::can_levy(const Holding &h, int side) const {
    bool ok = false;
    for_units_near(h.pos, h.radius + 1, [&](const Unit &u) {
        if (!u.alive || u.side != side || u.type == U_TRUCK || u.militia || u.embarked || u.moving()) return;
        if (in_place(h, u.pos)) ok = true;
    });
    return ok;
}

// Taxes of 0.5 DM a head and a food surplus of 20 g a head per day at full loyalty; a farm
// collective gives 400 kg a day. First estimates for a post-war spring.
void World::daily_yield(const Holding &h, double &cash, double &food_kg) const {
    cash = food_kg = 0;
    if (h.owner < 0) return;
    const double l = h.loyalty / 100.0;
    switch (h.kind) {
        case HK_TOWN:
            cash = h.population * 0.5 * l;
            food_kg = h.population * 0.02 * l;
            break;
        case HK_FARM: food_kg = 400.0 * l; break;
        default: break;
    }
}

int World::militia_max_squads(const Holding &h) const {
    if (h.kind != HK_TOWN) return 0;
    return std::min(MILITIA_MAX_SQUADS, h.population / MILITIA_PER_POP);
}

int World::militia_squads(const Holding &h) const {
    if (h.militia_unit < 0 || h.militia_unit >= (int)units.size()) return 0;
    const Unit &m = units[h.militia_unit];
    return m.alive ? m.elements : 0;
}

bool World::orderable(const Unit &u) const {
    if (u.militia) return false;
    if (u.type == U_TRUCK && u.convoy_route >= 0) return false;
    return true;
}

bool World::set_training(int hid, bool on) {
    if (hid < 0 || hid >= (int)holdings.size()) return false;
    Holding &h = holdings[hid];
    if (h.owner != SIDE_PLAYER || h.kind != HK_TOWN || militia_max_squads(h) == 0) return false;
    if (on && h.loyalty < 25.0f) return false;
    if (h.training == on) return true;
    h.training = on;
    if (on)
        push_event(EV_HOLDING, -1, false,
                   fmt("%s is raising militia: a squad of %d every %d days, up to %d squads.", h.name.c_str(),
                       MILITIA_SQUAD_MEN, MILITIA_TRAIN_MIN / 1440, militia_max_squads(h)));
    return true;
}

std::string World::levy(int hid, int what) {
    if (hid < 0 || hid >= (int)holdings.size() || what < 0 || what >= LV_COUNT) return "no such holding";
    Holding &h = holdings[hid];
    if (h.owner != SIDE_PLAYER) return "the company does not hold " + h.name;
    if (h.kind != HK_TOWN && h.kind != HK_FARM) return "nothing to requisition at a depot: its stores are already yours";
    if (!can_levy(h, SIDE_PLAYER)) return "a unit of the company must be halted in " + h.name + " to requisition";
    if (h.levy_until[what] > time_min)
        return fmt("%s was requisitioned for %s less than a day ago (again in %d h)", h.name.c_str(),
                   levy_kind_name(what), (int)((h.levy_until[what] - time_min + 59) / 60));
    double got = 0;
    float cost = 10.0f;
    switch (what) {
        case LV_FOOD:
            got = h.population * (h.kind == HK_FARM ? 0.5 : 0.1);
            h.food_kg += got;
            break;
        case LV_FUEL:
            if (h.kind == HK_FARM) return "a farm collective has no fuel to spare";
            got = h.population * 0.5;
            h.diesel_l += got;
            break;
        case LV_CASH:
            if (h.kind == HK_FARM) return "a farm collective has no cash to speak of";
            got = h.population * 2.0;
            h.cash += got;
            cost = 15.0f;
            break;
        case LV_MEN:
            if (h.manpower <= 0) return "nobody in " + h.name + " is willing to serve right now";
            got = h.manpower;
            stock.replacements += h.manpower;
            h.manpower = 0;
            cost = 15.0f;
            break;
        default: break;
    }
    h.levy_until[what] = time_min + LEVY_COOLDOWN_MIN;
    h.loyalty = std::max(0.0f, h.loyalty - cost);
    static const char *units_of[LV_COUNT] = {"kg of food", "L of fuel", "DM", "men"};
    push_event(EV_HOLDING, -1, false,
               fmt("Requisition in %s: %.0f %s%s. Loyalty falls to %.0f.", h.name.c_str(), got, units_of[what],
                   what == LV_MEN ? " join the company's replacements" : " go into the holding's stores",
                   h.loyalty));
    return "";
}

void World::capture(Holding &h, int side) {
    const int8_t was = h.owner;
    h.owner = (int8_t)side;
    h.loyalty = 20.0f; // taken: the people pay, grudgingly
    h.contested_since = -1;
    h.captured_at = time_min;
    h.training = false;
    h.training_progress = 0;
    if (h.militia_unit >= 0 && !units[h.militia_unit].alive) h.militia_unit = -1;
    std::string stores;
    if (h.diesel_l >= 1) stores += fmt(" %.0f L diesel", h.diesel_l);
    if (h.ammo_t >= 0.1) stores += fmt(" %.1f t ammunition", h.ammo_t);
    if (h.food_kg >= 1) stores += fmt(" %.0f kg food", h.food_kg);
    if (h.cash >= 1) stores += fmt(" %.0f DM", h.cash);
    if (side == SIDE_PLAYER) {
        h.known_owner = SIDE_PLAYER;
        push_event(EV_HOLDING, -1, true,
                   fmt("Company Murks takes %s (%s, pop. %d).%s Loyalty %.0f: taken, not won over.", h.name.c_str(),
                       holding_kind_name(h.kind), h.population,
                       stores.empty() ? "" : (" Stores found:" + stores + ".").c_str(), h.loyalty));
    } else if (was == SIDE_PLAYER) {
        h.known_owner = (int8_t)side;
        push_event(EV_HOLDING, -1, true,
                   fmt("%s has been taken by hostile forces.%s", h.name.c_str(),
                       stores.empty() ? "" : (" Lost with it:" + stores + ".").c_str()));
        // Routes through a lost source stop: the stores are no longer ours.
        for (auto &r : routes)
            if (r.active && r.side == SIDE_PLAYER &&
                (&holdings[r.source] == &h || (r.dest >= 0 && &holdings[r.dest] == &h)))
                r.active = false;
    }
}

// A town's militia: one unit per holding, growing a squad at a time. They hold the town, take
// no orders and fight to the end for their homes (BC_NEVER); untrained at first.
void World::add_militia_squad(Holding &h) {
    if (h.militia_unit >= 0 && units[h.militia_unit].alive) {
        Unit &m = units[h.militia_unit];
        ++m.elements_max;
        ++m.elements;
        m.strength = 100.0f * m.elements / m.elements_max;
        m.equipment = fmt("%d squads of %d militia, rifles", m.elements_max, MILITIA_SQUAD_MEN);
        return;
    }
    Unit m;
    m.id = (int)units.size();
    m.side = h.owner;
    m.type = U_INFANTRY;
    m.name = h.name + " Militia";
    m.equipment = fmt("1 squad of %d militia, rifles", MILITIA_SQUAD_MEN);
    m.formation = 0;
    for (int f = 0; f < (int)formations.size(); ++f)
        if (formations[f].side == h.owner) {
            m.formation = f;
            break;
        }
    m.slot = (int)formations[m.formation].members.size();
    m.pos = m.prev_pos = m.dest = h.pos;
    m.last_seen_pos[0] = m.last_seen_pos[1] = h.pos;
    m.morale = 55;
    m.readiness = 60;
    m.elements_max = m.elements = 1;
    m.roe = ROE_FREE;
    m.brk = BC_NEVER;
    m.standoff = 0;
    m.militia = true;
    m.holding = (int)(&h - &holdings[0]);
    m.squad_men = MILITIA_SQUAD_MEN;
    m.ai_next = rng_.range_int(0, 30);
    formations[m.formation].members.push_back(m.id);
    units.push_back(m);
    h.militia_unit = m.id;
    bucket_insert(units.back());
}

// Hourly: who holds each place, loyalty, and militia training.
void World::update_holdings() {
    for (int hi = 0; hi < (int)holdings.size(); ++hi) {
        Holding &h = holdings[hi];
        int present[SIDE_COUNT];
        for (int s = 0; s < SIDE_COUNT; ++s) present[s] = present_in(h, s);
        // Fighting inside the place in the last hour (collateral damage and fear).
        bool fighting = false;
        for_units_near(h.pos, h.radius + 1, [&](const Unit &u) {
            if (u.alive && in_place(h, u.pos) && (time_min - u.last_hit < 60 || time_min - u.last_fired < 60)) fighting = true;
        });
        if (fighting) h.last_fight = time_min;
        // Capture: one side alone in the place for CAPTURE_MIN takes it. Both present: the fight decides.
        const int sides = (present[0] > 0) + (present[1] > 0);
        if (sides == 1) {
            const int s = present[0] > 0 ? 0 : 1;
            if (s == h.owner) {
                h.contested_since = -1;
            } else if (h.contested_since < 0) {
                h.contested_since = time_min;
            } else if (time_min - h.contested_since >= CAPTURE_MIN) {
                capture(h, s);
            }
        } else {
            h.contested_since = -1;
        }
        // What the player knows: ownership is seen when the company is within 10 km (or the place is its own).
        bool own_near = h.owner == SIDE_PLAYER;
        if (!own_near)
            for_units_near(h.pos, 20, [&](const Unit &u) {
                if (u.alive && u.side == SIDE_PLAYER && hex_distance(u.pos, h.pos) <= 20) own_near = true;
            });
        if (own_near) h.known_owner = h.owner;
        if (h.owner < 0) continue;
        // Loyalty to the owner: protection raises it, enemies nearby with nobody to stop them,
        // fighting in the streets and requisitions lower it.
        const int enemy = 1 - h.owner;
        bool enemy_near = false;
        for_units_near(h.pos, 10, [&](const Unit &u) {
            if (u.alive && u.side == enemy && u.type != U_TRUCK && hex_distance(u.pos, h.pos) <= 10) enemy_near = true;
        });
        const bool militia_here = militia_squads(h) > 0;
        const bool troops_here = present[h.owner] > (militia_here ? 1 : 0);
        if (present[h.owner] > 0) h.loyalty += 0.05f + ((militia_here && troops_here) ? 0.05f : 0.0f);
        if (enemy_near && present[h.owner] == 0) h.loyalty -= 0.3f;
        if (fighting) h.loyalty -= 2.0f;
        h.loyalty = std::max(0.0f, std::min(100.0f, h.loyalty));
        // Militia training (the player's towns): a squad every MILITIA_TRAIN_MIN from the manpower pool.
        if (h.training && h.owner == SIDE_PLAYER) {
            const int squads = militia_squads(h);
            const int cap = militia_max_squads(h);
            if (squads >= cap) {
                h.training = false;
                h.training_progress = 0;
                push_event(EV_HOLDING, h.militia_unit, false,
                           fmt("%s has all the militia it can raise: %d squads.", h.name.c_str(), squads));
            } else if (h.loyalty < 25.0f) {
                h.training = false;
                h.training_progress = 0;
                push_event(EV_HOLDING, -1, true, fmt("%s stops raising militia: loyalty has fallen under 25.", h.name.c_str()));
            } else if (h.manpower < MILITIA_SQUAD_MEN) {
                // Nobody to train: wait for volunteers (the pool refills daily).
            } else {
                h.training_progress += 60.0f / MILITIA_TRAIN_MIN;
                if (h.training_progress >= 1.0f) {
                    h.training_progress -= 1.0f;
                    h.manpower -= MILITIA_SQUAD_MEN;
                    add_militia_squad(h);
                    push_event(EV_HOLDING, h.militia_unit, false,
                               fmt("%s: a militia squad of %d is ready (%d of %d).", h.name.c_str(), MILITIA_SQUAD_MEN,
                                   militia_squads(h), cap));
                }
            }
        }
    }
}

// The company trains parked at an own holding draw from its stores, up to the want levels,
// as fast as the handling allows (10,000 L, 10 t, 2,000 kg an hour; cash and men at once).
void World::update_trains_at_depot(int hq) {
    if (hq < 0) return;
    const Unit &q = units[hq];
    if (q.moving()) return;
    for (auto &h : holdings) {
        if (h.owner != SIDE_PLAYER || hex_distance(h.pos, q.pos) > std::max(3, h.radius)) continue;
        const double l = std::min({h.diesel_l, std::max(0.0, WANT_DIESEL_L - stock.diesel_l), 10000.0});
        const double t = std::min({h.ammo_t, std::max(0.0, WANT_AMMO_T - stock.ammo_t), 10.0});
        const double k = std::min({h.food_kg, std::max(0.0, WANT_FOOD_KG - stock.food_kg), 2000.0});
        if (l > 0) { h.diesel_l -= l; stock.diesel_l += l; }
        if (t > 0) { h.ammo_t -= t; stock.ammo_t += t; }
        if (k > 0) { h.food_kg -= k; stock.food_kg += k; }
        if (h.cash > 0) { stock.cash += h.cash; h.cash = 0; }
    }
}

double World::days_of_diesel() const {
    return stock.rate_diesel_lph > 1e-6 ? stock.diesel_l / (stock.rate_diesel_lph * 24.0) : 1e9;
}
double World::days_of_ammo() const {
    return stock.rate_ammo_tph > 1e-6 ? stock.ammo_t / (stock.rate_ammo_tph * 24.0) : 1e9;
}
double World::days_of_food() const {
    return stock.personnel > 0 ? stock.food_kg / (stock.personnel * 1.5) : 1e9;
}

// ------------------------------------------------------------------------------------
// Supply routes and convoys (MRK-09)
// ------------------------------------------------------------------------------------

int World::free_convoy(int side) const {
    for (const auto &u : units)
        if (u.alive && u.side == side && u.type == U_TRUCK && u.convoy_route < 0 && u.elements > 0) return u.id;
    return -1;
}

int World::set_supply_route(int source, int dest, std::string *error) {
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return -1;
    };
    if (source < 0 || source >= (int)holdings.size()) return fail("no such holding");
    if (dest >= (int)holdings.size()) return fail("no such destination");
    if (dest == source) return fail("source and destination are the same place");
    const Holding &s = holdings[source];
    if (s.owner != SIDE_PLAYER) return fail("the company does not hold " + s.name);
    if (dest >= 0 && holdings[dest].owner != SIDE_PLAYER) return fail("the company does not hold " + holdings[dest].name);
    for (const auto &r : routes)
        if (r.active && r.side == SIDE_PLAYER && r.source == source && r.dest == dest)
            return fail("that route already exists");
    const int cv = free_convoy(SIDE_PLAYER);
    if (cv < 0) return fail("no supply column is free: every column is working a route, or they are lost");
    Unit &u = units[cv];
    const HexCoord to = dest >= 0 ? holdings[dest].pos : (acting_hq(0) >= 0 ? units[acting_hq(0)].pos : u.pos);
    if (!reachable(u.pos, s.pos) || !reachable(s.pos, to)) return fail("no road or land route between the places");
    int rid = -1;
    for (int i = 0; i < (int)routes.size(); ++i)
        if (!routes[i].active && routes[i].convoy < 0) {
            rid = i;
            break;
        }
    if (rid < 0) {
        rid = (int)routes.size();
        routes.emplace_back();
    }
    SupplyRoute r;
    r.active = true;
    r.side = SIDE_PLAYER;
    r.source = source;
    r.dest = dest;
    r.convoy = cv;
    routes[rid] = r;
    u.convoy_route = rid;
    u.convoy_state = CV_TO_SOURCE;
    u.convoy_wait_until = -1;
    u.pending.active = false;
    u.resting = false;
    detach(cv);
    u.mode = MV_ROAD;
    start_path(u, s.pos, O_MOVE, false);
    push_event(EV_SUPPLY, cv, false,
               fmt("%s takes up the supply route %s -> %s: %d trucks, %.1f t a trip.", u.name.c_str(), s.name.c_str(),
                   dest >= 0 ? holdings[dest].name.c_str() : "the trains", u.elements, convoy_capacity_t(u)));
    return rid;
}

bool World::cancel_supply_route(int rid) {
    if (rid < 0 || rid >= (int)routes.size() || !routes[rid].active) return false;
    SupplyRoute &r = routes[rid];
    r.active = false;
    if (r.convoy >= 0 && units[r.convoy].alive) {
        Unit &u = units[r.convoy];
        // A loaded column still delivers; an empty one comes home.
        if (u.convoy_state == CV_TO_SOURCE || u.convoy_state == CV_LOADING || u.convoy_state == CV_IDLE) {
            u.convoy_state = CV_RETURN;
            u.convoy_wait_until = -1;
            const int hq = acting_hq(0);
            u.mode = MV_ROAD;
            start_path(u, hq >= 0 ? units[hq].pos : u.pos, O_MOVE, false);
        }
        push_event(EV_SUPPLY, r.convoy, false,
                   fmt("Supply route %s -> %s cancelled; %s %s.", holdings[r.source].name.c_str(),
                       r.dest >= 0 ? holdings[r.dest].name.c_str() : "the trains", u.name.c_str(),
                       u.convoy_state == CV_RETURN ? "returns to the trains" : "delivers what it carries first"));
    } else {
        r.convoy = -1;
    }
    return true;
}

// The column is out of trucks (or stranded): the route stops until a column is assigned again.
void World::convoy_suspend(Unit &u, const char *why) {
    if (u.convoy_route >= 0 && u.convoy_route < (int)routes.size()) {
        SupplyRoute &r = routes[u.convoy_route];
        r.active = false;
        r.convoy = -1;
        r.suspended_at = time_min;
        if (u.side == SIDE_PLAYER)
            push_event(EV_SUPPLY, u.id, true,
                       fmt("Supply route %s -> %s suspended: %s %s.", holdings[r.source].name.c_str(),
                           r.dest >= 0 ? holdings[r.dest].name.c_str() : "the trains", u.name.c_str(), why));
    }
    u.convoy_route = -1;
    u.convoy_state = CV_IDLE;
    u.cargo_l = u.cargo_t = u.cargo_kg = 0;
}

// Loads what the destination is short of (against its want levels), proportionally when the
// trucks can't take it all. 1 t = 1,190 L diesel = 1 t ammunition = 1,000 kg food.
void World::load_convoy(Unit &u, SupplyRoute &r) {
    Holding &s = holdings[r.source];
    double need_l, need_t, need_kg;
    if (r.dest < 0) {
        need_l = std::max(0.0, WANT_DIESEL_L - stock.diesel_l);
        need_t = std::max(0.0, WANT_AMMO_T - stock.ammo_t);
        need_kg = std::max(0.0, WANT_FOOD_KG - stock.food_kg);
    } else {
        const Holding &d = holdings[r.dest];
        need_l = std::max(0.0, WANT_DIESEL_L - d.diesel_l);
        need_t = std::max(0.0, WANT_AMMO_T - d.ammo_t);
        need_kg = std::max(0.0, WANT_FOOD_KG - d.food_kg);
    }
    double take_l = std::min(need_l, s.diesel_l), take_t = std::min(need_t, s.ammo_t), take_kg = std::min(need_kg, s.food_kg);
    const double tonnes = take_l / DIESEL_L_PER_T + take_t + take_kg / 1000.0;
    const double cap = convoy_capacity_t(u);
    if (tonnes > cap && tonnes > 0) {
        const double f = cap / tonnes;
        take_l *= f;
        take_t *= f;
        take_kg *= f;
    }
    s.diesel_l -= take_l;
    s.ammo_t -= take_t;
    s.food_kg -= take_kg;
    u.cargo_l = take_l;
    u.cargo_t = take_t;
    u.cargo_kg = take_kg;
    u.trip_started = time_min;
    // The trucks fill their own tanks from the depot's pumps while they load.
    const auto &ti = unit_type_info(u.type);
    const double own = std::min((100.0 - u.fuel) / 100.0 * ti.fuel_litres, s.diesel_l);
    if (own > 0) {
        s.diesel_l -= own;
        u.fuel += (float)(own / ti.fuel_litres * 100.0);
        u.out_of_fuel_reported = false;
    }
    if (u.side == SIDE_PLAYER) {
        if (tonnes <= 0.01)
            push_event(EV_SUPPLY, u.id, false,
                       fmt("%s at %s: nothing to carry (the stores are empty, or the destination wants nothing). Waiting.",
                           u.name.c_str(), s.name.c_str()));
        else
            push_event(EV_SUPPLY, u.id, false,
                       fmt("%s loaded at %s: %.0f L diesel, %.1f t ammunition, %.0f kg food.", u.name.c_str(),
                           s.name.c_str(), take_l, take_t, take_kg));
    }
}

void World::unload_convoy(Unit &u, SupplyRoute &r) {
    std::string where;
    if (r.dest < 0) {
        stock.diesel_l += u.cargo_l;
        stock.ammo_t += u.cargo_t;
        stock.food_kg += u.cargo_kg;
        where = "the trains";
    } else {
        Holding &d = holdings[r.dest];
        d.diesel_l += u.cargo_l;
        d.ammo_t += u.cargo_t;
        d.food_kg += u.cargo_kg;
        where = d.name;
    }
    r.delivered_l += u.cargo_l;
    r.delivered_t += u.cargo_t;
    r.delivered_kg += u.cargo_kg;
    ++r.trips;
    // And the trucks fill up where they unload: at the trains from the company stock, at a
    // holding from its stores.
    {
        const auto &ti = unit_type_info(u.type);
        double &pool = r.dest < 0 ? stock.diesel_l : holdings[r.dest].diesel_l;
        const double own = std::min((100.0 - u.fuel) / 100.0 * ti.fuel_litres, pool);
        if (own > 0) {
            pool -= own;
            if (r.dest < 0) used_diesel_l_ += own;
            u.fuel += (float)(own / ti.fuel_litres * 100.0);
            u.out_of_fuel_reported = false;
        }
    }
    if (u.side == SIDE_PLAYER && (u.cargo_l + u.cargo_t + u.cargo_kg) > 0.01)
        push_event(EV_SUPPLY, u.id, false,
                   fmt("%s delivered %.0f L diesel, %.1f t ammunition, %.0f kg food to %s from %s.", u.name.c_str(),
                       u.cargo_l, u.cargo_t, u.cargo_kg, where.c_str(), holdings[r.source].name.c_str()));
    u.cargo_l = u.cargo_t = u.cargo_kg = 0;
}

// Drive to `target` and count as arrived within `reach` hexes. A column halted short of it (an
// enemy-held hex, a fall-back, a lost route) tries the leg again after CONVOY_RESUME_MIN with no
// enemy known within 5 km.
bool World::convoy_arrive_or_repath(Unit &u, HexCoord target, int reach) {
    if (u.moving() || u.order == O_RETREAT || u.pending.active) return false;
    if (hex_distance(u.pos, target) <= reach) return true;
    // Not there: was it driven off or blocked? Wait, then try again.
    const bool shaken = time_min - u.last_targeted < CONVOY_RESUME_MIN || time_min - u.last_hit < CONVOY_RESUME_MIN;
    if (shaken || nearest_known_enemy(u, 10) >= 0) {
        u.convoy_wait_until = std::max(u.convoy_wait_until, time_min + CONVOY_RESUME_MIN);
        return false;
    }
    if (time_min < u.convoy_wait_until || paths_this_tick_ >= MAX_PATHS_PER_TICK) return false;
    u.mode = MV_ROAD;
    start_path(u, target, O_MOVE, false);
    if (!u.moving()) {
        // No route at all right now (enemy on every road): try again in an hour.
        u.convoy_wait_until = time_min + CONVOY_RESUME_MIN;
    }
    return false;
}

void World::update_convoys() {
    // A route whose column is gone (however it went) stops until a column is assigned again.
    for (auto &r : routes)
        if (r.active && r.convoy >= 0 && r.convoy < (int)units.size() && !units[r.convoy].alive)
            convoy_suspend(units[r.convoy], "was lost");
    for (auto &u : units) {
        if (!u.alive || u.type != U_TRUCK) continue;
        if (u.convoy_route < 0) {
            // With the trains: an idle column keeps station near the HQ (or whoever took over).
            if (u.convoy_state == CV_RETURN) {
                const int hq = acting_hq(0);
                if (hq < 0 || u.moving() || u.order == O_RETREAT) continue;
                if (hex_distance(u.pos, units[hq].pos) <= 3) {
                    u.convoy_state = CV_IDLE;
                } else if (time_min >= u.convoy_wait_until && paths_this_tick_ < MAX_PATHS_PER_TICK) {
                    u.mode = MV_ROAD;
                    start_path(u, regroup_point(u), O_MOVE, false);
                    u.convoy_wait_until = time_min + CONVOY_RESUME_MIN;
                }
            }
            continue;
        }
        SupplyRoute &r = routes[u.convoy_route];
        const int hq = acting_hq(u.side == SIDE_PLAYER ? 0 : u.formation);
        if (r.source < 0 || r.source >= (int)holdings.size()) {
            convoy_suspend(u, "has no source");
            continue;
        }
        // A source or destination lost to the enemy ends the route; the column comes home.
        if (r.active && (holdings[r.source].owner != u.side || (r.dest >= 0 && holdings[r.dest].owner != u.side))) {
            r.active = false;
            if (u.convoy_state == CV_TO_SOURCE || u.convoy_state == CV_LOADING) {
                u.convoy_state = CV_RETURN;
                u.convoy_wait_until = -1;
            }
        }
        switch (u.convoy_state) {
            case CV_IDLE:
                u.convoy_state = r.active ? CV_TO_SOURCE : CV_RETURN;
                u.convoy_wait_until = -1;
                break;
            case CV_TO_SOURCE:
                if (!r.active) {
                    u.convoy_state = CV_RETURN;
                    u.convoy_wait_until = -1;
                    break;
                }
                if (convoy_arrive_or_repath(u, holdings[r.source].pos, holdings[r.source].radius)) {
                    u.convoy_state = CV_LOADING;
                    u.convoy_wait_until = time_min + CONVOY_LOAD_MIN;
                }
                break;
            case CV_LOADING:
                if (time_min < u.convoy_wait_until) break;
                if (!r.active) {
                    u.convoy_state = CV_RETURN;
                    u.convoy_wait_until = -1;
                    break;
                }
                load_convoy(u, r);
                if (u.cargo_l + u.cargo_t + u.cargo_kg <= 0.01) {
                    u.convoy_wait_until = time_min + 60; // nothing to carry yet: look again in an hour
                    break;
                }
                u.convoy_state = CV_TO_DEST;
                u.convoy_wait_until = -1;
                break;
            case CV_TO_DEST: {
                bool there = false;
                if (r.dest >= 0) {
                    there = convoy_arrive_or_repath(u, holdings[r.dest].pos, holdings[r.dest].radius);
                } else {
                    if (hq < 0) break; // nobody runs the trains yet: wait where it is
                    const HexCoord t = units[hq].moving() ? units[hq].dest : units[hq].pos;
                    // The trains moved on: follow them.
                    if (u.moving() && hex_distance(u.dest, t) > 3 && paths_this_tick_ < MAX_PATHS_PER_TICK &&
                        time_min >= u.convoy_wait_until) {
                        u.mode = MV_ROAD;
                        start_path(u, t, O_MOVE, false);
                        u.convoy_wait_until = time_min + 30;
                    }
                    there = convoy_arrive_or_repath(u, units[hq].pos, 3);
                }
                if (there) {
                    u.convoy_state = CV_UNLOADING;
                    u.convoy_wait_until = time_min + CONVOY_LOAD_MIN;
                }
                break;
            }
            case CV_UNLOADING:
                if (time_min < u.convoy_wait_until) break;
                if (r.dest < 0 && (hq < 0 || hex_distance(u.pos, units[hq].pos) > 3)) {
                    // The trains left while we were unloading: catch up.
                    u.convoy_state = CV_TO_DEST;
                    break;
                }
                if (u.trip_started >= 0) {
                    // Round trip = this leg out and back (the return is as long as the way here).
                    r.last_round_trip_min = (int)(2 * (time_min - u.trip_started));
                }
                unload_convoy(u, r);
                u.trip_started = -1;
                if (r.active) {
                    u.convoy_state = CV_TO_SOURCE;
                } else {
                    u.convoy_state = CV_RETURN;
                }
                u.convoy_wait_until = -1;
                break;
            case CV_RETURN: {
                if (hq < 0) break;
                if (convoy_arrive_or_repath(u, units[hq].pos, 3)) {
                    u.convoy_state = CV_IDLE;
                    if (r.convoy == u.id) r.convoy = -1;
                    u.convoy_route = -1;
                    if (u.side == SIDE_PLAYER)
                        push_event(EV_SUPPLY, u.id, false, fmt("%s is back with the trains.", u.name.c_str()));
                }
                break;
            }
            default:
                u.convoy_state = CV_IDLE;
                break;
        }
    }
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
    static const int kDefault[U_TYPE_COUNT] = {2, 3, 3, 3, 2, 6, 6}; // HQ, tank, mech, infantry, recon, artillery (6-gun battery), trucks
    return kDefault[u.type < U_TYPE_COUNT ? u.type : 0];
}

EngagementTime World::engagement_time(const Unit &u) const {
    const auto &ti = unit_type_info(u.type);
    EngagementTime e;
    e.base_cycle = (float)ti.fire_interval;
    // Direct fire picks up the target (first round 8-24 s for trained crews: inside one tick).
    // A fire mission on an unplanned target: a gun platoon (up to 3 guns) 60-90 s, a battery 3-5 min.
    e.base_acquire = !ti.indirect ? 1.0f : u.elements_max <= 3 ? 1.25f : 4.0f;
    e.f_readiness = 1.0f + (100.0f - u.readiness) / 100.0f;
    e.f_morale = 1.0f + std::max(0.0f, 60.0f - u.morale) / 60.0f;
    e.f_ammo = u.ammo >= 25.0f ? 1.0f : 1.0f + (25.0f - u.ammo) / 25.0f;
    e.f_supp = 1.0f + u.suppression / 50.0f;
    e.f_night = is_night() ? 1.2f : 1.0f;
    e.factor = e.f_readiness * e.f_morale * e.f_ammo * e.f_supp * e.f_night;
    e.acquire = e.base_acquire * e.factor;
    e.cycle = e.base_cycle * e.factor;
    return e;
}

// Troops each APC or IFV carries besides its crew (Jane's, FM 100-2-3). Squads per platoon are
// one per vehicle; the number of men only changes the ration count and the unit panel.
static const CarrierInfo kCarriers[] = {
    {"BMP-1", 8}, {"BMP-2", 7}, {"BMP-3", 7}, {"BTR-50", 20}, {"BTR-50PK", 20}, {"BTR-60", 8}, {"BTR-60PB", 8},
    {"BTR-70", 7}, {"BTR-80", 7}, {"MT-LB", 11}, {"OT-64", 15}, {"OT-62", 18},
    {"M113", 11}, {"M113A1", 11}, {"M113A2", 11}, {"M113A3", 11}, {"M2", 6}, {"M2A1", 6}, {"Bradley", 6},
    {"Marder", 6}, {"AMX-10P", 8}, {"VAB", 10}, {"Warrior", 7}, {"FV432", 10}, {"Fuchs", 10}, {"YPR-765", 7},
};

const CarrierInfo *carrier_info(const std::string &e) {
    auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    for (const CarrierInfo &k : kCarriers) {
        const size_t n = std::strlen(k.key);
        for (size_t at = e.find(k.key); at != std::string::npos; at = e.find(k.key, at + 1))
            if ((at == 0 || !alnum(e[at - 1])) && (at + n == e.size() || !alnum(e[at + n]))) return &k;
    }
    return nullptr;
}

int World::weapon_range(const Unit &u) {
    const UnitTypeInfo &ti = unit_type_info(u.type);
    if (!ti.indirect) return ti.weapon_range;
    const GunInfo *g = gun_info(u.equipment);
    return g ? (int)(g->range_km * 2.0f) : ti.weapon_range; // whole hexes inside the range
}

float World::suppression_weight(const Unit &s) {
    // Per element and fire cycle against soft targets in the open, in 155 mm-round equivalents:
    // a tank's 2 rounds (0.8 each); an IFV's cannon bursts and MG; a squad's rifles and MG
    // (about 20 five-round bursts a cycle at 0.06); a recon vehicle's MG; an HQ's escort.
    switch (s.type) {
        case U_TANK: return 1.6f;
        case U_MECH: return 0.6f;
        case U_INFANTRY: return 1.0f;
        case U_RECON: return 0.5f;
        case U_HQ: return 0.2f;
        case U_TRUCK: return 0.1f; // drivers' rifles
        default: break;
    }
    // Guns: by calibre against 155 mm; rocket launchers fire a ripple of rockets each.
    const GunInfo *g = gun_info(s.equipment);
    if (!g) return 0.8f;
    const float mm = (float)std::atof(g->label);
    float w = mm > 0 ? mm / 155.0f : 0.8f;
    if (std::strstr(g->label, "MRL")) w *= 4.0f;
    return w;
}

void World::suppress(Unit &u, float S, int64_t hold_until) {
    if (S <= 0) return;
    u.suppression += (100.0f - u.suppression) * (1.0f - std::exp(-S / SUPP_S0));
    u.suppression = std::min(100.0f, u.suppression);
    u.supp_hold_until = std::max(u.supp_hold_until, hold_until);
}

// Suppression fades once the fire stops; soft units pin at 60 and get up again below 35.
void World::update_suppression() {
    for (auto &u : units) {
        if (!u.alive) continue;
        if (u.suppression > 0 && time_min >= u.supp_hold_until) {
            u.suppression *= SUPP_DECAY;
            if (u.suppression < 1.0f) u.suppression = 0;
        }
        const bool armored = unit_type_info(u.type).armored;
        if (!u.pinned && !armored && u.suppression >= PIN_AT) {
            u.pinned = true;
            if (u.side == SIDE_PLAYER)
                push_event(EV_UNDER_FIRE, u.id, false, fmt("%s is pinned down.", u.name.c_str()));
        } else if (u.pinned && (armored || u.suppression < UNPIN_AT)) {
            u.pinned = false;
        }
    }
}

bool World::observes(const Unit &o, const Unit &t) const {
    if (!o.alive || o.zone == Z_COLD) return false;
    const int d = hex_distance(o.pos, t.pos);
    return d <= (int)effective_spot_range(o) && line_of_sight(o.pos, t.pos, 3.0f, 2.5f);
}

FireNetLink World::fire_net_link(const Unit &u) const {
    FireNetLink l;
    if (!u.alive || u.formation < 0 || u.formation >= (int)formations.size()) return l;
    const auto &members = formations[u.formation].members;
    // Nearest guns of the formation, and whether m reaches them on the fire net.
    auto guns_for = [&](const Unit &m, int &bat, int &dist) {
        bat = -1;
        dist = 1 << 30;
        bool reach = false;
        for (int id : members) {
            const Unit &b = units[id];
            if (!b.alive || b.type != U_ARTILLERY) continue;
            const int d = hex_distance(m.pos, b.pos);
            const bool r = d <= net_range(m, b);
            if ((r && !reach) || (r == reach && d < dist)) {
                bat = id;
                dist = d;
                reach = r;
            }
        }
        return reach;
    };
    int bat, dist;
    const bool reach = guns_for(u, bat, dist);
    if (bat < 0) return l; // no guns: no fire net
    if (u.type == U_ARTILLERY) {
        l.role = FN_GUNS;
        l.status = FN_DIRECT;
        l.battery = u.id;
        return l;
    }
    l.role = on_fire_net(u) ? FN_OBSERVER : FN_CALLER;
    l.battery = bat;
    l.distance = dist;
    if (l.role == FN_OBSERVER && reach) {
        l.status = FN_DIRECT;
        return l;
    }
    // Relay: the nearest unit on the net within radio range that reaches the guns itself.
    int best = 1 << 30;
    for (int id : members) {
        const Unit &m = units[id];
        if (id == u.id || !m.alive || !on_fire_net(m)) continue;
        const int d = hex_distance(u.pos, m.pos);
        if (d > net_range(u, m) || d >= best) continue;
        int mb = id, md = 0;
        if (m.type != U_ARTILLERY && !guns_for(m, mb, md)) continue;
        best = d;
        l.status = FN_RELAY;
        l.via = id;
        l.battery = mb;
        l.distance = d;
    }
    return l;
}

int World::fire_call(const Unit &b, const Unit &t, int *observer) const {
    if (b.formation < 0 || b.formation >= (int)formations.size()) return -1;
    const auto &members = formations[b.formation].members;
    auto reaches_guns = [&](const Unit &m) {
        return m.id == b.id || (on_fire_net(m) && hex_distance(m.pos, b.pos) <= net_range(m, b));
    };
    int best = -1;
    for (int id : members) {
        const Unit &o = units[id];
        if (!observes(o, t)) continue;
        if (reaches_guns(o)) {
            if (observer) *observer = id;
            return 0;
        }
        if (best >= 0) continue;
        for (int mid : members) {
            const Unit &m = units[mid];
            if (mid == id || !m.alive || !on_fire_net(m)) continue;
            if (hex_distance(o.pos, m.pos) <= net_range(o, m) && reaches_guns(m)) {
                best = 1;
                if (observer) *observer = id;
                break;
            }
        }
    }
    return best;
}

int World::radio_range(const Unit &u) {
    return unit_type_info(u.type).mobility == M_FOOT ? RADIO_RANGE_FOOT : RADIO_RANGE;
}

int World::nearest_known_enemy(const Unit &u, int radius, int *dist) const {
    int best = -1, bd = radius + 1;
    // Last known positions can lag up to 30 minutes behind; search a little wider for them.
    for_units_near(u.pos, radius + 16, [&](const Unit &o) {
        if (!o.alive || o.side == u.side) return;
        HexCoord at;
        if (seen_by(o, u.side)) at = o.pos;
        else if (time_min - o.last_seen[u.side] <= 30) at = o.last_seen_pos[u.side];
        else return;
        int d = hex_distance(u.pos, at);
        if (d < bd) {
            bd = d;
            best = o.id;
        }
    });
    if (best >= 0 && dist) *dist = bd;
    return best;
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
    // The senior platoon still able to fight: first in the order of battle. A town's militia
    // never leaves its town, so it never runs the trains.
    for (int id : m) {
        const Unit &u = units[id];
        if (u.alive && !u.militia && (u.type == U_TANK || u.type == U_MECH || u.type == U_INFANTRY) && u.strength >= 30.0f)
            return id;
    }
    for (int id : m)
        if (units[id].alive && !units[id].militia && units[id].type != U_TRUCK) return id;
    for (int id : m)
        if (units[id].alive && !units[id].militia) return id;
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
    if (map.at_index(to_idx).terrain == T_WATER) return -1;
    return step_minutes(unit_type_info(u.type).mobility, u.mode, from_idx, to_idx, road_step(from_idx, to_idx), planning);
}

float World::step_minutes(int mob, int move_mode, int from_idx, int to_idx, bool road, bool planning) const {
    const Hex &a = map.at_index(from_idx), &b = map.at_index(to_idx);
    if (b.terrain == T_WATER) return -1;
    float c = road ? kRoadCost[mob] : kTerrainCost[mob][b.terrain];
    if (road && (a.features & F_MAJOR_ROAD) && (b.features & F_MAJOR_ROAD)) c *= 0.8f;
    if (!road && (b.features & F_RIVER)) c += kRiverPenalty[mob];
    int dz = b.elev - a.elev;
    c += (dz > 0 ? dz : 0) * (mob == M_FOOT ? 0.04f : 0.03f) + std::abs(dz) * 0.004f;
    switch (move_mode) {
        case MV_ROAD: c *= road ? 0.85f : (planning ? 1.8f : 1.0f); break;
        case MV_DELIBERATE: c *= 1.5f; break;
        case MV_ASSAULT: c *= 1.15f; break;
        default: break;
    }
    return c;
}

bool World::line_of_sight(HexCoord a, HexCoord b, float eye, float target) const {
    ++los_checks_;
    return sight_block(a, b, eye, target) == -1;
}

int World::sight_block(HexCoord a, HexCoord b, float eye, float target) const {
    thread_local std::vector<HexCoord> line;
    hex_line(a, b, line);
    const int n = (int)line.size() - 1;
    if (n <= 1) return -1;
    auto ground = [&](HexCoord h) { return (float)std::max<int>(0, map.at(h).elev); };
    float ea = ground(a) + eye, eb = ground(b) + target;
    for (int i = 1; i < n; ++i) {
        if (!map.in_bounds(line[i])) return -2;
        const Hex &h = map.at(line[i]);
        float obstacle = std::max<int>(0, h.elev) + h.cover;
        float ray = ea + (eb - ea) * ((float)i / n);
        if (obstacle > ray) return i;
    }
    return -1;
}

RouteEstimate World::route_estimate(HexCoord a, HexCoord b, int mobility, int move_mode) {
    RouteEstimate r;
    if (!map.in_bounds(a) || !map.in_bounds(b) || mobility < 0 || mobility > M_FOOT) return r;
    if (a == b) {
        r.found = true;
        return r;
    }
    if (map.at(b).terrain == T_WATER || !reachable(a, b)) return r;
    // As start_path: the same minimum step for the heuristic, the same weight and limit.
    float min_step = kRoadCost[mobility] * 0.8f * (move_mode == MV_ROAD ? 0.85f : 1.0f);
    PathResult pr = pathfinder().find(
        a, b, [&](int f, int t) { return step_minutes(mobility, move_mode, f, t, road_step(f, t), true); }, min_step,
        r.path, 400000, 1.3f);
    if (!pr.found || r.path.empty()) {
        r.path.clear();
        return r;
    }
    r.found = true;
    int from = map.index(a);
    for (int idx : r.path) {
        const bool road = road_step(from, idx);
        r.minutes += step_minutes(mobility, move_mode, from, idx, road); // driven, not planned, cost
        Vec2 p = hex_center(map.coord_of(from)), q = hex_center(map.coord_of(idx));
        const float km = (float)(std::hypot(q.x - p.x, q.y - p.y) / 1000.0);
        r.km += km;
        if (road) r.road_km += km;
        from = idx;
    }
    return r;
}

const char *mark_kind_name(int kind) {
    static const char *n[MK_COUNT] = {"Objective", "Phase line", "Target reference point", "Note"};
    return kind >= 0 && kind < MK_COUNT ? n[kind] : "?";
}

static bool marking_ok(const Map &map, const Marking &m) {
    if (m.kind >= MK_COUNT || m.name.size() > (size_t)MAX_MARK_NAME || m.xy.size() % 2 != 0) return false;
    const int pts = (int)m.xy.size() / 2;
    if (pts < 1 || pts > MAX_MARK_POINTS || (m.kind == MK_PHASE_LINE ? pts < 2 : pts != 1)) return false;
    for (size_t i = 0; i < m.xy.size(); i += 2)
        if (m.xy[i] < 0 || m.xy[i + 1] < 0 || m.xy[i] > map.width_m() || m.xy[i + 1] > map.height_m()) return false;
    return true;
}

int World::add_marking(const Marking &m) {
    if ((int)markings.size() >= MAX_MARKINGS || !marking_ok(map, m)) return -1;
    markings.push_back(m);
    return (int)markings.size() - 1;
}

bool World::rename_marking(int i, const std::string &name) {
    if (i < 0 || i >= (int)markings.size() || name.size() > (size_t)MAX_MARK_NAME) return false;
    markings[i].name = name;
    return true;
}

bool World::remove_marking(int i) {
    if (i < 0 || i >= (int)markings.size()) return false;
    markings.erase(markings.begin() + i);
    return true;
}

float World::effective_spot_range(const Unit &u) const {
    float r = (float)unit_type_info(u.type).spot_range;
    if (is_night()) r = std::max(2.0f, r * 0.35f);
    if (u.pinned) r = std::max(1.0f, r * 0.5f); // heads down
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
            if (dist > radio_range(u)) d += (dist * 0.5f) / 40.0f * 60.0f; // courier at 40 km/h
            else if (dist > command_zone(units[hq])) d *= 1.5f;     // outside the zone of command: relayed
        }
    }
    if (u.zone == Z_HOT && (time_min - u.last_fired < 10 || time_min - u.last_hit < 10)) d *= 1.5f;
    d *= 1.0f + (100.0f - u.readiness) / 200.0f;
    return (int)std::ceil(d);
}

bool World::same_kind_order(const Unit &u, OrderKind kind, MoveMode mode) const {
    if (kind == O_MOVE) {
        // A unit on a route is carrying out its move order even while it halts at a waypoint or
        // waits for an enemy-held hex or its stand-off line to clear. Assault is its own kind.
        MoveMode cur;
        if (u.on_route()) cur = u.route[u.wp_i].mode;
        else if (u.moving() && u.order == O_MOVE) cur = u.mode;
        else return false;
        return (cur == MV_ASSAULT) == (mode == MV_ASSAULT);
    }
    return (kind == O_RETREAT || kind == O_REGROUP || kind == O_REST) && u.order == kind && u.moving();
}

int World::order_delay(const Unit &u, OrderKind kind, MoveMode mode) const {
    if (same_kind_order(u, kind, mode)) return SAME_ORDER_DELAY;
    int d = command_delay(u);
    const bool needs_move = kind == O_MOVE || kind == O_RETREAT || kind == O_REGROUP || kind == O_REST;
    if (needs_move && !u.moving()) d += START_MOVING_DELAY;
    return d;
}

namespace {
// Metres from p to the segment a-b.
double seg_dist_m(Vec2 p, Vec2 a, Vec2 b) {
    const double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
    double t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}
} // namespace

bool World::is_adjustment(const Unit &u, const std::vector<Waypoint> &route) const {
    if (route.empty() || !u.alive || u.mounting || u.order == O_RETREAT) return false;
    // The route being followed now: from where the unit is, through the waypoints ahead. A unit
    // without one has only its position (adjust position). Regrouping or resting moves are
    // standing procedures, not routes: anything else for them is a new order.
    std::vector<Waypoint> old;
    if (u.on_route()) old.assign(u.route.begin() + u.wp_i, u.route.end());
    else if (u.moving()) return false;
    std::vector<Vec2> line{hex_center(u.pos)};
    for (const auto &w : old) line.push_back(hex_center(w.at));
    const HexCoord old_final = old.empty() ? u.pos : old.back().at;
    auto old_wp = [&](HexCoord h) -> const Waypoint * {
        for (const auto &w : old)
            if (w.at == h) return &w;
        return nullptr;
    };
    for (const auto &w : route) {
        const Waypoint *o = old_wp(w.at);
        if (w.mode == MV_ASSAULT && !(o && o->mode == MV_ASSAULT)) return false; // a change to Assault
        if (o) continue;                                                         // unchanged waypoint
        const Vec2 p = hex_center(w.at);
        double best = 1e18;
        if (line.size() == 1) best = std::hypot(p.x - line[0].x, p.y - line[0].y);
        for (size_t i = 1; i < line.size(); ++i) best = std::min(best, seg_dist_m(p, line[i - 1], line[i]));
        if (best > ADJUST_RADIUS_M) return false;
    }
    const Vec2 a = hex_center(old_final), b = hex_center(route.back().at);
    return std::hypot(a.x - b.x, a.y - b.y) <= ADJUST_RADIUS_M;
}

static bool valid_route(const Map &map, const std::vector<Waypoint> &route) {
    if (route.empty() || (int)route.size() > MAX_WAYPOINTS) return false;
    for (const auto &w : route) {
        if (!map.in_bounds(w.at) || map.at(w.at).terrain == T_WATER) return false;
        if (w.mode > MV_ASSAULT || w.halt_min < 0 || w.halt_min > MAX_WAYPOINT_HALT) return false;
        if (w.shape < -1 || w.shape >= FS_COUNT) return false;
    }
    return true;
}

int World::route_delay(int unit_id, const std::vector<Waypoint> &route, bool *adjust) const {
    if (adjust) *adjust = false;
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive || !orderable(units[unit_id])) return -1;
    if (!valid_route(map, route)) return -1;
    const Unit &u = units[unit_id];
    const bool adj = is_adjustment(u, route);
    if (adjust) *adjust = adj;
    // Move to Move: a unit already on its move order takes any change in 2 minutes.
    if (same_kind_order(u, O_MOVE, route.front().mode)) return SAME_ORDER_DELAY;
    const int start = u.moving() ? 0 : START_MOVING_DELAY;
    const int base = command_delay(u);
    if (!adj) return base + start;
    const int streak = time_min - u.last_adjust_at <= ADJUST_WINDOW ? u.adjust_streak + 1 : 1;
    const float f = streak == 1 ? ADJUST_FACTOR : streak == 2 ? ADJUST_FACTOR_2 : 1.0f;
    return std::max(ADJUST_MIN_DELAY, (int)std::ceil(base * f)) + start;
}

int World::order_route(int unit_id, const std::vector<Waypoint> &route, bool *adjust) {
    bool adj = false;
    const int delay = route_delay(unit_id, route, &adj);
    if (adjust) *adjust = adj;
    if (delay < 0) return -1;
    Unit &u = units[unit_id];
    if (same_kind_order(u, O_MOVE, route.front().mode)) {
        // Move to Move costs 2 minutes whatever the change, so it leaves the edit streak alone.
    } else if (adj) {
        u.adjust_streak = time_min - u.last_adjust_at <= ADJUST_WINDOW ? u.adjust_streak + 1 : 1;
        u.last_adjust_at = time_min;
    } else {
        u.adjust_streak = 0;
    }
    u.pending = {true, O_MOVE, route.back().at, time_min, time_min + delay, route.front().mode};
    u.pending.route = route;
    u.pending.adjust = adj;
    return delay;
}

int World::order_move(int unit_id, HexCoord target, MoveMode mode) {
    Waypoint w;
    w.at = target;
    w.mode = mode;
    return order_route(unit_id, {w});
}

int World::order_fall_back(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive || !orderable(units[unit_id])) return -1;
    Unit &u = units[unit_id];
    int delay = order_delay(u, O_RETREAT);
    u.pending = {true, O_RETREAT, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

int World::order_regroup(int unit_id, bool rest) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive || !orderable(units[unit_id])) return -1;
    Unit &u = units[unit_id];
    int delay = order_delay(u, rest ? O_REST : O_REGROUP);
    u.pending = {true, rest ? O_REST : O_REGROUP, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

bool World::can_dismount(const Unit &u) const {
    if (!u.alive || u.dismounts < 0) return false;
    const Unit &d = units[u.dismounts];
    return d.embarked && d.elements > 0;
}

bool World::can_mount(const Unit &u) const {
    if (!u.alive) return false;
    const Unit *c = nullptr, *d = nullptr;
    if (u.dismounts >= 0) {
        c = &u;
        d = &units[u.dismounts];
    } else if (u.carrier >= 0) {
        c = &units[u.carrier];
        d = &u;
    } else {
        return false;
    }
    // Squads can crowd into the vehicles left, up to two squads a vehicle.
    return c->alive && d->alive && !d->embarked && d->elements <= 2 * c->elements;
}

int World::order_dismount(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !can_dismount(units[unit_id])) return -1;
    Unit &u = units[unit_id];
    int delay = command_delay(u);
    u.pending = {true, O_DISMOUNT, u.pos, time_min, time_min + delay, u.mode};
    return delay;
}

int World::order_mount(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !can_mount(units[unit_id])) return -1;
    Unit &u = units[unit_id];
    int delay = command_delay(u);
    u.pending = {true, O_MOUNT, u.pos, time_min, time_min + delay, MV_HASTY};
    return delay;
}

// Each APC/IFV platoon gets its squad unit, embarked. Called once the scenario is spawned, so
// every other unit keeps its id.
void World::add_dismounts(int cid) {
    const Unit c = units[cid];
    const CarrierInfo *ci = carrier_info(c.equipment);
    Unit d;
    d.id = (int)units.size();
    d.side = c.side;
    d.type = U_INFANTRY;
    d.name = c.name + " Dismounts";
    d.squad_men = ci ? ci->men : 8;
    d.equipment = fmt("%d squads of %d men (%s)", c.elements_max, d.squad_men, ci ? ci->key : "APC infantry");
    d.formation = c.formation;
    d.slot = c.slot;
    d.pos = d.prev_pos = d.dest = c.pos;
    d.last_seen_pos[0] = d.last_seen_pos[1] = c.pos;
    d.morale = c.morale;
    d.elements_max = d.elements = c.elements_max;
    d.ai_next = c.ai_next;
    d.carrier = cid;
    d.embarked = true;
    d.alive = false;
    formations[c.formation].members.push_back(d.id);
    units.push_back(d);
    units[cid].dismounts = d.id;
}

void World::bucket_insert(const Unit &u) {
    if (buckets_.empty()) return;
    auto &b = buckets_[(size_t)(u.pos.row / BUCKET) * bucket_cols_ + u.pos.col / BUCKET];
    if (std::find(b.begin(), b.end(), u.id) == b.end()) b.push_back(u.id);
}

// Drops the unit's route: a new order, falling back, a drill or a team move replaces it.
static void clear_route(Unit &u) {
    u.route.clear();
    u.wp_i = 0;
    u.wp_halt_until = -1;
    u.leg_due = false;
    u.route_pause = RP_NONE;
}

static void halt(Unit &u) {
    clear_route(u);
    u.path.clear();
    u.path_i = 0;
    u.progress = 0;
    u.order = O_HOLD;
    u.dest = u.pos;
}

// The vehicles halt and the squads get out into the same hex; they can fight after the drill.
void World::dismount_now(Unit &c) {
    if (!can_dismount(c)) return;
    halt(c);
    Unit &d = units[c.dismounts];
    d.alive = true;
    d.embarked = false;
    d.mounting = false;
    d.mount_at = -1;
    d.pos = d.prev_pos = d.dest = c.pos;
    halt(d);
    d.pending.active = false;
    d.pending_stance.active = false;
    d.mode = MV_HASTY;
    d.roe = c.roe;
    d.brk = c.brk;
    d.standoff = c.standoff;
    d.resting = false;
    d.group = -1;
    d.follow = false;
    d.zone = c.zone;
    d.hot_until = c.hot_until;
    d.morale = c.morale; // they rode through the same fight
    d.readiness = c.readiness;
    d.strength = 100.0f * d.elements / d.elements_max;
    d.acquired = -1;
    d.fire_cooldown = DISMOUNT_MIN;
    d.last_hit = -1000;
    d.last_targeted = c.last_targeted;
    for (int s = 0; s < SIDE_COUNT; ++s) {
        d.seen_until[s] = c.seen_until[s];
        d.last_seen[s] = c.last_seen[s];
        d.last_seen_pos[s] = c.last_seen_pos[s];
    }
    bucket_insert(d);
    if (c.side == SIDE_PLAYER)
        push_event(EV_ORDER, d.id, false,
                   fmt("%s dismount %s: %d squad%s on foot.", c.name.c_str(), place_name(c.pos).c_str(), d.elements,
                       d.elements == 1 ? "" : "s"));
}

// The vehicles halt and wait; the squads walk to them (see update_mounting).
void World::start_mount(Unit &u) {
    if (!can_mount(u)) {
        if (u.side == SIDE_PLAYER)
            push_event(EV_INFO, u.id, false, fmt("%s can't mount up: not enough vehicles left.", u.name.c_str()));
        return;
    }
    Unit &c = u.dismounts >= 0 ? u : units[u.carrier];
    Unit &d = u.dismounts >= 0 ? units[u.dismounts] : u;
    detach(c.id);
    halt(c);
    d.mounting = true;
    d.mount_at = -1;
    if (d.pos != c.pos) {
        d.mode = MV_HASTY;
        start_path(d, c.pos, O_MOVE, false);
        if (!d.moving()) {
            d.mounting = false;
            if (d.side == SIDE_PLAYER)
                push_event(EV_INFO, d.id, false, fmt("%s: no route back to %s.", d.name.c_str(), c.name.c_str()));
            return;
        }
    }
    if (u.side == SIDE_PLAYER)
        push_event(EV_ORDER, d.id, false,
                   fmt("%s mounting up: %s halts and waits, %.1f km away.", d.name.c_str(), c.name.c_str(),
                       hex_distance(d.pos, c.pos) * 0.5));
}

void World::update_mounting() {
    for (auto &d : units) {
        if (!d.alive || !d.mounting) continue;
        Unit &c = units[d.carrier];
        if (!c.alive) {
            d.mounting = false;
            d.mount_at = -1;
            if (d.side == SIDE_PLAYER)
                push_event(EV_INFO, d.id, false, fmt("%s: the vehicles are gone, staying on foot.", d.name.c_str()));
            continue;
        }
        if (d.pos == c.pos) {
            if (d.moving()) halt(d);
            if (c.moving()) {
                d.mount_at = -1; // the vehicles drove off: wait for them to stop
                continue;
            }
            if (d.mount_at < 0) {
                d.mount_at = time_min + MOUNT_MIN;
            } else if (time_min >= d.mount_at) {
                if (d.elements > 2 * c.elements) { // vehicles lost on the way back
                    d.mounting = false;
                    d.mount_at = -1;
                    continue;
                }
                d.alive = false;
                d.embarked = true;
                d.mounting = false;
                d.mount_at = -1;
                d.pending.active = false;
                d.pending_stance.active = false;
                d.acquired = -1;
                halt(d);
                if (d.side == SIDE_PLAYER)
                    push_event(EV_ORDER, c.id, false,
                               fmt("%s mounted up: %d squad%s aboard.", c.name.c_str(), d.elements,
                                   d.elements == 1 ? "" : "s"));
            }
            continue;
        }
        d.mount_at = -1;
        // Follow the vehicles if they moved.
        if ((!d.moving() || hex_distance(d.dest, c.pos) >= 2) && paths_this_tick_ < MAX_PATHS_PER_TICK) {
            start_path(d, c.pos, O_MOVE, false);
            if (!d.moving() && d.pos != c.pos) {
                d.mounting = false;
                if (d.side == SIDE_PLAYER)
                    push_event(EV_INFO, d.id, false, fmt("%s: no route back to %s.", d.name.c_str(), c.name.c_str()));
            }
        }
    }
}

int World::set_stance(int unit_id, int roe, int brk) {
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive || units[unit_id].militia) return -1;
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
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive || units[unit_id].militia) return -1;
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
    if (v == STANDOFF_WEAPONS) return weapon_range(u);
    return v;
}

// Stand-off (a standing order, so no command delay): a unit set to keep its distance halts
// before it closes inside that range of a known enemy, and pulls back out to it when an enemy
// comes closer. An assault ignores it; team members go by their leader's.
void World::update_standoff() {
    for (auto &u : units) {
        if (!u.alive || u.order == O_RETREAT || u.mode == MV_ASSAULT || u.mounting) continue;
        if (u.follow && u.group >= 0) continue;
        const int S = standoff_hexes(u);
        if (S <= 0) continue;
        // Nearest enemy this side knows about (seen now, or seen in the last 10 minutes).
        HexCoord bpos;
        int bd = 0;
        const int best = standoff_threat(u, S, &bpos, &bd);
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
                pause_route(u, RP_STANDOFF, bpos); // picks the route up once it is clear
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
    if (!u.alive || !orderable(u)) return -1;
    int delay = command_delay(u);
    u.pending = {true, O_HOLD, u.pos, time_min, time_min + delay};
    return delay;
}

// A team member that left the formation on its own orders is told to take its place again.
// The order travels like any other; once it arrives the member keeps station on the leader.
int World::order_rejoin(int unit_id) {
    if (unit_id < 0 || unit_id >= (int)units.size()) return -1;
    Unit &u = units[unit_id];
    if (!u.alive || !orderable(u)) return -1;
    if (u.group < 0 || u.group >= (int)groups.size() || !groups[u.group].alive || groups[u.group].leader == unit_id) return -1;
    if (u.follow && !u.pending.active) return -1; // already in formation
    int delay = command_delay(u);
    u.pending = {true, O_REJOIN, u.pos, time_min, time_min + delay};
    return delay;
}

// Minutes for the rest of the unit's present leg.
static float leg_cost(const World &w, const Unit &u) {
    float cost = 0;
    int from = w.map.index(u.pos);
    for (size_t i = u.path_i; i < u.path.size(); ++i) {
        cost += std::max(0.0f, w.move_cost(u, from, u.path[i]));
        from = u.path[i];
    }
    return cost;
}

float World::plan_leg(int unit_id, HexCoord from, HexCoord to, MoveMode mode, std::vector<int> &path) {
    path.clear();
    if (unit_id < 0 || unit_id >= (int)units.size() || !units[unit_id].alive) return -1;
    if (!map.in_bounds(from) || !map.in_bounds(to) || map.at(to).terrain == T_WATER) return -1;
    if (from == to) return 0;
    // A scratch copy of the unit at `from`: start_path's rules, without touching the real one
    // or the path budget.
    Unit t = units[unit_id];
    t.pos = from;
    t.mode = mode;
    clear_route(t);
    const int saved = paths_this_tick_;
    start_path(t, to, O_MOVE, false);
    paths_this_tick_ = saved;
    if (!t.moving()) return -1;
    path = t.path;
    return leg_cost(*this, t);
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
    PathResult pr = pathfinder().find(
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
        OrderKind kind = u.pending.kind;
        // A team member's own order detaches it from the formation when the order arrives, not
        // when it is given, so it keeps station until then (MRK-22). Rejoin puts it back.
        const bool member = u.group >= 0 && u.group < (int)groups.size() && groups[u.group].alive &&
                            groups[u.group].leader != u.id;
        if (kind == O_REJOIN) {
            if (member) {
                u.follow = true;
                u.formation_wait = false;
                clear_route(u); // its own route ends: the team moves it again
                groups[u.group].has_dest = false; // take up the slot again
                if (u.side == SIDE_PLAYER)
                    push_event(EV_ORDER, u.id, false, fmt("%s rejoins %s.", u.name.c_str(), groups[u.group].name.c_str()));
            }
            continue;
        }
        if (member) detach(u.id);
        if (kind == O_DISMOUNT) {
            dismount_now(u);
            continue;
        }
        u.mounting = false; // any new order for squads walking back cancels it (O_MOUNT sets it again)
        u.mount_at = -1;
        if (kind == O_MOUNT) {
            start_mount(u);
            continue;
        }
        u.mode = u.pending.mode;
        u.resting = false;
        if (kind == O_MOVE) {
            // A route: the unit drops whatever it was doing and starts the first leg. An
            // adjustment re-plans from where the unit is, keeping its progress into the next hex.
            const bool adjusted = u.pending.adjust;
            const bool was_moving = u.moving();
            const int old_next = was_moving ? u.path[u.path_i] : -1;
            const float old_progress = u.progress;
            std::vector<Waypoint> route = std::move(u.pending.route);
            if (route.empty()) {
                Waypoint w;
                w.at = u.pending.target;
                w.mode = u.pending.mode;
                route.push_back(w);
            }
            u.pending.route.clear();
            clear_route(u);
            u.route = std::move(route);
            start_leg(u, u.side == SIDE_PLAYER);
            if (adjusted && was_moving && u.moving() && u.path[u.path_i] == old_next) u.progress = old_progress;
            if (adjusted && u.side == SIDE_PLAYER && u.on_route())
                push_event(EV_ORDER, u.id, false, fmt("%s adjusts its route.", u.name.c_str()));
            continue;
        }
        clear_route(u);
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
// Routes (MRK-20)
// ------------------------------------------------------------------------------------

void World::start_leg(Unit &u, bool announce) {
    u.leg_due = false;
    u.wp_halt_until = -1;
    if (!u.on_route()) return;
    const Waypoint &w = u.route[u.wp_i];
    u.mode = w.mode;
    // A team leader sets the team's formation for this leg (column on the road, line on the approach).
    if (w.shape >= 0 && u.group >= 0 && u.group < (int)groups.size() && groups[u.group].alive &&
        groups[u.group].leader == u.id && groups[u.group].shape != w.shape)
        set_group_shape(u.group, w.shape, -1);
    const bool single = u.route.size() == 1;
    start_path(u, w.at, O_MOVE, announce && single); // a plain move reports as before
    if (u.pos == w.at) return;                       // already there: update_routes moves on
    if (!u.moving()) {
        if (u.side == SIDE_PLAYER && !single)
            push_event(EV_INFO, u.id, false,
                       fmt("%s: no route to waypoint %d. It halts.", u.name.c_str(), u.wp_i + 1));
        clear_route(u);
        return;
    }
    if (announce && !single) {
        const int mins = (int)leg_cost(*this, u);
        push_event(EV_ORDER, u.id, false,
                   fmt("%s moving: waypoint %d of %d, %.0f km, about %dh%02d.", u.name.c_str(), u.wp_i + 1,
                       (int)u.route.size(), u.path.size() * 0.5, mins / 60, mins % 60));
    }
}

void World::pause_route(Unit &u, uint8_t why, HexCoord blocked) {
    if (!u.on_route()) return;
    u.route_pause = why;
    u.blocked_hex = blocked;
    u.leg_due = false;
    u.route_check = time_min + 1;
}

// The nearest enemy this unit's side knows about (seen now or in the last 10 minutes) within
// its stand-off plus 8 hexes; -1 none. The stand-off rules work from it.
int World::standoff_threat(const Unit &u, int S, HexCoord *at, int *dist) const {
    int best = -1, bd = 1 << 30;
    HexCoord bpos;
    for_units_near(u.pos, S + 8, [&](const Unit &o) {
        if (!o.alive || o.side == u.side) return;
        HexCoord p;
        if (seen_by(o, u.side)) p = o.pos;
        else if (time_min - o.last_seen[u.side] <= 10) p = o.last_seen_pos[u.side];
        else return;
        int d = hex_distance(u.pos, p);
        if (d < bd) {
            bd = d;
            best = o.id;
            bpos = p;
        }
    });
    if (at) *at = bpos;
    if (dist) *dist = bd;
    return best;
}

// Would the next step of the unit's path take it inside its stand-off?
bool World::standoff_blocks(const Unit &u) const {
    if (!u.moving() || u.mode == MV_ASSAULT || u.order != O_MOVE) return false;
    const int S = standoff_hexes(u);
    if (S <= 0) return false;
    HexCoord bpos;
    int bd = 0;
    if (standoff_threat(u, S, &bpos, &bd) < 0) return false;
    const HexCoord next = map.coord_of(u.path[u.path_i]);
    return hex_distance(next, bpos) < S && hex_distance(next, bpos) < bd;
}

void World::update_routes() {
    for (auto &u : units) {
        if (!u.alive || !u.on_route()) continue;
        if (u.moving() || u.order == O_RETREAT) continue;
        if (u.route_pause != RP_NONE) {
            // Halted by an enemy-held hex or at the stand-off line: pick the route up again once
            // that is clear (decided 2026-11-09). Checked every 2 minutes.
            if (time_min < u.route_check || paths_this_tick_ >= MAX_PATHS_PER_TICK) continue;
            u.route_check = time_min + 2;
            if (u.route_pause == RP_ENEMY_HEX && map.in_bounds(u.blocked_hex)) {
                bool held = false;
                for_units_near(u.blocked_hex, 1, [&](const Unit &o) {
                    if (o.alive && o.side != u.side && o.pos == u.blocked_hex) held = true;
                });
                if (held) continue;
            }
            const uint8_t why = u.route_pause;
            const HexCoord blocked = u.blocked_hex;
            u.route_pause = RP_NONE;
            start_leg(u, false);
            if (standoff_blocks(u)) { // still inside the stand-off going this way: wait
                u.path.clear();
                u.path_i = 0;
                u.progress = 0;
                u.order = O_HOLD;
                u.dest = u.pos;
                pause_route(u, why, blocked);
                u.route_check = time_min + 2;
                continue;
            }
            if (u.on_route() && u.side == SIDE_PLAYER)
                push_event(EV_INFO, u.id, false, fmt("%s resumes its route.", u.name.c_str()));
            continue;
        }
        if (u.leg_due) {
            if (time_min < u.wp_halt_until || paths_this_tick_ >= MAX_PATHS_PER_TICK) continue;
            start_leg(u, false);
            continue;
        }
        if (u.pos == u.route[u.wp_i].at) {
            // At the waypoint: halt the set time there, then on to the next one.
            const int halt_for = u.route[u.wp_i].halt_min;
            ++u.wp_i;
            if (!u.on_route()) {
                clear_route(u);
                continue;
            }
            u.leg_due = true;
            u.wp_halt_until = time_min - 1 + halt_for; // it arrived during the last tick
            if (halt_for > 0 && u.side == SIDE_PLAYER)
                push_event(EV_INFO, u.id, false,
                           fmt("%s halts %d min at waypoint %d.", u.name.c_str(), halt_for, u.wp_i));
            if (time_min >= u.wp_halt_until && paths_this_tick_ < MAX_PATHS_PER_TICK) start_leg(u, false);
            continue;
        }
        // Stopped short for another reason (the path ran into ground it can't cross): the unit
        // halts there, as a plain move always has.
        clear_route(u);
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
    update_routes();
    update_groups();
    update_movement();
    update_mounting();
    update_convoys();
    if (time_min % 5 == 0) {
        rebuild_buckets();
        update_zones();
    }
    update_spotting(time_min % 5 == 0);
    update_combat();
    update_suppression();
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
        if (id < 0 || id >= (int)units.size() || !units[id].alive || !orderable(units[id])) return -1;
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
    g.has_dest = false; // members take up their new slots
    return true;
}

// Team orders go to the leader; the members keeping station come along. A member that left
// the formation on its own orders stays out until it is told to rejoin (MRK-22).
int World::order_group_move(int gid, HexCoord target, MoveMode mode) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return -1;
    Group &g = groups[gid];
    for (int id : g.members) {
        if (!units[id].follow) continue; // detached members stay out until they rejoin (MRK-22)
        units[id].pending.active = false; // the team order replaces anything still on its way
        clear_route(units[id]);
    }
    return order_move(g.leader, target, mode);
}

int World::order_group_route(int gid, const std::vector<Waypoint> &route, bool *adjust) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return -1;
    Group &g = groups[gid];
    if (route_delay(g.leader, route) < 0) return -1;
    for (int id : g.members) {
        if (!units[id].follow) continue; // detached members stay out until they rejoin (MRK-22)
        units[id].pending.active = false; // the team order replaces anything still on its way
        clear_route(units[id]);
    }
    return order_route(g.leader, route, adjust);
}

int World::order_group_hold(int gid) {
    if (gid < 0 || gid >= (int)groups.size() || !groups[gid].alive) return -1;
    Group &g = groups[gid];
    for (int id : g.members) {
        if (!units[id].follow) continue;
        units[id].pending.active = false;
        clear_route(units[id]);
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
    // A team moving in column deploys into line once an enemy is known within 1.5 km.
    for (int gid = 0; gid < (int)groups.size(); ++gid) {
        Group &g = groups[gid];
        if (!g.alive || g.shape != FS_COLUMN || g.leader < 0 || !units[g.leader].alive) continue;
        int d = 0;
        if (nearest_known_enemy(units[g.leader], DEPLOY_LINE_HEXES, &d) < 0) continue;
        set_group_shape(gid, FS_LINE, -1);
        if (g.side == SIDE_PLAYER)
            push_event(EV_INFO, g.leader, false,
                       fmt("%s deploys from column into line: enemy known %.1f km away.", g.name.c_str(), d * 0.5));
    }
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
void World::deploy_if_off_road(Unit &u) {
    if (u.mode != MV_ROAD || !u.moving()) return;
    // The column also deploys once an enemy is known within 5 km (Soviet norm: 4-6 km).
    int d = 0;
    if (nearest_known_enemy(u, DEPLOY_COLUMN_HEXES, &d) >= 0) {
        u.mode = MV_HASTY;
        if (u.side == SIDE_PLAYER)
            push_event(EV_INFO, u.id, false,
                       fmt("%s leaves road march and deploys: enemy known %.1f km away.", u.name.c_str(), d * 0.5));
        return;
    }
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
        if (u.pinned) {
            if (u.order != O_RETREAT) continue; // gone to ground: no one moves forward
            speed *= 0.5f;                      // crawling back out of it
        }
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
                    pause_route(u, RP_ENEMY_HEX, next); // picks the route up once the hex is clear
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
            // A waypoint on the way is not an arrival (update_routes takes it from there).
            const bool waypoint = u.on_route() && u.wp_i + 1 < (int)u.route.size() && u.pos == u.route[u.wp_i].at;
            if (u.side == SIDE_PLAYER && !was_retreat && !u.follow && !halted_by_enemy && !waypoint) // team members keeping station don't report
                push_event(EV_ARRIVED, u.id, false, fmt("%s has arrived %s.", u.name.c_str(), place_name(u.pos).c_str()));
        }
    }
}

void World::update_zones() {
    stats_.hot = stats_.warm = stats_.cold = stats_.alive = 0;
    for (auto &u : units) {
        if (!u.alive) continue;
        int best = 1 << 30;
        // Guns that reach further than the hot zone run the full model whenever an enemy is
        // inside their range, so they can fire to their full range.
        const int reach = std::max(HOT_RANGE, weapon_range(u));
        for_units_near(u.pos, std::max(WARM_RANGE, reach), [&](const Unit &o) {
            if (o.side == u.side || !o.alive) return;
            int d = hex_distance(u.pos, o.pos);
            if (d < best) best = d;
        });
        if (best <= reach) u.hot_until = time_min + HOT_HOLD_MIN;
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
            bool concealed = terrain_conceals(th.terrain) &&
                             (!t.moving() || t.mode == MV_DELIBERATE) && time_min - t.last_fired > 3;
            if (concealed && d > (obs.type == U_RECON ? CONCEAL_SPOT_HEXES_RECON : CONCEAL_SPOT_HEXES)) {
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

// The battery moves 1-1.5 km to a fresh position, away from the nearest enemy it knows of when it
// can. A standing operating procedure: no command delay.
void World::displace_battery(Unit &u) {
    u.fire_pos_since = -1;
    u.acquired = -1;
    int foe = nearest_known_enemy(u, 60);
    HexCoord best = u.pos;
    int best_score = -(1 << 30);
    for (int dir = 0; dir < 6; ++dir) {
        HexCoord h = u.pos;
        for (int k = 0; k < 3; ++k) h = neighbor(h, (k == 1) ? (dir + 1) % 6 : dir); // 2-3 hexes out
        if (!map.in_bounds(h) || map.at(h).terrain == T_WATER || !reachable(u.pos, h)) continue;
        int score = foe >= 0 ? hex_distance(h, units[foe].last_seen_pos[u.side]) * 4 : 0;
        if (map.at(h).terrain == T_FOREST || map.at(h).terrain == T_URBAN) score += 3; // cover
        score += rng_.range_int(0, 2);
        if (score > best_score) {
            best_score = score;
            best = h;
        }
    }
    if (best == u.pos) return;
    const MoveMode keep = u.mode;
    u.mode = MV_HASTY;
    clear_route(u);
    start_path(u, best, O_MOVE, false);
    if (!u.moving()) u.mode = keep;
    if (u.side == SIDE_PLAYER && u.moving())
        push_event(EV_ORDER, u.id, false,
                   fmt("%s displaces to a new firing position after %d minutes of fire.", u.name.c_str(),
                       u.displace_after));
}

void World::update_combat() {
    for (auto &u : units) {
        if (!u.alive || u.zone != Z_HOT) continue;
        if (u.fire_cooldown > 0) {
            --u.fire_cooldown;
            continue;
        }
        const auto &ti = unit_type_info(u.type);
        if (ti.indirect && u.fire_pos_since >= 0) {
            // A fire assault ends after 15-20 minutes from one position, or once the guns have been
            // silent for half an hour; then they displace before counter-battery fire arrives.
            if (u.moving() || time_min - u.last_fired > 30) u.fire_pos_since = -1;
            else if (time_min - u.fire_pos_since >= u.displace_after) {
                displace_battery(u);
                continue;
            }
        }
        if (u.ammo <= 0) continue;
        if (u.roe == ROE_HOLD) continue;
        if (u.roe == ROE_RETURN && time_min - u.last_targeted > 5) continue;
        int best = -1, best_d = 1 << 30, best_call = 0;
        const int range = weapon_range(u);
        for_units_near(u.pos, range, [&](const Unit &t) {
            if (t.side == u.side || !t.alive || !seen_by(t, u.side)) return;
            int d = hex_distance(u.pos, t.pos);
            if (d > range || d >= best_d) return;
            // Same sight line as spotting: whatever a unit can see in range, it can shoot at.
            if (!ti.indirect && !line_of_sight(u.pos, t.pos, 3.0f, 2.5f)) return;
            // Guns fire only on what someone who can call fire on the fire net can see.
            int call = 0;
            if (ti.indirect && (call = fire_call(u, t)) < 0) return;
            best = t.id;
            best_d = d;
            best_call = call;
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
            // Guns that were just firing shift to a target within 1 km of the last one in about
            // 2 minutes (FM 100-2-1 standard) instead of plotting a new mission.
            float acquire = et.acquire;
            if (ti.indirect && u.last_target >= 0 && time_min - u.last_fired <= 10 &&
                hex_distance(units[u.last_target].pos, units[best].pos) <= 2)
                acquire = std::min(acquire, 2.0f * et.factor);
            if (best_call > 0) acquire += FIRE_RELAY_MIN; // the call is passed on by an observer
            u.acquired = best;
            u.fire_cooldown = minutes(acquire);
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
        u.ammo = std::max(0.0f, u.ammo - (ti.ammo_per_round > 0 ? ti.ammo_per_round * ti.rounds_per_cycle
                                                  : ti.indirect ? 2.5f : 1.2f));
        u.last_fired = time_min;
        u.last_target = t.id;
        u.readiness = std::max(20.0f, u.readiness - 0.1f);
        if (ti.indirect && u.fire_pos_since < 0) {
            u.fire_pos_since = time_min;
            u.displace_after = rng_.range_int(15, 20);
        }
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
        int kills = 0;
        bool hit = false;
        if (ti.indirect) {
            // A fire mission is one sheaf on the target: one hit roll, its weight in strength
            // points (scaled by the share of guns still firing) resolved as whole elements -
            // each point a 1-in-(100 / elements_max) chance of one element.
            if (rng_.chance(p)) {
                hit = true;
                float dmg = ti.firepower * (float)rng_.range(0.6, 1.4) * u.elements / (float)u.elements_max;
                if (tt.armored) dmg *= ti.anti_armor;
                float expect = dmg / (100.0f / t.elements_max);
                kills = (int)expect;
                if (rng_.chance(expect - kills)) ++kills;
            }
        } else {
            // Direct fire: every vehicle or squad still in the fight engages on its own, firing
            // rounds_per_cycle aimed rounds. Each round has the hit chance p; a hit knocks out one element with a chance set by the shooter's
            // firepower (x anti-armour against armour), calibrated by DIRECT_FIRE_WEIGHT.
            float pk = ti.firepower * (tt.armored ? ti.anti_armor : 1.0f) * DIRECT_FIRE_WEIGHT;
            for (int k = 0; k < u.elements * ti.rounds_per_cycle; ++k) {
                if (!rng_.chance(p)) continue;
                hit = true;
                if (rng_.chance(std::min(0.95, pk * rng_.range(0.6, 1.4)))) ++kills;
            }
        }
        {
            // Suppression: the fire suppresses whether it hits or not, and keeps doing so
            // until this shooter's next cycle; elements knocked out add to the shock.
            float S = suppression_weight(u) * u.elements + std::min(kills, t.elements) * 1.0f;
            if (tt.armored) S *= ti.indirect ? 0.5f : 0.3f;
            if (th.terrain == T_FOREST || th.terrain == T_URBAN) S *= 0.6f;
            suppress(t, S, time_min + std::max(1, minutes(et.cycle)));
        }
        if (hit) {
            const float per_element = 100.0f / t.elements_max;
            kills = std::min(kills, t.elements);
            if (kills > 0 && t.type == U_TRUCK && t.elements > 0) {
                // Each truck knocked out takes its share of the cargo with it.
                const double keep = (double)(t.elements - kills) / t.elements;
                const double lost_l = t.cargo_l * (1 - keep), lost_t = t.cargo_t * (1 - keep), lost_kg = t.cargo_kg * (1 - keep);
                t.cargo_l *= keep;
                t.cargo_t *= keep;
                t.cargo_kg *= keep;
                if (t.side == SIDE_PLAYER && lost_l + lost_t + lost_kg > 0.01)
                    push_event(EV_SUPPLY, t.id, false,
                               fmt("%s: %d truck%s lost with %.0f L diesel, %.1f t ammunition, %.0f kg food.", t.name.c_str(),
                                   kills, kills == 1 ? "" : "s", lost_l, lost_t, lost_kg));
            }
            t.elements -= kills;
            t.strength = 100.0f * t.elements / t.elements_max;
            if (kills > 0 && t.dismounts >= 0 && units[t.dismounts].embarked && units[t.dismounts].elements > 0) {
                // Squads aboard a knocked-out vehicle are lost with it (an estimate: no bail-out).
                Unit &d = units[t.dismounts];
                const float keep = d.elements * (float)t.elements / (float)(t.elements + kills);
                int k = (int)keep;
                if (rng_.chance(keep - k)) ++k;
                if (k < d.elements) {
                    if (t.side == SIDE_PLAYER)
                        push_event(EV_INFO, t.id, false,
                                   fmt("%d squad%s aboard %s lost with the vehicles.", d.elements - k,
                                       d.elements - k == 1 ? "" : "s", t.name.c_str()));
                    d.elements = k;
                    d.strength = 100.0f * k / d.elements_max;
                }
            }
            t.morale = std::max(0.0f, t.morale - kills * per_element * 0.6f);
            t.last_hit = time_min;
            if (t.elements <= 0) {
                t.strength = 0;
                t.alive = false;
                t.path.clear();
                t.path_i = 0;
                t.pending.active = false;
                if (t.side == SIDE_PLAYER && !t.militia) stats_.losses++;
                else if (t.side != SIDE_PLAYER) stats_.kills++;
                push_event(EV_DESTROYED, t.id, t.side == SIDE_PLAYER,
                           t.side != SIDE_PLAYER
                               ? fmt("Hostile %s destroyed by %s.", tt.name, u.name.c_str())
                           : t.militia
                               ? fmt("%s wiped out %s: the town has no garrison left.", t.name.c_str(),
                                     place_name(t.pos).c_str())
                           : t.type == U_HQ
                               ? fmt("%s has been destroyed %s. Orders will be slow, and the trains need about "
                                     "%d minutes to report to a new commander.",
                                     t.name.c_str(), place_name(t.pos).c_str(), HANDOVER_MIN)
                               : fmt("%s has been destroyed %s.", t.name.c_str(), place_name(t.pos).c_str()));
                if (t.type == U_TRUCK) {
                    const std::string why = fmt("destroyed %s with %.0f L diesel, %.1f t ammunition, %.0f kg food aboard",
                                                place_name(t.pos).c_str(), t.cargo_l, t.cargo_t, t.cargo_kg);
                    if (t.convoy_route >= 0) convoy_suspend(t, why.c_str());
                    t.cargo_l = t.cargo_t = t.cargo_kg = 0;
                }
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
            clear_route(u); // a unit that breaks drops its route
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
        if (u.militia || u.type == U_TRUCK) continue; // a garrison holds its town; trucks run their routes
        if (u.pending.active || (u.order == O_RETREAT && u.moving()) || u.mounting) continue;
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
        // Mounted infantry (Soviet practice): halted within 2 km of a known enemy, the squads
        // get out and the vehicles stay back to overwatch; with nothing seen within 5 km they
        // mount up again before moving on.
        // (Squads that can no longer all fit into the vehicles left go on as plain infantry.)
        const bool squads_out = u.dismounts >= 0 && units[u.dismounts].alive && can_mount(u);
        if (u.type == U_MECH) {
            if (tgt >= 0 && td <= 4 && !u.moving() && can_dismount(u)) {
                order_dismount(u.id);
                continue;
            }
            if (squads_out && (tgt < 0 || td > 10) && can_mount(u) && !units[u.dismounts].pending.active) {
                order_mount(u.id);
                continue;
            }
            if (squads_out) {
                if (u.moving() && tgt >= 0 && td < 6) order_hold(u.id);
                continue;
            }
        }
        // Squads whose vehicles are still running wait for them rather than wander off.
        if (u.carrier >= 0 && can_mount(u) && tgt < 0) continue;
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
        if (f.hunts_player && ((u.type == U_ARTILLERY && hex_distance(u.pos, obj) <= std::min(16, weapon_range(u) * 2 / 3)) ||
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
    // Holdings first: who holds what, loyalty, militia; then the trains draw from the depot they
    // stand at, so what a convoy or a requisition brought in is handed out this same hour.
    update_holdings();
    update_trains_at_depot(hq);
    stock.personnel = 0;
    for (auto &u : units) {
        if (!u.alive || u.side != SIDE_PLAYER) continue;
        const auto &ti = unit_type_info(u.type);
        if (u.militia) {
            // The town feeds and arms its own militia (small arms from the town's stores).
            u.ammo = std::min(100.0f, u.ammo + 10.0f);
            continue;
        }
        if (u.carrier >= 0) stock.personnel += u.squad_men * u.elements;          // squads on foot
        else if (u.dismounts >= 0 && units[u.dismounts].alive) stock.personnel += 3 * u.elements; // crews only
        else stock.personnel += (int)std::round(ti.crew * u.strength / 100.0f);
        // Resupply from the company train: halted near the HQ and out of the fight for 30 minutes
        // (enemies merely being nearby doesn't stop the trucks).
        bool fighting = time_min - u.last_fired < 30 || time_min - u.last_hit < 30 || time_min - u.last_targeted < 30;
        if (hq < 0 || fighting || u.moving() || hex_distance(u.pos, units[hq].pos) > 3) continue;
        float rate = (u.resting ? 50.0f : 25.0f) * (acting ? ACTING_SUPPLY_RATE : 1.0f);
        float want_fuel = std::min(rate, 100.0f - u.fuel);
        double litres = want_fuel / 100.0 * ti.fuel_litres;
        if (litres > 0 && stock.diesel_l > 0) {
            double got = std::min(litres, stock.diesel_l);
            stock.diesel_l -= got;
            used_diesel_l_ += got;
            u.fuel += (float)(got / ti.fuel_litres * 100.0);
            u.out_of_fuel_reported = false;
        }
        float want_ammo = std::min(rate, 100.0f - u.ammo);
        double tonnes = want_ammo / 100.0 * ti.ammo_tonnes;
        if (tonnes > 0 && stock.ammo_t > 0) {
            double got = std::min(tonnes, stock.ammo_t);
            stock.ammo_t -= got;
            used_ammo_t_ += got;
            u.ammo += (float)(got / ti.ammo_tonnes * 100.0);
        }
    }
    // Consumption smoothed over about a day, for the days-of-supply readout.
    stock.rate_diesel_lph = stock.rate_diesel_lph * 0.9 + used_diesel_l_ * 0.1;
    stock.rate_ammo_tph = stock.rate_ammo_tph * 0.9 + used_ammo_t_ * 0.1;
    used_diesel_l_ = used_ammo_t_ = 0;
    if ((time_min % 1440) == 6 * 60) {
        double need = stock.personnel * 1.5;
        if (stock.food_kg >= need) {
            stock.food_kg -= need;
        } else {
            stock.food_kg = 0;
            for (auto &u : units)
                if (u.alive && u.side == SIDE_PLAYER && !u.militia) u.morale = std::max(0.0f, u.morale - 10.0f);
            push_event(EV_SUPPLY, -1, true, "The company is out of food. Morale is falling.");
        }
        // The holdings' day: taxes and food into their stores, volunteers into the pool.
        double cash_day = 0, food_day = 0;
        int towns = 0;
        for (auto &h : holdings) {
            if (h.owner < 0) continue;
            double c, f;
            daily_yield(h, c, f);
            h.cash += c;
            h.food_kg = std::min(h.food_kg + f, std::max(h.food_kg, 10.0 * f + 2000.0)); // the stores hold ten days
            if (h.kind == HK_TOWN && h.loyalty >= 50.0f)
                h.manpower = std::min(h.manpower + (int)(h.population * 0.001), (int)(h.population * 0.01));
            if (h.owner == SIDE_PLAYER) {
                cash_day += c;
                food_day += f;
                ++towns;
            }
        }
        if (towns > 0)
            push_event(EV_HOLDING, -1, false,
                       fmt("The holdings' day: %.0f DM and %.0f kg of food into their stores (%d place%s). The trains "
                           "collect it when parked there, or a supply route carries it.",
                           cash_day, food_day, towns, towns == 1 ? "" : "s"));
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
                if (p.alive && p.side == SIDE_PLAYER && !p.militia) own.push_back(p.id);
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
        // A platoon's squads on foot count only once its vehicles no longer do.
        if (u.carrier >= 0 && units[u.carrier].alive && units[u.carrier].strength >= 30.0f) continue;
        if (u.alive && !u.militia && u.type != U_HQ && u.type != U_ARTILLERY && u.type != U_TRUCK && u.strength >= 30.0f) ++n;
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
constexpr char kSaveMagic[8] = {'M', 'U', 'R', 'K', 'S', 'S', 'V', '9'};

struct SaveWriter {
    std::vector<uint8_t> b;
    bool ok = true; // shared shape with SaveReader (io_route); writing never fails
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

template <typename A> void io_route(A &a, std::vector<Waypoint> &r) {
    uint32_t n = (uint32_t)r.size();
    a.pod(n);
    if (n > (uint32_t)MAX_WAYPOINTS) {
        n = 0;
        a.ok = false;
    }
    r.resize(n);
    for (auto &w : r) {
        a.pod(w.at); a.pod(w.mode); a.pod(w.halt_min); a.pod(w.shape);
    }
}

template <typename A, typename U> void io_unit(A &a, U &u) {
    a.pod(u.id); a.pod(u.side); a.pod(u.type); a.str(u.name); a.str(u.equipment);
    a.pod(u.formation); a.pod(u.slot); a.pod(u.pos); a.pod(u.prev_pos); a.pod(u.dest);
    a.ints(u.path);
    uint64_t pi = u.path_i;
    a.pod(pi);
    u.path_i = (size_t)pi;
    a.pod(u.progress); a.pod(u.step_cost); a.pod(u.order);
    // Field by field: whole structs would carry their (uninitialised) padding into the save.
    a.pod(u.pending.active); a.pod(u.pending.kind); a.pod(u.pending.target); a.pod(u.pending.issued_at);
    a.pod(u.pending.activates_at); a.pod(u.pending.mode);
    io_route(a, u.pending.route); a.pod(u.pending.adjust);
    io_route(a, u.route); a.pod(u.wp_i); a.pod(u.wp_halt_until); a.pod(u.leg_due);
    a.pod(u.route_pause); a.pod(u.blocked_hex); a.pod(u.route_check);
    a.pod(u.last_adjust_at); a.pod(u.adjust_streak);
    a.pod(u.mode);
    a.pod(u.roe); a.pod(u.brk);
    a.pod(u.pending_stance.active); a.pod(u.pending_stance.roe); a.pod(u.pending_stance.brk);
    a.pod(u.pending_stance.activates_at); a.pod(u.pending_stance.standoff);
    a.pod(u.resting); a.pod(u.last_targeted);
    a.pod(u.elements_max); a.pod(u.elements); a.pod(u.acquired); a.pod(u.group); a.pod(u.follow);
    a.pod(u.standoff); a.pod(u.standoff_next);
    a.pod(u.strength); a.pod(u.morale); a.pod(u.fuel); a.pod(u.ammo); a.pod(u.readiness);
    a.pod(u.zone); a.pod(u.hot_until); a.pod(u.fire_cooldown); a.pod(u.last_fired); a.pod(u.last_hit);
    a.pod(u.last_target); a.pod(u.fire_pos_since); a.pod(u.displace_after);
    a.pod(u.alive); a.pod(u.out_of_fuel_reported);
    a.pod(u.carrier); a.pod(u.dismounts); a.pod(u.embarked); a.pod(u.mounting); a.pod(u.mount_at);
    a.pod(u.squad_men);
    a.pod(u.suppression); a.pod(u.supp_hold_until); a.pod(u.pinned);
    a.pod(u.seen_until); a.pod(u.last_seen); a.pod(u.last_seen_pos); a.pod(u.ai_next);
    // v7: holdings and supply.
    a.pod(u.militia); a.pod(u.holding); a.pod(u.convoy_state); a.pod(u.convoy_route);
    a.pod(u.cargo_l); a.pod(u.cargo_t); a.pod(u.cargo_kg); a.pod(u.convoy_wait_until); a.pod(u.trip_started);
}

template <typename A, typename H> void io_holding(A &a, H &h) {
    a.pod(h.settlement); a.str(h.name); a.pod(h.pos); a.pod(h.radius); a.pod(h.kind); a.pod(h.owner);
    a.pod(h.known_owner); a.pod(h.loyalty); a.pod(h.population);
    a.pod(h.diesel_l); a.pod(h.ammo_t); a.pod(h.food_kg); a.pod(h.cash); a.pod(h.manpower);
    a.pod(h.militia_unit); a.pod(h.training); a.pod(h.training_progress); a.pod(h.contested_since);
    a.pod(h.captured_at); a.pod(h.last_fight);
    for (auto &x : h.levy_until) a.pod(x);
}

template <typename A, typename R> void io_route(A &a, R &r) {
    a.pod(r.active); a.pod(r.side); a.pod(r.source); a.pod(r.dest); a.pod(r.convoy); a.pod(r.trips);
    a.pod(r.delivered_l); a.pod(r.delivered_t); a.pod(r.delivered_kg); a.pod(r.last_round_trip_min);
    a.pod(r.suspended_at);
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
    w.pod(stock.diesel_l); w.pod(stock.ammo_t); w.pod(stock.food_kg); w.pod(stock.personnel);
    w.pod(stock.cash); w.pod(stock.replacements); w.pod(stock.rate_diesel_lph); w.pod(stock.rate_ammo_tph);
    w.pod(used_diesel_l_); w.pod(used_ammo_t_);
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
    w.pod((uint32_t)routes.size());
    for (const auto &r : routes) io_route(w, const_cast<SupplyRoute &>(r));
    w.pod((uint32_t)markings.size());
    for (const auto &m : markings) {
        w.pod(m.kind);
        w.str(m.name);
        w.ints(m.xy);
    }
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
    r.pod(t); r.pod(sd); r.pod(home); r.pod(rs);
    r.pod(st.diesel_l); r.pod(st.ammo_t); r.pod(st.food_kg); r.pod(st.personnel);
    r.pod(st.cash); r.pod(st.replacements); r.pod(st.rate_diesel_lph); r.pod(st.rate_ammo_tph);
    double used_l = 0, used_t = 0;
    r.pod(used_l); r.pod(used_t);
    r.pod(kills); r.pod(losses); r.pod(oc); r.str(ot);
    uint8_t dso = 0;
    r.pod(dso);
    std::vector<Formation> fs(r.count(4096));
    for (auto &f : fs) io_formation(r, f);
    std::vector<Unit> us(r.count(1 << 20));
    for (auto &u : us) io_unit(r, u);
    std::vector<Group> gs(r.count(4096));
    for (auto &g : gs) io_group(r, g);
    std::vector<Holding> hs(r.count(1 << 16));
    for (auto &h : hs) io_holding(r, h);
    std::vector<SupplyRoute> rts(r.count(4096));
    for (auto &x : rts) io_route(r, x);
    std::vector<Marking> mks(r.count(MAX_MARKINGS));
    for (auto &m : mks) {
        r.pod(m.kind);
        r.str(m.name);
        r.ints(m.xy);
    }
    if (!r.ok) return fail("save is truncated or corrupt");
    for (const auto &m : mks)
        if (!marking_ok(map, m)) return fail("save is corrupt");
    for (const auto &h : hs)
        if (h.settlement < 0 || h.settlement >= (int)map.settlements.size() || !map.in_bounds(h.pos) ||
            h.kind >= HK_COUNT || h.militia_unit >= (int)us.size())
            return fail("save is corrupt");
    for (const auto &x : rts)
        if (x.source >= (int)hs.size() || x.dest >= (int)hs.size() || x.convoy >= (int)us.size() ||
            (x.active && x.source < 0))
            return fail("save is corrupt");
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
            u.elements > u.elements_max || u.carrier >= (int)us.size() || u.dismounts >= (int)us.size() ||
            u.carrier < -1 || u.dismounts < -1 || u.holding >= (int)hs.size() || u.convoy_route >= (int)rts.size())
            return fail("save is corrupt");
        for (int idx : u.path)
            if (idx < 0 || idx >= n_cells) return fail("save is corrupt");
        if (u.wp_i < 0 || u.wp_i > (int)u.route.size()) return fail("save is corrupt");
        for (const auto *r : {&u.route, &u.pending.route})
            for (const auto &wp : *r)
                if (!map.in_bounds(wp.at)) return fail("save is corrupt");
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
    used_diesel_l_ = used_l;
    used_ammo_t_ = used_t;
    formations = std::move(fs);
    units = std::move(us);
    groups = std::move(gs);
    holdings = std::move(hs);
    routes = std::move(rts);
    markings = std::move(mks);
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
