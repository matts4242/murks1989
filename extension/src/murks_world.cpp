#include "murks_world.h"

#include "murks/geo.hpp"
#include "murks/render_data.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <cstring>

using namespace godot;

namespace {

String to_gd(const std::string &s) { return String::utf8(s.c_str()); }

const char *zone_name(int z) {
    switch (z) {
        case murks::Z_HOT: return "Hot";
        case murks::Z_WARM: return "Warm";
        default: return "Cold";
    }
}

const char *event_kind_name(int k) {
    switch (k) {
        case murks::EV_CONTACT: return "contact";
        case murks::EV_ORDER: return "order";
        case murks::EV_ARRIVED: return "arrived";
        case murks::EV_DESTROYED: return "destroyed";
        case murks::EV_RETREAT: return "retreat";
        case murks::EV_SUPPLY: return "supply";
        case murks::EV_UNDER_FIRE: return "under_fire";
        default: return "info";
    }
}

} // namespace

void MurksWorld::_bind_methods() {
    ClassDB::bind_method(D_METHOD("generate", "seed", "chunks_x", "chunks_y"), &MurksWorld::generate);
    ClassDB::bind_method(D_METHOD("load_map_bytes", "bytes"), &MurksWorld::load_map_bytes);
    ClassDB::bind_method(D_METHOD("get_map_info"), &MurksWorld::get_map_info);
    ClassDB::bind_method(D_METHOD("world_to_lonlat", "pos"), &MurksWorld::world_to_lonlat);
    ClassDB::bind_method(D_METHOD("setup_scenario", "hostile_groups"), &MurksWorld::setup_scenario);
    ClassDB::bind_method(D_METHOD("get_map_size"), &MurksWorld::get_map_size);
    ClassDB::bind_method(D_METHOD("get_chunk_count"), &MurksWorld::get_chunk_count);
    ClassDB::bind_method(D_METHOD("get_world_size"), &MurksWorld::get_world_size);
    ClassDB::bind_method(D_METHOD("get_chunk_origin", "chunk"), &MurksWorld::get_chunk_origin);
    ClassDB::bind_method(D_METHOD("hex_to_world", "hex"), &MurksWorld::hex_to_world);
    ClassDB::bind_method(D_METHOD("world_to_hex", "pos"), &MurksWorld::world_to_hex);
    ClassDB::bind_method(D_METHOD("get_hex_width"), &MurksWorld::get_hex_width);
    ClassDB::bind_method(D_METHOD("get_row_spacing"), &MurksWorld::get_row_spacing);
    ClassDB::bind_method(D_METHOD("build_chunk_mesh", "chunk"), &MurksWorld::build_chunk_mesh);
    ClassDB::bind_method(D_METHOD("build_chunk_grid_mesh", "chunk"), &MurksWorld::build_chunk_grid_mesh);
    ClassDB::bind_method(D_METHOD("build_overview_image"), &MurksWorld::build_overview_image);
    ClassDB::bind_method(D_METHOD("get_hex_info", "hex"), &MurksWorld::get_hex_info);
    ClassDB::bind_method(D_METHOD("get_settlements", "min_population"), &MurksWorld::get_settlements);
    ClassDB::bind_method(D_METHOD("advance", "ticks", "budget_ms"), &MurksWorld::advance);
    ClassDB::bind_method(D_METHOD("get_time_minutes"), &MurksWorld::get_time_minutes);
    ClassDB::bind_method(D_METHOD("get_time_string"), &MurksWorld::get_time_string);
    ClassDB::bind_method(D_METHOD("is_night"), &MurksWorld::is_night);
    ClassDB::bind_method(D_METHOD("player_in_hot_zone"), &MurksWorld::player_in_hot_zone);
    ClassDB::bind_method(D_METHOD("player_in_contact"), &MurksWorld::player_in_contact);
    ClassDB::bind_method(D_METHOD("get_objectives"), &MurksWorld::get_objectives);
    ClassDB::bind_method(D_METHOD("save_state"), &MurksWorld::save_state);
    ClassDB::bind_method(D_METHOD("load_state", "bytes"), &MurksWorld::load_state);
    ClassDB::bind_method(D_METHOD("get_units_snapshot"), &MurksWorld::get_units_snapshot);
    ClassDB::bind_method(D_METHOD("get_unit_stride"), &MurksWorld::get_unit_stride);
    ClassDB::bind_method(D_METHOD("get_unit_info", "id"), &MurksWorld::get_unit_info);
    ClassDB::bind_method(D_METHOD("order_move", "id", "hex", "mode"), &MurksWorld::order_move, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("order_fall_back", "id"), &MurksWorld::order_fall_back);
    ClassDB::bind_method(D_METHOD("order_regroup", "id", "rest"), &MurksWorld::order_regroup);
    ClassDB::bind_method(D_METHOD("set_stance", "id", "roe", "break_contact"), &MurksWorld::set_stance);
    ClassDB::bind_method(D_METHOD("order_hold", "id"), &MurksWorld::order_hold);
    ClassDB::bind_method(D_METHOD("estimate_command_delay", "id"), &MurksWorld::estimate_command_delay);
    ClassDB::bind_method(D_METHOD("create_group", "ids", "leader"), &MurksWorld::create_group);
    ClassDB::bind_method(D_METHOD("disband_group", "gid"), &MurksWorld::disband_group);
    ClassDB::bind_method(D_METHOD("set_group_shape", "gid", "shape", "spacing"), &MurksWorld::set_group_shape);
    ClassDB::bind_method(D_METHOD("order_group_move", "gid", "hex", "mode"), &MurksWorld::order_group_move, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("order_group_hold", "gid"), &MurksWorld::order_group_hold);
    ClassDB::bind_method(D_METHOD("get_groups"), &MurksWorld::get_groups);
    ClassDB::bind_method(D_METHOD("set_standoff", "id", "standoff"), &MurksWorld::set_standoff);
    ClassDB::bind_method(D_METHOD("set_default_standoff", "standoff"), &MurksWorld::set_default_standoff);
    ClassDB::bind_method(D_METHOD("get_default_standoff"), &MurksWorld::get_default_standoff);
    ClassDB::bind_method(D_METHOD("get_visible_hexes", "id"), &MurksWorld::get_visible_hexes);
    ClassDB::bind_method(D_METHOD("poll_events"), &MurksWorld::poll_events);
    ClassDB::bind_method(D_METHOD("get_stats"), &MurksWorld::get_stats);
}

Dictionary MurksWorld::generate(int seed, int chunks_x, int chunks_y) {
    murks::GenStats st = world_.generate((uint32_t)seed, chunks_x, chunks_y);
    generated_ = true;
    Dictionary d;
    d["ms_total"] = st.ms_total;
    d["ms_terrain"] = st.ms_terrain;
    d["ms_roads"] = st.ms_roads + st.ms_rail;
    d["hexes"] = world_.map.hex_count();
    d["memory_mb"] = world_.map.memory_bytes() / 1.0e6;
    d["settlements"] = st.settlements;
    d["cities"] = st.cities;
    d["road_hexes"] = st.road_hexes;
    d["rail_hexes"] = st.rail_hexes;
    d["river_hexes"] = st.river_hexes;
    return d;
}

Dictionary MurksWorld::load_map_bytes(const PackedByteArray &bytes) {
    Dictionary d;
    generated_ = world_.load_map(bytes.ptr(), (size_t)bytes.size());
    d["ok"] = generated_;
    if (!generated_) return d;
    murks::GenStats st;
    murks::count_map_stats(world_.map, st);
    d["hexes"] = world_.map.hex_count();
    d["memory_mb"] = world_.map.memory_bytes() / 1.0e6;
    d["settlements"] = st.settlements;
    d["cities"] = st.cities;
    d["road_hexes"] = st.road_hexes;
    d["rail_hexes"] = st.rail_hexes;
    d["river_hexes"] = st.river_hexes;
    d["ms_total"] = 0;
    return d;
}

Dictionary MurksWorld::get_map_info() const {
    Dictionary d;
    d["name"] = to_gd(world_.map.name);
    d["epsg"] = world_.map.epsg;
    d["cols"] = world_.map.cols();
    d["rows"] = world_.map.rows();
    d["width_km"] = world_.map.width_m() / 1000.0;
    d["height_km"] = world_.map.height_m() / 1000.0;
    d["settlements"] = (int)world_.map.settlements.size();
    return d;
}

Vector2 MurksWorld::world_to_lonlat(Vector2 pos) const {
    double lon, lat;
    if (!murks::world_to_lonlat(world_.map, pos.x, pos.y, lon, lat)) return Vector2(NAN, NAN);
    return Vector2((real_t)lon, (real_t)lat);
}

void MurksWorld::setup_scenario(int hostile_groups) {
    if (generated_) world_.setup_scenario(hostile_groups);
}

Vector2i MurksWorld::get_map_size() const { return Vector2i(world_.map.cols(), world_.map.rows()); }
Vector2i MurksWorld::get_chunk_count() const { return Vector2i(world_.map.chunks_x(), world_.map.chunks_y()); }
Vector2 MurksWorld::get_world_size() const { return Vector2(world_.map.width_m(), world_.map.height_m()); }
float MurksWorld::get_hex_width() const { return (float)murks::HEX_WIDTH; }
float MurksWorld::get_row_spacing() const { return (float)murks::ROW_SPACING; }

Vector2 MurksWorld::get_chunk_origin(Vector2i chunk) const {
    double ox, oy;
    murks::chunk_origin(chunk.x, chunk.y, ox, oy);
    return Vector2((real_t)ox, (real_t)oy);
}

Vector2 MurksWorld::hex_to_world(Vector2i hex) const {
    murks::Vec2 c = murks::hex_center({hex.x, hex.y});
    return Vector2((real_t)c.x, (real_t)c.y);
}

Vector2i MurksWorld::world_to_hex(Vector2 pos) const {
    murks::HexCoord h = murks::world_to_hex(pos.x, pos.y);
    return Vector2i(h.col, h.row);
}

Ref<ArrayMesh> MurksWorld::build_chunk_mesh(Vector2i chunk) const {
    Ref<ArrayMesh> mesh;
    mesh.instantiate();
    if (!generated_ || chunk.x < 0 || chunk.y < 0 || chunk.x >= world_.map.chunks_x() ||
        chunk.y >= world_.map.chunks_y())
        return mesh;
    murks::ChunkGeometry g;
    murks::build_chunk_geometry(world_.map, chunk.x, chunk.y, false, g);

    const int64_t nv = (int64_t)g.xy.size() / 2;
    PackedVector2Array verts;
    verts.resize(nv);
    Vector2 *vp = verts.ptrw();
    for (int64_t i = 0; i < nv; ++i) vp[i] = Vector2(g.xy[i * 2], g.xy[i * 2 + 1]);

    PackedColorArray cols;
    cols.resize(nv);
    Color *cp = cols.ptrw();
    for (int64_t i = 0; i < nv; ++i) {
        uint32_t c = g.rgba[i];
        cp[i] = Color((c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f,
                      ((c >> 24) & 255) / 255.0f);
    }

    PackedInt32Array idx;
    idx.resize((int64_t)g.indices.size());
    std::memcpy(idx.ptrw(), g.indices.data(), g.indices.size() * sizeof(int32_t));

    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = verts;
    arrays[Mesh::ARRAY_COLOR] = cols;
    arrays[Mesh::ARRAY_INDEX] = idx;
    mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
    return mesh;
}

Ref<ArrayMesh> MurksWorld::build_chunk_grid_mesh(Vector2i chunk) const {
    Ref<ArrayMesh> mesh;
    mesh.instantiate();
    if (!generated_ || chunk.x < 0 || chunk.y < 0 || chunk.x >= world_.map.chunks_x() ||
        chunk.y >= world_.map.chunks_y())
        return mesh;
    murks::ChunkGeometry g;
    murks::build_chunk_geometry(world_.map, chunk.x, chunk.y, true, g);
    const int64_t nv = (int64_t)g.grid_xy.size() / 2;
    PackedVector2Array verts;
    verts.resize(nv);
    Vector2 *vp = verts.ptrw();
    for (int64_t i = 0; i < nv; ++i) vp[i] = Vector2(g.grid_xy[i * 2], g.grid_xy[i * 2 + 1]);
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = verts;
    mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, arrays);
    return mesh;
}

Ref<Image> MurksWorld::build_overview_image() const {
    std::vector<uint8_t> rgba;
    murks::build_overview_rgba(world_.map, rgba);
    PackedByteArray bytes;
    bytes.resize((int64_t)rgba.size());
    std::memcpy(bytes.ptrw(), rgba.data(), rgba.size());
    return Image::create_from_data(world_.map.cols(), world_.map.rows(), false, Image::FORMAT_RGBA8, bytes);
}

Dictionary MurksWorld::get_hex_info(Vector2i hex) const {
    Dictionary d;
    murks::HexCoord h{hex.x, hex.y};
    if (!generated_ || !world_.map.in_bounds(h)) return d;
    const murks::Hex &x = world_.map.at(h);
    d["col"] = hex.x;
    d["row"] = hex.y;
    d["terrain"] = murks::terrain_name(x.terrain);
    d["elevation"] = (int)x.elev;
    d["cover"] = (int)x.cover;
    d["road"] = (x.features & murks::F_ROAD) != 0;
    d["major_road"] = (x.features & murks::F_MAJOR_ROAD) != 0;
    d["rail"] = (x.features & murks::F_RAIL) != 0;
    d["river"] = (x.features & murks::F_RIVER) != 0;
    d["bridge"] = (x.features & murks::F_BRIDGE) != 0;
    d["place"] = to_gd(world_.place_name(h));
    if (x.settlement != murks::NO_SETTLEMENT) {
        const murks::Settlement &s = world_.map.settlements[x.settlement];
        d["settlement"] = to_gd(s.name);
        d["population"] = s.population;
    }
    return d;
}

Array MurksWorld::get_settlements(int min_population) const {
    Array out;
    for (const auto &s : world_.map.settlements) {
        if (s.population < min_population) continue;
        Dictionary d;
        d["name"] = to_gd(s.name);
        d["population"] = s.population;
        d["radius"] = s.radius;
        murks::Vec2 c = murks::hex_center(s.pos);
        d["pos"] = Vector2((real_t)c.x, (real_t)c.y);
        out.push_back(d);
    }
    return out;
}

int MurksWorld::advance(int ticks, double budget_ms) { return generated_ ? world_.advance(ticks, budget_ms) : 0; }
int MurksWorld::get_time_minutes() const { return (int)world_.time_min; }
String MurksWorld::get_time_string() const { return to_gd(world_.time_string()); }
bool MurksWorld::is_night() const { return world_.is_night(); }
bool MurksWorld::player_in_hot_zone() const { return world_.player_in_hot_zone(); }
bool MurksWorld::player_in_contact() const { return world_.player_in_contact(); }

Dictionary MurksWorld::get_objectives() const {
    Dictionary d;
    const murks::Outcome oc = world_.outcome();
    d["outcome"] = (int)oc;
    d["outcome_name"] = oc == murks::OUT_VICTORY ? "victory" : oc == murks::OUT_DEFEAT ? "defeat" : "ongoing";
    d["outcome_text"] = to_gd(world_.outcome_text());
    d["kills"] = world_.stats().kills;
    d["losses"] = world_.stats().losses;
    d["time_minutes"] = (int64_t)world_.time_min;
    d["hq_alive"] = world_.hq_of(murks::SIDE_PLAYER) >= 0;
    if (!world_.formations.empty()) {
        d["own_effective"] = world_.effective_combat_units(0);
        d["own_start"] = world_.formations[0].start_combat;
    }
    int sf = world_.strike_formation();
    d["enemy_name"] = sf >= 0 ? to_gd(world_.formations[sf].name) : String();
    d["enemy_effective"] = sf >= 0 ? world_.effective_combat_units(sf) : 0;
    d["enemy_start"] = sf >= 0 ? world_.formations[sf].start_combat : 0;
    int home = world_.player_home;
    d["home"] = home >= 0 && home < (int)world_.map.settlements.size() ? to_gd(world_.map.settlements[home].name)
                                                                       : String();
    return d;
}

PackedByteArray MurksWorld::save_state() const {
    PackedByteArray out;
    if (!generated_) return out;
    std::vector<uint8_t> b = world_.save_state();
    out.resize((int64_t)b.size());
    if (!b.empty()) std::memcpy(out.ptrw(), b.data(), b.size());
    return out;
}

String MurksWorld::load_state(const PackedByteArray &bytes) {
    if (!generated_) return "no map loaded";
    std::string err;
    if (!world_.load_state(bytes.ptr(), (size_t)bytes.size(), &err)) return to_gd(err);
    return String();
}

// Units are drawn in the hex the simulation has them in (spotting, fire and stacking all
// use it), and move hex to hex: no sliding between hexes.
Vector2 MurksWorld::unit_world_pos(const murks::Unit &u) const {
    murks::Vec2 a = murks::hex_center(u.pos);
    return Vector2((real_t)a.x, (real_t)a.y);
}

PackedFloat32Array MurksWorld::get_units_snapshot() const {
    PackedFloat32Array out;
    const auto &units = world_.units;
    out.resize((int64_t)units.size() * UNIT_STRIDE);
    float *p = out.ptrw();
    int64_t n = 0;
    const int64_t now = world_.time_min;
    for (const auto &u : units) {
        if (!u.alive) continue;
        int flags = 0;
        Vector2 pos;
        if (u.side == murks::SIDE_PLAYER) {
            flags |= FLAG_SEEN;
            pos = unit_world_pos(u);
        } else if (world_.seen_by(u, murks::SIDE_PLAYER)) {
            flags |= FLAG_SEEN;
            pos = unit_world_pos(u);
        } else if (now - u.last_seen[murks::SIDE_PLAYER] <= 60) {
            flags |= FLAG_GHOST; // last known position only: fog of war is enforced here
            murks::Vec2 c = murks::hex_center(u.last_seen_pos[murks::SIDE_PLAYER]);
            pos = Vector2((real_t)c.x, (real_t)c.y);
        } else {
            continue;
        }
        bool own = u.side == murks::SIDE_PLAYER;
        if (u.moving() && (own || (flags & FLAG_SEEN))) flags |= FLAG_MOVING;
        if (own && u.pending.active) flags |= FLAG_PENDING;
        if (u.order == murks::O_RETREAT && (flags & FLAG_SEEN)) flags |= FLAG_RETREAT;
        if (now - u.last_fired <= 2 && (flags & FLAG_SEEN)) flags |= FLAG_FIRING;
        if (now - u.last_hit <= 2 && (flags & FLAG_SEEN)) flags |= FLAG_HIT;
        float *q = p + n * UNIT_STRIDE;
        q[SF_ID] = (float)u.id;
        q[SF_SIDE] = (float)u.side;
        q[SF_TYPE] = (float)u.type;
        q[SF_X] = pos.x;
        q[SF_Y] = pos.y;
        q[SF_STRENGTH] = own ? u.strength : -1.0f; // enemy strength is not known
        q[SF_ZONE] = own ? (float)u.zone : 0.0f;
        q[SF_FLAGS] = (float)flags;
        q[SF_MORALE] = own ? u.morale : -1.0f;
        q[SF_ORDER_IN] = own && u.pending.active ? (float)(u.pending.activates_at - now) : -1.0f;
        q[SF_FORMATION] = own ? (float)u.formation : -1.0f;
        q[SF_MODE] = own && u.moving() ? (float)u.mode : -1.0f;
        q[SF_ROE] = own ? (float)u.roe : -1.0f;
        const bool shown = own || (flags & FLAG_SEEN);
        q[SF_ORDER] = own ? (float)u.order : -1.0f;
        q[SF_FIRED_AT] = shown ? (float)u.last_fired : -1e6f;
        q[SF_TARGET] = shown ? (float)u.last_target : -1.0f;
        q[SF_TARGETED] = own ? (float)u.last_targeted : -1e6f;
        if (own) {
            q[SF_ENGAGE] = world_.engagement_time(u).factor;
            q[SF_ELEM] = (float)u.elements;
            q[SF_ELEM_MAX] = (float)u.elements_max;
        } else {
            q[SF_ENGAGE] = q[SF_ELEM] = q[SF_ELEM_MAX] = -1.0f;
        }
        const bool grouped = own && u.group >= 0 && u.group < (int)world_.groups.size() && world_.groups[u.group].alive;
        q[SF_GROUP] = grouped ? (float)u.group : -1.0f;
        q[SF_ROLE] = !grouped ? -1.0f : world_.groups[u.group].leader == u.id ? 2.0f : (u.follow ? 1.0f : 0.0f);
        q[SF_GROUP_COLOR] = grouped ? (float)world_.groups[u.group].color : -1.0f;
        q[SF_STANDOFF] = own ? (float)world_.standoff_hexes(u) : -1.0f;
        q[SF_CMD_ZONE] = own && (u.type == murks::U_HQ || (grouped && world_.groups[u.group].leader == u.id))
                             ? (float)world_.command_zone(u) : 0.0f;
        ++n;
    }
    out.resize(n * UNIT_STRIDE);
    return out;
}

Dictionary MurksWorld::get_unit_info(int id) const {
    Dictionary d;
    if (id < 0 || id >= (int)world_.units.size()) return d;
    const murks::Unit &u = world_.units[id];
    const murks::UnitTypeInfo &ti = murks::unit_type_info(u.type);
    bool own = u.side == murks::SIDE_PLAYER;
    d["id"] = u.id;
    d["side"] = (int)u.side;
    d["alive"] = u.alive;
    d["type"] = (int)u.type;
    d["type_name"] = ti.name;
    if (!own) {
        // Only what an observer could tell.
        d["name"] = String("Hostile ") + ti.name;
        d["seen"] = world_.seen_by(u, murks::SIDE_PLAYER);
        d["last_seen_minutes_ago"] = (int)(world_.time_min - u.last_seen[murks::SIDE_PLAYER]);
        d["location"] = to_gd(world_.place_name(u.last_seen_pos[murks::SIDE_PLAYER]));
        return d;
    }
    d["name"] = to_gd(u.name);
    d["formation"] = to_gd(world_.formations[u.formation].name);
    d["formation_id"] = u.formation;
    d["crew"] = (int)std::lround(ti.crew * u.strength / 100.0f);
    d["equipment"] = to_gd(u.equipment);
    d["strength"] = u.strength;
    if (own) {
        // Combat effectiveness is binary per element: in the fight or knocked out.
        d["elements_active"] = u.elements;
        d["elements_max"] = u.elements_max;
        const murks::EngagementTime et = world_.engagement_time(u);
        d["acquire_min"] = et.acquire;
        d["cycle_min"] = et.cycle;
        d["engage_factor"] = et.factor;
        d["engage_readiness"] = et.f_readiness;
        d["engage_morale"] = et.f_morale;
        d["engage_ammo"] = et.f_ammo;
        d["engage_fire"] = et.f_fire;
        static const char *kNoun[murks::U_TYPE_COUNT] = {"vehicles", "vehicles", "vehicles", "squads", "vehicles", "guns"};
        d["element_noun"] = kNoun[u.type < murks::U_TYPE_COUNT ? u.type : 0];
    }
    d["morale"] = u.morale;
    d["fuel"] = u.fuel;
    d["ammo"] = u.ammo;
    d["readiness"] = u.readiness;
    d["zone"] = zone_name(u.zone);
    {
        int64_t now = world_.time_min;
        d["engaged"] = now - u.last_fired < 15 || now - u.last_hit < 15 || now - u.last_targeted < 15;
    }
    d["location"] = to_gd(world_.place_name(u.pos));
    d["hex"] = Vector2i(u.pos.col, u.pos.row);
    d["spot_range_km"] = ti.spot_range * 0.5 * (world_.is_night() ? 0.35 : 1.0);
    d["weapon_range_km"] = ti.weapon_range * 0.5;
    static const char *kModes[4] = {"Hasty", "Road march", "Deliberate", "Assault"};
    static const char *kRoe[3] = {"Fire at will", "Return fire", "Hold fire"};
    static const char *kBrk[3] = {"Heavy losses", "Light losses", "Never"};
    const char *order = "Holding";
    switch (u.order) {
        case murks::O_MOVE: order = u.mode == murks::MV_HASTY ? "Moving" : kModes[u.mode]; break;
        case murks::O_RETREAT: order = "Falling back"; break;
        case murks::O_REGROUP: order = "Regrouping on HQ"; break;
        case murks::O_REST: order = u.moving() ? "Moving to rest" : "Resting"; break;
        default: break;
    }
    d["order"] = order;
    d["mode"] = (int)u.mode;
    d["mode_name"] = kModes[u.mode];
    d["roe"] = (int)u.roe;
    d["roe_name"] = kRoe[u.roe];
    d["break_contact"] = (int)u.brk;
    d["break_contact_name"] = kBrk[u.brk];
    d["resting"] = u.resting;
    d["stance_pending"] = u.pending_stance.active;
    if (u.pending_stance.active) {
        d["stance_in"] = (int)(u.pending_stance.activates_at - world_.time_min);
        d["pending_roe"] = (int)u.pending_stance.roe;
        d["pending_break_contact"] = (int)u.pending_stance.brk;
    }
    d["command_delay"] = world_.command_delay(u);
    if (own) {
        static const char *kShape[murks::FS_COUNT] = {"Keep layout", "Column", "Line", "Wedge"};
        const bool grouped = u.group >= 0 && u.group < (int)world_.groups.size() && world_.groups[u.group].alive;
        d["group_id"] = grouped ? u.group : -1;
        if (grouped) {
            const murks::Group &g = world_.groups[u.group];
            d["group_name"] = to_gd(g.name);
            d["group_color"] = (int)g.color;
            d["group_leader"] = g.leader;
            d["group_role"] = g.leader == u.id ? "leader" : (u.follow ? "in formation" : "detached");
            d["group_shape"] = (int)g.shape;
            d["group_shape_name"] = kShape[g.shape < murks::FS_COUNT ? g.shape : 0];
            d["group_spacing_km"] = g.spacing * 0.5;
        }
        int hq = world_.hq_above(u);
        if (hq >= 0) {
            const murks::Unit &h = world_.units[hq];
            int dist = murks::hex_distance(u.pos, h.pos);
            d["hq_name"] = to_gd(h.name);
            d["hq_dist_km"] = dist * 0.5;
            d["hq_zone_km"] = world_.command_zone(h) * 0.5;
            d["in_zone"] = dist <= world_.command_zone(h);
        }
        if (u.type == murks::U_HQ || (grouped && world_.groups[u.group].leader == u.id))
            d["own_zone_km"] = world_.command_zone(u) * 0.5;
        // Stand-off: the unit's own setting (255 = company default, 254 = weapon range, 0 = fight).
        d["standoff_setting"] = (int)u.standoff;
        d["standoff_hexes"] = world_.standoff_hexes(u);
        d["standoff_pending"] = u.pending_stance.active && u.pending_stance.standoff != u.standoff;
        if (u.pending_stance.active) d["pending_standoff"] = (int)u.pending_stance.standoff;
    }
    d["pending"] = u.pending.active;
    if (u.pending.active) {
        static const char *kPend[5] = {"Hold", "Move", "Fall back", "Regroup", "Rest & resupply"};
        d["pending_kind"] = u.pending.kind == murks::O_MOVE && u.pending.mode != murks::MV_HASTY
                                ? kModes[u.pending.mode]
                                : kPend[u.pending.kind];
        d["pending_in"] = (int)(u.pending.activates_at - world_.time_min);
        murks::Vec2 t = murks::hex_center(u.pending.target);
        d["pending_target"] = Vector2((real_t)t.x, (real_t)t.y);
    }
    PackedVector2Array path;
    if (u.moving()) {
        path.push_back(unit_world_pos(u));
        for (size_t i = u.path_i; i < u.path.size(); ++i) {
            murks::Vec2 c = murks::hex_center(world_.map.coord_of(u.path[i]));
            path.push_back(Vector2((real_t)c.x, (real_t)c.y));
        }
        // Rough time left: remaining step costs.
        float mins = u.step_cost - u.progress;
        for (size_t i = u.path_i + 1; i < u.path.size(); ++i) mins += world_.move_cost(u, u.path[i - 1], u.path[i]);
        d["eta_minutes"] = (int)mins;
        d["route_km"] = (u.path.size() - u.path_i) * 0.5;
    }
    d["path"] = path;
    return d;
}

static bool own_unit(const murks::World &w, int id) {
    return id >= 0 && id < (int)w.units.size() && w.units[id].side == murks::SIDE_PLAYER;
}

int MurksWorld::order_move(int id, Vector2i hex, int mode) {
    if (!own_unit(world_, id)) return -1;
    world_.detach(id); // a team member given its own order leaves the formation
    return world_.order_move(id, {hex.x, hex.y}, (murks::MoveMode)std::max(0, std::min(mode, 3)));
}

int MurksWorld::order_fall_back(int id) {
    if (!own_unit(world_, id)) return -1;
    world_.detach(id);
    return world_.order_fall_back(id);
}
int MurksWorld::order_regroup(int id, bool rest) {
    if (!own_unit(world_, id)) return -1;
    world_.detach(id);
    return world_.order_regroup(id, rest);
}
int MurksWorld::set_stance(int id, int roe, int break_contact) {
    return own_unit(world_, id) ? world_.set_stance(id, roe, break_contact) : -1;
}

int MurksWorld::order_hold(int id) {
    if (id < 0 || id >= (int)world_.units.size() || world_.units[id].side != murks::SIDE_PLAYER) return -1;
    world_.detach(id);
    return world_.order_hold(id);
}

int MurksWorld::create_group(const PackedInt32Array &ids, int leader) {
    std::vector<int> v;
    for (int64_t i = 0; i < ids.size(); ++i) {
        if (!own_unit(world_, ids[i])) return -1;
        v.push_back(ids[i]);
    }
    return world_.create_group(v, leader);
}
bool MurksWorld::disband_group(int gid) { return world_.disband_group(gid); }
int MurksWorld::set_standoff(int id, int standoff) {
    if (!own_unit(world_, id)) return -1;
    return world_.set_standoff(id, standoff);
}
void MurksWorld::set_default_standoff(int standoff) {
    world_.default_standoff = (uint8_t)(standoff == murks::STANDOFF_WEAPONS ? standoff : std::max(0, std::min(standoff, 20)));
}
int MurksWorld::get_default_standoff() const { return world_.default_standoff; }
bool MurksWorld::set_group_shape(int gid, int shape, int spacing) { return world_.set_group_shape(gid, shape, spacing); }
int MurksWorld::order_group_move(int gid, Vector2i hex, int mode) {
    return world_.order_group_move(gid, {hex.x, hex.y}, (murks::MoveMode)std::max(0, std::min(mode, 3)));
}
int MurksWorld::order_group_hold(int gid) { return world_.order_group_hold(gid); }

Array MurksWorld::get_groups() const {
    static const char *kShape[murks::FS_COUNT] = {"Keep layout", "Column", "Line", "Wedge"};
    Array out;
    for (int i = 0; i < (int)world_.groups.size(); ++i) {
        const murks::Group &g = world_.groups[i];
        if (!g.alive || g.side != murks::SIDE_PLAYER) continue;
        Dictionary d;
        d["id"] = i;
        d["name"] = to_gd(g.name);
        d["color"] = (int)g.color;
        d["leader"] = g.leader;
        PackedInt32Array m;
        m.push_back(g.leader);
        for (int id : g.members) m.push_back(id);
        d["units"] = m;
        d["shape"] = (int)g.shape;
        d["shape_name"] = kShape[g.shape < murks::FS_COUNT ? g.shape : 0];
        d["spacing"] = g.spacing;
        d["zone_hexes"] = world_.command_zone(world_.units[g.leader]);
        out.push_back(d);
    }
    return out;
}

int MurksWorld::estimate_command_delay(int id) const {
    if (id < 0 || id >= (int)world_.units.size()) return -1;
    return world_.command_delay(world_.units[id]);
}

PackedVector2Array MurksWorld::get_visible_hexes(int id) const {
    PackedVector2Array out;
    if (id < 0 || id >= (int)world_.units.size() || world_.units[id].side != murks::SIDE_PLAYER) return out;
    std::vector<murks::HexCoord> hexes;
    world_.visible_hexes(id, hexes);
    out.resize((int64_t)hexes.size());
    Vector2 *p = out.ptrw();
    for (size_t i = 0; i < hexes.size(); ++i) {
        murks::Vec2 c = murks::hex_center(hexes[i]);
        p[i] = Vector2((real_t)c.x, (real_t)c.y);
    }
    return out;
}

Array MurksWorld::poll_events() {
    Array out;
    for (const auto &e : world_.drain_events()) {
        Dictionary d;
        d["time"] = (int)e.time;
        d["time_string"] = to_gd(murks::World::time_string_at(e.time));
        d["kind"] = event_kind_name(e.kind);
        d["unit"] = e.unit;
        d["pause"] = e.pause_worthy;
        d["text"] = to_gd(e.text);
        if (e.unit >= 0 && e.unit < (int)world_.units.size()) {
            const murks::Unit &u = world_.units[e.unit];
            bool visible = u.side == murks::SIDE_PLAYER || world_.seen_by(u, murks::SIDE_PLAYER) ||
                           e.kind == murks::EV_DESTROYED;
            murks::Vec2 c = murks::hex_center(visible ? u.pos : u.last_seen_pos[murks::SIDE_PLAYER]);
            d["pos"] = Vector2((real_t)c.x, (real_t)c.y);
        }
        out.push_back(d);
    }
    return out;
}

Dictionary MurksWorld::get_stats() const {
    const murks::SimStats &s = world_.stats();
    Dictionary d;
    d["avg_tick_us"] = s.avg_tick_us;
    d["max_tick_us"] = s.max_tick_us;
    d["hot"] = s.hot;
    d["warm"] = s.warm;
    d["cold"] = s.cold;
    d["alive"] = s.alive;
    d["ticks"] = (int64_t)s.ticks;
    d["los_checks"] = s.los_checks_last_tick;
    d["diesel_l"] = world_.stock.diesel_l;
    d["ammo_t"] = world_.stock.ammo_t;
    d["food_kg"] = world_.stock.food_kg;
    d["personnel"] = world_.stock.personnel;
    d["units_total"] = (int)world_.units.size();
    return d;
}
