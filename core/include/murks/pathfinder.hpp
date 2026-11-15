// Murks 1989 — A* on the hex grid.
//
// Scratch arrays cover the whole map and are reset with a generation stamp, so
// a search costs nothing to start no matter how big the map is.
#pragma once

#include "map.hpp"

#include <functional>
#include <vector>

namespace murks {

// Cost in minutes (or any unit) to step from one hex index into a neighbour.
// Return a negative value for "impassable".
using StepCost = std::function<float(int from_idx, int to_idx)>;

struct PathResult {
    bool found = false;
    float cost = 0;
    int expanded = 0;
};

class Pathfinder {
public:
    void attach(const Map *map);
    const Map *attached() const { return map_; }

    // Fills out_path with hex indices from the step after `from` up to `to`.
    // `min_step_cost` must not exceed the cheapest possible step, so the
    // heuristic stays admissible; `weight` > 1 trades optimality for speed.
    PathResult find(HexCoord from, HexCoord to, const StepCost &cost, float min_step_cost, std::vector<int> &out_path,
                    int max_expanded = 600000, float weight = 1.0f);

private:
    const Map *map_ = nullptr;
    std::vector<float> g_;
    std::vector<int> came_;
    std::vector<uint32_t> stamp_;
    std::vector<uint32_t> closed_;
    uint32_t gen_ = 0;
};

} // namespace murks
