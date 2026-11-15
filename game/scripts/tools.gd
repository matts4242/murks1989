class_name MapTools
extends Control
## Planning tools (work board MRK-26), drawn in screen space under the counters:
##   - the 1 km map grid with grid references (Shift+G);
##   - the ruler (Ctrl+drag, Shift+R, or right-click > Measure from here): distance, grid
##     bearing, travel time by the rules' own route search, and the elevation profile;
##   - the elevation profile of a unit's route;
##   - map markings: objectives, phase lines, target reference points and notes. They live in
##     the core world, so the quick save keeps them; the simulation never reads them.

const MK_OBJECTIVE := 0
const MK_PHASE := 1
const MK_TRP := 2
const MK_NOTE := 3
const MARK_NAMES := ["Objective", "Phase line", "Target reference point", "Note"]
const MOBILITY := [["Tracked", 0], ["Wheeled", 1], ["On foot", 2]]   # murks::Mobility
const MOVE_MODES := [["Move", 0], ["Road march", 1], ["Deliberate move", 2]]
const INK := Color(0.04, 0.05, 0.07)
const HALO := Color(1, 1, 1, 0.92)
const RULER := Color(1.0, 0.82, 0.3)
const GRID := Color(0.08, 0.16, 0.42, 0.45)

var main: Node
var font: Font
var show_km_grid := false

# Tool armed: "" none, "ruler", "drag" (Ctrl+drag in progress), or "mark_<kind>".
var tool := ""
var ruler_on := false               # a ruler is on the map
var ruler_live := false             # its end follows the cursor
var ruler_a := Vector2.ZERO         # world metres
var ruler_b := Vector2.ZERO
var est: Array = []                 # one get_route_estimate per MOBILITY row (for the shown ruler)
var est_mode := 0                   # index into MOVE_MODES
var shown_mob := 0                  # index into MOBILITY: the route drawn and profiled
var profile_src := "line"           # "line", "route" (the ruler's route), "unit" (a unit's route)
var profile_unit := -1
var profile: Dictionary = {}
var phase_pts := PackedVector2Array() # a phase line being drawn (world metres)
var marks: Array = []               # world.get_markings()
var drawn_marks: Array = []         # {i, rect} or {i, seg: PackedVector2Array} in screen px, for right-click


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	font = ThemeDB.fallback_font


func refresh_marks() -> void:
	marks = main.world.get_markings()
	queue_redraw()


func _world(screen_pos: Vector2) -> Vector2:
	return main._screen_to_world(screen_pos)


# --- Input -------------------------------------------------------------------------------

## Map input for the tools; true when it was used here (main then ignores it).
func handle_input(event: InputEvent) -> bool:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_LEFT:
			if mb.pressed and mb.ctrl_pressed and tool == "":
				start_ruler(_world(mb.position))
				tool = "drag"
				return true
			if tool == "drag":
				if not mb.pressed:
					ruler_b = _world(mb.position)
					tool = ""
					finish_ruler()
				return true
			if tool == "ruler" and mb.pressed:
				if not ruler_live:
					start_ruler(_world(mb.position))
					tool = "ruler"
				else:
					ruler_b = _world(mb.position)
					tool = ""
					finish_ruler()
				return true
			if tool.begins_with("mark_") and mb.pressed:
				_place_point(int(tool.substr(5)), _world(mb.position))
				return true
			if tool != "" and not mb.pressed:
				return true
		elif mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed and tool != "":
			if tool == "mark_%d" % MK_PHASE:
				finish_phase_line()
			else:
				cancel()
				main.hud.show_toast("Tool put away.", 1.5)
			return true
	elif event is InputEventMouseMotion:
		if ruler_live:
			ruler_b = _world((event as InputEventMouseMotion).position)
			_update_live_panel()
		if ruler_live or not phase_pts.is_empty():
			queue_redraw()
	elif event is InputEventKey and event.pressed and not event.echo and tool == "mark_%d" % MK_PHASE:
		var k := (event as InputEventKey).keycode
		if k == KEY_ENTER or k == KEY_KP_ENTER:
			finish_phase_line()
			return true
		if k == KEY_BACKSPACE and not phase_pts.is_empty():
			phase_pts.remove_at(phase_pts.size() - 1)
			queue_redraw()
			return true
	return false


## Esc: put the armed tool away, else take the ruler off the map. True if it did something.
func cancel() -> bool:
	if tool != "":
		tool = ""
		phase_pts.clear()
		if ruler_live:
			ruler_live = false
			if not est.is_empty() or profile_src == "unit":
				_show_panel()
			else:
				clear_ruler()
		queue_redraw()
		return true
	if ruler_on or profile_src == "unit":
		clear_ruler()
		return true
	return false


## Shift+key shortcuts. True if the key was one of ours.
func shift_key(key: Key) -> bool:
	match key:
		KEY_R: arm("ruler")
		KEY_G: main._on_toggle("KmGrid")
		KEY_O: arm("mark_%d" % MK_OBJECTIVE)
		KEY_P: arm("mark_%d" % MK_PHASE)
		KEY_T: arm("mark_%d" % MK_TRP)
		KEY_N: arm("mark_%d" % MK_NOTE)
		_: return false
	return true


func arm(name: String) -> void:
	cancel()
	tool = name
	main.move_mode = -1
	match name:
		"ruler": main.hud.show_toast("Ruler: click the start, then the end (or Ctrl+drag). Esc puts it away.", 4.0)
		"mark_0": main.hud.show_toast("Objective: click where it is. Esc cancels.", 3.0)
		"mark_1": main.hud.show_toast("Phase line: click its points; right-click or Enter to finish, Backspace removes the last point, Esc cancels.", 6.0)
		"mark_2": main.hud.show_toast("Target reference point: click where it is. Esc cancels.", 3.0)
		"mark_3": main.hud.show_toast("Note: click where it goes. Esc cancels.", 3.0)


## Menu actions of the tools (Tools menu and the right-click map menu). True if handled.
func on_action(name: String) -> bool:
	if name.begins_with("mark_here_"):
		var kind := name.substr(10).to_int()
		if kind == MK_PHASE:
			arm("mark_%d" % MK_PHASE)
			phase_pts.append(main.menu_world_pos)
		else:
			_place_point(kind, main.menu_world_pos)
		return true
	if name.begins_with("mark_rename_"):
		var i := name.substr(12).to_int()
		if i >= 0 and i < marks.size():
			var mk: Dictionary = marks[i]
			main.hud.ask_text("Rename " + String(MARK_NAMES[int(mk["kind"])]).to_lower(),
				"Text of the note" if int(mk["kind"]) == MK_NOTE else "Name on the map",
				String(mk["name"]), _renamed.bind(i))
		return true
	if name.begins_with("mark_delete_"):
		var i := name.substr(12).to_int()
		if i >= 0 and i < marks.size():
			var nm := String((marks[i] as Dictionary)["name"])
			main.world.remove_marking(i)
			refresh_marks()
			main.hud.show_toast("Deleted %s." % nm, 2.5)
		return true
	if name.begins_with("mark_") and name.substr(5).is_valid_int():
		arm(name)
		return true
	if name.begins_with("ruler_mob_"):
		shown_mob = clampi(name.substr(10).to_int(), 0, MOBILITY.size() - 1)
		if profile_src == "route":
			_make_profile()
		_show_panel()
		queue_redraw()
		return true
	if name.begins_with("ruler_mode_"):
		est_mode = clampi(name.substr(11).to_int(), 0, MOVE_MODES.size() - 1)
		_estimate()
		if profile_src == "route":
			_make_profile()
		_show_panel()
		queue_redraw()
		return true
	if name.begins_with("profile_"):
		var src := name.substr(8)
		if src in ["line", "route"] and ruler_on:
			profile_src = src
			_make_profile()
			_show_panel()
			queue_redraw()
		return true
	match name:
		"tool_ruler": arm("ruler")
		"measure_here":
			arm("ruler")
			start_ruler(main.menu_world_pos)
			main.hud.show_toast("Measuring from here: click the end. Esc puts the ruler away.", 3.0)
		"tool_clear_ruler": clear_ruler()
		"tool_route_profile": profile_unit_route()
		"marks_clear":
			if marks.is_empty():
				return true
			main.hud.ask_confirm("Clear all markings", "Delete all %d markings from the map?" % marks.size(), _clear_all)
		"sight_eye_changed":
			if not profile.is_empty() and profile_src == "line":
				_make_profile()
				_show_panel()
		_: return false
	return true


# --- Ruler ---------------------------------------------------------------------------------

func start_ruler(at: Vector2) -> void:
	ruler_on = true
	ruler_live = true
	ruler_a = at
	ruler_b = at
	est.clear()
	profile = {}
	profile_src = "line"
	profile_unit = -1
	_update_live_panel()
	queue_redraw()


## The end is set: route estimates for each mobility, then the profile.
func finish_ruler() -> void:
	ruler_live = false
	if ruler_a.distance_to(ruler_b) < 50.0:
		clear_ruler()
		return
	var own: Array[int] = main._own_selected()
	if not own.is_empty():
		var mob := int(main.world.get_unit_info(own[0]).get("mobility", -1))
		if mob >= 0 and mob < MOBILITY.size():
			shown_mob = mob
	_estimate()
	_make_profile()
	_show_panel()
	queue_redraw()


func _estimate() -> void:
	est.clear()
	var a: Vector2i = main.world.world_to_hex(ruler_a)
	var b: Vector2i = main.world.world_to_hex(ruler_b)
	for m in MOBILITY:
		est.append(main.world.get_route_estimate(a, b, int(m[1]), int(MOVE_MODES[est_mode][1])))


func clear_ruler() -> void:
	ruler_on = false
	ruler_live = false
	est.clear()
	profile = {}
	profile_src = "line"
	profile_unit = -1
	if tool == "ruler" or tool == "drag":
		tool = ""
	main.hud.show_ruler({})
	queue_redraw()


## The profile of the selected unit's current route (until waypoint routes, MRK-20).
func profile_unit_route() -> void:
	var own: Array[int] = main._own_selected()
	if own.is_empty():
		main.hud.show_toast("Select one of your units that is on the move.", 3.0)
		return
	var info: Dictionary = main.world.get_unit_info(own[0])
	var path: PackedVector2Array = info.get("path", PackedVector2Array())
	if path.size() < 2:
		main.hud.show_toast("%s is not on the move: no route to profile." % String(info.get("name", "The unit")), 3.0)
		return
	ruler_on = false
	ruler_live = false
	est.clear()
	profile_src = "unit"
	profile_unit = own[0]
	_make_profile()
	_show_panel()
	queue_redraw()


func eye_m() -> float:
	return float(main.hud.PLAN_EYES[main.sight_eye_index][0])


func _make_profile() -> void:
	var pts := PackedVector2Array()
	match profile_src:
		"line":
			pts = PackedVector2Array([ruler_a, ruler_b])
		"route":
			if shown_mob < est.size():
				pts = (est[shown_mob] as Dictionary).get("path", PackedVector2Array())
		"unit":
			var info: Dictionary = main.world.get_unit_info(profile_unit)
			pts = info.get("path", PackedVector2Array())
	profile = main.world.get_profile(pts, eye_m()) if pts.size() >= 2 else {}
	if profile_src != "line" and pts.size() == 2:
		# A two-hex route: still a route (no sight line).
		profile["block"] = -3


static func duration(minutes: float) -> String:
	var m := roundi(minutes)
	if m < 60:
		return "%d min" % m
	return "%d h %02d" % [m / 60, m % 60]


func _bearing_text() -> String:
	var deg: float = main.world.get_grid_bearing(ruler_a, ruler_b)
	return "%03d° grid (%04d mils)" % [roundi(deg) % 360, roundi(deg / 360.0 * 6400.0) % 6400]


func _update_live_panel() -> void:
	var km := ruler_a.distance_to(ruler_b) / 1000.0
	main.hud.show_ruler({"title": "Ruler", "line": "%.2f km · bearing %s" % [km, _bearing_text()],
		"refs": "From GR %s" % main.world.get_grid_ref(ruler_a), "live": true})


func _show_panel() -> void:
	var d := {}
	if profile_src == "unit":
		var info: Dictionary = main.world.get_unit_info(profile_unit)
		d["title"] = "Route of " + String(info.get("name", "unit"))
		var bits: Array[String] = []
		if info.has("route_km"):
			bits.append("%.1f km to go" % float(info["route_km"]))
		if info.has("eta_minutes"):
			bits.append("about %s" % duration(float(info["eta_minutes"])))
		d["line"] = ", ".join(bits) if not bits.is_empty() else "Arrived."
	else:
		var km := ruler_a.distance_to(ruler_b) / 1000.0
		d["title"] = "Ruler"
		d["line"] = "%.2f km straight · bearing %s" % [km, _bearing_text()]
		d["refs"] = "GR %s  →  GR %s" % [main.world.get_grid_ref(ruler_a), main.world.get_grid_ref(ruler_b)]
		var rows: Array = []
		for i in MOBILITY.size():
			var e: Dictionary = est[i] if i < est.size() else {}
			var text := "%s: no route (water in the way)" % MOBILITY[i][0]
			if e.get("found", false):
				text = "%s  %s  ·  %.1f km, %.1f on roads" % [MOBILITY[i][0], duration(float(e["minutes"])),
					float(e["km"]), float(e["road_km"])]
			rows.append(text)
		d["rows"] = rows
		d["shown"] = shown_mob
		d["mode"] = est_mode
		d["src"] = profile_src
	d["profile"] = profile
	d["eye_index"] = main.sight_eye_index
	d["summary"] = _profile_summary()
	main.hud.show_ruler(d)


func _profile_summary() -> String:
	if profile.is_empty():
		return ""
	var km: PackedFloat32Array = profile["km"]
	var text := "Climb %d m, descent %d m, steepest %d%% between hexes." % [roundi(float(profile["climb"])),
		roundi(float(profile["descent"])), roundi(float(profile["steepest"]))]
	var blk := int(profile.get("block", -3))
	if blk == -1:
		text += "\nSight: the end can be seen from the start (eye %d m, target %.1f m)." % [roundi(float(profile["eye"])), float(profile["target"])]
	elif blk >= 0 and blk < km.size():
		var terr: PackedInt32Array = profile["terrain"]
		var cover: PackedFloat32Array = profile["cover"]
		var what := "the ground"
		if cover[blk] > 0.0:
			what = "%s %d m tall" % [_terrain_word(terr[blk]), roundi(cover[blk])]
		text += "\nSight: blocked %.1f km out by %s (eye %d m, target %.1f m)." % [km[blk], what,
			roundi(float(profile["eye"])), float(profile["target"])]
	elif blk == -2:
		text += "\nSight: the line leaves the map."
	return text


static func _terrain_word(t: int) -> String:
	# murks::Terrain: 0 water, 1 open, 2 farmland, 3 forest, 4 wetland, 5 urban.
	match t:
		3: return "forest"
		4: return "reeds"
		5: return "buildings"
		_: return "trees and hedges"


# --- Markings ------------------------------------------------------------------------------

func _place_point(kind: int, at: Vector2) -> void:
	if kind == MK_PHASE:
		phase_pts.append(at)
		queue_redraw()
		return
	tool = ""
	match kind:
		MK_TRP:
			_add(kind, "TRP %02d" % _next_number(kind), PackedVector2Array([at]))
		MK_OBJECTIVE:
			main.hud.ask_text("Objective", "Name on the map", "OBJ %d" % _next_number(kind),
				_add_named.bind(kind, PackedVector2Array([at])))
		MK_NOTE:
			main.hud.ask_text("Note", "Text of the note", "", _add_named.bind(kind, PackedVector2Array([at])))


func finish_phase_line() -> void:
	if phase_pts.size() < 2:
		main.hud.show_toast("A phase line needs at least two points.", 2.5)
		return
	var pts := phase_pts.duplicate()
	phase_pts.clear()
	tool = ""
	main.hud.ask_text("Phase line", "Name on the map", "PL %d" % _next_number(MK_PHASE), _add_named.bind(MK_PHASE, pts))
	queue_redraw()


## From the name dialog: an empty name adds nothing.
func _add_named(text: String, kind: int, pts: PackedVector2Array) -> void:
	if text.strip_edges() != "":
		_add(kind, text, pts)


func _renamed(text: String, i: int) -> void:
	if text.strip_edges() != "":
		main.world.rename_marking(i, text)
		refresh_marks()


func _clear_all() -> void:
	main.world.clear_markings()
	refresh_marks()
	main.hud.show_toast("All markings cleared.", 2.5)


func _add(kind: int, text: String, pts: PackedVector2Array) -> void:
	var i: int = main.world.add_marking(kind, text, pts)
	if i < 0:
		main.hud.show_toast("Could not add the marking (off the map, or 500 markings already).", 3.0)
	refresh_marks()


## 1 + the highest number among this kind's default names ("OBJ 3" -> 4).
func _next_number(kind: int) -> int:
	var n := 0
	for mk in marks:
		if int(mk["kind"]) != kind:
			continue
		var parts := String(mk["name"]).split(" ")
		if parts.size() == 2 and parts[1].is_valid_int():
			n = maxi(n, parts[1].to_int())
	return n + 1


## The marking under a screen point (the topmost), or -1.
func marking_at(screen_pos: Vector2) -> int:
	for k in range(drawn_marks.size() - 1, -1, -1):
		var dm: Dictionary = drawn_marks[k]
		if dm.has("rect"):
			if (dm["rect"] as Rect2).grow(3.0).has_point(screen_pos):
				return int(dm["i"])
		else:
			var seg: PackedVector2Array = dm["seg"]
			for j in seg.size() - 1:
				if Geometry2D.get_closest_point_to_segment(screen_pos, seg[j], seg[j + 1]).distance_to(screen_pos) <= 7.0:
					return int(dm["i"])
	return -1


# --- Drawing -------------------------------------------------------------------------------

func _draw() -> void:
	if main == null or not main.ready_to_play:
		return
	var xf: Transform2D = get_viewport().get_canvas_transform()
	var zoom: float = main.camera.zoom.x
	if show_km_grid:
		_draw_grid(xf, zoom)
	_draw_marks(xf)
	_draw_ruler(xf, zoom)
	_draw_unit_route(xf)


## Grid lines on the map's projected metres: 1 km squares, or 10 km / 100 km zoomed out.
func _draw_grid(xf: Transform2D, zoom: float) -> void:
	var mi: Dictionary = main.map_info
	var e0 := float(mi.get("grid_e0", 0.0))
	var n0 := float(mi.get("grid_n0", 0.0))
	var step := 1000.0
	while step * zoom < 36.0 and step < 100000.0:
		step *= 10.0
	var inv := xf.affine_inverse()
	var tl: Vector2 = inv * Vector2.ZERO
	var br: Vector2 = inv * size
	var ws: Vector2 = main.world.get_world_size()
	tl = tl.clamp(Vector2.ZERO, ws)
	br = br.clamp(Vector2.ZERO, ws)
	var lab_y := 140.0
	var lab_x := 10.0
	# Eastings: vertical lines.
	var e := ceilf((e0 + tl.x) / step) * step
	while e <= e0 + br.x:
		var x: float = (xf * Vector2(e - e0, 0)).x
		var heavy := _grid_weight(e, step)
		var top: Vector2 = xf * Vector2(e - e0, tl.y)
		var bot: Vector2 = xf * Vector2(e - e0, br.y)
		draw_line(Vector2(x, top.y), Vector2(x, bot.y), GRID if heavy == 0 else GRID.darkened(0.2), 1.0 + heavy * 0.8)
		if x > 34.0:
			_grid_label(Vector2(x + 3, lab_y), _grid_text(e, step), heavy)
		e += step
	# Northings: horizontal lines.
	var n := ceilf((n0 - br.y) / step) * step
	while n <= n0 - tl.y:
		var y: float = (xf * Vector2(0, n0 - n)).y
		var heavy := _grid_weight(n, step)
		var left: Vector2 = xf * Vector2(tl.x, n0 - n)
		var right: Vector2 = xf * Vector2(br.x, n0 - n)
		draw_line(Vector2(left.x, y), Vector2(right.x, y), GRID if heavy == 0 else GRID.darkened(0.2), 1.0 + heavy * 0.8)
		if y > lab_y + 16.0:
			_grid_label(Vector2(lab_x, y - 3), _grid_text(n, step), heavy)
		n += step


## 0 a plain line, 1 every 10 km, 2 every 100 km (at the step shown).
static func _grid_weight(v: float, step: float) -> int:
	var km := roundi(v / 1000.0)
	if km % 100 == 0:
		return 2
	if km % 10 == 0 and step < 10000.0:
		return 1
	return 0


## The figures printed on a grid line: kilometres within the 100 km square ("21"), the
## square's own figures on its edge lines ("47 00"), or the square at the widest zoom ("47").
static func _grid_text(v: float, step: float) -> String:
	var km := roundi(v / 1000.0)
	if step >= 100000.0:
		return "%d" % (km / 100)
	if km % 100 == 0:
		return "%d 00" % (km / 100)
	return "%02d" % (km % 100)


func _grid_label(p: Vector2, text: String, heavy: int) -> void:
	var fs := 13 if heavy > 0 else 12
	draw_string_outline(font, p, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, 4, Color(1, 1, 1, 0.85))
	draw_string(font, p, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0.06, 0.12, 0.36))


func _halo_text(p: Vector2, text: String, fs: int, col := INK) -> void:
	draw_string_outline(font, p, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, 5, HALO)
	draw_string(font, p, text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, col)


func _halo_line(pts: PackedVector2Array, w: float, col := INK) -> void:
	if pts.size() < 2:
		return
	draw_polyline(pts, HALO, w + 2.5, true)
	draw_polyline(pts, col, w, true)


func _draw_marks(xf: Transform2D) -> void:
	drawn_marks.clear()
	var view := Rect2(Vector2(-200, -200), size + Vector2(400, 400))
	for i in marks.size():
		var mk: Dictionary = marks[i]
		var kind := int(mk["kind"])
		var nm := String(mk["name"])
		var wp: PackedVector2Array = mk["points"]
		var sp := PackedVector2Array()
		for p in wp:
			sp.append(xf * p)
		if kind == MK_PHASE:
			_halo_line(sp, 3.0)
			var label := nm if nm.begins_with("PL") else "PL " + nm
			# Long on the screen: the name once more half-way along.
			var total := 0.0
			for j in sp.size() - 1:
				total += sp[j].distance_to(sp[j + 1])
			if total > 500.0:
				var half := total * 0.5
				for j in sp.size() - 1:
					var seg := sp[j].distance_to(sp[j + 1])
					if half <= seg:
						var mp := sp[j].lerp(sp[j + 1], half / maxf(seg, 0.001))
						if view.has_point(mp):
							_halo_text(mp + Vector2(8, 5), label, 14)
						break
					half -= seg
			for end in [0, sp.size() - 1]:
				var p: Vector2 = sp[end]
				if not view.has_point(p):
					continue
				var nb: Vector2 = sp[1] if end == 0 else sp[sp.size() - 2]
				var out := (p - nb).normalized()
				var tw := font.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, 14).x
				var at := p + out * 10.0 + Vector2(-tw * 0.5 if absf(out.x) < 0.5 else (0.0 if out.x > 0 else -tw), 5)
				_halo_text(at, label, 14)
			drawn_marks.append({"i": i, "seg": sp})
			continue
		var c: Vector2 = sp[0]
		if not view.has_point(c):
			continue
		match kind:
			MK_OBJECTIVE:
				draw_arc(c, 15.0, 0, TAU, 40, HALO, 6.0, true)
				draw_arc(c, 15.0, 0, TAU, 40, INK, 2.5, true)
				draw_circle(c, 2.5, INK)
				var label := nm if nm.begins_with("OBJ") else "OBJ " + nm
				var tw := font.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, 15).x
				_halo_text(c + Vector2(-tw * 0.5, -22), label, 15)
				drawn_marks.append({"i": i, "rect": Rect2(c - Vector2(maxf(18, tw * 0.5), 38), Vector2(maxf(36, tw), 56))})
			MK_TRP:
				for d in [Vector2(10, 0), Vector2(0, 10)]:
					draw_line(c - d, c + d, HALO, 5.0, true)
				for d in [Vector2(10, 0), Vector2(0, 10)]:
					draw_line(c - d, c + d, INK, 2.0, true)
				draw_arc(c, 5.0, 0, TAU, 20, INK, 1.5, true)
				_halo_text(c + Vector2(9, -8), nm, 13)
				var tw := font.get_string_size(nm, HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
				drawn_marks.append({"i": i, "rect": Rect2(c - Vector2(12, 22), Vector2(tw + 24, 34))})
			MK_NOTE:
				var text := nm if nm.length() <= 48 else nm.substr(0, 47) + "…"
				var tw := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
				var r := Rect2(c + Vector2(8, -10), Vector2(tw + 10, 19))
				draw_line(c, c + Vector2(8, -2), INK, 1.5, true)
				draw_circle(c, 3.0, INK)
				draw_rect(r, Color(1.0, 0.95, 0.62, 0.95))
				draw_rect(r, Color(0.3, 0.26, 0.1, 0.9), false, 1.0)
				draw_string(font, r.position + Vector2(5, 14), text, HORIZONTAL_ALIGNMENT_LEFT, -1, 13, Color(0.12, 0.1, 0.02))
				drawn_marks.append({"i": i, "rect": r.merge(Rect2(c - Vector2(5, 5), Vector2(10, 10)))})
	# A phase line being drawn: its points, and the next leg to the cursor.
	if not phase_pts.is_empty():
		var sp := PackedVector2Array()
		for p in phase_pts:
			sp.append(xf * p)
		sp.append(get_local_mouse_position())
		for j in sp.size() - 1:
			draw_dashed_line(sp[j], sp[j + 1], INK, 2.5, 9.0, true)
		for j in sp.size() - 1:
			draw_circle(sp[j], 4.0, HALO)
			draw_circle(sp[j], 2.5, INK)


func _draw_ruler(xf: Transform2D, zoom: float) -> void:
	if not ruler_on:
		return
	# The route shown (the selected unit's mobility, or tracked), dotted.
	if not ruler_live and shown_mob < est.size():
		var path: PackedVector2Array = (est[shown_mob] as Dictionary).get("path", PackedVector2Array())
		if path.size() >= 2:
			var step := maxi(1, path.size() / 400)
			var pts := PackedVector2Array()
			for i in range(0, path.size(), step):
				pts.append(xf * path[i])
			pts.append(xf * path[path.size() - 1])
			for i in pts.size() - 1:
				draw_dashed_line(pts[i], pts[i + 1], Color(0, 0, 0, 0.7), 4.5, 7.0, true)
				draw_dashed_line(pts[i], pts[i + 1], RULER, 2.5, 7.0, true)
	var a: Vector2 = xf * ruler_a
	var b: Vector2 = xf * ruler_b
	draw_line(a, b, Color(0, 0, 0, 0.7), 5.0, true)
	draw_line(a, b, RULER, 2.5, true)
	# Ticks every kilometre (every 10 km when they would crowd), heavier every fifth.
	var len_m := ruler_a.distance_to(ruler_b)
	if len_m > 1.0:
		var tick := 1000.0 if 1000.0 * zoom >= 7.0 else 10000.0
		var dir := (b - a).normalized()
		var nrm := Vector2(-dir.y, dir.x)
		var k := 1
		while k * tick < len_m and k < 400:
			var p := a.lerp(b, k * tick / len_m)
			var h := 7.0 if k % 5 == 0 else 4.0
			draw_line(p - nrm * h, p + nrm * h, Color(0, 0, 0, 0.8), 3.0, true)
			draw_line(p - nrm * h, p + nrm * h, RULER, 1.5, true)
			k += 1
	draw_circle(a, 5.0, Color(0, 0, 0, 0.75))
	draw_circle(a, 3.5, RULER)
	draw_circle(b, 5.0, Color(0, 0, 0, 0.75))
	draw_circle(b, 3.5, RULER)
	# The figures just beyond the end, along the line, kept on the screen.
	var t := "%.2f km  %s" % [len_m / 1000.0, _bearing_text().split(" ")[0]]
	var tw := font.get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, 14).x
	var out := (b - a).normalized() if len_m > 1.0 else Vector2.RIGHT
	var at := b + out * 16.0 + Vector2(-tw * 0.5 + out.x * tw * 0.5, 5.0 + out.y * 8.0)
	at.x = clampf(at.x, 6.0, size.x - tw - 6.0)
	at.y = clampf(at.y, 60.0, size.y - 8.0)
	draw_string_outline(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, 5, Color(0, 0, 0, 0.85))
	draw_string(font, at, t, HORIZONTAL_ALIGNMENT_LEFT, -1, 14, RULER)
	# Where the sight line is blocked, on the straight-line profile.
	var blk := int(profile.get("block", -3)) if profile_src == "line" else -3
	if blk >= 0:
		var centres: PackedVector2Array = profile["centres"]
		if blk < centres.size():
			var p: Vector2 = xf * centres[blk]
			draw_line(p + Vector2(-6, -6), p + Vector2(6, 6), Color(0.9, 0.15, 0.1), 3.0, true)
			draw_line(p + Vector2(-6, 6), p + Vector2(6, -6), Color(0.9, 0.15, 0.1), 3.0, true)


func _draw_unit_route(xf: Transform2D) -> void:
	if profile_src != "unit" or profile.is_empty():
		return
	var centres: PackedVector2Array = profile["centres"]
	if centres.size() < 2:
		return
	var a: Vector2 = xf * centres[0]
	var b: Vector2 = xf * centres[centres.size() - 1]
	for p in [a, b]:
		draw_circle(p, 5.0, Color(0, 0, 0, 0.75))
		draw_circle(p, 3.5, RULER)
