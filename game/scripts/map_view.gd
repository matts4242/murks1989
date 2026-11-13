class_name MapView
extends Node2D
## Draws the one map. Far out it shows a one-pixel-per-hex overview texture;
## closer in it streams 64 x 64-hex chunk meshes built by the C++ core,
## creating chunks that come into view and freeing ones far away. Closer still a
## detail layer adds a pattern inside every hex (tree crowns, building blocks,
## furrows, marsh marks), rail sleepers and bridges. The topographic mode keeps
## all of that and makes the ground itself step at every height band (a shader over
## the baked height raster): the border between two bands is the contour.

const CHUNK_HEXES := 64
const BUILD_BUDGET_MS := 6.0     # per frame
const MAX_LOADED := 260          # chunk meshes kept before unloading far ones
const CHUNK_MODE_PX := 7.0       # hex size on screen at which chunks replace the overview
const GRID_MODE_PX := 22.0       # hex size on screen at which hex outlines appear
const DETAIL_MODE_PX := 16.0     # hex size on screen at which terrain patterns appear
const DETAIL_FULL_PX := 26.0     # ... and at which they reach full strength
const TOPO_CLOSE_PX := 20.0      # hex size on screen from which the height bands are 20 m
const TOPO_FAR_PX := 3.0         # ... and below which they are 500 m (100 m in between)
const RELIEF_SHADER := preload("res://shaders/relief.gdshader")

var world: MurksWorld
var overview: Sprite2D
var chunk_root: Node2D
var detail_root: Node2D
var grid_root: Node2D
var chunks := {}                 # Vector2i -> MeshInstance2D
var details := {}                # Vector2i -> MeshInstance2D
var grids := {}                  # Vector2i -> MeshInstance2D
var relief: Sprite2D             # height bands in topographic mode: a shader over the height raster
var relief_labels := {}          # Vector2i -> {"lod", "labels", "spots"}: height figures per chunk
var show_topo := false
var topo_lod := 0                # 0: 20 m bands, every 100 m heavier; 1: 100 m / 500 m; 2: 500 m / 2,500 m
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
	relief = Sprite2D.new()   # over the patterns too, so woods and towns step with the ground
	relief.centered = false
	relief.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
	relief.visible = false
	add_child(relief)
	grid_root = Node2D.new()
	grid_root.z_index = 1
	grid_root.modulate = Color(0.05, 0.05, 0.05, 0.22)
	add_child(grid_root)
	setup_relief()


func update_view(center: Vector2, zoom: float, viewport_size: Vector2) -> void:
	builds_last_frame = 0
	build_ms_last_frame = 0.0
	var px_per_hex := world.get_hex_width() * zoom
	var use_chunks := px_per_hex >= CHUNK_MODE_PX
	chunk_root.visible = use_chunks
	grid_root.visible = use_chunks and show_grid and px_per_hex >= GRID_MODE_PX
	# Patterns fade in over a short zoom range instead of popping.
	var want_topo := show_topo and relief.texture != null
	var want_detail := use_chunks and show_detail and px_per_hex >= DETAIL_MODE_PX
	relief.visible = want_topo
	if want_topo:
		# Band height by zoom; the shader needs the ground distance per pixel for the grade.
		topo_lod = 0 if px_per_hex >= TOPO_CLOSE_PX else (1 if px_per_hex >= TOPO_FAR_PX else 2)
		var mat: ShaderMaterial = relief.material
		mat.set_shader_parameter("interval", [20.0, 100.0, 500.0][topo_lod])
		mat.set_shader_parameter("metres_per_pixel", 1.0 / zoom)
	detail_root.visible = want_detail
	if want_detail:
		detail_root.modulate.a = clampf((px_per_hex - DETAIL_MODE_PX) / (DETAIL_FULL_PX - DETAIL_MODE_PX), 0.0, 1.0) * 0.75 + 0.25
	if not use_chunks:
		relief_labels.clear()
		return

	var half := viewport_size * 0.5 / zoom
	var c0 := Vector2i(floori((center.x - half.x) / chunk_size_m.x) - 1, floori((center.y - half.y) / chunk_size_m.y) - 1)
	var c1 := Vector2i(floori((center.x + half.x) / chunk_size_m.x) + 1, floori((center.y + half.y) / chunk_size_m.y) + 1)
	c0 = c0.clamp(Vector2i.ZERO, chunk_count - Vector2i.ONE)
	c1 = c1.clamp(Vector2i.ZERO, chunk_count - Vector2i.ONE)

	var want_grid := grid_root.visible
	var needed: Array[Vector2i] = []
	for cy in range(c0.y, c1.y + 1):
		for cx in range(c0.x, c1.x + 1):
			var key := Vector2i(cx, cy)
			if not chunks.has(key) or (want_grid and not grids.has(key)) or (want_detail and not details.has(key)):
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
			if not chunks.has(key):
				_build_chunk(key)
			if want_grid and not grids.has(key):
				_build_grid(key)
			if want_detail and not details.has(key):
				_build_detail(key)
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


## Loads the height raster of the map's relief file into the band shader. Without a
## relief file the topographic mode is simply not available.
func setup_relief() -> void:
	relief.texture = null
	relief.material = null
	relief_labels.clear()
	if not world.has_contours():
		return
	var img: Image = world.get_height_image()
	if img == null or img.is_empty():
		return
	relief.texture = ImageTexture.create_from_image(img)
	var px := world.get_height_pixel_size()
	relief.scale = Vector2(px, px)   # texel (i, j) covers the square whose centre is ((j + 0.5) px, (i + 0.5) px)
	var mat := ShaderMaterial.new()
	mat.shader = RELIEF_SHADER
	relief.material = mat


func _build_grid(key: Vector2i) -> void:
	var mi := MeshInstance2D.new()
	mi.mesh = world.build_chunk_grid_mesh(key)
	mi.position = world.get_chunk_origin(key)
	grid_root.add_child(mi)
	grids[key] = mi


func _unload_outside(lo: Vector2i, hi: Vector2i) -> void:
	for dict in [chunks, grids, details]:
		for key in dict.keys():
			var k: Vector2i = key
			if k.x < lo.x or k.y < lo.y or k.x > hi.x or k.y > hi.y:
				(dict[k] as Node).queue_free()
				dict.erase(k)
