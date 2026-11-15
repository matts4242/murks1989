// Murks 1989 — the planning map (work board MRK-25): the ground shown the way a commander
// plans on it. Every figure comes from the functions the rules themselves use, so what the
// map shows is what the simulation does:
//   going  — World::step_minutes (the pathfinder's step cost), off-road, by mobility;
//   cover  — terrain_cover_factor and terrain_conceals (the combat and spotting model);
//   slope  — hex_slope (the same as the status-bar readout);
//   sight  — World::line_of_sight with the spotting model's target height.
// The planning tools (MRK-26) add the ruler's route estimate (World::route_estimate), the
// elevation profile (heights and cover the rules use, and the sight line by sight_block) and
// grid references on the map's own projected metres.
// Drawing data only: nothing here changes the simulation.
#pragma once

#include "sim.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace murks {

enum PlanMode : int {
    PLAN_OFF = 0,
    PLAN_GOING_TRACKED,
    PLAN_GOING_WHEELED,
    PLAN_GOING_FOOT,
    PLAN_COVER,
    PLAN_SLOPE,
    PLAN_SIGHT,
    PLAN_COUNT
};

const char *plan_mode_name(int mode);
// Mobility (M_TRACKED, M_WHEELED, M_FOOT) of a going mode, or -1.
int plan_mobility(int mode);

// Minutes to cross a hex off-road at full readiness in the normal move mode: the mean of the
// step cost into it from each land neighbour (terrain, river, climbing). -1 for water.
float hex_going(const World &w, int idx, int mobility);

// One RGBA8 pixel per hex (rows top to bottom), the tint the map lays over the ground for a
// whole-map mode (going, cover, slope): colour and strength. Alpha 0 leaves the ground as is.
void build_plan_rgba(const World &w, int mode, std::vector<uint8_t> &rgba);

// The colour key of a mode, in the order the panel lists it. `rgba` is the tint.
struct PlanKey {
    uint32_t rgba;
    std::string label;
    std::string note;
};
void plan_key(int mode, std::vector<PlanKey> &out);

// Line of sight from one hex: for every hex within `radius` hexes, whether a target
// `target_m` above the ground there can be seen by an eye `eye_m` above the ground at
// `from`, by World::line_of_sight. The result is a box of hexes from `origin` (offset
// coordinates), `w` x `h`, one RGBA8 tint per hex (seen, dead ground, out of range).
struct Viewshed {
    HexCoord origin{0, 0};
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    int seen = 0, total = 0; // land and water hexes within range
};
constexpr int SIGHT_RADIUS_HEXES = 24; // 12 km
void viewshed(const World &w, HexCoord from, float eye_m, float target_m, int radius, Viewshed &out);

// Tints of the line-of-sight mode, shared with the shader (outside the box = out of range).
uint32_t sight_tint(int kind); // 0 seen, 1 dead ground, 2 out of range

// --- Planning tools (MRK-26) ---

// One hex of an elevation profile: the ground and cover the rules use (heights below sea level
// count as 0, as for line of sight).
struct ProfilePoint {
    HexCoord hex;
    float km = 0;   // from the start, hex centre to hex centre
    int ground = 0; // metres
    int cover = 0;  // metres above the ground (trees, buildings)
    int terrain = 0;
};
struct Profile {
    std::vector<ProfilePoint> pts;
    // Straight lines only: World::sight_block from the first hex to the last (eye_m above the
    // ground at the start, target_m at the end): -1 clear, -2 off the map, else the index in
    // pts of the hex that blocks it. -3 for a route (no sight line).
    int block = -3;
    float eye_m = 0, target_m = 0;
    float climb_m = 0, descent_m = 0;
    float steepest_pct = 0; // the steepest hex-to-hex grade along it
};
// Along the straight line from a to b (the hexes of hex_line, the line line_of_sight follows).
void profile_line(const World &w, HexCoord a, HexCoord b, float eye_m, float target_m, Profile &out);
// Along a route: the hexes as given (the start first).
void profile_route(const World &w, const std::vector<HexCoord> &hexes, Profile &out);

// Grid on the map's own projected metres: easting = e0 + x, northing = n0 - y (EPSG:3035 on a
// baked map; a procedural map counts from its lower left corner).
void grid_origin(const Map &m, double &e0, double &n0);
// Grid reference to 100 m: the 100 km square, then 3 figures of easting and 3 of northing,
// e.g. "47/32 214 683" for easting 4,721,4xx m and northing 3,268,3xx m.
std::string grid_ref(const Map &m, double wx, double wy);
// Grid bearing from a to b in degrees (0 = grid north, clockwise), world positions.
double grid_bearing(double ax, double ay, double bx, double by);

} // namespace murks
