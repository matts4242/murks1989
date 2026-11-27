class_name MapView
extends Node2D
## Draws the one map. The ground is a single sprite over the whole map painted pixel by
## pixel by shaders/terrain.gdshader from per-hex data and the baked height raster:
## forest canopy, field parcels, pasture, marsh, towns and water, with hill shading, the
## hex grid, and in topographic mode contour lines printed into the ground. Far out the
## shader falls back to the sprite's own texture (one average colour per hex, mipmapped)
## so nothing shimmers. Closer in, roads, rivers and railways are drawn over it as
## 64 x 64-hex chunk meshes built by the C++ core, streamed in as they come into view
## and freed when far away. A planning mode (MRK-25) lays a tint per hex over the ground:
## going, cover, slope from one whole-map image, line of sight from a small local one.

const CHUNK_HEXES := 64
const MAX_LOADED := 260          # chunk meshes kept before unloading far ones
const CHUNK_MODE_PX := 7.0       # hex size on screen from which roads, rivers and rail are drawn as lines
const GRID_MODE_PX := 14.0       # hex size on screen at which hex outlines start to show (faintly)
const GRID_FULL_PX := 30.0       # ... and at which they reach full strength
const GRID_STRENGTH := 0.32
const TOPO_CLOSE_PX := 20.0      # hex size on screen from which contours are every 20 m
const TOPO_FAR_PX := 3.0         # ... and below which every 500 m (100 m in between)
const NATURAL_EDGE_NOISE := 0.3  # how far terrain borders wander (0 = crisp hex borders)
const TERRAIN_SHADER := preload("res://shaders/terrain.gdshader")
# Planning modes, as murks::PlanMode in core/include/murks/planning.hpp.
const PLAN_OFF := 0
const PLAN_COVER := 4
const PLAN_SLOPE := 5
const PLAN_SIGHT := 6
const PLAN_COUNT := 7

var build_budget_ms := 6.0       # per frame
var world: MurksWorld
var ground: Sprite2D             # the painted ground
var terrain_mat: ShaderMaterial
var chunk_root: Node2D
var chunks := {}                 # Vector2i -> MeshInstance2D (roads, rivers, railways)
var relief_labels := {}          # Vector2i -> {"lod", "labels", "spots"}: height figures per chunk
var show_topo := false
var topo_lod := 0                # 0: contours every 20 m, heavier every 100 m; 1: 100 m / 500 m; 2: 500 m / 2,500 m
var show_grid := true
var show_detail := true          # painted textures (off: one flat colour per terrain)
var natural_edges := true        # terrain borders wander (off: crisp hex borders)
var plan_mode := PLAN_OFF        # the planning mode shown
var plan_built := -1             # the whole-map mode whose tint image is loaded
var plan_build_ms := 0.0
var chunk_count := Vector2i.ZERO
var chunk_size_m := Vector2.ZERO
var builds_last_frame := 0
var build_ms_last_frame := 0.0
var total_built := 0


func setup(w: MurksWorld) -> void:
	world = w
	chunk_count = w.get_chunk_count()
	chunk_size_m = Vector2(CHUNK_HEXES * w.get_hex_width(), CHUNK_HEXES * w.get_row_spacing())
	var info: Dictionary = w.get_map_info()

	var img: Image = w.build_overview_image()
	img.generate_mipmaps()
	ground = Sprite2D.new()
	ground.texture = ImageTexture.create_from_image(img)
	ground.centered = false
	ground.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS
	ground.scale = Vector2(w.get_hex_width(), w.get_row_spacing())
	# Pixel (c, r) sits under hex (c, r): offset so pixel centres line up with hex centres.
	var hex_size := w.get_hex_width() / sqrt(3.0)
	ground.position = Vector2(w.get_hex_width() * 0.25, hex_size - w.get_row_spacing() * 0.5)
	ground.z_index = -2
	terrain_mat = ShaderMaterial.new()
	terrain_mat.shader = TERRAIN_SHADER
	terrain_mat.set_shader_parameter("hex_data", ImageTexture.create_from_image(w.build_hex_data_image()))
	terrain_mat.set_shader_parameter("hex_count", Vector2i(int(info["cols"]), int(info["rows"])))
	ground.material = terrain_mat
	add_child(ground)

	chunk_root = Node2D.new()
	add_child(chunk_root)
	setup_relief()


## Loads the height raster of the map's relief file for hill shading and contours. Without
## a relief file the ground is painted unshaded and the topographic mode is not available.
func setup_relief() -> void:
	relief_labels.clear()
	terrain_mat.set_shader_parameter("has_height", false)
	if not world.has_contours():
		return
	var img: Image = world.get_height_image()
	if img == null or img.is_empty():
		return
	terrain_mat.set_shader_parameter("height_tex", ImageTexture.create_from_image(img))
	terrain_mat.set_shader_parameter("height_px", world.get_height_pixel_size())
	terrain_mat.set_shader_parameter("has_height", true)


## Shows a planning mode. Whole-map modes build their tint image once when chosen (a few
## hundred milliseconds on a large map); line of sight shows whatever set_sight last gave.
func set_plan_mode(mode: int) -> void:
	plan_mode = clampi(mode, PLAN_OFF, PLAN_COUNT - 1)
	if plan_mode == PLAN_OFF:
		terrain_mat.set_shader_parameter("plan_mode", 0)
	elif plan_mode == PLAN_SIGHT:
		terrain_mat.set_shader_parameter("plan_mode", 2)
	else:
		if plan_built != plan_mode:
			var t0 := Time.get_ticks_usec()
			var img: Image = world.build_plan_image(plan_mode)
			img.generate_mipmaps()
			terrain_mat.set_shader_parameter("plan_tex", ImageTexture.create_from_image(img))
			plan_built = plan_mode
			plan_build_ms = (Time.get_ticks_usec() - t0) / 1000.0
		terrain_mat.set_shader_parameter("plan_mode", 1)


## The line-of-sight tint from MurksWorld.get_viewshed.
func set_sight(view: Dictionary) -> void:
	if view.is_empty():
		return
	terrain_mat.set_shader_parameter("local_tex", ImageTexture.create_from_image(view["image"]))
	terrain_mat.set_shader_parameter("local_origin", view["origin"])
	terrain_mat.set_shader_parameter("local_size", view["size"])
	terrain_mat.set_shader_parameter("local_outside", view["outside"])


func has_relief() -> bool:
	return bool(terrain_mat.get_shader_parameter("has_height"))


func update_view(center: Vector2, zoom: float, viewport_size: Vector2) -> void:
	builds_last_frame = 0
	build_ms_last_frame = 0.0
	var px_per_hex := world.get_hex_width() * zoom
	var use_chunks := px_per_hex >= CHUNK_MODE_PX
	chunk_root.visible = use_chunks

	terrain_mat.set_shader_parameter("metres_per_pixel", 1.0 / zoom)
	terrain_mat.set_shader_parameter("detail", 1.0 if show_detail else 0.0)
	terrain_mat.set_shader_parameter("edge_noise", NATURAL_EDGE_NOISE if natural_edges else 0.0)
	var grid := clampf((px_per_hex - GRID_MODE_PX) / (GRID_FULL_PX - GRID_MODE_PX), 0.0, 1.0) * GRID_STRENGTH
	terrain_mat.set_shader_parameter("grid_alpha", grid if show_grid else 0.0)
	var want_topo := show_topo and has_relief()
	topo_lod = 0 if px_per_hex >= TOPO_CLOSE_PX else (1 if px_per_hex >= TOPO_FAR_PX else 2)
	terrain_mat.set_shader_parameter("contour_interval", [20.0, 100.0, 500.0][topo_lod] if want_topo else 0.0)
	if not use_chunks:
		relief_labels.clear()
		return

	var half := viewport_size * 0.5 / zoom
	var c0 := Vector2i(floori((center.x - half.x) / chunk_size_m.x) - 1, floori((center.y - half.y) / chunk_size_m.y) - 1)
	var c1 := Vector2i(floori((center.x + half.x) / chunk_size_m.x) + 1, floori((center.y + half.y) / chunk_size_m.y) + 1)
	c0 = c0.clamp(Vector2i.ZERO, chunk_count - Vector2i.ONE)
	c1 = c1.clamp(Vector2i.ZERO, chunk_count - Vector2i.ONE)

	var needed: Array[Vector2i] = []
	for cy in range(c0.y, c1.y + 1):
		for cx in range(c0.x, c1.x + 1):
			var key := Vector2i(cx, cy)
			if not chunks.has(key):
				needed.append(key)
			# Height figures for the chunks in view (cheap: a few dozen numbers per chunk).
			if want_topo and (not relief_labels.has(key) or int(relief_labels[key]["lod"]) != topo_lod):
				relief_labels[key] = {"lod": topo_lod,
					"labels": world.get_chunk_contour_labels(key, 100 if topo_lod == 0 else 500),
					"spots": world.get_chunk_spot_heights(key)}
	if not want_topo:
		relief_labels.clear()
	elif relief_labels.size() > 64:
		for key in relief_labels.keys():
			var k: Vector2i = key
			if k.x < c0.x or k.y < c0.y or k.x > c1.x or k.y > c1.y:
				relief_labels.erase(k)

	if not needed.is_empty():
		var mid := Vector2(center.x / chunk_size_m.x, center.y / chunk_size_m.y)
		needed.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
			return (Vector2(a) - mid).length_squared() < (Vector2(b) - mid).length_squared())
		var t0 := Time.get_ticks_usec()
		for key in needed:
			_build_chunk(key)
			builds_last_frame += 1
			build_ms_last_frame = (Time.get_ticks_usec() - t0) / 1000.0
			if build_ms_last_frame > build_budget_ms:
				break

	if chunks.size() > MAX_LOADED:
		_unload_outside(c0 - Vector2i(2, 2), c1 + Vector2i(2, 2))


func _build_chunk(key: Vector2i) -> void:
	var mi := MeshInstance2D.new()
	mi.mesh = world.build_chunk_mesh(key)
	mi.position = world.get_chunk_origin(key)
	chunk_root.add_child(mi)
	chunks[key] = mi
	total_built += 1


func _unload_outside(lo: Vector2i, hi: Vector2i) -> void:
	for key in chunks.keys():
		var k: Vector2i = key
		if k.x < lo.x or k.y < lo.y or k.x > hi.x or k.y > hi.y:
			(chunks[k] as Node).queue_free()
			chunks.erase(k)
