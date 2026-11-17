// Civilians (work board MRK-34): traffic between towns and the towns' services (police, fire
// brigade, ambulance). They are not units and not a side: nobody orders them, targets them or
// captures with them. They are non-combatants and take no damage (user, 16 Nov 2026): fire
// aimed at a unit in their hex passes them by. They get out of the way of fighting and slow
// military columns on the roads they share. Services come out to a place that was hit once it
// has gone quiet.
//
// All numbers here are first estimates (marked), to be replaced by sourced values.
//
// Civilians roll their own dice (civ_rng_), so creating them never shifts the military rolls.
// Only congestion reaches back into the fight.
#include "murks/geo.hpp"
#include "murks/planning.hpp"
#include "murks/sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace murks {

namespace {

std::string cfmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// --- First estimates ----------------------------------------------------------------------
constexpr int kActiveRadius = 60;      // hexes (30 km) from any military unit: towns with traffic
constexpr int kKeepRadius = 80;        // hexes (40 km): beyond this civilians leave the simulation
constexpr int kTripRadius = 50;        // hexes (25 km): how far a trip goes (the real map lists towns, not every village)
constexpr int kMaxCivilians = 250;
constexpr int kMaxPerTown = 12;        // vehicles out on the road from one town at once
constexpr double kPeoplePerVehicle = 2000.0;
constexpr double kNightTraffic = 0.25;
constexpr int kPathsPerTick = 3;
constexpr int kDangerMinutes = 15;     // a shot or a hit keeps the area dangerous this long
constexpr int kFleeRadius = 6;         // hexes (3 km): traffic turns away
constexpr int kPullOverRadius = 2;     // hexes (1 km): traffic stops at the roadside
constexpr int kPullOverMinutes = 30;
constexpr int kQuietMinutes = 30;      // services go out once no shot within 2 km for this long
constexpr int kQuietRadius = 4;
constexpr int kServiceRadius = 30;     // hexes (15 km): how far a town's services go
constexpr int kSceneMinutes = 60;
constexpr int kPoliceTown = 5000, kFireTown = 10000;

// Speed on a road against a military wheeled vehicle's road time (0.9 min a hex, ~33 km/h):
// cars a little faster, tractors much slower.
const float kCivSpeed[CV_KIND_COUNT] = {1.2f, 0.9f, 0.45f, 0.85f, 1.2f, 1.0f, 1.1f};
const char *kCivNames[CV_KIND_COUNT] = {"car", "bus", "tractor", "lorry", "police car", "fire engine", "ambulance"};
const char *kCivStates[] = {"driving", "parked", "pulled over", "on its way to a scene", "at a scene", "going home"};

} // namespace

const char *civ_kind_name(int kind) { return kind >= 0 && kind < CV_KIND_COUNT ? kCivNames[kind] : "?"; }
const char *civ_state_name(int state) { return state >= 0 && state <= CS_RETURNING ? kCivStates[state] : "?"; }

const Civilian *World::civilian(int id) const {
    for (const auto &c : civilians)
        if (c.id == id && c.alive) return &c;
    return nullptr;
}

int World::civilians_at(int hex_idx) const {
    auto it = civ_hex_count_.find(hex_idx);
    return it == civ_hex_count_.end() ? 0 : it->second;
}

void World::civ_rebuild_counts() {
    civ_hex_count_.clear();
    // Only traffic on the move holds up a column: parked, pulled-over and working vehicles
    // are off the carriageway.
    for (const auto &c : civilians)
        if (c.alive && c.moving() && (c.state == CS_DRIVING || c.state == CS_RESPONDING || c.state == CS_RETURNING))
            ++civ_hex_count_[map.index(c.pos)];
}

// GDR or Poland, for the police: west or east of the Oder-Neisse line (a few points along
// it; good enough for a police car's livery, not for borders).
uint8_t World::town_country(int s) {
    if (town_country_.size() != map.settlements.size()) town_country_.assign(map.settlements.size(), 255);
    if (s < 0 || s >= (int)town_country_.size()) return CC_GDR;
    if (town_country_[s] != 255) return town_country_[s];
    uint8_t c = CC_GDR;
    if (map.epsg == 3035) {
        double e0, n0;
        grid_origin(map, e0, n0);
        Vec2 p = hex_center(map.settlements[s].pos);
        double lon, lat;
        laea3035_inverse(e0 + p.x, n0 - p.y, lon, lat);
        static const double line[][2] = {{54.2, 14.2}, {53.4, 14.4}, {52.6, 14.6}, {52.0, 14.7}, {51.4, 14.8}, {50.8, 15.0}};
        double border = 14.6;
        for (int i = 0; i + 1 < 6; ++i)
            if (lat <= line[i][0] && lat >= line[i + 1][0]) {
                const double t = (line[i][0] - lat) / (line[i][0] - line[i + 1][0]);
                border = line[i][1] + t * (line[i + 1][1] - line[i][1]);
            }
        if (lat < 50.8) border = 15.0;
        if (lat > 54.2) border = 14.2;
        c = lon >= border ? CC_POLAND : CC_GDR;
    } else {
        c = map.settlements[s].pos.col * 2 >= map.cols() ? CC_POLAND : CC_GDR; // generated maps: east half
    }
    town_country_[s] = c;
    return c;
}

// Civilians keep to roads: a road step at their own speed, anything else is slow going.
float World::civ_step_minutes(const Civilian &c, int from_idx, int to_idx) const {
    const Hex &b = map.at_index(to_idx);
    if (b.terrain == T_WATER) return -1;
    const bool road = road_step(from_idx, to_idx);
    float m = road ? road_minutes(M_WHEELED) : terrain_minutes(M_WHEELED, b.terrain) * 2.0f;
    if (road && (map.at_index(from_idx).features & F_MAJOR_ROAD) && (b.features & F_MAJOR_ROAD)) m *= 0.8f;
    if (!road && (b.features & F_RIVER)) m += 60.0f;
    return m / kCivSpeed[c.kind];
}

// Any other place within a trip's reach, at random; with none that close, the nearest one.
int World::civ_pick_destination(int home, HexCoord from) {
    int dest = -1, seen = 0, nearest = -1, nearest_d = 1 << 30;
    for (int t = 0; t < (int)map.settlements.size(); ++t) {
        if (t == home) continue;
        const int d = hex_distance(map.settlements[t].pos, from);
        if (d < nearest_d) nearest_d = d, nearest = t;
        if (d > kTripRadius) continue;
        if (civ_rng_.range_int(0, seen++) == 0) dest = t;
    }
    return dest >= 0 ? dest : (nearest_d <= kTripRadius * 2 ? nearest : -1);
}

bool World::civ_path(Civilian &c, HexCoord to) {
    c.path.clear();
    c.path_i = 0;
    c.progress = 0;
    c.target = to;
    if (to == c.pos) return true;
    if (civ_paths_this_tick_ >= kPathsPerTick || !map.in_bounds(to) || map.at(to).terrain == T_WATER ||
        !reachable(c.pos, to))
        return false;
    ++civ_paths_this_tick_;
    const float min_step = road_minutes(M_WHEELED) * 0.8f / 1.2f;
    PathResult pr = pathfinder().find(
        c.pos, to, [&](int f, int t) { return civ_step_minutes(c, f, t); }, min_step, c.path, 60000, 1.6f);
    if (!pr.found || c.path.empty()) {
        c.path.clear();
        return false;
    }
    c.step_cost = civ_step_minutes(c, map.index(c.pos), c.path[0]);
    return c.step_cost >= 0;
}

int World::civ_new(uint8_t kind, int home, HexCoord at) {
    Civilian c;
    c.id = civ_next_id_++;
    c.kind = kind;
    c.home = home;
    c.pos = c.prev_pos = c.target = at;
    c.country = town_country(home);
    civilians.push_back(c);
    return (int)civilians.size() - 1;
}

int World::nearest_danger(HexCoord h, int64_t since) const {
    int best = 999;
    for (const auto &d : danger_)
        if (d.until - kDangerMinutes >= since) best = std::min(best, hex_distance(h, d.at));
    return best;
}

// Called from the combat code for every shot: the shooter's and the target's hex.
void World::note_danger(HexCoord at, bool hit) {
    bool merged = false;
    for (auto &d : danger_)
        if (d.at == at) {
            d.until = time_min + kDangerMinutes;
            merged = true;
            break;
        }
    if (!merged) danger_.push_back({at, time_min + kDangerMinutes});
    for (auto &in : incidents_)
        if (hex_distance(in.at, at) <= kQuietRadius && !in.served) in.last_shot = time_min;
    if (!hit) return;
    for (const auto &in : incidents_)
        if (!in.served && hex_distance(in.at, at) <= 2) return;
    if (incidents_.size() < 64) incidents_.push_back({at, time_min, false});
}

void World::civ_update_active_towns() {
    active_towns_.clear();
    if (!civilians_on) return;
    // Towns within reach of any military unit (both sides): coarse check against unit positions.
    std::vector<HexCoord> pts;
    for (const auto &u : units)
        if (u.alive) pts.push_back(u.pos);
    for (int s = 0; s < (int)map.settlements.size(); ++s) {
        const HexCoord p = map.settlements[s].pos;
        for (const auto &q : pts)
            if (hex_distance(p, q) <= kActiveRadius) {
                active_towns_.push_back(s);
                break;
            }
    }
}

void World::civ_station_services() {
    // A service of each kind the town is big enough for, parked in town; services of towns that
    // went quiet leave once they are home.
    std::vector<uint8_t> active(map.settlements.size(), 0);
    for (int s : active_towns_) active[s] = 1;
    for (auto &c : civilians)
        if (c.alive && civ_is_service(c.kind) && c.state == CS_PARKED && (c.home < 0 || !active[c.home])) c.alive = false;
    for (int s : active_towns_) {
        const int pop = map.settlements[s].population;
        bool has[CV_KIND_COUNT] = {};
        for (const auto &c : civilians)
            if (c.alive && c.home == s && civ_is_service(c.kind)) has[c.kind] = true;
        const HexCoord at = map.settlements[s].pos;
        if (pop >= kPoliceTown && !has[CV_POLICE]) civilians[civ_new(CV_POLICE, s, at)].state = CS_PARKED;
        if (pop >= kFireTown && !has[CV_FIRE]) civilians[civ_new(CV_FIRE, s, at)].state = CS_PARKED;
        if (pop >= kFireTown && !has[CV_AMBULANCE]) civilians[civ_new(CV_AMBULANCE, s, at)].state = CS_PARKED;
    }
}

void World::civ_spawn_traffic() {
    int total = 0;
    std::vector<int> out(map.settlements.size(), 0);
    for (const auto &c : civilians)
        if (c.alive && !civ_is_service(c.kind)) {
            ++total;
            if (c.home >= 0) ++out[c.home];
        }
    const double f = is_night() ? kNightTraffic : 1.0;
    for (int s : active_towns_) {
        if (total >= kMaxCivilians) break;
        const Settlement &S = map.settlements[s];
        const double want = std::min((double)kMaxPerTown, S.population / kPeoplePerVehicle * f);
        if (out[s] >= want) {
            // Small places: below one vehicle, now and then one goes out.
            if (!(want < 1.0 && out[s] == 0 && civ_rng_.chance(want * 0.5))) continue;
        } else if (!civ_rng_.chance(0.5)) {
            continue;
        }
        // A destination: any other place within a trip's reach, picked at random.
        const int dest = civ_pick_destination(s, S.pos);
        if (dest < 0) continue;
        uint8_t kind = CV_CAR;
        const double r = civ_rng_.uniform();
        if (S.population >= 5000 && r < 0.15) kind = CV_BUS;
        else if (r < 0.32) kind = CV_LORRY;
        else if (S.population < 5000 && r < 0.55) kind = CV_TRACTOR;
        else if (S.population >= 5000 && r < 0.38) kind = CV_TRACTOR;
        const int i = civ_new(kind, s, S.pos);
        Civilian &c = civilians[i];
        c.dest = dest;
        if (!civ_path(c, map.settlements[dest].pos)) {
            civilians.pop_back();
            --civ_next_id_;
            if (civ_paths_this_tick_ >= kPathsPerTick) break;
            continue;
        }
        ++total;
        ++out[s];
    }
}

void World::civ_move() {
    for (auto &c : civilians) {
        if (!c.alive || !c.moving() || c.state == CS_PULLED_OVER || c.state == CS_PARKED || c.state == CS_AT_SCENE)
            continue;
        c.progress += 1.0f;
        while (c.moving() && c.progress >= c.step_cost) {
            c.progress -= c.step_cost;
            c.prev_pos = c.pos;
            c.pos = map.coord_of(c.path[c.path_i++]);
            if (c.moving()) {
                c.step_cost = civ_step_minutes(c, map.index(c.pos), c.path[c.path_i]);
                if (c.step_cost < 0) {
                    c.path.clear();
                    c.path_i = 0;
                }
            }
        }
        if (c.moving()) continue;
        c.progress = 0;
        switch (c.state) {
            case CS_DRIVING: c.alive = false; break; // arrived: off the road and out of the simulation
            case CS_RESPONDING:
                c.state = CS_AT_SCENE;
                c.wait_until = time_min + kSceneMinutes;
                break;
            case CS_RETURNING: c.state = CS_PARKED; break;
            default: break;
        }
    }
}

void World::civ_react() {
    // Every 2 minutes: traffic near fighting turns away or pulls over; services near fresh
    // fighting break off and go home.
    const int64_t since = time_min - kDangerMinutes;
    for (auto &c : civilians) {
        if (!c.alive) continue;
        const int d = nearest_danger(c.pos, since);
        if (civ_is_service(c.kind)) {
            if ((c.state == CS_RESPONDING || c.state == CS_AT_SCENE) && d <= kQuietRadius && c.home >= 0) {
                c.state = CS_RETURNING;
                if (!civ_path(c, map.settlements[c.home].pos)) c.state = CS_AT_SCENE, c.wait_until = time_min + 10;
            } else if (c.state == CS_AT_SCENE && time_min >= c.wait_until && c.home >= 0) {
                c.state = CS_RETURNING;
                if (!civ_path(c, map.settlements[c.home].pos)) c.state = CS_AT_SCENE;
            } else if (c.state == CS_RETURNING && !c.moving() && c.home >= 0 && c.pos != map.settlements[c.home].pos) {
                civ_path(c, map.settlements[c.home].pos); // a path refused for lack of budget: try again
            }
            continue;
        }
        if (c.state == CS_PULLED_OVER) {
            if (time_min >= c.wait_until && d > kPullOverRadius) c.state = CS_DRIVING;
            continue;
        }
        if (!c.moving() && c.dest >= 0 && c.pos != map.settlements[c.dest].pos) {
            civ_path(c, map.settlements[c.dest].pos); // no route earlier (budget): try again
            continue;
        }
        if (d <= kPullOverRadius) {
            c.state = CS_PULLED_OVER;
            c.wait_until = time_min + kPullOverMinutes;
            continue;
        }
        if (d <= kFleeRadius && !c.fleeing) {
            // Away from it: the place within a trip's reach that is farthest from the danger and
            // farther than here, home first if it qualifies.
            HexCoord danger = c.pos;
            int dd = 999;
            for (const auto &m : danger_)
                if (m.until - kDangerMinutes >= since && hex_distance(c.pos, m.at) < dd) dd = hex_distance(c.pos, m.at), danger = m.at;
            int best = -1, best_d = hex_distance(c.pos, danger) + 4;
            if (c.home >= 0 && hex_distance(map.settlements[c.home].pos, danger) >= best_d &&
                hex_distance(map.settlements[c.home].pos, c.pos) <= kTripRadius * 2)
                best = c.home;
            if (best < 0)
                for (int t = 0; t < (int)map.settlements.size(); ++t) {
                    const HexCoord p = map.settlements[t].pos;
                    if (hex_distance(p, c.pos) > kTripRadius) continue;
                    const int td = hex_distance(p, danger);
                    if (td > best_d) best = t, best_d = td;
                }
            if (best >= 0) {
                const int old_dest = c.dest;
                c.dest = best;
                if (civ_path(c, map.settlements[best].pos)) c.fleeing = true;
                else c.dest = old_dest; // no route yet (budget): try again in 2 min
            }
        }
    }
}

bool World::player_hears_of(HexCoord at, int town) const {
    for (const auto &u : units)
        if (u.alive && !u.removed && u.side == SIDE_PLAYER && hex_distance(u.pos, at) <= (int)effective_spot_range(u))
            return true;
    for (const auto &h : holdings) {
        if (h.owner != SIDE_PLAYER) continue;
        if (hex_distance(h.pos, at) <= h.radius + 2) return true;
        if (town >= 0 && hex_distance(h.pos, map.settlements[town].pos) <= h.radius) return true;
    }
    return false;
}

void World::civ_dispatch_services() {
    for (auto &in : incidents_) {
        if (in.served || time_min - in.last_shot < kQuietMinutes) continue;
        if (nearest_danger(in.at, time_min - kDangerMinutes) <= kQuietRadius) continue;
        // The nearest town with services parked: they all go.
        int town = -1, town_d = kServiceRadius + 1;
        for (const auto &c : civilians)
            if (c.alive && civ_is_service(c.kind) && c.state == CS_PARKED && c.home >= 0) {
                const int d = hex_distance(map.settlements[c.home].pos, in.at);
                if (d < town_d) town_d = d, town = c.home;
            }
        if (town < 0) continue;
        int sent = 0;
        for (auto &c : civilians)
            if (c.alive && civ_is_service(c.kind) && c.state == CS_PARKED && c.home == town && civ_path(c, in.at)) {
                c.state = CS_RESPONDING;
                c.dest = -1;
                ++sent;
            }
        if (sent == 0) continue; // no route this tick: try again in 5 min
        in.served = true;
        // The player hears of it only through someone of their own: a unit that could see the place,
        // or an own holding there or at the town the services came from. Fights between the other
        // sides far away (three-way bands) stay unknown, as their shooting does.
        if (player_hears_of(in.at, town))
            push_event(EV_INFO, -1, false,
                       cfmt("Fire brigade and ambulance from %s on their way to %s.", map.settlements[town].name.c_str(),
                            place_name(in.at).c_str()));
    }
    incidents_.erase(std::remove_if(incidents_.begin(), incidents_.end(),
                                    [&](const Incident &in) { return time_min - in.last_shot > 6 * 60; }),
                     incidents_.end());
}

// Spread over five ticks: each civilian is checked every 5 minutes, a fifth of them a tick.
void World::civ_update_seen() {
    for (auto &c : civilians) {
        if (!c.alive || (c.id + time_min) % 5 != 0) continue;
        c.seen = false;
        for (const auto &u : units) {
            if (!u.alive || u.side != SIDE_PLAYER) continue;
            const int r = (int)effective_spot_range(u);
            const int d = hex_distance(u.pos, c.pos);
            if (d > r) continue;
            if (d <= 1 || line_of_sight(u.pos, c.pos, 3.0f, 2.0f)) {
                c.seen = true;
                break;
            }
        }
        if (!c.seen)
            for (const auto &h : holdings)
                if (h.owner == SIDE_PLAYER && hex_distance(h.pos, c.pos) <= h.radius) {
                    c.seen = true;
                    break;
                }
    }
}

void World::update_civilians() {
    civ_paths_this_tick_ = 0;
    if (!civilians_on) {
        if (!civilians.empty()) civilians.clear();
        civ_hex_count_.clear();
        return;
    }
    danger_.erase(std::remove_if(danger_.begin(), danger_.end(), [&](const DangerMark &d) { return d.until < time_min; }),
                  danger_.end());
    if (time_min % 60 == 0 || (active_towns_.empty() && time_min % 10 == 0)) {
        civ_update_active_towns();
        civ_station_services();
        // Out of reach of the fighting: gone from the simulation.
        for (auto &c : civilians) {
            if (!c.alive || civ_is_service(c.kind)) continue;
            bool near = false;
            for (const auto &u : units)
                if (u.alive && hex_distance(u.pos, c.pos) <= kKeepRadius) {
                    near = true;
                    break;
                }
            if (!near) c.alive = false;
        }
    }
    if (time_min % 10 == 0) civ_spawn_traffic();
    civ_move();
    if (time_min % 2 == 0) civ_react();
    if (time_min % 5 == 0) civ_dispatch_services();
    civ_update_seen();
    civilians.erase(std::remove_if(civilians.begin(), civilians.end(), [](const Civilian &c) { return !c.alive; }),
                    civilians.end());
    civ_rebuild_counts();
    stats_.civilians = (int)civilians.size();
}

int World::sandbox_place_civilian(int kind, HexCoord at) {
    if (!sandbox || kind < 0 || kind >= CV_KIND_COUNT || !map.in_bounds(at)) return -1;
    // The nearest land hex to the click.
    HexCoord pos = at;
    if (map.at(pos).terrain == T_WATER) {
        bool found = false;
        for (int r = 1; r <= 6 && !found; ++r)
            for (int dc = -r; dc <= r && !found; ++dc)
                for (int dr = -r; dr <= r && !found; ++dr) {
                    HexCoord h{at.col + dc, at.row + dr};
                    if (map.in_bounds(h) && hex_distance(h, at) == r && map.at(h).terrain != T_WATER) pos = h, found = true;
                }
        if (!found) return -1;
    }
    const int home = map.nearest_settlement(pos, 200);
    if (home < 0) return -1;
    const int i = civ_new((uint8_t)kind, home, pos);
    Civilian &c = civilians[i];
    if (civ_is_service(kind)) {
        c.state = CS_RETURNING;
        civ_paths_this_tick_ = 0;
        if (!civ_path(c, map.settlements[home].pos) || !c.moving()) c.state = CS_PARKED;
    } else {
        const int dest = civ_pick_destination(home, pos);
        c.dest = dest;
        civ_paths_this_tick_ = 0;
        if (dest >= 0) civ_path(c, map.settlements[dest].pos);
        // With no route it waits at the roadside until the next reaction check moves it on.
        if (!c.moving()) c.state = CS_PULLED_OVER, c.wait_until = time_min + 10;
    }
    c.seen = true;
    civ_rebuild_counts();
    stats_.civilians = (int)civilians.size();
    push_event(EV_INFO, -1, false, cfmt("Sandbox: civilian %s placed %s.", civ_kind_name(kind), place_name(pos).c_str()));
    return c.id;
}

bool World::sandbox_remove_civilian(int id) {
    if (!sandbox) return false;
    for (size_t i = 0; i < civilians.size(); ++i)
        if (civilians[i].id == id && civilians[i].alive) {
            civilians.erase(civilians.begin() + (long)i);
            civ_rebuild_counts();
            stats_.civilians = (int)civilians.size();
            return true;
        }
    return false;
}

} // namespace murks
