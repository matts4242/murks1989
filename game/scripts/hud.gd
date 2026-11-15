class_name Hud
extends Control
## Interface built in code, all under one Theme:
## - menu bar (Game, View, Time, Orders, Help) with the clock, speeds and stocks;
## - unit panel: status text, order buttons and dropdowns for fire, break contact,
##   stand-off, formation and spread, and a Team menu;
## - right-click order menu at the cursor (the same orders, aimed at that hex);
## - title screen, Esc pause menu, controls and map-credits dialogs, end screen;
## - event log, hover status line, banners and toasts.
## Every menu item sends the same command as its key, so menus, panel and keys never disagree.

signal speed_pressed(index: int)
signal toggle_pressed(name: String)
signal hold_pressed
signal command(name: String)        # orders: panel, Orders menu, right-click menu (same names the keys use)
signal menu_action(name: String)    # game level: start, continue, new_game, title, quit, save, load, resume, ...
signal event_clicked(pos: Vector2)
signal end_choice(choice: String)    # end screen: continue / load / restart / quit

const TEXT := Color(0.92, 0.92, 0.86)
const MUTED := Color(0.72, 0.77, 0.68)
const AMBER := Color(1.0, 0.86, 0.5)
const HINT := Color(0.6, 0.85, 1.0)

# Stand-off choices: [label, value]; 255 = company default, 254 = the unit's weapon range, else hexes.
const STANDOFFS := [["Default", 255], ["Fight", 0], ["1 km", 2], ["2 km", 4], ["3 km", 6], ["Weapon range", 254]]
const TEAM_COLORS := [Color("#f2c14e"), Color("#39c0b0"), Color("#c77dff"), Color("#8bd450"),
	Color("#ff8c42"), Color("#5ab0ff"), Color("#ff6f91"), Color("#d9d9d9")]
const SPREADS := [1, 2, 4] # hexes between formation slots: 500 m, 1 km, 2 km
const SPREAD_NAMES := ["500 m", "1 km", "2 km"]
const MOVES := [
	["Move", KEY_F, "Hasty move: normal speed, shoots on the move at a penalty."],
	["Road march", KEY_Q, "Fast on roads, slow off them. Shoots badly; a bunched column is easy to hit."],
	["Deliberate move", KEY_E, "Slow (x1.5 time). Stays concealed in cover and shoots as if halted."],
	["Assault", KEY_R, "Slightly slower. Presses the attack and may enter enemy-held hexes; breaks contact only at heavier losses."]]
const ROES := [["At will", "Engage anything in range and sight."],
	["Return fire", "Only shoot back when fired upon. Good for scouts and ambushes."],
	["Hold fire", "Never fire. Stays hidden; will not defend itself."]]
const BREAKS := [["Heavy loss", "Fall back on its own below 30% strength or morale 25."],
	["Light loss", "Fall back early: below 60% strength or morale 45. Preserves the company."],
	["Never", "Hold on until morale collapses (below 8)."]]
const SHAPES := [["Keep", "Hold the team's present layout around its leader."],
	["Column", "One behind the other along the line of march."],
	["Line", "Abreast of the leader, across the line of march."],
	["Wedge", "A V behind the leader: the leader at the point."]]
# Overlay toggles in the View and Time menus: [toggle name, menu label, key].
const VIEW_TOGGLES := [["Group", "Company grouping on the theater map", KEY_O],
	["Direct command", "Direct command: who takes orders from whom", KEY_T],
	["Fire support", "Fire support net and gun ranges", KEY_I],
	["Supply", "Supply routes and convoys", KEY_F3],
	["Grid", "Hex grid", KEY_G], ["Detail", "Painted terrain textures (off: flat colours)", KEY_F6],
	["Edges", "Natural terrain edges (off: crisp hex borders)", 0],
	["Vehicles", "Vehicles on counters (off: NATO symbols)", KEY_F7],
	["Topo", "Topographic map: contour lines and heights", KEY_F8], ["Sight", "Line of sight of the selected unit", KEY_V],
	["Zones", "Simulation fidelity zones", KEY_Z]]
const LEVY_TIPS := ["0.1 kg a head from the town's larders (a farm collective gives 0.5). Loyalty -10.",
	"0.5 L a head from garages and stations. Loyalty -10.",
	"2 DM a head in taxes taken at once. Loyalty -15.",
	"Every volunteer in the pool joins the company as replacements. Loyalty -15."]
const SPEED_ITEMS := [["Pause", KEY_SPACE], ["1 min per second", KEY_1], ["5 min per second", KEY_2],
	["30 min per second", KEY_3], ["2 hours per second", KEY_4], ["Max", KEY_5]]

var main: Node
var save_path := ""                 # quick save file, set by main (enables Continue / Quick load)

# Top bar
var time_label: Label
var speed_buttons: Array[Button] = []
var cap_label: Label
var stock_label: Label
var perf_label: Label
var menu_buttons := {}              # "Game" / "View" / ... -> MenuButton
var toggle_state := {}              # toggle name -> on
var speed_index := 0

# Unit panel
var unit_panel: PanelContainer
var unit_text: RichTextLabel
var orders_box: VBoxContainer
var orders_body: VBoxContainer
var orders_toggle: Button
var delay_label: Label
var mode_buttons: Array[Button] = []
var mount_button: Button
var roe_opt: OptionButton
var brk_opt: OptionButton
var standoff_opt: OptionButton
var default_opt: OptionButton
var shape_opt: OptionButton
var spread_opt: OptionButton
var team_menu: MenuButton
var orders_hint: Label
# What the selection can do and how it is set, for the dropdowns and both order menus.
# roe / brk / shape / spacing / standoff: -1 mixed across the selection.
var ord := {"has_own": false, "mode": -1, "roe": 0, "brk": 0, "pending": false, "delay": "",
	"in_team": false, "can_form": false, "shape": -1, "spacing": -1, "team": "",
	"standoff": 255, "default_standoff": 0, "can_dismount": false, "can_mount": false}

# Holding panel (a town, farm collective or depot selected by its pennant)
var holding_panel: PanelContainer
var holding_text: RichTextLabel
var train_button: Button
var levy_menu: MenuButton
var route_menu: MenuButton
var hold := {}                      # the holding shown, as world.get_holding gives it
var hold_routes: Array = []         # its routes
var hold_targets: Array = []        # other own holdings a route can end at

var order_popup: PopupMenu          # right-click menu
var log_box: VBoxContainer
var status_label: Label
var objective_label: RichTextLabel
var banner: Label
var banner_until := 0
var toast: Label
var toast_until := 0
var loading: Label

# Dialogs and screens
var help_panel: Control
var credits_panel: Control
var legend_panel: Control
var title_screen: Control
var title_brief: RichTextLabel
var title_continue: Button
var pause_screen: Control
var pause_time: Label
var pause_load: Button
var end_panel: PanelContainer
var end_title: Label
var end_text: RichTextLabel
var end_load_button: Button


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	theme = _make_theme()
	_build_top_bar()
	_build_unit_panel()
	_build_holding_panel()
	_build_log()
	_build_status()
	_build_banners()
	_build_end_screen()
	_build_pause_menu()
	_build_title_screen()
	help_panel = _build_dialog("Controls", _controls_body())
	credits_panel = _build_dialog("Map data credits", _credits_body())
	legend_panel = _build_dialog("Map legend", _legend_body())
	order_popup = _new_popup()
	add_child(order_popup)
	_build_loading()


# --- Look ----------------------------------------------------------------------------

func _box(bg: Color, border: Color, radius := 3, pad := Vector4(8, 4, 8, 4)) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = bg
	sb.border_color = border
	sb.set_border_width_all(1)
	sb.set_corner_radius_all(radius)
	sb.content_margin_left = pad.x
	sb.content_margin_top = pad.y
	sb.content_margin_right = pad.z
	sb.content_margin_bottom = pad.w
	return sb


func _panel_style(alpha := 0.88) -> StyleBoxFlat:
	return _box(Color(0.07, 0.08, 0.07, alpha), Color(0.45, 0.5, 0.4, 0.6), 4, Vector4(10, 6, 10, 6))


## One theme for every panel, button, dropdown, menu and tooltip: dark olive, amber when on.
func _make_theme() -> Theme:
	var th := Theme.new()
	th.default_font_size = 14
	th.set_stylebox("panel", "PanelContainer", _panel_style())
	var bn := _box(Color(0.12, 0.14, 0.11, 0.96), Color(0.3, 0.34, 0.26))
	var bh := _box(Color(0.19, 0.22, 0.16, 0.98), Color(0.55, 0.6, 0.44))
	var bp := _box(Color(0.3, 0.28, 0.15, 0.98), Color(0.9, 0.76, 0.38))
	var bd := _box(Color(0.1, 0.11, 0.1, 0.6), Color(0.18, 0.2, 0.18))
	var clear := _box(Color(0, 0, 0, 0), Color(0, 0, 0, 0))
	for t in ["Button", "OptionButton", "MenuButton"]:
		th.set_stylebox("normal", t, clear if t == "MenuButton" else bn)
		th.set_stylebox("hover", t, bh)
		th.set_stylebox("pressed", t, bp)
		th.set_stylebox("hover_pressed", t, bp)
		th.set_stylebox("disabled", t, clear if t == "MenuButton" else bd)
		th.set_stylebox("focus", t, StyleBoxEmpty.new())
		th.set_color("font_color", t, TEXT)
		th.set_color("font_hover_color", t, Color(1, 1, 0.94))
		th.set_color("font_pressed_color", t, AMBER)
		th.set_color("font_hover_pressed_color", t, AMBER)
		th.set_color("font_focus_color", t, TEXT)
		th.set_color("font_disabled_color", t, Color(0.45, 0.48, 0.43))
	th.set_stylebox("panel", "PopupMenu", _box(Color(0.08, 0.09, 0.08, 0.98), Color(0.5, 0.55, 0.42), 4, Vector4(4, 6, 4, 6)))
	th.set_stylebox("hover", "PopupMenu", _box(Color(0.26, 0.28, 0.17), Color(0.75, 0.68, 0.38), 2, Vector4(0, 0, 0, 0)))
	var sep := StyleBoxLine.new()
	sep.color = Color(0.45, 0.5, 0.4, 0.55)
	sep.thickness = 1
	th.set_stylebox("separator", "PopupMenu", sep)
	th.set_stylebox("labeled_separator_left", "PopupMenu", sep)
	th.set_stylebox("labeled_separator_right", "PopupMenu", sep)
	th.set_color("font_color", "PopupMenu", TEXT)
	th.set_color("font_hover_color", "PopupMenu", Color(1, 0.95, 0.75))
	th.set_color("font_disabled_color", "PopupMenu", Color(0.52, 0.55, 0.5))
	th.set_color("font_accelerator_color", "PopupMenu", Color(0.62, 0.68, 0.58))
	th.set_color("font_separator_color", "PopupMenu", AMBER)
	th.set_constant("v_separation", "PopupMenu", 7)
	th.set_constant("h_separation", "PopupMenu", 8)
	th.set_constant("item_start_padding", "PopupMenu", 10)
	th.set_constant("item_end_padding", "PopupMenu", 14)
	th.set_font_size("font_size", "PopupMenu", 14)
	th.set_font_size("font_separator_size", "PopupMenu", 12)
	th.set_stylebox("panel", "TooltipPanel", _box(Color(0.06, 0.07, 0.06, 0.97), Color(0.5, 0.55, 0.42), 3, Vector4(8, 5, 8, 5)))
	th.set_color("font_color", "TooltipLabel", TEXT)
	th.set_font_size("font_size", "TooltipLabel", 13)
	var hl := StyleBoxLine.new()
	hl.color = Color(0.45, 0.5, 0.4, 0.6)
	th.set_stylebox("separator", "HSeparator", hl)
	var vl := StyleBoxLine.new()
	vl.color = Color(0.45, 0.5, 0.4, 0.6)
	vl.vertical = true
	th.set_stylebox("separator", "VSeparator", vl)
	th.set_color("font_color", "Label", TEXT)
	th.set_color("default_color", "RichTextLabel", TEXT)
	return th


func _offsets(c: Control, l: float, t: float, r: float, b: float) -> void:
	c.offset_left = l
	c.offset_top = t
	c.offset_right = r
	c.offset_bottom = b


func _label(text: String, size := 15, color := TEXT) -> Label:
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


func _small_button(text: String, toggle: bool, tip: String) -> Button:
	var b := _button(text, toggle)
	b.add_theme_font_size_override("font_size", 12)
	b.tooltip_text = tip
	b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return b


## A dropdown: `items` are [label, tooltip]; picking one emits command `cmd_fmt % values[i]`.
func _dropdown(items: Array, values: Array, cmd_fmt: String, tip: String) -> OptionButton:
	var o := OptionButton.new()
	o.focus_mode = Control.FOCUS_NONE
	o.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	o.add_theme_font_size_override("font_size", 12)
	o.tooltip_text = tip
	o.fit_to_longest_item = false
	o.clip_text = true
	for i in items.size():
		o.add_item(String(items[i][0]), i)
		o.set_item_tooltip(i, String(items[i][1]))
	o.item_selected.connect(func(i: int) -> void: command.emit(cmd_fmt % values[i]))
	return o


## Shows the selection's value in a dropdown; -1 means it differs across the selection.
func _show_choice(o: OptionButton, index: int) -> void:
	if index < 0:
		if o.selected != -1:
			o.select(-1)
		o.text = "Mixed"
	elif o.selected != index:
		o.select(index)


# --- Menus ---------------------------------------------------------------------------

func _new_popup() -> PopupMenu:
	var pm := PopupMenu.new()
	pm.set_meta("acts", [])
	pm.id_pressed.connect(_on_menu_id.bind(pm))
	return pm


func _reset(pm: PopupMenu) -> void:
	for c in pm.get_children():
		if c is PopupMenu:
			pm.remove_child(c)
			c.queue_free()
	pm.clear()
	pm.set_meta("acts", [])


## Adds an item. `kind`: cmd (orders), menu (game actions), toggle, speed. `check`: -1 plain,
## 0/1 a checkbox, 2/3 a radio item (off/on).
func _add(pm: PopupMenu, label: String, kind: String, arg: Variant, key := 0, enabled := true, check := -1, tip := "") -> void:
	var acts: Array = pm.get_meta("acts")
	var id := acts.size()
	acts.append([kind, arg])
	if check < 0:
		pm.add_item(label, id, key as Key)
	elif check < 2:
		pm.add_check_item(label, id, key as Key)
	else:
		pm.add_radio_check_item(label, id, key as Key)
	var idx := pm.item_count - 1
	if check >= 0:
		pm.set_item_checked(idx, check == 1 or check == 3)
	pm.set_item_disabled(idx, not enabled)
	if tip != "":
		pm.set_item_tooltip(idx, tip)


func _note(pm: PopupMenu, text: String) -> void:
	pm.add_item(text)
	pm.set_item_disabled(pm.item_count - 1, true)


func _submenu(pm: PopupMenu, label: String, enabled := true) -> PopupMenu:
	var sub := _new_popup()
	pm.add_submenu_node_item(label, sub)
	pm.set_item_disabled(pm.item_count - 1, not enabled)
	return sub


func _on_menu_id(id: int, pm: PopupMenu) -> void:
	var acts: Array = pm.get_meta("acts", [])
	if id < 0 or id >= acts.size():
		return
	var a: Array = acts[id]
	match String(a[0]):
		"cmd": command.emit(String(a[1]))
		"menu": menu_action.emit(String(a[1]))
		"toggle": toggle_pressed.emit(String(a[1]))
		"speed": speed_pressed.emit(int(a[1]))


func _fill_game(pm: PopupMenu) -> void:
	_reset(pm)
	var has_save := save_path != "" and FileAccess.file_exists(save_path)
	_add(pm, "New game", "menu", "new_game")
	pm.add_separator()
	_add(pm, "Quick save", "menu", "save", KEY_F5)
	_add(pm, "Quick load", "menu", "load", KEY_F9, has_save)
	pm.add_separator()
	_add(pm, "Pause menu", "menu", "pause_menu", KEY_ESCAPE)
	_add(pm, "Quit to title screen", "menu", "title")
	_add(pm, "Quit to desktop", "menu", "quit")


func _fill_view(pm: PopupMenu) -> void:
	_reset(pm)
	pm.add_separator("Overlays")
	for t in VIEW_TOGGLES:
		_add(pm, String(t[1]), "toggle", t[0], int(t[2]), true, 1 if toggle_state.get(t[0], false) else 0)
	pm.add_separator("Camera")
	_add(pm, "Centre on the selection", "menu", "centre", KEY_C)
	_add(pm, "Next unit", "menu", "next_unit", KEY_TAB)
	_add(pm, "Whole map", "menu", "whole_map", KEY_M)
	pm.add_separator()
	_add(pm, "Map legend", "menu", "legend", KEY_F4)


func _fill_time(pm: PopupMenu) -> void:
	_reset(pm)
	pm.add_separator("Game time per real second")
	for i in SPEED_ITEMS.size():
		_add(pm, String(SPEED_ITEMS[i][0]), "speed", i, int(SPEED_ITEMS[i][1]), true, 3 if speed_index == i else 2)
	pm.add_separator()
	_add(pm, "Step 15 minutes", "menu", "step", KEY_PERIOD, speed_index == 0, -1, "Only while paused.")
	pm.add_separator()
	_add(pm, "WeGo pulse: pause each HQ order cycle", "toggle", "WeGo", KEY_P, true, 1 if toggle_state.get("WeGo", false) else 0)
	_add(pm, "Auto-pause on contacts and losses", "toggle", "Auto-pause", KEY_U, true, 1 if toggle_state.get("Auto-pause", false) else 0)


func _fill_help(pm: PopupMenu) -> void:
	_reset(pm)
	_add(pm, "Controls", "menu", "help", KEY_F1)
	_add(pm, "Map legend", "menu", "legend", KEY_F4)
	_add(pm, "Contract briefing", "menu", "briefing")
	_add(pm, "Map data credits", "menu", "credits", KEY_F2)


func _fill_orders_bar(pm: PopupMenu) -> void:
	fill_orders(pm, false, {})


## Orders for the selection. `at_hex`: the right-click menu, whose move orders go to the hex
## under the cursor (`ctx` gives its title and notes); otherwise the Orders menu, whose move
## items arm the next right-click.
func fill_orders(pm: PopupMenu, at_hex: bool, ctx: Dictionary) -> void:
	_reset(pm)
	if not ord["has_own"]:
		if at_hex:
			pm.add_separator(String(ctx.get("title", "Map")))
			if ctx.has("info"):
				_note(pm, String(ctx["info"]))
			_fill_place(pm, ctx)
			_add(pm, "Centre the map here", "cmd", "centre_here")
			_add(pm, "Whole map", "menu", "whole_map", KEY_M)
			pm.add_separator()
		_note(pm, "Select one of your units to give orders")
		return
	if at_hex and ctx.has("holding"):
		_fill_place(pm, ctx)
	if at_hex:
		pm.add_separator(String(ctx.get("title", "Orders")))
		if ctx.has("info"):
			_note(pm, String(ctx["info"]))
		if ctx.has("enemy"):
			_note(pm, String(ctx["enemy"]))
		var can_move: bool = ctx.get("can_move", true)
		for i in MOVES.size():
			_add(pm, String(MOVES[i][0]) + " here", "cmd", "move_here_%d" % i, int(MOVES[i][1]), can_move, -1, String(MOVES[i][2]))
		pm.add_separator()
	else:
		pm.add_separator("Next right-click sends")
		_add(pm, "Order menu (nothing armed)", "cmd", "disarm", 0, true, 3 if int(ord["mode"]) < 0 else 2,
			"Right-click opens this menu at the clicked hex.")
		for i in MOVES.size():
			_add(pm, String(MOVES[i][0]), "cmd", "mode_%d" % i, int(MOVES[i][1]), true, 3 if int(ord["mode"]) == i else 2,
				String(MOVES[i][2]) + " Then right-click the map.")
		pm.add_separator()
	_add(pm, "Hold position", "cmd", "hold", KEY_H, true, -1, "Stop and hold. A team leader halts its team.")
	_add(pm, "Fall back", "cmd", "fall_back", KEY_B, true, -1, "Withdraw about 6 km away from known enemies, or back to the HQ.")
	_add(pm, "Regroup on the HQ", "cmd", "regroup", KEY_Y, true, -1, "Form up around the company HQ (or whoever took over from it).")
	_add(pm, "Rest & resupply", "cmd", "rest", KEY_N, true, -1, "At the HQ (or whoever took over from it): double refill rate, faster recovery.")
	if ord["can_dismount"]:
		_add(pm, "Dismount the squads", "cmd", "dismount", KEY_COMMA, true, -1, "The vehicles halt and the squads get out to move and fight on foot.")
	if ord["can_mount"]:
		_add(pm, "Mount up", "cmd", "mount", KEY_COMMA, true, -1, "The squads walk back to their vehicles and climb aboard (2 min once together).")
	pm.add_separator("Standing orders")
	var fire := _submenu(pm, "Fire: " + _choice_name(ROES, int(ord["roe"])))
	for i in ROES.size():
		_add(fire, String(ROES[i][0]), "cmd", "roe_%d" % i, KEY_X if i == 0 else 0, true, 3 if int(ord["roe"]) == i else 2, String(ROES[i][1]))
	var brk := _submenu(pm, "Break contact: " + _choice_name(BREAKS, int(ord["brk"])).to_lower())
	for i in BREAKS.size():
		_add(brk, String(BREAKS[i][0]), "cmd", "brk_%d" % i, 0, true, 3 if int(ord["brk"]) == i else 2, String(BREAKS[i][1]))
	var so := _submenu(pm, "Stand-off: " + _standoff_label(int(ord["standoff"])))
	for s in STANDOFFS:
		var v: int = s[1]
		_add(so, String(s[0]), "cmd", "standoff_%d" % v, 0, true, 3 if int(ord["standoff"]) == v else 2, _standoff_tip(v))
	var dso := _submenu(pm, "Company default stand-off: " + _standoff_label(int(ord["default_standoff"])))
	for i in range(1, STANDOFFS.size()):
		var v: int = STANDOFFS[i][1]
		_add(dso, String(STANDOFFS[i][0]), "cmd", "default_standoff_%d" % v, 0, true, 3 if int(ord["default_standoff"]) == v else 2,
			"Company standing order for every unit set to Default. Applies at once.")
	var team := _submenu(pm, "Team" + ((": " + String(ord["team"])) if String(ord["team"]) != "" else ""))
	_fill_team(team)
	if at_hex:
		pm.add_separator()
		_add(pm, "Centre the map here", "cmd", "centre_here")
	if String(ord["delay"]) != "":
		pm.add_separator()
		_note(pm, "Orders arrive in %s (command delay)" % ord["delay"])


## The place under the cursor: select it, and for the company's own places raise militia,
## requisition, and set a supply route from its stores.
func _fill_place(pm: PopupMenu, ctx: Dictionary) -> void:
	if not ctx.has("holding"):
		return
	var hd: Dictionary = ctx["holding"]
	var own: bool = hd.get("own", false)
	pm.add_separator("%s · %s" % [String(hd["name"]), String(hd["kind_name"])])
	var line := String(hd["owner_name"])
	if own:
		line += " · loyalty %d%%" % int(hd.get("loyalty", 0.0))
	if int(hd.get("militia_squads", 0)) > 0:
		line += " · militia %d squad%s" % [int(hd["militia_squads"]), "" if int(hd["militia_squads"]) == 1 else "s"]
	_note(pm, line)
	_add(pm, "Select " + String(hd["name"]), "cmd", "holding_select_%d" % int(hd["id"]), 0, true, -1, "Open the place's panel: stocks, loyalty, militia, requisitions and routes.")
	if not own:
		pm.add_separator()
		return
	if int(hd["kind"]) == 0:
		_add(pm, "Raise militia", "cmd", "holding_train_%d" % int(hd["id"]), 0, hd.get("can_train", false) or hd.get("training", false),
			1 if hd.get("training", false) else 0, "A squad of %d every %d days from the town's volunteers, up to %d. It garrisons the town and takes no orders." % [
			int(hd.get("squad_men", 10)), int(hd.get("train_days", 2)), int(hd.get("militia_max", 0))])
	var lv := _submenu(pm, "Requisition", hd.get("can_levy", false))
	for l in hd.get("levies", []):
		var wait := int(l["hours_until"])
		var txt := "%s: %s" % [String(l["name"]), _levy_amount(int(l["kind"]), float(l["amount"]))]
		if wait > 0:
			txt += "  (again in %d h)" % wait
		_add(lv, txt, "cmd", "levy_%d_%d" % [int(hd["id"]), int(l["kind"])], 0, l["possible"] and wait == 0, -1, LEVY_TIPS[int(l["kind"])])
	if not hd.get("can_levy", false):
		_note(pm, "Requisition needs one of your units halted in the place")
	var rt := _submenu(pm, "Supply route from here to")
	_add(rt, "The trains (the HQ)", "cmd", "route_%d_-1" % int(hd["id"]), 0, true, -1, "A supply column shuttles what the company is short of from these stores to the HQ.")
	for t in ctx.get("own_holdings", []):
		if int(t["id"]) != int(hd["id"]):
			_add(rt, "%s (%s)" % [String(t["name"]), String(t["kind_name"]).to_lower()], "cmd", "route_%d_%d" % [int(hd["id"]), int(t["id"])])
	pm.add_separator()


func _levy_amount(kind: int, amount: float) -> String:
	match kind:
		0: return "%s kg" % _thousands(int(amount))
		1: return "%s L" % _thousands(int(amount))
		2: return "%s DM" % _thousands(int(amount))
	return "%d men" % int(amount)


func _fill_team(pm: PopupMenu) -> void:
	if pm.item_count > 0:
		_reset(pm)
	var in_team: bool = ord["in_team"]
	_add(pm, "Form team from the selection", "cmd", "team_form", KEY_J, ord["can_form"], -1,
		"Lock the selected units under one leader: the HQ if it is selected, else the first unit you selected.")
	_add(pm, "Disband", "cmd", "team_disband", KEY_MASK_SHIFT | KEY_J, in_team, -1, "Release the selected units' teams.")
	_add(pm, "Select the whole team", "cmd", "team_select", KEY_L, in_team)
	_add(pm, "Select the whole company", "cmd", "select_company", KEY_K)
	pm.add_separator("Formation")
	for i in SHAPES.size():
		_add(pm, String(SHAPES[i][0]), "cmd", "shape_%d" % i, 0, in_team, 3 if in_team and int(ord["shape"]) == i else 2,
			String(SHAPES[i][1]) + " Members keep station on the leader; the team moves at its slowest member's pace.")
	pm.add_separator("Spread")
	for i in SPREADS.size():
		_add(pm, SPREAD_NAMES[i], "cmd", "spread_%d" % SPREADS[i], 0, in_team, 3 if in_team and int(ord["spacing"]) == SPREADS[i] else 2,
			"Distance between formation places. 500 m is the doctrinal platoon spacing.")


func _choice_name(list: Array, i: int) -> String:
	return String(list[i][0]) if i >= 0 and i < list.size() else "mixed"


func _standoff_label(v: int) -> String:
	for s in STANDOFFS:
		if int(s[1]) == v:
			return String(s[0]).to_lower() if v != 255 else "company default"
	return "mixed"


func _standoff_tip(v: int) -> String:
	var tip := "Use the company default." if v == 255 else ("Stay and fight: close with the enemy, no stand-off." if v == 0 else
		("Keep known enemies at this unit's own weapon range." if v == 254 else "Halt %.1f km short of known enemies and pull back out if they come closer." % (v * 0.5)))
	return tip + " An assault ignores stand-off; team members go by their leader's."


## Right-click order menu at `screen_pos`. `ctx`: title, info, enemy, can_move.
func open_order_menu(screen_pos: Vector2, ctx: Dictionary) -> void:
	fill_orders(order_popup, true, ctx)
	order_popup.reset_size()
	var vs := get_viewport_rect().size
	var sz := Vector2(order_popup.size)
	var p := screen_pos + Vector2(2, 2)
	if p.x + sz.x > vs.x:
		p.x = maxf(0.0, screen_pos.x - sz.x - 2)
	if p.y + sz.y > vs.y:
		p.y = maxf(0.0, vs.y - sz.y)
	order_popup.popup(Rect2i(Vector2i(p), Vector2i(sz)))


func is_menu_open() -> bool:
	if order_popup.visible:
		return true
	for k in menu_buttons:
		if (menu_buttons[k] as MenuButton).get_popup().visible:
			return true
	return false


# --- Top bar ---------------------------------------------------------------------------

func _build_top_bar() -> void:
	var bar := PanelContainer.new()
	bar.add_theme_stylebox_override("panel", _box(Color(0.07, 0.08, 0.07, 0.94), Color(0.45, 0.5, 0.4, 0.6), 0, Vector4(6, 4, 10, 4)))
	bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	add_child(bar)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	bar.add_child(row)

	var menus := [["Game", _fill_game], ["View", _fill_view], ["Time", _fill_time], ["Orders", _fill_orders_bar], ["Help", _fill_help]]
	for m in menus:
		var mb := MenuButton.new()
		mb.text = m[0]
		mb.flat = false
		mb.switch_on_hover = true
		mb.focus_mode = Control.FOCUS_NONE
		mb.set_disable_shortcuts(true) # the keys are handled once, by main
		mb.add_theme_font_size_override("font_size", 15)
		var pm := mb.get_popup()
		pm.set_meta("acts", [])
		pm.id_pressed.connect(_on_menu_id.bind(pm))
		var fill: Callable = m[1]
		pm.about_to_popup.connect(fill.bind(pm))
		row.add_child(mb)
		menu_buttons[m[0]] = mb
	row.add_child(VSeparator.new())

	time_label = _label("--", 18, Color(1, 0.95, 0.8))
	time_label.custom_minimum_size.x = 200
	row.add_child(time_label)
	var names := ["Pause", "1 min", "5 min", "30 min", "2 h", "Max"]  # game time per real second
	for i in names.size():
		var b := _button(names[i], true)
		b.tooltip_text = ("Pause / resume. Key Space" if i == 0 else "Game time per real second. Key %d" % i)
		b.pressed.connect(func() -> void: speed_pressed.emit(i))
		row.add_child(b)
		speed_buttons.append(b)
	cap_label = _label("", 13, Color(1, 0.55, 0.4))
	cap_label.custom_minimum_size.x = 120
	row.add_child(cap_label)
	row.add_child(VSeparator.new())
	stock_label = _label("", 14)
	stock_label.tooltip_text = "Company stocks: diesel, ammunition, food, and men on the rolls."
	stock_label.mouse_filter = Control.MOUSE_FILTER_PASS
	row.add_child(stock_label)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(spacer)
	perf_label = _label("", 11, Color(0.6, 0.65, 0.6))
	perf_label.custom_minimum_size.x = 200
	perf_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	row.add_child(perf_label)


# --- Unit panel ------------------------------------------------------------------------

func _build_unit_panel() -> void:
	unit_panel = PanelContainer.new()
	unit_panel.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_offsets(unit_panel, -392, 52, -8, 52)
	unit_panel.custom_minimum_size = Vector2(384, 0)
	unit_panel.visible = false
	add_child(unit_panel)
	var col := VBoxContainer.new()
	unit_panel.add_child(col)
	unit_text = RichTextLabel.new()
	unit_text.bbcode_enabled = true
	unit_text.fit_content = true
	unit_text.scroll_active = false
	unit_text.custom_minimum_size = Vector2(340, 0)
	unit_text.add_theme_font_size_override("normal_font_size", 14)
	unit_text.add_theme_font_size_override("bold_font_size", 16)
	unit_text.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(unit_text)
	_build_orders(col)


func _build_holding_panel() -> void:
	holding_panel = PanelContainer.new()
	holding_panel.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	_offsets(holding_panel, -392, 52, -8, 52)
	holding_panel.custom_minimum_size = Vector2(384, 0)
	holding_panel.visible = false
	add_child(holding_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 5)
	holding_panel.add_child(col)
	holding_text = RichTextLabel.new()
	holding_text.bbcode_enabled = true
	holding_text.fit_content = true
	holding_text.scroll_active = false
	holding_text.custom_minimum_size = Vector2(340, 0)
	holding_text.add_theme_font_size_override("normal_font_size", 14)
	holding_text.add_theme_font_size_override("bold_font_size", 16)
	holding_text.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(holding_text)
	col.add_child(HSeparator.new())
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	col.add_child(row)
	train_button = _button("Raise militia", true)
	train_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	train_button.pressed.connect(func() -> void:
		if hold.has("id"):
			command.emit("holding_train_%d" % int(hold["id"])))
	row.add_child(train_button)
	levy_menu = MenuButton.new()
	levy_menu.text = "Requisition ▾"
	levy_menu.flat = false
	levy_menu.focus_mode = Control.FOCUS_NONE
	levy_menu.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	levy_menu.tooltip_text = "Take food, fuel, cash or men from the place at once. Costs loyalty; once a day per kind; needs one of your units halted there."
	var lp := levy_menu.get_popup()
	lp.set_meta("acts", [])
	lp.id_pressed.connect(_on_menu_id.bind(lp))
	lp.about_to_popup.connect(_fill_levy_menu.bind(lp))
	row.add_child(levy_menu)
	route_menu = MenuButton.new()
	route_menu.text = "Supply route ▾"
	route_menu.flat = false
	route_menu.focus_mode = Control.FOCUS_NONE
	route_menu.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	route_menu.tooltip_text = "Send the supply column shuttling from this place's stores to the trains or another holding, or cancel a route."
	var rp := route_menu.get_popup()
	rp.set_meta("acts", [])
	rp.id_pressed.connect(_on_menu_id.bind(rp))
	rp.about_to_popup.connect(_fill_route_menu.bind(rp))
	row.add_child(route_menu)


func _fill_levy_menu(pm: PopupMenu) -> void:
	_reset(pm)
	if hold.is_empty():
		return
	for l in hold.get("levies", []):
		var wait := int(l["hours_until"])
		var txt := "%s: %s, loyalty -%d" % [String(l["name"]), _levy_amount(int(l["kind"]), float(l["amount"])), int(l["loyalty_cost"])]
		if wait > 0:
			txt += "  (again in %d h)" % wait
		_add(pm, txt, "cmd", "levy_%d_%d" % [int(hold["id"]), int(l["kind"])], 0, l["possible"] and wait == 0 and hold.get("can_levy", false), -1, LEVY_TIPS[int(l["kind"])])
	if not hold.get("can_levy", false):
		_note(pm, "Needs one of your units halted in the place")


func _fill_route_menu(pm: PopupMenu) -> void:
	_reset(pm)
	if hold.is_empty():
		return
	pm.add_separator("From %s to" % String(hold["name"]))
	_add(pm, "The trains (the HQ)", "cmd", "route_%d_-1" % int(hold["id"]), 0, true, -1, "A supply column shuttles what the company is short of from these stores to the HQ.")
	for t in hold_targets:
		if int(t["id"]) != int(hold["id"]):
			_add(pm, "%s (%s)" % [String(t["name"]), String(t["kind_name"]).to_lower()], "cmd", "route_%d_%d" % [int(hold["id"]), int(t["id"])])
	if not hold_routes.is_empty():
		pm.add_separator("Routes here")
		for r in hold_routes:
			var txt := "%s → %s: %s" % [String(r["source_name"]), String(r["dest_name"]), "suspended" if r.get("suspended", false) else String(r.get("state_name", "")).to_lower()]
			_add(pm, "Cancel " + txt, "cmd", "cancel_route_%d" % int(r["id"]), 0, r.get("active", false))


## The holding panel: what the place is, its stores and people, militia, routes; actions below.
func show_holding(hd: Dictionary, routes: Array, own_holdings: Array) -> void:
	hold = hd
	hold_targets = own_holdings
	hold_routes = []
	for r in routes:
		if int(r["source"]) == int(hd["id"]) or int(r["dest"]) == int(hd["id"]):
			hold_routes.append(r)
	unit_panel.visible = false
	holding_panel.visible = true
	var own: bool = hd.get("own", false)
	var t := "[b]%s[/b]   %s\n" % [hd["name"], hd["kind_name"]]
	t += "[color=#b8c0a8]%s · %s people[/color]\n" % [hd["location"], _thousands(int(hd["population"]))]
	var oc := "#9fc8ff" if own else ("#ff8c7a" if int(hd["owner"]) == 1 else "#c8c8c0")
	t += "Held by [color=%s]%s[/color]" % [oc, hd["owner_name"]]
	if hd.get("fighting", false):
		t += "   [color=#ff6b4a]fighting in the place[/color]"
	t += "\n"
	var ms := int(hd.get("militia_squads", 0))
	if ms > 0 or own:
		t += "Militia: %d of %d squads%s\n" % [ms, int(hd.get("militia_max", 0)), "" if own or hd.get("militia_seen", false) else " seen"]
	if not own:
		t += "\n[color=#b8c0a8]Hold the place with your units for %d hours, unopposed, to take it. A garrison blocks that.[/color]\n" % 2
		holding_text.text = t
		train_button.visible = false
		levy_menu.visible = false
		route_menu.visible = false
		return
	train_button.visible = int(hd["kind"]) == 0
	levy_menu.visible = true
	route_menu.visible = true
	t += _bar("Loyalty", float(hd["loyalty"]))
	if hd.get("contested", false):
		t += "[color=#ff6b4a]Enemy alone in the place for %d min: taken at 120[/color]\n" % int(hd.get("contested_minutes", 0))
	t += "\n[b]Stores[/b]   diesel %s L · ammo %.1f t · food %s kg · cash %s DM\n" % [
		_thousands(int(hd["diesel_l"])), float(hd["ammo_t"]), _thousands(int(hd["food_kg"])), _thousands(int(hd["cash"]))]
	if float(hd.get("cash_per_day", 0.0)) > 0.0 or float(hd.get("food_per_day", 0.0)) > 0.0:
		t += "Each day at 06:00: +%s DM, +%s kg food (scaled by loyalty)\n" % [_thousands(int(hd["cash_per_day"])), _thousands(int(hd["food_per_day"]))]
	if int(hd["kind"]) == 0:
		t += "Volunteers: %d men in the pool\n" % int(hd["manpower"])
	t += "[color=#9fc8ff]%s[/color]\n" % ("The trains are parked here and draw from these stores." if hd.get("trains_here", false) else "Park the HQ here (or send a supply route) to move the stores to the company.")
	if hd.get("training", false):
		t += "Raising militia: next squad in %.1f days\n" % ((1.0 - float(hd.get("training_progress", 0.0))) * int(hd.get("train_days", 2)))
	train_button.set_pressed_no_signal(hd.get("training", false))
	train_button.disabled = not (hd.get("can_train", false) or hd.get("training", false))
	if hold_routes.is_empty():
		t += "No supply route."
	else:
		for r in hold_routes:
			var state := "suspended" if r.get("suspended", false) else String(r.get("state_name", "")).to_lower()
			t += "Route %s → %s: %s" % [r["source_name"], r["dest_name"], state]
			if int(r.get("trips", 0)) > 0:
				t += " · %d trip%s, %.1f t delivered, %.1f t/day" % [int(r["trips"]), "" if int(r["trips"]) == 1 else "s",
					float(r["delivered_l"]) / 1190.0 + float(r["delivered_t"]) + float(r["delivered_kg"]) / 1000.0, float(r.get("tonnes_per_day", 0.0))]
			t += "\n"
	holding_text.text = t


func hide_holding() -> void:
	holding_panel.visible = false


## Orders: everything selected (one unit, a team or a company). Every order and setting travels
## down the chain of command and arrives after the command delay.
func _build_orders(parent: Control) -> void:
	orders_box = VBoxContainer.new()
	orders_box.add_theme_constant_override("separation", 5)
	parent.add_child(orders_box)
	orders_box.add_child(HSeparator.new())
	var head := HBoxContainer.new()
	orders_box.add_child(head)
	orders_toggle = _button("▾ Orders", false)
	orders_toggle.flat = true
	orders_toggle.add_theme_color_override("font_color", AMBER)
	orders_toggle.tooltip_text = "Show or hide the orders. Right-click the map for the order menu at any time."
	orders_toggle.pressed.connect(func() -> void:
		orders_body.visible = not orders_body.visible
		orders_toggle.text = ("▾ Orders" if orders_body.visible else "▸ Orders"))
	head.add_child(orders_toggle)
	var sp := Control.new()
	sp.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(sp)
	delay_label = _label("", 12, MUTED)
	head.add_child(delay_label)

	orders_body = VBoxContainer.new()
	orders_body.add_theme_constant_override("separation", 5)
	orders_box.add_child(orders_body)

	# Arm the next right-click with a move (none armed: right-click opens the order menu).
	var moves := HBoxContainer.new()
	moves.add_theme_constant_override("separation", 4)
	orders_body.add_child(moves)
	var short := ["Move", "Road march", "Deliberate", "Assault"]
	for i in MOVES.size():
		var b := _small_button(short[i], true, "%s Arms the next right-click: it sends this order at once instead of opening the menu. Key %s; Esc cancels." % [
			MOVES[i][2], OS.get_keycode_string(MOVES[i][1])])
		b.pressed.connect(func() -> void: command.emit(("mode_%d" % i) if b.button_pressed else "disarm"))
		moves.add_child(b)
		mode_buttons.append(b)

	var acts := HBoxContainer.new()
	acts.add_theme_constant_override("separation", 4)
	orders_body.add_child(acts)
	for d in [["Hold", "hold", "Stop and hold position. A team leader halts its team. Key H"],
			["Fall back", "fall_back", "Withdraw about 6 km away from known enemies, or back to the HQ. Key B"],
			["Regroup", "regroup", "Form up around the company HQ (or whoever took over from it). Key Y"],
			["Resupply", "rest", "Rest & resupply at the HQ (or whoever took over from it): double refill rate, faster recovery. Key N"]]:
		var name: String = d[1]
		var b := _small_button(d[0], false, d[2])
		b.pressed.connect(func() -> void: command.emit(name))
		acts.add_child(b)
	mount_button = _small_button("Dismount", false, "APC/IFV platoons: dismount the squads, or mount them up again. Key , (comma)")
	mount_button.pressed.connect(func() -> void: command.emit("mount_toggle"))
	acts.add_child(mount_button)

	var grid := GridContainer.new()
	grid.columns = 4
	grid.add_theme_constant_override("h_separation", 6)
	grid.add_theme_constant_override("v_separation", 4)
	orders_body.add_child(grid)
	roe_opt = _dropdown(ROES, [0, 1, 2], "roe_%d", "When the unit may open fire. Key X cycles.")
	brk_opt = _dropdown(BREAKS, [0, 1, 2], "brk_%d", "When the unit falls back on its own.")
	var so_items: Array = []
	var so_vals: Array = []
	for s in STANDOFFS:
		so_items.append([s[0], _standoff_tip(int(s[1]))])
		so_vals.append(s[1])
	standoff_opt = _dropdown(so_items, so_vals, "standoff_%d", "Stay and fight, or keep known enemies at a distance: halt short of it and pull back out to it.")
	default_opt = _dropdown(so_items.slice(1), so_vals.slice(1), "default_standoff_%d", "Company standing order for every unit set to Default. Applies at once.")
	shape_opt = _dropdown(SHAPES, [0, 1, 2, 3], "shape_%d", "The team's formation around its leader.")
	var sp_items: Array = []
	for i in SPREADS.size():
		sp_items.append([SPREAD_NAMES[i], "Distance between formation places. 500 m is the doctrinal platoon spacing (300–600 m)."])
	spread_opt = _dropdown(sp_items, SPREADS, "spread_%d", "Distance between the team's formation places.")
	for pair in [["Fire", roe_opt], ["Break off", brk_opt], ["Stand-off", standoff_opt], ["Co. default", default_opt],
			["Formation", shape_opt], ["Spread", spread_opt]]:
		var l := _label(pair[0], 12, MUTED)
		l.custom_minimum_size.x = 64
		grid.add_child(l)
		grid.add_child(pair[1])

	var team_row := HBoxContainer.new()
	orders_body.add_child(team_row)
	var tl := _label("Team", 12, MUTED)
	tl.custom_minimum_size.x = 64
	team_row.add_child(tl)
	team_menu = MenuButton.new()
	team_menu.text = "No team  ▾"
	team_menu.flat = false
	team_menu.focus_mode = Control.FOCUS_NONE
	team_menu.set_disable_shortcuts(true)
	team_menu.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	team_menu.alignment = HORIZONTAL_ALIGNMENT_LEFT
	team_menu.add_theme_font_size_override("font_size", 12)
	team_menu.add_theme_stylebox_override("normal", _box(Color(0.12, 0.14, 0.11, 0.96), Color(0.3, 0.34, 0.26)))
	team_menu.tooltip_text = "Form, disband or select a team; formation and spread. Keys J, Shift+J, L, K."
	var tpm := team_menu.get_popup()
	tpm.set_meta("acts", [])
	tpm.id_pressed.connect(_on_menu_id.bind(tpm))
	tpm.about_to_popup.connect(_fill_team.bind(tpm))
	team_row.add_child(team_menu)

	orders_hint = _label("", 12, HINT)
	orders_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	orders_hint.custom_minimum_size.x = 340
	orders_box.add_child(orders_hint)


## Highlights the armed move mode and the selection's fire / break-contact settings
## (-1 = mixed across the selection). `pending` marks settings still on their way.
func set_orders_state(mode: int, roe: int, brk: int, pending: bool, delay_text: String) -> void:
	ord["has_own"] = true
	ord["mode"] = mode
	ord["roe"] = roe
	ord["brk"] = brk
	ord["pending"] = pending
	ord["delay"] = delay_text
	for i in mode_buttons.size():
		mode_buttons[i].set_pressed_no_signal(i == mode)
	_show_choice(roe_opt, roe)
	_show_choice(brk_opt, brk)
	delay_label.text = "arrive in %s" % delay_text
	var names := ["a hasty move", "a road march", "a deliberate move", "an assault"]
	var t := ("Next right-click sends %s (Esc cancels)." % names[mode]) if mode >= 0 else "Right-click the map for the order menu."
	orders_hint.text = t + ("  Fire / break-contact change on its way." if pending else "")


func set_has_own(has: bool) -> void:
	ord["has_own"] = has
	if not has:
		ord["delay"] = ""


## Team controls: the selection's team formation (shape -1 when none, -2 mixed).
func set_team_state(in_team: bool, can_form: bool, shape: int, spacing: int, team_name := "") -> void:
	ord["in_team"] = in_team
	ord["can_form"] = can_form
	ord["shape"] = shape if shape >= 0 else -1
	ord["spacing"] = spacing if spacing >= 0 else -1
	ord["team"] = team_name
	shape_opt.disabled = not in_team
	spread_opt.disabled = not in_team
	if in_team:
		_show_choice(shape_opt, shape if shape >= 0 else -1)
		_show_choice(spread_opt, SPREADS.find(spacing))
	else:
		shape_opt.select(-1)
		shape_opt.text = "—"
		spread_opt.select(-1)
		spread_opt.text = "—"
	team_menu.text = ((team_name if team_name != "" else "Several teams") if in_team else ("No team: form one" if can_form else "No team")) + "  ▾"


## Stand-off dropdowns: the selection's own setting (-1 mixed) and the company default.
func set_standoff_state(setting: int, default_value: int) -> void:
	ord["standoff"] = setting
	ord["default_standoff"] = default_value
	var si := -1
	for i in STANDOFFS.size():
		if int(STANDOFFS[i][1]) == setting:
			si = i
	_show_choice(standoff_opt, si)
	var di := -1
	for i in range(1, STANDOFFS.size()):
		if int(STANDOFFS[i][1]) == default_value:
			di = i - 1
	_show_choice(default_opt, di)


## Mounted infantry: what the comma key / Dismount button would do for the selection.
func set_mount_state(can_dismount: bool, can_mount: bool) -> void:
	ord["can_dismount"] = can_dismount
	ord["can_mount"] = can_mount
	mount_button.text = "Mount up" if can_mount and not can_dismount else "Dismount"
	mount_button.disabled = not (can_dismount or can_mount)


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


# --- Log, status line, banners ---------------------------------------------------------

func _build_log() -> void:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _panel_style(0.8))
	panel.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_offsets(panel, 8, -8, 640, -8) # grows upward with the reports it holds
	panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	add_child(panel)
	var col := VBoxContainer.new()
	panel.add_child(col)
	col.add_child(_label("Reports  (click one to centre the map on it)", 13, MUTED))
	log_box = VBoxContainer.new()
	log_box.add_theme_constant_override("separation", 0)
	col.add_child(log_box)


func _build_status() -> void:
	var sp := PanelContainer.new()
	sp.add_theme_stylebox_override("panel", _panel_style(0.75))
	sp.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_offsets(sp, 8, 104, 8, 104)
	sp.mouse_filter = Control.MOUSE_FILTER_IGNORE
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


func _build_banners() -> void:
	banner = _label("", 17, Color(1, 0.85, 0.5))
	banner.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_offsets(banner, -390, 100, 390, 130) # clear of the unit panel on a 1600 px screen
	banner.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	banner.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART # long briefings wrap instead of running under the panels
	banner.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.9))
	banner.add_theme_constant_override("outline_size", 6)
	banner.mouse_filter = Control.MOUSE_FILTER_IGNORE
	banner.z_index = 5 # above the pause menu, so a save or load can report back
	add_child(banner)

	toast = _label("", 15, Color(0.85, 0.95, 1.0))
	toast.set_anchors_preset(Control.PRESET_CENTER_TOP)
	_offsets(toast, -390, 186, 390, 212)
	toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	toast.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	toast.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.9))
	toast.add_theme_constant_override("outline_size", 5)
	toast.mouse_filter = Control.MOUSE_FILTER_IGNORE
	toast.z_index = 5
	add_child(toast)


func _build_loading() -> void:
	loading = _label("Murks 1989\nLoading the map...", 26, Color(1, 0.95, 0.8))
	loading.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	loading.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	loading.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	var bg := StyleBoxFlat.new()
	bg.bg_color = Color(0.05, 0.06, 0.05, 1.0)
	loading.add_theme_stylebox_override("normal", bg)
	add_child(loading)


# --- Screens and dialogs ---------------------------------------------------------------

## A full-screen layer that dims the map and stops clicks reaching it, with `content` centred.
func _screen(dim: float, content: Control) -> Control:
	var s := Control.new()
	s.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	s.mouse_filter = Control.MOUSE_FILTER_STOP
	s.visible = false
	var bg := ColorRect.new()
	bg.color = Color(0.03, 0.04, 0.03, dim)
	bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
	s.add_child(bg)
	var cc := CenterContainer.new()
	cc.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	cc.mouse_filter = Control.MOUSE_FILTER_IGNORE
	s.add_child(cc)
	cc.add_child(content)
	add_child(s)
	return s


func _menu_button(text: String, action: String, tip := "") -> Button:
	var b := _button(text)
	b.custom_minimum_size = Vector2(300, 40)
	b.add_theme_font_size_override("font_size", 17)
	b.tooltip_text = tip
	b.pressed.connect(func() -> void: menu_action.emit(action))
	return b


func _title_font(size: int) -> FontVariation:
	var fv := FontVariation.new()
	fv.base_font = ThemeDB.fallback_font
	fv.spacing_glyph = maxi(1, size / 12)
	fv.variation_embolden = 0.7
	return fv


func _build_title_screen() -> void:
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 10)
	col.custom_minimum_size = Vector2(560, 0)
	var title := _label("MURKS 1989", 76, AMBER)
	title.add_theme_font_override("font", _title_font(76))
	title.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.9))
	title.add_theme_constant_override("outline_size", 10)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	col.add_child(title)
	var sub := _label("Spring 1989. Europe after the war. One company, one contract.", 17, MUTED)
	sub.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	col.add_child(sub)
	var brief_box := PanelContainer.new()
	col.add_child(brief_box)
	title_brief = RichTextLabel.new()
	title_brief.bbcode_enabled = true
	title_brief.fit_content = true
	title_brief.scroll_active = false
	title_brief.custom_minimum_size = Vector2(540, 0)
	title_brief.add_theme_font_size_override("normal_font_size", 15)
	title_brief.add_theme_font_size_override("bold_font_size", 15)
	brief_box.add_child(title_brief)
	var gap := Control.new()
	gap.custom_minimum_size.y = 6
	col.add_child(gap)
	var buttons := VBoxContainer.new()
	buttons.add_theme_constant_override("separation", 8)
	buttons.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	col.add_child(buttons)
	buttons.add_child(_menu_button("New campaign", "start", "Take the contract from the start: 14 April 1989, 06:00."))
	title_continue = _menu_button("Continue from quick save", "continue", "Load the quick save (F5 saves during play).")
	buttons.add_child(title_continue)
	buttons.add_child(_menu_button("Controls", "help"))
	buttons.add_child(_menu_button("Map data credits", "credits"))
	buttons.add_child(_menu_button("Quit", "quit"))
	var foot := _label("Proof of concept · Poland and East Germany on 3.8 million 500 m hexes · one map, one clock", 12, Color(0.55, 0.6, 0.55))
	foot.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	col.add_child(foot)
	title_screen = _screen(0.88, col)


func show_title(briefing: String) -> void:
	title_brief.text = briefing
	title_continue.disabled = save_path == "" or not FileAccess.file_exists(save_path)
	title_screen.visible = true


func hide_title() -> void:
	title_screen.visible = false


func _build_pause_menu() -> void:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _box(Color(0.07, 0.08, 0.07, 0.97), Color(0.6, 0.62, 0.45, 0.8), 6, Vector4(22, 16, 22, 18)))
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 8)
	panel.add_child(col)
	var t := _label("PAUSED", 30, AMBER)
	t.add_theme_font_override("font", _title_font(30))
	t.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	col.add_child(t)
	pause_time = _label("", 14, MUTED)
	pause_time.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	col.add_child(pause_time)
	col.add_child(HSeparator.new())
	col.add_child(_menu_button("Resume", "resume", "Esc"))
	col.add_child(_menu_button("Quick save", "save", "F5"))
	pause_load = _menu_button("Quick load", "load", "F9")
	col.add_child(pause_load)
	col.add_child(_menu_button("Controls", "help", "F1"))
	col.add_child(_menu_button("Contract briefing", "briefing"))
	col.add_child(_menu_button("Map data credits", "credits", "F2"))
	col.add_child(HSeparator.new())
	col.add_child(_menu_button("New game", "new_game", "Start the contract again from 14 April, 06:00."))
	col.add_child(_menu_button("Quit to title screen", "title"))
	col.add_child(_menu_button("Quit to desktop", "quit"))
	pause_screen = _screen(0.55, panel)


func show_pause(time_text: String) -> void:
	pause_time.text = time_text
	pause_load.disabled = save_path == "" or not FileAccess.file_exists(save_path)
	pause_screen.visible = true


func hide_pause() -> void:
	pause_screen.visible = false


## Title screen or pause menu up: the map takes no input.
func is_modal() -> bool:
	return title_screen.visible or pause_screen.visible


## A centred dialog with a title, a body and a Close button. Not modal: it sits over everything.
func _build_dialog(title: String, body: Control) -> Control:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _box(Color(0.07, 0.08, 0.07, 1.0), Color(0.6, 0.62, 0.45, 0.8), 6, Vector4(20, 14, 20, 16)))
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.grow_horizontal = Control.GROW_DIRECTION_BOTH
	panel.grow_vertical = Control.GROW_DIRECTION_BOTH
	panel.visible = false
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 10)
	panel.add_child(col)
	var head := HBoxContainer.new()
	col.add_child(head)
	var t := _label(title, 20, AMBER)
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(t)
	var close := _button("Close")
	close.pressed.connect(func() -> void: panel.visible = false)
	head.add_child(close)
	col.add_child(HSeparator.new())
	col.add_child(body)
	add_child(panel)
	return panel


func _controls_body() -> Control:
	var sections := [
		["Mouse", [["Left click", "Select a unit (Shift adds or removes)"],
			["Double-click", "Select its team, or else its whole company"],
			["Right click", "Order menu at that hex (or send the armed order)"],
			["Drag, WASD, arrows", "Pan the map"], ["Wheel", "Zoom"]]],
		["Orders", [["F  Q  E  R", "Arm the next right-click: move, road march,\ndeliberate move, assault"],
			["Esc", "Disarm; with nothing armed, the pause menu"],
			["H", "Hold position"], ["B  Y  N", "Fall back, regroup on the HQ, rest & resupply"],
			[", (comma)", "Dismount or mount up"], ["X", "Cycle fire: at will, return fire, hold fire"],
			["J / Shift+J", "Form a team / disband it"], ["L / K", "Select the whole team / company"],
			["Tab / C", "Next unit / centre on the selection"]]],
		["Time", [["Space", "Pause or resume"], ["1 – 5", "1 min, 5 min, 30 min, 2 h per second, max"],
			[". (period)", "Step 15 minutes while paused"], ["P / U", "WeGo pulse / auto-pause on contact"]]],
		["View", [["O  T  I", "Company grouping, direct command, fire support"],
			["F3", "Supply routes and convoys"],
			["G  V  Z", "Hex grid, line of sight, fidelity zones"], ["M", "Whole map"]]],
		["Holdings", [["Click a pennant", "Open the place's panel"],
			["Right-click a town", "Raise militia, requisition, supply route"]]],
		["Game", [["F5 / F9", "Quick save / quick load"], ["F1 / F2", "Controls / map data credits"],
			["F4 / F6", "Map legend / painted terrain textures on or off"],
			["F7", "Counters show vehicles or NATO symbols"],
			["F8", "Topographic map: contour lines and heights"],
			["Esc", "Pause menu"]]]]
	var cols := HBoxContainer.new()
	cols.add_theme_constant_override("separation", 28)
	var halves := [[0, 1], [2, 3, 5, 4]]
	for h in halves:
		var col := VBoxContainer.new()
		col.add_theme_constant_override("separation", 4)
		cols.add_child(col)
		for si in h:
			var sec: Array = sections[si]
			col.add_child(_label(String(sec[0]), 14, AMBER))
			var g := GridContainer.new()
			g.columns = 2
			g.add_theme_constant_override("h_separation", 14)
			g.add_theme_constant_override("v_separation", 2)
			col.add_child(g)
			for row in sec[1]:
				var k := _label(String(row[0]), 13, Color(0.75, 0.88, 1.0))
				k.custom_minimum_size.x = 140
				g.add_child(k)
				g.add_child(_label(String(row[1]), 13))
			var gap := Control.new()
			gap.custom_minimum_size.y = 4
			col.add_child(gap)
	return cols


func _credits_body() -> Control:
	return _label(
		"Elevation: Copernicus DEM GLO-90. © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH\n" +
		"  2014-2018, provided under COPERNICUS by the European Union and ESA; all rights reserved.\n" +
		"  Produced using Copernicus WorldDEM-90 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018.\n" +
		"Land cover: © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021)\n" +
		"  processed by ESA WorldCover consortium. CC BY 4.0.\n" +
		"Rivers, roads, railways: Made with Natural Earth. Free vector and raster map data @ naturalearthdata.com.\n" +
		"Place names and populations: GeoNames (geonames.org), CC BY 4.0. Some names changed to 1989 usage.\n" +
		"Local roads are generated; borders, roads and rail are not yet corrected to 1989.", 13, Color(0.9, 0.9, 0.85))


## The three dialogs share the middle of the screen: opening one closes the others.
func _toggle_dialog(panel: Control) -> void:
	var open := not panel.visible
	for p in [help_panel, credits_panel, legend_panel]:
		(p as Control).visible = false
	panel.visible = open


func toggle_help() -> void:
	_toggle_dialog(help_panel)


func toggle_credits() -> void:
	_toggle_dialog(credits_panel)


func toggle_legend() -> void:
	_toggle_dialog(legend_panel)


## Closes a controls, credits or legend dialog if one is open; true if it did.
func close_dialog() -> bool:
	if help_panel.visible or credits_panel.visible or legend_panel.visible:
		help_panel.visible = false
		credits_panel.visible = false
		legend_panel.visible = false
		return true
	return false


# --- Map legend ----------------------------------------------------------------------
# The ground swatches are painted by the map's own terrain shader (swatch mode), so they
# always match the map. Line colours and widths repeat append_lines in
# core/src/render_data.cpp; contour colours repeat contours() in the shader. Change both
# together.

const LEGEND_TERRAIN := [
	[1, "Open ground", "Pasture and rough ground. Fast going, no cover."],
	[2, "Fields", "Ploughed, sown or fallow parcels. Open; slower when wet."],
	[3, "Forest", "Light woods: cover from view and fire, slow for vehicles."],
	[11, "Dense forest", "Tall, close timber: the best concealment, the slowest going."],
	[4, "Wetland", "Marsh, bog and pools: very slow, little cover."],
	[5, "Built-up, light", "Villages, farms and suburbs: good cover."],
	[13, "Built-up, dense", "Town blocks, estates and works: the strongest cover."],
	[0, "Water", "Lakes, sea and wide rivers: darker where deeper."]]
const LEGEND_LINES := [
	["road_major", "Main road", "Trunk roads and motorways."],
	["road", "Road", "Minor and local roads."],
	["rail", "Railway", "With sleepers across it."],
	["river_major", "Major river", "Crossed at bridges."],
	["river", "River or stream", ""],
	["bridge", "Bridge", "Pale deck with dark parapets."],
	["contour", "Contour line (F8)", "Every 20 m close in, every 100 m further out."],
	["contour_index", "Index contour", "Every fifth line, heavier, with its height."],
	["spot", "Spot height", "A summit and its height in metres."]]
const LEGEND_SWATCH_M := 420.0   # metres of ground across a terrain swatch


func _legend_body() -> Control:
	var cols := HBoxContainer.new()
	cols.add_theme_constant_override("separation", 30)
	for part in [["Ground", LEGEND_TERRAIN], ["Roads, rail, water and relief", LEGEND_LINES]]:
		var col := VBoxContainer.new()
		col.add_theme_constant_override("separation", 6)
		cols.add_child(col)
		col.add_child(_label(String(part[0]), 14, AMBER))
		for row in part[1]:
			var line := HBoxContainer.new()
			line.add_theme_constant_override("separation", 10)
			col.add_child(line)
			var sw: Control
			if row[0] is int:
				sw = _terrain_swatch(int(row[0]))
			else:
				sw = Control.new()
				sw.draw.connect(_draw_legend_swatch.bind(sw, String(row[0])))
			sw.custom_minimum_size = Vector2(58, 44)
			line.add_child(sw)
			var text := VBoxContainer.new()
			text.add_theme_constant_override("separation", 0)
			text.size_flags_vertical = Control.SIZE_SHRINK_CENTER
			line.add_child(text)
			text.add_child(_label(String(row[1]), 14))
			if String(row[2]) != "":
				text.add_child(_label(String(row[2]), 12, MUTED))
		if part[0] == "Ground":
			col.add_child(_label("Hill shading from the 90 m elevation data: slopes facing\nnorth-west are lit, south-east in shadow; high ground is paler.\nWoods and towns cast a shadow to the south-east.\nBorders between terrains wander naturally (View menu:\ncrisp hex borders); the grid shows the true hexes and the\nstatus bar names the terrain of the hex under the cursor.\nForest stands, field parcels and building types are\ndecoration: only the terrain and light or dense count.", 12, MUTED))
		else:
			col.add_child(_label("Roads, rivers and railways bend smoothly through\nthe hexes they link. Contour lines are a darker tone of\nthe ground they cross; lines close together mean steep\nground. Town names are sized by population.", 12, MUTED))
	return cols


## A swatch of one ground type, painted by the terrain shader itself.
func _terrain_swatch(code: int) -> Control:
	var r := ColorRect.new()
	var m := ShaderMaterial.new()
	m.shader = MapView.TERRAIN_SHADER
	m.set_shader_parameter("swatch", code)
	m.set_shader_parameter("swatch_size", LEGEND_SWATCH_M)
	m.set_shader_parameter("metres_per_pixel", LEGEND_SWATCH_M / 58.0)
	r.material = m
	return r


func _draw_legend_swatch(sw: Control, kind: String) -> void:
	var c := sw.size * 0.5
	var k := 21.0 / 288.7   # metres on the map -> pixels in the swatch, as for a 21 px hex
	var ground := Color8(124, 149, 77)
	sw.draw_rect(Rect2(Vector2(0, c.y - 15), Vector2(sw.size.x, 30)), ground)
	var a := Vector2(2, c.y)
	var b := Vector2(sw.size.x - 2, c.y)
	match kind:
		"road_major":
			sw.draw_line(a, b, Color8(112, 46, 30), 58.0 * 2 * k)
			sw.draw_line(a, b, Color8(230, 112, 58), 41.0 * 2 * k)
		"road":
			sw.draw_line(a, b, Color8(104, 82, 58), 36.0 * 2 * k)
			sw.draw_line(a, b, Color8(242, 230, 192), 23.0 * 2 * k)
		"rail":
			sw.draw_line(a, b, Color8(44, 42, 46), 2.0)
			for i in 7:
				var x := a.x + (i + 0.5) * (b.x - a.x) / 7.0
				sw.draw_line(Vector2(x, c.y - 28.0 * k), Vector2(x, c.y + 28.0 * k), Color8(44, 42, 46), 1.2)
		"river_major":
			sw.draw_line(a, b, Color8(64, 104, 144), 84.0 * 2 * k)
			sw.draw_line(a, b, Color8(98, 146, 190), 70.0 * 2 * k)
		"river":
			sw.draw_line(a, b, Color8(70, 110, 148), 46.0 * 2 * k)
			sw.draw_line(a, b, Color8(110, 156, 196), 33.0 * 2 * k)
		"bridge":
			var top := Vector2(c.x, c.y - 15)
			var bot := Vector2(c.x, c.y + 15)
			sw.draw_line(top, bot, Color8(70, 110, 148), 46.0 * 2 * k)
			sw.draw_line(top, bot, Color8(110, 156, 196), 33.0 * 2 * k)
			var l := 130.0 * k
			var w := 52.0 * k
			sw.draw_line(c - Vector2(l, 0), c + Vector2(l, 0), Color8(232, 226, 210), w * 2.0)
			sw.draw_line(a, b, Color8(104, 82, 58), 36.0 * 2 * k)
			sw.draw_line(a, b, Color8(242, 230, 192), 23.0 * 2 * k)
			for sgn in [-1.0, 1.0]:
				sw.draw_line(c + Vector2(-l, w * sgn), c + Vector2(l, w * sgn), Color8(34, 30, 28), 1.2)
				for e in [-1.0, 1.0]:
					var q := c + Vector2(l * e, w * sgn)
					sw.draw_line(q, q + Vector2(e * 1.8, sgn * 2.2), Color8(34, 30, 28), 1.2)
		"contour", "contour_index":
			# The shader prints contours as the ground colour times (0.50, 0.40, 0.30).
			var ink := Color(ground.r * 0.5, ground.g * 0.4, ground.b * 0.3)
			var heavy := kind == "contour_index"
			var pts := PackedVector2Array()
			for i in 13:
				var t := i / 12.0
				pts.append(Vector2(lerpf(a.x, b.x, t), c.y + sin(t * 5.0) * 7.0))
			sw.draw_polyline(pts, ground.lerp(ink, 0.9 if heavy else 0.65), 2.0 if heavy else 1.0, true)
			if heavy:
				sw.draw_string(get_theme_default_font(), Vector2(c.x - 9, c.y + 13), "300", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, Color8(42, 26, 12))
		"spot":
			sw.draw_colored_polygon(PackedVector2Array([c + Vector2(-12, -5), c + Vector2(-7.5, 3), c + Vector2(-16.5, 3)]), Color8(42, 26, 12))
			sw.draw_string(get_theme_default_font(), c + Vector2(-4, 4), "412", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color8(42, 26, 12))


# --- Updates -------------------------------------------------------------------------

func show_banner(text: String, seconds := 6.0) -> void:
	banner.text = text
	banner_until = Time.get_ticks_msec() + int(seconds * 1000)


func show_toast(text: String, seconds := 4.0) -> void:
	toast.text = text
	toast_until = Time.get_ticks_msec() + int(seconds * 1000)


func set_speed(index: int, capped: bool) -> void:
	speed_index = index
	for i in speed_buttons.size():
		speed_buttons[i].set_pressed_no_signal(i == index)
	cap_label.text = "Capped: in contact" if capped else ""


func set_toggle(name: String, on: bool) -> void:
	toggle_state[name] = on


func refresh(time_text: String, night: bool, stats: Dictionary, fps: float, map_stats: String) -> void:
	time_label.text = time_text + ("  night" if night else "")
	stock_label.text = "Diesel %s L%s   Ammo %.1f t%s   Food %s kg%s   Men %d%s   Cash %s DM" % [
		_thousands(int(stats["diesel_l"])), _days(float(stats.get("days_diesel", 1e9))),
		float(stats["ammo_t"]), _days(float(stats.get("days_ammo", 1e9))),
		_thousands(int(stats["food_kg"])), _days(float(stats.get("days_food", 1e9))),
		int(stats["personnel"]), (" (+%d)" % int(stats["replacements"])) if int(stats.get("replacements", 0)) > 0 else "",
		_thousands(int(stats.get("cash", 0.0)))]
	stock_label.tooltip_text = "Company stocks and days of supply at the recent rate of use; men on the rolls (+ replacements waiting); cash. %d holding%s, %d trucks, %d route%s running." % [
		int(stats.get("holdings_owned", 0)), "" if int(stats.get("holdings_owned", 0)) == 1 else "s", int(stats.get("trucks", 0)),
		int(stats.get("routes_active", 0)), "" if int(stats.get("routes_active", 0)) == 1 else "s"]
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
		b.clip_text = true # long reports end at the panel edge; the full text is in the tooltip
		b.tooltip_text = String(e["text"])
		b.add_theme_font_size_override("font_size", 13)
		var kind: String = e["kind"]
		var col := Color(0.88, 0.9, 0.84)
		if kind == "contact" or kind == "under_fire": col = Color(1.0, 0.7, 0.45)
		elif kind == "destroyed": col = Color(1.0, 0.45, 0.4)
		elif kind == "retreat" or kind == "supply": col = Color(1.0, 0.85, 0.4)
		elif kind == "holding": col = Color(0.75, 0.95, 0.7)
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
	end_panel.add_theme_stylebox_override("panel", _box(Color(0.07, 0.08, 0.07, 0.97), Color(0.6, 0.62, 0.45, 0.8), 6, Vector4(20, 14, 20, 16)))
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


## " (12 d)" for a finite days-of-supply figure, "" when nothing is being used.
func _days(d: float) -> String:
	if d >= 999.0:
		return ""
	return " (%.1f d)" % d if d < 10.0 else " (%d d)" % int(d)


func _thousands(v: int) -> String:
	var s := str(abs(v))
	var out := ""
	while s.length() > 3:
		out = "," + s.substr(s.length() - 3) + out
		s = s.substr(0, s.length() - 3)
	return ("-" if v < 0 else "") + s + out
