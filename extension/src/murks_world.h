// Murks 1989 — Godot binding for the C++ core.
//
// MurksWorld is the only class GDScript talks to. Calls are coarse on purpose:
// whole chunks, whole snapshots, many ticks per call, so the cost of crossing
// between GDScript and C++ stays small.
#pragma once

#include "murks/contours.hpp"
#include "murks/sim.hpp"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

namespace godot {

class MurksWorld : public RefCounted {
    GDCLASS(MurksWorld, RefCounted)

public:
    // Layout of get_units_snapshot(): UNIT_STRIDE floats per visible unit.
    enum SnapshotField {
        SF_ID = 0,
        SF_SIDE,
        SF_TYPE,
        SF_X,
        SF_Y,
        SF_STRENGTH,
        SF_ZONE,
        SF_FLAGS,
        SF_MORALE,
        SF_ORDER_IN,  // minutes until a pending order takes effect, -1 if none
        SF_FORMATION, // own units: formation (company) index; hostile units: -1 (their organisation is unknown)
        SF_MODE,      // own units: MoveMode while moving, else -1
        SF_ROE,       // own units: rules of engagement (0 free, 1 return fire, 2 hold fire), else -1
        SF_ORDER,     // own units: OrderKind (0 hold, 1 move, 2 falling back, 3 regroup, 4 rest), 5 squads
                      // walking back to mount up, else -1
        SF_FIRED_AT,  // own or spotted units: game minute of the last shot, else -1e6 (drives fire effects)
        SF_TARGET,    // own or spotted units: id of the last unit it fired at, else -1
        SF_TARGETED,  // own units: game minute it was last fired at, else -1e6
        SF_ENGAGE,    // own units: engagement-time factor (1 = fresh crews; 2 = twice as slow), else -1
        SF_ELEM,      // own units: elements (vehicles, guns, squads) still in the fight, else -1
        SF_ELEM_MAX,  // own units: elements at full strength, else -1
        SF_GROUP,     // own units: team id, else -1
        SF_ROLE,      // own units in a team: 2 leader, 1 member in formation, 0 member detached; else -1
        SF_GROUP_COLOR, // own units in a team: team colour index 0..7, else -1
        SF_CMD_ZONE,  // own HQs and team leaders: zone of command in hexes, else 0
        SF_STANDOFF,  // own units: effective stand-off in hexes (0 = stay and fight), else -1
        // Direct command overlay (own units): who it takes orders from, and how they reach it.
        SF_CMD_BOSS,  // id of the HQ or team leader it answers to, -1 none
        SF_CMD_LINK,  // -1 top of the chain (an HQ), 0 no commander (orders improvised), 1 inside the
                      // commander's zone, 2 outside it (orders x1.5), 3 beyond radio range (courier)
        // Fire support overlay (own units; see murks::FireNetLink).
        SF_FS_ROLE,    // -1 no fire net, 0 guns, 1 observer on the net, 2 calls fire through a relay
        SF_FS_STATUS,  // 0 cut off, 1 relayed, 2 direct
        SF_FS_BATTERY, // id of the guns it reaches (or the nearest guns when cut off), -1 none
        SF_FS_VIA,     // relayed: id of the unit on the net that passes its calls on, else -1
        SF_FS_RANGE,   // guns: range in hexes (for the range ring), else 0
        // Mounted infantry (own units).
        SF_CARRY,      // APC/IFV platoons: squads aboard (0 while they are out), else -1
        SF_MOUNT_LINK, // carriers with squads out: the squad unit's id; squads on foot: their vehicles' id; else -1
        SF_SUPP,       // own units: suppression 0-100, +1000 when pinned; else -1
        // Holdings and supply (MRK-08 / MRK-09).
        SF_MILITIA,    // own or seen units: 1 for a town's militia (garrison, takes no orders), else 0
        SF_CONVOY,     // own supply columns: ConvoyState (0 with the trains .. 5 returning), else -1
        SF_CARGO_T,    // own supply columns: tonnes aboard, else -1
        // Counter tags (MRK-35): only what differs from the company's standing orders is tagged.
        SF_STANDOFF_SET, // own units: the unit's own stand-off setting (255 = company default), else -1
        UNIT_STRIDE
    };
    enum SnapshotFlag {
        FLAG_SEEN = 1,
        FLAG_GHOST = 2,
        FLAG_MOVING = 4,
        FLAG_PENDING = 8,
        FLAG_RETREAT = 16,
        FLAG_FIRING = 32,
        FLAG_HIT = 64,
    };

    Dictionary generate(int seed, int chunks_x, int chunks_y);
    Dictionary load_map_bytes(const PackedByteArray &bytes);
    Dictionary get_map_info() const;
    Vector2 world_to_lonlat(Vector2 pos) const; // (lon, lat) in degrees; (NAN, NAN) if not georeferenced
    // kind: murks::ScenarioKind (0 the contract, 1 the three-way).
    void setup_scenario(int hostile_groups, int kind = 0);
    int get_scenario() const;

    Vector2i get_map_size() const;
    Vector2i get_chunk_count() const;
    Vector2 get_world_size() const;
    Vector2 get_chunk_origin(Vector2i chunk) const;
    Vector2 hex_to_world(Vector2i hex) const;
    Vector2i world_to_hex(Vector2 pos) const;
    float get_hex_width() const;
    float get_row_spacing() const;

    Ref<ArrayMesh> build_chunk_mesh(Vector2i chunk) const;
    // Relief for the topographic mode (drawing data baked beside the map; optional).
    bool load_contour_bytes(const PackedByteArray &bytes);
    bool has_contours() const;
    Ref<Image> get_height_image() const;
    float get_height_pixel_size() const;
    PackedFloat32Array get_chunk_contour_labels(Vector2i chunk, int step) const;
    PackedFloat32Array get_chunk_spot_heights(Vector2i chunk) const;
    Ref<Image> build_overview_image() const;
    Ref<Image> build_hex_data_image() const;
    Ref<Image> build_plan_image(int mode) const;
    Array get_plan_key(int mode) const;
    String get_plan_mode_name(int mode) const;
    Dictionary get_viewshed(Vector2i hex, float eye_m) const;
    // Planning tools (MRK-26).
    Dictionary get_route_estimate(Vector2i from, Vector2i to, int mobility, int mode);
    Dictionary get_profile(PackedVector2Array points, float eye_m) const;
    String get_grid_ref(Vector2 pos) const;
    float get_grid_bearing(Vector2 a, Vector2 b) const;
    Array get_markings() const;
    int add_marking(int kind, String name, PackedVector2Array points);
    bool rename_marking(int index, String name);
    bool remove_marking(int index);
    void clear_markings();
    Dictionary get_hex_info(Vector2i hex) const;
    Array get_settlements(int min_population) const;

    int advance(int ticks, double budget_ms);
    int get_time_minutes() const;
    String get_time_string() const;
    bool is_night() const;
    bool player_in_hot_zone() const;
    bool player_in_contact() const;
    Dictionary get_objectives() const;
    PackedByteArray save_state() const;
    String load_state(const PackedByteArray &bytes); // "" on success, else the reason

    PackedFloat32Array get_units_snapshot() const;
    int command_link(const murks::Unit &u, int boss) const;
    int get_unit_stride() const { return UNIT_STRIDE; }
    Dictionary get_unit_info(int id) const;
    int order_move(int id, Vector2i hex, int mode = 0);
    // Routes (MRK-20). A route is an Array of Dictionaries {hex: Vector2i, mode: int,
    // halt: int minutes, shape: int (-1 keep)}, up to 16.
    int order_route(int id, const Array &route);
    Dictionary route_delay(int id, const Array &route) const; // {delay, adjust}; delay -1 = not valid
    int order_group_route(int gid, const Array &route);
    Dictionary plan_leg(int id, Vector2i from, Vector2i to, int mode); // {minutes, path} (world positions)
    int order_hold(int id);
    int order_rejoin(int id); // detached team member takes its place again
    int request_reinforcements(int id); // lost vehicles, guns or trucks come in 2-3 days (MRK-07); -1 if none can be asked
    int order_fall_back(int id);
    int order_regroup(int id, bool rest);
    int order_dismount(int id);
    int order_mount(int id);
    int set_stance(int id, int roe, int break_contact);
    int estimate_command_delay(int id) const;
    int create_group(const PackedInt32Array &ids, int leader);
    bool disband_group(int gid);
    bool set_group_shape(int gid, int shape, int spacing);
    int order_group_move(int gid, Vector2i hex, int mode = 0);
    int order_group_hold(int gid);
    Array get_groups() const;
    Array get_oob() const; // the player's order of battle by chain of command
    int set_standoff(int id, int standoff);
    void set_default_standoff(int standoff);
    int get_default_standoff() const;
    PackedVector2Array get_visible_hexes(int id) const;
    Array poll_events();
    Dictionary get_stats() const;

    // Holdings (towns, farm collectives, depots) and supply routes. What the player sees of a
    // place they don't hold is limited to what they know: owner as last seen, no stocks.
    Array get_holdings() const;
    Dictionary get_holding(int hid) const;
    int holding_at(Vector2i hex) const;             // holding whose footprint holds the hex, -1 none
    bool set_training(int hid, bool on);            // raise militia at an own town
    String train_engineers(int hid);                // engineer course at an own town: "" or why not
    String build(int id, int kind);                 // engineers build a garage (4) or depot (5) where they stand
    String levy(int hid, int kind);                 // requisition (0 food, 1 fuel, 2 cash, 3 men); "" = done
    Dictionary set_supply_route(int source, int dest); // {"id": route or -1, "error": reason}
    bool cancel_supply_route(int rid);
    Array get_supply_routes() const;                // every route still worth showing (active or suspended)
    bool is_orderable(int id) const;                // not militia, not a column working a route

    // Sandbox (MRK-31). The flag is set right after setup_scenario; kinds follow World::SandboxKind
    // (0 tank, 1 mech, 2 rifle, 3 recon, 4 battery, 5 HQ, 6 supply column).
    void set_sandbox(bool on);
    bool is_sandbox() const;
    void set_hostile_ai(bool on);
    bool get_hostile_ai() const;
    int sandbox_place(int side, int kind, Vector2i hex);
    bool sandbox_remove(int id);
    bool set_start_time(int day, int hour);
    bool is_clock_started() const;
    // Display only: draw every hostile where it is, seen or not (spotting is unchanged).
    void set_reveal_hostiles(bool on);
    // Civilians (MRK-34). Snapshot: CIV_STRIDE floats per civilian the player sees (all of them
    // with Show all hostiles in the sandbox): id, kind (murks::CivKind), x, y, heading x, heading y,
    // state (murks::CivState), country (0 GDR, 1 Poland).
    static constexpr int CIV_STRIDE = 8;
    PackedFloat32Array get_civilians_snapshot() const;
    Dictionary get_civilian_info(int id) const;
    int sandbox_place_civilian(int kind, Vector2i hex);
    bool sandbox_remove_civilian(int id);
    bool get_reveal_hostiles() const;

protected:
    static void _bind_methods();

private:
    Vector2 unit_world_pos(const murks::Unit &u) const;
    murks::World world_;
    bool reveal_hostiles_ = false;
    murks::ContourSet contours_;
    bool generated_ = false;
};

} // namespace godot
