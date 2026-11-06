class_name Hud
extends Control
## Interface built in code: top bar (clock, speed, stocks, toggles),
## unit panel, event log, hover status line, help and banners.

signal speed_pressed(index: int)
signal toggle_pressed(name: String)
signal hold_pressed
signal command(name: String)        # order buttons in the unit / company panel
signal event_clicked(pos: Vector2)
signal end_choice(choice: String)    # end screen: continue / load / restart / quit

var main: Node
var time_label: Label
var speed_buttons: Array[Button] = []
var cap_label: Label
var stock_label: Label
var perf_label: Label
var toggle_buttons := {}
var unit_panel: PanelContainer
var unit_text: RichTextLabel
var hold_button: Button
var orders_box: VBoxContainer
var mode_buttons: Array[Button] = []
var roe_buttons: Array[Button] = []
var brk_buttons: Array[Button] = []
var orders_hint: Label
var team_buttons: Array[Button] = []    # form, disband, select
var shape_buttons: Array[Button] = []
var spread_buttons: Array[Button] = []
var standoff_buttons: Array[Button] = []
var default_buttons: Array[Button] = []
# Stand-off choices: [label, value]; 255 = company default, 254 = the unit's weapon range, else hexes.
const STANDOFFS := [["Default", 255], ["Fight", 0], ["1 km", 2], ["2 km", 4], ["3 km", 6], ["Wpn", 254]]
const TEAM_COLORS := [Color("#f2c14e"), Color("#39c0b0"), Color("#c77dff"), Color("#8bd450"),
	Color("#ff8c42"), Color("#5ab0ff"), Color("#ff6f91"), Color("#d9d9d9")]
const SPREADS := [1, 2, 4] # hexes between formation slots: 500 m, 1 km, 2 km
var log_box: VBoxContainer
var status_label: Label
var help_panel: PanelContainer
var credits_panel: PanelContainer
var banner: Label
var banner_until := 0
var toast: Label
var toast_until := 0
var loading: Label
var objective_label: RichTextLabel
var end_panel: PanelContainer
var end_title: Label
var end_text: RichTextLabel
var end_load_button: Button


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build_top_bar()
	_build_unit_panel()
	_build_log()
	_build_status_and_help()
	_build_banners()
	_build_end_screen()


func _panel_style(alpha := 0.86) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.07, 0.08, 0.07, alpha)
	sb.border_color = Color(0.45, 0.5, 0.4, 0.6)
	sb.set_border_width_all(1)
	sb.set_corner_radius_all(4)
	sb.content_margin_left = 10
	sb.content_margin_right = 10
	sb.content_margin_top = 6
	sb.content_margin_bottom = 6
	return sb


func _offsets(c: Control, l: float, t: float, r: float, b: float) -> void:
	c.offset_left = l
	c.offset_top = t
	c.offset_right = r
	c.offset_bottom = b


func _label(text: String, size := 15, color := Color(0.92, 0.92, 0.86)) -> Label:
	var l := Label.new()
	l.text = text
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_color", color)
	return l


func _button(text: String, toggle := false) -> Button:
	var b := Button.new()
	b.text = text
	b.toggle_mode = toggle
	b.focus_mode = Control.FOCUS_NONE
	b.add_theme_font_size_override("font_size", 14)
	return b


func _build_top_bar() -> void:
	var bar := PanelContainer.new()
	bar.add_theme_stylebox_override("panel", _panel_style(0.92))
	bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	add_child(bar)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	bar.add_child(row)

	time_label = _label("--", 18, Color(1, 0.95, 0.8))
	time_label.custom_minimum_size.x = 210
	row.add_child(time_label)

	var names := ["Pause", "1 min", "5 min", "30 min", "2 h", "Max"]  # game time per real second
	for i in names.size():
		var b := _button(names[i], true)
		b.tooltip_text = ("Key Space" if i == 0 else "Game time per real second. Key %d" % i)
		b.pressed.connect(func() -> void: speed_pressed.emit(i))
		row.add_child(b)
		speed_buttons.append(b)
	cap_label = _label("", 13, Color(1, 0.55, 0.4))
	cap_label.custom_minimum_size.x = 150
	row.add_child(cap_label)

	row.add_child(VSeparator.new())
	stock_label = _label("", 14)
	row.add_child(stock_label)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(spacer)

	var toggles := [["Group", "O"], ["Direct command", "T"], ["Fire support", "I"], ["Grid", "G"], ["Sight", "V"], ["Zones", "Z"], ["WeGo", "P"], ["Auto-pause", "U"]]
	for t in toggles:
		var b := _button(t[0], true)
		b.tooltip_text = "Key %s" % t[1]
		if t[0] == "Direct command":
			b.tooltip_text = "Key T. Outline and link every unit to the HQ or team leader it takes orders from: solid inside the zone, amber outside it (orders ×1.5), red by courier."
		elif t[0] == "Fire support":
			b.tooltip_text = "Key I. The fire support radio net: guns with their range rings, observers on the net, who relays whose calls for fire, and who is cut off."
		var key: String = t[0]
		b.pressed.connect(func() -> void: toggle_pressed.emit(key))
		row.add_child(b)
		toggle_buttons[key] = b
	perf_label = _label("", 11, Color(0.7, 0.75, 0.7))
	perf_label.custom_minimum_size.x = 210
	row.add_child(perf_label)


func _build_unit_panel() -> void:
	unit_panel = PanelContainer.new()
	unit_panel.add_theme_stylebox_override("panel", _panel_style())
	unit_panel.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_offsets(unit_panel, -372, 60, -8, 60)
	unit_panel.custom_minimum_size = Vector2(364, 0)
	unit_panel.visible = false
	add_child(unit_panel)
	var col := VBoxContainer.new()
	unit_panel.add_child(col)
	unit_text = RichTextLabel.new()
	unit_text.bbcode_enabled = true
	unit_text.fit_content = true
	unit_text.scroll_active = false
	unit_text.custom_minimum_size = Vector2(310, 0)
	unit_text.add_theme_font_size_override("normal_font_size", 14)
	unit_text.add_theme_font_size_override("bold_font_size", 16)
	unit_text.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(unit_text)
	_build_orders(col)


## Command panel: orders for everything selected (one unit or a whole company).
## All of them travel down the chain of command and arrive after the command delay.
func _build_orders(parent: Control) -> void:
	orders_box = VBoxContainer.new()
	orders_box.add_theme_constant_override("separation", 4)
	parent.add_child(orders_box)
	orders_box.add_child(HSeparator.new())
	orders_box.add_child(_label("Orders", 14, Color(1, 0.92, 0.7)))

	# Move mode: what a right-click on the map will order.
	var moves := GridContainer.new()
	moves.columns = 4
	moves.add_theme_constant_override("h_separation", 4)
	orders_box.add_child(moves)
	var move_defs := [["Move", "F", "Hasty move: normal speed, shoots on the move at a penalty."],
		["Road march", "Q", "Fast on roads, slow off them. Shoots badly; a bunched column is easy to hit."],
		["Deliberate", "E", "Slow (x1.5 time). Stays concealed in cover and shoots as if halted."],
		["Assault", "R", "Slightly slower. Presses the attack: breaks contact only at heavier losses."]]
	for i in move_defs.size():
		var d: Array = move_defs[i]
		var b := _small_button(d[0], true, "%s Then right-click the map. Key %s" % [d[2], d[1]])
		b.pressed.connect(func() -> void: command.emit("mode_%d" % i))
		moves.add_child(b)
		mode_buttons.append(b)

	var acts := GridContainer.new()
	acts.columns = 4
	acts.add_theme_constant_override("h_separation", 4)
	orders_box.add_child(acts)
	var act_defs := [["Hold", "hold", "Stop and hold position. Key H"],
		["Fall back", "fall_back", "Withdraw about 6 km away from known enemies, or back to the HQ. Key B"],
		["Regroup", "regroup", "Form up around the company HQ (or whoever took over from it). Key Y"],
		["Resupply", "rest", "Rest & resupply at the HQ (or whoever took over from it): double refill rate, faster recovery. Key N"],
		["Dismount", "dismount", "APC/IFV platoons: the vehicles halt and the squads get out (one per vehicle), to move and fight on foot. Key , (comma)"],
		["Mount up", "mount", "The squads walk back to their vehicles, which halt and wait, and mount up (2 min once together). Order the vehicles or the squads. Key , (comma)"]]
	for d in act_defs:
		var name: String = d[1]
		var b := _small_button(d[0], false, d[2])
		b.pressed.connect(func() -> void: command.emit(name))
		acts.add_child(b)
	hold_button = acts.get_child(0) as Button

	var roe_row := HBoxContainer.new()
	orders_box.add_child(roe_row)
	var roe_l := _label("Fire", 12, Color(0.75, 0.8, 0.7))
	roe_l.custom_minimum_size.x = 82
	roe_row.add_child(roe_l)
	var roe_defs := [["At will", "Engage anything in range and sight."],
		["Return fire", "Only shoot back when fired upon. Good for scouts and ambushes."],
		["Hold fire", "Never fire. Stays hidden; will not defend itself."]]
	for i in roe_defs.size():
		var b := _small_button(roe_defs[i][0], true, roe_defs[i][1] + " Key X cycles.")
		b.pressed.connect(func() -> void: command.emit("roe_%d" % i))
		roe_row.add_child(b)
		roe_buttons.append(b)

	var brk_row := HBoxContainer.new()
	orders_box.add_child(brk_row)
	var brk_l := _label("Break contact", 12, Color(0.75, 0.8, 0.7))
	brk_l.custom_minimum_size.x = 82
	brk_row.add_child(brk_l)
	var brk_defs := [["Heavy loss", "Fall back on its own below 30% strength or morale 25."],
		["Light loss", "Fall back early: below 60% strength or morale 45. Preserves the company."],
		["Never", "Hold on until morale collapses (below 8)."]]
	for i in brk_defs.size():
		var b := _small_button(brk_defs[i][0], true, brk_defs[i][1])
		b.pressed.connect(func() -> void: command.emit("brk_%d" % i))
		brk_row.add_child(b)
		brk_buttons.append(b)

	# Teams: lock units under a leader, who commands them within its zone; members keep formation.
	var team_row := HBoxContainer.new()
	orders_box.add_child(team_row)
	var team_l := _label("Team", 12, Color(0.75, 0.8, 0.7))
	team_l.custom_minimum_size.x = 82
	team_row.add_child(team_l)
	var team_defs := [["Form team", "team_form", "Lock the selected units under one leader: the HQ if it is selected, else the first unit you selected. Key J"],
		["Disband", "team_disband", "Release the selected units' teams. Shift+J"],
		["Select", "team_select", "Select the whole team of the selected unit. Key L (or double-click a team member)"]]
	for d in team_defs:
		var cmd: String = d[1]
		var b := _small_button(d[0], false, d[2])
		b.pressed.connect(func() -> void: command.emit(cmd))
		team_row.add_child(b)
		team_buttons.append(b)

	var shape_row := HBoxContainer.new()
	orders_box.add_child(shape_row)
	var shape_l := _label("Formation", 12, Color(0.75, 0.8, 0.7))
	shape_l.custom_minimum_size.x = 82
	shape_row.add_child(shape_l)
	var shape_defs := [["Keep", "Hold the team's present layout around its leader."],
		["Column", "One behind the other along the line of march."],
		["Line", "Abreast of the leader, across the line of march."],
		["Wedge", "A V behind the leader: the leader at the point."]]
	for i in shape_defs.size():
		var b := _small_button(shape_defs[i][0], true, shape_defs[i][1] + " Members keep station on the leader; the team moves at its slowest member's pace.")
		b.pressed.connect(func() -> void: command.emit("shape_%d" % i))
		shape_row.add_child(b)
		shape_buttons.append(b)

	var spread_row := HBoxContainer.new()
	orders_box.add_child(spread_row)
	var spread_l := _label("Spread", 12, Color(0.75, 0.8, 0.7))
	spread_l.custom_minimum_size.x = 82
	spread_row.add_child(spread_l)
	var spread_names := ["500 m", "1 km", "2 km"]
	for i in SPREADS.size():
		var sp: int = SPREADS[i]
		var b := _small_button(spread_names[i], true, "Distance between formation places. 500 m is the doctrinal platoon spacing (300–600 m); 2 km is wider than one company's 1 km command zone.")
		b.pressed.connect(func() -> void: command.emit("spread_%d" % sp))
		spread_row.add_child(b)
		spread_buttons.append(b)

	# Stand-off: stay and fight, or keep known enemies at a distance (halt short of it, pull
	# back to it). Per unit through the command delay; "Default" follows the company default.
	var so_row := HBoxContainer.new()
	orders_box.add_child(so_row)
	var so_l := _label("Stand-off", 12, Color(0.75, 0.8, 0.7))
	so_l.custom_minimum_size.x = 66
	so_row.add_child(so_l)
	for i in STANDOFFS.size():
		var v: int = STANDOFFS[i][1]
		var tip := "Use the company default." if v == 255 else ("Stay and fight: close with the enemy, no stand-off." if v == 0 else
			("Keep known enemies at this unit's own weapon range." if v == 254 else "Halt %s short of known enemies and pull back out if they come closer." % STANDOFFS[i][0]))
		var b := _small_button(STANDOFFS[i][0], true, tip + " An assault ignores stand-off; team members go by their leader's.")
		b.pressed.connect(func() -> void: command.emit("standoff_%d" % v))
		so_row.add_child(b)
		standoff_buttons.append(b)
	var def_row := HBoxContainer.new()
	orders_box.add_child(def_row)
	var def_l := _label("Co. default", 12, Color(0.75, 0.8, 0.7))
	def_l.custom_minimum_size.x = 66
	def_row.add_child(def_l)
	for i in range(1, STANDOFFS.size()):
		var v: int = STANDOFFS[i][1]
		var b := _small_button(STANDOFFS[i][0], true, "Company standing order for every unit set to Default.")
		b.pressed.connect(func() -> void: command.emit("default_standoff_%d" % v))
		def_row.add_child(b)
		default_buttons.append(b)

	orders_hint = _label("", 12, Color(0.6, 0.85, 1.0))
	orders_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	orders_hint.custom_minimum_size.x = 340
	orders_box.add_child(orders_hint)


func _small_button(text: String, toggle: bool, tip: String) -> Button:
	var b := _button(text, toggle)
	b.add_theme_font_size_override("font_size", 12)
	b.tooltip_text = tip
	b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return b


## Highlights the armed move mode and the selection's fire / break-contact
## settings (-1 = mixed across the selection). `pending` marks settings still on their way.
func set_orders_state(mode: int, roe: int, brk: int, pending: bool, delay_text: String) -> void:
	for i in mode_buttons.size():
		mode_buttons[i].set_pressed_no_signal(i == mode)
	for i in roe_buttons.size():
		roe_buttons[i].set_pressed_no_signal(i == roe)
	for i in brk_buttons.size():
		brk_buttons[i].set_pressed_no_signal(i == brk)
	var names := ["a hasty move", "a road march", "a deliberate move", "an assault"]
	orders_hint.text = "Right-click the map for %s. Orders arrive in %s.%s" % [
		names[mode], delay_text, "  (Fire / break-contact change on its way)" if pending else ""]


## Team controls: the selection's team formation (shape -1 when none or mixed).
func set_team_state(in_team: bool, can_form: bool, shape: int, spacing: int) -> void:
	team_buttons[0].disabled = not can_form
	team_buttons[1].disabled = not in_team
	team_buttons[2].disabled = not in_team
	for i in shape_buttons.size():
		shape_buttons[i].disabled = not in_team
		shape_buttons[i].set_pressed_no_signal(in_team and i == shape)
	for i in spread_buttons.size():
		spread_buttons[i].disabled = not in_team
		spread_buttons[i].set_pressed_no_signal(in_team and SPREADS[i] == spacing)


## Stand-off buttons: the selection's own setting (-1 mixed) and the company default.
func set_standoff_state(setting: int, default_value: int) -> void:
	for i in standoff_buttons.size():
		standoff_buttons[i].set_pressed_no_signal(STANDOFFS[i][1] == setting)
	for i in default_buttons.size():
		default_buttons[i].set_pressed_no_signal(STANDOFFS[i + 1][1] == default_value)


func _standoff_text(info: Dictionary) -> String:
	var hx := int(info.get("standoff_hexes", 0))
	var setting := int(info.get("standoff_setting", 255))
	var what := "stays and fights" if hx <= 0 else ("keeps %.1f km from known enemies" % (hx * 0.5))
	var src := " (company default)" if setting == 255 else (" (weapon range)" if setting == 254 else "")
	var pend := "  [color=#ffd84a](change on its way)[/color]" if info.get("standoff_pending", false) else ""
	return "Stand-off: %s%s%s\n" % [what, src, pend]


func _team_line(info: Dictionary) -> String:
	var t := ""
	if int(info.get("group_id", -1)) >= 0:
		var col: Color = TEAM_COLORS[int(info["group_color"]) % TEAM_COLORS.size()]
		t += "[color=#%s]■ %s[/color]  %s · %s, %.1f km spread\n" % [col.to_html(false), info["group_name"],
			info["group_role"], String(info["group_shape_name"]).to_lower(), float(info["group_spacing_km"])]
	if info.has("own_zone_km"):
		t += "Commands within %.1f km\n" % float(info["own_zone_km"])
	if info.has("hq_name"):
		var inz: bool = info["in_zone"]
		t += "Answers to %s, %.1f km away: [color=%s]%s[/color]\n" % [info["hq_name"], float(info["hq_dist_km"]),
			"#9fc8ff" if inz else "#ffc040", ("inside its %.1f km zone" % float(info["hq_zone_km"])) if inz else ("outside its %.1f km zone, orders ×1.5" % float(info["hq_zone_km"]))]
		if int(info.get("command_link", 1)) == 3:
			t += "[color=#ff6b4a]Beyond radio range (%.0f km): orders go by courier[/color]\n" % float(info.get("radio_km", 20.0))
	elif int(info.get("command_link", -1)) == 0:
		t += "[color=#ff6b4a]No HQ to take orders from: orders improvised (+25 min)[/color]\n"
	return t


## Mounted infantry: squads aboard an APC/IFV platoon, or squads on foot and their vehicles.
func _mounted_line(info: Dictionary) -> String:
	var mi := "[color=#c8f0a0]Squads[/color]  "
	if info.has("squads_max"):
		if int(info.get("squads_aboard", 0)) > 0:
			return mi + "%d aboard (%d men each) · Dismount: , key\n" % [int(info["squads_aboard"]), int(info["squad_men"])]
		if info.get("squads_out", false):
			var walking := " · walking back to mount up" if info.get("squads_mounting", false) else ""
			return mi + "%d on foot as %s, %.1f km away%s\n" % [int(info["squads_count"]), info["squads_name"], float(info["squads_km"]), walking]
		return mi + "[color=#ff6b4a]all lost[/color]\n"
	if info.has("carrier_name"):
		if not info.get("carrier_alive", false):
			return mi + "on foot: their vehicles (%s) are lost\n" % info["carrier_name"]
		if info.get("mounting", false):
			return mi + "walking back to %s, %.1f km, to mount up\n" % [info["carrier_name"], float(info["carrier_km"])]
		var fits := "" if info.get("can_mount", false) else "  [color=#ff6b4a](too few vehicles left to mount)[/color]"
		return mi + "dismounted from %s (%d vehicles), %.1f km away%s\n" % [info["carrier_name"], int(info["carrier_vehicles"]), float(info["carrier_km"]), fits]
	return ""


## Fire support net (its own radio band) for one unit.
func _fire_support_line(info: Dictionary) -> String:
	var role := int(info.get("fs_role", -1))
	if role < 0:
		return ""
	var fs := "[color=#ff6f3c]Fire support[/color]  "
	var status := int(info.get("fs_status", 0))
	if role == 0:
		return fs + "guns: %d observers on the net, %d relayed, %d cut off\n" % [int(info.get("fs_direct_count", 0)),
			int(info.get("fs_relay_count", 0)), int(info.get("fs_cut_count", 0))]
	if status == 2:
		return fs + "observer on the net, %s %.1f km away\n" % [info.get("fs_battery", "guns"), float(info.get("fs_battery_km", 0.0))]
	if status == 1:
		return fs + "calls fire through %s (%.1f km), +%d min\n" % [info.get("fs_via", "?"), float(info.get("fs_via_km", 0.0)), int(info.get("fs_relay_min", 2))]
	return fs + "[color=#ff6b4a]cut off[/color]: %s %.1f km away, nobody on the net within radio range (%.0f km)\n" % [
		info.get("fs_battery", "guns"), float(info.get("fs_battery_km", 0.0)), float(info.get("radio_km", 20.0))]


func _build_log() -> void:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _panel_style(0.8))
	panel.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_offsets(panel, 8, -244, 640, -8)
	panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(panel)
	var col := VBoxContainer.new()
	panel.add_child(col)
	col.add_child(_label("Reports  (click to centre the map)        F1: all controls", 13, Color(0.75, 0.8, 0.7)))
	log_box = VBoxContainer.new()
	log_box.add_theme_constant_override("separation", 0)
	col.add_child(log_box)


func _build_status_and_help() -> void:
	var sp := PanelContainer.new()
	sp.add_theme_stylebox_override("panel", _panel_style(0.75))
	sp.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_offsets(sp, 8, 104, 8, 104)
	add_child(sp)
	var op := PanelContainer.new()
	op.add_theme_stylebox_override("panel", _panel_style(0.8))
	op.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_offsets(op, 8, 52, 8, 52)
	op.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(op)
	objective_label = RichTextLabel.new()
	objective_label.bbcode_enabled = true
	objective_label.fit_content = true
	objective_label.autowrap_mode = TextServer.AUTOWRAP_OFF
	objective_label.scroll_active = false
	objective_label.custom_minimum_size = Vector2(560, 0)
	objective_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	objective_label.add_theme_font_size_override("normal_font_size", 13)
	objective_label.add_theme_font_size_override("bold_font_size", 13)
	op.add_child(objective_label)

	status_label = _label("", 13, Color(0.9, 0.9, 0.85))
	sp.add_child(status_label)
	sp.visible = false

	help_panel = PanelContainer.new()
	help_panel.add_theme_stylebox_override("panel", _panel_style(0.8))
	help_panel.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	_offsets(help_panel, -330, -230, -8, -8)
	help_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	help_panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	help_panel.visible = false  # F1
	add_child(help_panel)
	help_panel.add_child(_label(
		"Left click: select unit (Shift adds)\n" +
		"Left drag / WASD: pan    Wheel: zoom\n" +
		"Right click: move order to hex\n" +
		"H: hold    Tab: next unit    C: centre\n" +
		"Double-click / K: whole company   O: grouping\n" +
		"F/Q/E/R: move/road/deliberate/assault\n" +
		"B: fall back  Y: regroup  N: resupply  X: fire\n" +
		"Space: pause    1-5: speed    .: +15 min\n" +
		"M: whole map    G grid  V sight  Z zones\n" +
		"T: direct command    I: fire support net\n" +
		", (comma): dismount / mount up\n" +
		"P: WeGo pulse    U: auto-pause    F1: help\n" +
		"F5: quick save    F9: quick load\n" +
		"F2: map data credits", 13, Color(0.85, 0.88, 0.8)))

	credits_panel = PanelContainer.new()
	credits_panel.add_theme_stylebox_override("panel", _panel_style(0.92))
	credits_panel.set_anchors_preset(Control.PRESET_CENTER)
	_offsets(credits_panel, -380, -150, 380, 150)
	credits_panel.visible = false
	add_child(credits_panel)
	credits_panel.add_child(_label(
		"Map data credits (F2 to close)\n\n" +
		"Elevation: Copernicus DEM GLO-90. © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH\n" +
		"  2014-2018, provided under COPERNICUS by the European Union and ESA; all rights reserved.\n" +
		"  Produced using Copernicus WorldDEM-90 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018.\n" +
		"Land cover: © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021)\n" +
		"  processed by ESA WorldCover consortium. CC BY 4.0.\n" +
		"Rivers, roads, railways: Made with Natural Earth. Free vector and raster map data @ naturalearthdata.com.\n" +
		"Place names and populations: GeoNames (geonames.org), CC BY 4.0. Some names changed to 1989 usage.\n" +
		"Local roads are generated; borders, roads and rail are not yet corrected to 1989.", 13, Color(0.9, 0.9, 0.85)))


func _build_banners() -> void:
	banner = _label("", 17, Color(1, 0.85, 0.5))
	banner.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_offsets(banner, -450, 100, 450, 130)
	banner.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	banner.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART # long briefings wrap instead of running under the panels
	banner.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.9))
	banner.add_theme_constant_override("outline_size", 6)
	add_child(banner)

	toast = _label("", 15, Color(0.85, 0.95, 1.0))
	toast.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_offsets(toast, -450, 186, 450, 212)
	toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	toast.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.9))
	toast.add_theme_constant_override("outline_size", 5)
	add_child(toast)

	loading = _label("Murks 1989\nLoading the map...", 26, Color(1, 0.95, 0.8))
	loading.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	loading.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	loading.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	add_child(loading)


# --- Updates -------------------------------------------------------------------------

func show_banner(text: String, seconds := 6.0) -> void:
	banner.text = text
	banner_until = Time.get_ticks_msec() + int(seconds * 1000)


func show_toast(text: String, seconds := 4.0) -> void:
	toast.text = text
	toast_until = Time.get_ticks_msec() + int(seconds * 1000)


func set_speed(index: int, capped: bool) -> void:
	for i in speed_buttons.size():
		speed_buttons[i].set_pressed_no_signal(i == index)
	cap_label.text = "Capped: company in contact" if capped else ""


func set_toggle(name: String, on: bool) -> void:
	if toggle_buttons.has(name):
		(toggle_buttons[name] as Button).set_pressed_no_signal(on)


func refresh(time_text: String, night: bool, stats: Dictionary, fps: float, map_stats: String) -> void:
	time_label.text = time_text + ("  night" if night else "")
	stock_label.text = "Diesel %s L   Ammo %.1f t   Food %s kg   Men %d" % [
		_thousands(int(stats["diesel_l"])), float(stats["ammo_t"]), _thousands(int(stats["food_kg"])), int(stats["personnel"])]
	perf_label.text = "%d fps · sim %.0f us/min · %d units\nhot %d / warm %d / cold %d · %s" % [
		int(fps), float(stats["avg_tick_us"]), int(stats["alive"]), int(stats["hot"]), int(stats["warm"]), int(stats["cold"]), map_stats]
	var now := Time.get_ticks_msec()
	if now > banner_until: banner.text = ""
	if now > toast_until: toast.text = ""


func set_status(text: String) -> void:
	status_label.text = text
	status_label.get_parent().visible = text != ""


func set_events(events: Array) -> void:
	for c in log_box.get_children():
		c.queue_free()
	for e in events:
		var b := _button("%s   %s" % [String(e["time_string"]).substr(11), e["text"]])
		b.flat = true
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.add_theme_font_size_override("font_size", 13)
		var kind: String = e["kind"]
		var col := Color(0.88, 0.9, 0.84)
		if kind == "contact" or kind == "under_fire": col = Color(1.0, 0.7, 0.45)
		elif kind == "destroyed": col = Color(1.0, 0.45, 0.4)
		elif kind == "retreat" or kind == "supply": col = Color(1.0, 0.85, 0.4)
		elif kind == "order" or kind == "arrived": col = Color(0.7, 0.85, 1.0)
		b.add_theme_color_override("font_color", col)
		if e.has("pos"):
			var p: Vector2 = e["pos"]
			b.pressed.connect(func() -> void: event_clicked.emit(p))
		log_box.add_child(b)


func show_unit(info: Dictionary, count: int) -> void:
	if info.is_empty():
		unit_panel.visible = false
		return
	unit_panel.visible = true
	var t := ""
	if int(info["side"]) != 0:
		orders_box.visible = false
		t += "[b]%s[/b]\n" % info["name"]
		if info.get("seen", false):
			t += "In sight now\n"
		else:
			t += "Last seen %d min ago\n" % int(info.get("last_seen_minutes_ago", 0))
		t += "%s\nStrength unknown." % info.get("location", "")
		unit_text.text = t
		return
	orders_box.visible = true
	t += "[b]%s[/b]   %s\n" % [info["name"], info["type_name"]]
	if count > 1:
		t += "[color=#9fc8ff](+%d more selected)[/color]\n" % (count - 1)
	t += "[color=#b8c0a8]%s · %s[/color]\n" % [info["formation"], info["equipment"]]
	t += "%s\n\n" % info["location"]
	if info.has("elements_max"):
		# Combat effectiveness: each element is in the fight or knocked out.
		var em := int(info["elements_max"])
		var ea := int(info["elements_active"])
		var pc := "#5fd35f" if ea >= em else ("#ffc040" if ea * 2 >= em else "#ff5040")
		t += "In the fight  [color=%s]%s[/color][color=#666]%s[/color]  %d of %d %s%s\n" % [pc, "■".repeat(ea),
			"□".repeat(em - ea), ea, em, info["element_noun"], ("  (%d knocked out)" % (em - ea)) if em > ea else ""]
		var ef := float(info["engage_factor"])
		var ec := "#9fc8ff" if ef < 1.25 else ("#ffc040" if ef < 1.8 else "#ff5040")
		t += "Engagement   [color=%s]acquire %.1f min · fire every %.1f min[/color]\n" % [ec, float(info["acquire_min"]), float(info["cycle_min"])]
		t += "[color=#b8c0a8]  readiness ×%.2f · morale ×%.2f · ammo ×%.2f · suppressed ×%.2f · night ×%.2f[/color]\n" % [
			float(info["engage_readiness"]), float(info["engage_morale"]), float(info["engage_ammo"]), float(info.get("engage_supp", 1.0)),
			float(info.get("engage_night", 1.0))]
		var sp := float(info.get("suppression", 0.0))
		if sp >= 1.0:
			var scol := "#ff5040" if info.get("pinned", false) else ("#ffc040" if sp >= 30.0 else "#b8c0a8")
			t += "Suppression  [color=%s]%d%s[/color]\n" % [scol, int(sp),
				"  PINNED: gone to ground, won't move forward, spots at half range" if info.get("pinned", false) else ""]
	else:
		t += _bar("Strength", float(info["strength"]))
	t += _bar("Morale", float(info["morale"]))
	t += _bar("Fuel", float(info["fuel"]))
	t += _bar("Ammo", float(info["ammo"]))
	t += _bar("Readiness", float(info["readiness"]))
	t += "\n%s   Order: %s\n" % [_situation(info), info["order"]]
	if info.has("gun_name"):
		t += "%s · range %.1f km\n" % [info["gun_name"], float(info["gun_range_km"])]
	t += "Sight %.1f km · weapons %.1f km\n" % [float(info["spot_range_km"]), float(info["weapon_range_km"])]
	t += _fire_support_line(info)
	t += _mounted_line(info)
	t += _standoff_text(info)
	t += _team_line(info)
	t += "%s · breaks contact: %s\n" % [info["roe_name"], String(info["break_contact_name"]).to_lower()]
	t += "New orders take [b]%d min[/b] to arrive\n" % int(info["command_delay"])
	if info.get("pending", false):
		t += "[color=#ffd84a]%s order arrives in %d min[/color]\n" % [info["pending_kind"], int(info["pending_in"])]
	if info.has("eta_minutes"):
		var eta := int(info["eta_minutes"])
		t += "Route %.1f km, about %dh%02d left\n" % [float(info["route_km"]), eta / 60, eta % 60]
	unit_text.text = t


## Several own units: averages and a roster. `company` is the company name when
## the selection is exactly one whole company, else "".
func show_group(infos: Array, company: String) -> void:
	if infos.is_empty():
		unit_panel.visible = false
		return
	unit_panel.visible = true
	orders_box.visible = true
	var n := infos.size()
	var sums := {"strength": 0.0, "morale": 0.0, "fuel": 0.0, "ammo": 0.0, "readiness": 0.0}
	var men := 0
	var ef_sum := 0.0
	var el_act := 0
	var el_max := 0
	var engaged := 0
	var hot := 0
	var warm := 0
	var moving := 0
	var pending := 0
	var delay_lo := 1 << 30
	var delay_hi := 0
	for info in infos:
		for k in sums:
			sums[k] += float(info[k])
		men += int(info.get("crew", 0))
		ef_sum += float(info.get("engage_factor", 1.0))
		el_act += int(info.get("elements_active", 0))
		el_max += int(info.get("elements_max", 0))
		if info.get("engaged", false): engaged += 1
		elif info["zone"] == "Hot": hot += 1
		elif info["zone"] == "Warm": warm += 1
		if info["order"] != "Holding": moving += 1
		if info.get("pending", false): pending += 1
		delay_lo = mini(delay_lo, int(info["command_delay"]))
		delay_hi = maxi(delay_hi, int(info["command_delay"]))
	var t := ""
	if company.begins_with("Team "):
		t += "[b]%s[/b]   whole team, led by %s\n" % [company, infos[0]["name"]]
		t += _team_line(infos[0])
	elif company != "":
		t += "[b]%s[/b]   whole company\n" % company
	else:
		t += "[b]%d units selected[/b]\n" % n
	t += "[color=#b8c0a8]%d platoons and sections · %d men[/color]\n\n" % [n, men]
	var pc := "#5fd35f" if el_act >= el_max else ("#ffc040" if el_act * 2 >= el_max else "#ff5040")
	t += "In the fight  [color=%s]%d of %d[/color] vehicles, guns and squads%s\n" % [pc, el_act, el_max,
		("  (%d knocked out)" % (el_max - el_act)) if el_max > el_act else ""]
	t += "Engagement   [color=#b8c0a8]average ×%.2f the time of fresh crews[/color]\n" % (ef_sum / n)
	t += _bar("Morale", sums["morale"] / n)
	t += _bar("Fuel", sums["fuel"] / n)
	t += _bar("Ammo", sums["ammo"] / n)
	t += _bar("Readiness", sums["readiness"] / n)
	t += "\n"
	if engaged > 0:
		t += "[color=#ff6a50]%d in combat[/color]   " % engaged
	if hot > 0:
		t += "[color=#ff9a50]%d near enemies[/color]   " % hot
	if warm > 0:
		t += "[color=#ffc040]%d within 40 km of enemies[/color]" % warm
	if engaged > 0 or hot > 0 or warm > 0:
		t += "\n"
	t += "%d moving, %d holding" % [moving, n - moving]
	if pending > 0:
		t += ", [color=#ffd84a]%d with orders on the way[/color]" % pending
	t += "\nNew orders take [b]%s[/b] to arrive\n\n" % (("%d min" % delay_lo) if delay_lo == delay_hi else ("%d-%d min" % [delay_lo, delay_hi]))
	var shown := 0
	for info in infos:
		if shown >= 12:
			t += "[color=#9fa89a]... and %d more[/color]\n" % (n - shown)
			break
		var em := int(info.get("elements_max", 1))
		var ea := int(info.get("elements_active", em))
		var col := "#5fd35f" if ea >= em else ("#ffc040" if ea * 2 >= em else "#ff5040")
		var state: String = info["order"]
		if info.get("pending", false):
			state = "orders in %d min" % int(info["pending_in"])
		var pips := "■".repeat(ea) + "□".repeat(em - ea)
		t += "%s [color=#9fa89a]%s[/color]  [color=%s]%s %d/%d[/color]  [color=#9fa89a]%s[/color]\n" % [
			info["name"], info["type_name"], col, pips, ea, em, state]
		shown += 1
	unit_text.text = t


## One line on how close the fighting is, in plain words.
func _situation(info: Dictionary) -> String:
	if info.get("engaged", false):
		return "[color=#ff6a50]In combat[/color]"
	match String(info["zone"]):
		"Hot": return "[color=#ff9a50]Enemies within 10 km[/color]"
		"Warm": return "[color=#ffc040]Enemies within 40 km[/color]"
	return "[color=#9fc8ff]Quiet[/color]"


func set_objectives(obj: Dictionary) -> void:
	var es := int(obj.get("enemy_start", 0))
	var ee := int(obj.get("enemy_effective", 0))
	var os := int(obj.get("own_start", 0))
	var oe := int(obj.get("own_effective", 0))
	var hq_ok: bool = obj.get("hq_alive", false)
	var hq_text := "alive"
	if not hq_ok:
		var acting := String(obj.get("acting_hq", ""))
		var left := int(obj.get("handover_minutes", -1))
		if acting != "":
			hq_text = "lost: orders slow, %s runs the trains (half-rate resupply)" % acting
		elif left > 0:
			hq_text = "lost: orders slow, trains reorganising (%d min)" % left
		else:
			hq_text = "lost: orders slow, no resupply"
	var t := ""
	match int(obj.get("outcome", 0)):
		1: t += "[color=#7fe07f][b]Victory.[/b][/color]  "
		2: t += "[color=#ff6050][b]Defeat.[/b][/color]  "
		_: t += "[b]Contract:[/b] "
	t += "break the %s: [color=%s]%d of %d[/color] fighting units left\n" % [obj.get("enemy_name", "enemy"),
		"#7fe07f" if ee == 0 else "#ffc040", ee, es]
	t += "Keep 3+ units fighting [color=%s](%d of %d)[/color]   ·   HQ [color=%s]%s[/color]   ·   kills %d, lost %d" % [
		"#7fe07f" if oe >= 3 else "#ff6050", oe, os, "#7fe07f" if hq_ok else "#ff6050",
		hq_text, int(obj.get("kills", 0)), int(obj.get("losses", 0))]
	objective_label.text = t


func _build_end_screen() -> void:
	end_panel = PanelContainer.new()
	end_panel.add_theme_stylebox_override("panel", _panel_style(0.95))
	end_panel.set_anchors_preset(Control.PRESET_CENTER)
	_offsets(end_panel, -310, -125, 310, 125)
	end_panel.visible = false
	add_child(end_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 12)
	end_panel.add_child(col)
	end_title = _label("", 30, Color(1, 0.9, 0.6))
	end_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	col.add_child(end_title)
	end_text = RichTextLabel.new()
	end_text.bbcode_enabled = true
	end_text.fit_content = true
	end_text.scroll_active = false
	end_text.add_theme_font_size_override("normal_font_size", 15)
	col.add_child(end_text)
	var row := HBoxContainer.new()
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	row.add_theme_constant_override("separation", 10)
	col.add_child(row)
	for d in [["Keep playing", "continue"], ["Load quick save", "load"], ["New game", "restart"], ["Quit", "quit"]]:
		var b := _button(d[0])
		var choice: String = d[1]
		b.pressed.connect(func() -> void: end_choice.emit(choice))
		row.add_child(b)
		if choice == "load":
			end_load_button = b


func show_end_screen(obj: Dictionary, has_save: bool) -> void:
	var victory := int(obj["outcome"]) == 1
	end_title.text = "Contract fulfilled" if victory else "Company Murks is finished"
	end_title.add_theme_color_override("font_color", Color(0.6, 1.0, 0.6) if victory else Color(1.0, 0.5, 0.4))
	var mins := int(obj.get("time_minutes", 0)) - 6 * 60
	var t := "%s\n\n" % obj.get("outcome_text", "")
	var days := mins / 1440
	var hours := (mins % 1440) / 60
	t += "Campaign length: %d day%s %d hour%s\n" % [days, "" if days == 1 else "s", hours, "" if hours == 1 else "s"]
	t += "Hostile units destroyed: %d\n" % int(obj.get("kills", 0))
	t += "Own units lost: %d   ·   still fighting: %d of %d\n" % [int(obj.get("losses", 0)),
		int(obj.get("own_effective", 0)), int(obj.get("own_start", 0))]
	t += "%s: %d of %d units still fighting" % [obj.get("enemy_name", ""), int(obj.get("enemy_effective", 0)),
		int(obj.get("enemy_start", 0))]
	end_text.text = t
	end_load_button.disabled = not has_save
	end_panel.visible = true


func _bar(name: String, v: float) -> String:
	var n := int(round(clampf(v, 0, 100) / 10.0))
	var col := "#5fd35f" if v >= 60 else ("#ffc040" if v >= 30 else "#ff5040")
	return "%-10s [color=%s]%s[/color][color=#444]%s[/color] %d\n" % [name, col, "■".repeat(n), "■".repeat(10 - n), int(v)]


func _thousands(v: int) -> String:
	var s := str(abs(v))
	var out := ""
	while s.length() > 3:
		out = "," + s.substr(s.length() - 3) + out
		s = s.substr(0, s.length() - 3)
	return ("-" if v < 0 else "") + s + out
