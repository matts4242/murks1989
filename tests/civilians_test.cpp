// Civilians (work board MRK-34): traffic, services, danger, congestion, collateral, loyalty,
// save/load, and that civilian dice never shift the military ones. Exit code 0 = all passed.
#include "murks/sim.hpp"

#include <cstdio>
#include <string>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

static void ticks(World &w, int n) {
    for (int i = 0; i < n; ++i) w.tick();
}

static int count_kind(const World &w, bool services) {
    int n = 0;
    for (const auto &c : w.civilians)
        if (c.alive && civ_is_service(c.kind) == services) ++n;
    return n;
}

int main() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0);

    std::printf("Traffic:\n");
    World quiet = w; // the same world with nobody fighting (below)
    ticks(w, 180);
    check(count_kind(w, false) > 0, "traffic is on the roads after 3 h");
    check(count_kind(w, true) > 0, "towns of 5,000+ have their services parked");
    int on_road = 0, moving = 0;
    for (const auto &c : w.civilians) {
        if (!c.alive || civ_is_service(c.kind)) continue;
        if (c.moving()) ++moving;
        if (w.map.at(c.pos).features & (F_ROAD | F_MAJOR_ROAD)) ++on_road;
    }
    check(moving > 0 && on_road * 10 >= count_kind(w, false) * 7, "most traffic is moving and on roads");
    bool all_near = true;
    for (const auto &c : w.civilians) {
        bool near = false;
        for (const auto &u : w.units)
            if (u.alive && hex_distance(u.pos, c.pos) <= 90) near = true;
        all_near = all_near && near;
    }
    check(all_near, "civilians only exist near the military units");
    {
        const int id = w.civilians.empty() ? -1 : w.civilians[0].id;
        check(id >= 0 && w.civilian(id) != nullptr && w.civilian(123456) == nullptr, "found by id");
    }
    {
        // Trips end: a vehicle seen now is mostly gone 6 h later.
        std::vector<int> ids;
        for (const auto &c : w.civilians)
            if (!civ_is_service(c.kind) && c.moving()) ids.push_back(c.id);
        World later = w;
        ticks(later, 360);
        int still = 0;
        for (int id : ids)
            if (later.civilian(id)) ++still;
        check(!ids.empty() && still * 4 < (int)ids.size(), "trips end: most vehicles have arrived after 6 h");
    }

    std::printf("Not a side:\n");
    {
        bool targeted = false;
        World f = w;
        for (int t = 0; t < 600; ++t) {
            f.tick();
            for (const auto &u : f.units)
                if (u.last_target >= (int)f.units.size()) targeted = true;
        }
        check(!targeted, "no unit ever takes a civilian as its target");
        check(f.units.size() == w.units.size(), "civilians are never units");
    }

    std::printf("Danger:\n");
    {
        World d = w;
        // A firefight next to a driving vehicle: put a hostile tank platoon beside it and one of
        // ours in range, then watch the vehicle.
        int cid = -1;
        for (const auto &c : d.civilians)
            if (!civ_is_service(c.kind) && c.moving() && c.path.size() - c.path_i > 6) {
                cid = c.id;
                break;
            }
        check(cid >= 0, "a vehicle on a longer trip");
        if (cid >= 0) {
            d.sandbox = true;
            const HexCoord at = d.civilian(cid)->pos;
            const int h = d.sandbox_place(SIDE_HOSTILE, World::SB_TANK, {at.col + 1, at.row});
            const int o = d.sandbox_place(SIDE_PLAYER, World::SB_TANK, {at.col + 4, at.row});
            d.sandbox = false;
            bool stopped = false, turned = false;
            for (int t = 0; t < 30 && d.civilian(cid); ++t) {
                d.tick();
                const Civilian *c = d.civilian(cid);
                if (c && c->state == CS_PULLED_OVER) stopped = true;
                if (c && c->fleeing) turned = true;
            }
            check(h >= 0 && o >= 0 && (stopped || turned || !d.civilian(cid)),
                  "a vehicle next to a firefight pulls over, turns away or is caught in it");
        }
    }

    std::printf("Congestion:\n");
    {
        World c = w;
        // A road hex with traffic costs a column more to enter than the same hex empty.
        int from = -1, to = -1;
        for (const auto &v : c.civilians)
            if (v.alive && v.path_i > 1 && c.road_step(v.path[v.path_i - 2], c.map.index(v.pos))) {
                to = c.map.index(v.pos);
                from = v.path[v.path_i - 2]; // path[path_i - 1] is where it stands
                break;
            }
        check(to >= 0, "a vehicle on a road");
        if (to >= 0) {
            const Unit &u = c.units[1];
            const float busy = c.move_cost(u, from, to);
            const float plan = c.move_cost(u, from, to, true);
            const int n = c.civilians_at(to);
            c.civilians_on = false;
            c.tick();
            const float empty = c.move_cost(u, from, to);
            check(n > 0 && busy > empty * 1.2f && busy <= empty * 2.01f, "driving into it costs 25% more a vehicle, at most double");
            check(plan <= c.move_cost(u, from, to, true) * 1.0001f, "routes are planned on empty roads");
        }
    }

    std::printf("Collateral and loyalty:\n");
    {
        World k = w;
        k.sandbox = true;
        // A civilian car in the home town, a hostile tank platoon in the same hex, our battery firing on it.
        int home = -1;
        for (int i = 0; i < (int)k.holdings.size(); ++i)
            if (k.holdings[i].owner == SIDE_PLAYER && k.holdings[i].kind == HK_TOWN) home = i;
        const HexCoord at = k.holdings[home].pos;
        const float loyalty0 = k.holdings[home].loyalty;
        const int lost0 = k.stats().civ_lost_player;
        int placed = 0;
        for (int i = 0; i < 12; ++i)
            if (k.sandbox_place_civilian(CV_CAR, at) >= 0) ++placed;
        for (auto &c : k.civilians)
            if (c.pos == at) c.state = CS_PULLED_OVER, c.wait_until = k.time_min + 600; // keep them there
        // The hostile platoon goes into the cars' hex itself, our tanks next door (adjacent hexes
        // always see each other).
        const int h = k.sandbox_place(SIDE_HOSTILE, World::SB_RIFLE, at);
        k.units[h].pos = at;
        const int o = k.sandbox_place(SIDE_PLAYER, World::SB_TANK, {at.col + 1, at.row});
        k.sandbox = false; // the fight runs under the normal rules
        for (int t = 0; t < 240 && k.stats().civ_lost_player == lost0; ++t) k.tick();
        k.sandbox = true;
        check(placed == 12 && h >= 0 && o >= 0 && hex_distance(k.units[o].pos, at) <= 1,
              "12 cars and a hostile platoon in our home town, our tanks next door");
        check(k.stats().civ_lost_player > lost0, "our fire on the town destroys civilian vehicles");
        check(k.holdings[home].loyalty < loyalty0, "and the town holds it against us");
        // Removing a civilian in the sandbox.
        int id = -1;
        for (const auto &c : k.civilians)
            if (c.alive) id = c.id;
        check(id >= 0 && k.sandbox_remove_civilian(id) && !k.civilian(id), "sandbox: a civilian can be deleted");
    }

    std::printf("Services:\n");
    {
        World s = w;
        // A hit near a town with services; then quiet: its services drive out, work the scene and go home.
        int town = -1;
        HexCoord at{};
        for (const auto &c : s.civilians)
            if (c.alive && c.kind == CV_FIRE && c.state == CS_PARKED) {
                town = c.home;
                at = s.map.settlements[town].pos;
                break;
            }
        check(town >= 0, "a parked fire engine");
        if (town >= 0) {
            s.sandbox = true;
            const HexCoord scene{at.col + 4, at.row + 2};
            const int h = s.sandbox_place(SIDE_HOSTILE, World::SB_RIFLE, scene);
            const int o = s.sandbox_place(SIDE_PLAYER, World::SB_TANK, {s.units[h].pos.col + 1, s.units[h].pos.row});
            // Let them fight for a while, then take both off so it goes quiet.
            bool fought = false;
            for (int t = 0; t < 60; ++t) {
                s.tick();
                if (s.units[o].last_fired > 0 || s.units[h].last_fired > 0 || s.units[h].strength < 100) fought = true;
            }
            s.sandbox_remove(h);
            s.sandbox_remove(o);
            bool responded = false, scene_worked = false, home_again = false;
            for (int t = 0; t < 360; ++t) {
                s.tick();
                for (const auto &c : s.civilians) {
                    if (c.home != town || c.kind != CV_FIRE) continue;
                    if (c.state == CS_RESPONDING) responded = true;
                    if (c.state == CS_AT_SCENE) scene_worked = true;
                    if (scene_worked && c.state == CS_PARKED) home_again = true;
                }
            }
            check(fought, "a firefight near the town");
            check(responded && scene_worked, "after it goes quiet the fire engine drives out and works the scene");
            check(home_again, "and goes home");
        }
    }

    std::printf("Separate dice:\n");
    {
        // Two runs, civilians on and off, on a quiet start: the military rolls drawn are the same
        // up to the first time traffic and units share a hex in a way that matters. Here only
        // the first 30 minutes, before anyone fights.
        World a = quiet, b = quiet;
        b.civilians_on = false;
        ticks(a, 30);
        ticks(b, 30);
        bool same = true;
        for (size_t i = 0; i < a.units.size(); ++i)
            if (a.units[i].pos != b.units[i].pos || a.units[i].morale != b.units[i].morale) same = false;
        check(same, "creating civilians does not change what the units do (first 30 min)");
    }

    std::printf("Save/load:\n");
    {
        std::vector<uint8_t> s = w.save_state();
        World a = w;
        ticks(a, 240);
        std::vector<uint8_t> after = a.save_state();
        World l;
        l.generate(1989, 24, 22);
        l.setup_scenario(0);
        std::string err;
        check(l.load_state(s.data(), s.size(), &err) && l.civilians.size() == w.civilians.size(), "load ok, civilians back");
        ticks(l, 240);
        check(after == l.save_state(), "replay 4 h after load is byte-identical");
        std::vector<uint8_t> old = s;
        old[7] = 'A';
        check(!l.load_state(old.data(), old.size(), &err), "a v10 save is refused");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
