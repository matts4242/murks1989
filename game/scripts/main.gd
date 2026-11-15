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
var tools: MapTools                # ruler, profile, km grid, markings (MRK-26)
var map_info: Dictionary = {}      # world.get_map_info()
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
var move_mode := -1                # armed right-click order: -1 none (right-click opens the order menu), 0 hasty, 1 road march, 2 deliberate, 3 assault
const MODE_NAMES := ["Move", "Road march", "Deliberate move", "Assault"]
const MAX_WAYPOINTS := 16

# Route plotting (MRK-20): Shift+right-click adds a waypoint for the selected unit or team;
# Enter sends the whole route as one order, Backspace removes the last waypoint, Esc drops the plan.
var route_draft: Array = []   # [{hex: Vector2i, mode: int, halt: int, shape: int}]
var route_unit := -1          # who the plan is for (a team's leader for a team)
var route_team := -1          # the team, when the plan is for a team
var route_preview := {}       # {hex, path, minutes}: from the last waypoint to the cursor
static var skip_title := false     # New game from the menus: reload straight into play, past the title screen
var menu_world_pos := Vector2.ZERO # where the right-click order menu was opened (world units)
var resume_speed := 0              # speed to go back to when the pause menu closes
var briefing_text := ""
var formation_names := {}          # formation id -> name (own companies)
var selected: Array[int] = []
var snapshot := PackedFloat32Array()
var stride := 10
var los_hexes := PackedVector2Array()
# Planning map (MRK-25): the mode shown, and for line of sight the point and eye height.
var plan_mode := 0
var sight_from := Vector2i(-1, -1)
var sight_eye_index := 1           # into hud.PLAN_EYES: 3 m, as the rules
var sight_view := {}               # MurksWorld.get_viewshed
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
var show_supply := false           # supply overlay (F3): routes and convoys
var holdings: Array = []           # world.get_holdings(), refreshed with the HUD
var routes: Array = []             # world.get_supply_routes(), refreshed with the HUD
var selected_holding := -1         # holding shown in the panel (clears the unit selection)
var show_cmd := false              # direct command overlay: every unit linked to its commander, zones, outlines
var show_fs := false               # fire support overlay: the fire net, range rings, fire missions, outlines
var show_oob := true               # order of battle panel (F11)
var oob_rows: Array = []           # world.get_oob(), refreshed with the HUD
var oob_children := {}             # unit id -> ids that answer to it (chain of command; squads under their vehicles)
var oob_under := {}                # unit id -> true: under a selected commander (blue on the map and in the panel)
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
var uitest := false                # --uitest: scripted check of the menus, then quit
var views_dir := ""                # --views=<dir>: render a fixed set of map views, then quit


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--autotest="):
			autotest_dir = a.substr(11)
		elif a.begins_with("--groups="):
			hostile_groups = a.substr(9).to_int()
		elif a == "--procedural":
			force_procedural = true
		elif a == "--uitest":
			uitest = true
		elif a.begins_with("--views="):
			views_dir = a.substr(8)

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
	tools = MapTools.new()           # under the counters
	tools.main = self
	overlay_layer.add_child(tools)
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
	hud.menu_action.connect(_on_menu_action)
	hud.end_choice.connect(_on_end_choice)
	hud.save_path = SAVE_FILE
	hud.event_clicked.connect(func(p: Vector2) -> void: camera.position = p)
	hud.oob_pick.connect(_on_oob_pick)

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
	map_info = info
	tools.refresh_marks()
	print("Murks: map '%s' %d x %d hexes (%.1f MB), %d towns; startup %d ms" % [info["name"], int(info["cols"]),
		int(info["rows"]), float(gen_info["memory_mb"]), int(info["settlements"]), Time.get_ticks_msec() - t0])

	var hq: Dictionary = world.get_unit_info(0)
	camera.position = world.hex_to_world(hq["hex"])
	camera.zoom = Vector2.ONE * 0.045
	selected = [0]
	hud.loading.visible = false
	hud.set_toggle("Grid", map_view.show_grid)
	hud.set_toggle("Detail", map_view.show_detail)
	hud.set_toggle("Edges", map_view.natural_edges)
	hud.set_toggle("Vehicles", overlay.show_vehicles)
	hud.set_toggle("Topo", map_view.show_topo)
	hud.set_toggle("KmGrid", tools.show_km_grid)
	_show_plan_panel()   # fills the View > Planning map list
	hud.set_toggle("Auto-pause", auto_pause)
	hud.set_toggle("Group", group_companies)
	hud.set_oob_visible(show_oob)
	_refresh_oob()
	set_speed(0)
	var obj: Dictionary = world.get_objectives()
	briefing_text = "Company Murks is billeted at %s. The %s is coming for you from the east.\nBreak it before it breaks you, and guard the HQ: without it orders crawl and supplies slow to half." % [
		obj.get("home", _home_name()), obj.get("enemy_name", "enemy")]
	_poll_events()
	ready_to_play = true
	if views_dir != "":
		_run_views()
	elif autotest_dir != "":
		_run_autotest()
	elif uitest:
		hud.show_title(_title_briefing())
		_run_uitest()
	elif skip_title:
		skip_title = false
		_begin_campaign()
	else:
		hud.show_title(_title_briefing())


func _title_briefing() -> String:
	var obj: Dictionary = world.get_objectives()
	return ("[b]The contract[/b]\nCompany Murks is billeted at [color=#ffd98a]%s[/color], 14 April 1989. The [color=#ff9a80]%s[/color] is coming " +
		"for you from the east. Break it: none of its %d fighting platoons left at 30%% strength or more. Keep three of " +
		"your own platoons fighting, and guard the HQ: without it orders crawl and supplies slow to half.") % [
		obj.get("home", _home_name()), obj.get("enemy_name", "enemy"), int(obj.get("enemy_start", 6))]


## Leaves the title screen: the clock is still paused; the briefing says how to start.
func _begin_campaign() -> void:
	hud.hide_title()
	hud.show_banner(briefing_text + "\nSelect a unit and right-click the map for orders. Space starts the clock; Esc opens the menu; F1 shows all controls.", 12.0)


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
	else:
		_load_contours(path.get_basename() + ".murkc")
	return result


## Contour lines for the topographic mode: same container as the map, baked beside it by
## tools/bake_contours.py. Optional; without it the mode is simply not available.
func _load_contours(path: String) -> void:
	if not FileAccess.file_exists(path):
		return
	var data := FileAccess.get_file_as_bytes(path)
	if data.size() < 16 or data.slice(0, 8).get_string_from_ascii() != "MURKSZ01":
		push_warning("Not a Murks contour file: " + path)
		return
	var raw_size := data.decode_u64(8)
	var raw := data.slice(16).decompress(raw_size, FileAccess.COMPRESSION_DEFLATE)
	if raw.size() != raw_size or not world.load_contour_bytes(raw):
		push_warning("Could not read relief data from " + path)


func _home_name() -> String:
	var info: Dictionary = world.get_unit_info(0)
	return String(info.get("location", "")).trim_prefix("in ")


# --- Main loop -------------------------------------------------------------------------

func _process(delta: float) -> void:
	if not ready_to_play:
		return
	var _t_start := Time.get_ticks_usec()
	if not hud.is_modal():
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
	_update_oob_under()
	_update_los()
	night_tint.color = Color(0.62, 0.66, 0.82) if world.is_night() else Color.WHITE
	map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
	overlay.queue_redraw()
	tools.queue_redraw()

	hud_timer -= delta
	if hud_timer <= 0.0:
		hud_timer = 0.1
		var stats: Dictionary = world.get_stats()
		holdings = world.get_holdings()
		routes = world.get_supply_routes()
		hud.refresh(world.get_time_string(), world.is_night(), stats, Engine.get_frames_per_second(),
			"script %.1f ms · chunks %d" % [script_ms, map_view.chunks.size()])
		hud.set_speed(speed_index, eff != speed_index)
		_refresh_selection_panel()
		_refresh_oob()
		_check_outcome()
	script_ms = script_ms * 0.9 + 0.1 * ((Time.get_ticks_usec() - _t_start) / 1000.0 + overlay.draw_ms)


func _refresh_selection_panel() -> void:
	if selected.is_empty():
		hud.show_unit({}, 0)
		if selected_holding >= 0:
			var hd: Dictionary = world.get_holding(selected_holding)
			if hd.is_empty():
				selected_holding = -1
			else:
				hud.show_holding(hd, routes, _own_holdings())
		else:
			hud.hide_holding()
		return
	hud.hide_holding()
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
		hud.set_has_own(false)
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
	var team_name := String(_team(teams[0]).get("name", "")) if teams.size() == 1 else ""
	hud.set_team_state(not teams.is_empty(), _own_selected().size() >= 2, shape, spacing, team_name)
	var can_dis := false
	var can_mnt := false
	var detached := false
	for id in _own_selected():
		var mi: Dictionary = world.get_unit_info(id)
		can_dis = can_dis or bool(mi.get("can_dismount", false))
		can_mnt = can_mnt or bool(mi.get("can_mount", false))
		detached = detached or String(mi.get("group_role", "")) == "detached"
	hud.set_mount_state(can_dis, can_mnt)
	hud.set_rejoin_state(detached)
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
	if hud.is_modal():
		# Title screen or pause menu: only Esc and the help keys get through.
		if event is InputEventKey and event.pressed and not event.echo:
			var k := (event as InputEventKey).keycode
			if k in [KEY_ESCAPE, KEY_F1, KEY_F2, KEY_F4]:
				_on_key(k)
		return
	if tools.handle_input(event):
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
			# Shift+right-click plots a waypoint; an armed order (F/Q/E/R or the panel) goes at
			# once; otherwise the order menu opens.
			if mb.shift_pressed and not _own_selected().is_empty():
				_add_waypoint(_screen_to_world(mb.position))
			elif move_mode >= 0 and not _own_selected().is_empty():
				_order_move_to(_screen_to_world(mb.position))
			else:
				_open_order_menu(mb.position)
	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if dragging:
			if (mm.position - drag_start).length() > 4.0:
				drag_moved = true
			if drag_moved:
				camera.position -= mm.relative / camera.zoom.x
		_update_status()
		_update_route_preview()
	elif event is InputEventKey and event.pressed and not event.echo:
		var ke := event as InputEventKey
		if ke.shift_pressed and tools.shift_key(ke.keycode):
			return
		_on_key(ke.keycode)


func _on_key(key: Key) -> void:
	match key:
		KEY_SPACE: set_speed(0 if speed_index > 0 else last_speed)
		KEY_1: set_speed(1)
		KEY_2: set_speed(2)
		KEY_3: set_speed(3)
		KEY_4: set_speed(4)
		KEY_5: set_speed(5)
		KEY_PERIOD: _step_15()
		KEY_H: _order_hold()
		KEY_G: _on_toggle("Grid")
		KEY_V: _on_toggle("Sight")
		KEY_Z: _on_toggle("Zones")
		KEY_P: _on_toggle("WeGo")
		KEY_U: _on_toggle("Auto-pause")
		KEY_TAB: _select_next()
		KEY_K: _on_command("select_company")
		KEY_O: _on_toggle("Group")
		KEY_T: _on_toggle("Direct command")
		KEY_I: _on_toggle("Fire support")
		KEY_J:
			_on_command("team_disband" if Input.is_key_pressed(KEY_SHIFT) else "team_form")
		KEY_L: _on_command("team_select")
		KEY_F: _on_command("mode_0")
		KEY_Q: _on_command("mode_1")
		KEY_E: _on_command("mode_2")
		KEY_R: _on_command("mode_3")
		KEY_B: _on_command("fall_back")
		KEY_Y: _on_command("regroup")
		KEY_U: _on_command("rejoin")
		KEY_N: _on_command("rest")
		KEY_COMMA: _on_command("mount_toggle")
		KEY_X: _cycle_roe()
		KEY_C, KEY_HOME: _on_menu_action("centre")
		KEY_M: _on_menu_action("whole_map")
		KEY_ESCAPE: _on_escape()
		KEY_F3: _on_toggle("Supply")
		KEY_ENTER, KEY_KP_ENTER: _send_route()
		KEY_BACKSPACE: _drop_waypoint()
		KEY_F1: hud.toggle_help()
		KEY_F2: hud.toggle_credits()
		KEY_F4: hud.toggle_legend()
		KEY_F6: _on_toggle("Detail")
		KEY_F7: _on_toggle("Vehicles")
		KEY_F8: _on_toggle("Topo")
		KEY_F10: set_plan_mode((plan_mode + 1) % MapView.PLAN_COUNT)
		KEY_F11: _on_toggle("OOB")
		KEY_F5: quick_save()
		KEY_F9: quick_load()


func _on_toggle(name: String) -> void:
	match name:
		"Grid":
			map_view.show_grid = not map_view.show_grid
			hud.set_toggle(name, map_view.show_grid)
		"KmGrid":
			tools.show_km_grid = not tools.show_km_grid
			hud.set_toggle(name, tools.show_km_grid)
			if tools.show_km_grid:
				hud.show_toast("1 km grid on the map's own metres; the status bar gives the grid reference (100 km square, then 100 m).", 5.0)
		"Detail":
			map_view.show_detail = not map_view.show_detail
			hud.set_toggle(name, map_view.show_detail)
		"Edges":
			map_view.natural_edges = not map_view.natural_edges
			hud.set_toggle(name, map_view.natural_edges)
			hud.show_toast("Terrain borders wander naturally; the grid shows the true hexes." if map_view.natural_edges
				else "Terrain follows the hex borders exactly.")
		"Topo":
			if not world.has_contours():
				hud.show_toast("This map has no relief data (bake it with tools/bake_contours.py).")
				hud.set_toggle(name, false)
				return
			map_view.show_topo = not map_view.show_topo
			hud.set_toggle(name, map_view.show_topo)
		"Vehicles":
			overlay.show_vehicles = not overlay.show_vehicles
			hud.set_toggle(name, overlay.show_vehicles)
			hud.show_toast("Counters show each unit's main vehicle." if overlay.show_vehicles else "Counters show NATO symbols.")
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
		"OOB":
			show_oob = not show_oob
			hud.set_oob_visible(show_oob)
			if show_oob:
				hud.oob_sig = "" # rebuild with what has changed while it was hidden
				_refresh_oob()
		"Auto-pause":
			auto_pause = not auto_pause
			hud.set_toggle(name, auto_pause)
		"Direct command":
			show_cmd = not show_cmd
			hud.set_toggle(name, show_cmd)
			hud.show_toast("Direct command: every unit is outlined and linked to the HQ or team leader it takes orders from. Solid: inside the commander's zone. Amber dashes: outside it, orders ×1.5. Red dashes: beyond radio range, orders go by courier. Double white ring: the HQ." if show_cmd else "Direct command overlay off: command zones show for the selection only.", 8.0)
		"Supply":
			show_supply = not show_supply
			hud.set_toggle(name, show_supply)
			hud.show_toast("Supply overlay: each route from its source to the trains or another holding, the column's road on top. Green: on the road. White: loading or unloading. Amber: halted. Red: under fire. Grey: suspended, the column is lost." if show_supply else "Supply overlay off.", 8.0)
		"Fire support":
			show_fs = not show_fs
			hud.set_toggle(name, show_fs)
			hud.show_toast("Fire support net (its own radio band): guns ringed thick with their range, observers (HQ and recon) linked to the guns they reach, other units dashed to the observer who relays their calls (+2 min). Grey dashes: cut off, nobody can pass on their calls. Guns only fire on what someone on the net can see." if show_fs else "Fire support overlay off.", 9.0)


# --- Menus ------------------------------------------------------------------------------

## Game-level actions from the menu bar, the title screen and the pause menu.
func _on_menu_action(name: String) -> void:
	if tools.on_action(name):
		return
	if name.begins_with("plan_") and name.substr(5).is_valid_int():
		set_plan_mode(name.substr(5).to_int())
		return
	if name.begins_with("sight_eye_"):
		sight_eye_index = clampi(name.substr(10).to_int(), 0, hud.PLAN_EYES.size() - 1)
		_update_sight()
		tools.on_action("sight_eye_changed")
		return
	match name:
		"start": _begin_campaign()
		"continue":
			if FileAccess.file_exists(SAVE_FILE):
				hud.hide_title()
				quick_load()
		"new_game":
			skip_title = true
			get_tree().reload_current_scene()
		"title":
			skip_title = false
			get_tree().reload_current_scene()
		"quit": get_tree().quit()
		"save": quick_save()
		"load":
			if FileAccess.file_exists(SAVE_FILE):
				hud.hide_pause()
				hud.hide_title()
				resume_speed = 0
			quick_load()
		"resume": _close_pause()
		"pause_menu": _open_pause()
		"help": hud.toggle_help()
		"credits": hud.toggle_credits()
		"legend": hud.toggle_legend()
		"plan_next": set_plan_mode((plan_mode + 1) % MapView.PLAN_COUNT)
		"briefing": hud.show_banner(briefing_text, 10.0)
		"centre":
			if not selected.is_empty():
				camera.position = unit_position(selected[0])
		"whole_map":
			camera.position = world.get_world_size() * 0.5
			camera.zoom = Vector2.ONE * clampf(get_viewport_rect().size.x / world.get_world_size().x * 0.95, ZOOM_MIN, ZOOM_MAX)
		"next_unit": _select_next()
		"step": _step_15()


## Esc: close a dialog, else cancel an armed order, else open or close the pause menu.
func _on_escape() -> void:
	if hud.close_dialog() or hud.title_screen.visible:
		return
	if hud.pause_screen.visible:
		_close_pause()
	elif tools.cancel():
		return
	elif not route_draft.is_empty():
		_clear_route_draft()
		hud.show_toast("Route plan dropped.", 2.0)
	elif move_mode >= 0:
		move_mode = -1
		hud.show_toast("Armed order cancelled: right-click opens the order menu again.", 2.5)
	else:
		_open_pause()


func _open_pause() -> void:
	resume_speed = speed_index
	if speed_index > 0:
		set_speed(0)
	hud.show_pause(world.get_time_string())


func _close_pause() -> void:
	hud.hide_pause()
	if resume_speed > 0:
		set_speed(resume_speed)
	resume_speed = 0


func _step_15() -> void:
	if speed_index == 0:
		world.advance(15, 1000.0)
	else:
		hud.show_toast("Pause first (Space), then step 15 minutes.", 2.5)


## Right-click: the order menu for the selection, aimed at the hex under the cursor. With
## nothing of ours selected it offers the map commands only.
func _open_order_menu(screen_pos: Vector2) -> void:
	menu_world_pos = _screen_to_world(screen_pos)
	var own := _own_selected()
	if own.is_empty():
		hud.set_has_own(false)
	else:
		_update_orders_state()
	var ctx := {}
	var hex := world.world_to_hex(menu_world_pos)
	var hi: Dictionary = world.get_hex_info(hex)
	var where := "off the map"
	if not hi.is_empty():
		var bits: Array[String] = [String(hi["terrain"]), "%d m" % int(hi["elevation"]), _slope_text(hi)]
		if hi.get("major_road", false): bits.append("main road")
		elif hi.get("road", false): bits.append("road")
		if hi.get("bridge", false): bits.append("bridge")
		if hi.has("settlement"): bits.append(String(hi["settlement"]))
		where = " · ".join(bits)
	var hid := world.holding_at(hex)
	if hid >= 0:
		ctx["holding"] = world.get_holding(hid)
		ctx["own_holdings"] = _own_holdings()
	var mi := tools.marking_at(screen_pos)
	if mi >= 0:
		var mk: Dictionary = tools.marks[mi]
		ctx["marking"] = {"i": mi, "kind": int(mk["kind"]), "name": String(mk["name"])}
	if own.is_empty():
		ctx["title"] = "Map"
		ctx["info"] = where
	else:
		var first: Dictionary = world.get_unit_info(own[0])
		var nm := String(first.get("name", "?"))
		var teams := _selected_teams()
		var company := _whole_company(own)
		if company != "":
			nm = company
		elif teams.size() == 1 and own.size() == (_team(teams[0]).get("units", PackedInt32Array()) as PackedInt32Array).size():
			nm = String(_team(teams[0])["name"])
		elif own.size() > 1:
			nm = "%s +%d" % [nm, own.size() - 1]
		ctx["title"] = "Orders: " + nm
		ctx["info"] = "%.1f km away · %s" % [unit_position(own[0]).distance_to(menu_world_pos) / 1000.0, where]
		ctx["can_move"] = not hi.is_empty()
	# A hostile counter under the cursor: only an assault may go into its hex.
	for i in range(overlay.drawn.size() - 1, -1, -1):
		var dc: Dictionary = overlay.drawn[i]
		if int(dc["side"]) != 0 and (dc["rect"] as Rect2).grow(3.0).has_point(screen_pos):
			var ei: Dictionary = world.get_unit_info(int(dc["id"]))
			ctx["enemy"] = "Hostile %s here: only an assault goes in" % String(ei.get("name", "unit"))
			break
	hud.open_order_menu(screen_pos, ctx)


## The company's name if `ids` is exactly one whole company (two or more units), else "".
func _whole_company(ids: Array[int]) -> String:
	if ids.size() < 2 or not disp_row.has(ids[0]):
		return ""
	var fid := int(snapshot[int(disp_row[ids[0]]) * stride + 10])
	for id in ids:
		if not disp_row.has(id) or int(snapshot[int(disp_row[id]) * stride + 10]) != fid:
			return ""
	var members := 0
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride + 10]) == fid:
			members += 1
	return formation_name(fid, ids[0]) if members == ids.size() else ""


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
	for hm in overlay.drawn_holdings:
		if (hm["rect"] as Rect2).grow(2.0).has_point(screen_pos):
			selected.clear()
			selected_holding = int(hm["id"])
			_refresh_selection_panel()
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
			selected_holding = -1
		return
	selected_holding = -1
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
	var mode := maxi(move_mode, 0)
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
			var offset := (unit_position(id) - centroid).limit_length(1000.0 if mode == 1 else 3000.0)
			target = world.world_to_hex(world_pos + offset)
		var d := -1
		if lead_team.has(id):
			d = world.order_group_move(lead_team[id], target, mode)
			if d < 0:
				d = world.order_group_move(lead_team[id], hex, mode)
			if d >= 0:
				var tm := _team(lead_team[id])
				team_names.append("%s (%s, %d units)" % [tm["name"], String(tm["shape_name"]).to_lower(), (tm["units"] as PackedInt32Array).size()])
				sent += (tm["units"] as PackedInt32Array).size() - 1
		else:
			d = world.order_move(id, target, mode)
			if d < 0:
				d = world.order_move(id, hex + SLOT_OFFSETS[k % SLOT_OFFSETS.size()], mode)
			if d < 0:
				d = world.order_move(id, hex, mode)
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
	hud.show_toast("%s ordered for %d unit%s. Orders take effect in %s (command delay).%s" % [MODE_NAMES[mode], sent, "" if sent == 1 else "s", when, teams_txt])
	# An armed order applies once: the next right-click opens the order menu again.
	move_mode = -1


# --- Routes (MRK-20) ---------------------------------------------------------------------

## Who a route plan goes to: one unit, or a whole team when its leader and only its members
## are selected. Empty when the selection is anything else.
func _route_target() -> Dictionary:
	var own := _own_selected()
	if own.is_empty():
		return {}
	for gid in _selected_teams():
		var tm := _team(gid)
		if tm.is_empty() or not own.has(int(tm["leader"])):
			continue
		var members: PackedInt32Array = tm["units"]
		var all_in := true
		for id in own:
			if not members.has(id):
				all_in = false
		if all_in:
			return {"unit": int(tm["leader"]), "team": gid}
	if own.size() == 1:
		return {"unit": own[0], "team": -1}
	return {}


func _routes_supported() -> bool:
	return world.has_method("order_route") # an older extension binary has no routes


func _clear_route_draft() -> void:
	route_draft.clear()
	route_unit = -1
	route_team = -1
	route_preview = {}


## Shift+right-click: adds a waypoint, at the armed move mode (hasty if none) for the leg to it.
## The first waypoint for a unit starts from the route it is on, so the plan extends that route.
func _add_waypoint(world_pos: Vector2) -> void:
	if not _routes_supported():
		hud.show_toast("Routes need the rebuilt extension (game/bin).")
		return
	var tgt := _route_target()
	if tgt.is_empty():
		hud.show_toast("Routes go to one unit or one team at a time: select it and Shift+right-click.")
		return
	if int(tgt["unit"]) != route_unit:
		_clear_route_draft()
		route_unit = int(tgt["unit"])
		route_team = int(tgt["team"])
		var info: Dictionary = world.get_unit_info(route_unit)
		for w in info.get("route", []):
			route_draft.append({"hex": w["hex"], "mode": int(w["mode"]), "halt": int(w["halt"]), "shape": int(w["shape"])})
	if route_draft.size() >= MAX_WAYPOINTS:
		hud.show_toast("A route holds %d waypoints at most." % MAX_WAYPOINTS)
		return
	var mode := maxi(move_mode, 0)
	route_draft.append({"hex": world.world_to_hex(world_pos), "mode": mode, "halt": 0, "shape": -1})
	var est: Dictionary = world.route_delay(route_unit, route_draft)
	if int(est.get("delay", -1)) < 0:
		route_draft.pop_back()
		hud.show_toast("Can't put a waypoint there: water or off the map.")
		return
	route_preview = {}
	hud.show_toast("Waypoint %d (%s). Enter sends the route: %s in %d min. Backspace removes the last, Esc drops the plan." % [
		route_draft.size(), MODE_NAMES[mode].to_lower(),
		"an adjustment" if est.get("adjust", false) else "a new order", int(est["delay"])], 5.0)


func _drop_waypoint() -> void:
	if route_draft.is_empty():
		return
	route_draft.pop_back()
	route_preview = {}
	if route_draft.is_empty():
		_clear_route_draft()
		hud.show_toast("Route plan dropped.", 2.0)
	else:
		hud.show_toast("Waypoint removed: %d left." % route_draft.size(), 2.0)


## Enter: the whole route goes down the chain as one order with one command delay.
func _send_route() -> void:
	if route_draft.is_empty() or not _routes_supported():
		return
	var est: Dictionary = world.route_delay(route_unit, route_draft)
	var d := -1
	if route_team >= 0:
		d = world.order_group_route(route_team, route_draft)
	else:
		d = world.order_route(route_unit, route_draft)
	if d < 0:
		hud.show_toast("That route can't be given.")
		return
	var last: Dictionary = route_draft[route_draft.size() - 1]
	pings.append({"pos": world.hex_to_world(last["hex"]), "t": Time.get_ticks_msec(), "color": Color(0.6, 0.85, 1.0)})
	hud.show_toast("Route of %d waypoint%s ordered%s. Takes effect in %d min (%s)." % [
		route_draft.size(), "" if route_draft.size() == 1 else "s",
		" for the team" if route_team >= 0 else "", d,
		"adjustment" if est.get("adjust", false) else "command delay"])
	_clear_route_draft()


## The leg from the last waypoint to the cursor, with its travel time, while plotting.
func _update_route_preview() -> void:
	if route_draft.is_empty() or not _routes_supported():
		return
	var hex: Vector2i = world.world_to_hex(get_global_mouse_position())
	if route_preview.get("hex", Vector2i(-1, -1)) == hex:
		return
	var last: Dictionary = route_draft[route_draft.size() - 1]
	var plan: Dictionary = world.plan_leg(route_unit, last["hex"], hex, maxi(move_mode, 0))
	route_preview = {"hex": hex, "path": plan.get("path", PackedVector2Array()), "minutes": float(plan.get("minutes", -1.0))}


## Orders from the command panel (and their keys).
func _on_command(name: String) -> void:
	if name.begins_with("move_here_"):
		# From the right-click menu: this order, to the hex the menu was opened on.
		move_mode = name.substr(10).to_int()
		_order_move_to(menu_world_pos)
	elif name.begins_with("mode_"):
		move_mode = name.substr(5).to_int()
		hud.show_toast("Next right-click sends: %s. Esc cancels." % MODE_NAMES[move_mode], 3.0)
	elif name == "disarm":
		move_mode = -1
	elif name == "centre_here":
		camera.position = menu_world_pos
	elif name == "look_here":
		look_from(world.world_to_hex(menu_world_pos))
	elif name.begins_with("holding_select_"):
		selected.clear()
		selected_holding = name.substr(15).to_int()
	elif name == "holding_centre":
		if selected_holding >= 0:
			camera.position = world.get_holding(selected_holding).get("pos", camera.position)
	elif name.begins_with("holding_train_"):
		var hid := name.substr(14).to_int()
		var hd: Dictionary = world.get_holding(hid)
		var on: bool = not hd.get("training", false)
		if world.set_training(hid, on):
			hud.show_toast(("%s raises militia: a squad of %d every %d days from the town's volunteers." % [hd["name"], int(hd.get("squad_men", 10)), int(hd.get("train_days", 2))]) if on else "%s stops raising militia." % hd["name"], 5.0)
		else:
			hud.show_toast("%s can't raise militia: it needs loyalty 25, enough people and room for another squad." % hd.get("name", "The place"), 5.0)
	elif name.begins_with("levy_"):
		# levy_<hid>_<kind>
		var parts := name.split("_")
		var hid := parts[1].to_int()
		var why: String = world.levy(hid, parts[2].to_int())
		if why == "":
			hud.show_toast("Requisition done. The town's loyalty falls; the goods are in its stores for the trains or a convoy.", 5.0)
		else:
			hud.show_toast("Requisition refused: " + why, 5.0)
	elif name.begins_with("route_"):
		# route_<source>_<dest>  (dest -1 = the trains)
		var parts := name.split("_")
		var res: Dictionary = world.set_supply_route(parts[1].to_int(), parts[2].to_int())
		if int(res.get("id", -1)) >= 0:
			var src: Dictionary = world.get_holding(parts[1].to_int())
			var dst := "the trains" if parts[2].to_int() < 0 else String(world.get_holding(parts[2].to_int()).get("name", "?"))
			hud.show_toast("Supply route set: %s → %s. The column drives to the source, loads what the destination is short of and shuttles until you cancel it. F3 shows it." % [src.get("name", "?"), dst], 7.0)
			if not show_supply:
				_on_toggle("Supply")
		else:
			hud.show_toast("No route: " + String(res.get("error", "")), 5.0)
	elif name.begins_with("cancel_route_"):
		if world.cancel_supply_route(name.substr(13).to_int()):
			hud.show_toast("Route cancelled: the column comes back to the trains.", 4.0)
	elif name == "select_company":
		if not selected.is_empty():
			_select_company_of(selected[0])
	elif name == "hold":
		_order_hold()
	elif name == "rejoin":
		var n := 0
		var lo := 1 << 30
		for id in _own_selected():
			var d: int = world.order_rejoin(id)
			if d >= 0:
				n += 1
				lo = mini(lo, d)
		if n > 0:
			hud.show_toast("Rejoin team: %d unit%s, orders arrive in %d min." % [n, "" if n == 1 else "s", lo])
		else:
			hud.show_toast("Nothing to rejoin: select a team member that left its formation.")
	elif name == "fall_back" or name == "regroup" or name == "rest":
		_order_group(name)
	elif name == "dismount" or name == "mount" or name == "mount_toggle":
		_order_mounting(name)
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


## The company's own holdings: [{id, name, kind_name}] for route menus.
func _own_holdings() -> Array:
	var out: Array = []
	for hd in holdings:
		if hd.get("own", false):
			out.append({"id": int(hd["id"]), "name": String(hd["name"]), "kind_name": String(hd["kind_name"])})
	return out


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


## Dismount the squads of the selected APC/IFV platoons, or mount them up again. The comma key
## dismounts when any selected platoon has squads aboard, otherwise mounts up.
func _order_mounting(kind: String) -> void:
	var own := _own_selected()
	var can_dis: Array[int] = []
	var can_mnt: Array[int] = []
	var carriers := {}
	for id in own:
		var info: Dictionary = world.get_unit_info(id)
		if info.get("can_dismount", false):
			can_dis.append(id)
		if info.get("can_mount", false):
			can_mnt.append(id)
			if info.has("squads_max"):
				carriers[id] = true
	if kind == "mount_toggle":
		kind = "dismount" if not can_dis.is_empty() else "mount"
	var ids: Array[int] = can_dis if kind == "dismount" else can_mnt
	if kind == "mount":
		# One order per platoon: skip squads whose vehicles are selected too.
		var keep: Array[int] = []
		for id in ids:
			var info: Dictionary = world.get_unit_info(id)
			if carriers.has(id) or not own.has(int(info.get("carrier_id", -1))):
				keep.append(id)
		ids = keep
	if ids.is_empty():
		hud.show_toast("Nothing to dismount: select an APC or IFV platoon with squads aboard." if kind == "dismount"
			else "Nothing to mount up: select squads on foot, or their vehicles.")
		return
	var lo := 1 << 30
	var hi := 0
	for id in ids:
		var d: int = world.order_dismount(id) if kind == "dismount" else world.order_mount(id)
		if d >= 0:
			lo = mini(lo, d)
			hi = maxi(hi, d)
	hud.show_toast("%s: %d platoon%s, orders arrive in %s." % ["Dismount" if kind == "dismount" else "Mount up",
		ids.size(), "" if ids.size() == 1 else "s", ("%d min" % lo) if lo == hi else ("%d-%d min" % [lo, hi])])


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


## "level", or the steepest grade of a hex with the way the ground falls: "slope 12% (7°) down to the south-east".
func _slope_text(info: Dictionary) -> String:
	var pct := float(info.get("slope_percent", 0.0))
	if int(info.get("slope_class", 0)) == 0 or String(info.get("slope_down", "")) == "":
		return "level" if pct < 3.0 else "slope %d%%" % roundi(pct)
	var names := ["level", "gentle", "moderate", "steep", "very steep"]
	return "%s slope %d%% (%d°) down to the %s" % [names[clampi(int(info["slope_class"]), 0, 4)], roundi(pct),
		roundi(float(info["slope_degrees"])), String(info["slope_down"])]


# --- Planning map (MRK-25) ----------------------------------------------------------------

## Shows a planning mode (MapView.PLAN_*; 0 = off). Line of sight looks from the point set
## with "Look from here", else from the selected unit, else from the HQ.
func set_plan_mode(mode: int) -> void:
	plan_mode = clampi(mode, 0, MapView.PLAN_COUNT - 1)
	if plan_mode == MapView.PLAN_SIGHT and world.get_hex_info(sight_from).is_empty():
		var id: int = selected[0] if not selected.is_empty() else 0   # unit 0: the company HQ
		sight_from = world.get_unit_info(id).get("hex", world.world_to_hex(camera.position))
	if plan_mode != 0 and plan_mode != MapView.PLAN_SIGHT and map_view.plan_built != plan_mode:
		hud.show_toast("Working out %s for the whole map..." % world.get_plan_mode_name(plan_mode), 1.5)
	map_view.set_plan_mode(plan_mode)
	if plan_mode == MapView.PLAN_SIGHT:
		_update_sight()
	else:
		_show_plan_panel()


## Line of sight from `hex` (right-click > Look from here).
func look_from(hex: Vector2i) -> void:
	sight_from = hex
	set_plan_mode(MapView.PLAN_SIGHT)


func _update_sight() -> void:
	if plan_mode != MapView.PLAN_SIGHT:
		return
	sight_view = world.get_viewshed(sight_from, float(hud.PLAN_EYES[sight_eye_index][0]))
	map_view.set_sight(sight_view)
	_show_plan_panel()


func _show_plan_panel() -> void:
	var names: Array = []
	for i in MapView.PLAN_COUNT:
		names.append(world.get_plan_mode_name(i))
	var info := ""
	if plan_mode == MapView.PLAN_SIGHT and not sight_view.is_empty():
		var hi: Dictionary = world.get_hex_info(sight_from)
		var where: String = String(hi.get("settlement", hi.get("place", "")))
		var seen := int(sight_view["seen"])
		var total := maxi(1, int(sight_view["total"]))
		info = "From hex %d,%d (%s, %d m), eye %d m: %d of %d hexes within 12 km seen (%d%%)." % [sight_from.x, sight_from.y,
			where, int(hi.get("elevation", 0)), int(hud.PLAN_EYES[sight_eye_index][0]), seen, total, roundi(100.0 * seen / total)]
	hud.show_plan(plan_mode, names, world.get_plan_key(plan_mode) if plan_mode != 0 else [], info, sight_eye_index)


## The planning mode's figure for a hex, for the status bar ("" when no mode is shown).
func _plan_text(info: Dictionary, hex: Vector2i) -> String:
	match plan_mode:
		1, 2, 3:
			var key: String = ["", "going_tracked", "going_wheeled", "going_foot"][plan_mode]
			var who: String = ["", "tracked", "wheeled", "on foot"][plan_mode]
			var m := float(info.get(key, -1.0))
			if m < 0.0:
				return "impassable %s" % who
			return "going %s %.0f km/h off-road (%.1f min a hex)" % [who, 30.0 / m, m]
		MapView.PLAN_COVER:
			if float(info.get("cover_vehicles", 1.0)) >= 1.0:
				return "no cover"
			return "cover: hits x%.1f vehicles, x%.1f infantry%s" % [float(info["cover_vehicles"]), float(info["cover_infantry"]),
				", hides units that sit still" if info.get("conceals", false) else ""]
		MapView.PLAN_SIGHT:
			if sight_view.is_empty():
				return ""
			var o: Vector2i = sight_view["origin"]
			var sz: Vector2i = sight_view["size"]
			var l := hex - o
			if l.x < 0 or l.y < 0 or l.x >= sz.x or l.y >= sz.y:
				return "beyond 12 km of the point"
			var v: int = (sight_view["mask"] as PackedByteArray)[l.y * sz.x + l.x]
			return ["beyond 12 km of the point", "dead ground from the point", "seen from the point"][v]
	return ""


func _update_status() -> void:
	var hex := world.world_to_hex(get_global_mouse_position())
	var info: Dictionary = world.get_hex_info(hex)
	if info.is_empty():
		hud.set_status("")
		return
	var bits: Array[String] = [String(info["terrain"]), "%d m" % int(info["elevation"]), _slope_text(info)]
	var pt := _plan_text(info, hex)
	if pt != "":
		bits.insert(0, pt)
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
	if tools.show_km_grid:
		where += "  GR " + world.get_grid_ref(get_global_mouse_position())
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
	if info.has("carrier_name"):
		s = "McD" # a mech platoon's squads on foot
	if s != "HQ" and s != "Bty" and nm.length() > 0 and nm[0].is_valid_int():
		s = nm[0] + s
	short_names[id] = s
	return s


# --- Order of battle ---------------------------------------------------------------------

## Reads the order of battle and works out who answers to whom. The links come from the rules'
## own chain of command (MurksWorld.get_oob: team members to their leader, the rest to the
## company HQ), so the panel and the blue highlight show the chain the command delay runs on.
func _refresh_oob() -> void:
	oob_rows = world.get_oob()
	oob_children = {}
	var alive := {}
	for r in oob_rows:
		if r["alive"]:
			alive[int(r["id"])] = true
	for r in oob_rows:
		var par := int(r["parent"])
		if r["alive"] and not r["militia"] and par >= 0 and alive.has(par):
			if not oob_children.has(par):
				oob_children[par] = []
			oob_children[par].append(int(r["id"]))
	_update_oob_under()
	hud.show_oob(oob_rows, selected, oob_under, oob_children)


## Everything under a selected HQ or team leader, all the way down.
func _update_oob_under() -> void:
	oob_under = {}
	for id in selected:
		if oob_children.has(id):
			_collect_under(id, oob_under)


func _collect_under(id: int, out: Dictionary) -> void:
	for c in oob_children.get(id, []):
		if not out.has(c):
			out[c] = true
			_collect_under(int(c), out)


## A row picked in the order of battle panel.
func _on_oob_pick(id: int, how: String) -> void:
	var row := {}
	for r in oob_rows:
		if int(r["id"]) == id:
			row = r
	if how == "company":
		_select_company_of(id)
		hud.oob_sel_seen = selected.duplicate() # picked in the panel: don't scroll it
		return
	if row.is_empty() or not row["alive"]:
		return
	# Squads aboard their vehicles are not on the map: pick the vehicles.
	if row["embarked"] and int(row["parent"]) >= 0:
		id = int(row["parent"])
	selected_holding = -1
	match how:
		"add":
			if selected.has(id):
				selected.erase(id)
			else:
				selected.append(id)
		"command":
			var under := {}
			_collect_under(id, under)
			var ids: Array[int] = [id]
			for u in under:
				if disp_row.has(int(u)):
					ids.append(int(u))
			selected = ids
			camera.position = unit_position(id)
			if ids.size() > 1:
				hud.show_toast("%s and the %d unit%s under it selected." % [row["name"], ids.size() - 1, "" if ids.size() == 2 else "s"])
		_:
			selected = [id]
	hud.oob_sel_seen = selected.duplicate() # picked in the panel: it is already in view
	_update_oob_under()
	_refresh_selection_panel()


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
	overlay._veh_cache.clear()
	if err != "":
		hud.show_toast("Could not load the save: " + err, 5.0)
		return
	tools.refresh_marks()
	tools.clear_ruler()
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


# --- Automated check of the menus: real input events and menu picks, checked against the sim. ---

func _run_uitest() -> void:
	var res := [0, 0] # passed, failed
	var check := func(ok: bool, what: String) -> void:
		res[0 if ok else 1] += 1
		print("UITEST %s  %s" % ["PASS" if ok else "FAIL", what])
	var vs := get_viewport_rect().size
	await _frames(3)

	check.call(hud.title_screen.visible and hud.is_modal(), "title screen up at start")
	_push_key(KEY_SPACE)
	await _frames(2)
	check.call(speed_index == 0, "Space does nothing behind the title screen")
	hud.menu_action.emit("start")
	await _frames(2)
	check.call(not hud.title_screen.visible and not hud.is_modal(), "New campaign leaves the title screen")

	# Right-click a hex with the whole company selected: the order menu opens; pick Road march here.
	_select_company_of(0)
	var own := _own_selected()
	_push_click(MOUSE_BUTTON_RIGHT, vs * Vector2(0.3, 0.45))
	await _frames(3)
	check.call(hud.order_popup.visible, "right-click opens the order menu")
	check.call(_menu_pick(hud.order_popup, "Road march here"), "order menu has Road march here")
	hud.order_popup.hide()
	var all_road := true
	for id in own:
		var inf: Dictionary = world.get_unit_info(id)
		all_road = all_road and inf.get("pending", false) and String(inf.get("pending_kind", "")) == "Road march"
	check.call(all_road and own.size() >= 5, "Road march here reaches all %d units of the company" % own.size())
	check.call(move_mode == -1, "nothing armed after a menu order")

	# Arm an assault with R: the next right-click sends it at once, no menu.
	selected = [1]
	_push_key(KEY_R)
	await _frames(1)
	check.call(move_mode == 3, "R arms an assault")
	_push_click(MOUSE_BUTTON_RIGHT, vs * Vector2(0.25, 0.3))
	await _frames(3)
	var inf1: Dictionary = world.get_unit_info(1)
	check.call(not hud.order_popup.visible and String(inf1.get("pending_kind", "")) == "Assault", "armed right-click sends the assault without a menu")
	check.call(move_mode == -1, "the armed order is used up")

	# Esc: cancels an armed order first, then opens and closes the pause menu (clock stops, resumes).
	_push_key(KEY_Q)
	_push_key(KEY_ESCAPE)
	await _frames(1)
	check.call(move_mode == -1 and not hud.pause_screen.visible, "Esc cancels an armed road march")
	set_speed(2)
	_push_key(KEY_ESCAPE)
	await _frames(1)
	check.call(hud.pause_screen.visible and speed_index == 0, "Esc opens the pause menu and stops the clock")
	_push_key(KEY_ESCAPE)
	await _frames(1)
	check.call(not hud.pause_screen.visible and speed_index == 2, "Esc again resumes at the same speed")
	set_speed(0)

	# Panel dropdowns send the same commands as the keys.
	hud.roe_opt.item_selected.emit(2)
	inf1 = world.get_unit_info(1)
	check.call(inf1.get("stance_pending", false) and int(inf1.get("pending_roe", -1)) == 2, "Fire dropdown: hold fire on its way")
	hud.standoff_opt.item_selected.emit(3) # 2 km
	inf1 = world.get_unit_info(1)
	check.call(int(inf1.get("pending_standoff", -1)) == 4, "Stand-off dropdown: 2 km on its way")

	# Menu bar: View > Hex grid toggles the grid.
	var grid_was: bool = map_view.show_grid
	var view := hud.menu_buttons["View"] as MenuButton
	view.show_popup()
	await _frames(2)
	check.call(_menu_pick(view.get_popup(), "Hex grid"), "View menu has Hex grid")
	view.get_popup().hide()
	check.call(map_view.show_grid != grid_was, "View > Hex grid toggles the grid")

	# Map visuals (MRK-29): the ground is painted by the terrain shader, with roads, rivers and
	# railways as chunk meshes over it. F6 and the View menu switch the painted textures off
	# and on; the View menu switches natural terrain edges; the grid fades in with the zoom.
	var zoom_was := camera.zoom
	camera.zoom = Vector2.ONE * 0.08   # 40 px per hex
	for i in 10:
		map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
		await get_tree().process_frame
	var tmat: ShaderMaterial = map_view.terrain_mat
	check.call(map_view.ground.material == tmat and tmat.shader != null and tmat.get_shader_parameter("hex_data") != null,
		"the ground is painted by the terrain shader")
	var line_meshes := 0
	for mi in map_view.chunks.values():
		line_meshes += (mi as MeshInstance2D).mesh.get_surface_count()
	check.call(map_view.chunk_root.visible and line_meshes > 0, "roads, rivers and railways are drawn over it (%d chunk meshes)" % line_meshes)
	if not map_view.show_grid:
		_on_toggle("Grid")   # the check above left it switched off
		await _frames(2)
	check.call(float(tmat.get_shader_parameter("grid_alpha")) > 0.0, "the hex grid shows at 40 px per hex")
	await RenderingServer.frame_post_draw
	var painted := get_viewport().get_texture().get_image()
	_push_key(KEY_F6)
	await _frames(3)
	check.call(not map_view.show_detail and float(tmat.get_shader_parameter("detail")) == 0.0, "F6 switches the painted textures off")
	await RenderingServer.frame_post_draw
	var flat_img := get_viewport().get_texture().get_image()
	var differ := 0
	for y in range(120, 780, 6):
		for x in range(40, 1560, 6):
			if absf(painted.get_pixel(x, y).v - flat_img.get_pixel(x, y).v) > 0.04:
				differ += 1
	check.call(differ > 2000, "the painted textures change the picture (%d of 27,940 sampled pixels)" % differ)
	view.show_popup()
	await _frames(2)
	check.call(_menu_pick(view.get_popup(), "Painted terrain textures (off: flat colours)"), "View menu has Painted terrain textures")
	view.get_popup().hide()
	await _frames(2)
	check.call(map_view.show_detail and float(tmat.get_shader_parameter("detail")) == 1.0, "View > Painted terrain textures turns them back on")
	view.show_popup()
	await _frames(2)
	check.call(_menu_pick(view.get_popup(), "Natural terrain edges (off: crisp hex borders)"), "View menu has Natural terrain edges")
	view.get_popup().hide()
	await _frames(2)
	check.call(not map_view.natural_edges and float(tmat.get_shader_parameter("edge_noise")) == 0.0, "crisp hex borders on request")
	_on_toggle("Edges")
	await _frames(2)
	check.call(map_view.natural_edges and float(tmat.get_shader_parameter("edge_noise")) > 0.0, "natural edges back on")
	camera.zoom = Vector2.ONE * 0.02   # 10 px per hex
	await _frames(2)
	check.call(float(tmat.get_shader_parameter("grid_alpha")) == 0.0 and map_view.chunk_root.visible, "no grid at 10 px per hex; roads still drawn")
	camera.zoom = zoom_was
	await _frames(2)
	view.show_popup()
	await _frames(2)
	check.call(_menu_pick(view.get_popup(), "Map legend"), "View menu has Map legend")
	view.get_popup().hide()
	await _frames(2)
	check.call(hud.legend_panel.visible, "the legend opens")
	_push_key(KEY_F1)
	await _frames(2)
	check.call(hud.help_panel.visible and not hud.legend_panel.visible, "opening Controls closes the legend")
	_push_key(KEY_F4)
	await _frames(2)
	check.call(hud.legend_panel.visible and not hud.help_panel.visible, "F4 opens the legend in its place")
	_push_key(KEY_ESCAPE)
	await _frames(2)
	check.call(not hud.legend_panel.visible and not hud.pause_screen.visible, "Esc closes the legend without opening the pause menu")

	# Counters: a fixed share of the hex at every zoom the hex can hold one, each showing
	# its unit's most important vehicle; F7 switches to NATO symbols and back.
	var fits := true
	for z in [0.03, 0.06, 0.12, 0.3]:
		camera.zoom = Vector2.ONE * float(z)
		await _frames(1)
		var hex_w: float = 500.0 * float(z)
		# A square of side a fits a pointy-top hex of width w up to a = 0.732 w.
		if overlay.counter_size() > hex_w * 0.732:
			fits = false
		if z >= 0.06 and absf(overlay.counter_size() / hex_w - 0.52) > 0.01:
			fits = false
	camera.zoom = zoom_was
	await _frames(2)
	check.call(fits, "counters fit inside their hex from 15 to 150 px per hex, at 52% of its width once above the minimum")
	check.call(Overlay.classify_equipment("4x T-55A") == "tank" and Overlay.classify_equipment("UAZ-469, R-145 radio") == "jeep"
		and Overlay.classify_equipment("3x BMP-1") == "ifv" and Overlay.classify_equipment("3x BTR-60PB") == "apc_w"
		and Overlay.classify_equipment("4x 2S1 Gvozdika") == "spg" and Overlay.classify_equipment("6x Ural-375D") == "truck"
		and Overlay.classify_equipment("2x BRDM-2") == "recon" and Overlay.classify_equipment("6x D-30, 6x Ural-375D") == "gun"
		and Overlay.classify_equipment("3x 2S19") == "spg" and Overlay.classify_equipment("M240 teams") == ""
		and Overlay.classify_equipment("3 squads of 8 men") == "inf", "equipment names map to the right vehicle, the most important one first")
	check.call(overlay.vehicle_of(0, 0) == "jeep" and overlay.vehicle_of(1, 1) == "tank", "the HQ shows its field car, 1st Tank its T-55")
	check.call(Overlay.classify_equipment(Overlay.strip_brackets("3 squads of 8 men (BMP-1)")) == "inf"
		and Overlay.classify_equipment(Overlay.strip_brackets("35 men, 3x BTR-60PB")) == "apc_w",
		"dismounted squads map to infantry; infantry with its own vehicles maps to the vehicle")
	var foot_ok := false
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride + 2]) == 3:
			foot_ok = overlay.vehicle_of(int(snapshot[i * stride]), 3) == ""
			break
	check.call(foot_ok, "infantry on foot shows the NATO symbol")
	_push_key(KEY_F7)
	await _frames(2)
	check.call(not overlay.show_vehicles, "F7 switches counters to NATO symbols")
	view.show_popup()
	await _frames(2)
	check.call(_menu_pick(view.get_popup(), "Vehicles on counters (off: NATO symbols)"), "View menu has Vehicles on counters")
	view.get_popup().hide()
	await _frames(2)
	check.call(overlay.show_vehicles, "View > Vehicles on counters turns them back on")

	# Orders menu > Team > Form team, then the Formation dropdown sets wedge.
	selected = [1, 2, 3]
	_update_orders_state()
	var om := hud.menu_buttons["Orders"] as MenuButton
	om.show_popup()
	await _frames(2)
	var team_sub := _submenu_of(om.get_popup(), "Team")
	check.call(team_sub != null and _menu_pick(team_sub, "Form team from the selection"), "Orders > Team > Form team")
	om.get_popup().hide()
	var teams := _selected_teams()
	check.call(teams.size() == 1, "a team is formed from three units")
	_update_orders_state()
	hud.shape_opt.item_selected.emit(3)
	check.call(teams.size() == 1 and int(_team(teams[0]).get("shape", -1)) == 3, "Formation dropdown: wedge")
	# A member's own order detaches it when it arrives; Rejoin puts it back (MRK-22).
	var member := -1
	for id in [1, 2, 3]:
		if String(world.get_unit_info(id).get("group_role", "")) == "in formation":
			member = id
	var mhex: Vector2i = world.get_unit_info(member).get("hex", Vector2i.ZERO)
	var mdelay: int = world.order_move(member, mhex + Vector2i(6, 0), 0)
	check.call(mdelay > 0 and String(world.get_unit_info(member).get("group_role", "")) == "in formation",
		"a solo order to a team member leaves it in formation while the order travels")
	world.advance(mdelay + 2, 500.0)
	check.call(String(world.get_unit_info(member).get("group_role", "")) == "detached", "when the order arrives the member is detached")
	selected = [member]
	_update_orders_state()
	check.call(hud.rejoin_button.visible and bool(hud.ord["detached"]), "the unit panel offers Rejoin")
	_on_command("rejoin")
	world.advance(60, 500.0)
	check.call(String(world.get_unit_info(member).get("group_role", "")) == "in formation", "Rejoin team puts it back in formation")
	selected = [1, 2, 3]

	# Holdings: the home town's panel, Raise militia, a supply route from its stores to the trains, cancel.
	selected.clear()
	holdings = world.get_holdings()
	var home_id := -1
	for hd in holdings:
		if hd.get("own", false) and int(hd["kind"]) == 0:
			home_id = int(hd["id"])
	check.call(home_id >= 0, "the company holds its home town")
	camera.position = world.get_holding(home_id)["pos"]
	camera.zoom = Vector2.ONE * 0.06
	await _frames(3)
	var marker_hit := false
	for hm in overlay.drawn_holdings:
		if int(hm["id"]) == home_id:
			_push_click(MOUSE_BUTTON_LEFT, (hm["rect"] as Rect2).get_center())
			marker_hit = true
	await _frames(3)
	check.call(marker_hit and selected_holding == home_id and hud.holding_panel.visible, "clicking the pennant opens the holding panel")
	hud.train_button.pressed.emit()
	await _frames(1)
	check.call(bool(world.get_holding(home_id).get("training", false)), "Raise militia starts training")
	_on_command("route_%d_-1" % home_id)
	routes = world.get_supply_routes()
	check.call(routes.size() == 1 and int(routes[0]["source"]) == home_id and int(routes[0]["dest"]) < 0 and show_supply,
		"a route from the town's stores to the trains is set and the supply overlay comes on")
	var truck := -1
	for id in _all_own_ids():
		if int(world.get_unit_info(id).get("convoy_route", -1)) >= 0:
			truck = id
	check.call(truck >= 0 and not world.is_orderable(truck), "the supply column is on the route and takes no orders")
	_push_click(MOUSE_BUTTON_RIGHT, get_viewport_rect().size * 0.5)
	await _frames(3)
	var place_sub := _submenu_of(hud.order_popup, "Requisition")
	check.call(hud.order_popup.visible and place_sub != null, "right-click in the town offers Requisition")
	hud.order_popup.hide()
	var rlist: Array = world.get_supply_routes()
	_on_command("cancel_route_%d" % (int(rlist[0]["id"]) if not rlist.is_empty() else 0))
	rlist = world.get_supply_routes()
	check.call(not rlist.is_empty() and not rlist[0]["active"] and String(rlist[0].get("state_name", "")).begins_with("Returning"),
		"the route can be cancelled: the column returns")
	camera.zoom = Vector2.ONE * 0.03

	# Right-click menu with nothing selected: map commands only (a frame after the last menu closed).
	selected.clear()
	selected_holding = -1
	await _frames(2)
	_push_click(MOUSE_BUTTON_RIGHT, vs * Vector2(0.5, 0.5))
	await _frames(3)
	var has_move := false
	for i in hud.order_popup.item_count:
		has_move = has_move or hud.order_popup.get_item_text(i).ends_with(" here") and \
			not (hud.order_popup.get_item_text(i) in ["Centre the map here", "Measure from here", "Mark here"])
	check.call(hud.order_popup.visible and not has_move, "nothing selected: map menu without move orders")
	hud.order_popup.hide()

	# Topographic mode (MRK-23 / MRK-29): contour lines printed into the painted ground by the
	# terrain shader from the baked height raster (20 m close in, 100 m further out, 500 m
	# on the whole map), with height figures. F8 and the View menu toggle it.
	check.call(world.has_contours() and map_view.has_relief(), "the map's relief file is loaded for hill shading and contours")
	var cam_was := camera.position
	camera.position = world.hex_to_world(Vector2i(900, 1330))   # hill country south of Wroclaw
	camera.zoom = Vector2.ONE * 0.06   # 30 px per hex
	for i in 6:
		map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var plain := get_viewport().get_texture().get_image()
	_push_key(KEY_F8)
	for i in 6:
		map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
		await get_tree().process_frame
	var topo_labels := 0
	for entry in map_view.relief_labels.values():
		topo_labels += (entry["labels"] as PackedFloat32Array).size() / 4 + (entry["spots"] as PackedFloat32Array).size() / 3
	var tm: ShaderMaterial = map_view.terrain_mat
	check.call(map_view.show_topo and map_view.topo_lod == 0 and is_equal_approx(float(tm.get_shader_parameter("contour_interval")), 20.0),
		"F8 prints contours every 20 m close in")
	check.call(topo_labels > 0, "100 m contours and summits carry heights (%d)" % topo_labels)
	await RenderingServer.frame_post_draw
	var with_lines := get_viewport().get_texture().get_image()
	var changed := 0
	for y in range(120, 780, 6):
		for x in range(40, 1560, 6):
			if absf(with_lines.get_pixel(x, y).v - plain.get_pixel(x, y).v) > 0.04:
				changed += 1
	check.call(changed > 800, "the contours show in the hills (%d of 27,940 sampled pixels differ)" % changed)
	var steep: Dictionary = world.get_hex_info(Vector2i(900, 1330))
	var flat: Dictionary = world.get_hex_info(world.world_to_hex(cam_was))
	check.call(float(steep["slope_percent"]) > float(flat["slope_percent"]) and _slope_text(steep).contains("down to the") ,
		"the hex readout gives the slope: hills \"%s\", home \"%s\"" % [_slope_text(steep), _slope_text(flat)])
	camera.zoom = Vector2.ONE * 0.024   # 12 px per hex
	await _frames(3)
	check.call(map_view.topo_lod == 1 and is_equal_approx(float(tm.get_shader_parameter("contour_interval")), 100.0), "further out the contours are 100 m apart")
	camera.zoom = Vector2.ONE * 0.003   # whole-map view
	await _frames(3)
	check.call(map_view.topo_lod == 2 and is_equal_approx(float(tm.get_shader_parameter("contour_interval")), 500.0), "on the whole-map view they are 500 m apart")
	view.show_popup()
	await _frames(2)
	check.call(_menu_pick(view.get_popup(), "Topographic map: contour lines and heights"), "View menu has Topographic map")
	view.get_popup().hide()
	await _frames(2)
	check.call(not map_view.show_topo and float(tm.get_shader_parameter("contour_interval")) == 0.0, "View > Topographic map turns it off again")
	camera.position = cam_was
	camera.zoom = zoom_was
	await _frames(2)

	# Planning map (MRK-25): modes from the View menu, F10 and the panel; each tints the
	# ground; the status bar gives the mode's figure; Look from here sets the line-of-sight
	# point and a higher eye sees at least as much.
	var cam_keep := camera.position
	camera.zoom = Vector2.ONE * 0.05
	for i in 4:
		map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var no_plan := get_viewport().get_texture().get_image()
	view.show_popup()
	await _frames(2)
	var plan_menu := _submenu_of(view.get_popup(), "Planning map")
	check.call(plan_menu != null and _menu_pick(plan_menu, "Going: tracked"), "View > Planning map > Going: tracked")
	view.get_popup().hide()
	await _frames(3)
	var ptm: ShaderMaterial = map_view.terrain_mat
	check.call(plan_mode == 1 and int(ptm.get_shader_parameter("plan_mode")) == 1 and hud.plan_panel.visible,
		"going mode on: tint loaded in %.0f ms, panel shown" % map_view.plan_build_ms)
	await RenderingServer.frame_post_draw
	var going_img := get_viewport().get_texture().get_image()
	var tinted := 0
	for y in range(120, 780, 6):
		for x in range(40, 1560, 6):
			if no_plan.get_pixel(x, y).to_html() != going_img.get_pixel(x, y).to_html() and absf(no_plan.get_pixel(x, y).h - going_img.get_pixel(x, y).h) > 0.02:
				tinted += 1
	check.call(tinted > 5000, "the going tint changes the picture (%d of 27,940 sampled pixels)" % tinted)
	var home_info: Dictionary = world.get_hex_info(world.world_to_hex(cam_keep))
	var going_line := _plan_text(home_info, world.world_to_hex(cam_keep))
	check.call(going_line.begins_with("going tracked") or going_line.begins_with("impassable"), "status bar gives the going: \"%s\"" % going_line)
	check.call(hud.plan_keys.get_child_count() >= 5, "the panel lists the going key (%d rows)" % hud.plan_keys.get_child_count())
	var modes_seen: Array = [plan_mode]
	for i in 4:
		_push_key(KEY_F10)
		await _frames(2)
		modes_seen.append(plan_mode)
	check.call(modes_seen == [1, 2, 3, 4, 5], "F10 steps through the modes %s" % str(modes_seen))
	hud.plan_pick.select(4)
	hud.plan_pick.item_selected.emit(4)
	await _frames(2)
	check.call(plan_mode == MapView.PLAN_COVER, "the panel's selector picks Cover and concealment")
	# Line of sight: right-click > Look from here on a lookout in the hills.
	var lookout := _lookout_near(Vector2i(900, 1330), 16)
	camera.position = world.hex_to_world(lookout)
	await _frames(2)
	selected = []
	_open_order_menu(get_viewport_rect().size * 0.5)
	await _frames(2)
	check.call(_menu_pick(hud.order_popup, "Look from here (line of sight)"), "right-click menu has Look from here")
	hud.order_popup.hide()
	await _frames(2)
	check.call(plan_mode == MapView.PLAN_SIGHT and sight_from == world.world_to_hex(menu_world_pos) and int(ptm.get_shader_parameter("plan_mode")) == 2,
		"Look from here switches to line of sight from that hex")
	var seen_3m := int(sight_view.get("seen", 0))
	hud.plan_eye_pick.select(3)
	hud.plan_eye_pick.item_selected.emit(3)
	await _frames(2)
	var seen_30m := int(sight_view.get("seen", 0))
	check.call(seen_3m > 0 and seen_30m >= seen_3m, "a 30 m eye sees at least as much as 3 m (%d vs %d of %d hexes)" % [seen_30m, seen_3m, int(sight_view.get("total", 0))])
	check.call(_plan_text(world.get_hex_info(lookout), lookout) == "seen from the point", "the point itself reads \"seen from the point\"")
	hud.plan_eye_pick.select(1)
	hud.plan_eye_pick.item_selected.emit(1)
	_push_key(KEY_F10)
	await _frames(2)
	check.call(plan_mode == 0 and int(ptm.get_shader_parameter("plan_mode")) == 0 and not hud.plan_panel.visible, "F10 after line of sight turns the planning map off")
	camera.position = cam_keep
	camera.zoom = zoom_was
	await _frames(2)

	# Order of battle panel: the HQ selected marks everything under it; rows select units.
	_refresh_oob()
	check.call(hud.oob_panel.visible and show_oob, "the order of battle panel is up by default")
	selected = [0]
	_refresh_oob()
	var company_ids: Array[int] = []
	for r in oob_rows:
		if r["alive"] and not r["militia"] and not r["embarked"] and int(r["id"]) != 0 and int(r["formation_id"]) == int(world.get_unit_info(0)["formation_id"]):
			company_ids.append(int(r["id"]))
	var all_under := company_ids.size() >= 5
	for id in company_ids:
		all_under = all_under and oob_under.has(id)
	check.call(all_under and not oob_under.has(0), "HQ selected: all %d units of the company under it are marked" % company_ids.size())
	check.call(hud.oob_items.has(0) and hud.oob_items.has(1) and (hud.oob_items[1] as TreeItem).get_custom_bg_color(0) == hud.OOB_SUB_BG
		and (hud.oob_items[0] as TreeItem).get_custom_bg_color(0) == hud.OOB_SEL_BG, "panel rows: the HQ amber, the units under it blue")
	check.call((hud.oob_items[1] as TreeItem).get_parent() == hud.oob_items[0], "a platoon sits under the HQ in the tree")
	var under_drawn := false
	for i in snapshot.size() / stride:
		if int(snapshot[i * stride]) == 1:
			under_drawn = overlay._row_item(i * stride, i).get("under", false)
	check.call(under_drawn, "the counters under the HQ carry the blue mark")
	# A real click on a row selects that unit (and a platoon commands nobody: no marks).
	var row2: TreeItem = hud.oob_items.get(2, null)
	var row_hit := false
	if row2 != null:
		hud.oob_tree.scroll_to_item(row2)
		await _frames(2)
		var rr := hud.oob_tree.get_item_area_rect(row2)
		_push_click(MOUSE_BUTTON_LEFT, hud.oob_tree.get_global_transform() * (rr.position + Vector2(40, rr.size.y * 0.5)))
		await _frames(2)
		row_hit = selected == [2]
	check.call(row_hit and oob_under.is_empty(), "clicking a row selects that unit")
	hud.oob_pick.emit(0, "command")
	await _frames(1)
	check.call(selected.size() == company_ids.size() + 1 and selected[0] == 0, "double-click on the HQ selects it and all %d under it" % company_ids.size())
	# A team: its leader marks its members only.
	var tm_ids := PackedInt32Array([1, 2, 4])
	var tg := world.create_group(tm_ids, 1)
	_refresh_oob()
	selected = [1]
	_update_oob_under()
	_refresh_oob()
	var on_map_under := 0 # 2nd Mech's squads ride aboard (under it, but not on the map)
	for u in oob_under:
		if disp_row.has(int(u)):
			on_map_under += 1
	check.call(tg >= 0 and on_map_under == 2 and oob_under.has(2) and oob_under.has(4)
		and (hud.oob_items[2] as TreeItem).get_parent() == hud.oob_items[1], "team leader selected: its members (under it in the tree) are marked")
	world.disband_group(tg)
	_push_key(KEY_F11)
	await _frames(1)
	check.call(not hud.oob_panel.visible, "F11 hides the order of battle")
	_push_key(KEY_F11)
	await _frames(1)
	check.call(hud.oob_panel.visible, "F11 shows it again")
	selected = [0]
	_refresh_oob()
	# Planning tools (MRK-26): the km grid, the ruler by Ctrl+drag with its travel times and
	# profile, markings placed with the keys and the right-click menu, renamed, saved, deleted.
	var cam_tools := camera.position
	camera.position = world.hex_to_world(Vector2i(900, 1330))
	camera.zoom = Vector2.ONE * 0.05
	selected = []
	await _frames(3)
	var tools_mb: MenuButton = hud.menu_buttons.get("Tools")
	check.call(tools_mb != null, "the top bar has a Tools menu")
	if tools_mb != null:
		hud._fill_tools(tools_mb.get_popup())
		var tool_items: Array[String] = []
		for i in tools_mb.get_popup().item_count:
			tool_items.append(tools_mb.get_popup().get_item_text(i))
		check.call("Ruler" in tool_items and "Phase line" in tool_items and "1 km grid and grid references" in tool_items,
			"Tools lists the ruler, the grid and the markings")
	_push_shift_key(KEY_G)
	await _frames(2)
	check.call(tools.show_km_grid and bool(hud.toggle_state.get("KmGrid", false)), "Shift+G turns the 1 km grid on")
	var gr: String = world.get_grid_ref(camera.position)
	var gre := RegEx.create_from_string("^\\d\\d/\\d\\d \\d\\d\\d \\d\\d\\d$")
	check.call(gre.search(gr) != null, "grid reference at the centre: GR %s" % gr)
	# Ruler: Ctrl+drag 300 px due east.
	var vc := get_viewport_rect().size * 0.5
	var p1 := vc + Vector2(-150, 40)
	var p2 := p1 + Vector2(300, 0)
	_push_ctrl_drag(p1, p2)
	await _frames(3)
	var want_m := 300.0 / camera.zoom.x
	var got_m := tools.ruler_a.distance_to(tools.ruler_b)
	check.call(tools.ruler_on and not tools.ruler_live and absf(got_m - want_m) < 2.0 and hud.ruler_panel.visible,
		"Ctrl+drag measures %.2f km (screen says %.2f km)" % [got_m / 1000.0, want_m / 1000.0])
	check.call(hud.ruler_line.text.begins_with("%.2f km" % (got_m / 1000.0)) and hud.ruler_line.text.contains("090° grid (1600 mils)"),
		"panel: \"%s\"" % hud.ruler_line.text)
	var e0: Dictionary = tools.est[0] if tools.est.size() == 3 else {}
	check.call(e0.get("found", false) and float(e0["minutes"]) > 0.0 and hud.ruler_rows[0].text.begins_with("Tracked"),
		"travel time by the rules' route search: \"%s\"" % (hud.ruler_rows[0].text if hud.ruler_rows.size() > 0 else ""))
	var e2: Dictionary = tools.est[2] if tools.est.size() == 3 else {}
	check.call(e2.get("found", false) and float(e2["minutes"]) > float(e0.get("minutes", 1.0e9)), "on foot takes longer than tracked")
	var prof_km: PackedFloat32Array = tools.profile.get("km", PackedFloat32Array())
	check.call(prof_km.size() > 10 and int(tools.profile.get("block", -3)) >= -2 and hud.ruler_chart.visible,
		"straight-line profile: %d hexes, sight %s" % [prof_km.size(), "clear" if int(tools.profile.get("block", -3)) == -1 else "blocked"])
	var a_hex := world.world_to_hex(tools.ruler_a)
	var b_hex := world.world_to_hex(tools.ruler_b)
	var vsh: Dictionary = world.get_viewshed(a_hex, tools.eye_m())
	if not vsh.is_empty() and a_hex.distance_to(b_hex) < 20:
		var vo: Vector2i = vsh["origin"]
		var vsz: Vector2i = vsh["size"]
		var vl := b_hex - vo
		var seen_b: bool = (vsh["mask"] as PackedByteArray)[vl.y * vsz.x + vl.x] == 2
		check.call(seen_b == (int(tools.profile["block"]) == -1), "the profile's sight line agrees with the line-of-sight mode")
	hud.ruler_src_route.pressed.emit()
	await _frames(2)
	var rkm: PackedFloat32Array = tools.profile.get("km", PackedFloat32Array())
	check.call(tools.profile_src == "route" and int(tools.profile.get("block", 0)) == -3 and rkm.size() >= 2 and
		absf(rkm[rkm.size() - 1] - float(e0.get("km", -1.0))) < 0.05, "profile along the tracked route (%.1f km)" % (rkm[rkm.size() - 1] if rkm.size() > 0 else -1.0))
	hud.ruler_mode_pick.select(1)
	hud.ruler_mode_pick.item_selected.emit(1)
	await _frames(2)
	check.call(tools.est_mode == 1 and tools.est.size() == 3, "the panel's mode picks road march for the estimate")
	_push_key(KEY_ESCAPE)
	await _frames(2)
	check.call(not tools.ruler_on and not hud.ruler_panel.visible and not hud.pause_screen.visible, "Esc takes the ruler off the map")
	# Markings.
	var marks_before := tools.marks.size()
	_push_shift_key(KEY_O)
	await _frames(1)
	_push_click(MOUSE_BUTTON_LEFT, vc + Vector2(-60, -40))
	await _frames(2)
	check.call(hud.ask_panel.visible and hud.ask_edit.text == "OBJ 1", "Shift+O and a click ask for the objective's name (\"%s\")" % hud.ask_edit.text)
	hud.ask_edit.text = "OBJ WOLF"
	hud.ask_edit.text_submitted.emit(hud.ask_edit.text)
	await _frames(2)
	_push_shift_key(KEY_T)
	await _frames(1)
	_push_click(MOUSE_BUTTON_LEFT, vc + Vector2(120, -80))
	await _frames(2)
	_push_shift_key(KEY_P)
	await _frames(1)
	for q in [Vector2(-200, 60), Vector2(-40, 90), Vector2(160, 70)]:
		_push_click(MOUSE_BUTTON_LEFT, vc + q)
		await _frames(1)
	_push_key(KEY_ENTER)
	await _frames(2)
	check.call(hud.ask_panel.visible and hud.ask_edit.text == "PL 1", "Enter finishes the phase line and asks its name")
	hud.ask_edit.text = "PL RED"
	hud._ask_done(true)
	await _frames(2)
	menu_world_pos = _screen_to_world(vc + Vector2(40, 140))
	_open_order_menu(vc + Vector2(40, 140))
	await _frames(2)
	var mark_sub := _submenu_of(hud.order_popup, "Mark here")
	check.call(mark_sub != null and _menu_pick(mark_sub, "Note"), "right-click > Mark here > Note")
	hud.order_popup.hide()
	await _frames(2)
	hud.ask_edit.text = "Bridge out"
	hud._ask_done(true)
	await _frames(3)
	var kinds: Array = []
	for mk in tools.marks:
		kinds.append(int(mk["kind"]))
	check.call(tools.marks.size() == marks_before + 4 and kinds.slice(marks_before) == [0, 2, 1, 3] and
		String(tools.marks[marks_before]["name"]) == "OBJ WOLF" and String(tools.marks[marks_before + 1]["name"]) == "TRP 01" and
		(tools.marks[marks_before + 2]["points"] as PackedVector2Array).size() == 3,
		"objective, TRP 01, a 3-point phase line and a note are on the map")
	# Right-click the objective: rename it.
	var obj_screen: Vector2 = get_viewport().get_canvas_transform() * (tools.marks[marks_before]["points"] as PackedVector2Array)[0]
	_open_order_menu(obj_screen)
	await _frames(2)
	check.call(_menu_pick(hud.order_popup, "Rename"), "right-click on the objective offers Rename")
	hud.order_popup.hide()
	await _frames(2)
	hud.ask_edit.text = "OBJ BEAR"
	hud._ask_done(true)
	await _frames(2)
	check.call(String(tools.marks[marks_before]["name"]) == "OBJ BEAR", "renamed to OBJ BEAR")
	# Saved with the game: save, clear, load.
	var blob: PackedByteArray = world.save_state()
	world.clear_markings()
	tools.refresh_marks()
	var lerr: String = world.load_state(blob)
	tools.refresh_marks()
	check.call(lerr == "" and tools.marks.size() == marks_before + 4, "markings come back with a load (%d)" % tools.marks.size())
	_open_order_menu(obj_screen)
	await _frames(2)
	check.call(_menu_pick(hud.order_popup, "Delete"), "right-click on the objective offers Delete")
	hud.order_popup.hide()
	await _frames(2)
	check.call(tools.marks.size() == marks_before + 3, "the objective is deleted")
	_push_shift_key(KEY_G)
	await _frames(1)
	check.call(not tools.show_km_grid, "Shift+G turns the grid off again")
	camera.position = cam_tools
	camera.zoom = zoom_was
	await _frames(2)

	# Last in the test because it moves the clock: 1st Mech dismounts; the squads take the
	# NATO symbol, the carriers keep their BMP.
	var units_before := snapshot.size() / stride
	var dismount_ok := false
	if world.order_dismount(3) >= 0:
		for k in 40:
			world.advance(1, 200.0)
		_refresh_snapshot()
		for i in snapshot.size() / stride:
			var uid := int(snapshot[i * stride])
			var di: Dictionary = world.get_unit_info(uid)
			if int(snapshot[i * stride + 2]) == 3 and String(di.get("equipment", "")).contains("(BMP-1)"):
				dismount_ok = overlay.vehicle_of(uid, 3) == "" and overlay.vehicle_of(3, 2) == "ifv"
	check.call(dismount_ok, "dismounted squads show the NATO symbol while their BMPs keep the vehicle (units %d -> %d)" % [units_before, snapshot.size() / stride])
	print("UITEST done: %d passed, %d failed" % [res[0], res[1]])
	get_tree().quit(1 if res[1] > 0 else 0)


func _frames(n: int) -> void:
	for i in n:
		await get_tree().process_frame


func _push_key(k: Key) -> void:
	for down in [true, false]:
		var ev := InputEventKey.new()
		ev.keycode = k
		ev.physical_keycode = k
		ev.pressed = down
		get_viewport().push_input(ev)


func _push_shift_key(k: Key) -> void:
	for down in [true, false]:
		var ev := InputEventKey.new()
		ev.keycode = k
		ev.physical_keycode = k
		ev.shift_pressed = true
		ev.pressed = down
		get_viewport().push_input(ev)


## Ctrl + left-button drag from a to b (the ruler).
func _push_ctrl_drag(a: Vector2, b: Vector2) -> void:
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.ctrl_pressed = true
	press.position = a
	press.global_position = a
	get_viewport().push_input(press)
	for t in [0.33, 0.66, 1.0]:
		var mv := InputEventMouseMotion.new()
		mv.position = a.lerp(b, t)
		mv.global_position = mv.position
		mv.ctrl_pressed = true
		get_viewport().push_input(mv)
	var rel := InputEventMouseButton.new()
	rel.button_index = MOUSE_BUTTON_LEFT
	rel.pressed = false
	rel.ctrl_pressed = true
	rel.position = b
	rel.global_position = b
	get_viewport().push_input(rel)


func _push_click(button: MouseButton, pos: Vector2) -> void:
	for down in [true, false]:
		var ev := InputEventMouseButton.new()
		ev.button_index = button
		ev.pressed = down
		ev.position = pos
		ev.global_position = pos
		get_viewport().push_input(ev)


## Picks the menu item labelled `text` as a click would; false if there is none or it is disabled.
func _menu_pick(pm: PopupMenu, text: String) -> bool:
	for i in pm.item_count:
		if pm.get_item_text(i) == text and not pm.is_item_disabled(i):
			pm.id_pressed.emit(pm.get_item_id(i))
			return true
	return false


func _submenu_of(pm: PopupMenu, prefix: String) -> PopupMenu:
	for i in pm.item_count:
		if pm.get_item_text(i).begins_with(prefix) and pm.get_item_submenu_node(i) != null:
			return pm.get_item_submenu_node(i)
	return null


# --- Automated check: scripted run that saves screenshots, then quits. ---------------------

## The highest hex of open ground or fields within `r` hexes: a good place to look from.
func _lookout_near(c: Vector2i, r: int) -> Vector2i:
	var best := c
	var best_e := -1.0e9
	for dy in range(-r, r + 1):
		for dx in range(-r, r + 1):
			var h := c + Vector2i(dx, dy)
			var hi: Dictionary = world.get_hex_info(h)
			if hi.is_empty() or not (String(hi["terrain"]) in ["Open ground", "Farmland"]):
				continue
			if float(hi["elevation"]) > best_e:
				best_e = float(hi["elevation"])
				best = h
	return best


## The hex of open ground or fields within `r` hexes that sees the most at 3 m: where a
## commander would put an observation post (screenshots and checks).
## Screenshots of the planning tools: the km grid, a ruler east-north-east from home with its
## routes and profile, and a few markings around it.
func _tools_demo(hq: Vector2) -> void:
	tools.show_km_grid = true
	world.clear_markings()
	world.add_marking(MapTools.MK_OBJECTIVE, "OBJ WOLF", PackedVector2Array([hq + Vector2(8600, -1300)]))
	world.add_marking(MapTools.MK_PHASE, "PL RED", PackedVector2Array([hq + Vector2(3200, -6000), hq + Vector2(3900, -2500),
		hq + Vector2(3400, 1000), hq + Vector2(4200, 4200)]))
	world.add_marking(MapTools.MK_TRP, "TRP 01", PackedVector2Array([hq + Vector2(6100, 400)]))
	world.add_marking(MapTools.MK_NOTE, "Bridge too weak for tanks?", PackedVector2Array([hq + Vector2(1500, -3200)]))
	tools.refresh_marks()
	tools.start_ruler(hq)
	tools.ruler_b = hq + Vector2(8200, -2900)
	tools.finish_ruler()


func _best_lookout(c: Vector2i, r: int) -> Vector2i:
	var best := c
	var best_seen := -1
	for dy in range(-r, r + 1, 2):
		for dx in range(-r, r + 1, 2):
			var h := c + Vector2i(dx, dy)
			var hi: Dictionary = world.get_hex_info(h)
			if hi.is_empty() or not (String(hi["terrain"]) in ["Open ground", "Farmland"]) or int(hi["cover"]) > 0:
				continue
			var seen := int(world.get_viewshed(h, 3.0).get("seen", 0))
			if seen > best_seen:
				best_seen = seen
				best = h
	return best


## --views=<dir>: renders a fixed set of map views without the HUD and quits. A quick way
## to check how the map looks after a change to the terrain shader or the line meshes.
## Extra options: --crisp (crisp hex borders), --flat (painted textures off), --topo.
func _run_views() -> void:
	hud.visible = false
	map_view.build_budget_ms = 1.0e9
	for a in OS.get_cmdline_user_args():
		if a == "--crisp":
			map_view.natural_edges = false
		elif a == "--flat":
			map_view.show_detail = false
		elif a == "--topo":
			map_view.show_topo = true
		elif a.begins_with("--plan="):
			plan_mode = a.substr(7).to_int()
		elif a.begins_with("--eye="):
			sight_eye_index = a.substr(6).to_int()
		elif a == "--tools":
			_tools_demo(world.hex_to_world(world.get_unit_info(0)["hex"]))
			hud.visible = true
	DirAccess.make_dir_recursive_absolute(views_dir)
	var hq: Vector2 = world.hex_to_world(world.get_unit_info(0)["hex"])
	var hills: Vector2 = world.hex_to_world(Vector2i(900, 1330))
	if plan_mode != 0:
		hud.visible = true   # the planning panel is part of what to check
		if plan_mode == MapView.PLAN_SIGHT:
			sight_from = _best_lookout(world.world_to_hex(hq), 14)
		set_plan_mode(plan_mode)
		print("Views: planning mode %d built in %.0f ms" % [plan_mode, map_view.plan_build_ms])
	var views := [["v1_hills_30px", hills, 0.06], ["v2_hills_10px", hills, 0.02], ["v3_home_25px", hq, 0.05],
		["v4_home_70px", hq + Vector2(3000, -1500), 0.14], ["v5_town_150px", hq + Vector2(900, 300), 0.3],
		["v6_region_3px", hills, 0.006], ["v7_hills_80px", hills + Vector2(-2500, 1200), 0.16]]
	selected = []
	for v in views:
		camera.position = v[1]
		camera.zoom = Vector2.ONE * float(v[2])
		_refresh_snapshot()
		for i in 3:
			map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
			overlay.queue_redraw()
			await get_tree().process_frame
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(views_dir.path_join(String(v[0]) + ".png"))
		print("Views: saved %s" % v[0])
	# The average colour of each painted ground type at ~10 m per pixel: the far-zoom
	# palette (base_color in core/src/render_data.cpp) should match these.
	var layer := CanvasLayer.new()
	layer.layer = 5
	add_child(layer)
	var rect := ColorRect.new()
	rect.position = Vector2.ZERO
	rect.size = Vector2(800, 800)
	var sm := ShaderMaterial.new()
	sm.shader = MapView.TERRAIN_SHADER
	sm.set_shader_parameter("swatch_size", 8000.0)
	sm.set_shader_parameter("metres_per_pixel", 10.0)
	rect.material = sm
	layer.add_child(rect)
	for code in [0, 1, 2, 3, 11, 4, 5, 13]:
		sm.set_shader_parameter("swatch", code)
		for i in 2:
			await get_tree().process_frame
		await RenderingServer.frame_post_draw
		var img := get_viewport().get_texture().get_image()
		var sum := Vector3.ZERO
		var n := 0
		for y in range(0, 800, 4):
			for x in range(0, 800, 4):
				var c := img.get_pixel(x, y)
				sum += Vector3(c.r, c.g, c.b)
				n += 1
		sum = sum / n * 255.0
		print("Views: average of terrain %d = %d, %d, %d" % [code, roundi(sum.x), roundi(sum.y), roundi(sum.z)])
	get_tree().quit()


func _run_autotest() -> void:
	print("Autotest: writing screenshots to ", autotest_dir)
	DirAccess.make_dir_recursive_absolute(autotest_dir)
	auto_pause = false
	_refresh_snapshot()
	var hq: Vector2 = world.hex_to_world(world.get_unit_info(0)["hex"])
	show_zones = false
	show_oob = false # the older shots stay as they were; the panel has shots of its own (42-43)
	hud.set_oob_visible(false)

	await _shot("01_company_close", hq, 0.07)
	# Menus: the title screen, the right-click order menu, the menu bar, a dropdown, the pause menu, controls.
	hud.show_title(_title_briefing())
	await _shot("20_title_screen", hq, 0.05)
	hud.hide_title()
	_select_company_of(0)
	await _shot("21_order_menu", hq, 0.05, func() -> void:
		_open_order_menu(get_viewport_rect().size * Vector2(0.36, 0.42)))
	hud.order_popup.hide()
	selected = [1]
	await _shot("22_view_menu", hq, 0.05, func() -> void:
		(hud.menu_buttons["View"] as MenuButton).show_popup())
	(hud.menu_buttons["View"] as MenuButton).get_popup().hide()
	await _shot("23_standoff_dropdown", hq, 0.05, func() -> void:
		hud.standoff_opt.show_popup())
	hud.standoff_opt.get_popup().hide()
	_open_pause()
	await _shot("24_pause_menu", hq, 0.05)
	_close_pause()
	hud.toggle_help()
	await _shot("25_controls", hq, 0.05)
	hud.toggle_help()
	# Map visuals (MRK-24, MRK-29): the painted ground close in and further out, the legend.
	hud.toggle_legend()
	await _shot("28_map_legend", hq, 0.08)
	hud.toggle_legend()
	selected = []
	# Topographic mode (MRK-23): hill country south of Wroclaw close in and further out, then home.
	map_view.show_topo = true
	var hills: Vector2 = world.hex_to_world(Vector2i(900, 1330))
	await _shot("33_topo_close", hills, 0.06)
	await _shot("34_topo_far", hills, 0.02)
	await _shot("36_topo_region", hills, 0.006)
	await _shot("35_topo_company", hq, 0.05)
	map_view.show_topo = false
	await _shot("31_counters_vehicles", hq + Vector2(900, 300), 0.24)
	overlay.show_vehicles = false
	await _shot("32_counters_nato", hq + Vector2(900, 300), 0.24)
	overlay.show_vehicles = true
	await _shot("29_terrain_close", hq + Vector2(3000, -1500), 0.16)
	await _shot("30_terrain_medium", hq, 0.045)
	map_view.natural_edges = false
	await _shot("37_terrain_crisp", hq + Vector2(3000, -1500), 0.16)
	map_view.natural_edges = true
	# Planning map (MRK-25): going for tracks, cover, slope in the hills, line of sight from home.
	set_plan_mode(1)
	await _shot("38_plan_going", hq, 0.05)
	set_plan_mode(MapView.PLAN_COVER)
	await _shot("39_plan_cover", hq + Vector2(900, 300), 0.14)
	set_plan_mode(MapView.PLAN_SLOPE)
	await _shot("40_plan_slope", hills, 0.03)
	sight_from = _best_lookout(world.world_to_hex(hq), 14)
	set_plan_mode(MapView.PLAN_SIGHT)
	await _shot("41_plan_sight", world.hex_to_world(sight_from), 0.025)
	set_plan_mode(0)
	# Planning tools (MRK-26): the 1 km grid, a ruler with its route and profile, and markings.
	_tools_demo(hq)
	await _shot("42_ruler_profile", hq + Vector2(4500, -1000), 0.06)
	tools.clear_ruler()
	await _shot("43_markings_grid", hq + Vector2(2500, -500), 0.1)
	tools.show_km_grid = false
	world.clear_markings()
	tools.refresh_marks()
	selected = [0]
	selected = [0]
	# Holdings: the home town's panel, then a supply route from its stores to the trains.
	holdings = world.get_holdings()
	var home_hd := -1
	for hd in holdings:
		if hd.get("own", false) and int(hd["kind"]) == 0:
			home_hd = int(hd["id"])
	if home_hd >= 0:
		selected = []
		selected_holding = home_hd
		world.set_training(home_hd, true)
		await _shot("26_holding_panel", world.get_holding(home_hd)["pos"], 0.05)
		world.set_supply_route(home_hd, -1)
		show_supply = true
		for k in 50:
			world.advance(1, 200.0)
		routes = world.get_supply_routes()
		await _shot("27_supply_route", world.get_holding(home_hd)["pos"], 0.05)
		show_supply = false
		world.cancel_supply_route(0)
		selected_holding = -1
		selected = [0]

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
		selected.clear()
		await _shot("16_direct_command", unit_position(0), 0.035)
		show_cmd = false
		show_fs = true
		await _shot("17_fire_support", unit_position(0), 0.02)
		show_cmd = true
		await _shot("18_both_overlays", unit_position(0), 0.035)
		show_cmd = false
		show_fs = false
		# Mounted infantry: 1st Mech's squads get out next to their BMPs.
		if world.order_dismount(3) >= 0:
			for k in 40:
				world.advance(1, 200.0)
			selected = [3]
			await _shot("19_dismounted", unit_position(3), 0.06)
			selected.clear()
		# Order of battle panel: the HQ selected, then the team leader.
		show_oob = true
		hud.set_oob_visible(true)
		selected = [0]
		await _shot("44_oob_hq", unit_position(0) + Vector2(-3000, 0), 0.04)
		selected = [pick[0]]
		await _shot("45_oob_team_leader", unit_position(pick[0]) + Vector2(-2000, 0), 0.05)
		show_oob = false
		hud.set_oob_visible(false)
		selected.clear()
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


## `before_capture`, if given, runs once the view has settled (e.g. to open a menu).
func _shot(name: String, pos: Vector2, zoom: float, before_capture := Callable()) -> void:
	camera.position = pos
	camera.zoom = Vector2.ONE * zoom
	_refresh_snapshot()
	for i in 40: # let chunks stream in
		map_view.update_view(camera.get_screen_center_position(), camera.zoom.x, get_viewport_rect().size)
		overlay.queue_redraw()
		hud.refresh(world.get_time_string(), world.is_night(), world.get_stats(), Engine.get_frames_per_second(), "chunks %d" % map_view.chunks.size())
		_refresh_selection_panel()
		_refresh_oob()
		await get_tree().process_frame
	if before_capture.is_valid():
		before_capture.call()
		for i in 4:
			await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png(autotest_dir.path_join(name + ".png"))
	print("Autotest: saved %s  chunks loaded %d (built %d)  overlay draw %.2f ms" % [name, map_view.chunks.size(),
		map_view.total_built, overlay.draw_ms])
