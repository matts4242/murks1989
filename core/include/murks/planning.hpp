// Murks 1989 — the planning map (work board MRK-25): the ground shown the way a commander
// plans on it. Every figure comes from the functions the rules themselves use, so what the
// map shows is what the simulation does:
//   going  — World::step_minutes (the pathfinder's step cost), off-road, by mobility;
//   cover  — terrain_cover_factor and terrain_conceals (the combat and spotting model);
//   slope  — hex_slope (the same as the status-bar readout);
//   sight  — World::line_of_sight with the spotting model's target height.
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

} // namespace murks
