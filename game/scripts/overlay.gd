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
var drawn_holdings: Array = []      # last drawn holding markers: {rect, id} (click-selection)


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
	if main.map_view.contour_root.visible:
		_draw_topo(xf, zoom, view)
	_draw_towns(xf, zoom, view)
	if main.show_los:
		_draw_los(xf, zoom)
	_draw_wrecks(xf, view)
	if main.show_supply:
		_draw_supply_routes(xf)
	_draw_holdings(xf, zoom, view)
	_draw_command_links(xf)
	_draw_mount_links(xf)
	_draw_fire_links(xf)
	_draw_orders(xf)
	_draw_units(xf, view)
	_draw_net_outlines()
	_draw_fx(xf)
	_draw_pings(xf)
	_draw_hover(xf)
	draw_ms = (Time.get_ticks_usec() - t0) / 1000.0


# --- Topographic mode: heights -----------------------------------------------------------
# Height figures along the index contours and spot heights on summits, for the chunks whose
# contour lines the map view is showing. Figures that would crowd each other are left out.

const TOPO_INK := Color("#2a1a0c")
const TOPO_HALO := Color(0.95, 0.94, 0.86, 0.8)

func _draw_topo(xf: Transform2D, zoom: float, view: Rect2) -> void:
	var px := 500.0 * zoom
	var taken := {}
	var cell := 86.0
	var fs := 11 if px < 30.0 else 12
	# Summits first: they are the fewer and the more useful.
	for entry in main.map_view.contours.values():
		var spots: PackedFloat32Array = entry["spots"]
		for i in range(0, spots.size(), 3):
			var p: Vector2 = xf * Vector2(spots[i], spots[i + 1])
			if not view.has_point(p):
				continue
			var key := Vector2i(floori(p.x / cell), floori(p.y / cell))
			if taken.has(key):
				continue
			taken[key] = true
			draw_colored_polygon(PackedVector2Array([p + Vector2(0, -5), p + Vector2(4.5, 3), p + Vector2(-4.5, 3)]), TOPO_INK)
			var text := str(int(spots[i + 2]))
			draw_string_outline(font, p + Vector2(7, 4), text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs + 1, 4, TOPO_HALO)
			draw_string(font, p + Vector2(7, 4), text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs + 1, TOPO_INK)
	if px < 9.0:
		return
	for entry in main.map_view.contours.values():
		var labels: PackedFloat32Array = entry["labels"]
		for i in range(0, labels.size(), 4):
			var p: Vector2 = xf * Vector2(labels[i], labels[i + 1])
			if not view.has_point(p):
				continue
			var key := Vector2i(floori(p.x / cell), floori(p.y / cell))
			if taken.has(key):
				continue
			taken[key] = true
			var text := str(int(labels[i + 3]))
			var sz := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs)
			draw_set_transform(p, labels[i + 2], Vector2.ONE)
			draw_string_outline(font, Vector2(-sz.x * 0.5, sz.y * 0.3), text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, 5, TOPO_HALO)
			draw_string(font, Vector2(-sz.x * 0.5, sz.y * 0.3), text, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, TOPO_INK)
	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)


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
const MIN_COUNTER := 10.0
const THEATER_COUNTER := 22.0
const COMPANY_COUNTER := 28.0
const ECHELONS := ["coy", "plt", "plt", "plt", "sec", "btty", "col"] # by unit type (col: a supply column)
const HOLDING_COL := [Color("#2c5ea8"), Color("#b0342b"), Color("#7a7f74")] # owner: company, hostile, independent
const HOLDING_GLYPH := ["T", "F", "D", "A"]  # town, farm collective, fuel depot, ammo dump
const SF_MILITIA := 35              # own or seen units: 1 for a town's militia
const SF_CONVOY := 36               # own supply columns: convoy state, else -1
const SF_CARGO_T := 37              # own supply columns: tonnes aboard, else -1
const CONVOY_COL := [Color("#9fb7a0"), Color("#8bd450"), Color("#f4f2ea"), Color("#5ab0ff"), Color("#f4f2ea"), Color("#c8c8c8")]
const TEAM_COLORS := [Color("#f2c14e"), Color("#39c0b0"), Color("#c77dff"), Color("#8bd450"),
	Color("#ff8c42"), Color("#5ab0ff"), Color("#ff6f91"), Color("#d9d9d9")]

# Snapshot fields for the command overlays (MurksWorld::SnapshotField).
const SF_FIRED_AT := 14
const SF_TARGET := 15
const SF_CMD_BOSS := 25
const SF_CMD_LINK := 26             # -1 top, 0 no commander, 1 in zone, 2 outside (x1.5), 3 courier
const SF_FS_ROLE := 27              # -1 no fire net, 0 guns, 1 observer on the net, 2 calls through a relay
const SF_FS_STATUS := 28            # 0 cut off, 1 relayed, 2 direct
const SF_FS_BATTERY := 29
const SF_FS_VIA := 30
const SF_FS_RANGE := 31             # guns: range in hexes
const SF_CARRY := 32                # APC/IFV platoons: squads aboard (0 while out), else -1
const SF_MOUNT_LINK := 33           # carrier <-> its squads on foot, else -1
const SF_SUPP := 34                 # own units: suppression 0-100, +1000 when pinned
const HQ_INK := Color(0.95, 0.95, 0.9)
const CMD_OUT := Color("#ffcc4d")   # outside the commander's zone
const CMD_CUT := Color("#ff6b4a")   # courier, or no commander
const FS_COL := Color("#ff6f3c")    # fire support net (artillery scarlet)
const FS_CUT := Color("#a8a8a8")    # cut off from the fire net


func grouping_active() -> bool:
	return main.group_companies and 500.0 * main.camera.zoom.x < GROUP_PX


func hex_px() -> float:
	return HEX_R * main.camera.zoom.x


## Counter side length: a fixed share of the hex (0.9 x the hex corner radius, about half
## the hex's width), so a counter and its pips always sit inside their own hex. Below
## MIN_COUNTER pixels it stops shrinking; that still fits the hex down to 14 px per hex,
## where company grouping takes over. Only with grouping switched off, further out than
## that, is a counter larger than its hex (nothing could be shown otherwise).
func counter_size() -> float:
	return maxf(hex_px() * 0.9, MIN_COUNTER)


## Everything the counter needs from one snapshot row.
func _row_item(o: int, i: int) -> Dictionary:
	var snap: PackedFloat32Array = main.snapshot
	var id := int(snap[o])
	var side := int(snap[o + 1])
	var flags := int(snap[o + 7])
	var item := {"id": id, "side": side, "type": int(snap[o + 2]), "flags": flags, "ghost": (flags & 2) != 0,
		"strength": snap[o + 5], "zone": int(snap[o + 6]), "morale": snap[o + 8], "order_in": snap[o + 9],
		"roe": int(snap[o + 12]), "badge": "", "label": "", "fire": 0.0, "sel": main.selected.has(id),
		"echelon": ECHELONS[clampi(int(snap[o + 2]), 0, 6)], "row": i,
		"engage": snap[o + 17], "elem": int(snap[o + 18]), "elem_max": int(snap[o + 19]),
		"team": int(snap[o + 20]), "role": int(snap[o + 21]), "tcol": int(snap[o + 22]), "standoff": int(snap[o + 24]),
		"carry": int(snap[o + SF_CARRY]), "supp": snap[o + SF_SUPP],
		"militia": snap[o + SF_MILITIA] > 0.5, "convoy": int(snap[o + SF_CONVOY]), "cargo": snap[o + SF_CARGO_T]}
	item["veh"] = vehicle_of(id, int(snap[o + 2]))
	if item["militia"]:
		# A town's garrison: one dot a squad, three from the third.
		item["echelon"] = "sec" if int(snap[o + 19]) <= 2 else "plt"
	if side == 0:
		match int(snap[o + 13]):
			1: item["badge"] = ["M", "R", "D", "A"][clampi(int(snap[o + 11]), 0, 3)]
			2: item["badge"] = "F"
			3: item["badge"] = "G"
			4: item["badge"] = "S"
			5: item["badge"] = "E" # squads walking back to embark
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
		st.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
			var la := _layer(a)
			var lb := _layer(b)
			return la < lb if la != lb else _veh_rank(a) < _veh_rank(b))
		# A stack shares the hex: counters shrink a little and fan out along the diagonal,
		# with the most important vehicle on top, so the whole stack stays inside the hex.
		var m: int = st.size()
		var ss := s * maxf(0.7, 1.0 - 0.08 * (m - 1))
		var step := minf(ss * 0.16, (s * 1.25 - ss) / maxf(1.0, m - 1.0))
		for k in m:
			var it: Dictionary = st[k]
			var off := (k - (m - 1) * 0.5) * step
			var c: Vector2 = (it["pos"] as Vector2) + Vector2(off, -off)
			_draw_counter(c, ss, it)
			drawn.append({"rect": Rect2(c - Vector2(ss, ss) * 0.5, Vector2(ss, ss)), "id": it["id"], "side": it["side"]})


func _veh_rank(it: Dictionary) -> int:
	return int(VEHICLE_RANK.get(String(it.get("veh", "")), 0))


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
	if it.get("militia", false):
		# Militia: the garrison's box is tinted, and a small 'm' sits in its corner.
		draw_rect(box, Color(0.85, 0.78, 0.55, 0.45 if ghost else 0.7))
		draw_rect(box, ink, false, lw)
	if show_vehicles and String(it.get("veh", "")) != "":
		_draw_vehicle(box, String(it["veh"]), ink, paper, side == 1, int(it["type"]) == 0)
	else:
		_draw_symbol(box, int(it["type"]), ink, lw)
	if it.get("militia", false) and s >= 22.0:
		draw_string(font, Vector2(box.end.x - s * 0.16, box.end.y - s * 0.03), "m", HORIZONTAL_ALIGNMENT_LEFT, -1, int(s * 0.18), ink)
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
		if label != "" and s >= 40.0:
			var fs := maxi(10, int(s * 0.22))
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
	# Suppression: a bar along the top edge (amber, red when pinned) and a PIN tag.
	var sv: float = it.get("supp", -1.0)
	if sv >= 10.0 and not ghost:
		var pinned := sv >= 1000.0
		var lvl := clampf(sv - (1000.0 if pinned else 0.0), 0.0, 100.0) / 100.0
		var bc := Color("#ff4b3a") if pinned else Color("#ffb52e")
		draw_rect(Rect2(c.x - h, c.y - h - maxf(2.0, s * 0.07), s * lvl, maxf(2.0, s * 0.07)), bc)
		if pinned and s >= 22.0:
			var pfs := int(s * 0.22)
			draw_string_outline(font, Vector2(c.x - h, c.y - h - s * 0.1), "PIN", HORIZONTAL_ALIGNMENT_LEFT, -1, pfs, 3, Color(0, 0, 0, 0.85))
			draw_string(font, Vector2(c.x - h, c.y - h - s * 0.1), "PIN", HORIZONTAL_ALIGNMENT_LEFT, -1, pfs, bc)
	# A supply column: tonnes aboard bottom right (or "empty"), and the leg it is on as its badge.
	var cv: int = it.get("convoy", -1)
	if cv >= 0 and s >= 22.0 and not ghost:
		var cargo: float = it.get("cargo", 0.0)
		var ct := ("%.0f t" % cargo) if cargo >= 0.95 else ("%.1f t" % cargo if cargo > 0.05 else "empty")
		var cfs := int(s * 0.2)
		var cw := font.get_string_size(ct, HORIZONTAL_ALIGNMENT_LEFT, -1, cfs).x
		draw_string(font, Vector2(c.x + h - s * 0.08 - cw, c.y + h - s * 0.07), ct, HORIZONTAL_ALIGNMENT_LEFT, -1, cfs,
			Color("#c8f0a0") if cargo > 0.05 else Color(0.85, 0.85, 0.8))
	# Squads aboard an APC/IFV platoon: "+3" bottom right.
	var carry: int = it.get("carry", -1)
	if carry > 0 and s >= 22.0 and not ghost:
		var ct := "+%d" % carry
		var cfs := int(s * 0.24)
		var cw := font.get_string_size(ct, HORIZONTAL_ALIGNMENT_LEFT, -1, cfs).x
		draw_string(font, Vector2(c.x + h - s * 0.08 - cw, c.y + h - s * 0.07), ct, HORIZONTAL_ALIGNMENT_LEFT, -1, cfs, Color("#c8f0a0"))
	# Order badge: M move, R road march, D deliberate, A assault, F falling back, G regroup, S rest, E embarking.
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
		6: # Supply column: the supply bar across the box, wheels under it
			draw_line(Vector2(r.position.x + r.size.x * 0.12, c.y - r.size.y * 0.08), Vector2(r.end.x - r.size.x * 0.12, c.y - r.size.y * 0.08), ink, lw * 1.6)
			for k in 3:
				draw_circle(Vector2(r.position.x + r.size.x * (0.25 + 0.25 * k), c.y + r.size.y * 0.22), r.size.y * 0.09, ink)


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


# --- Vehicle silhouettes ----------------------------------------------------------------
# With "Vehicles on counters" on (View menu, F7) the symbol box shows the unit's most
# important vehicle in side view instead of its NATO symbol: own units face right, hostile
# ones left. Infantry on foot (dismounted, or with no transport) shows the NATO symbol. The vehicle comes from the unit's equipment list; where that names several
# things ("UAZ-469, R-145 radio"), the one that ranks highest below wins. A company counter
# on the theater map shows the highest-ranking vehicle among its units, and in a stack
# sharing a hex the highest-ranking unit lies on top.

var show_vehicles := true
var _veh_cache := {}      # unit id -> silhouette key
var _veh_cache_tick := 0

## Importance when choosing which vehicle stands for a unit, a company or a stack.
const VEHICLE_RANK := {"tank": 100, "tank_w": 100, "spg": 90, "mlrs": 88, "ifv": 80, "apc_t": 72, "apc_w": 70,
	"gun": 60, "mortar": 55, "recon": 50, "truck": 30, "jeep": 20, "inf": 10}
## Equipment names (start of a word; a letter suffix such as the A in T-55A is allowed).
const VEHICLE_KEYS := {
	"tank": ["T-54", "T-55", "T-62", "T-64", "T-72", "T-80", "T-34", "PT-76"],
	"tank_w": ["M1", "M60", "M48", "Leopard", "Chieftain", "Challenger", "AMX-30", "Centurion"],
	"spg": ["2S1", "2S3", "2S5", "2S7", "2S19", "2S9", "2S4", "DANA", "M109", "M110"],
	"mlrs": ["BM-21", "RM-70", "BM-27", "MLRS", "LARS"],
	"ifv": ["BMP-1", "BMP-2", "BMP-3", "BMD-1", "M2", "M3", "Marder", "Warrior", "AMX-10P"],
	"apc_t": ["MT-LB", "M113", "BTR-50", "OT-62", "FV432"],
	"apc_w": ["BTR-60", "BTR-70", "BTR-80", "BTR-152", "OT-64", "VAB", "Fuchs", "SKOT"],
	"gun": ["D-30", "M-30", "M1938", "D-20", "ML-20", "D-1", "2A65", "2A36", "MT-12", "D-44"],
	"mortar": ["2S12", "2B11", "M-1943", "PM-43", "2B9", "mortar", "mortars"],
	"recon": ["BRDM-1", "BRDM-2", "Luchs", "Scimitar", "Scorpion", "FUG"],
	"truck": ["Ural-375", "Ural-4320", "Ural", "ZIL-131", "ZIL-157", "GAZ-66", "KrAZ-255", "Tatra", "W50", "truck", "trucks"],
	"jeep": ["UAZ-469", "UAZ", "GAZ-69", "Jeep", "Iltis", "M151", "Trabant"],
	"inf": ["men", "squad", "squads", "militia", "rifles"]}
## Used when the equipment names nothing known: by unit type (HQ, tank, mech, infantry, recon, artillery, supply).
const VEHICLE_BY_TYPE := ["jeep", "tank", "ifv", "", "recon", "spg", "truck"]


## The silhouette key for a unit; looked up once from its equipment and remembered.
func vehicle_of(id: int, type: int) -> String:
	var tick := Engine.get_process_frames() / 600
	if tick != _veh_cache_tick:   # equipment can change (new game, load, dismounts)
		_veh_cache_tick = tick
		_veh_cache.clear()
	if _veh_cache.has(id):
		return _veh_cache[id]
	# Infantry has no silhouette of its own. Squads that have dismounted ("3 squads of 8 men
	# (BMP-1)": the carrier they left is only named in brackets) show the NATO symbol, and so
	# does infantry with no transport. Only infantry that still has its vehicles with it
	# ("35 men, 3x BTR-60PB") shows the vehicle.
	var info: Dictionary = main.world.get_unit_info(id)
	var key := classify_equipment(strip_brackets(String(info.get("equipment", ""))))
	if key == "" and type != 3:
		key = VEHICLE_BY_TYPE[clampi(type, 0, VEHICLE_BY_TYPE.size() - 1)]
	if key == "inf":
		key = ""
	_veh_cache[id] = key
	return key


## The equipment string without anything in round brackets.
static func strip_brackets(text: String) -> String:
	var out := ""
	var depth := 0
	for ch in text:
		if ch == "(":
			depth += 1
		elif ch == ")":
			depth = maxi(0, depth - 1)
		elif depth == 0:
			out += ch
	return out


## The highest-ranking vehicle class named in an equipment string, or "" if none is known.
static func classify_equipment(equipment: String) -> String:
	var best := ""
	var best_rank := -1
	var words := equipment.replace(",", " ").replace("(", " ").replace(")", " ").replace(";", " ").split(" ", false)
	for w in words:
		for cls in VEHICLE_KEYS:
			var rank: int = VEHICLE_RANK[cls]
			if rank <= best_rank:
				continue
			for k in VEHICLE_KEYS[cls]:
				var key := String(k)
				if not w.begins_with(key):
					continue
				# "2S1" must not match "2S19", "M2" not "M240": only letters may follow.
				var rest := w.substr(key.length())
				var ok := true
				for ch in rest:
					if not ((ch >= "A" and ch <= "Z") or (ch >= "a" and ch <= "z")):
						ok = false
						break
				if ok and (rest.length() <= 3):
					best = cls
					best_rank = rank
					break
	return best


## Draws a side-view vehicle in the symbol box. Shapes are given in a 100 x 60 frame
## (x forward to the right, y down) and mirrored for the hostile side.
func _draw_vehicle(r: Rect2, key: String, ink: Color, paper: Color, flip: bool, hq: bool) -> void:
	var pad := r.size.y * 0.1
	var area := r.grow(-pad)
	var k := minf(area.size.x / 100.0, area.size.y / 60.0)
	var org := area.get_center() - Vector2(50.0, 30.0) * k
	var P := func(x: float, y: float) -> Vector2:
		return org + Vector2((100.0 - x) if flip else x, y) * k
	var poly := func(pts: Array, col: Color) -> void:
		var out := PackedVector2Array()
		for i in range(0, pts.size(), 2):
			out.append(P.call(float(pts[i]), float(pts[i + 1])))
		draw_colored_polygon(out, col)
	var line := func(x0: float, y0: float, x1: float, y1: float, w: float) -> void:
		draw_line(P.call(x0, y0), P.call(x1, y1), ink, maxf(1.0, w * k), true)
	var wheels := func(xs: Array, y: float, rad: float, col: Color) -> void:
		for x in xs:
			draw_circle(P.call(float(x), y), maxf(0.8, rad * k), col)
	var track := func(x0: float, x1: float, n: int) -> void:
		# Running gear: a track band with road wheels showing in the paper colour.
		poly.call([x0 + 4, 40, x1 - 4, 40, x1, 46, x1 - 6, 54, x0 + 6, 54, x0, 46], ink)
		if k * 3.2 >= 1.0:
			for i in n:
				draw_circle(P.call(x0 + 8.0 + (x1 - x0 - 16.0) * i / maxf(1.0, n - 1.0), 47.0), 3.2 * k, paper)
	match key:
		"tank":   # low hull, rounded turret well forward, long gun
			track.call(6, 94, 5)
			poly.call([8, 40, 14, 31, 88, 31, 96, 40], ink)
			poly.call([36, 31, 40, 20, 46, 16, 60, 16, 66, 20, 70, 31], ink)
			line.call(66, 23, 104, 23, 3.4)
		"tank_w":  # tall slab-sided turret set back, long gun
			track.call(6, 94, 6)
			poly.call([8, 40, 12, 30, 90, 30, 96, 40], ink)
			poly.call([28, 30, 28, 14, 62, 14, 72, 22, 72, 30], ink)
			line.call(70, 21, 106, 21, 3.6)
		"spg":   # big box turret at the rear, barrel raised
			track.call(6, 94, 6)
			poly.call([8, 40, 12, 31, 86, 31, 96, 40], ink)
			poly.call([14, 31, 16, 10, 54, 10, 60, 31], ink)
			line.call(56, 18, 102, 4, 4.2)
		"mlrs":  # truck with a rocket pack raised over the bed
			poly.call([6, 40, 96, 40, 96, 46, 6, 46], ink)
			poly.call([72, 22, 90, 22, 96, 32, 96, 40, 72, 40], ink)
			poly.call([6, 34, 58, 12, 64, 24, 12, 44], ink)
			wheels.call([18, 34, 82], 50, 7, ink)
		"ifv":   # wedge nose, small turret, thin gun
			track.call(4, 94, 6)
			poly.call([4, 40, 4, 28, 70, 26, 98, 38, 96, 40], ink)
			poly.call([40, 27, 44, 16, 58, 16, 62, 27], ink)
			line.call(58, 20, 90, 20, 2.4)
		"apc_t":  # plain box on tracks
			track.call(6, 94, 5)
			poly.call([6, 40, 6, 20, 78, 20, 96, 34, 96, 40], ink)
			line.call(50, 20, 60, 12, 2.4)
		"apc_w":  # boat hull on four big wheels, small turret
			poly.call([2, 26, 74, 24, 98, 36, 90, 46, 8, 46], ink)
			poly.call([54, 25, 57, 15, 68, 15, 71, 25], ink)
			line.call(68, 19, 86, 19, 2.2)
			wheels.call([16, 36, 64, 84], 50, 7.5, ink)
		"recon":  # short scout car on two axles
			poly.call([8, 32, 30, 22, 66, 22, 96, 36, 92, 46, 8, 46], ink)
			poly.call([40, 23, 43, 14, 56, 14, 59, 23], ink)
			line.call(56, 18, 74, 18, 2.2)
			wheels.call([24, 78], 49, 9, ink)
		"gun":   # towed gun: wheel, shield, raised barrel, trail
			line.call(40, 46, 4, 56, 3.4)
			poly.call([34, 20, 42, 20, 46, 46, 36, 46], ink)
			line.call(30, 36, 100, 14, 4.0)
			wheels.call([42], 46, 10, ink)
			if k * 4.0 >= 1.0:
				wheels.call([42], 46, 4, paper)
		"mortar":  # tube on a baseplate with a bipod
			line.call(22, 56, 60, 56, 3.4)
			line.call(30, 54, 68, 8, 5.0)
			line.call(56, 24, 74, 56, 2.4)
			line.call(56, 24, 50, 56, 2.4)
		"truck":  # cab, bonnet and a covered load bed
			poly.call([4, 12, 64, 12, 64, 40, 4, 40], ink)
			poly.call([68, 18, 84, 18, 90, 30, 98, 32, 98, 44, 68, 44], ink)
			poly.call([4, 40, 98, 40, 98, 45, 4, 45], ink)
			wheels.call([16, 32, 84], 50, 7.5, ink)
			if k * 3.4 >= 1.0:
				poly.call([72, 22, 82, 22, 86, 29, 72, 29], paper)
		"jeep":   # light field car
			poly.call([10, 30, 30, 30, 34, 16, 62, 16, 70, 30, 92, 32, 92, 46, 10, 46], ink)
			wheels.call([26, 76], 49, 9, ink)
			if k * 3.4 >= 1.0:
				poly.call([40, 20, 58, 20, 63, 29, 40, 29], paper)
	if hq:
		# Command vehicle: a whip aerial with a pennant.
		line.call(34, 22, 26, -2, 1.6)
		poly.call([26, -2, 44, 3, 27, 9], ink)


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
		"coy", "btty", "col":
			draw_rect(Rect2(p.x - d * 0.6, p.y - s * 0.06, d * 1.2, s * 0.12), col)
		"bn":
			for i in 2:
				draw_rect(Rect2(p.x + (i - 0.5) * gap - d * 0.6, p.y - s * 0.06, d * 1.2, s * 0.12), col)


# --- Holdings and supply routes ---------------------------------------------------------
# A holding is marked with a pennant on a pole at the edge of its hex: owner's colour (company
# blue, hostile red, independent grey), a letter for its kind (T town, F farm collective, D fuel
# depot, A ammo dump), and under it, for the company's own places, loyalty and the militia squads.
# Markers are clickable (drawn_holdings). Supply routes (View > Supply, F3) are drawn from the
# source to the destination along the column's road, coloured by what the column is doing.

func _draw_holdings(xf: Transform2D, zoom: float, view: Rect2) -> void:
	drawn_holdings = []
	var px := 500.0 * zoom
	if px < 1.2:
		return
	var list: Array = main.holdings
	var s := clampf(px * 1.1, 14.0, 34.0)
	for hd in list:
		var p: Vector2 = xf * (hd["pos"] as Vector2)
		if not view.has_point(p):
			continue
		var owner := int(hd["owner"])
		var own: bool = hd.get("own", false)
		# Independent places only show up once the hexes are big enough to matter.
		if owner < 0 and px < 4.0 and int(hd["id"]) != main.selected_holding:
			continue
		var col: Color = HOLDING_COL[2 if owner < 0 else clampi(owner, 0, 1)]
		var base := p + Vector2(px * 0.55, -px * 0.2)
		var top := base + Vector2(0, -s * 1.35)
		var sel: bool = int(hd["id"]) == main.selected_holding
		draw_line(base + Vector2(1, 1), top + Vector2(1, 1), Color(0, 0, 0, 0.5), maxf(1.5, s * 0.08))
		draw_line(base, top, Color(0.95, 0.95, 0.9), maxf(1.5, s * 0.08))
		var flag := PackedVector2Array([top, top + Vector2(s * 1.05, s * 0.3), top + Vector2(0, s * 0.62)])
		draw_colored_polygon(PackedVector2Array([flag[0] + Vector2(1, 1), flag[1] + Vector2(1, 1), flag[2] + Vector2(1, 1)]), Color(0, 0, 0, 0.45))
		draw_colored_polygon(flag, col)
		var closed := flag.duplicate()
		closed.append(flag[0])
		draw_polyline(closed, SIGNAL if sel else Color(0, 0, 0, 0.6), maxf(1.0, s * (0.1 if sel else 0.05)), true)
		var glyph: String = HOLDING_GLYPH[clampi(int(hd["kind"]), 0, 3)]
		_text_center(top + Vector2(s * 0.3, s * 0.3), glyph, int(maxf(8.0, s * 0.42)), Color.WHITE)
		if hd.get("fighting", false):
			draw_circle(top + Vector2(-s * 0.22, 0), s * 0.14, Color("#ff6b4a"))
		if own and s >= 20.0:
			var tag := "%d%%" % int(hd.get("loyalty", 0.0))
			var ms := int(hd.get("militia_squads", 0))
			if ms > 0:
				tag += "  M%d" % ms
			if hd.get("training", false):
				tag += " +"
			if hd.get("contested", false):
				tag += "  !"
			draw_string_outline(font, base + Vector2(s * 0.2, 3), tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, 3, Color(0, 0, 0, 0.85))
			draw_string(font, base + Vector2(s * 0.2, 3), tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, Color(1, 1, 0.9))
		elif not own and s >= 20.0 and int(hd.get("militia_squads", 0)) > 0:
			var mtag := "M%d" % int(hd["militia_squads"])
			draw_string_outline(font, base + Vector2(s * 0.2, 3), mtag, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, 3, Color(0, 0, 0, 0.85))
			draw_string(font, base + Vector2(s * 0.2, 3), mtag, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, Color(1, 0.85, 0.8))
		if sel:
			# The place's footprint.
			var r: float = (int(hd["radius"]) + 0.5) * px * 1.16
			draw_arc(p, r, 0, TAU, 48, SIGNAL, 2.0)
		drawn_holdings.append({"rect": Rect2(top + Vector2(-s * 0.2, -s * 0.15), Vector2(s * 1.4, s * 1.1)).merge(Rect2(base - Vector2(s * 0.3, s * 0.3), Vector2(s * 0.6, s * 0.6))), "id": int(hd["id"])})


func _draw_supply_routes(xf: Transform2D) -> void:
	var routes: Array = main.routes
	var w := clampf(hex_px() * 0.35, 2.0, 7.0)
	for r in routes:
		var a: Vector2 = xf * (r["source_pos"] as Vector2)
		var b: Vector2 = xf * (r["dest_pos"] as Vector2)
		var suspended: bool = r.get("suspended", false)
		var state := int(r.get("state", -1))
		var col: Color = CONVOY_COL[clampi(state, 0, 5)] if state >= 0 else Color(0.6, 0.6, 0.6)
		if r.get("under_fire", false):
			col = Color("#ff6b4a")
		elif r.get("halted", false):
			col = Color("#ffcc4d")
		if suspended or not r.get("active", true):
			col = Color(0.75, 0.75, 0.7, 0.8)
		# The whole route as a faint line, the column's current road on top.
		draw_dashed_line(a, b, Color(col.r, col.g, col.b, 0.45), w * 0.5, 10.0)
		var path: PackedVector2Array = r.get("path", PackedVector2Array())
		if path.size() >= 2:
			var pts := PackedVector2Array()
			var step := maxi(1, path.size() / 300)
			for k in range(0, path.size(), step):
				pts.append(xf * path[k])
			pts.append(xf * path[path.size() - 1])
			draw_polyline(pts, Color(0, 0, 0, 0.5), w + 2.0, true)
			draw_polyline(pts, col, w, true)
		# End markers: a ring at each end, filled at the source.
		draw_circle(a, w * 1.4, col)
		draw_arc(a, w * 1.4, 0, TAU, 16, Color(0, 0, 0, 0.6), 1.0)
		draw_arc(b, w * 1.4, 0, TAU, 16, col, 2.0)
		# Label at the midpoint: source -> destination, state and throughput.
		var mid := (a + b) * 0.5
		var txt := "%s → %s" % [String(r["source_name"]), String(r["dest_name"])]
		var line2 := "SUSPENDED" if suspended else String(r.get("state_name", ""))
		var tpd := float(r.get("tonnes_per_day", 0.0))
		if tpd > 0.0 and not suspended:
			line2 += "  ·  %.1f t/day" % tpd
		if r.has("cargo_tonnes") and float(r["cargo_tonnes"]) > 0.05:
			line2 += "  ·  %.1f t aboard" % float(r["cargo_tonnes"])
		draw_string_outline(font, mid + Vector2(8, -4), txt, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, 4, Color(0, 0, 0, 0.85))
		draw_string(font, mid + Vector2(8, -4), txt, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color(1, 1, 0.92))
		draw_string_outline(font, mid + Vector2(8, 9), line2, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, 4, Color(0, 0, 0, 0.85))
		draw_string(font, mid + Vector2(8, 9), line2, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, col.lightened(0.3))


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
		var veh := vehicle_of(int(snap[o]), t)
		if int(VEHICLE_RANK.get(veh, 0)) > int(VEHICLE_RANK.get(String(cl.get("veh", "")), -1)):
			cl["veh"] = veh
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
			item["veh"] = String(cl.get("veh", ""))
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
	if all:
		_draw_all_command_links(xf, w, dash, zones)
	for i in n:
		var o := i * stride
		if all or int(snap[o + 1]) != 0:
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


## Direct command overlay (T): every own unit linked to the HQ or team leader it takes orders
## from - solid inside the commander's zone, amber dashes outside it (orders x1.5), red short
## dashes beyond radio range (orders go by courier). Fills `zones` with the commanders' rings.
func _draw_all_command_links(xf: Transform2D, w: float, dash: float, zones: Dictionary) -> void:
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	for i in snap.size() / stride:
		var o := i * stride
		if int(snap[o + 1]) != 0:
			continue
		var boss := int(snap[o + SF_CMD_BOSS])
		if boss < 0 or not main.disp_row.has(boss):
			continue
		var bi: int = main.disp_row[boss]
		var col := _commander_color(o, bi)
		var a: Vector2 = xf * main.disp[i]
		var b: Vector2 = xf * main.disp[bi]
		match int(snap[o + SF_CMD_LINK]):
			1:
				var c1 := col
				c1.a = 0.85
				draw_line(a, b, c1, w + 0.5, true)
			2:
				draw_dashed_line(a, b, CMD_OUT, w + 0.5, dash)
			3:
				draw_dashed_line(a, b, CMD_CUT, w + 0.5, dash * 0.5)
		zones[bi] = col


## The commander's colour for the unit at snapshot offset o: the team colour when it answers to
## its team leader (row bi), white for the company HQ.
func _commander_color(o: int, bi: int) -> Color:
	var snap: PackedFloat32Array = main.snapshot
	var tcol := int(snap[o + 22])
	if tcol >= 0 and int(snap[bi * main.stride + 21]) == 2:
		return TEAM_COLORS[tcol % TEAM_COLORS.size()]
	return HQ_INK


## Squads on foot and their vehicles: a dotted line between them when either is selected (or
## with the Direct command overlay on), brighter while the squads are walking back to mount up.
func _draw_mount_links(xf: Transform2D) -> void:
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	var w := clampf(hex_px() * 0.04, 1.0, 2.5)
	for i in snap.size() / stride:
		var o := i * stride
		if int(snap[o + 1]) != 0 or int(snap[o + SF_CARRY]) >= 0:
			continue # draw each pair once, from the squads' side
		var other := int(snap[o + SF_MOUNT_LINK])
		if other < 0 or not main.disp_row.has(other):
			continue
		var id := int(snap[o])
		if not (main.show_cmd or main.selected.has(id) or main.selected.has(other)):
			continue
		var col := Color("#c8f0a0")
		col.a = 0.95 if int(snap[o + 13]) == 5 else 0.6
		draw_dashed_line(xf * main.disp[i], xf * main.disp[int(main.disp_row[other])], col, w, 3.0)


## Fire support overlay (I): each battery's range ring, observers on the fire net linked to the
## guns they reach, relayed callers linked to the observer who passes their calls on, and each
## battery's current fire mission. A selected battery shows its range ring even without it.
func _draw_fire_links(xf: Transform2D) -> void:
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	var all: bool = main.show_fs
	var zoom: float = main.camera.zoom.x
	var w := clampf(hex_px() * 0.045, 1.0, 3.0)
	var dash := clampf(hex_px() * 0.24, 4.0, 14.0)
	var now: float = main.world.get_time_minutes()
	for i in snap.size() / stride:
		var o := i * stride
		if int(snap[o + 1]) != 0:
			continue
		var role := int(snap[o + SF_FS_ROLE])
		if role < 0:
			continue
		var a: Vector2 = xf * main.disp[i]
		if role == 0:
			if not (all or main.selected.has(int(snap[o]))):
				continue
			var r: float = snap[o + SF_FS_RANGE] * 500.0 * zoom
			if r >= 6.0:
				var rc := FS_COL
				rc.a = 0.75
				_dashed_circle(a, r, rc, 2.0)
				var tag := "%s  %.1f km" % [main.short_name(int(snap[o])), snap[o + SF_FS_RANGE] * 0.5]
				var tp := a + Vector2(-font.get_string_size(tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 11).x * 0.5, -r - 4.0)
				draw_string_outline(font, tp, tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, 3, Color(0, 0, 0, 0.85))
				draw_string(font, tp, tag, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, FS_COL)
			# The current fire mission: guns to the target of their last rounds.
			var tid := int(snap[o + SF_TARGET])
			if all and tid >= 0 and now - snap[o + SF_FIRED_AT] <= 5.0 and main.disp_row.has(tid):
				var b: Vector2 = xf * main.disp[int(main.disp_row[tid])]
				var mc := FS_COL
				mc.a = 0.9
				draw_line(a, b, mc, w + 1.0, true)
				var dir := (b - a).normalized()
				var nrm := Vector2(-dir.y, dir.x)
				var tip := b - dir * maxf(8.0, counter_size() * 0.5)
				draw_colored_polygon(PackedVector2Array([tip, tip - dir * 10.0 + nrm * 5.0, tip - dir * 10.0 - nrm * 5.0]), mc)
			continue
		if not all:
			continue
		var status := int(snap[o + SF_FS_STATUS])
		if status == 2:
			var bat := int(snap[o + SF_FS_BATTERY])
			if bat >= 0 and main.disp_row.has(bat):
				var c2 := FS_COL
				c2.a = 0.7
				draw_line(a, xf * main.disp[int(main.disp_row[bat])], c2, w, true)
		elif status == 1:
			var via := int(snap[o + SF_FS_VIA])
			if via >= 0 and main.disp_row.has(via):
				draw_dashed_line(a, xf * main.disp[int(main.disp_row[via])], FS_COL, w, dash * 0.4)


## Highlight outlines around each own counter for the command overlays that are on: the direct
## command ring (commander's colour; amber dashed outside the zone, red dashed by courier or with
## no commander, a double white ring on the HQ) and, further out, the fire support ring (scarlet:
## thick on the guns, solid on observers on the net, dashed when relayed, grey dashed when cut
## off). Needs the counters' places, so it runs after _draw_units (not on the grouped theater map).
func _draw_net_outlines() -> void:
	if not (main.show_cmd or main.show_fs) or drawn.is_empty():
		return
	var snap: PackedFloat32Array = main.snapshot
	var stride: int = main.stride
	for dc in drawn:
		if int(dc["side"]) != 0 or not main.disp_row.has(int(dc["id"])):
			continue
		var row: int = main.disp_row[int(dc["id"])]
		var o := row * stride
		if (int(snap[o + 7]) & 2) != 0:
			continue
		var rect: Rect2 = dc["rect"]
		var s := rect.size.x
		var c := rect.get_center()
		var rad := maxf(1.5, s * 0.11)
		var lw := maxf(1.5, s * 0.06)
		if main.show_cmd:
			var ring := _rrect(c, s * 1.38, rad * 1.6)
			var boss := int(snap[o + SF_CMD_BOSS])
			var col := HQ_INK
			if boss >= 0 and main.disp_row.has(boss):
				col = _commander_color(o, int(main.disp_row[boss]))
			var link := int(snap[o + SF_CMD_LINK])
			if link < 0:
				_outline(ring, HQ_INK, lw * 1.3)
				_outline(_rrect(c, s * 1.52, rad * 1.8), HQ_INK, lw * 0.6)
			elif link == 0:
				_dashed_poly(ring, CMD_CUT, lw, maxf(2.0, s * 0.06))
				_net_tag(rect, true, "NO HQ", CMD_CUT)
			elif link == 1:
				_outline(ring, col, lw)
			elif link == 2:
				_dashed_poly(ring, CMD_OUT, lw, maxf(3.0, s * 0.14))
				_net_tag(rect, true, "×1.5", CMD_OUT)
			else:
				_dashed_poly(ring, CMD_CUT, lw, maxf(2.0, s * 0.07))
				_net_tag(rect, true, "COURIER", CMD_CUT)
		if main.show_fs:
			var role := int(snap[o + SF_FS_ROLE])
			if role < 0:
				continue
			var fr := _rrect(c, s * (1.66 if main.show_cmd else 1.38), rad * 2.0)
			var status := int(snap[o + SF_FS_STATUS])
			if role == 0:
				_outline(fr, FS_COL, lw * 1.6)
				_net_tag(rect, false, "GUNS", FS_COL)
			elif status == 2:
				_outline(fr, FS_COL, lw)
				_net_tag(rect, false, "FO", FS_COL)
			elif status == 1:
				_dashed_poly(fr, FS_COL, lw * (1.0 if role == 1 else 0.7), maxf(3.0, s * 0.12))
			else:
				_dashed_poly(fr, FS_CUT, lw * 0.8, maxf(2.0, s * 0.06))
				_net_tag(rect, false, "NO FS", FS_CUT)


## A small tag beside a counter: command tags top right, fire support tags bottom right.
func _net_tag(rect: Rect2, top: bool, text: String, col: Color) -> void:
	if rect.size.x < 28.0:
		return
	var p := Vector2(rect.end.x + rect.size.x * 0.3, rect.position.y + 4.0 if top else rect.end.y + 2.0)
	draw_string_outline(font, p, text, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, 3, Color(0, 0, 0, 0.85))
	draw_string(font, p, text, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, col)


## A closed polygon drawn as a dashed line that runs on round the corners.
func _dashed_poly(pts: PackedVector2Array, col: Color, w: float, dash: float) -> void:
	var on := true
	var left := dash
	var n := pts.size()
	for k in n:
		var a: Vector2 = pts[k]
		var b: Vector2 = pts[(k + 1) % n]
		var seg := a.distance_to(b)
		if seg < 0.001:
			continue
		var t := 0.0
		while t < seg:
			var step := minf(left, seg - t)
			if on:
				draw_line(a.lerp(b, t / seg), a.lerp(b, (t + step) / seg), col, w, true)
			t += step
			left -= step
			if left <= 0.0001:
				on = not on
				left = dash


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
