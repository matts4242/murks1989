#include "murks/pathfinder.hpp"

#include <algorithm>
#include <queue>

namespace murks {

void Pathfinder::attach(const Map *map) {
    map_ = map;
    size_t n = (size_t)map->hex_count();
    g_.assign(n, 0.0f);
    came_.assign(n, -1);
    stamp_.assign(n, 0);
    closed_.assign(n, 0);
    gen_ = 0;
}

PathResult Pathfinder::find(HexCoord from, HexCoord to, const StepCost &cost, float min_step_cost,
                            std::vector<int> &out_path, int max_expanded, float weight) {
    PathResult res;
    out_path.clear();
    if (!map_ || !map_->in_bounds(from) || !map_->in_bounds(to)) return res;
    if (from == to) {
        res.found = true;
        return res;
    }
    if (++gen_ == 0) { // stamp wrapped: clear once every 4 billion searches
        std::fill(stamp_.begin(), stamp_.end(), 0);
        std::fill(closed_.begin(), closed_.end(), 0);
        gen_ = 1;
    }

    struct Node {
        float f;
        int idx;
        bool operator>(const Node &o) const { return f > o.f; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    const int start = map_->index(from), goal = map_->index(to);
    g_[start] = 0;
    came_[start] = -1;
    stamp_[start] = gen_;
    open.push({weight * min_step_cost * hex_distance(from, to), start});

    while (!open.empty()) {
        Node cur = open.top();
        open.pop();
        if (closed_[cur.idx] == gen_) continue;
        closed_[cur.idx] = gen_;
        if (cur.idx == goal) {
            res.found = true;
            res.cost = g_[goal];
            break;
        }
        if (++res.expanded > max_expanded) break;

        HexCoord ch = map_->coord_of(cur.idx);
        float gcur = g_[cur.idx];
        for (int d = 0; d < 6; ++d) {
            HexCoord nh = neighbor(ch, d);
            if (!map_->in_bounds(nh)) continue;
            int ni = map_->index(nh);
            if (closed_[ni] == gen_) continue;
            float step = cost(cur.idx, ni);
            if (step < 0) continue;
            float ng = gcur + step;
            if (stamp_[ni] != gen_ || ng < g_[ni]) {
                stamp_[ni] = gen_;
                g_[ni] = ng;
                came_[ni] = cur.idx;
                open.push({ng + weight * min_step_cost * hex_distance(nh, to), ni});
            }
        }
    }

    if (res.found) {
        for (int i = goal; i != start; i = came_[i]) out_path.push_back(i);
        std::reverse(out_path.begin(), out_path.end());
    }
    return res;
}

} // namespace murks
