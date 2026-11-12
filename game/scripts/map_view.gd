class_name MapView
extends Node2D
## Draws the one map. Far out it shows a one-pixel-per-hex overview texture;
## closer in it streams 64 x 64-hex chunk meshes built by the C++ core,
## creating chunks that come into view and freeing ones far away. Closer still a
## detail layer adds a pattern inside every hex (tree crowns, building blocks,
## furrows, marsh marks), rail sleepers and bridges. The topographic mode keeps
## all of that and lays over it a slope tint with downhill wedges and contour
## lines baked from the elevation data, drawn as darker ground, not as a separate sheet.

const CHUNK_HEXES := 64
const BUILD_BUDGET_MS := 6.0     # per frame
const MAX_LOADED := 260          # chunk meshes kept before unloading far ones
const CHUNK_MODE_PX := 7.0       # hex size on screen at which chunks replace the overview
const GRID_MODE_PX := 22.0       # hex size on screen at which hex outlines appear
const DETAIL_MODE_PX := 16.0     # hex size on screen at which terrain patterns appear
const DETAIL_FULL_PX := 26.0     # ... and at which they reach full strength
const TOPO_CLOSE_PX := 20.0      # hex size on screen from which every 20 m line is drawn
const TOPO_DETAIL_ALPHA := 0.6    # terrain patterns step back a little under the contours

var world: MurksWorld
var overview: Sprite2D
var chunk_root: Node2D
var detail_root: Node2D
var grid_root: Node2D
var chunks := {}                 # Vector2i -> MeshInstance2D
var details := {}                # Vector2i -> MeshInstance2D
var grids := {}                  # Vector2i -> MeshInstance2D
var slope_root: Node2D           # slope tint and downhill wedges in topographic mode
var slopes := {}                 # Vector2i -> MeshInstance2D
var contour_root: Node2D
var contours := {}               # Vector2i -> {"lod", "nodes", "twin", "labels", "spots"}
var show_topo := false
var topo_lod := 0                # 0: lines every 20 m, index 100 m; 1: every 100 m, index 500 m
var chunk_count := Vector2i.ZERO
var chunk_size_m := Vector2.ZERO
var show_grid := true
var show_detail := true
var builds_last_frame := 0
var build_ms_last_frame := 0.0
var total_built := 0


func setup(w: MurksWorld) -> void:
	world = w
	chunk_count = w.get_chunk_count()
	chunk_size_m = Vector2(CHUNK_HEXES * w.get_hex_width(), CHUNK_HEXES * w.get_row_spacing())

	var img: Image = w.build_overview_image()
	overview = Sprite2D.new()
	overview.texture = ImageTexture.create_from_image(img)
	overview.centered = false
	overview.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
	overview.scale = Vector2(w.get_hex_width(), w.get_row_spacing())
	# Pixel (c, r) sits under hex (c, r): offset so pixel centres line up with hex centres.
	var hex_size := w.get_hex_width() / sqrt(3.0)
	overview.position = Vector2(w.get_hex_width() * 0.25, hex_size - w.get_row_spacing() * 0.5)
	overview.z_index = -2
	add_child(overview)

	chunk_root = Node2D.new()
	add_child(chunk_root)
	detail_root = Node2D.new()
	add_child(detail_root)
	slope_root = Node2D.new()   # over the patterns, so its tint and wedges read through woods and towns
	slope_root.visible = false
	add_child(slope_root)
	contour_root = Node2D.new()
	add_child(contour_root)
	grid_root = Node2D.new()
	grid_root.z_index = 1
	grid_root.modulate = Color(0.05, 0.05, 0.05, 0.22)
	add_child(grid_root)


func update_view(center: Vector2, zoom: float, viewport_size: Vector2) -> void:
	builds_last_frame = 0
	build_ms_last_frame = 0.0
	var px_per_hex := world.get_hex_width() * zoom
	var use_chunks := px_per_hex >= CHUNK_MODE_PX
	chunk_root.visible = use_chunks
	grid_root.visible = use_chunks and show_grid and px_per_hex >= GRID_MODE_PX
	# Patterns fade in over a short zoom range instead of popping.
	var want_topo := use_chunks and show_topo and world.has_contours()
	var want_detail := use_chunks and show_detail and px_per_hex >= DETAIL_MODE_PX
	contour_root.visible = want_topo
	slope_root.visible = want_topo
	topo_lod = 0 if px_per_hex >= TOPO_CLOSE_PX else 1
	detail_root.visible = want_detail
	if want_detail:
		detail_root.modulate.a = (clampf((px_per_hex - DETAIL_MODE_PX) / (DETAIL_FULL_PX - DETAIL_MODE_PX), 0.0, 1.0) * 0.75 + 0.25) \
			* (TOPO_DETAIL_ALPHA if want_topo else 1.0)
	if not use_chunks:
		return

	var half := viewport_size * 0.5 / zoom
	if want_topo:
		# Index contours are drawn twice, most of a pixel apart, so they read heavier.
		var nudge := Vector2(0.7, 0.7) / zoom
		for entry in contours.values():
			if entry["twin"] != null:
				(entry["twin"] as MeshInstance2D).position = (entry["nodes"][0] as MeshInstance2D).position + nudge
	var c0 := Vector2i(floori((center.x - half.x) / chunk_size_m.x) - 1, floori((center.y - half.y) / chunk_size_m.y) - 1)
	var c1 := Vector2i(floori((center.x + half.x) / chunk_size_m.x) + 1, floori((center.y + half.y) / chunk_size_m.y) + 1)
	c0 = c0.clamp(Vector2i.ZERO, chunk_count - Vector2i.ONE)
	c1 = c1.clamp(Vector2i.ZERO, chunk_count - Vector2i.ONE)

	var want_grid := grid_root.visible
	var needed: Array[Vector2i] = []
	for cy in range(c0.y, c1.y + 1):
		for cx in range(c0.x, c1.x + 1):
			var key := Vector2i(cx, cy)
			if not chunks.has(key) or (want_grid and not grids.has(key)) or (want_detail and not details.has(key)) \
					or (want_topo and (not slopes.has(key) or not contours.has(key) or int(contours[key]["lod"]) != topo_lod)):
				needed.append(key)

	if not needed.is_empty():
		var mid := Vector2(center.x / chunk_size_m.x, center.y / chunk_size_m.y)
		needed.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
			return (Vector2(a) - mid).length_squared() < (Vector2(b) - mid).length_squared())
		var t0 := Time.get_ticks_usec()
		for key in needed:
			if not chunks.has(key):
				_build_chunk(key)
			if want_grid and not grids.has(key):
				_build_grid(key)
			if want_detail and not details.has(key):
				_build_detail(key)
			if want_topo and not slopes.has(key):
				_build_slope(key)
			if want_topo and (not contours.has(key) or int(contours[key]["lod"]) != topo_lod):
				_build_contours(key, topo_lod)
			builds_last_frame += 1
			build_ms_last_frame = (Time.get_ticks_usec() - t0) / 1000.0
			if build_ms_last_frame > BUILD_BUDGET_MS:
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


func _build_detail(key: Vector2i) -> void:
	var mi := MeshInstance2D.new()
	mi.mesh = world.build_chunk_detail_mesh(key)
	mi.position = world.get_chunk_origin(key)
	detail_root.add_child(mi)
	details[key] = mi


func _build_slope(key: Vector2i) -> void:
	var mi := MeshInstance2D.new()
	mi.mesh = world.build_chunk_slope_mesh(key)
	mi.position = world.get_chunk_origin(key)
	slope_root.add_child(mi)
	slopes[key] = mi


func _build_contours(key: Vector2i, lod: int) -> void:
	_free_contours(key)
	var step := 20 if lod == 0 else 100
	var index_step := 100 if lod == 0 else 500
	var origin: Vector2 = world.get_chunk_origin(key)
	var nodes: Array = []
	var twin: MeshInstance2D = null
	for part in [1, 2]:
		var mesh: ArrayMesh = world.build_chunk_contour_mesh(key, step, index_step, part)
		if mesh.get_surface_count() == 0:
			continue
		var mi := MeshInstance2D.new()
		mi.mesh = mesh
		mi.position = origin
		contour_root.add_child(mi)
		nodes.append(mi)
		if part == 2:
			twin = MeshInstance2D.new()
			twin.mesh = mesh
			twin.position = origin
			contour_root.add_child(twin)
	contours[key] = {"lod": lod, "nodes": nodes, "twin": twin,
		"labels": world.get_chunk_contour_labels(key, index_step), "spots": world.get_chunk_spot_heights(key)}


func _free_contours(key: Vector2i) -> void:
	if not contours.has(key):
		return
	var entry: Dictionary = contours[key]
	for mi in entry["nodes"]:
		(mi as Node).queue_free()
	if entry["twin"] != null:
		(entry["twin"] as Node).queue_free()
	contours.erase(key)


func _build_grid(key: Vector2i) -> void:
	var mi := MeshInstance2D.new()
	mi.mesh = world.build_chunk_grid_mesh(key)
	mi.position = world.get_chunk_origin(key)
	grid_root.add_child(mi)
	grids[key] = mi


func _unload_outside(lo: Vector2i, hi: Vector2i) -> void:
	for dict in [chunks, grids, details, slopes]:
		for key in dict.keys():
			var k: Vector2i = key
			if k.x < lo.x or k.y < lo.y or k.x > hi.x or k.y > hi.y:
				(dict[k] as Node).queue_free()
				dict.erase(k)
	for key in contours.keys():
		var k: Vector2i = key
		if k.x < lo.x or k.y < lo.y or k.x > hi.x or k.y > hi.y:
			_free_contours(k)
