#include "murks_world.h"

#include "murks/geo.hpp"
#include "murks/planning.hpp"
#include "murks/render_data.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <cmath>
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
        case murks::EV_HOLDING: return "holding";
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
    ClassDB::bind_method(D_METHOD("load_contour_bytes", "bytes"), &MurksWorld::load_contour_bytes);
    ClassDB::bind_method(D_METHOD("has_contours"), &MurksWorld::has_contours);
    ClassDB::bind_method(D_METHOD("get_height_image"), &MurksWorld::get_height_image);
    ClassDB::bind_method(D_METHOD("get_height_pixel_size"), &MurksWorld::get_height_pixel_size);
    ClassDB::bind_method(D_METHOD("get_chunk_contour_labels", "chunk", "step"), &MurksWorld::get_chunk_contour_labels);
    ClassDB::bind_method(D_METHOD("get_chunk_spot_heights", "chunk"), &MurksWorld::get_chunk_spot_heights);
    ClassDB::bind_method(D_METHOD("build_overview_image"), &MurksWorld::build_overview_image);
    ClassDB::bind_method(D_METHOD("build_hex_data_image"), &MurksWorld::build_hex_data_image);
    ClassDB::bind_method(D_METHOD("build_plan_image", "mode"), &MurksWorld::build_plan_image);
    ClassDB::bind_method(D_METHOD("get_plan_key", "mode"), &MurksWorld::get_plan_key);
    ClassDB::bind_method(D_METHOD("get_plan_mode_name", "mode"), &MurksWorld::get_plan_mode_name);
    ClassDB::bind_method(D_METHOD("get_viewshed", "hex", "eye_m"), &MurksWorld::get_viewshed);
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
    ClassDB::bind_method(D_METHOD("order_dismount", "id"), &MurksWorld::order_dismount);
    ClassDB::bind_method(D_METHOD("order_mount", "id"), &MurksWorld::order_mount);
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
    ClassDB::bind_method(D_METHOD("order_rejoin", "id"), &MurksWorld::order_rejoin);
    ClassDB::bind_method(D_METHOD("get_holdings"), &MurksWorld::get_holdings);
    ClassDB::bind_method(D_METHOD("get_holding", "hid"), &MurksWorld::get_holding);
    ClassDB::bind_method(D_METHOD("holding_at", "hex"), &MurksWorld::holding_at);
    ClassDB::bind_method(D_METHOD("set_training", "hid", "on"), &MurksWorld::set_training);
    ClassDB::bind_method(D_METHOD("levy", "hid", "kind"), &MurksWorld::levy);
    ClassDB::bind_method(D_METHOD("set_supply_route", "source", "dest"), &MurksWorld::set_supply_route);
    ClassDB::bind_method(D_METHOD("cancel_supply_route", "rid"), &MurksWorld::cancel_supply_route);
    ClassDB::bind_method(D_METHOD("get_supply_routes"), &MurksWorld::get_supply_routes);
    ClassDB::bind_method(D_METHOD("is_orderable", "id"), &MurksWorld::is_orderable);
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
    contours_.clear(); // they belong to the map that was loaded before
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

// Copies core triangle geometry (positions, vertex colours, indices) into a Godot mesh.
static void fill_triangle_mesh(const murks::ChunkGeometry &g, Ref<ArrayMesh> &mesh) {
    const int64_t nv = (int64_t)g.xy.size() / 2;
    if (nv == 0 || g.indices.empty()) return;
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
}

// Roads, rivers and railways of a chunk (the ground itself is painted by the terrain shader).
Ref<ArrayMesh> MurksWorld::build_chunk_mesh(Vector2i chunk) const {
    Ref<ArrayMesh> mesh;
    mesh.instantiate();
    if (!generated_ || chunk.x < 0 || chunk.y < 0 || chunk.x >= world_.map.chunks_x() ||
        chunk.y >= world_.map.chunks_y())
        return mesh;
    murks::ChunkGeometry g;
    murks::build_chunk_lines(world_.map, chunk.x, chunk.y, g);
    fill_triangle_mesh(g, mesh);
    return mesh;
}

// The relief file must have been baked for this map: same number of chunks.
bool MurksWorld::load_contour_bytes(const PackedByteArray &bytes) {
    if (!generated_ || !contours_.load(bytes.ptr(), (size_t)bytes.size())) {
        contours_.clear();
        return false;
    }
    if (contours_.chunks_x() != world_.map.chunks_x() || contours_.chunks_y() != world_.map.chunks_y()) {
        contours_.clear();
        return false;
    }
    return true;
}

bool MurksWorld::has_contours() const {
    return contours_.loaded();
}

// The height raster as a 16-bit float image (metres), for the shader that draws the
// height bands. Empty if no relief file is loaded.
Ref<Image> MurksWorld::get_height_image() const {
    Ref<Image> img;
    if (!contours_.loaded()) return img;
    std::vector<uint16_t> half;
    contours_.heights_half(half);
    PackedByteArray bytes;
    bytes.resize((int64_t)half.size() * 2);
    std::memcpy(bytes.ptrw(), half.data(), half.size() * 2);
    return Image::create_from_data(contours_.raster_w(), contours_.raster_h(), false, Image::FORMAT_RH, bytes);
}

float MurksWorld::get_height_pixel_size() const {
    return contours_.raster_pixel();
}

// [x, y, angle, height] per height figure, in world metres, for contours that are multiples of `step`.
PackedFloat32Array MurksWorld::get_chunk_contour_labels(Vector2i chunk, int step) const {
    PackedFloat32Array out;
    std::vector<murks::ContourLabel> labels;
    contours_.labels(chunk.x, chunk.y, step, labels);
    double ox, oy;
    murks::chunk_origin(chunk.x, chunk.y, ox, oy);
    out.resize((int64_t)labels.size() * 4);
    float *p = out.ptrw();
    for (size_t i = 0; i < labels.size(); ++i) {
        p[i * 4] = (float)(ox + labels[i].x);
        p[i * 4 + 1] = (float)(oy + labels[i].y);
        p[i * 4 + 2] = labels[i].angle;
        p[i * 4 + 3] = (float)labels[i].level;
    }
    return out;
}

// [x, y, height] per spot height, in world metres.
PackedFloat32Array MurksWorld::get_chunk_spot_heights(Vector2i chunk) const {
    PackedFloat32Array out;
    std::vector<murks::SpotHeight> spots;
    contours_.spots(chunk.x, chunk.y, spots);
    double ox, oy;
    murks::chunk_origin(chunk.x, chunk.y, ox, oy);
    out.resize((int64_t)spots.size() * 3);
    float *p = out.ptrw();
    for (size_t i = 0; i < spots.size(); ++i) {
        p[i * 3] = (float)(ox + spots[i].x);
        p[i * 3 + 1] = (float)(oy + spots[i].y);
        p[i * 3 + 2] = (float)spots[i].elev;
    }
    return out;
}

// Planning map (MRK-25): the tint of a whole-map mode, one RGBA8 pixel per hex.
Ref<Image> MurksWorld::build_plan_image(int mode) const {
    std::vector<uint8_t> rgba;
    murks::build_plan_rgba(world_, mode, rgba);
    PackedByteArray bytes;
    bytes.resize((int64_t)rgba.size());
    std::memcpy(bytes.ptrw(), rgba.data(), rgba.size());
    return Image::create_from_data(world_.map.cols(), world_.map.rows(), false, Image::FORMAT_RGBA8, bytes);
}

// The colour key of a mode: [Color tint (alpha = strength, 0 for a note row), label, note] per row.
Array MurksWorld::get_plan_key(int mode) const {
    std::vector<murks::PlanKey> key;
    murks::plan_key(mode, key);
    Array out;
    for (const auto &k : key) {
        Array row;
        row.append(Color((k.rgba & 255) / 255.0f, ((k.rgba >> 8) & 255) / 255.0f, ((k.rgba >> 16) & 255) / 255.0f,
                         ((k.rgba >> 24) & 255) / 255.0f));
        row.append(to_gd(k.label));
        row.append(to_gd(k.note));
        out.append(row);
    }
    return out;
}

String MurksWorld::get_plan_mode_name(int mode) const {
    return to_gd(std::string(murks::plan_mode_name(mode)));
}

// Line of sight from a hex at an eye height to 12 km, as the rules check it (target 2.5 m).
// {origin: Vector2i, size: Vector2i, image: Image (RGBA8 tint per hex), seen: int, total: int}
Dictionary MurksWorld::get_viewshed(Vector2i hex, float eye_m) const {
    Dictionary d;
    murks::HexCoord from{hex.x, hex.y};
    if (!generated_ || !world_.map.in_bounds(from)) return d;
    murks::Viewshed vs;
    murks::viewshed(world_, from, eye_m, murks::SPOT_TARGET_M, murks::SIGHT_RADIUS_HEXES, vs);
    PackedByteArray bytes;
    bytes.resize((int64_t)vs.rgba.size());
    std::memcpy(bytes.ptrw(), vs.rgba.data(), vs.rgba.size());
    d["origin"] = Vector2i(vs.origin.col, vs.origin.row);
    d["size"] = Vector2i(vs.w, vs.h);
    d["image"] = Image::create_from_data(vs.w, vs.h, false, Image::FORMAT_RGBA8, bytes);
    // 0 out of range, 1 dead ground, 2 seen, per hex of the box (for the status bar).
    PackedByteArray mask;
    mask.resize((int64_t)vs.w * vs.h);
    const uint32_t seen_t = murks::sight_tint(0), dead_t = murks::sight_tint(1);
    for (int64_t i = 0; i < (int64_t)vs.w * vs.h; ++i) {
        const uint8_t *px = &vs.rgba[(size_t)i * 4];
        uint32_t t = px[0] | (px[1] << 8) | (px[2] << 16) | ((uint32_t)px[3] << 24);
        mask.set(i, t == seen_t ? 2 : (t == dead_t ? 1 : 0));
    }
    d["mask"] = mask;
    d["seen"] = vs.seen;
    d["total"] = vs.total;
    d["outside"] = Color(((murks::sight_tint(2)) & 255) / 255.0f, ((murks::sight_tint(2) >> 8) & 255) / 255.0f,
                         ((murks::sight_tint(2) >> 16) & 255) / 255.0f, ((murks::sight_tint(2) >> 24) & 255) / 255.0f);
    return d;
}

// Per-hex terrain, density, elevation and cover for the terrain shader (one pixel per hex).
Ref<Image> MurksWorld::build_hex_data_image() const {
    std::vector<uint8_t> rgba;
    murks::build_hex_data_rgba(world_.map, rgba);
    PackedByteArray bytes;
    bytes.resize((int64_t)rgba.size());
    std::memcpy(bytes.ptrw(), rgba.data(), rgba.size());
    return Image::create_from_data(world_.map.cols(), world_.map.rows(), false, Image::FORMAT_RGBA8, bytes);
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
    {
        // Steepest grade to a neighbouring hex, and which way the ground falls.
        static const char *kDir[6] = {"east", "north-east", "north-west", "west", "south-west", "south-east"};
        int dir;
        double pct = murks::hex_slope(world_.map, hex.x, hex.y, dir);
        d["slope_percent"] = pct;
        d["slope_degrees"] = std::atan(pct / 100.0) * 180.0 / 3.14159265358979323846;
        d["slope_class"] = murks::slope_class(pct);
        d["slope_down"] = dir >= 0 ? to_gd(std::string(kDir[dir])) : String();
    }
    {
        // The planning map's figures for this hex: going off-road per mobility (minutes),
        // cover factors and whether it hides a unit that sits still.
        const int idx = world_.map.index(h);
        d["going_tracked"] = murks::hex_going(world_, idx, murks::M_TRACKED);
        d["going_wheeled"] = murks::hex_going(world_, idx, murks::M_WHEELED);
        d["going_foot"] = murks::hex_going(world_, idx, murks::M_FOOT);
        d["cover_vehicles"] = murks::terrain_cover_factor(x.terrain, false);
        d["cover_infantry"] = murks::terrain_cover_factor(x.terrain, true);
        d["conceals"] = murks::terrain_conceals(x.terrain);
    }
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
    // Who runs the trains once the HQ is lost: "" while the HQ lives or during the handover.
    const int acting = world_.acting_hq(0);
    d["acting_hq"] = acting >= 0 && world_.units[acting].type != murks::U_HQ ? to_gd(world_.units[acting].name) : String();
    d["handover_minutes"] = world_.handover_minutes_left(0); // -1 = nobody left to take over
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

// -1 top of the chain, 0 no commander, 1 inside the zone, 2 outside it, 3 beyond radio range:
// the same tests as World::command_delay.
int MurksWorld::command_link(const murks::Unit &u, int boss) const {
    const bool member = u.group >= 0 && u.group < (int)world_.groups.size() && world_.groups[u.group].alive &&
                        world_.groups[u.group].leader != u.id;
    if (u.type == murks::U_HQ && !member) return -1;
    if (boss < 0) return 0;
    const murks::Unit &h = world_.units[boss];
    const int dist = murks::hex_distance(u.pos, h.pos);
    if (dist > murks::World::radio_range(u)) return 3;
    return dist > world_.command_zone(h) ? 2 : 1;
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
        q[SF_ORDER] = !own ? -1.0f : u.mounting ? 5.0f : (float)u.order;
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
        q[SF_CMD_BOSS] = -1.0f;
        q[SF_CMD_LINK] = -1.0f;
        q[SF_FS_ROLE] = -1.0f;
        q[SF_FS_STATUS] = 0.0f;
        q[SF_FS_BATTERY] = -1.0f;
        q[SF_FS_VIA] = -1.0f;
        q[SF_FS_RANGE] = 0.0f;
        q[SF_CARRY] = -1.0f;
        q[SF_MOUNT_LINK] = -1.0f;
        q[SF_SUPP] = own ? u.suppression + (u.pinned ? 1000.0f : 0.0f) : -1.0f;
        q[SF_MILITIA] = shown && u.militia ? 1.0f : 0.0f;
        q[SF_CONVOY] = own && u.type == murks::U_TRUCK ? (float)u.convoy_state : -1.0f;
        q[SF_CARGO_T] = own && u.type == murks::U_TRUCK
                            ? (float)(u.cargo_l / murks::DIESEL_L_PER_T + u.cargo_t + u.cargo_kg / 1000.0)
                            : -1.0f;
        if (own && u.dismounts >= 0) {
            const murks::Unit &dm = world_.units[u.dismounts];
            q[SF_CARRY] = dm.embarked ? (float)dm.elements : 0.0f;
            if (dm.alive) q[SF_MOUNT_LINK] = (float)dm.id;
        } else if (own && u.carrier >= 0 && world_.units[u.carrier].alive) {
            q[SF_MOUNT_LINK] = (float)u.carrier;
        }
        if (own) {
            const int boss = world_.hq_above(u);
            q[SF_CMD_BOSS] = (float)boss;
            q[SF_CMD_LINK] = (float)command_link(u, boss);
            const murks::FireNetLink fl = world_.fire_net_link(u);
            q[SF_FS_ROLE] = (float)fl.role;
            q[SF_FS_STATUS] = (float)fl.status;
            q[SF_FS_BATTERY] = (float)fl.battery;
            q[SF_FS_VIA] = (float)fl.via;
            q[SF_FS_RANGE] = fl.role == murks::FN_GUNS ? (float)murks::World::weapon_range(u) : 0.0f;
        }
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
        d["engage_supp"] = et.f_supp;
        d["suppression"] = u.suppression;
        d["pinned"] = u.pinned;
        d["engage_night"] = et.f_night;
        static const char *kNoun[murks::U_TYPE_COUNT] = {"vehicles", "vehicles", "vehicles", "squads", "vehicles", "guns", "trucks"};
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
    d["weapon_range_km"] = murks::World::weapon_range(u) * 0.5;
    if (const murks::GunInfo *g = ti.indirect ? murks::gun_info(u.equipment) : nullptr) {
        d["gun_name"] = g->label;
        d["gun_range_km"] = g->range_km;
    }
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
        d["command_link"] = command_link(u, hq);
        // Mounted infantry.
        d["can_dismount"] = world_.can_dismount(u);
        d["can_mount"] = world_.can_mount(u);
        if (u.dismounts >= 0) {
            const murks::Unit &dm = world_.units[u.dismounts];
            d["squads_max"] = dm.elements_max;
            d["squad_men"] = dm.squad_men;
            d["squads_aboard"] = dm.embarked ? dm.elements : 0;
            d["squads_out"] = dm.alive;
            if (dm.alive) {
                d["squads_name"] = to_gd(dm.name);
                d["squads_km"] = murks::hex_distance(u.pos, dm.pos) * 0.5;
                d["squads_count"] = dm.elements;
                d["squads_mounting"] = dm.mounting;
            }
        }
        if (u.carrier >= 0) {
            const murks::Unit &cv = world_.units[u.carrier];
            d["carrier_id"] = u.carrier;
            d["carrier_name"] = to_gd(cv.name);
            d["carrier_alive"] = cv.alive;
            d["carrier_km"] = murks::hex_distance(u.pos, cv.pos) * 0.5;
            d["carrier_vehicles"] = cv.elements;
            d["mounting"] = u.mounting;
        }
        d["radio_km"] = murks::World::radio_range(u) * 0.5;
        // Fire support net.
        const murks::FireNetLink fl = world_.fire_net_link(u);
        d["fs_role"] = (int)fl.role;
        if (fl.role != murks::FN_NONE) {
            d["fs_status"] = (int)fl.status;
            if (fl.battery >= 0) {
                d["fs_battery"] = to_gd(world_.units[fl.battery].name);
                d["fs_battery_km"] = murks::hex_distance(u.pos, world_.units[fl.battery].pos) * 0.5;
            }
            if (fl.via >= 0) {
                d["fs_via"] = to_gd(world_.units[fl.via].name);
                d["fs_via_km"] = fl.distance * 0.5;
            }
            d["fs_relay_min"] = murks::FIRE_RELAY_MIN;
            if (fl.role == murks::FN_GUNS) {
                int obs = 0, relayed = 0, cut = 0;
                for (int id : world_.formations[u.formation].members) {
                    const murks::Unit &m = world_.units[id];
                    if (!m.alive || id == u.id) continue;
                    const murks::FireNetLink ml = world_.fire_net_link(m);
                    if (ml.role == murks::FN_GUNS || ml.role == murks::FN_NONE) continue;
                    if (ml.status == murks::FN_DIRECT) ++obs;
                    else if (ml.status == murks::FN_RELAY) ++relayed;
                    else ++cut;
                }
                d["fs_direct_count"] = obs;
                d["fs_relay_count"] = relayed;
                d["fs_cut_count"] = cut;
            }
        }
        // Stand-off: the unit's own setting (255 = company default, 254 = weapon range, 0 = fight).
        d["standoff_setting"] = (int)u.standoff;
        d["standoff_hexes"] = world_.standoff_hexes(u);
        d["standoff_pending"] = u.pending_stance.active && u.pending_stance.standoff != u.standoff;
        if (u.pending_stance.active) d["pending_standoff"] = (int)u.pending_stance.standoff;
        // Holdings and supply.
        d["orderable"] = world_.orderable(u);
        if (u.holding >= 0 && u.holding < (int)world_.holdings.size()) d["holding"] = u.holding;
        if (u.type == murks::U_TRUCK) {
            d["convoy_state"] = (int)u.convoy_state;
            d["convoy_state_name"] = murks::convoy_state_name(u.convoy_state);
            d["convoy_route"] = u.convoy_route;
            d["cargo_l"] = u.cargo_l;
            d["cargo_t"] = u.cargo_t;
            d["cargo_kg"] = u.cargo_kg;
            d["cargo_tonnes"] = u.cargo_l / murks::DIESEL_L_PER_T + u.cargo_t + u.cargo_kg / 1000.0;
            d["capacity_t"] = world_.convoy_capacity_t(u);
            if (u.convoy_route >= 0 && u.convoy_route < (int)world_.routes.size()) {
                const murks::SupplyRoute &r = world_.routes[u.convoy_route];
                d["route_source"] = r.source >= 0 ? to_gd(world_.holdings[r.source].name) : String();
                d["route_dest"] = r.dest >= 0 ? to_gd(world_.holdings[r.dest].name) : String("the trains");
            }
        }
    }
    d["militia"] = u.militia;
    d["pending"] = u.pending.active;
    if (u.pending.active) {
        static const char *kPend[8] = {"Hold", "Move", "Fall back", "Regroup", "Rest & resupply", "Dismount",
                                       "Mount up", "Rejoin team"};
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
    if (!own_unit(world_, id)) return -1; // a team member's own order detaches it when it arrives (core)
    return world_.order_move(id, {hex.x, hex.y}, (murks::MoveMode)std::max(0, std::min(mode, 3)));
}

int MurksWorld::order_fall_back(int id) {
    if (!own_unit(world_, id)) return -1;
    return world_.order_fall_back(id);
}
int MurksWorld::order_regroup(int id, bool rest) {
    if (!own_unit(world_, id)) return -1;
    return world_.order_regroup(id, rest);
}
int MurksWorld::order_dismount(int id) {
    return own_unit(world_, id) ? world_.order_dismount(id) : -1;
}
int MurksWorld::order_mount(int id) {
    if (!own_unit(world_, id)) return -1;
    return world_.order_mount(id);
}
int MurksWorld::set_stance(int id, int roe, int break_contact) {
    return own_unit(world_, id) ? world_.set_stance(id, roe, break_contact) : -1;
}

int MurksWorld::order_hold(int id) {
    if (id < 0 || id >= (int)world_.units.size() || world_.units[id].side != murks::SIDE_PLAYER) return -1;
    return world_.order_hold(id);
}

int MurksWorld::order_rejoin(int id) { return own_unit(world_, id) ? world_.order_rejoin(id) : -1; }

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
    d["cash"] = world_.stock.cash;
    d["replacements"] = world_.stock.replacements;
    d["days_diesel"] = world_.days_of_diesel();
    d["days_ammo"] = world_.days_of_ammo();
    d["days_food"] = world_.days_of_food();
    d["diesel_lph"] = world_.stock.rate_diesel_lph;
    d["ammo_tph"] = world_.stock.rate_ammo_tph;
    int owned = 0, trucks = 0, routes = 0;
    for (const auto &h : world_.holdings)
        if (h.owner == murks::SIDE_PLAYER) ++owned;
    for (const auto &u : world_.units)
        if (u.alive && u.side == murks::SIDE_PLAYER && u.type == murks::U_TRUCK) trucks += u.elements;
    for (const auto &r : world_.routes)
        if (r.active) ++routes;
    d["holdings_owned"] = owned;
    d["trucks"] = trucks;
    d["routes_active"] = routes;
    d["units_total"] = (int)world_.units.size();
    return d;
}

// --- Holdings and supply routes ---------------------------------------------------------

static const char *levy_noun(int k) {
    static const char *n[murks::LV_COUNT] = {"Food", "Fuel", "Cash", "Men"};
    return n[k < 0 || k >= murks::LV_COUNT ? 0 : k];
}

Dictionary MurksWorld::get_holding(int hid) const {
    Dictionary d;
    if (hid < 0 || hid >= (int)world_.holdings.size()) return d;
    const murks::Holding &h = world_.holdings[hid];
    const bool own = h.owner == murks::SIDE_PLAYER;
    d["id"] = hid;
    d["name"] = to_gd(h.name);
    d["kind"] = (int)h.kind;
    d["kind_name"] = murks::holding_kind_name(h.kind);
    murks::Vec2 c = murks::hex_center(h.pos);
    d["pos"] = Vector2((real_t)c.x, (real_t)c.y);
    d["hex"] = Vector2i(h.pos.col, h.pos.row);
    d["radius"] = h.radius;
    d["population"] = h.population;
    // The owner as the player knows it: their own places exactly, the rest as last seen.
    d["owner"] = (int)h.known_owner;
    d["owner_name"] = h.known_owner == murks::SIDE_PLAYER ? "Company Murks" : h.known_owner == murks::SIDE_HOSTILE ? "Hostile" : "Independent";
    d["own"] = own;
    d["location"] = to_gd(world_.place_name(h.pos));
    // Garrison: own militia exactly; others' when seen.
    int militia = 0;
    bool militia_seen = false;
    if (h.militia_unit >= 0 && h.militia_unit < (int)world_.units.size()) {
        const murks::Unit &m = world_.units[h.militia_unit];
        if (m.alive && (own || world_.seen_by(m, murks::SIDE_PLAYER))) {
            militia = m.elements;
            militia_seen = true;
        }
    }
    d["militia_squads"] = militia;
    d["militia_seen"] = militia_seen;
    d["militia_max"] = world_.militia_max_squads(h);
    d["militia_unit"] = own ? h.militia_unit : -1;
    d["present_own"] = world_.present_in(h, murks::SIDE_PLAYER);
    d["contested"] = h.contested_since >= 0 && own;
    if (h.contested_since >= 0 && own) d["contested_minutes"] = (int)(world_.time_min - h.contested_since);
    d["captured_minutes_ago"] = (int)(world_.time_min - h.captured_at);
    d["fighting"] = world_.time_min - h.last_fight < 60;
    if (!own) return d;
    d["loyalty"] = h.loyalty;
    d["diesel_l"] = h.diesel_l;
    d["ammo_t"] = h.ammo_t;
    d["food_kg"] = h.food_kg;
    d["cash"] = h.cash;
    d["manpower"] = h.manpower;
    double cash = 0, food = 0;
    world_.daily_yield(h, cash, food);
    d["cash_per_day"] = cash;
    d["food_per_day"] = food;
    d["training"] = h.training;
    d["training_progress"] = h.training_progress;
    d["can_train"] = h.kind == murks::HK_TOWN && world_.militia_max_squads(h) > 0 && h.loyalty >= 25.0f &&
                     world_.militia_squads(h) < world_.militia_max_squads(h);
    d["train_days"] = murks::MILITIA_TRAIN_MIN / 1440;
    d["squad_men"] = murks::MILITIA_SQUAD_MEN;
    d["can_levy"] = world_.can_levy(h, murks::SIDE_PLAYER) && (h.kind == murks::HK_TOWN || h.kind == murks::HK_FARM);
    Array levies;
    for (int k = 0; k < murks::LV_COUNT; ++k) {
        Dictionary l;
        l["kind"] = k;
        l["name"] = levy_noun(k);
        const int64_t wait = h.levy_until[k] - world_.time_min;
        l["hours_until"] = wait > 0 ? (int)((wait + 59) / 60) : 0;
        bool possible = h.kind == murks::HK_TOWN || (h.kind == murks::HK_FARM && k == murks::LV_FOOD);
        if (k == murks::LV_MEN && h.manpower <= 0) possible = false;
        l["possible"] = possible;
        double amount = 0;
        switch (k) {
            case murks::LV_FOOD: amount = h.population * (h.kind == murks::HK_FARM ? 0.5 : 0.1); break;
            case murks::LV_FUEL: amount = h.population * 0.5; break;
            case murks::LV_CASH: amount = h.population * 2.0; break;
            case murks::LV_MEN: amount = h.manpower; break;
            default: break;
        }
        l["amount"] = amount;
        l["loyalty_cost"] = (k == murks::LV_CASH || k == murks::LV_MEN) ? 15 : 10;
        levies.push_back(l);
    }
    d["levies"] = levies;
    // The trains parked here draw from the stores.
    const int hq = world_.acting_hq(0);
    d["trains_here"] = hq >= 0 && !world_.units[hq].moving() &&
                       murks::hex_distance(h.pos, world_.units[hq].pos) <= std::max(3, h.radius);
    PackedInt32Array rs;
    for (int i = 0; i < (int)world_.routes.size(); ++i) {
        const murks::SupplyRoute &r = world_.routes[i];
        if ((r.active || r.suspended_at >= 0) && (r.source == hid || r.dest == hid)) rs.push_back(i);
    }
    d["routes"] = rs;
    return d;
}

Array MurksWorld::get_holdings() const {
    Array out;
    for (int i = 0; i < (int)world_.holdings.size(); ++i) out.push_back(get_holding(i));
    return out;
}

int MurksWorld::holding_at(Vector2i hex) const { return world_.holding_at({hex.x, hex.y}); }
bool MurksWorld::set_training(int hid, bool on) { return world_.set_training(hid, on); }
String MurksWorld::levy(int hid, int kind) { return to_gd(world_.levy(hid, kind)); }

Dictionary MurksWorld::set_supply_route(int source, int dest) {
    Dictionary d;
    std::string err;
    d["id"] = world_.set_supply_route(source, dest, &err);
    d["error"] = to_gd(err);
    return d;
}

bool MurksWorld::cancel_supply_route(int rid) { return world_.cancel_supply_route(rid); }

bool MurksWorld::is_orderable(int id) const {
    return id >= 0 && id < (int)world_.units.size() && world_.units[id].alive && world_.orderable(world_.units[id]);
}

Array MurksWorld::get_supply_routes() const {
    Array out;
    const int hq = world_.acting_hq(0);
    for (int i = 0; i < (int)world_.routes.size(); ++i) {
        const murks::SupplyRoute &r = world_.routes[i];
        if (r.side != murks::SIDE_PLAYER || r.source < 0 || r.source >= (int)world_.holdings.size()) continue;
        const bool working = r.convoy >= 0 && r.convoy < (int)world_.units.size() && world_.units[r.convoy].alive &&
                             world_.units[r.convoy].convoy_route == i;
        if (!r.active && r.suspended_at < 0 && !working) continue; // cancelled, and its column is home
        if (!r.active && !working && r.suspended_at >= 0 && world_.time_min - r.suspended_at > 1440) continue;
        Dictionary d;
        d["id"] = i;
        d["active"] = r.active;
        d["suspended"] = !r.active && r.suspended_at >= 0;
        d["source"] = r.source;
        d["dest"] = r.dest;
        d["source_name"] = to_gd(world_.holdings[r.source].name);
        d["dest_name"] = r.dest >= 0 ? to_gd(world_.holdings[r.dest].name) : String("the trains");
        murks::Vec2 sc = murks::hex_center(world_.holdings[r.source].pos);
        d["source_pos"] = Vector2((real_t)sc.x, (real_t)sc.y);
        murks::HexCoord dh = r.dest >= 0 ? world_.holdings[r.dest].pos : (hq >= 0 ? world_.units[hq].pos : world_.holdings[r.source].pos);
        murks::Vec2 dc = murks::hex_center(dh);
        d["dest_pos"] = Vector2((real_t)dc.x, (real_t)dc.y);
        d["convoy"] = working ? r.convoy : -1;
        d["trips"] = r.trips;
        d["delivered_l"] = r.delivered_l;
        d["delivered_t"] = r.delivered_t;
        d["delivered_kg"] = r.delivered_kg;
        d["round_trip_min"] = r.last_round_trip_min;
        d["distance_km"] = murks::hex_distance(world_.holdings[r.source].pos, dh) * 0.5;
        if (working) {
            const murks::Unit &c = world_.units[r.convoy];
            d["convoy_name"] = to_gd(c.name);
            d["state"] = (int)c.convoy_state;
            d["state_name"] = murks::convoy_state_name(c.convoy_state);
            d["trucks"] = c.elements;
            d["cargo_tonnes"] = c.cargo_l / murks::DIESEL_L_PER_T + c.cargo_t + c.cargo_kg / 1000.0;
            d["capacity_t"] = world_.convoy_capacity_t(c);
            d["under_fire"] = world_.time_min - c.last_targeted < 15 || world_.time_min - c.last_hit < 15;
            d["halted"] = !c.moving() && (c.convoy_state == murks::CV_TO_SOURCE || c.convoy_state == murks::CV_TO_DEST ||
                                           c.convoy_state == murks::CV_RETURN);
            d["tonnes_per_day"] = r.last_round_trip_min > 0 ? world_.convoy_capacity_t(c) * 1440.0 / r.last_round_trip_min : 0.0;
            d["convoy_pos"] = unit_world_pos(c);
            PackedVector2Array path;
            if (c.moving()) {
                path.push_back(unit_world_pos(c));
                const size_t step = std::max<size_t>(1, (c.path.size() - c.path_i) / 400);
                for (size_t k = c.path_i; k < c.path.size(); k += step) {
                    murks::Vec2 cc = murks::hex_center(world_.map.coord_of(c.path[k]));
                    path.push_back(Vector2((real_t)cc.x, (real_t)cc.y));
                }
                murks::Vec2 last = murks::hex_center(world_.map.coord_of(c.path.back()));
                path.push_back(Vector2((real_t)last.x, (real_t)last.y));
            }
            d["path"] = path;
        } else {
            d["state"] = -1;
            d["state_name"] = r.active ? "No column" : "Suspended";
            d["tonnes_per_day"] = 0.0;
        }
        out.push_back(d);
    }
    return out;
}
