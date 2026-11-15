class_name ProfileChart
extends Control
## Elevation profile (MRK-26): the ground and cover the rules use, hex by hex, with the sight
## line from the start to the end for a straight line (green while clear, red past the hex
## that blocks it). Data: MurksWorld.get_profile.

const BG := Color(0.05, 0.06, 0.05, 0.6)
const GROUND := Color(0.58, 0.52, 0.38)
const GROUND_EDGE := Color(0.86, 0.80, 0.62)
# Cover drawn on top of the ground, by murks::Terrain (0 water .. 5 urban).
const COVER_COL := [Color(0.3, 0.5, 0.8), Color(0.45, 0.6, 0.3), Color(0.5, 0.62, 0.3), Color(0.2, 0.42, 0.2),
	Color(0.4, 0.55, 0.45), Color(0.55, 0.52, 0.5)]
const TEXT := Color(0.85, 0.87, 0.8)
const MUTED := Color(0.6, 0.63, 0.56)

var data: Dictionary = {}
var font: Font


func _ready() -> void:
	font = ThemeDB.fallback_font
	custom_minimum_size = Vector2(420, 132)
	mouse_filter = Control.MOUSE_FILTER_PASS


func set_profile(d: Dictionary) -> void:
	data = d
	queue_redraw()


func _process(_delta: float) -> void:
	if visible and get_global_rect().has_point(get_global_mouse_position()):
		queue_redraw()


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	if data.is_empty():
		return
	var km: PackedFloat32Array = data["km"]
	var ground: PackedFloat32Array = data["ground"]
	var cover: PackedFloat32Array = data["cover"]
	var terr: PackedInt32Array = data["terrain"]
	var n := km.size()
	if n < 2:
		return
	var total := maxf(0.001, km[n - 1])
	var lo := INF
	var hi := -INF
	for i in n:
		lo = minf(lo, ground[i])
		hi = maxf(hi, ground[i] + cover[i])
	var blk := int(data.get("block", -3))
	var eye := float(data.get("eye", 0.0))
	var tgt := float(data.get("target", 0.0))
	if blk != -3:
		hi = maxf(hi, ground[0] + eye)
	# At least 40 m of height range, so flat ground stays flat on the chart.
	var span := maxf(40.0, hi - lo)
	lo = lo - span * 0.08
	hi = lo + span * 1.2
	var pad_l := 40.0
	var pad_b := 16.0
	var w := size.x - pad_l - 6.0
	var h := size.y - pad_b - 6.0
	var to_px := func(x_km: float, z: float) -> Vector2:
		return Vector2(pad_l + x_km / total * w, 6.0 + (1.0 - (z - lo) / (hi - lo)) * h)
	# Each hex as a column half-way to its neighbours: ground, then its cover on top.
	for i in n:
		var x0 := km[0] if i == 0 else (km[i - 1] + km[i]) * 0.5
		var x1 := km[n - 1] if i == n - 1 else (km[i] + km[i + 1]) * 0.5
		var top: Vector2 = to_px.call(x0, ground[i])
		var right: Vector2 = to_px.call(x1, lo)
		draw_rect(Rect2(top, right - top), GROUND)
		if cover[i] > 0.0:
			var ct: Vector2 = to_px.call(x0, ground[i] + cover[i])
			draw_rect(Rect2(ct, Vector2(right.x - ct.x, top.y - ct.y)), COVER_COL[clampi(terr[i], 0, 5)])
	var line := PackedVector2Array()
	for i in n:
		line.append(to_px.call(km[i], ground[i]))
	draw_polyline(line, GROUND_EDGE, 1.5, true)
	# The sight line: eye above the start to the target above the end.
	if blk != -3:
		var a: Vector2 = to_px.call(0.0, ground[0] + eye)
		var b: Vector2 = to_px.call(total, ground[n - 1] + tgt)
		if blk == -1:
			draw_line(a, b, Color(0.45, 0.95, 0.5), 2.0, true)
		elif blk >= 0 and blk < n:
			var t := km[blk] / total
			var m := a.lerp(b, t)
			draw_line(a, m, Color(0.45, 0.95, 0.5), 2.0, true)
			draw_dashed_line(m, b, Color(1.0, 0.35, 0.3), 2.0, 5.0, true)
			draw_line(m + Vector2(-5, -5), m + Vector2(5, 5), Color(1.0, 0.35, 0.3), 2.5, true)
			draw_line(m + Vector2(-5, 5), m + Vector2(5, -5), Color(1.0, 0.35, 0.3), 2.5, true)
		draw_circle(a, 3.0, Color(1.0, 0.82, 0.3))
	# Axes: heights at the left, kilometres along the bottom.
	var fs := 11
	for z in [lo + span * 0.1, hi - span * 0.1]:
		var p: Vector2 = to_px.call(0.0, z)
		draw_string(font, Vector2(2, p.y + 4), "%d m" % roundi(z), HORIZONTAL_ALIGNMENT_LEFT, -1, fs, MUTED)
		draw_line(Vector2(pad_l - 3, p.y), Vector2(pad_l, p.y), MUTED, 1.0)
	var step := 0.5
	for s in [0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0]:
		step = s
		if total / s <= 8.0:
			break
	var k := 0.0
	while k <= total + 0.0001:
		var p: Vector2 = to_px.call(k, lo)
		draw_line(Vector2(p.x, p.y), Vector2(p.x, p.y + 3), MUTED, 1.0)
		draw_string(font, Vector2(p.x - 8, size.y - 3), ("%d" % roundi(k)) if step >= 1.0 else ("%.1f" % k), HORIZONTAL_ALIGNMENT_LEFT, -1, fs, MUTED)
		k += step
	draw_string(font, Vector2(4, size.y - 3), "km", HORIZONTAL_ALIGNMENT_LEFT, -1, fs, MUTED)
	# Under the mouse: distance, height and cover there.
	var mp := get_local_mouse_position()
	if Rect2(Vector2(pad_l, 0), Vector2(w, size.y)).has_point(mp):
		var x_km := (mp.x - pad_l) / w * total
		var best := 0
		for i in n:
			if absf(km[i] - x_km) < absf(km[best] - x_km):
				best = i
		var p: Vector2 = to_px.call(km[best], ground[best])
		draw_line(Vector2(p.x, 6), Vector2(p.x, 6 + h), Color(1, 1, 1, 0.35), 1.0)
		var t := "%.1f km · %d m" % [km[best], roundi(ground[best])]
		if cover[best] > 0.0:
			t += " + %d m cover" % roundi(cover[best])
		var tw := font.get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		var tx := clampf(p.x + 6, pad_l, size.x - tw - 4)
		draw_string_outline(font, Vector2(tx, 18), t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, 4, Color(0, 0, 0, 0.9))
		draw_string(font, Vector2(tx, 18), t, HORIZONTAL_ALIGNMENT_LEFT, -1, fs, TEXT)
