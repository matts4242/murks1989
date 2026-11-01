class_name Overlay
extends Control
## Screen-space layer over the map: town names, unit counters, routes,
## pending orders, line of sight, fidelity zones, fire effects and event pings.
## Drawing in screen space keeps text readable at every zoom level; counters
## scale with the hex once a hex is larger than the minimum counter.

const GROUP_PX := 14.0              # theater map: hexes smaller than this on screen -> group companies
const GROUP_RADIUS := 44.0          # screen px: members of one company this close merge into one counter
const HEX_R := 288.675              # hex corner radius in metres (500 m flat to flat)
const INK := Color(0.06, 0.06, 0.08)
const ZONE_COLORS := [Color(0, 0, 0, 0), Color(1.0, 0.75, 0.2), Color(1.0, 0.25, 0.2)]

var main: Node
var font: Font
var town_names := PackedStringArray()
var town_pos := PackedVector2Array()
var town_pop := PackedInt32Array()
var draw_ms := 0.0
var groups: Array = []              # last drawn groups: {pos, ids, side} (used for click-selection)
var drawn: Array = []               # last drawn single counters: {rect, id, side}, bottom to top (click-selection)


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	font = ThemeDB.fallback_font


func set_towns(list: Array) -> void:
	for t in list:
		town_names.append(t["name"])
		town_pos.append(t["pos"])
		town_pop.append(t["population"])


func _draw() -> void:
	if main == null or not main.ready_to_play:
		return
	var t0 := Time.get_ticks_usec()
	var xf: Transform2D = get_viewport().get_canvas_transform()
	var zoom: float = main.camera.zoom.x
	var view := Rect2(Vector2(-60, -60), size + Vector2(120, 120))
	if main.show_zones:
		_draw_zones(xf, zoom)
	_draw_towns(xf, zoom, view)
	if main.show_los:
		_draw_los(xf, zoom)
	_draw_wrecks(xf, view)
	_draw_command_links(xf)
	_draw_orders(xf)
	_draw_units(xf, view)
	_draw_fx(xf)
	_draw_pings(xf)
	_draw_hover(xf)
	draw_ms = (Time.get_ticks_usec() - t0) / 1000.0


# --- Towns -------------------------------------------------------------------------

func _draw_towns(xf: Transform2D, zoom: float, view: Rect2) -> void:
	var px := 500.0 * zoom
	var min_pop := 0
	if px < 2.0: min_pop = 250000
	elif px < 4.0: min_pop = 60000
	elif px < 8.0: min_pop = 20000
	elif px < 16.0: min_pop = 6000
	elif px < 30.0: min_pop = 2500
	for i in town_pos.size():
		var pop := town_pop[i]
		if pop < min_pop:
			continue
		var s: Vector2 = xf * town_pos[i]
		if not view.has_point(s):
			continue
		var fs := 11
		if pop >= 500000: fs = 18
		elif pop >= 100000: fs = 15
		elif pop >= 20000: fs = 13
		var dot_r := 2.5 if pop < 20000 else 3.5
		if px < 8.0:
			draw_circle(s, dot_r + 1.0, Color(0, 0, 0, 0.7))
			draw_circle(s, dot_r, Color(1, 1, 1, 0.9))
		var p := s + Vector2(6, -6)
		draw_string_outline(font, p, town_names[i], HORIZONTAL_ALIGNMENT_LEFT, -1, fs, 4, Color(1, 1, 0.94, 0.85))
		draw_string(font, p, town_names[i], HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color(0.12, 0.10, 0.08))


# --- Units -------------------------------------------------------------------------
# Counter style from the Forward Defense engine: a square counter in the side's colour
# with a white NATO symbol box, echelon marks above it, strength bottom-left and a short
# name bottom-right, an order badge top-left, a morale marker top-right (corner flag when
# shaken, hatching when broken or falling back), an under-fire bar along the top, and a
# signal-yellow selection ring. Last-seen markers are faded with a dashed outline and "?".

const SIDE_COL := [Color("#2c5ea8"), Color("#b0342b")]
const BOX_PAPER := Color("#f4f2ea")
const SYMBOL_INK := Color("#141712")
const SIGNAL := Color("#d39a17")
const MIN_COUNTER := 32.0
const THEATER_COUNTER := 28.0
const COMPANY_COUNTER := 36.0
const ECHELONS := ["coy", "plt", "plt", "plt", "sec", "btty"] # by unit type
const TEAM_COLORS := [Color("#f2c14e"), Color("#39c0b0"), Color("#c77dff"), Color("#8bd450"),
	Color("#ff8c42"), Color("#5ab0ff"), Color("#ff6f91"), Color("#d9d9d9")]


func grouping_active() -> bool:
	return main.group_companies and 500.0 * main.camera.zoom.x < GROUP_PX


func hex_px() -> float:
	return HEX_R * main.camera.zoom.x


## Counter side length: the counter fills its hex (1.28 x the hex corner radius) once
## the hex is big enough, and never gets smaller than MIN_COUNTER.
func counter_size() -> float:
	return clampf(hex_px() * 1.28, MIN_COUNTER, 112.0)


## Everything the counter needs from one snapshot row.
func _row_item(o: int, i: int) -> Dictionary:
	var snap: PackedFloat32Array = main.snapshot
	var id := int(snap[o])
	var side := int(snap[o + 1])
	var flags := int(snap[o + 7])
	var item := {"id": id, "side": side, "type": int(snap[o + 2]), "flags": flags, "ghost": (flags & 2) != 0,
		"strength": snap[o + 5], "zone": int(snap[o + 6]), "morale": snap[o + 8], "order_in": snap[o + 9],
		"roe": int(snap[o + 12]), "badge": "", "label": "", "fire": 0.0, "sel": main.selected.has(id),
		"echelon": ECHELONS[clampi(int(snap[o + 2]), 0, 5)], "row": i,
		"engage": snap[o + 17], "elem": int(snap[o + 18]), "elem_max": int(snap[o + 19]),
		"team": int(snap[o + 20]), "role": int(snap[o + 21]), "tcol": int(snap[o + 22]), "standoff": int(snap[o + 24])}
	if side == 0:
		match int(snap[o + 13]):
			1: item["badge"] = ["M", "R", "D", "A"][clampi(int(snap[o + 11]), 0, 3)]
			2: item["badge"] = "F"
			3: item["badge"] = "G"
			4: item["badge"] = "S"
		item["label"] = main.short_name(id)
		var age: float = main.world.get_time_minutes() - snap[o + 16]
		if age < 15.0:
			item["fire"] = 1.0 - age / 15.0
	if (flags & 64) != 0:
		item["fire"] = 1.0
	return item


func _draw_units(xf: Transform2D, view: Rect2) -> void:
	groups = []
	drawn = []
	if grouping_active():
		_draw_grouped(xf, view)
		return
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	var n := snap.size() / stride
	var s := counter_size()
	var by_hex := hex_px() * 1.7 >= s * 0.75 # hexes big enough to hold their own stack
	var stacks := {}
	for i in n:
		var o := i * stride
		var sp: Vector2 = xf * main.disp[i]
		if not view.grow(s).has_point(sp):
			continue
		var key: Vector2i
		if by_hex:
			key = Vector2i(roundi(snap[o + 3] / 50.0), roundi(snap[o + 4] / 50.0)) # the hex the unit is in
		else:
			key = Vector2i(floori(sp.x / (s * 0.6)), floori(sp.y / (s * 0.6)))
		var item := _row_item(o, i)
		item["pos"] = sp
		if not stacks.has(key):
			stacks[key] = []
		stacks[key].append(item)
	# Stacks are painted top to bottom of the screen; inside a stack, last-seen markers go
	# underneath, then own units, then spotted hostiles, and the selected counter on top.
	var order: Array = stacks.values()
	order.sort_custom(func(a: Array, b: Array) -> bool: return (a[0]["pos"] as Vector2).y < (b[0]["pos"] as Vector2).y)
	for st in order:
		st.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return _layer(a) < _layer(b))
		var m: int = st.size()
		for k in m:
			var it: Dictionary = st[k]
			var off := (k - (m - 1) * 0.5) * s * 0.16
			var c: Vector2 = (it["pos"] as Vector2) + Vector2(off, -off)
			_draw_counter(c, s, it)
			drawn.append({"rect": Rect2(c - Vector2(s, s) * 0.5, Vector2(s, s)), "id": it["id"], "side": it["side"]})


func _layer(it: Dictionary) -> int:
	return (0 if it["ghost"] else (1 if it["side"] == 0 else 2)) + (10 if it["sel"] else 0)


func _rrect(c: Vector2, s: float, rad: float) -> PackedVector2Array:
	var h := s * 0.5
	var pts := PackedVector2Array()
	var corners := [Vector2(h - rad, -h + rad), Vector2(h - rad, h - rad), Vector2(-h + rad, h - rad), Vector2(-h + rad, -h + rad)]
	var start := [-PI * 0.5, 0.0, PI * 0.5, PI]
	for q in 4:
		for j in 4:
			var a: float = start[q] + PI * 0.5 * j / 3.0
			pts.append(c + (corners[q] as Vector2) + Vector2(cos(a), sin(a)) * rad)
	return pts


func _outline(pts: PackedVector2Array, col: Color, w: float) -> void:
	var closed := pts.duplicate()
	closed.append(pts[0])
	draw_polyline(closed, col, w, true)


func _text_center(p: Vector2, text: String, fs: int, col: Color) -> void:
	var sz := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs)
	draw_string(font, p + Vector2(-sz.x * 0.5, sz.y * 0.32), text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, col)


## One unit counter centred at c with side length s.
func _draw_counter(c: Vector2, s: float, it: Dictionary) -> void:
	var h := s * 0.5
	var rad := maxf(1.5, s * 0.11)
	var ghost: bool = it["ghost"]
	var side: int = it["side"]
	var fill: Color = SIDE_COL[clampi(side, 0, 1)]
	var shape := _rrect(c, s, rad)
	if ghost:
		fill.a = 0.5
	else:
		draw_colored_polygon(_rrect(c + Vector2(s * 0.05, s * 0.07), s, rad), Color(0, 0, 0, 0.32))
	draw_colored_polygon(shape, fill)
	_outline(shape, Color(0, 0, 0, 0.35), 1.0)
	# Symbol box with the NATO symbol, echelon marks above it.
	var box := Rect2(c.x - s * 0.35, c.y - s * 0.24, s * 0.7, s * 0.42)
	var paper := BOX_PAPER
	var ink := SYMBOL_INK
	if ghost:
		paper.a = 0.55
		ink.a = 0.6
	draw_rect(box, paper)
	var lw := maxf(0.8, s * 0.038)
	draw_rect(box, ink, false, lw)
	_draw_symbol(box, int(it["type"]), ink, lw)
	_draw_echelon(String(it["echelon"]), Vector2(c.x, c.y - s * 0.35), s, Color(1, 1, 1, 0.6 if ghost else 1.0))
	if ghost:
		_text_center(Vector2(c.x, c.y + h - s * 0.17), "?", int(s * 0.28), Color.WHITE)
		var corners := [c + Vector2(-h, -h), c + Vector2(h, -h), c + Vector2(h, h), c + Vector2(-h, h)]
		for q in 4:
			draw_dashed_line(corners[q], corners[(q + 1) % 4], Color.WHITE, maxf(1.0, s * 0.05), maxf(2.0, s * 0.1))
	else:
		# Combat effectiveness is binary per element: each vehicle, gun or squad is in the fight
		# or knocked out. Bottom-left: how many are in the fight (white when all are, yellow
		# from half, red below half). Under the counter: one pip each, hollow once knocked out.
		var em: int = it["elem_max"]
		if em > 0 and s >= 18.0:
			var ea0: int = it["elem"]
			var cc := Color.WHITE if ea0 >= em else (Color("#ffcc4d") if ea0 * 2 >= em else Color("#ff6b4a"))
			draw_string(font, Vector2(c.x - h + s * 0.08, c.y + h - s * 0.07), str(ea0),
				HORIZONTAL_ALIGNMENT_LEFT, -1, int(s * 0.27), cc)
		if em > 0 and s >= 18.0 and not it.get("no_pips", false):
			var ea: int = it["elem"]
			var pw := clampf(s * 0.11, 4.0, 9.0)
			var gap := pw * 1.4
			var cnt := mini(em, 8)
			var x0 := c.x - (cnt * gap - (gap - pw)) * 0.5
			var py := c.y + h + 3.0
			for k in cnt:
				var r := Rect2(x0 + k * gap, py, pw, pw)
				if k < ea:
					draw_rect(r.grow(1.0), Color(0, 0, 0, 0.6))
					draw_rect(r, Color.WHITE)
				else:
					draw_rect(r, Color(0, 0, 0, 0.35))
					draw_rect(r, Color(1, 1, 1, 0.9), false, 1.0)
		var label: String = it["label"]
		if label != "" and s >= 44.0:
			var fs := int(s * 0.2)
			var tw := font.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
			var lp := Vector2(c.x - tw * 0.5, c.y + h + fs + clampf(s * 0.11, 4.0, 9.0) + 6.0)
			draw_string_outline(font, lp, label, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, 3, Color(0, 0, 0, 0.8))
			draw_string(font, lp, label, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color.WHITE)
	# Under fire: a bar along the top edge, red when hit in the last two minutes.
	var fire: float = it["fire"]
	if not ghost and fire > 0.05:
		var hit := (int(it["flags"]) & 64) != 0
		draw_rect(Rect2(c.x - h + s * 0.07, c.y - h + s * 0.04, (s - s * 0.14) * fire, maxf(1.5, s * 0.05)),
			Color("#ff6b4a") if hit else Color("#ffcc4d"))
	# Morale: own units shaken below 45 (corner flag), broken below 25; anyone seen falling back is broken.
	var mor: float = it["morale"]
	var broken := (int(it["flags"]) & 16) != 0 or (mor >= 0.0 and mor < 25.0)
	if not ghost and broken:
		_hatch(c, s)
	elif not ghost and mor >= 0.0 and mor < 45.0:
		draw_colored_polygon(PackedVector2Array([c + Vector2(h, -h + s * 0.02), c + Vector2(h, -h + s * 0.3),
			c + Vector2(h - s * 0.28, -h + s * 0.02)]), Color("#ffcc4d"))
	# Order badge: M move, R road march, D deliberate, A assault, F falling back, G regroup, S rest.
	var badge: String = it["badge"]
	if badge != "" and s >= 22.0 and not ghost:
		var br := s * 0.17
		var bc := c + Vector2(-h + s * 0.02 + br * 0.6, -h + s * 0.02 + br * 0.6)
		draw_circle(bc, br, SYMBOL_INK)
		_text_center(bc, badge, int(br * 1.3), Color.WHITE)
	# Zone dot for own units, outside the right edge.
	if side == 0 and int(it["zone"]) > 0 and not ghost:
		var zp := c + Vector2(h + 5.0, 0)
		draw_circle(zp, 3.5, ZONE_COLORS[int(it["zone"])])
		draw_arc(zp, 3.5, 0, TAU, 12, INK, 1.0)
	# Orders still travelling down the chain of command.
	var oin: float = it["order_in"]
	if oin >= 0.0:
		var t := "+%d′" % int(ceil(oin))
		var fs := 11
		var tw := font.get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		var tr := Rect2(Vector2(c.x - tw * 0.5 - 4, c.y - h - 17), Vector2(tw + 8, 14))
		draw_rect(tr, SIGNAL)
		draw_string(font, Vector2(tr.position.x + 4, tr.end.y - 3), t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color("#221800"))
	var roe: int = it["roe"]
	var so: int = it.get("standoff", -1)
	if (roe > 0 or so > 0) and not ghost:
		var tag := ""
		if roe > 0:
			tag = "RET FIRE" if roe == 1 else "HOLD FIRE"
		if so > 0:
			tag += (" " if tag != "" else "") + "S/O %.1fkm" % (so * 0.5)
		var tp := Vector2(c.x - h, c.y + h + (20.0 if s < 44.0 else s * 0.2 + 24.0))
		draw_string_outline(font, tp, tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 9, 3, Color(0, 0, 0, 0.85))
		draw_string(font, tp, tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 9, Color(1.0, 0.8, 0.45))
	# Team colour: a frame in the team's colour (thinner and dashed-looking when the member
	# has left the formation on its own orders); the leader carries a pennant in it.
	var tcol: int = it.get("tcol", -1)
	if tcol >= 0 and not ghost:
		var tc: Color = TEAM_COLORS[tcol % TEAM_COLORS.size()]
		var role: int = it.get("role", -1)
		_outline(_rrect(c, s + s * 0.1, rad * 1.3), tc, maxf(2.0, s * (0.08 if role != 0 else 0.04)))
		if role == 2:
			var px := c.x + h - s * 0.05
			var py := c.y - h - s * 0.42
			draw_line(Vector2(px, py), Vector2(px, c.y - h), SYMBOL_INK, maxf(1.5, s * 0.04))
			draw_colored_polygon(PackedVector2Array([Vector2(px, py), Vector2(px + s * 0.34, py + s * 0.1),
				Vector2(px, py + s * 0.2)]), tc)
	if it["sel"]:
		_outline(_rrect(c, s + s * 0.24, rad * 1.5), SIGNAL, maxf(2.0, s * 0.09))


## Diagonal white hatching across the counter (broken morale).
func _hatch(c: Vector2, s: float) -> void:
	var h := s * 0.5
	var col := Color(1, 1, 1, 0.55)
	for k in range(-2, 3):
		var x0: float = -h + float(k) * s * 0.3
		# Line from (x0, h) going up-right at 45 degrees, clipped to the square.
		var a := Vector2(x0, h)
		var b := Vector2(x0 + s, -h)
		if a.x < -h:
			a = Vector2(-h, h - (-h - x0))
		if b.x > h:
			b = Vector2(h, -h + (b.x - h))
		if a.x < b.x:
			draw_line(c + a, c + b, col, s * 0.07)


## NATO-style unit symbol inside the symbol box.
func _draw_symbol(r: Rect2, type: int, ink: Color, lw: float) -> void:
	var c := r.get_center()
	match type:
		0: # HQ
			_text_center(c + Vector2(0, r.size.y * 0.04), "HQ", int(maxf(6.0, r.size.y * 0.66)), ink)
		1: # Armour: oval
			_stadium(r, ink, lw)
		2: # Mechanised infantry: cross and oval
			draw_line(r.position, r.end, ink, lw)
			draw_line(Vector2(r.position.x, r.end.y), Vector2(r.end.x, r.position.y), ink, lw)
			_stadium(r, ink, lw)
		3: # Infantry: cross
			draw_line(r.position, r.end, ink, lw)
			draw_line(Vector2(r.position.x, r.end.y), Vector2(r.end.x, r.position.y), ink, lw)
		4: # Recon: single diagonal
			draw_line(Vector2(r.position.x, r.end.y), Vector2(r.end.x, r.position.y), ink, lw)
		5: # Self-propelled artillery: oval and dot
			_stadium(r, ink, lw)
			draw_circle(c, r.size.y * 0.12, ink)


## The tracked-vehicle oval: a stadium inside the symbol box.
func _stadium(r: Rect2, ink: Color, lw: float) -> void:
	var box := Rect2(r.position + Vector2(r.size.x * 0.19, r.size.y * 0.24), Vector2(r.size.x * 0.62, r.size.y * 0.52))
	var rad := box.size.y * 0.5
	var pts := PackedVector2Array()
	for j in 9:
		var a := PI * 0.5 + PI * j / 8.0
		pts.append(Vector2(box.position.x + rad, box.position.y + rad) + Vector2(cos(a), sin(a)) * rad)
	for j in 9:
		var a := -PI * 0.5 + PI * j / 8.0
		pts.append(Vector2(box.end.x - rad, box.position.y + rad) + Vector2(cos(a), sin(a)) * rad)
	pts.append(pts[0])
	draw_polyline(pts, ink, lw, true)


## Echelon marks above the symbol box: one dot a section, three a platoon, a bar a company or battery.
func _draw_echelon(ech: String, p: Vector2, s: float, col: Color) -> void:
	var d := s * 0.04
	var gap := s * 0.11
	match ech:
		"sec":
			draw_circle(p, d, col)
		"plt":
			for i in 3:
				draw_circle(p + Vector2((i - 1) * gap, 0), d, col)
		"coy", "btty":
			draw_rect(Rect2(p.x - d * 0.6, p.y - s * 0.06, d * 1.2, s * 0.12), col)
		"bn":
			for i in 2:
				draw_rect(Rect2(p.x + (i - 0.5) * gap - d * 0.6, p.y - s * 0.06, d * 1.2, s * 0.12), col)


# --- Company grouping on the theater map ---------------------------------------------

## Own units merge with nearby members of the same company. Hostile units are
## grouped by proximity only: the player does not know how they are organised.
func _draw_grouped(xf: Transform2D, view: Rect2) -> void:
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	var clusters: Array = []
	var by_key := {}
	for i in snap.size() / stride:
		var o := i * stride
		var s: Vector2 = xf * main.disp[i]
		if not view.grow(GROUP_RADIUS).has_point(s):
			continue
		var side := int(snap[o + 1])
		var flags := int(snap[o + 7])
		var ghost := (flags & 2) != 0
		var team := int(snap[o + 20])
		var key := "g" if ghost else ("h" if side != 0 else ("t%d" % team if team >= 0 else "f%d" % int(snap[o + 10])))
		var list: Array = by_key.get(key, [])
		var cl: Dictionary = {}
		for c in list:
			if (c["anchor"] as Vector2).distance_to(s) <= GROUP_RADIUS:
				cl = c
				break
		if cl.is_empty():
			cl = {"key": key, "side": side, "ghost": ghost, "anchor": s, "sum": Vector2.ZERO, "ids": [],
				"types": {}, "str_sum": 0.0, "str_n": 0, "zone": 0, "flags": 0, "order_min": -1.0, "first": o, "row": i,
				"fire": 0.0, "mor_min": 1000.0, "el": 0, "el_max": 0}
			list.append(cl)
			by_key[key] = list
			clusters.append(cl)
		cl["ids"].append(int(snap[o]))
		cl["sum"] += s
		var t := int(snap[o + 2])
		cl["types"][t] = cl["types"].get(t, 0) + 1
		if snap[o + 5] >= 0.0:
			cl["str_sum"] += snap[o + 5]
			cl["str_n"] += 1
		if snap[o + 19] > 0.0:
			cl["el"] += int(snap[o + 18])
			cl["el_max"] += int(snap[o + 19])
		cl["zone"] = maxi(cl["zone"], int(snap[o + 6]))
		cl["flags"] = int(cl["flags"]) | flags
		if snap[o + 8] >= 0.0:
			cl["mor_min"] = minf(cl["mor_min"], snap[o + 8])
		if snap[o + 9] >= 0.0 and (cl["order_min"] < 0.0 or snap[o + 9] < cl["order_min"]):
			cl["order_min"] = snap[o + 9]

	# Layout: own companies keep their true place; hostile and last-seen groups
	# that would cover them are pushed aside, with a leader line back.
	var placed: Array = []
	for pass_i in 3:
		for cl in clusters:
			var ghost: bool = cl["ghost"]
			var side: int = cl["side"]
			if pass_i == 0 and (ghost or side != 0): continue
			if pass_i == 1 and (ghost or side == 0): continue
			if pass_i == 2 and not ghost: continue
			var true_pos: Vector2 = cl["sum"] / (cl["ids"] as Array).size()
			var pos := true_pos
			var min_sep := (COMPANY_COUNTER + 22.0) if (cl["ids"] as Array).size() > 1 else (THEATER_COUNTER + 12.0)
			for attempt in 8:
				var clash := false
				for other in placed:
					var op: Vector2 = other
					if pos.distance_to(op) < min_sep:
						var dir := (pos - op).normalized() if pos.distance_to(op) > 1.0 else Vector2(1, -0.7).normalized()
						pos = op + dir * min_sep
						clash = true
				if not clash:
					break
			placed.append(pos)
			cl["pos"] = pos
			cl["true_pos"] = true_pos

	# Draw ghosts first, then friends, then hostiles on top.
	for pass_i in 3:
		for cl in clusters:
			var ghost: bool = cl["ghost"]
			var side: int = cl["side"]
			if pass_i == 0 and not ghost: continue
			if pass_i == 1 and (ghost or side != 0): continue
			if pass_i == 2 and (ghost or side == 0): continue
			var ids: Array = cl["ids"]
			var center: Vector2 = cl["pos"]
			var true_pos: Vector2 = cl["true_pos"]
			if not view.has_point(center):
				continue
			if center.distance_to(true_pos) > 2.0:
				draw_line(true_pos, center, Color(0, 0, 0, 0.75), 1.5)
				draw_circle(true_pos, 3.0, Color(0, 0, 0, 0.85))
			var sel := false
			for id in ids:
				if main.selected.has(id):
					sel = true
					break
			groups.append({"pos": center, "ids": ids, "side": side})
			var o: int = cl["first"]
			var item := _row_item(o, int(cl["row"]))
			item["sel"] = sel
			if ids.size() == 1:
				_draw_counter(center, THEATER_COUNTER, item)
				drawn.append({"rect": Rect2(center - Vector2.ONE * THEATER_COUNTER * 0.5, Vector2.ONE * THEATER_COUNTER),
					"id": item["id"], "side": side})
				continue
			var label := ""
			var team_id := int(snap[o + 20])
			if side == 0 and team_id >= 0:
				var tm: Dictionary = main._team(team_id)
				label = "%s  ×%d" % [tm.get("name", "Team"), ids.size()]
				item["role"] = 1
			elif side == 0:
				label = "%s  ×%d" % [main.formation_name(int(snap[o + 10]), int(snap[o])), ids.size()]
			elif ghost:
				label = "Last seen ×%d" % ids.size()
			else:
				label = "Hostile ×%d" % ids.size()
			item["type"] = _dominant_type(cl["types"])
			item["echelon"] = "coy"
			item["strength"] = cl["str_sum"] / cl["str_n"] if cl["str_n"] > 0 else -1.0
			item["elem"] = cl["el"]
			item["elem_max"] = cl["el_max"]
			item["no_pips"] = true
			item["flags"] = cl["flags"]
			item["zone"] = cl["zone"]
			item["order_in"] = cl["order_min"]
			item["morale"] = cl["mor_min"] if cl["mor_min"] < 1000.0 else -1.0
			item["badge"] = ""
			item["label"] = ""
			item["roe"] = 0
			_draw_company(center, item, ids.size(), label)


func _dominant_type(types: Dictionary) -> int:
	var best := 0
	var best_n := -1
	for t in types:
		# Combat types outrank the HQ when choosing the company symbol.
		var n: int = types[t] * (1 if t == 0 else 10)
		if n > best_n:
			best_n = n
			best = t
	return best


## A company counter: the counter on two offset frames, with a count badge and the name.
func _draw_company(c: Vector2, it: Dictionary, count: int, label: String) -> void:
	var s := COMPANY_COUNTER
	var side: int = it["side"]
	var ghost: bool = it["ghost"]
	var back: Color = (SIDE_COL[clampi(side, 0, 1)] as Color).darkened(0.25)
	if ghost:
		back.a = 0.4
	for k in [2, 1]:
		var bc: Vector2 = c + Vector2(s * 0.09, -s * 0.09) * float(k)
		var pts := _rrect(bc, s, s * 0.11)
		draw_colored_polygon(pts, back)
		_outline(pts, Color(0, 0, 0, 0.45), 1.0)
	_draw_counter(c, s, it)
	var badge := c + Vector2(s * 0.5 + 8, -s * 0.5)
	draw_circle(badge, 9.0, Color(0.1, 0.1, 0.12, 0.95 if not ghost else 0.5))
	_text_center(badge, str(count), 11, Color.WHITE)
	var lp := Vector2(c.x - s * 0.5 - 6, c.y + s * 0.5 + 16)
	draw_string_outline(font, lp, label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, 4, Color(0, 0, 0, 0.8))
	draw_string(font, lp, label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.92, 0.96, 1.0) if side == 0 else Color(1.0, 0.82, 0.78))


# --- Orders, routes and command links -------------------------------------------------

## A route in the unit's colour with an arrowhead, as in the Forward Defense engine.
func _stroke_route(pts: PackedVector2Array, col: Color, w: float, dash: float) -> void:
	if pts.size() < 2:
		return
	if dash > 0.0:
		for i in pts.size() - 1:
			draw_dashed_line(pts[i], pts[i + 1], col, w, dash)
	else:
		draw_polyline(pts, Color(0, 0, 0, 0.45), w + 2.0, true)
		draw_polyline(pts, col, w, true)
	var a := pts[pts.size() - 2]
	var b := pts[pts.size() - 1]
	if a.distance_to(b) < 0.5 and pts.size() > 2:
		a = pts[pts.size() - 3]
	var ang := (b - a).angle()
	var hs := maxf(7.0, w * 3.2)
	draw_colored_polygon(PackedVector2Array([b, b - Vector2.from_angle(ang - 0.45) * hs, b - Vector2.from_angle(ang + 0.45) * hs]), col)


func _signal_tag(p: Vector2, text: String) -> void:
	var fs := 12
	var tw := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
	var r := Rect2(p + Vector2(10, -20), Vector2(tw + 10, 17))
	draw_rect(r, SIGNAL)
	draw_string(font, Vector2(r.position.x + 5, r.end.y - 4), text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, Color("#221800"))


func _draw_orders(xf: Transform2D) -> void:
	var w := clampf(hex_px() * 0.1, 2.0, 6.0)
	var own_col: Color = SIDE_COL[0].lightened(0.35)
	for id in main.selected:
		var info: Dictionary = main.world.get_unit_info(id)
		if info.is_empty() or not info.get("alive", false) or int(info["side"]) != 0:
			continue
		var start: Vector2 = xf * main.display_position(id)
		var path: PackedVector2Array = info.get("path", PackedVector2Array())
		if String(info.get("group_role", "")) == "in formation":
			path = PackedVector2Array() # members keep station on the leader: only the leader's route is drawn
		if path.size() >= 2:
			var step := maxi(1, path.size() / 400)
			var pts := PackedVector2Array([start])
			for i in range(step, path.size(), step):
				pts.append(xf * path[i])
			pts.append(xf * path[path.size() - 1])
			_stroke_route(pts, own_col, w, 0.0)
			var end := pts[pts.size() - 1]
			if info.has("eta_minutes"):
				var eta := int(info["eta_minutes"])
				var t := "%.1f km, %dh%02d" % [float(info["route_km"]), eta / 60, eta % 60]
				draw_string_outline(font, end + Vector2(10, 18), t, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, 4, Color(0, 0, 0, 0.85))
				draw_string(font, end + Vector2(10, 18), t, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.9, 0.97, 1.0))
		if info.get("pending", false):
			var to: Vector2 = xf * (info["pending_target"] as Vector2)
			if to.distance_to(start) > 4.0:
				_stroke_route(PackedVector2Array([start, to]), own_col, w * 0.9, 8.0)
			else:
				to = start
			_signal_tag(to, "%s +%d′" % [info.get("pending_kind", "Order"), int(info.get("pending_in", 0))])


## Chain of command on the map: each team member's dashed line to its leader in the team
## colour, other units' line to the company HQ, and dashed rings for each commander's zone of
## effective command. Shown for the selection, or for everyone with the Command toggle (T).
func _draw_command_links(xf: Transform2D) -> void:
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	var n := snap.size() / stride
	var hq_row := {}     # formation -> row of its HQ
	var lead_row := {}   # team id -> row of its leader
	for i in n:
		var o := i * stride
		if int(snap[o + 1]) != 0:
			continue
		if int(snap[o + 2]) == 0:
			hq_row[int(snap[o + 10])] = i
		if int(snap[o + 21]) == 2:
			lead_row[int(snap[o + 20])] = i
	var all: bool = main.show_cmd
	var sel_teams := {}
	for id in main.selected:
		if main.disp_row.has(id):
			var t := int(snap[int(main.disp_row[id]) * stride + 20])
			if t >= 0:
				sel_teams[t] = true
	var w := clampf(hex_px() * 0.045, 1.0, 3.0)
	var dash := clampf(hex_px() * 0.24, 4.0, 14.0)
	var zones := {} # row -> colour of the zone ring to draw
	for i in n:
		var o := i * stride
		if int(snap[o + 1]) != 0:
			continue
		var id := int(snap[o])
		var team := int(snap[o + 20])
		var role := int(snap[o + 21])
		var is_sel: bool = main.selected.has(id)
		var a: Vector2 = xf * main.disp[i]
		if team >= 0 and role != 2 and lead_row.has(team) and (all or sel_teams.has(team)):
			var tc: Color = TEAM_COLORS[int(snap[o + 22]) % TEAM_COLORS.size()]
			tc.a = 0.9 if role == 1 else 0.5
			draw_dashed_line(a, xf * main.disp[lead_row[team]], tc, w + 0.5, dash)
			zones[lead_row[team]] = TEAM_COLORS[int(snap[o + 22]) % TEAM_COLORS.size()]
		elif int(snap[o + 2]) != 0 and hq_row.has(int(snap[o + 10])) and (is_sel or (all and role == 2)) and main.selected.size() <= 12:
			draw_dashed_line(a, xf * main.disp[hq_row[int(snap[o + 10])]], Color(0.1, 0.12, 0.1, 0.8), w, dash)
			zones[hq_row[int(snap[o + 10])]] = Color(0.95, 0.95, 0.9)
		if snap[o + 23] > 0.0 and (all or is_sel):
			zones[i] = TEAM_COLORS[int(snap[o + 22]) % TEAM_COLORS.size()] if role == 2 else Color(0.95, 0.95, 0.9)
	var zoom: float = main.camera.zoom.x
	for row in zones:
		var o: int = int(row) * stride
		var r: float = snap[o + 23] * 500.0 * zoom
		if r < 6.0:
			continue
		var col: Color = zones[row]
		col.a = 0.8
		_dashed_circle(xf * main.disp[row], r, col, 2.0)


func _dashed_circle(c: Vector2, r: float, col: Color, w: float) -> void:
	var segs := clampi(int(r / 6.0), 24, 120)
	for k in range(0, segs, 2):
		var a0 := TAU * k / segs
		var a1 := TAU * (k + 1) / segs
		draw_arc(c, r, a0, a1, 4, col, w, true)


# --- Wrecks, fire effects, hover -----------------------------------------------------------

func _draw_wrecks(xf: Transform2D, view: Rect2) -> void:
	var k := hex_px()
	if k < 6.0:
		return
	var d := maxf(4.0, k * 0.25)
	var col := Color(0.16, 0.12, 0.08, 0.7)
	for wr in main.wrecks:
		var p: Vector2 = xf * (wr["pos"] as Vector2)
		if not view.has_point(p):
			continue
		draw_line(p + Vector2(-d, -d), p + Vector2(d, d), col, maxf(1.5, k * 0.08))
		draw_line(p + Vector2(d, -d), p + Vector2(-d, d), col, maxf(1.5, k * 0.08))


## Tracers, shell bursts and explosions, timed in real milliseconds like the engine's playback.
func _draw_fx(xf: Transform2D) -> void:
	var now := Time.get_ticks_msec()
	var k := maxf(hex_px(), 9.0)
	for f in main.fx:
		var age := float(now - int(f["t0"]))
		if age > 1000.0:
			continue
		var a: Vector2 = xf * (f["a"] as Vector2)
		var b: Vector2 = xf * (f["b"] as Vector2)
		match String(f["k"]):
			"shot":
				var t := minf(1.0, age / 260.0)
				var alpha := maxf(0.0, 1.0 - age / 800.0)
				var tc := Color("#9cc4ff") if int(f["side"]) == 0 else Color("#ffb19c")
				tc.a = alpha
				draw_line(a, a.lerp(b, t), tc, maxf(1.2, k * 0.06))
				var head := Color("#fff6c8")
				head.a = alpha
				draw_line(a.lerp(b, maxf(0.0, t - 0.25)), a.lerp(b, t), head, maxf(1.0, k * 0.03))
				if age > 200.0:
					var pr := (age - 200.0) / 600.0
					var ic := Color("#ffd166")
					ic.a = maxf(0.0, 1.0 - pr)
					draw_circle(b + Vector2(k * 0.2, -k * 0.1), k * 0.22 * (0.6 + pr), ic)
			"arty", "incoming":
				if String(f["k"]) == "arty" and age < 150.0:
					var mc := Color("#fff6c8")
					mc.a = 1.0 - age / 150.0
					draw_circle(a, k * 0.25, mc) # muzzle flash at the guns
				var puffs: PackedVector2Array = f["puffs"]
				for i in puffs.size():
					var a2 := age - i * 90.0 - (200.0 if String(f["k"]) == "arty" else 0.0)
					if a2 < 0.0 or a2 > 700.0:
						continue
					var q := a2 / 700.0
					var pc := Color("#ffd166") if q < 0.25 else Color(0.35, 0.31, 0.27, 0.8)
					pc.a *= maxf(0.0, 1.0 - q)
					draw_circle(b + Vector2(puffs[i].x * k * 2.2, puffs[i].y * k * 2.0), k * (0.18 + q * 0.5), pc)
			"boom":
				var q := minf(1.0, age / 900.0)
				var bc := Color("#ff6a2a")
				bc.a = maxf(0.0, 1.0 - q)
				draw_circle(b, k * (0.3 + q * 0.9), bc)


## Outline of the hex under the mouse once hexes are big enough to point at.
func _draw_hover(xf: Transform2D) -> void:
	var k := hex_px()
	if k < 9.0 or main.world == null:
		return
	var wp: Vector2 = xf.affine_inverse() * get_local_mouse_position()
	var hc: Vector2 = main.world.hex_to_world(main.world.world_to_hex(wp))
	var sc: Vector2 = xf * hc
	var pts := PackedVector2Array()
	for i in 7:
		var a := deg_to_rad(60.0 * i - 30.0)
		pts.append(sc + Vector2(cos(a), sin(a)) * k * 0.97)
	draw_polyline(pts, Color(0.07, 0.08, 0.06, 0.85), 1.6, true)


# --- Line of sight and zones ------------------------------------------------------------

func _draw_los(xf: Transform2D, zoom: float) -> void:
	var hexes: PackedVector2Array = main.los_hexes
	if hexes.is_empty():
		return
	var corners := PackedVector2Array()
	var size_m := 500.0 / sqrt(3.0)
	for i in 6:
		var a := deg_to_rad(60.0 * i - 30.0)
		corners.append(Vector2(cos(a), sin(a)) * size_m * zoom)
	var col := Color(0.3, 1.0, 0.8, 0.22)
	for h in hexes:
		var s: Vector2 = xf * h
		var poly := PackedVector2Array()
		for k in 6:
			poly.append(s + corners[k])
		draw_colored_polygon(poly, col)


func _draw_zones(xf: Transform2D, zoom: float) -> void:
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	for i in snap.size() / stride:
		var o := i * stride
		if int(snap[o + 1]) != 0:
			continue
		var s: Vector2 = xf * Vector2(snap[o + 3], snap[o + 4])
		match int(snap[o + 6]):
			2: draw_arc(s, 10000.0 * zoom, 0, TAU, 72, Color(1.0, 0.25, 0.2, 0.45), 1.5)  # hot: 10 km
			1: draw_arc(s, 40000.0 * zoom, 0, TAU, 120, Color(1.0, 0.75, 0.2, 0.35), 1.5) # warm: 40 km


func _draw_pings(xf: Transform2D) -> void:
	var now := Time.get_ticks_msec()
	for p in main.pings:
		var age := (now - int(p["t"])) / 1000.0
		if age > 3.0:
			continue
		var s: Vector2 = xf * (p["pos"] as Vector2)
		var col: Color = p["color"]
		col.a = 1.0 - age / 3.0
		draw_arc(s, 12.0 + age * 40.0, 0, TAU, 32, col, 3.0)
