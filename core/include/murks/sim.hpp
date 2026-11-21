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

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace murks {

// U_TRUCK: a supply column (the company trains' trucks); it carries cargo on supply routes and
// never counts as a fighting unit.
enum UnitType : uint8_t { U_HQ = 0, U_TANK, U_MECH, U_INFANTRY, U_RECON, U_ARTILLERY, U_TRUCK, U_TYPE_COUNT };
enum Mobility : uint8_t { M_TRACKED = 0, M_WHEELED, M_FOOT };
// SIDE_RIVAL only takes the field in the three-way scenario (SC_THREE_WAY): a third force at war
// with both the company and the hostiles. Every side treats every other side as the enemy.
enum Side : int8_t { SIDE_PLAYER = 0, SIDE_HOSTILE = 1, SIDE_RIVAL = 2, SIDE_COUNT = 3 };
// The scenario a game was set up with (saved with it).
//   SC_CONTRACT  - the contract: Company Murks against the 7th Guards Remnant.
//   SC_THREE_WAY - 1v1v1: the company, the 7th Guards Remnant and Kessler's Free Corps, each at
//                  war with the other two. The company wins once both of the others are broken.
enum ScenarioKind : uint8_t { SC_CONTRACT = 0, SC_THREE_WAY, SC_COUNT };
// O_DISMOUNT and O_MOUNT only travel as pending orders; they never become a unit's current order.
enum OrderKind : uint8_t { O_HOLD = 0, O_MOVE, O_RETREAT, O_REGROUP, O_REST, O_DISMOUNT, O_MOUNT, O_REJOIN };
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

// Factor on the chance of being hit for a unit in this terrain (forest 0.7 for vehicles, 0.5
// for soft targets; town 0.6 / 0.4; wetland 0.9; open ground 1).
float terrain_cover_factor(int terrain, bool soft_target);
// The step-cost tables (minutes per hex at full readiness): off-road by terrain, on a road,
// and the extra for crossing a river off-road.
float terrain_minutes(int mobility, int terrain);
float road_minutes(int mobility);
float river_penalty_minutes(int mobility);
// Slope (MRK-04): factor on a step's minutes for its grade (rise / 500 m, negative downhill).
// Vehicles: 1 on the level, rising with the climb (tracked x1.2 at 10%, x1.4 at 15%, x2.6 at
// 30%; wheeled x1.4, x1.8 and x4), held at most x10 / x20; a descent counts as a climb of 0.6
// of its grade (-10%: tracked x1.07, wheeled x1.2). Nothing is impassable. On foot,
// Tobler's hiking function (x0.84 at -5%, x1.7 at +15%, x2.9 at +30%, at most x8). A road takes
// a hill at 0.7 of its grade. The river penalty is added after the factor, unchanged.
constexpr float SLOPE_DESCENT_SHARE = 0.6f; // a vehicle's descent counts as a climb of this share
constexpr float SLOPE_ROAD_SHARE = 0.7f;    // roads wind up a hill: they take it at this share
float slope_factor(int mobility, float grade, bool road);
float slope_factor_min(int mobility); // the smallest slope_factor can be (x0.84 on foot, else 1)
// Forest and towns hide a unit that sits still (or moves deliberately) and has not fired for
// a few minutes: it is spotted only within these ranges, beyond them only by chance.
constexpr int CONCEAL_SPOT_HEXES = 2;       // 1 km
constexpr int CONCEAL_SPOT_HEXES_RECON = 4; // 2 km
inline bool terrain_conceals(int terrain) { return terrain == T_FOREST || terrain == T_URBAN; }
// Eye and target heights (metres above ground) the spotting and direct-fire model uses.
constexpr float SPOT_EYE_M = 3.0f;
constexpr float SPOT_TARGET_M = 2.5f;
// Routes (MRK-20). A move order carries up to MAX_WAYPOINTS waypoints. Each sets the move mode
// for the leg that leads to it, how long to halt there, and (for a team leader) the formation
// shape the team takes for that leg. The whole route goes down the chain as one order with one
// command delay; on reaching a waypoint the unit plans the next leg.
constexpr int MAX_WAYPOINTS = 16;
constexpr int MAX_WAYPOINT_HALT = 60; // minutes
struct Waypoint {
    HexCoord at;
    MoveMode mode = MV_HASTY; // for the leg that ends here
    int16_t halt_min = 0;     // minutes to halt here before the next leg (0-60)
    int8_t shape = -1;        // team leaders: formation shape for this leg (FormationShape), -1 keep
};
inline bool operator==(const Waypoint &a, const Waypoint &b) {
    return a.at == b.at && a.mode == b.mode && a.halt_min == b.halt_min && a.shape == b.shape;
}

// Route edits. An edit that keeps every changed waypoint within ADJUST_RADIUS_M of the old route
// and moves the final destination no more than that is an adjustment: it costs ADJUST_FACTOR of
// the normal command delay (at least ADJUST_MIN_DELAY), and the unit keeps following its old
// route until it arrives. The second adjustment within ADJUST_WINDOW minutes of the one before
// costs ADJUST_FACTOR_2, the third and later full delay. A halted unit moving 2 km or less gets
// the same rate (adjust position). A change to Assault is always a new order. Starting values
// accepted by the user on 2026-11-09 (Work Board MRK-20).
constexpr float ADJUST_RADIUS_M = 2000.0f;
constexpr float ADJUST_FACTOR = 0.5f, ADJUST_FACTOR_2 = 0.75f;
constexpr int ADJUST_MIN_DELAY = 3;
constexpr int ADJUST_WINDOW = 30;
// Flashpoint Campaigns: Cold War's order delay (FM01 sec 21, Issuing Orders), chosen by the user
// on 2026-11-11 for MRK-20: a unit already carrying out the same kind of order (Move to Move,
// just with new waypoints) takes the change in SAME_ORDER_DELAY minutes; a unit that is not
// moving and gets an order that needs movement takes START_MOVING_DELAY minutes more.
constexpr int SAME_ORDER_DELAY = 2;
constexpr int START_MOVING_DELAY = 5;

// Why a route is paused: the unit halted short of an enemy-held hex or at its stand-off line,
// and picks the route up again once that is clear.
enum RoutePause : uint8_t { RP_NONE = 0, RP_ENEMY_HEX, RP_STANDOFF };

struct PendingOrder {
    bool active = false;
    OrderKind kind = O_HOLD;
    HexCoord target;             // O_MOVE: the route's final waypoint
    int64_t issued_at = 0;
    int64_t activates_at = 0;
    MoveMode mode = MV_HASTY;
    std::vector<Waypoint> route; // O_MOVE: the waypoints, in order (one for a plain move)
    bool adjust = false;         // an adjustment of the route in progress (see ADJUST_*)
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

    HexCoord pos, prev_pos, dest; // dest: the end of the present leg
    std::vector<int> path; // hex indices, for the present leg only
    // Route: the waypoints still ahead start at wp_i. Empty when the unit has no route (holding,
    // falling back, keeping station in a team, or moved by a standing procedure).
    std::vector<Waypoint> route;
    int wp_i = 0;
    int64_t wp_halt_until = -1;   // halting at waypoint wp_i - 1 until then
    bool leg_due = false;         // the next leg still has to be planned (lazy, path budget)
    uint8_t route_pause = RP_NONE;
    HexCoord blocked_hex;         // RP_ENEMY_HEX: the hex it halted in front of
    int64_t route_check = 0;      // next check whether a paused route is clear
    int64_t last_adjust_at = -100000; // when the last adjustment was issued
    int adjust_streak = 0;            // adjustments in a row, each within ADJUST_WINDOW of the last
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
    // Suppression 0-100: fire at the unit raises it whether or not it hits; it holds while the
    // fire goes on and fades with a ~1.4-minute half-life once it stops ("usually only effective
    // for the duration of the fire"; artillery's lingers about 2 minutes). It slows engagement
    // time, and soft units at 60+ are pinned: they stop moving and spot at half range.
    float suppression = 0;
    int64_t supp_hold_until = -1; // the incoming fire goes on until then (the shooter's cycle)
    bool pinned = false;
    int64_t fire_pos_since = -1;
    int displace_after = 0; // minutes, drawn when the first shot from a position is fired
    bool alive = true;
    bool removed = false; // taken off the map in the sandbox (MRK-31): not a kill, not a loss
    bool out_of_fuel_reported = false;

    // Mounted infantry. An APC/IFV platoon (the carrier) has a squad unit of its own (one squad
    // per vehicle) that rides aboard: while embarked it is off the map (alive = false) and its
    // elements are the squads aboard. Dismounted, it is an infantry unit like any other.
    int carrier = -1;    // squad units: the vehicles they ride in
    int dismounts = -1;  // carriers: their squad unit
    bool embarked = false;
    bool mounting = false;  // squad unit walking back to its vehicles to mount up
    int64_t mount_at = -1;  // when the mount drill finishes (both halted in the same hex), -1 none
    int squad_men = 0;      // squad units: men per squad (display and ration count)

    // Holdings (see Holding). Militia are the garrison a town raises: they hold their town, take
    // no orders and never count as part of the company. A supply column (U_TRUCK) working a
    // route carries cargo and runs the convoy state machine (see SupplyRoute / ConvoyState).
    bool militia = false;
    int holding = -1;            // militia: the holding they garrison
    uint8_t convoy_state = 0;    // ConvoyState (trucks only)
    int convoy_route = -1;       // the SupplyRoute this column works, -1 none
    double cargo_l = 0, cargo_t = 0, cargo_kg = 0; // diesel, ammunition, food aboard
    int64_t convoy_wait_until = -1; // loading / unloading finishes then; or earliest resume after a halt
    int64_t trip_started = -1;      // when the column left the source with this load

    int64_t seen_until[SIDE_COUNT] = {-1, -1, -1};
    int64_t last_seen[SIDE_COUNT] = {-100000, -100000, -100000};
    HexCoord last_seen_pos[SIDE_COUNT];
    int64_t ai_next = 0;

    bool moving() const { return path_i < path.size(); }
    bool on_route() const { return wp_i < (int)route.size(); }
    // The final destination: the last waypoint of the route, else the end of the present leg.
    HexCoord final_dest() const { return route.empty() ? dest : route.back().at; }
};

// ------------------------------------------------------------------------------------
// Holdings and supply (company loop, JA2-style holdings; Shadow Empire-style routes)
// ------------------------------------------------------------------------------------

// A place the company can hold: a town (taxes, food, recruits, militia), a farm collective
// (food), a fuel depot or an ammo dump (finite stores). Every holding keeps a stock of its
// own: yields land there, requisitions go there, and the company draws from it with its
// trains parked at the place or by a supply route (convoys). Capture, loyalty and yields
// are first estimates (Work Board MRK-08).
enum HoldingKind : uint8_t { HK_TOWN = 0, HK_FARM, HK_FUEL_DEPOT, HK_AMMO_DUMP, HK_COUNT };
const char *holding_kind_name(int kind);

struct Holding {
    int settlement = -1;        // index into Map::settlements
    std::string name;           // the settlement's name
    HexCoord pos;
    int radius = 1;             // hexes: the place's footprint (units inside count as present)
    uint8_t kind = HK_TOWN;
    int8_t owner = -1;          // a Side, or -1 independent
    int8_t known_owner = -1;    // the owner as far as the player knows (fog of war)
    float loyalty = 30;         // 0-100 towards the owner; income and militia scale with it
    int population = 0;
    double diesel_l = 0, ammo_t = 0, food_kg = 0, cash = 0; // the holding's own stock
    int manpower = 0;           // men willing to serve (militia and replacements)
    int militia_unit = -1;      // the garrison unit, -1 none yet
    bool training = false;      // the owner is raising militia here
    float training_progress = 0; // 0..1 towards the next squad
    int64_t contested_since = -1; // an enemy of the owner alone in the place since (-1 no)
    int64_t captured_at = -100000;
    int64_t last_fight = -100000; // fighting inside the place (collateral damage)
    int64_t levy_until[4] = {-1, -1, -1, -1}; // next time each LevyKind is allowed
};

enum LevyKind : uint8_t { LV_FOOD = 0, LV_FUEL, LV_CASH, LV_MEN, LV_COUNT };
const char *levy_kind_name(int kind);

// A supply route: a truck column shuttles between a source holding and the company trains
// (the HQ, or whoever took them over) or another own holding. Throughput follows from
// trucks x load / round trip, so distance, roads and ambushes set what arrives.
enum ConvoyState : uint8_t { CV_IDLE = 0, CV_TO_SOURCE, CV_LOADING, CV_TO_DEST, CV_UNLOADING, CV_RETURN };
const char *convoy_state_name(int state);

struct SupplyRoute {
    bool active = false;
    int8_t side = SIDE_PLAYER;
    int source = -1;            // holding index
    int dest = -1;              // holding index, or -1 = the company trains
    int convoy = -1;            // unit id of the column working it (-1 suspended)
    int trips = 0;
    double delivered_l = 0, delivered_t = 0, delivered_kg = 0;
    int last_round_trip_min = 0; // minutes source -> destination -> source, last completed
    int64_t suspended_at = -1;   // the column was lost then (-1 never)
};

// Player map markings (work board MRK-26): objectives, phase lines, target reference points and
// notes, drawn on the map and saved with the game. The simulation never reads them.
enum MarkKind : uint8_t { MK_OBJECTIVE = 0, MK_PHASE_LINE, MK_TRP, MK_NOTE, MK_COUNT };
const char *mark_kind_name(int kind);
struct Marking {
    uint8_t kind = MK_NOTE;
    std::string name;       // "OBJ WOLF", "PL RED", "TRP 01", or the note's text
    std::vector<int> xy;    // world metres, x,y pairs: one point, or 2+ for a phase line
};
constexpr int MAX_MARKINGS = 500;
constexpr int MAX_MARK_POINTS = 64;
constexpr int MAX_MARK_NAME = 80; // bytes

// A route estimate for planning (the ruler): the best route from a to b for a mobility class
// and move mode, found the way orders find routes (same step costs, same search; hexes held
// by the enemy are ignored), and its travel time at full readiness.
struct RouteEstimate {
    bool found = false;
    float minutes = 0;     // at full readiness (a tired unit is up to 40% slower)
    float km = 0;          // along the route, hex centre to hex centre
    float road_km = 0;     // of which on roads
    std::vector<int> path; // hex indices after the start, up to the end
};

constexpr int CAPTURE_MIN = 120;          // minutes one side alone in a place before it changes hands (estimate)
constexpr int MILITIA_TRAIN_MIN = 2 * 1440; // per squad of 10 (estimate)
constexpr int MILITIA_SQUAD_MEN = 10;
constexpr int MILITIA_PER_POP = 2000;     // one squad per 2,000 people
constexpr int MILITIA_MAX_SQUADS = 6;
constexpr int LEVY_COOLDOWN_MIN = 1440;   // per holding and kind
constexpr int CONVOY_LOAD_MIN = 30;       // loading or unloading a column (estimate)
constexpr int CONVOY_RESUME_MIN = 60;     // after being driven off or halted, try the leg again
constexpr double TRUCK_LOAD_T = 4.5;      // Ural-375D payload, tonnes
constexpr double DIESEL_L_PER_T = 1190.0; // 0.84 kg per litre
constexpr double WANT_DIESEL_L = 30000, WANT_AMMO_T = 60, WANT_FOOD_KG = 6000; // the trains' want levels

// Replacements (work board MRK-10): men levied from the holdings wait in the company's pool
// (CompanyStock::replacements) and fill infantry squads that were knocked out. A rifle platoon,
// or the squads of an APC/IFV platoon (aboard or on foot), that stands at the trains (as for
// resupply: halted within 3 hexes of the HQ or acting commander, out of the fight for 30
// minutes) gets REPLACE_SQUADS_PER_HOUR whole squads back an hour, twice that while resting,
// half under an acting commander; units are served in order of battle while the pool lasts.
// An APC/IFV platoon's squads are filled up to one squad a vehicle left. Vehicles, guns and
// their crews are not replaced: men alone don't make a tank. The new men come in at
// REPLACEMENT_MORALE, so a refilled platoon's morale moves towards it in proportion. Rates and
// morale are estimates (raw volunteers with a few days' drill at most).
constexpr int REPLACE_SQUADS_PER_HOUR = 1;
constexpr float REPLACEMENT_MORALE = 50.0f;

// Reinforcements (work board MRK-07): vehicles, guns and trucks are not bought. A lost one is
// replaced only by asking the company's headquarters for reinforcements for the unit (a request
// for what the unit is short of), which arrive at the trains REINF_MIN_DELAY_MIN to
// REINF_MIN_DELAY_MIN + REINF_EXTRA_DELAY_MIN minutes later (two to three days, drawn at the
// request). They are handed over to the unit once it is at the trains (as for resupply and
// replacements), with fresh crews: morale and fuel and ammunition of the new vehicles mix into the unit's
// by their share. One request per unit at a time; a unit wiped out in the meantime loses it, and a
// platoon that has been wiped out is not raised again. The wait is the whole cost: there is no
// price. The delay and the fresh crews' morale are estimates.
constexpr int REINF_MIN_DELAY_MIN = 2 * 1440;
constexpr int REINF_EXTRA_DELAY_MIN = 1440;
constexpr float REINFORCEMENT_MORALE = 70.0f;
struct Reinforcement {
    int unit = -1;       // the unit they are for
    int count = 0;       // vehicles, guns or trucks asked for
    int64_t asked = 0;   // when the request was made
    int64_t due = 0;     // when they reach the trains
};

// Time a unit needs to acquire a target and to work through each engagement cycle. Crew
// condition does not change how many vehicles fight (that is the binary in-the-fight count);
// it changes how fast they find, lay on and engage targets.
struct EngagementTime {
    float base_acquire = 1, base_cycle = 3; // minutes: detect, identify and lay / each fire cycle
    float f_readiness = 1; // tired crews are slower: 1 + (100 - readiness) / 100
    float f_morale = 1;    // shaken crews hesitate: 1 + max(0, 60 - morale) / 60
    float f_ammo = 1;      // below a quarter load, crews pick shots and reload from reserves: up to x2
    float f_supp = 1;      // suppression: 1 + suppression / 50, up to x3 (see Unit::suppression)
    float f_night = 1;     // at night: x1.2 (time to first round rose 10-30% at night, ARI 1978)
    float factor = 1;      // product of the factors
    float acquire = 1, cycle = 3; // minutes after the factors
};

// Guns and mortars by equipment name, with their maximum range firing the standard HE round
// (no rocket-assisted or base-bleed rounds: rarely issued in 1989). A unit whose equipment names
// none of them keeps its type's weapon range.
struct GunInfo {
    const char *key;   // matched as a whole word in Unit::equipment ("2S1" does not match "2S19")
    const char *label; // "122 mm SP howitzer 2S1"
    float range_km;
};
const GunInfo *gun_info(const std::string &equipment);

// Fire support net: a radio band of its own for calls for fire, separate from the command net.
// The guns, the HQ and the recon sections (the artillery observers) are on it. Anyone else
// passes a call for fire to an observer on the net within radio range, who relays it.
enum FireNetRole : int8_t { FN_NONE = -1, FN_GUNS = 0, FN_OBSERVER = 1, FN_CALLER = 2 };
enum FireNetStatus : int8_t { FN_CUT = 0, FN_RELAY = 1, FN_DIRECT = 2 };
struct FireNetLink {
    int8_t role = FN_NONE;    // FN_NONE: the formation has no guns, so no fire net
    int8_t status = FN_CUT;   // FN_DIRECT: on the net and within radio range of the guns
    int battery = -1;         // the guns it reaches (nearest); when cut, the nearest guns anyway
    int via = -1;             // FN_RELAY: the unit on the net that passes its calls on
    int distance = 0;         // hexes to the guns (direct, cut) or to the relay (relay)
};
constexpr int FIRE_RELAY_MIN = 2; // minutes a relayed call for fire adds (estimate)

// Suppression. One fire cycle at a unit adds (100 - s) x (1 - e^(-S / SUPP_S0)), where S sums
// the shooters' elements x their weight per element and cycle: about 1 for one 155 mm round
// (relative suppression per round, CDEC/DTIC: 155 mm 1, 105 mm tank round 0.8, 81 mm 0.7,
// .50 cal 5-round burst 0.06). Armour (x0.3 direct, x0.5 indirect) and cover (x0.6) shield.
constexpr float SUPP_S0 = 3.0f;
constexpr float SUPP_DECAY = 0.6f; // per minute once the fire has stopped (half-life ~1.4 min)
constexpr float PIN_AT = 60.0f, UNPIN_AT = 35.0f;

// APCs and IFVs by equipment name: how many troops each vehicle carries. One squad per vehicle.
struct CarrierInfo {
    const char *key; // whole word in Unit::equipment, as for GunInfo
    int men;         // dismounts per vehicle
};
const CarrierInfo *carrier_info(const std::string &equipment);
constexpr int DISMOUNT_MIN = 1; // minutes from halt until the squads can fight (estimate)
constexpr int MOUNT_MIN = 2;    // minutes to load once squads and vehicles are in one hex (estimate)

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
    bool broken = false;           // hunters: announced as broken (three-way: one down, one to go)
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
    EV_HOLDING, // a holding changes hands, raises militia, is levied
};

struct Event {
    int64_t time;
    EventKind kind;
    int unit;
    bool pause_worthy;
    std::string text;
};

// Wages (MRK-07): paid with the food at 06:00 from the company's cash, per man on the rolls.
// 10 DM a day is an estimate (about 300 DM a month, a hired hand's pay, not a wage for a
// skilled man): 289 men cost ~2,900 DM a day and the starting 20,000 DM lasts a week. A company
// that cannot pay in full pays what it has and every unit's morale drops by UNPAID_MORALE_HIT.
constexpr double WAGE_DM_PER_MAN_DAY = 10.0;
constexpr float UNPAID_MORALE_HIT = 5.0f;

struct CompanyStock {
    double diesel_l = 30000, ammo_t = 60, food_kg = 6000;
    int personnel = 0;
    double cash = 20000;   // Deutsche Mark (1989 prices); wages are paid from it (WAGE_*); vehicles are not bought: see Reinforcement (MRK-07)
    int replacements = 0;  // men levied from holdings, waiting to fill squads (see REPLACE_*, MRK-10)
    // Consumption, smoothed hourly: what the trains hand out per hour. Days of supply follow.
    double rate_diesel_lph = 0, rate_ammo_tph = 0;
};

// Civilians (work board MRK-34): traffic and services on the map. Not units and not a side:
// nobody orders, targets, spots-for-combat or captures with them. They drive between towns,
// get out of the way of fighting and slow military columns on the roads they share. They are
// non-combatants and take no damage (user, 16 Nov 2026).
enum CivKind : uint8_t { CV_CAR = 0, CV_BUS, CV_TRACTOR, CV_LORRY, CV_POLICE, CV_FIRE, CV_AMBULANCE, CV_KIND_COUNT };
enum CivState : uint8_t {
    CS_DRIVING = 0,  // on a trip between towns
    CS_PARKED,       // a service waiting in its town
    CS_PULLED_OVER,  // fighting close by: stopped at the roadside
    CS_RESPONDING,   // a service on its way to a scene
    CS_AT_SCENE,     // a service working a scene
    CS_RETURNING,    // a service on its way home
};
enum CivCountry : uint8_t { CC_GDR = 0, CC_POLAND };
const char *civ_kind_name(int kind);
const char *civ_state_name(int state);
inline bool civ_is_service(int kind) { return kind >= CV_POLICE; }

struct Civilian {
    int id = -1;
    uint8_t kind = CV_CAR;
    uint8_t state = CS_DRIVING;
    uint8_t country = CC_GDR;
    bool alive = true;
    bool seen = false;        // the player sees it (own unit in range and line of sight; checked every 5 min)
    bool fleeing = false;     // turned away from fighting
    HexCoord pos, prev_pos;
    int home = -1;            // settlement it belongs to
    int dest = -1;            // settlement it drives to (-1: a scene, or home for a service)
    HexCoord target;          // where the current path ends
    std::vector<int> path;    // hex indices after pos
    int path_i = 0;
    float progress = 0, step_cost = 0;
    int64_t wait_until = 0;
    bool moving() const { return path_i < (int)path.size(); }
};

struct DangerMark {
    HexCoord at;
    int64_t until = 0;
};

// A place that was hit: services from the nearest town go there once it has been quiet.
struct Incident {
    HexCoord at;
    int64_t last_shot = 0;
    bool served = false;
};

struct SimStats {
    double avg_tick_us = 0, max_tick_us = 0;
    int hot = 0, warm = 0, cold = 0, alive = 0;
    int64_t ticks = 0;
    int los_checks_last_tick = 0;
    int paths_last_tick = 0;
    int kills = 0;  // hostile units destroyed
    int losses = 0; // own units destroyed
    int civilians = 0; // civilian vehicles on the map
};

class World {
public:
    World();

    GenStats generate(uint32_t seed, int chunks_x, int chunks_y);
    // Loads a baked map (MURKSMAP bytes). Returns false if the data is invalid.
    bool load_map(const uint8_t *data, size_t size);
    void setup_scenario(int extra_hostile_groups, int kind = SC_CONTRACT);

    // Runs up to `ticks` minutes, stopping early once `budget_ms` is spent or right after a
    // tick that raised a pause-worthy event (so fast-forward never runs past a contact).
    int advance(int ticks, double budget_ms = 1e12);
    void tick();

    // Orders go through the command delay; returns the delay in minutes, or -1.
    int order_move(int unit_id, HexCoord target, MoveMode mode = MV_HASTY);
    // A route of 1..MAX_WAYPOINTS waypoints (a plain move is a route of one). Replaces the
    // route in progress and any order still on its way. Returns the delay in minutes, or -1.
    // *adjust tells whether it went as an adjustment (see ADJUST_*).
    int order_route(int unit_id, const std::vector<Waypoint> &route, bool *adjust = nullptr);
    // What order_route would cost now, without giving it: the delay, and whether it is an
    // adjustment. -1 when the route is not valid.
    int route_delay(int unit_id, const std::vector<Waypoint> &route, bool *adjust = nullptr) const;
    // Plans one leg for the map (route preview and ETAs) without moving anyone: fills `path`
    // with hex indices and returns the minutes the unit would need at `mode`, or -1 (no route).
    float plan_leg(int unit_id, HexCoord from, HexCoord to, MoveMode mode, std::vector<int> &path);
    int order_hold(int unit_id);
    int order_fall_back(int unit_id);              // away from known enemies, else back to the HQ
    int order_regroup(int unit_id, bool rest);
    // Mounted infantry: the carrier halts and its squads get out; or the squads walk back to
    // the (halted) vehicles and mount up. Either goes through the ordered unit's command delay.
    // The mount order can go to the carrier or to its squads. -1 when not possible.
    int order_dismount(int unit_id);
    int order_mount(int unit_id);
    bool can_dismount(const Unit &u) const; // a carrier with squads aboard
    bool can_mount(const Unit &u) const;    // a carrier with squads out, or squads with vehicles left
    // Gives an APC/IFV platoon its squad unit (embarked); setup_scenario does this for every U_MECH.
    void add_dismounts(int carrier_id);     // to the company HQ; rest = rest & resupply there
    int set_stance(int unit_id, int roe, int brk); // rules of engagement / break contact
    int set_standoff(int unit_id, int standoff);   // per-unit stand-off (through the command delay)
    int standoff_hexes(const Unit &u) const;       // effective stand-off in hexes, 0 = stay and fight
    uint8_t default_standoff = 0;                  // company default stand-off (player side)
    int command_delay(const Unit &u) const;
    // Delay for an order of this kind (and, for a move, this mode) to a unit, with Flashpoint's
    // same-order and start-moving rules on top of command_delay. Route adjustments: route_delay.
    int order_delay(const Unit &u, OrderKind kind, MoveMode mode = MV_HASTY) const;
    bool same_kind_order(const Unit &u, OrderKind kind, MoveMode mode) const;
    // Is this route an adjustment of the unit's route in progress (see ADJUST_*)?
    bool is_adjustment(const Unit &u, const std::vector<Waypoint> &route) const;

    // Teams. create_group returns the team id or -1; the leader must be one of ids.
    int create_group(const std::vector<int> &ids, int leader);
    bool disband_group(int gid);
    bool set_group_shape(int gid, int shape, int spacing);
    int order_rejoin(int unit_id);                                            // a detached member takes its place again (command delay)
    int order_group_move(int gid, HexCoord target, MoveMode mode = MV_HASTY); // leader moves, team follows
    int order_group_hold(int gid);                                           // leader halts, team forms up on it
    int order_group_route(int gid, const std::vector<Waypoint> &route, bool *adjust = nullptr); // leader's route, team follows
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
    int strike_formation() const; // the (first) hunting formation (the scenario's victory target), or -1
    // Every hunting formation: the contract's one, or the three-way's two (hostile, then rival).
    std::vector<int> hunter_formations() const;
    // How event texts name a side's units: "hostile", or in the three-way "rival" for SIDE_RIVAL.
    const char *side_word(int side) const;
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
    // The same step cost for a mobility and move mode, with the road choice made by the caller
    // (move_cost passes road_step). Used by the planning map's going mode.
    float step_minutes(int mobility, int move_mode, int from_idx, int to_idx, bool road, bool planning = false) const;
    bool road_step(int from_idx, int to_idx) const; // both hexes joined by a road edge
    bool line_of_sight(HexCoord a, HexCoord b, float eye, float target) const;
    // The hex that blocks that sight line: its index along hex_line(a, b) (1 .. n-1), -1 if the
    // line is clear, -2 if it leaves the map. line_of_sight is sight_block(...) == -1.
    int sight_block(HexCoord a, HexCoord b, float eye, float target) const;
    // Planning (MRK-26): see RouteEstimate. Uses the order pathfinder's scratch space, so it is
    // not const, but it changes no simulation state.
    RouteEstimate route_estimate(HexCoord a, HexCoord b, int mobility, int move_mode);
    // Markings (MRK-26). add_marking: the index, or -1 if the kind, points or count are bad.
    int add_marking(const Marking &m);
    bool rename_marking(int i, const std::string &name);
    bool remove_marking(int i);
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
    // Weapon range in hexes: guns by their equipment (gun_info), everything else by type.
    static int weapon_range(const Unit &u);
    // Fire support net (see FireNetLink). Members: guns, HQs and recon.
    static bool on_fire_net(const Unit &u) { return u.type == U_ARTILLERY || u.type == U_HQ || u.type == U_RECON; }
    // Two radios reach each other out to the shorter of their ranges.
    static int net_range(const Unit &a, const Unit &b) { return std::min(radio_range(a), radio_range(b)); }
    FireNetLink fire_net_link(const Unit &u) const;
    // Can anyone in the battery's formation call fire on this target? -1 no one who can see it
    // can reach the guns; 0 an observer on the net (or the guns themselves) sees it; 1 it is
    // seen by someone whose call is relayed (FIRE_RELAY_MIN more). *observer: who sees it.
    int fire_call(const Unit &battery, const Unit &target, int *observer = nullptr) const;
    bool observes(const Unit &o, const Unit &t) const; // o has eyes on t now (range and sight line)
    // Suppression one element of this shooter puts on a soft target in the open per fire cycle.
    static float suppression_weight(const Unit &shooter);
    float effective_spot_range(const Unit &u) const; // hexes: night and being pinned cut it
    // Adds fire worth S suppression units at u, with the fire going on until hold_until.
    static void suppress(Unit &u, float S, int64_t hold_until);
    // Nearest enemy this unit's side knows about (seen now or in the last 30 minutes) within
    // `radius` hexes, by its last known position; -1 none. Sets *dist when found.
    int nearest_known_enemy(const Unit &u, int radius, int *dist = nullptr) const;
    // Doctrinal deployment distances from the nearest known enemy (Soviet norms, Isby 1981).
    static constexpr int DEPLOY_COLUMN_HEXES = 10; // road march -> combat movement at 5 km (4-6 km)
    static constexpr int DEPLOY_LINE_HEXES = 3;    // a team in column -> line at 1.5 km
    // Direct-fire hit weight, calibrated with tests/attrition_test so tank against tank at 1.5 km in
    // the open kills about 0.11 tanks per firing tank-minute (Historical Values doc).
#ifndef MURKS_DIRECT_FIRE_WEIGHT
#define MURKS_DIRECT_FIRE_WEIGHT 0.073f
#endif
    static constexpr float DIRECT_FIRE_WEIGHT = MURKS_DIRECT_FIRE_WEIGHT;
    bool reachable(HexCoord a, HexCoord b) const; // same connected piece of land
    const SimStats &stats() const { return stats_; }
    void reseed_rng(uint64_t s) { rng_.reseed(s * 31ull + 7); } // for tests and harnesses

    // Holdings. holding_at: the holding whose footprint holds the hex, -1 none. in_place: the
    // unit stands inside the holding's footprint.
    int holding_at(HexCoord h) const;
    bool in_place(const Holding &h, HexCoord pos) const { return hex_distance(h.pos, pos) <= h.radius; }
    // Units of `side` present in the place (alive, on the map, not trucks); militia count for
    // their owner.
    int present_in(const Holding &h, int side) const;
    // The day's wage bill (DM) at the present headcount, and the days the cash covers it
    // (1e9 with no bill). Income from holdings is not counted: it is what the trains collect.
    double wage_bill() const { return stock.personnel * WAGE_DM_PER_MAN_DAY; }
    double days_of_cash() const { return wage_bill() > 0 ? stock.cash / wage_bill() : 1e9; }
    // Daily yield into the holding's own stock at its present loyalty (cash DM, food kg).
    void daily_yield(const Holding &h, double &cash, double &food_kg) const;
    int militia_max_squads(const Holding &h) const;
    int militia_squads(const Holding &h) const; // squads in the fight now
    // Raise militia at an own town (on/off). false if the holding can't (not a town, not owned,
    // loyalty under 25).
    bool set_training(int hid, bool on);
    // Requisition at an own town with an own unit present: moves goods into the holding's stock
    // (men into the company's replacement pool) at a loyalty cost. Returns "" on success, else
    // the reason.
    std::string levy(int hid, int what);
    // Does `side` have a unit (not militia, not a truck) halted inside the place?
    bool can_levy(const Holding &h, int side) const;
    // Supply routes (player side). set_supply_route: source holding -> dest holding (or -1 = the
    // trains), worked by a free supply column; returns the route id or -1 (reason in *error).
    int set_supply_route(int source, int dest, std::string *error = nullptr);
    bool cancel_supply_route(int rid);
    int free_convoy(int side) const;    // an idle supply column of this side, -1 none
    double convoy_capacity_t(const Unit &u) const { return u.elements * TRUCK_LOAD_T; }
    // Days of supply at the smoothed consumption (1e9 when nothing is being used).
    double days_of_diesel() const;
    double days_of_ammo() const;
    // Sandbox (MRK-31): the scenario with tools on top. No victory or defeat; units can be put
    // on the map and taken off for either side; the start time can be changed until the clock
    // first moves; the hostile AI can be switched off (hostiles then make no moves of their own,
    // but still fire by their rules of engagement and rearm). All saved with the game.
    enum SandboxKind { SB_TANK = 0, SB_MECH, SB_RIFLE, SB_RECON, SB_BATTERY, SB_HQ, SB_SUPPLY, SB_KIND_COUNT };
    bool sandbox = false;
    bool hostile_ai = true; // also drives the rival side in the three-way
    uint8_t scenario = SC_CONTRACT; // ScenarioKind
    bool clock_started() const { return clock_started_; }
    // Puts a unit of this kind for this side on the nearest free land hex to `at`. Own units join
    // the company; hostile ones a "Sandbox Hostiles" formation with no objective (they react to
    // what they see, otherwise hold). APC/IFV platoons get their squads. Returns the id or -1.
    int sandbox_place(int side, int kind, HexCoord at);
    // Takes a unit off the map, with a carrier's squads (or squads' vehicles' link), out of its
    // team (a removed leader disbands it), ending a removed supply column's route. Not a kill
    // or a loss. False when not in the sandbox or the unit is already gone.
    bool sandbox_remove(int unit_id);
    // Day 14..20 (April 1989) and hour 0..23; only in the sandbox and before the clock moves.
    bool set_start_time(int day, int hour);
    int sandbox_hostile_formation() const; // -1 until a hostile is placed

    double days_of_food() const;

    // Civilians (MRK-34).
    std::vector<Civilian> civilians;
    bool civilians_on = true;               // off: none are created (tests, harnesses)
    const Civilian *civilian(int id) const; // by id, nullptr if gone
    int civilians_at(int hex_idx) const;
    bool player_hears_of(HexCoord at, int town) const; // a civilian report reaches the player (MRK-34)    // civilian vehicles in a hex (congestion)
    // Sandbox: puts a civilian of this kind on the nearest land hex to `at`. Traffic heads for
    // a town nearby, services park in the nearest town as their home. Returns the id or -1.
    int sandbox_place_civilian(int kind, HexCoord at);
    bool sandbox_remove_civilian(int id);

    // Replacements (see REPLACE_*). replacement_squads: the unit whose squads the pool fills for
    // u (u itself for a rifle platoon or squads on foot, the squads aboard an APC/IFV platoon),
    // -1 none. squads_short: squads that unit could take now (up to its establishment; mounted
    // squads up to one a vehicle left). men_per_squad: what one squad costs the pool.
    // replace_rate: squads an hour this unit would get at the trains now (resting, acting HQ).
    int replacement_squads(const Unit &u) const;
    int squads_short(const Unit &u) const;
    // Reinforcements (see Reinforcement). vehicles_short: vehicles, guns or trucks this unit is
    // short of and has not already asked for (0 for infantry, whose men are the replacements, and
    // for units that cannot ask). request_reinforcements: the minutes until they arrive, or -1
    // with the reason in `why` (nothing short, already asked, no headquarters to ask).
    // reinforcement_for: the unit's pending request, or nullptr.
    std::vector<Reinforcement> reinforcements;
    int vehicles_short(const Unit &u) const;
    int request_reinforcements(int unit_id, std::string *why = nullptr);
    const Reinforcement *reinforcement_for(int unit_id) const;
    static int men_per_squad(const Unit &squads);
    float replace_rate(const Unit &u) const { return replace_rate(u, acting_hq(0) >= 0 && units[acting_hq(0)].type != U_HQ); }
    // Halted within TRAINS_REACH hexes of the trains (the HQ or acting commander) and out of the
    // fight for 30 minutes: where resupply and replacements reach a unit of the company.
    static constexpr int TRAINS_REACH = 3;
    bool at_trains(const Unit &u) const { return at_trains(u, acting_hq(0)); }
    // A unit the player may order: not militia, and not a column working a route.
    bool orderable(const Unit &u) const;

    Map map;
    std::vector<Unit> units;
    std::vector<Formation> formations;
    std::vector<Holding> holdings;
    std::vector<SupplyRoute> routes;
    std::vector<Marking> markings; // the player's own; never read by the simulation
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
    void update_suppression();
    void dismount_now(Unit &carrier);
    void start_mount(Unit &ordered);
    void update_mounting();
    void bucket_insert(const Unit &u);
    void displace_battery(Unit &u);
    void update_morale_and_sop();
    void update_ai();
    void update_logistics();
    void update_hunters();
    void check_outcome();
    // Holdings and supply.
    void setup_holdings();
    void update_holdings();              // hourly: presence, capture, loyalty, militia training
    void update_trains_at_depot(int hq); // hourly: the trains draw from an own holding they stand at
    bool at_trains(const Unit &u, int hq) const;
    static float replace_rate(const Unit &u, bool acting);
    void replace_losses(int hq, bool acting); // hourly: the replacement pool fills squads (MRK-10)
    void deliver_reinforcements(int hq);       // hourly: arrived reinforcements go to units at the trains (MRK-07)
    void update_convoys();               // every tick: the convoy state machine
    bool convoy_arrive_or_repath(Unit &u, HexCoord target, int reach); // true once halted within reach
    void load_convoy(Unit &u, SupplyRoute &r);
    void unload_convoy(Unit &u, SupplyRoute &r);
    void convoy_suspend(Unit &u, const char *why);
    void capture(Holding &h, int side);
    void add_militia_squad(Holding &h);
    double used_diesel_l_ = 0, used_ammo_t_ = 0; // this hour, for the consumption rates
    void push_event(EventKind k, int unit, bool pause, const std::string &text);
    void start_path(Unit &u, HexCoord target, OrderKind kind, bool announce);
    void start_leg(Unit &u, bool announce);  // plans the leg to route[wp_i]
    void update_routes();                    // halts at waypoints, next legs, resuming paused routes
    void pause_route(Unit &u, uint8_t why, HexCoord blocked);
    int standoff_threat(const Unit &u, int S, HexCoord *at, int *dist) const;
    bool standoff_blocks(const Unit &u) const;
    HexCoord regroup_point(const Unit &u) const;
    HexCoord fall_back_point(const Unit &u) const;
    int spawn(int formation, int8_t side, UnitType type, const std::string &name, const std::string &equip,
              HexCoord near, std::vector<int> &taken);
    template <typename F> void for_units_near(HexCoord h, int radius, F &&fn) const;

    Pathfinder pf_;
    // The pathfinder, attached to this world's map. A copied World's pathfinder still points at
    // the map it was copied from, so it is re-attached on first use (searches do not depend on
    // the scratch state, so results are unchanged).
    Pathfinder &pathfinder() {
        if (pf_.attached() != &map) pf_.attach(&map);
        return pf_;
    }
    std::vector<int32_t> region_; // land region per hex, -1 for water
    Rng rng_;
    std::vector<Event> events_;
    SimStats stats_;
    int bucket_cols_ = 0, bucket_rows_ = 0;
    std::vector<std::vector<int>> buckets_;
    mutable int los_checks_ = 0;
    int paths_this_tick_ = 0;
    bool alert_ = false; // a pause-worthy event was raised this tick
    bool clock_started_ = false; // the clock has moved since setup (locks the sandbox start time)
    // Civilians (MRK-34), in civilians.cpp.
    Rng civ_rng_;                           // their own dice: spawning never shifts the military rolls
    int civ_next_id_ = 0;
    int civ_paths_this_tick_ = 0;
    std::vector<int> active_towns_;         // settlements within reach of the fighting, hourly
    std::vector<DangerMark> danger_;
    std::vector<Incident> incidents_;
    std::vector<uint8_t> town_country_;     // per settlement (derived from the map)
    std::unordered_map<int, int> civ_hex_count_; // hex index -> civilian vehicles (derived each tick)
    void update_civilians();
    void civ_update_active_towns();
    void civ_spawn_traffic();
    void civ_station_services();
    void civ_move();
    void civ_react();
    void civ_dispatch_services();
    void civ_update_seen();
    void civ_rebuild_counts();
    bool civ_path(Civilian &c, HexCoord to);
    int civ_pick_destination(int home, HexCoord from);
    float civ_step_minutes(const Civilian &c, int from_idx, int to_idx) const;
    int civ_new(uint8_t kind, int home, HexCoord at);
    uint8_t town_country(int settlement);
    int nearest_danger(HexCoord h, int64_t since) const; // hexes to the nearest danger mark, 999 none
    void note_danger(HexCoord at, bool hit);
    Outcome outcome_ = OUT_ONGOING;
    std::string outcome_text_;
    // Who the company trains were last reported working for (-1 the HQ or no one). Only used
    // to announce a new acting commander; recomputed on load, so it is not saved.
    int trains_announced_ = -1;
    int dead_hq(int formation) const; // the formation's destroyed HQ, -1 if alive or none
};

} // namespace murks
