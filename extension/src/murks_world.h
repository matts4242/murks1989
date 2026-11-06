// Murks 1989 — Godot binding for the C++ core.
//
// MurksWorld is the only class GDScript talks to. Calls are coarse on purpose:
// whole chunks, whole snapshots, many ticks per call, so the cost of crossing
// between GDScript and C++ stays small.
#pragma once

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
        SF_ORDER,     // own units: OrderKind (0 hold, 1 move, 2 falling back, 3 regroup, 4 rest), else -1
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
    void setup_scenario(int hostile_groups);

    Vector2i get_map_size() const;
    Vector2i get_chunk_count() const;
    Vector2 get_world_size() const;
    Vector2 get_chunk_origin(Vector2i chunk) const;
    Vector2 hex_to_world(Vector2i hex) const;
    Vector2i world_to_hex(Vector2 pos) const;
    float get_hex_width() const;
    float get_row_spacing() const;

    Ref<ArrayMesh> build_chunk_mesh(Vector2i chunk) const;
    Ref<ArrayMesh> build_chunk_grid_mesh(Vector2i chunk) const;
    Ref<Image> build_overview_image() const;
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
    int order_hold(int id);
    int order_fall_back(int id);
    int order_regroup(int id, bool rest);
    int set_stance(int id, int roe, int break_contact);
    int estimate_command_delay(int id) const;
    int create_group(const PackedInt32Array &ids, int leader);
    bool disband_group(int gid);
    bool set_group_shape(int gid, int shape, int spacing);
    int order_group_move(int gid, Vector2i hex, int mode = 0);
    int order_group_hold(int gid);
    Array get_groups() const;
    int set_standoff(int id, int standoff);
    void set_default_standoff(int standoff);
    int get_default_standoff() const;
    PackedVector2Array get_visible_hexes(int id) const;
    Array poll_events();
    Dictionary get_stats() const;

protected:
    static void _bind_methods();

private:
    Vector2 unit_world_pos(const murks::Unit &u) const;
    murks::World world_;
    bool generated_ = false;
};

} // namespace godot
