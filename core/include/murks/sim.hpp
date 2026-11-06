// Murks 1989 — the world simulation: one map, one clock.
//
// Fixed tick of one game-minute. Units move hex by hex along A* routes,
// orders arrive after a command delay, and spotting and combat run only
// where they matter (simulation fidelity zones):
//   hot  — within 10 km (20 hexes) of an enemy: spotting + combat every tick
//   warm — within 40 km (80 hexes): spotting every 5 ticks, no combat
//   cold — everywhere else: movement only
#pragma once

#include "generator.hpp"
#include "map.hpp"
#include "noise.hpp"
#include "pathfinder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace murks {

enum UnitType : uint8_t { U_HQ = 0, U_TANK, U_MECH, U_INFANTRY, U_RECON, U_ARTILLERY, U_TYPE_COUNT };
enum Mobility : uint8_t { M_TRACKED = 0, M_WHEELED, M_FOOT };
enum Side : int8_t { SIDE_PLAYER = 0, SIDE_HOSTILE = 1, SIDE_COUNT = 2 };
enum OrderKind : uint8_t { O_HOLD = 0, O_MOVE, O_RETREAT, O_REGROUP, O_REST };
// How a unit moves (Flashpoint-style move orders).
enum MoveMode : uint8_t {
    MV_HASTY = 0,   // normal speed, fires on the move at a penalty
    MV_ROAD,        // road march: fast on roads, slow off them; poor shooting, bunched-up target
    MV_DELIBERATE,  // slow, stays concealed and shoots as if halted
    MV_ASSAULT,     // a bit slower, presses on: breaks contact only at heavier losses
};
// Rules of engagement.
enum Roe : uint8_t { ROE_FREE = 0, ROE_RETURN, ROE_HOLD };
// Standing order: when to break contact.
enum BreakContact : uint8_t { BC_HEAVY = 0, BC_LIGHT, BC_NEVER };
enum Zone : uint8_t { Z_COLD = 0, Z_WARM, Z_HOT };

struct UnitTypeInfo {
    const char *name;
    Mobility mobility;
    int spot_range;      // hexes, daylight
    int weapon_range;    // hexes
    float firepower;
    float anti_armor;    // multiplier against armoured targets
    bool armored;
    bool indirect;       // fires at anything its side can see
    int fire_interval;   // minutes between shots
    float fuel_per_hex;  // % of tank per hex moved
    float fuel_litres;   // litres for a full tank across the platoon
    float ammo_tonnes;   // tonnes for a full load
    int crew;
    int rounds_per_cycle; // aimed rounds each element fires per fire cycle (direct fire)
    float ammo_per_round; // % of the load per round fired; 0 = the flat per-cycle use
};
const UnitTypeInfo &unit_type_info(int type);

struct PendingOrder {
    bool active = false;
    OrderKind kind = O_HOLD;
    HexCoord target;
    int64_t issued_at = 0;
    int64_t activates_at = 0;
    MoveMode mode = MV_HASTY;
};

// Rules of engagement and break-contact changes also travel down the chain of command.
// Stand-off: how close a unit lets known enemies come. 0 = stay and fight; 1..20 = keep that
// many hexes away; STANDOFF_WEAPONS = at its own weapon range; STANDOFF_DEFAULT = company default.
constexpr uint8_t STANDOFF_WEAPONS = 254;
constexpr uint8_t STANDOFF_DEFAULT = 255;

struct PendingStance {
    bool active = false;
    uint8_t roe = ROE_FREE, brk = BC_HEAVY;
    int64_t activates_at = 0;
    uint8_t standoff = STANDOFF_DEFAULT;
};

struct Unit {
    int id = 0;
    int8_t side = SIDE_PLAYER;
    UnitType type = U_INFANTRY;
    std::string name;
    std::string equipment;
    int formation = 0;
    int slot = 0;

    HexCoord pos, prev_pos, dest;
    std::vector<int> path; // hex indices
    size_t path_i = 0;
    float progress = 0, step_cost = 1;
    OrderKind order = O_HOLD;
    PendingOrder pending;
    MoveMode mode = MV_HASTY;
    uint8_t roe = ROE_FREE;
    uint8_t brk = BC_HEAVY;
    PendingStance pending_stance;
    bool resting = false;          // rest & resupply: faster refill and recovery near the HQ
    int64_t last_targeted = -1000; // last time anyone fired at this unit

    // Elements: the vehicles, guns or squads that make up the unit. Each is either in the fight
    // or knocked out; strength is 100 x elements / elements_max, kept for the existing rules.
    int elements_max = 1, elements = 1;
    int acquired = -1; // target this unit has acquired and is laid on (-1 none)
    uint8_t standoff = STANDOFF_DEFAULT; // see STANDOFF_*
    int64_t standoff_next = 0;   // next stand-off check (every few minutes)
    int group = -1;              // team (Group index), -1 none
    bool follow = false;         // team member keeping formation on its leader
    bool formation_wait = false; // this tick: hold position so the formation stays together
    float strength = 100, morale = 80, fuel = 100, ammo = 100, readiness = 100;
    Zone zone = Z_COLD;
    int64_t hot_until = -1;
    int fire_cooldown = 0;
    int64_t last_fired = -1000, last_hit = -1000;
    int last_target = -1;
    // Guns fire from one position for 15-20 minutes, then displace before counter-battery fire
    // finds them. fire_pos_since = first shot from the present position (-1 none).
    int64_t fire_pos_since = -1;
    int displace_after = 0; // minutes, drawn when the first shot from a position is fired
    bool alive = true;
    bool out_of_fuel_reported = false;

    int64_t seen_until[SIDE_COUNT] = {-1, -1};
    int64_t last_seen[SIDE_COUNT] = {-100000, -100000};
    HexCoord last_seen_pos[SIDE_COUNT];
    int64_t ai_next = 0;

    bool moving() const { return path_i < path.size(); }
};

// Time a unit needs to acquire a target and to work through each engagement cycle. Crew
// condition does not change how many vehicles fight (that is the binary in-the-fight count);
// it changes how fast they find, lay on and engage targets.
struct EngagementTime {
    float base_acquire = 1, base_cycle = 3; // minutes: detect, identify and lay / each fire cycle
    float f_readiness = 1; // tired crews are slower: 1 + (100 - readiness) / 100
    float f_morale = 1;    // shaken crews hesitate: 1 + max(0, 60 - morale) / 60
    float f_ammo = 1;      // below a quarter load, crews pick shots and reload from reserves: up to x2
    float f_fire = 1;      // under fire in the last 5 minutes: x1.3
    float f_night = 1;     // at night: x1.2 (time to first round rose 10-30% at night, ARI 1978)
    float factor = 1;      // product of the factors
    float acquire = 1, cycle = 3; // minutes after the factors
};

// Teams: subordinate groups under the company. A team has a leader (an HQ or a platoon
// commander) who commands it within a zone; members follow the leader in a formation.
enum FormationShape : uint8_t { FS_KEEP = 0, FS_COLUMN, FS_LINE, FS_WEDGE, FS_COUNT };
constexpr int HQ_COMMAND_ZONE = 8;     // hexes (4 km): a headquarters' zone of effective command
constexpr int LEADER_COMMAND_ZONE = 2; // hexes (1 km): a company area, about 0.9 km centre to corner

struct Group {
    bool alive = false;
    std::string name;
    int8_t side = SIDE_PLAYER;
    int formation = 0;           // the company it belongs to
    int leader = -1;
    std::vector<int> members;    // without the leader
    std::vector<float> keep_dx, keep_dy; // "keep layout": each member's offset from the leader, metres
    uint8_t shape = FS_KEEP;
    int spacing = 2;             // hexes between formation slots
    uint8_t color = 0;
    bool has_dest = false;       // members' slots were last set around this leader destination
    HexCoord dest;
    float heading = 0;           // radians, direction of travel the slots are laid out on
    std::vector<int> trail;      // hexes the leader has driven through, oldest first (capped)
};

struct Formation {
    std::string name;
    int8_t side = SIDE_HOSTILE;
    int objective = -1;      // settlement index
    bool hunts_player = false;
    int64_t dwell_until = 0;
    std::vector<int> members;
    // Hunting formations chase the player's last known position.
    bool has_target = false;
    HexCoord target;
    int64_t target_time = -100000; // when that position was true
    int64_t intel_next = 0;        // next intelligence report on the company's whereabouts
    int start_combat = 0;          // combat units at the start, for the victory check
};

enum Outcome : uint8_t { OUT_ONGOING = 0, OUT_VICTORY, OUT_DEFEAT };

enum EventKind : uint8_t {
    EV_INFO = 0,
    EV_CONTACT,
    EV_ORDER,
    EV_ARRIVED,
    EV_DESTROYED,
    EV_RETREAT,
    EV_SUPPLY,
    EV_UNDER_FIRE,
};

struct Event {
    int64_t time;
    EventKind kind;
    int unit;
    bool pause_worthy;
    std::string text;
};

struct CompanyStock {
    double diesel_l = 30000, ammo_t = 60, food_kg = 6000;
    int personnel = 0;
};

struct SimStats {
    double avg_tick_us = 0, max_tick_us = 0;
    int hot = 0, warm = 0, cold = 0, alive = 0;
    int64_t ticks = 0;
    int los_checks_last_tick = 0;
    int paths_last_tick = 0;
    int kills = 0;  // hostile units destroyed
    int losses = 0; // own units destroyed
};

class World {
public:
    World();

    GenStats generate(uint32_t seed, int chunks_x, int chunks_y);
    // Loads a baked map (MURKSMAP bytes). Returns false if the data is invalid.
    bool load_map(const uint8_t *data, size_t size);
    void setup_scenario(int extra_hostile_groups);

    // Runs up to `ticks` minutes, stopping early once `budget_ms` is spent or right after a
    // tick that raised a pause-worthy event (so fast-forward never runs past a contact).
    int advance(int ticks, double budget_ms = 1e12);
    void tick();

    // Orders go through the command delay; returns the delay in minutes, or -1.
    int order_move(int unit_id, HexCoord target, MoveMode mode = MV_HASTY);
    int order_hold(int unit_id);
    int order_fall_back(int unit_id);              // away from known enemies, else back to the HQ
    int order_regroup(int unit_id, bool rest);     // to the company HQ; rest = rest & resupply there
    int set_stance(int unit_id, int roe, int brk); // rules of engagement / break contact
    int set_standoff(int unit_id, int standoff);   // per-unit stand-off (through the command delay)
    int standoff_hexes(const Unit &u) const;       // effective stand-off in hexes, 0 = stay and fight
    uint8_t default_standoff = 0;                  // company default stand-off (player side)
    int command_delay(const Unit &u) const;

    // Teams. create_group returns the team id or -1; the leader must be one of ids.
    int create_group(const std::vector<int> &ids, int leader);
    bool disband_group(int gid);
    bool set_group_shape(int gid, int shape, int spacing);
    int order_group_move(int gid, HexCoord target, MoveMode mode = MV_HASTY); // leader moves, team follows
    int order_group_hold(int gid);                                           // leader halts, team forms up on it
    void detach(int unit_id); // a member given its own order leaves the formation until the next team order
    int hq_above(const Unit &u) const;      // the HQ or team leader this unit takes orders from, -1 none
    int command_zone(const Unit &hq) const; // zone of effective command in hexes
    std::vector<Group> groups;

    std::vector<Event> drain_events();
    void visible_hexes(int unit_id, std::vector<HexCoord> &out) const;
    bool player_in_hot_zone() const;
    // True when the company is actually fighting: an own unit fired, was fired at or hit in the
    // last 15 minutes, or a hostile is in sight within 5 km. Drives the time-compression cap.
    bool player_in_contact() const;
    Outcome outcome() const { return outcome_; }
    std::string outcome_text() const { return outcome_text_; }
    int strike_formation() const; // the hunting formation (the scenario's victory target), or -1
    // Effective (alive, strength >= 30%) combat units of a formation; HQ and guns don't count.
    int effective_combat_units(int formation) const;

    // Quick save: the whole simulation state except the map (saved with the map name to check).
    std::vector<uint8_t> save_state() const;
    bool load_state(const uint8_t *data, size_t size, std::string *error = nullptr);
    bool is_night() const;
    std::string time_string() const { return time_string_at(time_min); }
    static std::string time_string_at(int64_t minutes);
    std::string place_name(HexCoord h) const; // "in Korec" or "8 km E of Wolze"
    bool seen_by(const Unit &u, int side) const { return time_min < u.seen_until[side]; }

    // Minutes for one hex step. `planning` is the route-finding cost: a road march weighs
    // off-road hexes heavily there so routes stick to roads, but off-road steps are then
    // driven at the normal rate.
    float move_cost(const Unit &u, int from_idx, int to_idx, bool planning = false) const;
    bool road_step(int from_idx, int to_idx) const; // both hexes joined by a road edge
    bool line_of_sight(HexCoord a, HexCoord b, float eye, float target) const;
    int hq_of(int side) const;
    // Losing an HQ: the trains keep running, but someone has to take them over. After a
    // handover of HANDOVER_MIN minutes the senior surviving platoon (first in the order of
    // battle still able to fight) acts for the HQ: resupply and regrouping go to it, at
    // ACTING_SUPPLY_RATE of the normal rate. Command delays don't change (+25 min still applies).
    static constexpr int HANDOVER_MIN = 60;
    static constexpr float ACTING_SUPPLY_RATE = 0.5f;
    // The formation's HQ if alive, else the acting commander once the handover is done at time
    // `at` (default now), else -1.
    int acting_hq(int formation, int64_t at = -1) const;
    // Minutes until the handover is done (0 if done or the HQ is alive, -1 if there is no one).
    int handover_minutes_left(int formation) const;
    static int element_count(const Unit &u); // from the equipment ("4x T-55A", "35 men") or the type
    EngagementTime engagement_time(const Unit &u) const;
    // Radio range in hexes before orders need a courier: vehicle sets (R-123) 20 km, manpack
    // sets (R-107) for units on foot 6 km.
    static int radio_range(const Unit &u);
    // Nearest enemy this unit's side knows about (seen now or in the last 30 minutes) within
    // `radius` hexes, by its last known position; -1 none. Sets *dist when found.
    int nearest_known_enemy(const Unit &u, int radius, int *dist = nullptr) const;
    // Doctrinal deployment distances from the nearest known enemy (Soviet norms, Isby 1981).
    static constexpr int DEPLOY_COLUMN_HEXES = 10; // road march -> combat movement at 5 km (4-6 km)
    static constexpr int DEPLOY_LINE_HEXES = 3;    // a team in column -> line at 1.5 km
    // Direct-fire hit weight, calibrated with tests/attrition_test so tank against tank at 1.5 km in
    // the open kills about 0.11 tanks per firing tank-minute (Historical Values doc).
#ifndef MURKS_DIRECT_FIRE_WEIGHT
#define MURKS_DIRECT_FIRE_WEIGHT 0.068f
#endif
    static constexpr float DIRECT_FIRE_WEIGHT = MURKS_DIRECT_FIRE_WEIGHT;
    bool reachable(HexCoord a, HexCoord b) const; // same connected piece of land
    const SimStats &stats() const { return stats_; }
    void reseed_rng(uint64_t s) { rng_.reseed(s * 31ull + 7); } // for tests and harnesses

    Map map;
    std::vector<Unit> units;
    std::vector<Formation> formations;
    CompanyStock stock;
    int64_t time_min = 0; // minutes since 1989-04-14 00:00
    uint32_t seed = 1989;
    int player_home = -1; // settlement index

private:
    void attach_map();
    void label_land_regions();
    void rebuild_buckets();
    void update_zones();
    void update_spotting(bool full_pass);
    void update_orders();
    void update_movement();
    void update_groups();
    void update_standoff();
    HexCoord slot_for(const Group &g, int k, HexCoord center, float heading) const;
    void deploy_if_off_road(Unit &u);
    void update_combat();
    void displace_battery(Unit &u);
    void update_morale_and_sop();
    void update_ai();
    void update_logistics();
    void update_hunters();
    void check_outcome();
    void push_event(EventKind k, int unit, bool pause, const std::string &text);
    void start_path(Unit &u, HexCoord target, OrderKind kind, bool announce);
    HexCoord regroup_point(const Unit &u) const;
    HexCoord fall_back_point(const Unit &u) const;
    int spawn(int formation, int8_t side, UnitType type, const std::string &name, const std::string &equip,
              HexCoord near, std::vector<int> &taken);
    float effective_spot_range(const Unit &u) const;
    template <typename F> void for_units_near(HexCoord h, int radius, F &&fn) const;

    Pathfinder pf_;
    std::vector<int32_t> region_; // land region per hex, -1 for water
    Rng rng_;
    std::vector<Event> events_;
    SimStats stats_;
    int bucket_cols_ = 0, bucket_rows_ = 0;
    std::vector<std::vector<int>> buckets_;
    mutable int los_checks_ = 0;
    int paths_this_tick_ = 0;
    bool alert_ = false; // a pause-worthy event was raised this tick
    Outcome outcome_ = OUT_ONGOING;
    std::string outcome_text_;
    // Who the company trains were last reported working for (-1 the HQ or no one). Only used
    // to announce a new acting commander; recomputed on load, so it is not saved.
    int trains_announced_ = -1;
    int dead_hq(int formation) const; // the formation's destroyed HQ, -1 if alive or none
};

} // namespace murks
