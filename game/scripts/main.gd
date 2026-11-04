extends Node2D
## Murks 1989 proof of concept: one continuous 500 m hex map, one clock.
## All simulation lives in the C++ core (MurksWorld); this script handles the
## camera, input, time controls and wiring the views together.

const SEED := 1989
const CHUNKS := Vector2i(24, 22)          # 1536 x 1408 hexes, ~770 x 610 km
const HOSTILE_GROUPS := 24                # patrolling bands; raise to stress-test
const MAP_FILE := "res://data/poland_1989.murkz"  # baked real map; procedural map if missing
const SPEEDS := [0.0, 1.0, 5.0, 30.0, 120.0, -1.0]  # game minutes per real second, -1 = max
const CONTACT_CAP := 2                    # max speed index while units are in contact
const SAVE_FILE := "user://quicksave.murks-save"
const ZOOM_MIN := 0.0012
const ZOOM_MAX := 0.25
const SLOT_OFFSETS := [Vector2i(0, 0), Vector2i(2, 0), Vector2i(-2, 0), Vector2i(1, 2), Vector2i(-1, 2),
	Vector2i(1, -2), Vector2i(-1, -2), Vector2i(4, 0), Vector2i(-4, 0), Vector2i(0, 4), Vector2i(0, -4)]

var world: MurksWorld
var camera: Camera2D
var map_view: MapView
var overlay: Overlay
var hud: Hud
var night_tint: CanvasModulate

var ready_to_play := false
var speed_index := 0
var last_speed := 1
var tick_acc := 0.0
var auto_pause := true
var wego := false
var next_pulse := -1
var show_los := false
var show_zones := false
var group_companies := true
var move_mode := 0                 # what right-click orders: 0 hasty, 1 road march, 2 deliberate, 3 assault
const MODE_NAMES := ["Move", "Road march", "Deliberate move", "Assault"]        # theater map: merge each company's nearby units into one counter
var formation_names := {}          # formation id -> name (own companies)
var selected: Array[int] = []
var snapshot := PackedFloat32Array()
var stride := 10
var los_hexes := PackedVector2Array()
var los_for := -1
var los_time := -1
var pings: Array = []
# Counter display (ported from the Forward Defense engine): counters glide between hex
# centres when the simulation moves them, then sit in their hex; fire and losses show as
# short tracers, shell bursts and explosions; destroyed units leave a wreck mark.
const GLIDE_MS := 260.0
var disp := PackedVector2Array()   # display position per snapshot row (world units)
var disp_row := {}                 # unit id -> snapshot row
var glide := {}                    # unit id -> [from, to, start msec]
var fx: Array = []                 # {k: "shot"/"arty"/"incoming"/"boom", a, b, t0, side, puffs}
var wrecks: Array = []             # {pos, side}
var fire_seen := {}                # shooter id -> last shot minute already shown
var targeted_seen := {}            # own unit id -> last "fired at" minute already shown
var short_names := {}              # own unit id -> short counter label
var groups_cache: Array = []       # own teams: {id, name, color, leader, units, shape, spacing, zone_hexes}
var show_cmd := false              # command zones and team links for all teams, not just the selection
const SF_GROUP := 20
const SF_ROLE := 21
var event_log: Array = []
var dragging := false
var drag_moved := false
var drag_start := Vector2.ZERO
var hud_timer := 0.0
var script_ms := 0.0               # smoothed GDScript cost per frame (incl. clock advance)
var last_contact_pause := -100000  # game minute of the last contact auto-pause
var gen_info := {}
var autotest_dir := ""
var hostile_groups := HOSTILE_GROUPS
var force_procedural := false
var outcome_shown := 0             # outcome already announced with the end screen (0 none)


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--autotest="):
			autotest_dir = a.substr(11)
		elif a.begins_with("--groups="):
			hostile_groups = a.substr(9).to_int()
		elif a == "--procedural":
			force_procedural = true

	camera = Camera2D.new()
	add_child(camera)
	camera.make_current()
	night_tint = CanvasModulate.new()
	night_tint.color = Color.WHITE
	add_child(night_tint)
	map_view = MapView.new()
	add_child(map_view)

	var overlay_layer := CanvasLayer.new()
	overlay_layer.layer = 1
	add_child(overlay_layer)
	overlay = Overlay.new()
	overlay.main = self
	overlay_layer.add_child(overlay)

	var hud_layer := CanvasLayer.new()
	hud_layer.layer = 2
	add_child(hud_layer)
	hud = Hud.new()
	hud.main = self
	hud_layer.add_child(hud)
	hud.speed_pressed.connect(_on_speed_button)
	hud.toggle_pressed.connect(_on_toggle)
	hud.hold_pressed.connect(_order_hold)
	hud.command.connect(_on_command)
	hud.end_choice.connect(_on_end_choice)
	hud.event_clicked.connect(func(p: Vector2) -> void: camera.position = p)

	# Let the loading text draw before the heavy work starts.
	await get_tree().process_frame
	await get_tree().process_frame
	if not ClassDB.class_exists("MurksWorld"):
		hud.loading.text = "The C++ extension did not load.\nBuild it (see README) or check game/bin/ for the library."
		return
	_start_world()


func _start_world() -> void:
	var t0 := Time.get_ticks_msec()
	world = MurksWorld.new()
	gen_info = {}
	if not force_procedural and FileAccess.file_exists(MAP_FILE):
		gen_info = _load_baked_map(MAP_FILE)
	if not gen_info.get("ok", false):
		gen_info = world.generate(SEED, CHUNKS.x, CHUNKS.y)
	world.setup_scenario(hostile_groups)
	stride = world.get_unit_stride()
	map_view.setup(world)
	overlay.set_towns(world.get_settlements(0))
	var info: Dictionary = world.get_map_info()
	print("Murks: map '%s' %d x %d hexes (%.1f MB), %d towns; startup %d ms" % [info["name"], int(info["cols"]),
		int(info["rows"]), float(gen_info["memory_mb"]), int(info["settlements"]), Time.get_ticks_msec() - t0])

	var hq: Dictionary = world.get_unit_info(0)
	camera.position = world.hex_to_world(hq["hex"])
	camera.zoom = Vector2.ONE * 0.045
	selected = [0]
	hud.loading.visible = false
	hud.set_toggle("Grid", map_view.show_grid)
	hud.set_toggle("Auto-pause", auto_pause)
	hud.set_toggle("Group", group_companies)
	set_speed(0)
	var obj: Dictionary = world.get_objectives()
	hud.show_banner("Company Murks is billeted at %s. The %s is coming for you from the east.\nBreak it before it breaks you, and guard the HQ: without it orders crawl and supplies slow to half. Select a unit and right-click to move; Space starts the clock; F1 shows all controls." % [
		obj.get("home", _home_name()), obj.get("enemy_name", "enemy")], 12.0)
	_poll_events()
	ready_to_play = true
	if autotest_dir != "":
		_run_autotest()


## Baked maps are a MURKSMAP file compressed with zlib: "MURKSZ01", u64 raw size, data.
func _load_baked_map(path: String) -> Dictionary:
	var data := FileAccess.get_file_as_bytes(path)
	if data.size() < 16 or data.slice(0, 8).get_string_from_ascii() != "MURKSZ01":
		push_warning("Not a Murks map: " + path)
		return {}
	var raw_size := data.decode_u64(8)
	var raw := data.slice(16).decompress(raw_size, FileAccess.COMPRESSION_DEFLATE)
	if raw.size() != raw_size:
		push_warning("Could not decompress " + path)
		return {}
	var result: Dictionary = world.load_map_bytes(raw)
	if not result.get("ok", false):
		push_warning("Invalid map data in " + path)
	return result


func _home_name() -> String:
	var info: Dictionary = world.get_unit_info(0)
	return String(info.get("location", "")).trim_prefix("in ")


# --- Main loop -------------------------------------------------------------------------

func _process(delta: float) -> void:
	if not ready_to_play:
		return
	var _t_start := Time.get_ticks_usec()
	_keyboard_pan(delta)

	var eff := effective_speed()
	if eff > 0:
		var rate: float = SPEEDS[eff]
		if rate < 0:
			# Max speed: run in short slices so the contact cap takes over as soon as fighting starts.
			var t_end := Time.get_ticks_usec() + 14000
			while Time.get_ticks_usec() < t_end:
				var done := world.advance(30, (t_end - Time.get_ticks_usec()) / 1000.0)
				if done < 30 or world.player_in_contact():
					break
			tick_acc = 0.0
		else:
			tick_acc += delta * rate
			var n := int(tick_acc)
			if n > 0:
				tick_acc -= n
				var done := world.advance(n, 25.0)
				if done < n:
					tick_acc = 0.0
		_check_wego()
	_poll_events()

	_refresh_snapshot()
	groups_cache = world.get_groups()
	_detect_fire_effects()
	_prune_selection()
	_update_los()
	night_tint.color = Color(0.62, 0.66, 0.82) if world.is_night() else Color.WHITE
	map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
	overlay.queue_redraw()

	hud_timer -= delta
	if hud_timer <= 0.0:
		hud_timer = 0.1
		var stats: Dictionary = world.get_stats()
		hud.refresh(world.get_time_string(), world.is_night(), stats, Engine.get_frames_per_second(),
			"script %.1f ms · chunks %d" % [script_ms, map_view.chunks.size()])
		hud.set_speed(speed_index, eff != speed_index)
		_refresh_selection_panel()
		_check_outcome()
	script_ms = script_ms * 0.9 + 0.1 * ((Time.get_ticks_usec() - _t_start) / 1000.0 + overlay.draw_ms)


func _refresh_selection_panel() -> void:
	if selected.is_empty():
		hud.show_unit({}, 0)
		return
	var first: Dictionary = world.get_unit_info(selected[0])
	_update_orders_state()
	if selected.size() == 1 or int(first.get("side", 1)) != 0:
		hud.show_unit(first, selected.size())
		return
	var infos: Array = []
	for id in selected:
		var info: Dictionary = world.get_unit_info(id)
		if not info.is_empty() and info["alive"] and int(info["side"]) == 0:
			infos.append(info)
	# Is the selection exactly one whole team?
	var tg := int(first.get("group_id", -1))
	if tg >= 0:
		var team := _team(tg)
		if not team.is_empty() and (team["units"] as PackedInt32Array).size() == infos.size():
			var all_in := true
			for info in infos:
				if int(info.get("group_id", -1)) != tg:
					all_in = false
			if all_in:
				var ordered: Array = []
				for info in infos:
					if int(info["id"]) == int(team["leader"]):
						ordered.push_front(info)
					else:
						ordered.append(info)
				hud.show_group(ordered, String(team["name"]))
				return
	# Is the selection exactly one whole company?
	var fid := int(first["formation_id"])
	var whole := true
	for info in infos:
		if int(info["formation_id"]) != fid:
			whole = false
	if whole:
		var members := 0
		for i in snapshot.size() / stride:
			if int(snapshot[i * stride + 10]) == fid:
				members += 1
		whole = members == infos.size()
	hud.show_group(infos, String(first["formation"]) if whole else "")


## Shows the armed move mode and the selection's fire / break-contact settings in the panel.
func _update_orders_state() -> void:
	var roe := -2
	var brk := -2
	var pending := false
	var lo := 1 << 30
	var hi := 0
	for id in _own_selected():
		var info: Dictionary = world.get_unit_info(id)
		var sp: bool = info.get("stance_pending", false)
		pending = pending or sp
		var r := int(info["pending_roe"]) if sp else int(info["roe"])
		var b := int(info["pending_break_contact"]) if sp else int(info["break_contact"])
		roe = r if roe == -2 or roe == r else -1
		brk = b if brk == -2 or brk == b else -1
		lo = mini(lo, int(info["command_delay"]))
		hi = maxi(hi, int(info["command_delay"]))
	if roe == -2:
		hud.set_team_state(false, false, -1, -1)
		return
	var teams := _selected_teams()
	var shape := -1
	var spacing := -1
	for gid in teams:
		var tm := _team(gid)
		if tm.is_empty():
			continue
		shape = int(tm["shape"]) if shape == -1 or shape == int(tm["shape"]) else -2
		spacing = int(tm["spacing"]) if spacing == -1 or spacing == int(tm["spacing"]) else -2
	hud.set_team_state(not teams.is_empty(), _own_selected().size() >= 2, shape, spacing)
	var so := -2
	for id in _own_selected():
		var inf: Dictionary = world.get_unit_info(id)
		var v := int(inf.get("pending_standoff", inf.get("standoff_setting", 255))) if inf.get("standoff_pending", false) else int(inf.get("standoff_setting", 255))
		so = v if so == -2 or so == v else -1
	hud.set_standoff_state(so, world.get_default_standoff())
	hud.set_orders_state(move_mode, roe, brk, pending, ("%d min" % lo) if lo == hi else ("%d-%d min" % [lo, hi]))


func effective_speed() -> int:
	if world == null:
		return 0
	if speed_index > CONTACT_CAP and world.player_in_contact():
		return CONTACT_CAP
	return speed_index


func _on_speed_button(index: int) -> void:
	if index == 0 and speed_index == 0:
		set_speed(last_speed) # the Pause button doubles as resume
	else:
		set_speed(index)


func set_speed(index: int) -> void:
	if world == null:
		return
	speed_index = clampi(index, 0, SPEEDS.size() - 1)
	if speed_index > 0:
		last_speed = speed_index
		if wego:
			next_pulse = world.get_time_minutes() + maxi(1, world.estimate_command_delay(0))
	tick_acc = 0.0
	hud.set_speed(speed_index, effective_speed() != speed_index)


func pause(reason: String) -> void:
	if speed_index == 0:
		return
	speed_index = 0
	hud.set_speed(0, false)
	if reason != "":
		hud.show_banner("Paused: " + reason)


func _check_wego() -> void:
	if not wego or speed_index == 0:
		return
	var now := world.get_time_minutes()
	if next_pulse < 0:
		next_pulse = now + maxi(1, world.estimate_command_delay(0))
	elif now >= next_pulse:
		next_pulse = now + maxi(1, world.estimate_command_delay(0))
		pause("WeGo pulse — the HQ can issue new orders.")


func _poll_events() -> void:
	var events: Array = world.poll_events()
	if events.is_empty():
		return
	var pause_reason := ""
	for e in events:
		# Order acknowledgements are visible on the map already; keep the log for news.
		if e["kind"] != "order":
			event_log.push_front(e)
		if e["kind"] == "destroyed" and e.has("pos"):
			var dead: Dictionary = world.get_unit_info(int(e["unit"]))
			var dside := int(dead.get("side", 1))
			wrecks.append({"pos": e["pos"], "side": dside})
			fx.append({"k": "boom", "a": e["pos"], "b": e["pos"], "t0": Time.get_ticks_msec(), "side": dside})
			if wrecks.size() > 300:
				wrecks.pop_front()
		if e.has("pos"):
			var col := Color(1, 0.5, 0.3)
			if e["kind"] == "order" or e["kind"] == "arrived": col = Color(0.6, 0.85, 1.0)
			pings.append({"pos": e["pos"], "t": Time.get_ticks_msec(), "color": col})
		if e["pause"] and pause_reason == "":
			# Contacts arrive in bursts during a fight: pause for the first, not every one.
			var is_contact: bool = e["kind"] == "contact" or e["kind"] == "under_fire"
			if not is_contact or int(e["time"]) - last_contact_pause >= 20:
				pause_reason = e["text"]
				if is_contact:
					last_contact_pause = int(e["time"])
	while event_log.size() > 9:
		event_log.pop_back()
	hud.set_events(event_log)
	if pings.size() > 40:
		pings = pings.slice(pings.size() - 40)
	if auto_pause and pause_reason != "":
		pause(pause_reason)


func _prune_selection() -> void:
	if selected.is_empty():
		return
	var alive := {}
	for i in snapshot.size() / stride:
		alive[int(snapshot[i * stride])] = true
	var keep: Array[int] = []
	for id in selected:
		if alive.has(id):
			keep.append(id)
	selected = keep


func _update_los() -> void:
	if not show_los or selected.is_empty():
		los_hexes = PackedVector2Array()
		los_for = -1
		return
	var id: int = selected[0]
	var now := world.get_time_minutes()
	if id != los_for or now - los_time >= 2:
		los_hexes = world.get_visible_hexes(id)
		los_for = id
		los_time = now


func unit_position(id: int) -> Vector2:
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride]) == id:
			return Vector2(snapshot[i * stride + 3], snapshot[i * stride + 4])
	return Vector2.ZERO


# --- Input ---------------------------------------------------------------------------

func _unhandled_input(event: InputEvent) -> void:
	if not ready_to_play:
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			_zoom_at(mb.position, 1.15)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			_zoom_at(mb.position, 1.0 / 1.15)
		elif mb.button_index == MOUSE_BUTTON_LEFT and mb.pressed and mb.double_click:
			# Double-click a unit: select its team, or its whole company if it has none.
			_click_select(mb.position, false)
			if selected.size() == 1 and not _select_team_of(selected[0]):
				_select_company_of(selected[0])
			dragging = false
		elif mb.button_index == MOUSE_BUTTON_LEFT or mb.button_index == MOUSE_BUTTON_MIDDLE:
			if mb.pressed:
				dragging = true
				drag_moved = false
				drag_start = mb.position
			else:
				if mb.button_index == MOUSE_BUTTON_LEFT and not drag_moved:
					_click_select(mb.position, mb.shift_pressed)
				dragging = false
		elif mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
			_order_move_to(get_global_mouse_position())
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if dragging:
			if (mm.position - drag_start).length() > 4.0:
				drag_moved = true
			if drag_moved:
				camera.position -= mm.relative / camera.zoom.x
		_update_status()
	elif event is InputEventKey and event.pressed and not event.echo:
		_on_key((event as InputEventKey).keycode)


func _on_key(key: Key) -> void:
	match key:
		KEY_SPACE: set_speed(0 if speed_index > 0 else last_speed)
		KEY_1: set_speed(1)
		KEY_2: set_speed(2)
		KEY_3: set_speed(3)
		KEY_4: set_speed(4)
		KEY_5: set_speed(5)
		KEY_PERIOD:
			if speed_index == 0:
				world.advance(15, 1000.0)
		KEY_H: _order_hold()
		KEY_G: _on_toggle("Grid")
		KEY_V: _on_toggle("Sight")
		KEY_Z: _on_toggle("Zones")
		KEY_P: _on_toggle("WeGo")
		KEY_U: _on_toggle("Auto-pause")
		KEY_TAB: _select_next()
		KEY_K:
			if not selected.is_empty():
				_select_company_of(selected[0])
		KEY_O: _on_toggle("Group")
		KEY_T: _on_toggle("Command")
		KEY_J:
			_on_command("team_disband" if Input.is_key_pressed(KEY_SHIFT) else "team_form")
		KEY_L: _on_command("team_select")
		KEY_F: _on_command("mode_0")
		KEY_Q: _on_command("mode_1")
		KEY_E: _on_command("mode_2")
		KEY_R: _on_command("mode_3")
		KEY_B: _on_command("fall_back")
		KEY_Y: _on_command("regroup")
		KEY_N: _on_command("rest")
		KEY_X: _cycle_roe()
		KEY_C, KEY_HOME:
			if not selected.is_empty():
				camera.position = unit_position(selected[0])
		KEY_M:
			camera.position = world.get_world_size() * 0.5
			camera.zoom = Vector2.ONE * clampf(get_viewport_rect().size.x / world.get_world_size().x * 0.95, ZOOM_MIN, ZOOM_MAX)
		KEY_F1: hud.help_panel.visible = not hud.help_panel.visible
		KEY_F2: hud.credits_panel.visible = not hud.credits_panel.visible
		KEY_F5: quick_save()
		KEY_F9: quick_load()


func _on_toggle(name: String) -> void:
	match name:
		"Grid":
			map_view.show_grid = not map_view.show_grid
			hud.set_toggle(name, map_view.show_grid)
		"Sight":
			show_los = not show_los
			hud.set_toggle(name, show_los)
		"Zones":
			show_zones = not show_zones
			hud.set_toggle(name, show_zones)
			if show_zones:
				hud.show_toast("Red ring: within 10 km of an enemy, full combat model. Amber: 10-40 km, spotting only. No ring: cold, movement only.", 7.0)
		"WeGo":
			wego = not wego
			next_pulse = -1
			hud.set_toggle(name, wego)
			hud.show_toast("WeGo pulse on: the clock pauses each time the HQ can issue orders." if wego else "WeGo pulse off.")
		"Group":
			group_companies = not group_companies
			hud.set_toggle(name, group_companies)
			hud.show_toast("Theater map groups each company's nearby units into one counter." if group_companies else "Company grouping off: every platoon is shown.")
		"Auto-pause":
			auto_pause = not auto_pause
			hud.set_toggle(name, auto_pause)
		"Command":
			show_cmd = not show_cmd
			hud.set_toggle(name, show_cmd)
			hud.show_toast("Command zones: each HQ and team leader's zone of effective command, with lines to the units it commands. Units outside their zone get orders x1.5 slower." if show_cmd else "Command zones shown for the selection only.", 6.0)


func _keyboard_pan(delta: float) -> void:
	var dir := Vector2.ZERO
	if Input.is_key_pressed(KEY_W) or Input.is_key_pressed(KEY_UP): dir.y -= 1
	if Input.is_key_pressed(KEY_S) or Input.is_key_pressed(KEY_DOWN): dir.y += 1
	if Input.is_key_pressed(KEY_A) or Input.is_key_pressed(KEY_LEFT): dir.x -= 1
	if Input.is_key_pressed(KEY_D) or Input.is_key_pressed(KEY_RIGHT): dir.x += 1
	if dir != Vector2.ZERO:
		camera.position += dir.normalized() * 700.0 * delta / camera.zoom.x


func _zoom_at(screen_pos: Vector2, factor: float) -> void:
	var before := _screen_to_world(screen_pos)
	var z := clampf(camera.zoom.x * factor, ZOOM_MIN, ZOOM_MAX)
	camera.zoom = Vector2(z, z)
	camera.force_update_scroll()
	var after := _screen_to_world(screen_pos)
	camera.position += before - after


func _screen_to_world(screen_pos: Vector2) -> Vector2:
	var vp := get_viewport_rect().size
	return camera.get_screen_center_position() + (screen_pos - vp * 0.5) / camera.zoom.x


func _click_select(screen_pos: Vector2, add: bool) -> void:
	# Theater map: a click on a company counter selects the whole group.
	var best_group: Dictionary = {}
	var best_gd := 26.0
	for g in overlay.groups:
		var gd := (g["pos"] as Vector2).distance_to(screen_pos)
		if gd < best_gd and (g["ids"] as Array).size() > 1:
			best_gd = gd
			best_group = g
	if not best_group.is_empty():
		var ids: Array[int] = []
		ids.assign(best_group["ids"])
		if add:
			for id in ids:
				if not selected.has(id):
					selected.append(id)
		else:
			selected = ids
		return
	# A click on a drawn counter picks that counter (topmost first), so units fanned out
	# in a shared hex can each be selected.
	for i in range(overlay.drawn.size() - 1, -1, -1):
		var dc: Dictionary = overlay.drawn[i]
		if (dc["rect"] as Rect2).grow(3.0).has_point(screen_pos):
			var hit_id := int(dc["id"])
			if add:
				if selected.has(hit_id):
					selected.erase(hit_id)
				else:
					selected.append(hit_id)
			else:
				selected = [hit_id]
			return
	var xf := get_viewport().get_canvas_transform()
	var best := -1
	var best_d := 22.0
	var best_side := 1
	for i in snapshot.size() / stride:
		var o := i * stride
		var s: Vector2 = xf * Vector2(snapshot[o + 3], snapshot[o + 4])
		var d := s.distance_to(screen_pos)
		var side := int(snapshot[o + 1])
		# Prefer own units when counters overlap.
		if d < best_d or (d < 22.0 and side == 0 and best_side != 0):
			best = int(snapshot[o])
			best_d = d
			best_side = side
	if best < 0:
		if not add:
			selected.clear()
		return
	if add:
		if selected.has(best):
			selected.erase(best)
		else:
			selected.append(best)
	else:
		selected = [best]


func _select_next() -> void:
	var own: Array[int] = []
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride + 1]) == 0:
			own.append(int(snapshot[i * stride]))
	if own.is_empty():
		return
	var cur := own.find(selected[0]) if not selected.is_empty() else -1
	selected = [own[(cur + 1) % own.size()]]
	camera.position = unit_position(selected[0])


## Name of an own formation (company), cached.
func formation_name(fid: int, sample_unit: int) -> String:
	if not formation_names.has(fid):
		var info: Dictionary = world.get_unit_info(sample_unit)
		formation_names[fid] = String(info.get("formation", "Company"))
	return formation_names[fid]


## Selects every living unit of the company that `unit_id` belongs to.
func _select_company_of(unit_id: int) -> void:
	var fid := -1
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride]) == unit_id:
			fid = int(snapshot[i * stride + 10])
	if fid < 0:
		return # hostile or unknown
	var ids: Array[int] = []
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride + 10]) == fid:
			ids.append(int(snapshot[i * stride]))
	selected = ids
	hud.show_toast("%s selected: %d units. Right-click moves the company and keeps its layout." % [formation_name(fid, unit_id), ids.size()])


func _all_own_ids() -> Array[int]:
	var out: Array[int] = []
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride + 1]) == 0:
			out.append(int(snapshot[i * stride]))
	return out


func _own_selected() -> Array[int]:
	var out: Array[int] = []
	for id in selected:
		var info: Dictionary = world.get_unit_info(id)
		if not info.is_empty() and int(info["side"]) == 0 and info["alive"]:
			out.append(id)
	return out


func _order_move_to(world_pos: Vector2) -> void:
	var own := _own_selected()
	if own.is_empty():
		hud.show_toast("Select one of your units first (left click).")
		return
	var hex := world.world_to_hex(world_pos)
	var lo := 1 << 30
	var hi := 0
	var sent := 0
	# A team whose leader is selected moves as a team: the order goes to the leader and the
	# members follow in formation. Everyone else gets their own order.
	var lead_team := {} # leader id -> team id
	for gid in _selected_teams():
		var tm := _team(gid)
		if not tm.is_empty() and own.has(int(tm["leader"])):
			lead_team[int(tm["leader"])] = gid
	var movers: Array[int] = []
	var team_names: Array[String] = []
	for id in own:
		var g := _team_of_unit(id)
		if g >= 0 and lead_team.values().has(g) and not lead_team.has(id):
			continue # follows its leader
		movers.append(id)
	# Several movers keep their layout around the target (spread capped at 3 km),
	# so a company moves as a company instead of piling into one hex.
	var centroid := Vector2.ZERO
	for id in movers:
		centroid += unit_position(id)
	centroid /= maxi(1, movers.size())
	for k in movers.size():
		var id := movers[k]
		var target := hex
		if movers.size() > 1:
			# A road march closes up into a column; other moves keep a 3 km spread.
			var offset := (unit_position(id) - centroid).limit_length(1000.0 if move_mode == 1 else 3000.0)
			target = world.world_to_hex(world_pos + offset)
		var d := -1
		if lead_team.has(id):
			d = world.order_group_move(lead_team[id], target, move_mode)
			if d < 0:
				d = world.order_group_move(lead_team[id], hex, move_mode)
			if d >= 0:
				var tm := _team(lead_team[id])
				team_names.append("%s (%s, %d units)" % [tm["name"], String(tm["shape_name"]).to_lower(), (tm["units"] as PackedInt32Array).size()])
				sent += (tm["units"] as PackedInt32Array).size() - 1
		else:
			d = world.order_move(id, target, move_mode)
			if d < 0:
				d = world.order_move(id, hex + SLOT_OFFSETS[k % SLOT_OFFSETS.size()], move_mode)
			if d < 0:
				d = world.order_move(id, hex, move_mode)
		if d >= 0:
			sent += 1
			lo = mini(lo, d)
			hi = maxi(hi, d)
	if sent == 0:
		hud.show_toast("Can't move there: water or off the map.")
		return
	pings.append({"pos": world.hex_to_world(hex), "t": Time.get_ticks_msec(), "color": Color(0.6, 0.85, 1.0)})
	var when := ("%d min" % lo) if lo == hi else ("%d-%d min" % [lo, hi])
	var teams_txt := (" Moving in formation: %s." % ", ".join(team_names)) if not team_names.is_empty() else ""
	hud.show_toast("%s ordered for %d unit%s. Orders take effect in %s (command delay).%s" % [MODE_NAMES[move_mode], sent, "" if sent == 1 else "s", when, teams_txt])
	# A move mode applies to one order: the next right-click is a plain move again.
	move_mode = 0


## Orders from the command panel (and their keys).
func _on_command(name: String) -> void:
	if name.begins_with("mode_"):
		move_mode = name.substr(5).to_int()
		hud.show_toast("Right-click the map to order: %s." % MODE_NAMES[move_mode], 3.0)
	elif name == "hold":
		_order_hold()
	elif name == "fall_back" or name == "regroup" or name == "rest":
		_order_group(name)
	elif name.begins_with("roe_"):
		_set_stance(name.substr(4).to_int(), -1)
	elif name.begins_with("brk_"):
		_set_stance(-1, name.substr(4).to_int())
	elif name.begins_with("default_standoff_"):
		var v := name.substr(17).to_int()
		world.set_default_standoff(v)
		hud.show_toast("Company default stand-off: %s. Applies at once to every unit set to Default." % _standoff_name(v), 5.0)
	elif name.begins_with("standoff_"):
		var v := name.substr(9).to_int()
		var own := _own_selected()
		var lo := 1 << 30
		for id in own:
			lo = mini(lo, world.set_standoff(id, v))
		if not own.is_empty():
			hud.show_toast("Stand-off %s for %d unit%s, in effect after %d+ min (command delay)." % [_standoff_name(v), own.size(), "" if own.size() == 1 else "s", lo])
	elif name == "team_form":
		_form_team()
	elif name == "team_disband":
		var n := 0
		for gid in _selected_teams():
			if world.disband_group(gid):
				n += 1
		_refresh_snapshot()
		groups_cache = world.get_groups()
		hud.show_toast("%d team%s disbanded." % [n, "" if n == 1 else "s"] if n > 0 else "No team in the selection.")
	elif name == "team_select":
		if selected.is_empty() or not _select_team_of(selected[0]):
			hud.show_toast("That unit isn't in a team. Select units and press J to form one.")
	elif name.begins_with("shape_") or name.begins_with("spread_"):
		var teams := _selected_teams()
		if teams.is_empty():
			hud.show_toast("Select a team first (form one with J).")
		for gid in teams:
			if name.begins_with("shape_"):
				world.set_group_shape(gid, name.substr(6).to_int(), -1)
			else:
				world.set_group_shape(gid, -1, name.substr(7).to_int())
		groups_cache = world.get_groups()
		if not teams.is_empty():
			var tm := _team(teams[0])
			hud.show_toast("%s: %s, %.1f km spread. Members take up their places." % [tm["name"], tm["shape_name"], int(tm["spacing"]) * 0.5])
	_refresh_selection_panel()


func _standoff_name(v: int) -> String:
	match v:
		255: return "company default"
		0: return "stay and fight"
		254: return "at weapon range"
	return "%.1f km" % (v * 0.5)


# --- Teams ------------------------------------------------------------------------------

func _team(gid: int) -> Dictionary:
	for tm in groups_cache:
		if int(tm["id"]) == gid:
			return tm
	return {}


func _team_of_unit(id: int) -> int:
	if disp_row.has(id):
		return int(snapshot[int(disp_row[id]) * stride + SF_GROUP])
	return -1


func _selected_teams() -> Array[int]:
	var out: Array[int] = []
	for id in _own_selected():
		var g := _team_of_unit(id)
		if g >= 0 and not out.has(g):
			out.append(g)
	return out


func _select_team_of(unit_id: int) -> bool:
	var g := _team_of_unit(unit_id)
	var tm := _team(g)
	if tm.is_empty():
		return false
	var ids: Array[int] = []
	for id in (tm["units"] as PackedInt32Array):
		ids.append(id)
	selected = ids
	hud.show_toast("%s selected: %d units led by %s. Right-click moves the team in %s formation." % [
		tm["name"], ids.size(), world.get_unit_info(int(tm["leader"])).get("name", "?"), String(tm["shape_name"]).to_lower()])
	return true


## Lock the selected units into a team. The HQ leads if it is selected, otherwise the
## first unit selected (the one clicked first).
func _form_team() -> void:
	var own := _own_selected()
	if own.size() < 2:
		hud.show_toast("Select at least two of your units (Shift+click), then press J.")
		return
	var leader := own[0]
	for id in own:
		if disp_row.has(id) and int(snapshot[int(disp_row[id]) * stride + 2]) == 0:
			leader = id
	var ids := PackedInt32Array()
	for id in own:
		ids.append(id)
	var gid := world.create_group(ids, leader)
	if gid < 0:
		hud.show_toast("Can't form that team: all units must be alive and in the same company.")
		return
	_refresh_snapshot()
	groups_cache = world.get_groups()
	var tm := _team(gid)
	var li: Dictionary = world.get_unit_info(leader)
	hud.show_toast("%s formed: %s leads %d units and commands within %.1f km. Formation: keep layout (change it below)." % [
		tm["name"], li.get("name", "?"), own.size() - 1, int(tm["zone_hexes"]) * 0.5], 6.0)


func _order_group(kind: String) -> void:
	var own := _own_selected()
	if own.is_empty():
		hud.show_toast("Select your units first.")
		return
	var lo := 1 << 30
	var hi := 0
	for id in own:
		var d := -1
		match kind:
			"fall_back": d = world.order_fall_back(id)
			"regroup": d = world.order_regroup(id, false)
			"rest": d = world.order_regroup(id, true)
		if d >= 0:
			lo = mini(lo, d)
			hi = maxi(hi, d)
	var what: String = {"fall_back": "Fall back", "regroup": "Regroup on the HQ", "rest": "Rest & resupply at the HQ"}[kind]
	hud.show_toast("%s: %d unit%s, orders arrive in %s." % [what, own.size(), "" if own.size() == 1 else "s",
		("%d min" % lo) if lo == hi else ("%d-%d min" % [lo, hi])])


func _set_stance(roe: int, brk: int) -> void:
	var own := _own_selected()
	if own.is_empty():
		return
	for id in own:
		world.set_stance(id, roe, brk)
	if roe >= 0:
		hud.show_toast("Fire: %s for %d unit%s (after the command delay)." % [["at will", "return fire only", "hold fire"][roe], own.size(), "" if own.size() == 1 else "s"])
	else:
		hud.show_toast("Break contact at: %s for %d unit%s (after the command delay)." % [["heavy losses", "light losses", "never"][brk], own.size(), "" if own.size() == 1 else "s"])


func _cycle_roe() -> void:
	var own := _own_selected()
	if own.is_empty():
		return
	var info: Dictionary = world.get_unit_info(own[0])
	var cur := int(info["pending_roe"]) if info.get("stance_pending", false) else int(info["roe"])
	_set_stance((cur + 1) % 3, -1)


func _order_hold() -> void:
	var own := _own_selected()
	# A selected team leader halts its team, which forms up on it; members follow.
	var led := {}
	for gid in _selected_teams():
		var tm := _team(gid)
		if not tm.is_empty() and own.has(int(tm["leader"])):
			led[gid] = true
			world.order_group_hold(gid)
	for id in own:
		var g := _team_of_unit(id)
		if g >= 0 and led.has(g):
			continue
		world.order_hold(id)
	if not own.is_empty():
		hud.show_toast("Hold orders sent to %d unit%s." % [own.size(), "" if own.size() == 1 else "s"])


func _update_status() -> void:
	var hex := world.world_to_hex(get_global_mouse_position())
	var info: Dictionary = world.get_hex_info(hex)
	if info.is_empty():
		hud.set_status("")
		return
	var bits: Array[String] = [String(info["terrain"]), "%d m" % int(info["elevation"])]
	if info.get("major_road", false): bits.append("main road")
	elif info.get("road", false): bits.append("road")
	if info.get("rail", false): bits.append("railway")
	if info.get("bridge", false): bits.append("bridge")
	elif info.get("river", false): bits.append("river")
	if info.has("settlement"):
		bits.append("%s (pop. %s)" % [info["settlement"], hud._thousands(int(info["population"]))])
	else:
		bits.append(String(info["place"]))
	var ll: Vector2 = world.world_to_lonlat(get_global_mouse_position())
	var where := "Hex %d,%d" % [hex.x, hex.y]
	if not is_nan(ll.x):
		where += "  %.3f°%s %.3f°%s" % [absf(ll.y), "N" if ll.y >= 0 else "S", absf(ll.x), "E" if ll.x >= 0 else "W"]
	hud.set_status("%s   %s" % [where, "  ·  ".join(bits)])


# --- Counter display: glide, fire effects, labels ---------------------------------------

func _glide_at(g: Array, now: int) -> Vector2:
	var t := clampf((now - int(g[2])) / GLIDE_MS, 0.0, 1.0)
	t = t * t * (3.0 - 2.0 * t) # ease in and out
	return (g[0] as Vector2).lerp(g[1], t)


func _refresh_snapshot() -> void:
	snapshot = world.get_units_snapshot()
	_update_display_positions()


## The simulation moves a unit a whole hex at a time; the counter glides there in a
## quarter second and then sits in the centre of the hex it occupies.
func _update_display_positions() -> void:
	var n := snapshot.size() / stride
	disp.resize(n)
	disp_row.clear()
	var now := Time.get_ticks_msec()
	for i in n:
		var o := i * stride
		var id := int(snapshot[o])
		disp_row[id] = i
		var p := Vector2(snapshot[o + 3], snapshot[o + 4])
		if (int(snapshot[o + 7]) & 2) != 0: # last-seen marker: no movement to show
			disp[i] = p
			glide.erase(id)
			continue
		var g: Array = glide.get(id, [])
		if g.is_empty():
			g = [p, p, now - int(GLIDE_MS)]
			glide[id] = g
		elif g[1] != p:
			var cur := _glide_at(g, now)
			g = [p, p, now - int(GLIDE_MS)] if cur.distance_to(p) > 1600.0 else [cur, p, now]
			glide[id] = g
		disp[i] = _glide_at(g, now)


## Fire effects from the snapshot: a tracer from a visible shooter to its target, shell
## bursts for artillery, and incoming fire on own units from shooters the player can't see.
func _detect_fire_effects() -> void:
	var n := snapshot.size() / stride
	var now_ms := Time.get_ticks_msec()
	var now_min := world.get_time_minutes()
	var row := {}
	for i in n:
		row[int(snapshot[i * stride])] = i
	var shown_targets := {}
	for i in n:
		var o := i * stride
		var id := int(snapshot[o])
		var fired := int(snapshot[o + 14])
		if fired < -100000:
			continue # not visible to the player
		if not fire_seen.has(id):
			fire_seen[id] = fired # first sight: remember, don't replay old shots
			continue
		var prev: int = fire_seen[id]
		fire_seen[id] = fired
		if fired <= prev or now_min - fired > 3:
			continue
		var tid := int(snapshot[o + 15])
		if not row.has(tid):
			continue
		var j: int = row[tid]
		if (int(snapshot[j * stride + 7]) & 2) != 0:
			continue
		var kind := "arty" if int(snapshot[o + 2]) == 5 else "shot"
		fx.append({"k": kind, "a": disp[i], "b": disp[j], "t0": now_ms, "side": int(snapshot[o + 1]),
			"puffs": _puffs()})
		shown_targets[tid] = true
	for i in n:
		var o := i * stride
		if int(snapshot[o + 1]) != 0:
			continue
		var id := int(snapshot[o])
		var hit_at := int(snapshot[o + 16])
		if not targeted_seen.has(id):
			targeted_seen[id] = hit_at
			continue
		var prev: int = targeted_seen[id]
		targeted_seen[id] = hit_at
		if hit_at <= prev or now_min - hit_at > 3 or shown_targets.has(id):
			continue
		fx.append({"k": "incoming", "a": disp[i], "b": disp[i], "t0": now_ms, "side": 1, "puffs": _puffs()})
	while fx.size() > 160:
		fx.pop_front()


## Where a unit's counter is drawn right now (gliding between hexes, or its hex centre).
func display_position(id: int) -> Vector2:
	if disp_row.has(id) and int(disp_row[id]) < disp.size():
		return disp[disp_row[id]]
	return unit_position(id)


func _puffs() -> PackedVector2Array:
	var out := PackedVector2Array()
	for k in 6:
		out.append(Vector2(randf() - 0.5, randf() - 0.5))
	return out


## Short label for an own counter: "1Tk", "2Mc", "HQ", "Bty".
func short_name(id: int) -> String:
	if short_names.has(id):
		return short_names[id]
	var info: Dictionary = world.get_unit_info(id)
	var nm := String(info.get("name", ""))
	var abbr := {"HQ": "HQ", "Tank Plt": "Tk", "Mech Plt": "Mc", "Rifle Plt": "Rf", "Recon Sect": "Rc", "Arty Bty": "Bty"}
	var tn := String(info.get("type_name", ""))
	var s: String = abbr.get(tn, nm.substr(0, 3))
	if s != "HQ" and s != "Bty" and nm.length() > 0 and nm[0].is_valid_int():
		s = nm[0] + s
	short_names[id] = s
	return s


# --- Objectives, end of the game, quick save -------------------------------------------

func _check_outcome() -> void:
	var obj: Dictionary = world.get_objectives()
	hud.set_objectives(obj)
	var oc := int(obj["outcome"])
	if oc != 0 and oc != outcome_shown:
		outcome_shown = oc
		pause("")
		hud.show_end_screen(obj, FileAccess.file_exists(SAVE_FILE))


func _on_end_choice(choice: String) -> void:
	hud.end_panel.visible = false
	match choice:
		"restart": get_tree().reload_current_scene()
		"load": quick_load()
		"quit": get_tree().quit()


func quick_save() -> void:
	var bytes: PackedByteArray = world.save_state()
	var f := FileAccess.open(SAVE_FILE, FileAccess.WRITE)
	if bytes.is_empty() or f == null:
		hud.show_toast("Could not save the game.", 4.0)
		return
	f.store_buffer(bytes)
	f.close()
	hud.show_toast("Saved (%s). F9 loads it." % world.get_time_string(), 3.0)


func quick_load() -> void:
	if not FileAccess.file_exists(SAVE_FILE):
		hud.show_toast("No quick save yet. F5 saves.", 3.0)
		return
	var err: String = world.load_state(FileAccess.get_file_as_bytes(SAVE_FILE))
	if err != "":
		hud.show_toast("Could not load the save: " + err, 5.0)
		return
	speed_index = 0
	hud.set_speed(0, false)
	event_log.clear()
	hud.set_events(event_log)
	pings.clear()
	selected.clear()
	los_for = -1
	glide.clear()
	fx.clear()
	wrecks.clear()
	fire_seen.clear()
	targeted_seen.clear()
	outcome_shown = int(world.get_objectives()["outcome"])
	hud.end_panel.visible = false
	_refresh_snapshot()
	var hq := world.get_unit_info(0)
	if not hq.is_empty() and hq["alive"]:
		selected = [0]
		camera.position = world.hex_to_world(hq["hex"])
	hud.show_toast("Loaded the quick save (%s). Paused." % world.get_time_string(), 4.0)


# --- Automated check: scripted run that saves screenshots, then quits. ---------------------

func _run_autotest() -> void:
	print("Autotest: writing screenshots to ", autotest_dir)
	DirAccess.make_dir_recursive_absolute(autotest_dir)
	auto_pause = false
	_refresh_snapshot()
	var hq: Vector2 = world.hex_to_world(world.get_unit_info(0)["hex"])
	show_zones = false

	await _shot("01_company_close", hq, 0.07)

	# Teams: lock four live combat units under the first as leader, wedge at 1 km, march west.
	var pick: Array[int] = []
	for id in _all_own_ids():
		if int(snapshot[int(disp_row[id]) * stride + 2]) in [1, 2, 3] and pick.size() < 4:
			pick.append(id)
	if pick.size() >= 3:
		selected = pick
		_form_team()
		_on_command("shape_3")
		_on_command("spread_2")
		_on_command("default_standoff_4")
		var lead_pos := unit_position(pick[0])
		move_mode = 0
		_order_move_to(lead_pos + Vector2(-9000, 0))
		show_cmd = true
		for k in 90:
			world.advance(1, 200.0)
		_select_team_of(pick[0])
		await _shot("14_team_wedge", unit_position(pick[0]) + Vector2(-2000, 0), 0.05)
		await _shot("15_team_theater", unit_position(pick[0]) + Vector2(8000, 0), 0.02)
		show_cmd = false
	await _shot("02_region_chunks", hq, 0.016)
	await _shot("03_whole_map_overview", world.get_world_size() * 0.5, get_viewport_rect().size.x / world.get_world_size().x * 0.95)

	# Recon east, tanks behind: contact with the hostile strike group follows.
	var home_hex: Vector2i = world.get_unit_info(0)["hex"]
	world.order_move(8, home_hex + Vector2i(60, -4))
	world.order_move(9, home_hex + Vector2i(50, 6))
	world.order_move(1, home_hex + Vector2i(30, 0))
	selected = [8, 1]
	world.advance(9, 1000.0)
	await _shot("04_orders_and_routes", hq + Vector2(7000, 0), 0.03)

	var t0 := Time.get_ticks_msec()
	var ticks := 0
	while world.get_time_minutes() < 6 * 60 + 8 * 60 and Time.get_ticks_msec() - t0 < 60000:
		ticks += world.advance(60, 1000.0)
		_poll_events()
	print("Autotest: simulated %d minutes in %d ms" % [ticks, Time.get_ticks_msec() - t0])
	_refresh_snapshot()
	show_zones = true
	show_los = true
	selected = [3]
	_update_los()
	var focus := unit_position(3)
	if focus == Vector2.ZERO:
		focus = hq
	await _shot("05_contact_zones_sight", focus, 0.035)
	show_los = false
	show_zones = false
	await _shot("06_battle_close", focus, 0.09)
	await _shot_fire("06b_fire_effects", 0.07)

	# Theater map: companies grouped, whole company selected (double-click / K).
	_select_company_of(0)
	await _shot("07_theater_company_selected", unit_position(0) + Vector2(15000, 0), 0.018)
	group_companies = false
	await _shot("08_theater_ungrouped", unit_position(0) + Vector2(15000, 0), 0.018)
	group_companies = true
	selected = [0]
	await _shot("09_theater_wide", unit_position(0) + Vector2(60000, 20000), 0.006)

	# Command panel: whole company on a road march, the battery on hold fire.
	_select_company_of(0)
	_on_command("mode_1")
	_on_command("brk_1")
	_order_move_to(unit_position(0) + Vector2(-22000, -6000))
	await _shot("10_command_panel_orders", unit_position(0) + Vector2(-6000, -2000), 0.03)
	world.advance(40, 1000.0)
	var battery := -1
	for id in selected:
		if String(world.get_unit_info(id)["type_name"]) == "Arty Bty":
			battery = id
	if battery >= 0:
		selected = [battery]
		_on_command("roe_2")
	world.advance(30, 1000.0)
	_select_company_of(0)
	await _shot("11_command_panel_marching", unit_position(0), 0.03)

	# Quick save / load, then play on (scripted: the company holds) until the contract resolves.
	quick_save()
	quick_load()
	var t_stop := world.get_time_minutes() + 4 * 1440
	while world.get_time_minutes() < t_stop and int(world.get_objectives()["outcome"]) == 0:
		world.advance(600, 2000.0)
	_poll_events()
	_check_outcome()
	print("Autotest: outcome ", world.get_objectives())
	await _shot("12_end_screen" if int(world.get_objectives()["outcome"]) != 0 else "12_contract_status", unit_position(0), 0.03)
	hud.end_panel.visible = false
	# Close-up of the most worn-down unit: combat effectiveness, element pips, panel breakdown.
	var worn := -1
	var worst_ce := 1000.0
	for id in _all_own_ids():
		var inf: Dictionary = world.get_unit_info(id)
		var frac := float(inf.get("elements_active", 1)) / maxf(1.0, float(inf.get("elements_max", 1)))
		var score := frac * 100.0 - float(inf.get("engage_factor", 1.0))
		if inf.get("alive", false) and score < worst_ce:
			worst_ce = score
			worn = id
	if worn >= 0:
		selected = [worn]
		await _shot("13_combat_effectiveness", unit_position(worn), 0.12)


	# Streaming test: sweep across the map at chunk zoom and record the worst frame.
	camera.zoom = Vector2.ONE * 0.02
	camera.position = Vector2(20000, 300000)
	var worst := 0.0
	var built_before := map_view.total_built
	var t_pan := Time.get_ticks_msec()
	for f in 240:
		camera.position.x += 3000.0
		await get_tree().process_frame
		worst = maxf(worst, map_view.build_ms_last_frame)
	print("Autotest: panned %d km, built %d chunks, worst chunk-build time in one frame %.1f ms, loaded now %d" % [
		240 * 3, map_view.total_built - built_before, worst, map_view.chunks.size()])

	var stats: Dictionary = world.get_stats()
	print("Autotest stats: ", stats)
	for e in event_log:
		print("  ", e["time_string"], "  ", e["text"])
	get_tree().quit()


## Plays on a minute per frame until fire is exchanged in sight, then captures the tracers.
func _shot_fire(name: String, zoom: float) -> void:
	camera.zoom = Vector2.ONE * zoom
	var auto := auto_pause
	auto_pause = false
	for f in 900:
		world.advance(1, 200.0)
		await get_tree().process_frame
		var found := false
		for e in fx:
			if e["k"] == "shot" or e["k"] == "arty":
				camera.position = (e["a"] as Vector2).lerp(e["b"], 0.5)
				found = true
				break
		if found:
			for k in 2:
				world.advance(1, 200.0)
				await get_tree().process_frame
			break
	auto_pause = auto
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(autotest_dir.path_join(name + ".png"))
	print("Autotest: saved %s with %d effects live" % [name, fx.size()])


func _shot(name: String, pos: Vector2, zoom: float) -> void:
	camera.position = pos
	camera.zoom = Vector2.ONE * zoom
	_refresh_snapshot()
	for i in 40: # let chunks stream in
		map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
		overlay.queue_redraw()
		hud.refresh(world.get_time_string(), world.is_night(), world.get_stats(), Engine.get_frames_per_second(), "chunks %d" % map_view.chunks.size())
		_refresh_selection_panel()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png(autotest_dir.path_join(name + ".png"))
	print("Autotest: saved %s  chunks loaded %d (built %d)  overlay draw %.2f ms" % [name, map_view.chunks.size(),
		map_view.total_built, overlay.draw_ms])
