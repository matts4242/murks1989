class_name Hud
extends Control
## Interface built in code, all under one Theme:
## - menu bar (Game, View, Time, Orders, Help) with the clock, speeds and stocks;
## - unit panel: status text, order buttons and dropdowns for fire, break contact,
##   stand-off, formation and spread, and a Team menu;
## - order-of-battle panel (F11): the chain of command as a tree, the selection and every
##   unit under a selected commander marked in it;
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
signal oob_pick(id: int, how: String) # order of battle row: "select", "add", "command" (double-click), "company"

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
# View menu (MRK-35): [submenu, [toggle, label, key], ...]. Simulation fidelity zones are in Help > Debug.
const VIEW_GROUPS := [
	["Map", [["Grid", "Hex grid", KEY_G], ["KmGrid", "1 km map grid and grid references", KEY_G | KEY_MASK_SHIFT],
		["Topo", "Topographic map: contour lines and heights", KEY_F8],
		["Detail", "Painted terrain textures (off: flat colours)", KEY_F6],
		["Edges", "Natural terrain edges (off: crisp hex borders)", 0]]],
	["Units", [["Group", "Company grouping on the theater map", KEY_O],
		["Vehicles", "Vehicles on counters (off: NATO symbols)", KEY_F7],
		["Civilians", "Civilian traffic and services", KEY_F12]]],
	["Command overlays", [["Direct command", "Direct command: who takes orders from whom", KEY_T],
		["Fire support", "Fire support net and gun ranges", KEY_I],
		["Supply", "Supply routes and convoys", KEY_F3],
		["Sight", "Line of sight of the selected unit", KEY_V]]]]
const LEVY_TIPS := ["0.1 kg a head from the town's larders (a farm collective gives 0.5). Loyalty -10.",
	"0.5 L a head from garages and stations. Loyalty -10.",
	"2 DM a head in taxes taken at once. Loyalty -15.",
	"Every volunteer in the pool joins the company as replacements, who fill knocked-out infantry squads at the trains. Loyalty -15."]
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
var plan_mode_state := 0            # the planning mode shown (MapView.PLAN_*)
var plan_panel: PanelContainer
var plan_pick: OptionButton
var plan_keys: VBoxContainer
var plan_eye_row: HBoxContainer
var plan_eye_pick: OptionButton
var plan_info: Label
# Order of battle panel
var oob_panel: PanelContainer
var sandbox_panel: PanelContainer     # MRK-31: sandbox tools (place, delete, start time, AI)
var sbx_side: OptionButton
var sbx_kind: OptionButton
var sbx_place: Button
var sbx_delete_tool: Button
var sbx_day: OptionButton
var sbx_hour: OptionButton
var sbx_time_note: Label
var sbx_ai: CheckBox
var sbx_reveal: CheckBox
var title_new_game: Button
var title_campaign: Button
var title_three_way: Button
var title_sandbox: Button
var oob_tree: Tree
var oob_foot: Label
var oob_items := {}                 # unit id -> TreeItem
var oob_sig := ""                   # shape of the tree last built; rebuilt when it changes
var oob_folded := {}                # row key -> folded by the player
var oob_sel_seen: Array = []        # selection last scrolled to
const PLAN_EYES := [[2.0, "2 m: on foot"], [3.0, "3 m: vehicle (as the rules)"], [10.0, "10 m: rooftop"], [30.0, "30 m: mast or tower"]]
# Ruler and elevation profile panel (MRK-26)
var ruler_panel: PanelContainer
var ruler_title: Label
var ruler_line: Label
var ruler_refs: Label
var ruler_travel: VBoxContainer
var ruler_mode_pick: OptionButton
var ruler_rows: Array[Button] = []
var ruler_prof_row: HBoxContainer
var ruler_src_line: Button
var ruler_src_route: Button
var ruler_eye_pick: OptionButton
var ruler_chart: ProfileChart
var ruler_summary: Label
# Name / confirm dialog
var ask_panel: PanelContainer
var ask_title: Label
var ask_prompt: Label
var ask_edit: LineEdit
var ask_cb: Callable
var ask_has_text := false
var speed_index := 0
var top_bar: PanelContainer
var objective_panel: PanelContainer
var status_panel: PanelContainer
var log_panel: PanelContainer
var log_fold: Button
var log_filter_buttons: Array[Button] = []
var log_filter := 0                 # index into LOG_FILTERS
var log_open := false               # folded: the latest 3 reports; open: 10
var log_events: Array = []
var show_perf := false              # Help > Debug: frame and simulation timings in the top bar
# Docked layout (MRK-35): order of battle left, the selection right with the tool panels
# (sandbox, planning key, ruler) stacked under it, reports along the bottom.
const DOCK_PAD := 8.0
const RIGHT_W := 384.0
const LEFT_W := 300.0
const LOG_FILTERS := [["All", []], ["Combat", ["contact", "under_fire", "destroyed", "retreat"]],
	["Supply", ["supply", "holding"]], ["Orders", ["order", "arrived"]]]

# Unit panel
var unit_panel: PanelContainer
var unit_text: RichTextLabel
var orders_box: VBoxContainer
var orders_body: VBoxContainer
var unit_scroll: ScrollContainer     # MRK-35: the unit panel scrolls when the dock is short
var unit_col: VBoxContainer
var details_toggle: Button          # folds: details, standing orders, team (state kept across selections)
var details_text: RichTextLabel
var standing_toggle: Button
var standing_body: GridContainer
var team_toggle: Button
var team_body: VBoxContainer
var extra_row: HBoxContainer        # Dismount / Mount up and Rejoin, only when they apply
var fold_open := {"details": false, "standing": false, "team": false}
var delay_label: Label
var mode_buttons: Array[Button] = []
var mount_button: Button
var rejoin_button: Button
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
	"standoff": 255, "default_standoff": 0, "can_dismount": false, "can_mount": false, "detached": false, "can_reinforce": false}

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
	_build_plan_panel()
	_build_oob_panel()
	_build_ruler_panel()
	_build_sandbox_panel()
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
	_build_ask()
	_build_loading()
	resized.connect(_layout)
	_layout.call_deferred()


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


## Rich text sized to its content, for the docked panels.
func _rich(size: int) -> RichTextLabel:
	var r := RichTextLabel.new()
	r.bbcode_enabled = true
	r.fit_content = true
	r.scroll_active = false
	r.custom_minimum_size = Vector2(RIGHT_W - 34, 0)
	r.add_theme_font_size_override("normal_font_size", size)
	r.add_theme_font_size_override("bold_font_size", size + 2)
	r.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return r


## A section header that folds what follows it; `key` names the fold in fold_open.
func _fold_button(key: String, title: String, tip: String) -> Button:
	var b := _button("", false)
	b.flat = true
	b.alignment = HORIZONTAL_ALIGNMENT_LEFT
	b.clip_text = true
	b.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	b.add_theme_font_size_override("font_size", 13)
	b.add_theme_color_override("font_color", AMBER)
	b.tooltip_text = tip
	b.set_meta("title", title)
	b.set_meta("summary", "")
	b.pressed.connect(func() -> void:
		fold_open[key] = not fold_open[key]
		_apply_folds())
	_fold_label(b, false)
	return b


func _fold_label(b: Button, open: bool) -> void:
	var sm := String(b.get_meta("summary", ""))
	b.text = ("▾ " if open else "▸ ") + String(b.get_meta("title")) + (("  ·  " + sm) if sm != "" and not open else "")


func _set_fold_summary(b: Button, summary: String) -> void:
	b.set_meta("summary", summary)
	_fold_label(b, b.get_meta("open", false))


func _apply_folds() -> void:
	for pair in [[details_toggle, "details", details_text], [standing_toggle, "standing", standing_body], [team_toggle, "team", team_body]]:
		var b: Button = pair[0]
		var open: bool = fold_open[pair[1]]
		b.set_meta("open", open)
		_fold_label(b, open)
		(pair[2] as Control).visible = open and b.visible


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
		"local": _local_action(String(a[1]))
		"local_log": log_fold.pressed.emit()


## Menu items the HUD handles itself.
func _local_action(name: String) -> void:
	match name:
		"perf":
			show_perf = not show_perf
			perf_label.visible = show_perf


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
	for g in VIEW_GROUPS:
		var sub := _submenu(pm, String(g[0]))
		for t in g[1]:
			_add(sub, String(t[1]), "toggle", t[0], int(t[2]), true, 1 if toggle_state.get(t[0], false) else 0)
	_add(pm, "Map legend", "menu", "legend", KEY_F4)
	pm.add_separator("Panels")
	_add(pm, "Order of battle", "toggle", "OOB", KEY_F11, true, 1 if toggle_state.get("OOB", false) else 0)
	_add(pm, "More reports (10 instead of 3)", "local_log", "", 0, true, 1 if log_open else 0)
	pm.add_separator("Camera")
	_add(pm, "Centre on the selection", "menu", "centre", KEY_C)
	_add(pm, "Next unit", "menu", "next_unit", KEY_TAB)
	_add(pm, "Whole map", "menu", "whole_map", KEY_M)


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


## Planning tools (MRK-26): ruler and profile, the 1 km grid, markings.
func _fill_tools(pm: PopupMenu) -> void:
	_reset(pm)
	var t: Node = main.tools
	var plan := _submenu(pm, "Planning map")
	_add(plan, "Next planning mode", "menu", "plan_next", KEY_F10)
	plan.add_separator()
	for i in plan_names.size():
		_add(plan, String(plan_names[i]), "menu", "plan_%d" % i, 0, true, 3 if plan_mode_state == i else 2)
	pm.add_separator("Measure")
	_add(pm, "Ruler", "menu", "tool_ruler", KEY_R | KEY_MASK_SHIFT, true, -1,
		"Click the start and the end, or Ctrl+drag: distance, grid bearing, travel time, elevation profile.")
	_add(pm, "Elevation profile of the selected unit's route", "menu", "tool_route_profile", 0, bool(ord["has_own"]), -1,
		"The ground along the route a unit of yours is moving on.")
	_add(pm, "Clear the ruler", "menu", "tool_clear_ruler", 0, t.ruler_on or t.profile_src == "unit")
	pm.add_separator("Map")
	_add(pm, "1 km grid and grid references", "toggle", "KmGrid", KEY_G | KEY_MASK_SHIFT, true, 1 if toggle_state.get("KmGrid", false) else 0)
	pm.add_separator("Markings")
	var keys := [KEY_O, KEY_P, KEY_T, KEY_N]
	var tips := ["A named objective at a point.", "A named line: click its points, right-click or Enter to finish.",
		"A numbered target reference point (TRP 01, 02, ...).", "A short note on the map."]
	for k in MapTools.MARK_NAMES.size():
		_add(pm, String(MapTools.MARK_NAMES[k]), "menu", "mark_%d" % k, keys[k] | KEY_MASK_SHIFT, true, -1, tips[k])
	_add(pm, "Clear all markings", "menu", "marks_clear", 0, not (t.marks as Array).is_empty(), -1,
		"Right-click a single marking to rename or delete it.")


func _fill_help(pm: PopupMenu) -> void:
	_reset(pm)
	_add(pm, "Controls", "menu", "help", KEY_F1)
	_add(pm, "Map legend", "menu", "legend", KEY_F4)
	_add(pm, "Contract briefing", "menu", "briefing")
	_add(pm, "Map data credits", "menu", "credits", KEY_F2)
	var dbg := _submenu(pm, "Debug")
	_add(dbg, "Simulation fidelity zones", "toggle", "Zones", KEY_Z, true, 1 if toggle_state.get("Zones", false) else 0)
	_add(dbg, "Performance readout", "local", "perf", 0, true, 1 if show_perf else 0)


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
			_fill_marking(pm, ctx)
			_add(pm, "Centre the map here", "cmd", "centre_here")
			_add(pm, "Look from here (line of sight)", "cmd", "look_here")
			_fill_map_tools(pm)
			_add(pm, "Whole map", "menu", "whole_map", KEY_M)
			pm.add_separator()
		_note(pm, "Select one of your units to give orders")
		return
	# Short menu (MRK-35): the moves, Hold and Fall back up top; the rest in submenus.
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
	var more := _submenu(pm, "More orders")
	_add(more, "Regroup on the HQ", "cmd", "regroup", KEY_Y, true, -1, "Form up around the company HQ (or whoever took over from it).")
	_add(more, "Rest & resupply", "cmd", "rest", KEY_N, true, -1, "At the HQ (or whoever took over from it): double refill rate and replacement squads, faster recovery.")
	if ord["can_dismount"]:
		_add(more, "Dismount the squads", "cmd", "dismount", KEY_COMMA, true, -1, "The vehicles halt and the squads get out to move and fight on foot.")
	if ord["can_mount"]:
		_add(more, "Mount up", "cmd", "mount", KEY_COMMA, true, -1, "The squads walk back to their vehicles and climb aboard (2 min once together).")
	if ord["can_reinforce"]:
		_add(more, "Request reinforcements", "cmd", "reinforce", 0, true, -1, "Ask the rear for the vehicles, guns or trucks the unit has lost. They reach the trains in two to three days and join once the unit is there. There is no price, only the wait.")
	if ord["detached"]:
		_add(more, "Rejoin team", "cmd", "rejoin", KEY_L | KEY_MASK_SHIFT, true, -1, "A member that left its formation on its own orders takes its place by the leader again.")
	var st := _submenu(pm, "Standing orders")
	var fire := _submenu(st, "Fire: " + _choice_name(ROES, int(ord["roe"])))
	for i in ROES.size():
		_add(fire, String(ROES[i][0]), "cmd", "roe_%d" % i, KEY_X if i == 0 else 0, true, 3 if int(ord["roe"]) == i else 2, String(ROES[i][1]))
	var brk := _submenu(st, "Break contact: " + _choice_name(BREAKS, int(ord["brk"])).to_lower())
	for i in BREAKS.size():
		_add(brk, String(BREAKS[i][0]), "cmd", "brk_%d" % i, 0, true, 3 if int(ord["brk"]) == i else 2, String(BREAKS[i][1]))
	var so := _submenu(st, "Stand-off: " + _standoff_label(int(ord["standoff"])))
	for s2 in STANDOFFS:
		var v: int = s2[1]
		_add(so, String(s2[0]), "cmd", "standoff_%d" % v, 0, true, 3 if int(ord["standoff"]) == v else 2, _standoff_tip(v))
	var dso := _submenu(st, "Company default stand-off: " + _standoff_label(int(ord["default_standoff"])))
	for i in range(1, STANDOFFS.size()):
		var v: int = STANDOFFS[i][1]
		_add(dso, String(STANDOFFS[i][0]), "cmd", "default_standoff_%d" % v, 0, true, 3 if int(ord["default_standoff"]) == v else 2,
			"Company standing order for every unit set to Default. Applies at once.")
	var team := _submenu(pm, "Team" + ((": " + String(ord["team"])) if String(ord["team"]) != "" else ""))
	_fill_team(team)
	if at_hex:
		var here := _submenu(pm, "Map here")
		_add(here, "Centre the map here", "cmd", "centre_here")
		_add(here, "Look from here (line of sight)", "cmd", "look_here")
		_fill_map_tools(here)
		if ctx.has("holding") or ctx.has("marking"):
			pm.add_separator()
		_fill_place(pm, ctx, true)
		_fill_marking(pm, ctx, true)
	if String(ord["delay"]) != "":
		pm.add_separator()
		_note(pm, "Orders arrive in %s (command delay)" % ord["delay"])


## The marking under the cursor (MRK-26): rename or delete it.
func _fill_marking(pm: PopupMenu, ctx: Dictionary, nested := false) -> void:
	if not ctx.has("marking"):
		return
	var mk: Dictionary = ctx["marking"]
	var i := int(mk["i"])
	var head := "%s: %s" % [MapTools.MARK_NAMES[int(mk["kind"])], String(mk["name"])]
	if nested:
		pm = _submenu(pm, head)
	else:
		pm.add_separator(head)
	_add(pm, "Rename", "menu", "mark_rename_%d" % i)
	_add(pm, "Delete", "menu", "mark_delete_%d" % i)


## Ruler and markings at the hex under the cursor (MRK-26).
func _fill_map_tools(pm: PopupMenu) -> void:
	_add(pm, "Measure from here", "menu", "measure_here", 0, true, -1, "The ruler starts here: click the end.")
	var mark := _submenu(pm, "Mark here")
	for k in MapTools.MARK_NAMES.size():
		var label := String(MapTools.MARK_NAMES[k])
		if k == MapTools.MK_PHASE:
			label = "Phase line from here"
		_add(mark, label, "menu", "mark_here_%d" % k)


## The place under the cursor: select it, and for the company's own places raise militia,
## requisition, and set a supply route from its stores.
func _fill_place(pm: PopupMenu, ctx: Dictionary, nested := false) -> void:
	if not ctx.has("holding"):
		return
	var hd: Dictionary = ctx["holding"]
	var own: bool = hd.get("own", false)
	var head := "%s · %s" % [String(hd["name"]), String(hd["kind_name"])]
	if nested:
		pm = _submenu(pm, head)
	else:
		pm.add_separator(head)
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
	top_bar = bar
	bar.add_theme_stylebox_override("panel", _box(Color(0.07, 0.08, 0.07, 0.94), Color(0.45, 0.5, 0.4, 0.6), 0, Vector4(6, 4, 10, 4)))
	bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	add_child(bar)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	bar.add_child(row)

	var menus := [["Game", _fill_game], ["View", _fill_view], ["Time", _fill_time], ["Orders", _fill_orders_bar],
		["Tools", _fill_tools], ["Help", _fill_help]]
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
	perf_label.visible = false # Help > Debug > Performance readout
	row.add_child(perf_label)


# --- Unit panel ------------------------------------------------------------------------

func _build_unit_panel() -> void:
	unit_panel = PanelContainer.new()
	unit_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	unit_panel.custom_minimum_size = Vector2(RIGHT_W, 0)
	unit_panel.visible = false
	add_child(unit_panel)
	unit_scroll = ScrollContainer.new()
	unit_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	unit_panel.add_child(unit_scroll)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	unit_scroll.add_child(col)
	unit_col = col
	unit_text = _rich(14)
	col.add_child(unit_text)
	details_toggle = _fold_button("details", "Details", "Engagement times and what slows them, sight and weapon ranges, fire support, stand-off, chain of command.")
	col.add_child(details_toggle)
	details_text = _rich(13)
	details_text.visible = false
	col.add_child(details_text)
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
	var oc := "#9fc8ff" if own else ("#ff8c7a" if int(hd["owner"]) == 1 else ("#a8e090" if int(hd["owner"]) == 2 else "#c8c8c0"))
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
	orders_box.add_theme_constant_override("separation", 4)
	parent.add_child(orders_box)
	orders_box.add_child(HSeparator.new())
	var head := HBoxContainer.new()
	orders_box.add_child(head)
	var ol := _label("Orders", 13, AMBER)
	ol.tooltip_text = "Right-click the map for the order menu at any time."
	ol.mouse_filter = Control.MOUSE_FILTER_PASS
	head.add_child(ol)
	var sp := Control.new()
	sp.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(sp)
	delay_label = _label("", 12, MUTED)
	head.add_child(delay_label)

	orders_body = VBoxContainer.new()
	orders_body.add_theme_constant_override("separation", 4)
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
			["Resupply", "rest", "Rest & resupply at the HQ (or whoever took over from it): double refill rate and replacement squads, faster recovery. Key N"]]:
		var name: String = d[1]
		var b := _small_button(d[0], false, d[2])
		b.pressed.connect(func() -> void: command.emit(name))
		acts.add_child(b)
	# Only when they apply: squads in and out of their vehicles, and a member back to its team.
	extra_row = HBoxContainer.new()
	extra_row.add_theme_constant_override("separation", 4)
	orders_body.add_child(extra_row)
	mount_button = _small_button("Dismount", false, "APC/IFV platoons: dismount the squads, or mount them up again. Key , (comma)")
	mount_button.pressed.connect(func() -> void: command.emit("mount_toggle"))
	rejoin_button = _small_button("Rejoin team", false, "A team member that left its formation on its own orders takes its place by the leader again. Key Shift+L")
	rejoin_button.pressed.connect(func() -> void: command.emit("rejoin"))
	rejoin_button.visible = false
	extra_row.add_child(mount_button)
	extra_row.add_child(rejoin_button)

	orders_hint = _label("", 12, HINT)
	orders_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	orders_hint.custom_minimum_size.x = RIGHT_W - 34
	orders_hint.visible = false
	orders_body.add_child(orders_hint)

	# Standing orders: folded to a one-line summary.
	standing_toggle = _fold_button("standing", "Standing orders", "When the selection may fire, when it falls back on its own, how far it keeps from known enemies; and the company default stand-off.")
	orders_box.add_child(standing_toggle)
	standing_body = GridContainer.new()
	standing_body.columns = 4
	standing_body.add_theme_constant_override("h_separation", 6)
	standing_body.add_theme_constant_override("v_separation", 4)
	orders_box.add_child(standing_body)
	roe_opt = _dropdown(ROES, [0, 1, 2], "roe_%d", "When the unit may open fire. Key X cycles.")
	brk_opt = _dropdown(BREAKS, [0, 1, 2], "brk_%d", "When the unit falls back on its own.")
	var so_items: Array = []
	var so_vals: Array = []
	for s in STANDOFFS:
		so_items.append([s[0], _standoff_tip(int(s[1]))])
		so_vals.append(s[1])
	standoff_opt = _dropdown(so_items, so_vals, "standoff_%d", "Stay and fight, or keep known enemies at a distance: halt short of it and pull back out to it.")
	default_opt = _dropdown(so_items.slice(1), so_vals.slice(1), "default_standoff_%d", "Company standing order for every unit set to Default. Applies at once.")
	for pair in [["Fire", roe_opt], ["Break off", brk_opt], ["Stand-off", standoff_opt], ["Co. default", default_opt]]:
		var l := _label(pair[0], 12, MUTED)
		l.custom_minimum_size.x = 60
		standing_body.add_child(l)
		standing_body.add_child(pair[1])

	# Team: folded to its name, formation and spread.
	team_toggle = _fold_button("team", "Team", "Lock units under one leader: formation, spread, form, disband, select. Keys J, Shift+J, L, K.")
	orders_box.add_child(team_toggle)
	team_body = VBoxContainer.new()
	team_body.add_theme_constant_override("separation", 4)
	orders_box.add_child(team_body)
	var tgrid := GridContainer.new()
	tgrid.columns = 4
	tgrid.add_theme_constant_override("h_separation", 6)
	team_body.add_child(tgrid)
	shape_opt = _dropdown(SHAPES, [0, 1, 2, 3], "shape_%d", "The team's formation around its leader.")
	var sp_items: Array = []
	for i in SPREADS.size():
		sp_items.append([SPREAD_NAMES[i], "Distance between formation places. 500 m is the doctrinal platoon spacing (300–600 m)."])
	spread_opt = _dropdown(sp_items, SPREADS, "spread_%d", "Distance between the team's formation places.")
	for pair in [["Formation", shape_opt], ["Spread", spread_opt]]:
		var l := _label(pair[0], 12, MUTED)
		l.custom_minimum_size.x = 60
		tgrid.add_child(l)
		tgrid.add_child(pair[1])
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
	team_body.add_child(team_menu)
	_apply_folds()


## One-line summaries on the folded sections.
func _update_fold_summaries() -> void:
	var roe := int(ord["roe"])
	var brk := int(ord["brk"])
	var fire := (_choice_name(ROES, roe).to_lower() if roe >= 0 else "mixed fire")
	var brk_t := (_choice_name(BREAKS, brk).to_lower() if brk >= 0 else "mixed")
	var so := int(ord["standoff"])
	var so_t := "S/O " + ((_standoff_label(int(ord["default_standoff"])) + " (co.)") if so == 255 else _standoff_label(so))
	_set_fold_summary(standing_toggle, "%s · %s · %s" % [fire, brk_t, so_t])
	if ord["in_team"]:
		var sh := int(ord["shape"])
		var spc := SPREADS.find(int(ord["spacing"]))
		_set_fold_summary(team_toggle, "%s · %s · %s" % [String(ord["team"]) if String(ord["team"]) != "" else "several teams",
			String(SHAPES[sh][0]).to_lower() if sh >= 0 and sh < SHAPES.size() else "mixed", SPREAD_NAMES[spc] if spc >= 0 else "mixed"])
	else:
		_set_fold_summary(team_toggle, "form one (J)" if ord["can_form"] else "none")


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
	var t := ("Next right-click sends %s (Esc cancels)." % names[mode]) if mode >= 0 else ""
	if pending:
		t += (" " if t != "" else "") + "Fire / break-contact change on its way."
	orders_hint.text = t
	orders_hint.visible = t != ""
	_update_fold_summaries()


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
	_update_fold_summaries()


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
	_update_fold_summaries()


## Teams: whether the selection holds a detached member (shows Rejoin team).
func set_reinforce_state(can: bool) -> void:
	ord["can_reinforce"] = can


func set_rejoin_state(detached: bool) -> void:
	ord["detached"] = detached
	rejoin_button.visible = detached
	extra_row.visible = mount_button.visible or rejoin_button.visible


## Mounted infantry: what the comma key / Dismount button would do for the selection.
func set_mount_state(can_dismount: bool, can_mount: bool) -> void:
	ord["can_dismount"] = can_dismount
	ord["can_mount"] = can_mount
	mount_button.text = "Mount up" if can_mount and not can_dismount else "Dismount"
	mount_button.visible = can_dismount or can_mount
	extra_row.visible = mount_button.visible or rejoin_button.visible


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


## Replacements (MRK-10): squads short, and whether the company's pool is filling them.
func _replacements_line(info: Dictionary) -> String:
	var n := int(info.get("squads_short", 0))
	if n <= 0:
		return ""
	var men := int(info["replace_men"])
	var pool := int(info.get("replacements_pool", 0))
	var t := "[color=#c8f0a0]Replacements[/color]  %d squad%s short (%d men each) · " % [n, "" if n == 1 else "s", men]
	if pool < men:
		return t + "[color=#ff6b4a]%d men in the pool: requisition men at an own town[/color]\n" % pool
	if info.get("at_trains", false):
		var r := float(info.get("replace_per_hour", 1.0))
		var rate := ("%d squad%s an hour" % [int(r), "" if int(r) == 1 else "s"]) if r >= 1.0 else ("a squad every %d hours" % int(round(1.0 / r)))
		return t + "[color=#5fd35f]filling at the trains, %s[/color]\n" % rate
	return t + "%d men waiting: halt within %.1f km of the HQ, out of the fight\n" % [pool, float(info.get("trains_km", 1.5))]


## "2 d 5 h" / "7 h" / "40 min" for a number of minutes.
func _wait_text(minutes: int) -> String:
	if minutes >= 1440:
		return "%d d %d h" % [minutes / 1440, (minutes % 1440) / 60]
	return "%d h" % (minutes / 60) if minutes >= 60 else "%d min" % minutes


## Reinforcements (MRK-07): vehicles, guns or trucks the unit is short of, or the request on its way.
func _reinforcement_line(info: Dictionary) -> String:
	if info.has("reinf_count"):
		var n := int(info["reinf_count"])
		var what := "%d vehicle%s" % [n, "" if n == 1 else "s"]
		var due := int(info["reinf_due_in"])
		if due > 0:
			return "[color=#c8f0a0]Reinforcements[/color]  %s on the way, at the trains in about %s\n" % [what, _wait_text(due)]
		if info.get("reinf_at_trains", false):
			return "[color=#5fd35f]Reinforcements[/color]  %s joining\n" % what
		return "[color=#c8f0a0]Reinforcements[/color]  %s waiting at the trains: halt within %.1f km of the HQ, out of the fight\n" % [what, float(info.get("trains_km", 1.5))]
	var short := int(info.get("vehicles_short", 0))
	if short > 0:
		return "[color=#ffc040]%d vehicle%s lost[/color] · request reinforcements\n" % [short, "" if short == 1 else "s"]
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


# --- Planning map panel (MRK-25) ----------------------------------------------------------
# Next to the reports when a planning mode is on: the mode, its colour key (from the core,
# so it always matches the map) and, for line of sight, the eye height and what is seen.

var plan_names: Array = ["Off"]


func _build_plan_panel() -> void:
	plan_panel = PanelContainer.new()
	plan_panel.add_theme_stylebox_override("panel", _panel_style(0.88))
	plan_panel.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_offsets(plan_panel, 652, -8, 1032, -8)
	plan_panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	plan_panel.visible = false
	add_child(plan_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	plan_panel.add_child(col)
	var head := HBoxContainer.new()
	col.add_child(head)
	head.add_child(_label("Planning map", 14, AMBER))
	plan_pick = OptionButton.new()
	plan_pick.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	plan_pick.tooltip_text = "F10 steps through the modes."
	plan_pick.item_selected.connect(func(i: int) -> void: menu_action.emit("plan_%d" % i))
	head.add_child(plan_pick)
	var close := Button.new()
	close.text = "Off"
	close.tooltip_text = "Planning map off"
	close.pressed.connect(func() -> void: menu_action.emit("plan_0"))
	head.add_child(close)
	plan_keys = VBoxContainer.new()
	plan_keys.add_theme_constant_override("separation", 2)
	col.add_child(plan_keys)
	plan_eye_row = HBoxContainer.new()
	col.add_child(plan_eye_row)
	plan_eye_row.add_child(_label("Eye height", 13, MUTED))
	plan_eye_pick = OptionButton.new()
	for e in PLAN_EYES:
		plan_eye_pick.add_item(String(e[1]))
	plan_eye_pick.select(1)
	plan_eye_pick.item_selected.connect(func(i: int) -> void: menu_action.emit("sight_eye_%d" % i))
	plan_eye_row.add_child(plan_eye_pick)
	plan_info = _label("", 13, TEXT)
	plan_info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	plan_info.custom_minimum_size = Vector2(RIGHT_W - 34, 0)
	col.add_child(plan_info)


# --- Sandbox panel (MRK-31) --------------------------------------------------------------

const SBX_KINDS := [["Tank platoon", "Own 4x T-55A, hostile 3x T-72M"], ["Mech platoon", "Own 3x BMP-1, hostile 3x BMP-2, with their squads aboard"],
	["Rifle platoon", "Own 35 men with RPG-7, hostile 30 men"], ["Recon section", "Own 2x BRDM-2, hostile BRDM-2"],
	["Battery", "Own 4x 2S1, hostile 3x 2S1"], ["HQ", "Own UAZ-469 with R-145 radio, hostile BTR-60PU"],
	["Supply column", "6x Ural-375D (own side only)"]]
## Civilian kinds (MRK-34), in World CivKind order.
const SBX_CIV_KINDS := [["Car", "Trabant, Wartburg or Polski Fiat: drives to a town nearby"],
	["Bus", "Ikarus coach: drives to a town nearby"], ["Tractor", "Farm tractor: drives to a town nearby, slowly"],
	["Lorry", "IFA W50 or Star flatbed: drives to a town nearby"],
	["Police car", "Volkspolizei or Milicja Obywatelska: based in the nearest town"],
	["Fire engine", "Based in the nearest town; goes out to places that were hit once it is quiet"],
	["Ambulance", "Based in the nearest town; goes out with the fire brigade"]]
var _sbx_kind_civ := false


func _build_sandbox_panel() -> void:
	sandbox_panel = PanelContainer.new()
	sandbox_panel.add_theme_stylebox_override("panel", _panel_style(0.9))
	# Top left, beside the order of battle and under the contract line: clear of the unit
	# panel (top right) and the reports, planning and ruler panels (bottom).
	sandbox_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_offsets(sandbox_panel, 356, 104, 676, 104)
	sandbox_panel.visible = false
	add_child(sandbox_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	sandbox_panel.add_child(col)
	var head := _label("Sandbox", 15, AMBER)
	head.tooltip_text = "No victory or defeat. Tools for setting up fights to test."
	head.mouse_filter = Control.MOUSE_FILTER_PASS
	col.add_child(head)
	var row := HBoxContainer.new()
	col.add_child(row)
	sbx_side = OptionButton.new()
	sbx_side.focus_mode = Control.FOCUS_NONE
	sbx_side.add_item("Own", 0)
	sbx_side.add_item("Hostile", 1)
	sbx_side.add_item("Civilian", 2)
	sbx_side.tooltip_text = "Which side the Place tool puts units down for"
	sbx_side.item_selected.connect(func(i: int) -> void: menu_action.emit("sbx_side_%d" % i))
	row.add_child(sbx_side)
	sbx_kind = OptionButton.new()
	sbx_kind.focus_mode = Control.FOCUS_NONE
	sbx_kind.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for i in SBX_KINDS.size():
		sbx_kind.add_item(String(SBX_KINDS[i][0]), i)
		sbx_kind.set_item_tooltip(i, String(SBX_KINDS[i][1]))
	sbx_kind.tooltip_text = "What the Place tool puts down (the scenario's kit for that side)"
	sbx_kind.item_selected.connect(func(i: int) -> void: menu_action.emit("sbx_kind_%d" % i))
	row.add_child(sbx_kind)
	var row2 := HBoxContainer.new()
	col.add_child(row2)
	sbx_place = _small_button("Place (Ins)", true, "Left-click the map to put a unit on the nearest free land hex. Ins or Esc ends it.")
	sbx_place.pressed.connect(func() -> void: menu_action.emit("sbx_place_tool"))
	row2.add_child(sbx_place)
	var del := _small_button("Delete selected (Del)", false, "Take the selected units off the map (squads go with their vehicles). Not a kill or a loss.")
	del.pressed.connect(func() -> void: menu_action.emit("sbx_delete"))
	row2.add_child(del)
	sbx_delete_tool = _small_button("Click-delete (Shift+Del)", true, "Left-click a unit, own or hostile, to take it off the map.")
	sbx_delete_tool.pressed.connect(func() -> void: menu_action.emit("sbx_delete_tool"))
	col.add_child(sbx_delete_tool)
	var row3 := HBoxContainer.new()
	col.add_child(row3)
	row3.add_child(_label("Start", 13, MUTED))
	sbx_day = OptionButton.new()
	sbx_day.focus_mode = Control.FOCUS_NONE
	for d in range(14, 21):
		sbx_day.add_item("%d April" % d, d)
	sbx_day.item_selected.connect(func(i: int) -> void: menu_action.emit("sbx_day_%d" % (14 + i)))
	row3.add_child(sbx_day)
	sbx_hour = OptionButton.new()
	sbx_hour.focus_mode = Control.FOCUS_NONE
	for h in 24:
		sbx_hour.add_item("%02d:00" % h, h)
	sbx_hour.item_selected.connect(func(i: int) -> void: menu_action.emit("sbx_hour_%d" % i))
	row3.add_child(sbx_hour)
	sbx_time_note = _label("", 12, MUTED)
	col.add_child(sbx_time_note)
	sbx_ai = CheckBox.new()
	sbx_ai.text = "Hostile AI"
	sbx_ai.focus_mode = Control.FOCUS_NONE
	sbx_ai.tooltip_text = "Off: hostiles make no moves of their own (no hunting, no patrols).\nThey still fire by their rules of engagement and rearm."
	sbx_ai.toggled.connect(func(on: bool) -> void: menu_action.emit("sbx_ai_%s" % ("on" if on else "off")))
	col.add_child(sbx_ai)
	sbx_reveal = CheckBox.new()
	sbx_reveal.text = "Show all hostiles"
	sbx_reveal.focus_mode = Control.FOCUS_NONE
	sbx_reveal.tooltip_text = "Display only: every hostile is drawn where it is, seen or not. Spotting is unchanged."
	sbx_reveal.toggled.connect(func(on: bool) -> void: menu_action.emit("sbx_reveal_%s" % ("on" if on else "off")))
	col.add_child(sbx_reveal)


## Fills the sandbox panel. `d`: visible, side, kind, tool ("", "place", "delete"), day, hour,
## clock_started, hostile_ai, reveal.
func show_sandbox(d: Dictionary) -> void:
	sandbox_panel.visible = d.get("visible", false)
	if not sandbox_panel.visible:
		return
	sbx_side.select(int(d.get("side", 0)))
	var civ := int(d.get("side", 0)) == 2
	if civ != _sbx_kind_civ:
		_sbx_kind_civ = civ
		sbx_kind.clear()
		var kinds: Array = SBX_CIV_KINDS if civ else SBX_KINDS
		for i in kinds.size():
			sbx_kind.add_item(String(kinds[i][0]), i)
			sbx_kind.set_item_tooltip(i, String(kinds[i][1]))
	sbx_kind.select(int(d.get("kind", 0)))
	sbx_place.set_pressed_no_signal(String(d.get("tool", "")) == "place")
	sbx_delete_tool.set_pressed_no_signal(String(d.get("tool", "")) == "delete")
	sbx_day.select(clampi(int(d.get("day", 14)) - 14, 0, 6))
	sbx_hour.select(clampi(int(d.get("hour", 6)), 0, 23))
	var locked: bool = d.get("clock_started", false)
	sbx_day.disabled = locked
	sbx_hour.disabled = locked
	sbx_time_note.text = "Locked: the clock has moved." if locked else "Can be changed until the clock first moves."
	sbx_ai.set_pressed_no_signal(d.get("hostile_ai", true))
	sbx_reveal.set_pressed_no_signal(d.get("reveal", true))


# --- Order of battle panel ---------------------------------------------------------------

const OOB_SEL_BG := Color(0.55, 0.42, 0.08, 0.55)   # the selection (amber, as on the map)
const OOB_SUB_BG := Color(0.18, 0.4, 0.62, 0.45)    # under a selected commander (blue, as on the map)
const OOB_SUB := Color("#7fd0ff")
const OOB_TYPE_ABBR := {"HQ": "HQ", "Tank Plt": "Tank", "Mech Plt": "Mech", "Rifle Plt": "Rifle", "Recon Sect": "Recon",
	"Arty Bty": "Arty", "Supply Col": "Trucks"}


func _build_oob_panel() -> void:
	oob_panel = PanelContainer.new()
	oob_panel.add_theme_stylebox_override("panel", _panel_style(0.88))
	# Left edge, between the contract line at the top and the reports at the bottom.
	oob_panel.anchor_left = 0.0
	oob_panel.anchor_right = 0.0
	oob_panel.anchor_top = 0.0
	oob_panel.anchor_bottom = 1.0
	_offsets(oob_panel, 8, 134, 348, -272)
	oob_panel.visible = false
	add_child(oob_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	oob_panel.add_child(col)
	var head := HBoxContainer.new()
	col.add_child(head)
	var title := _label("Order of battle", 15, AMBER)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title.tooltip_text = "Who answers to whom, by the chain of command the orders travel down:\nteam members under their leader, everyone else under the company HQ,\nsquads under their APC/IFV platoon."
	title.mouse_filter = Control.MOUSE_FILTER_PASS
	head.add_child(title)
	var fold := _small_button("Unfold", false, "Unfold every branch")
	fold.pressed.connect(func() -> void: _oob_fold_all(false))
	head.add_child(fold)
	var fold2 := _small_button("Fold", false, "Fold every team and platoon")
	fold2.pressed.connect(func() -> void: _oob_fold_all(true))
	head.add_child(fold2)
	var close := _small_button("×", false, "Hide the panel (F11)")
	close.pressed.connect(func() -> void: toggle_pressed.emit("OOB"))
	head.add_child(close)
	for b in [fold, fold2, close]:
		(b as Button).size_flags_horizontal = Control.SIZE_SHRINK_END
	oob_tree = Tree.new()
	oob_tree.columns = 3
	oob_tree.hide_root = true
	oob_tree.select_mode = Tree.SELECT_ROW
	oob_tree.focus_mode = Control.FOCUS_NONE
	oob_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	oob_tree.scroll_horizontal_enabled = false
	oob_tree.set_column_expand(0, true)
	oob_tree.set_column_expand(1, false)
	oob_tree.set_column_custom_minimum_width(1, 40)
	oob_tree.set_column_expand(2, false)
	oob_tree.set_column_custom_minimum_width(2, 88)
	oob_tree.set_column_clip_content(0, true)
	oob_tree.add_theme_stylebox_override("panel", StyleBoxEmpty.new())
	oob_tree.add_theme_stylebox_override("focus", StyleBoxEmpty.new())
	# The tree's own selection is not shown: rows are tinted from the map's selection instead.
	for sb in ["selected", "selected_focus", "cursor", "cursor_unfocused"]:
		oob_tree.add_theme_stylebox_override(sb, StyleBoxEmpty.new())
	oob_tree.add_theme_stylebox_override("hovered", _box(Color(1, 1, 1, 0.06), Color(0, 0, 0, 0), 2, Vector4(0, 0, 0, 0)))
	oob_tree.add_theme_color_override("font_color", TEXT)
	oob_tree.add_theme_color_override("font_selected_color", TEXT)
	oob_tree.add_theme_color_override("guide_color", Color(0.45, 0.5, 0.4, 0.35))
	oob_tree.add_theme_color_override("relationship_line_color", Color(0.45, 0.5, 0.4, 0.55))
	oob_tree.add_theme_color_override("parent_hl_line_color", Color(0.6, 0.66, 0.5, 0.8))
	oob_tree.add_theme_constant_override("draw_relationship_lines", 1)
	oob_tree.add_theme_constant_override("relationship_line_width", 1)
	oob_tree.add_theme_constant_override("parent_hl_line_width", 1)
	oob_tree.add_theme_constant_override("item_margin", 14)
	oob_tree.add_theme_constant_override("v_separation", 2)
	oob_tree.add_theme_constant_override("h_separation", 4)
	oob_tree.add_theme_constant_override("inner_item_margin_left", 2)
	oob_tree.add_theme_font_size_override("font_size", 13)
	oob_tree.item_mouse_selected.connect(_on_oob_click)
	oob_tree.item_activated.connect(_on_oob_activated)
	oob_tree.item_collapsed.connect(func(it: TreeItem) -> void:
		var key = it.get_metadata(0).get("key", "")
		if key != "":
			oob_folded[key] = it.collapsed)
	col.add_child(oob_tree)
	oob_foot = _label("", 12, MUTED)
	oob_foot.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	oob_foot.custom_minimum_size = Vector2(300, 0)
	col.add_child(oob_foot)


func set_oob_visible(on: bool) -> void:
	oob_panel.visible = on
	set_toggle("OOB", on)


func _on_oob_click(_pos: Vector2, button: int) -> void:
	var it := oob_tree.get_selected()
	oob_tree.deselect_all()
	if it == null or button != MOUSE_BUTTON_LEFT:
		return
	var md: Dictionary = it.get_metadata(0)
	match String(md.get("kind", "")):
		"unit":
			oob_pick.emit(int(md["id"]), "add" if Input.is_key_pressed(KEY_SHIFT) or Input.is_key_pressed(KEY_CTRL) else "select")
		"company":
			oob_pick.emit(int(md["id"]), "company")


func _on_oob_activated() -> void:
	var it := oob_tree.get_selected()
	oob_tree.deselect_all()
	if it == null:
		return
	var md: Dictionary = it.get_metadata(0)
	if String(md.get("kind", "")) == "unit":
		oob_pick.emit(int(md["id"]), "command")


func _oob_fold_all(fold: bool) -> void:
	var root := oob_tree.get_root()
	if root == null:
		return
	var stack: Array[TreeItem] = [root]
	while not stack.is_empty():
		var it: TreeItem = stack.pop_back()
		for c in it.get_children():
			stack.append(c)
		if it == root or it.get_child_count() == 0:
			continue
		var md: Dictionary = it.get_metadata(0)
		if String(md.get("kind", "")) == "company" or String(md.get("kind", "")) == "section":
			it.collapsed = false if String(md.get("kind", "")) == "company" else fold
		else:
			it.collapsed = fold
		oob_folded[String(md.get("key", ""))] = it.collapsed


## Fills the panel. `rows`: MurksWorld.get_oob(); `selected`: unit ids selected on the map;
## `under`: unit id -> true for everything under a selected commander; `children`: unit id ->
## its subordinates (by the chain of command, as main works it out).
func show_oob(rows: Array, selected: Array, under: Dictionary, children: Dictionary) -> void:
	if not oob_panel.visible:
		return
	var by_id := {}
	for r in rows:
		by_id[int(r["id"])] = r
	# Rebuild only when the shape of the tree changes (a team formed, a unit lost, squads out).
	var sig := ""
	for r in rows:
		sig += "%d:%d:%d:%s:%s:%d;" % [int(r["id"]), int(r["parent"]), 1 if r["alive"] else 0, r["role"], r.get("group_name", ""),
			1 if r["embarked"] else 0]
	if sig != oob_sig:
		oob_sig = sig
		_rebuild_oob(rows, by_id, children)
	# Update the rows in place.
	var placed := {}
	for id in oob_items:
		var it: TreeItem = oob_items[id]
		var r: Dictionary = by_id.get(id, {})
		if r.is_empty():
			continue
		_fill_oob_row(it, r, children, by_id)
		var bg := Color(0, 0, 0, 0)
		if selected.has(id):
			bg = OOB_SEL_BG
		elif under.has(id):
			bg = OOB_SUB_BG
		for c in 3:
			it.set_custom_bg_color(c, bg)
		placed[id] = true
	# A new selection made on the map: unfold down to it and scroll it into view.
	if selected != oob_sel_seen:
		oob_sel_seen = selected.duplicate()
		if selected.size() >= 1 and oob_items.has(int(selected[0])):
			var it: TreeItem = oob_items[int(selected[0])]
			var p := it.get_parent()
			while p != null:
				p.collapsed = false
				p = p.get_parent()
			oob_tree.scroll_to_item(it)
	# Footer: what a selected commander has under it.
	var foot := ""
	for id in selected:
		var r: Dictionary = by_id.get(int(id), {})
		if r.is_empty() or not children.has(int(id)):
			continue
		var n := _count_under(int(id), children, by_id)
		foot = "%s commands %d unit%s, marked blue here and on the map." % [r["name"], n, "" if n == 1 else "s"]
		break
	if foot == "":
		foot = "Click: select · Shift-click: add · Double-click: the unit and everything under it, centred."
	oob_foot.text = foot


func _count_under(id: int, children: Dictionary, by_id: Dictionary) -> int:
	var n := 0
	for c in children.get(id, []):
		var r: Dictionary = by_id.get(int(c), {})
		if not r.is_empty() and r["alive"] and not r["embarked"]:
			n += 1
		n += _count_under(int(c), children, by_id)
	return n


func _rebuild_oob(rows: Array, by_id: Dictionary, children: Dictionary) -> void:
	var first_shown := oob_tree.get_item_at_position(Vector2(10, 10))
	var keep_key := ""
	if first_shown != null:
		keep_key = String(first_shown.get_metadata(0).get("key", ""))
	oob_tree.clear()
	oob_items.clear()
	var root := oob_tree.create_item()
	# One branch per own company, in the order the companies come.
	var companies: Array[int] = []
	var militia: Array = []
	for r in rows:
		if r["militia"]:
			militia.append(r)
		elif not companies.has(int(r["formation_id"])):
			companies.append(int(r["formation_id"]))
	var restore: TreeItem = null
	for fid in companies:
		var sample := -1
		var alive := 0
		var total := 0
		for r in rows:
			if int(r["formation_id"]) == fid and not r["militia"]:
				total += 1
				if r["alive"]:
					alive += 1
					if sample < 0:
						sample = int(r["id"])
		var cname := ""
		for r in rows:
			if int(r["formation_id"]) == fid:
				cname = String(r["formation"])
				break
		var ci := oob_tree.create_item(root)
		ci.set_text(0, cname)
		ci.set_custom_color(0, AMBER)
		ci.set_text(1, "%d" % alive)
		ci.set_tooltip_text(1, "%d of %d units still in being" % [alive, total])
		ci.set_custom_color(1, MUTED)
		ci.set_tooltip_text(0, "%s: click to select the whole company." % cname)
		ci.set_metadata(0, {"kind": "company", "id": sample, "key": "co%d" % fid})
		ci.set_selectable(0, sample >= 0)
		# Tops of the chain: the HQ (or, with the HQ lost, everyone it commanded), then the rest
		# whose commander is not in the list.
		var dead: Array = []
		for r in rows:
			if int(r["formation_id"]) != fid or r["militia"]:
				continue
			if not r["alive"]:
				dead.append(r)
				continue
			var par := int(r["parent"])
			var pr: Dictionary = by_id.get(par, {})
			if par < 0 or pr.is_empty() or not pr["alive"]:
				_add_oob_branch(ci, r, by_id, children)
		if not dead.is_empty():
			var di := oob_tree.create_item(ci)
			di.set_text(0, "Knocked out (%d)" % dead.size())
			di.set_custom_color(0, Color(0.6, 0.6, 0.56))
			di.set_metadata(0, {"kind": "section", "key": "ko%d" % fid})
			di.set_selectable(0, false)
			di.set_selectable(1, false)
			di.set_selectable(2, false)
			di.collapsed = oob_folded.get("ko%d" % fid, true)
			for r in dead:
				var it := oob_tree.create_item(di)
				it.set_metadata(0, {"kind": "dead", "id": int(r["id"]), "key": "u%d" % int(r["id"])})
				for c in 3:
					it.set_selectable(c, false)
				oob_items[int(r["id"])] = it
		ci.collapsed = oob_folded.get("co%d" % fid, false)
	if not militia.is_empty():
		var mi := oob_tree.create_item(root)
		mi.set_text(0, "Militia garrisons")
		mi.set_custom_color(0, Color(0.85, 0.78, 0.55))
		mi.set_tooltip_text(0, "Town garrisons raised from the company's holdings. They hold their town and take no orders.")
		mi.set_metadata(0, {"kind": "section", "key": "militia"})
		for c in 3:
			mi.set_selectable(c, false)
		for r in militia:
			if r["alive"]:
				var it := oob_tree.create_item(mi)
				it.set_metadata(0, {"kind": "unit", "id": int(r["id"]), "key": "u%d" % int(r["id"])})
				oob_items[int(r["id"])] = it
		mi.collapsed = oob_folded.get("militia", false)
	# Back to where the list was scrolled.
	if keep_key != "":
		var stack: Array[TreeItem] = [root]
		while not stack.is_empty() and restore == null:
			var it: TreeItem = stack.pop_back()
			for c in it.get_children():
				stack.append(c)
			if it != root and String(it.get_metadata(0).get("key", "")) == keep_key:
				restore = it
		if restore != null:
			oob_tree.scroll_to_item(restore, false)


func _add_oob_branch(parent: TreeItem, r: Dictionary, by_id: Dictionary, children: Dictionary) -> void:
	var id := int(r["id"])
	var it := oob_tree.create_item(parent)
	it.set_metadata(0, {"kind": "unit", "id": id, "key": "u%d" % id})
	oob_items[id] = it
	var kids: Array = children.get(id, [])
	for c in kids:
		var cr: Dictionary = by_id.get(int(c), {})
		if not cr.is_empty() and cr["alive"]:
			_add_oob_branch(it, cr, by_id, children)
	if it.get_child_count() > 0:
		it.collapsed = oob_folded.get("u%d" % id, false)


func _fill_oob_row(it: TreeItem, r: Dictionary, children: Dictionary, by_id: Dictionary) -> void:
	var tn := String(r["type_name"])
	var abbr: String = OOB_TYPE_ABBR.get(tn, tn)
	var nm := String(r["name"])
	# "1st Tank" + "Tank" reads twice: add the type only where the name doesn't say it.
	var label := nm if nm.containsn(abbr) or nm.containsn(tn) else "%s %s" % [nm, abbr]
	var role := String(r["role"])
	if r["militia"]:
		label = "%s (%s)" % [nm, r.get("holding_name", "town")]
	elif by_id.has(int(r["parent"])) and int(by_id[int(r["parent"])]["id"]) >= 0 and String(r["type_name"]) != String(by_id[int(r["parent"])]["type_name"]) and nm.containsn("dismount"):
		label = "Squads" # an APC/IFV platoon's own squads, listed under their vehicles
	elif role == "leader":
		label += "  ▸ " + String(r.get("group_name", "Team"))
	elif role == "acting":
		label += "  (acting HQ)"
	if r["embarked"]:
		label += " · aboard"
	it.set_text(0, label)
	var name_col := TEXT
	if not r["alive"]:
		name_col = Color(0.55, 0.55, 0.52)
	elif role == "hq":
		name_col = Color(1, 0.95, 0.8)
	elif int(r.get("group_color", -1)) >= 0:
		name_col = TEAM_COLORS[int(r["group_color"]) % TEAM_COLORS.size()]
	elif r["embarked"]:
		name_col = MUTED
	it.set_custom_color(0, name_col)
	# Elements in the fight, coloured as on the counters.
	var em := int(r["elements_max"])
	var ea := int(r["elements"])
	it.set_text(1, "%d/%d" % [ea, em])
	it.set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT)
	it.set_custom_color(1, Color(0.55, 0.55, 0.52) if not r["alive"] else (Color.WHITE if ea >= em else (Color("#ffcc4d") if ea * 2 >= em else Color("#ff6b4a"))))
	var st := String(r["status"])
	if r["pending"]:
		st = "Order sent"
	it.set_text(2, st)
	var sc := MUTED
	match st:
		"Engaged": sc = Color(1.0, 0.7, 0.45)
		"Pinned", "Knocked out": sc = Color("#ff6b4a") if st == "Pinned" else Color(0.55, 0.55, 0.52)
		"Falling back": sc = Color(1.0, 0.85, 0.4)
		"Order sent": sc = HINT
		"Holding", "Aboard": sc = MUTED
		_: sc = Color(0.7, 0.85, 1.0)
	it.set_custom_color(2, sc)
	# Tooltip: what it is and how it stands to its commander.
	var tip := "%s — %s" % [nm, tn]
	if String(r.get("equipment", "")) != "":
		tip += ", " + String(r["equipment"])
	var par := int(r["parent"])
	var pr: Dictionary = by_id.get(par, {})
	if not pr.is_empty() and r["alive"]:
		var link := int(r.get("link", -1))
		var how := ""
		match link:
			1: how = ", inside its zone"
			2: how = ", outside its zone: orders ×1.5"
			3: how = ", beyond radio range: orders by courier"
		tip += "\nAnswers to %s%s" % [pr["name"], how]
	elif r["alive"] and not r["militia"] and role != "hq":
		tip += "\nNo HQ to answer to: orders improvised (+25 min)"
	if children.has(int(r["id"])):
		var n := _count_under(int(r["id"]), children, by_id)
		if n > 0:
			tip += "\nCommands %d unit%s" % [n, "" if n == 1 else "s"]
	if r["alive"] and not r["embarked"]:
		tip += "\nClick: select · Double-click: it and everything under it"
	for c in 3:
		it.set_tooltip_text(c, tip)


## Shows the panel for `mode` (0 hides it). `key`: rows from MurksWorld.get_plan_key.
## `info`: a line under the key (line of sight: the point and what is seen).
func show_plan(mode: int, names: Array, key: Array, info := "", eye_index := 1) -> void:
	plan_mode_state = mode
	plan_names = names
	plan_panel.visible = mode != 0
	if mode == 0:
		return
	plan_pick.clear()
	for n in names:
		plan_pick.add_item(String(n))
	plan_pick.select(mode)
	for c in plan_keys.get_children():
		c.queue_free()
	for row in key:
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 8)
		plan_keys.add_child(line)
		var tint: Color = row[0]
		if tint.a > 0.0:
			# The swatch is what the shader does to a mid-green patch of ground.
			var g := Color(0.50, 0.56, 0.34)
			var l := g.r * 0.30 + g.g * 0.55 + g.b * 0.15
			var base := g.lerp(Color(l, l, l), 0.35)
			var sw := ColorRect.new()
			sw.color = base.lerp(Color(tint.r, tint.g, tint.b), tint.a)
			sw.custom_minimum_size = Vector2(26, 16)
			sw.size_flags_vertical = Control.SIZE_SHRINK_CENTER
			line.add_child(sw)
		else:
			var gap := Control.new()
			gap.custom_minimum_size = Vector2(26, 0)
			line.add_child(gap)
		var text := VBoxContainer.new()
		text.add_theme_constant_override("separation", 0)
		line.add_child(text)
		text.add_child(_label(String(row[1]), 13))
		if String(row[2]) != "":
			var note := _label(String(row[2]), 11, MUTED)
			note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			note.custom_minimum_size = Vector2(330, 0)
			text.add_child(note)
	plan_eye_row.visible = mode == MapView.PLAN_SIGHT
	plan_eye_pick.select(eye_index)
	plan_info.text = info
	plan_info.visible = info != ""


# --- Ruler and elevation profile (MRK-26) ----------------------------------------------

func _build_ruler_panel() -> void:
	ruler_panel = PanelContainer.new()
	ruler_panel.add_theme_stylebox_override("panel", _panel_style(0.9))
	ruler_panel.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_offsets(ruler_panel, 1044, -8, 1490, -8)
	ruler_panel.grow_vertical = Control.GROW_DIRECTION_BEGIN
	ruler_panel.visible = false
	add_child(ruler_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 4)
	ruler_panel.add_child(col)
	var head := HBoxContainer.new()
	col.add_child(head)
	ruler_title = _label("Ruler", 14, AMBER)
	ruler_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(ruler_title)
	var close := Button.new()
	close.text = "Clear"
	close.tooltip_text = "Take the ruler off the map (Esc)"
	close.pressed.connect(func() -> void: menu_action.emit("tool_clear_ruler"))
	head.add_child(close)
	ruler_line = _label("", 14)
	col.add_child(ruler_line)
	ruler_refs = _label("", 12, MUTED)
	col.add_child(ruler_refs)
	ruler_travel = VBoxContainer.new()
	ruler_travel.add_theme_constant_override("separation", 2)
	col.add_child(ruler_travel)
	var mode_row := HBoxContainer.new()
	ruler_travel.add_child(mode_row)
	var ml := _label("Travel time, full readiness:", 13, MUTED)
	ml.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mode_row.add_child(ml)
	ruler_mode_pick = OptionButton.new()
	for m in MapTools.MOVE_MODES:
		ruler_mode_pick.add_item(String(m[0]))
	ruler_mode_pick.tooltip_text = "Move mode for the route search and times (as orders use it)."
	ruler_mode_pick.item_selected.connect(func(i: int) -> void: menu_action.emit("ruler_mode_%d" % i))
	mode_row.add_child(ruler_mode_pick)
	var group := ButtonGroup.new()
	for i in MapTools.MOBILITY.size():
		var b := Button.new()
		b.toggle_mode = true
		b.button_group = group
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.add_theme_font_size_override("font_size", 13)
		b.tooltip_text = "Show this route on the map (dotted). The route a unit of this kind would take, found as orders find it; a tired unit is up to 40% slower."
		b.pressed.connect(func() -> void: menu_action.emit("ruler_mob_%d" % i))
		ruler_travel.add_child(b)
		ruler_rows.append(b)
	ruler_prof_row = HBoxContainer.new()
	col.add_child(ruler_prof_row)
	ruler_prof_row.add_child(_label("Profile", 13, MUTED))
	var sg := ButtonGroup.new()
	ruler_src_line = _small_button("Straight line", true, "Along the ruler, with the line of sight from its start to its end.")
	ruler_src_line.button_group = sg
	ruler_src_line.pressed.connect(func() -> void: menu_action.emit("profile_line"))
	ruler_prof_row.add_child(ruler_src_line)
	ruler_src_route = _small_button("Along the route", true, "Along the route shown on the map.")
	ruler_src_route.button_group = sg
	ruler_src_route.pressed.connect(func() -> void: menu_action.emit("profile_route"))
	ruler_prof_row.add_child(ruler_src_route)
	ruler_eye_pick = OptionButton.new()
	for e in PLAN_EYES:
		ruler_eye_pick.add_item("eye " + String(e[1]).split(":")[0])
	ruler_eye_pick.tooltip_text = "Eye height at the start of the sight line (the planning map's line of sight uses the same)."
	ruler_eye_pick.item_selected.connect(func(i: int) -> void: menu_action.emit("sight_eye_%d" % i))
	ruler_prof_row.add_child(ruler_eye_pick)
	ruler_chart = ProfileChart.new()
	col.add_child(ruler_chart)
	ruler_summary = _label("", 12)
	ruler_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	ruler_summary.custom_minimum_size = Vector2(RIGHT_W - 34, 0)
	col.add_child(ruler_summary)


## The ruler panel. `d` empty hides it. Keys: title, line, refs, live (while the end follows the
## cursor), rows (travel lines per mobility), shown, mode, src ("line", "route"; absent for a
## unit's route), profile (MurksWorld.get_profile), eye_index, summary.
func show_ruler(d: Dictionary) -> void:
	ruler_panel.visible = not d.is_empty()
	if d.is_empty():
		return
	ruler_title.text = String(d.get("title", "Ruler"))
	ruler_line.text = String(d.get("line", ""))
	ruler_refs.text = String(d.get("refs", ""))
	ruler_refs.visible = ruler_refs.text != ""
	var live: bool = d.get("live", false)
	var rows: Array = d.get("rows", [])
	ruler_travel.visible = not live and not rows.is_empty()
	for i in ruler_rows.size():
		ruler_rows[i].text = String(rows[i]) if i < rows.size() else ""
		ruler_rows[i].set_pressed_no_signal(i == int(d.get("shown", 0)))
	ruler_mode_pick.select(int(d.get("mode", 0)))
	var prof: Dictionary = d.get("profile", {})
	ruler_prof_row.visible = not live and d.has("src")
	ruler_src_line.set_pressed_no_signal(String(d.get("src", "")) == "line")
	ruler_src_route.set_pressed_no_signal(String(d.get("src", "")) == "route")
	ruler_eye_pick.select(int(d.get("eye_index", 1)))
	ruler_eye_pick.visible = String(d.get("src", "")) == "line"
	ruler_chart.visible = not live and not prof.is_empty()
	ruler_chart.set_profile(prof)
	ruler_summary.text = String(d.get("summary", ""))
	ruler_summary.visible = not live and ruler_summary.text != ""
	_fit_ruler.call_deferred()


## The ruler sits in the tool slot of the right dock (MRK-35).
func _fit_ruler() -> void:
	_layout()


# --- Name and confirm dialog (markings) -------------------------------------------------

func _build_ask() -> void:
	ask_panel = PanelContainer.new()
	ask_panel.add_theme_stylebox_override("panel", _box(Color(0.07, 0.08, 0.07, 1.0), Color(0.6, 0.62, 0.45, 0.8), 6, Vector4(18, 12, 18, 14)))
	ask_panel.set_anchors_preset(Control.PRESET_CENTER)
	ask_panel.grow_horizontal = Control.GROW_DIRECTION_BOTH
	ask_panel.grow_vertical = Control.GROW_DIRECTION_BOTH
	ask_panel.visible = false
	add_child(ask_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 8)
	col.custom_minimum_size = Vector2(380, 0)
	ask_panel.add_child(col)
	ask_title = _label("", 18, AMBER)
	col.add_child(ask_title)
	ask_prompt = _label("", 13, MUTED)
	ask_prompt.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	col.add_child(ask_prompt)
	ask_edit = LineEdit.new()
	ask_edit.max_length = 40
	ask_edit.text_submitted.connect(func(_t: String) -> void: _ask_done(true))
	col.add_child(ask_edit)
	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_END
	col.add_child(buttons)
	var cancel := _button("Cancel")
	cancel.pressed.connect(func() -> void: _ask_done(false))
	buttons.add_child(cancel)
	var ok := _button("OK")
	ok.pressed.connect(func() -> void: _ask_done(true))
	buttons.add_child(ok)


## Asks for a line of text; `on_done` gets it on OK (Enter). Esc or Cancel drops it.
func ask_text(title: String, prompt: String, default_text: String, on_done: Callable) -> void:
	ask_title.text = title
	ask_prompt.text = prompt
	ask_edit.visible = true
	ask_edit.text = default_text
	ask_has_text = true
	ask_cb = on_done
	ask_panel.visible = true
	ask_edit.grab_focus()
	ask_edit.select_all()


## Asks to confirm; `on_yes` is called on OK.
func ask_confirm(title: String, text: String, on_yes: Callable) -> void:
	ask_title.text = title
	ask_prompt.text = text
	ask_edit.visible = false
	ask_has_text = false
	ask_cb = on_yes
	ask_panel.visible = true


func _ask_done(ok: bool) -> void:
	if not ask_panel.visible:
		return
	ask_panel.visible = false
	ask_edit.release_focus()
	var cb := ask_cb
	ask_cb = Callable()
	if ok and cb.is_valid():
		if ask_has_text:
			cb.call(ask_edit.text.strip_edges())
		else:
			cb.call()


# --- Log, status line, banners ---------------------------------------------------------

func _build_log() -> void:
	log_panel = PanelContainer.new()
	log_panel.add_theme_stylebox_override("panel", _panel_style(0.82))
	log_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	add_child(log_panel)
	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 0)
	log_panel.add_child(col)
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 4)
	col.add_child(head)
	var title := _label("Reports", 13, AMBER)
	title.tooltip_text = "Click a report to centre the map on it."
	title.mouse_filter = Control.MOUSE_FILTER_PASS
	head.add_child(title)
	head.add_child(VSeparator.new())
	for i in LOG_FILTERS.size():
		var fb := _button(String(LOG_FILTERS[i][0]), true)
		fb.flat = true
		fb.add_theme_font_size_override("font_size", 12)
		fb.set_pressed_no_signal(i == 0)
		fb.pressed.connect(func() -> void:
			log_filter = i
			for k in log_filter_buttons.size():
				log_filter_buttons[k].set_pressed_no_signal(k == i)
			_render_log())
		head.add_child(fb)
		log_filter_buttons.append(fb)
	var sp := Control.new()
	sp.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(sp)
	log_fold = _button("▴ More", false)
	log_fold.flat = true
	log_fold.add_theme_font_size_override("font_size", 12)
	log_fold.tooltip_text = "Show the latest 10 reports, or only the latest 3."
	log_fold.pressed.connect(func() -> void:
		log_open = not log_open
		log_fold.text = "▾ Less" if log_open else "▴ More"
		_render_log())
	head.add_child(log_fold)
	log_box = VBoxContainer.new()
	log_box.add_theme_constant_override("separation", 0)
	col.add_child(log_box)


func _build_status() -> void:
	var sp := PanelContainer.new()
	sp.add_theme_stylebox_override("panel", _panel_style(0.75))
	sp.set_anchors_preset(Control.PRESET_TOP_LEFT)
	sp.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(sp)
	status_panel = sp
	var op := PanelContainer.new()
	op.add_theme_stylebox_override("panel", _panel_style(0.8))
	op.set_anchors_preset(Control.PRESET_TOP_LEFT)
	op.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(op)
	objective_panel = op
	objective_label = RichTextLabel.new()
	objective_label.bbcode_enabled = true
	objective_label.fit_content = true
	objective_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
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
	title_new_game = _menu_button("New game", "new_game_placeholder", "Not built yet.")
	title_new_game.disabled = true
	buttons.add_child(title_new_game)
	title_campaign = _menu_button("Campaign", "campaign_placeholder", "Not built yet.")
	title_campaign.disabled = true
	buttons.add_child(title_campaign)
	title_sandbox = _menu_button("Sandbox", "sandbox",
		"The map and the contract scenario with tools on top: place and delete units for either side,\nset the start time, switch the hostile AI off. No victory or defeat.")
	buttons.add_child(title_sandbox)
	title_three_way = _menu_button("Three-way (1v1v1)", "three_way",
		"Company Murks, the 7th Guards Remnant and Kessler's Free Corps on one map, each at war with\nthe other two. You win once both of them are broken, by your hand or each other's.")
	buttons.add_child(title_three_way)
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
	return title_screen.visible or pause_screen.visible or ask_panel.visible


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
			["H", "Hold position"], ["B  Y  N", "Fall back, regroup on the HQ, rest & resupply"], ["Shift+L", "Rejoin team (a detached member)"],
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
		["Planning tools", [["Ctrl+drag, Shift+R", "Ruler: distance, bearing, travel time, profile"],
			["Shift+G", "1 km grid and grid references"],
			["Shift+O  P  T  N", "Mark an objective, phase line, TRP, note"],
			["Right-click a marking", "Rename or delete it"]]],
		["Game", [["F5 / F9", "Quick save / quick load"], ["F1 / F2", "Controls / map data credits"],
			["F4 / F6", "Map legend / painted terrain textures on or off"],
			["F7", "Counters show vehicles or NATO symbols"],
			["F10", "Planning map: going, cover, slope, line of sight (steps through)"],
			["F11", "Order of battle panel: click a row to select, double-click for the whole command"],
			["F8", "Topographic map: contour lines and heights"],
			["Esc", "Pause menu"]]]]
	var cols := HBoxContainer.new()
	cols.add_theme_constant_override("separation", 28)
	var halves := [[0, 1, 5], [2, 3, 6, 4]]
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


## Closes the name dialog (cancelled), or a controls, credits or legend dialog if one is
## open; true if it did.
func close_dialog() -> bool:
	if ask_panel.visible:
		_ask_done(false)
		return true
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
	var ground := Color8(126, 140, 82)   # open ground, as the far-zoom palette (render_data.cpp base_color)
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


## Docks every panel (MRK-35): the selection on the right with the tool panels (sandbox,
## planning key, ruler) stacked under it from the bottom up; the contract line, the order
## of battle and the reports on the left; the hover readout, banners and toasts over the map
## between them. Panels keep their size from their content; the unit panel scrolls when the
## right column is too short for it.
func _layout() -> void:
	if top_bar == null or unit_panel == null or log_panel == null or oob_panel == null:
		return
	var vs := size
	if vs.x < 400.0 or vs.y < 300.0:
		return
	var top := top_bar.size.y + 6.0
	var rx := vs.x - DOCK_PAD - RIGHT_W
	var bottom := vs.y - DOCK_PAD
	for p in [ruler_panel, plan_panel, sandbox_panel]:
		var pc := p as Control
		if pc.visible:
			var h := pc.get_combined_minimum_size().y
			_dock(pc, rx, bottom - h, RIGHT_W, h)
			bottom -= h + 6.0
	var avail := bottom - top - 14.0
	var content := unit_col.get_combined_minimum_size().y
	unit_scroll.custom_minimum_size = Vector2(RIGHT_W - 20.0, clampf(content, 40.0, maxf(40.0, avail)))
	_dock(unit_panel, rx, top, RIGHT_W, 0.0)
	_dock(holding_panel, rx, top, RIGHT_W, 0.0)
	var map_r := rx - DOCK_PAD
	objective_label.custom_minimum_size.x = maxf(200.0, map_r - DOCK_PAD - 22.0)
	_dock(objective_panel, DOCK_PAD, top, map_r - DOCK_PAD, 0.0)
	var below := top + objective_panel.get_combined_minimum_size().y + 6.0
	var lh := log_panel.get_combined_minimum_size().y
	_dock(log_panel, DOCK_PAD, vs.y - DOCK_PAD - lh, map_r - DOCK_PAD, lh)
	var left_r := DOCK_PAD
	if oob_panel.visible:
		_dock(oob_panel, DOCK_PAD, below, LEFT_W, maxf(120.0, vs.y - DOCK_PAD - lh - 6.0 - below))
		left_r = DOCK_PAD + LEFT_W + 6.0
	_dock(status_panel, left_r, below, 0.0, 0.0)
	var cx := (left_r + map_r) * 0.5
	var half := minf(390.0, (map_r - left_r) * 0.5 - 10.0)
	_dock(banner, cx - half, below + 46.0, half * 2.0, 30.0)
	_dock(toast, cx - half, below + 132.0, half * 2.0, 26.0)


func _dock(c: Control, x: float, y: float, w: float, h: float) -> void:
	c.anchor_left = 0.0
	c.anchor_top = 0.0
	c.anchor_right = 0.0
	c.anchor_bottom = 0.0
	c.grow_horizontal = Control.GROW_DIRECTION_END
	c.grow_vertical = Control.GROW_DIRECTION_END
	c.offset_left = x
	c.offset_top = y
	c.offset_right = x + w
	c.offset_bottom = y + h


func refresh(time_text: String, night: bool, stats: Dictionary, fps: float, map_stats: String) -> void:
	time_label.text = time_text + ("  night" if night else "")
	stock_label.text = "Diesel %s L%s   Ammo %.1f t%s   Food %s kg%s   Men %d%s   %s DM%s" % [
		_thousands(int(stats["diesel_l"])), _days(float(stats.get("days_diesel", 1e9))),
		float(stats["ammo_t"]), _days(float(stats.get("days_ammo", 1e9))),
		_thousands(int(stats["food_kg"])), _days(float(stats.get("days_food", 1e9))),
		int(stats["personnel"]), (" (+%d)" % int(stats["replacements"])) if int(stats.get("replacements", 0)) > 0 else "",
		_thousands(int(stats.get("cash", 0.0))), _days(float(stats.get("days_cash", 1e9)), 3.0)]
	stock_label.tooltip_text = "Company stocks; days of supply at the recent rate of use (shown on the bar under 30 days, cash under 3): diesel %s, ammunition %s, food %s. Men on the rolls (+ replacements waiting: they fill knocked-out infantry squads at the trains); cash, which pays the men %s DM a day at 06:00 (%s of cash; an unpaid company loses morale). %d holding%s, %d trucks, %d route%s running." % [
		_days_tip(float(stats.get("days_diesel", 1e9))), _days_tip(float(stats.get("days_ammo", 1e9))), _days_tip(float(stats.get("days_food", 1e9))),
		_thousands(int(stats.get("wage_bill", 0.0))), _days_tip(float(stats.get("days_cash", 1e9))),
		int(stats.get("holdings_owned", 0)), "" if int(stats.get("holdings_owned", 0)) == 1 else "s", int(stats.get("trucks", 0)),
		int(stats.get("routes_active", 0)), "" if int(stats.get("routes_active", 0)) == 1 else "s"]
	perf_label.visible = show_perf
	perf_label.text = "%d fps · sim %.0f us/min · %d units\nhot %d / warm %d / cold %d · %s" % [
		int(fps), float(stats["avg_tick_us"]), int(stats["alive"]), int(stats["hot"]), int(stats["warm"]), int(stats["cold"]), map_stats]
	var now := Time.get_ticks_msec()
	if now > banner_until: banner.text = ""
	if now > toast_until: toast.text = ""
	_layout()


func set_status(text: String) -> void:
	status_label.text = text
	status_label.get_parent().visible = text != ""


func set_events(events: Array) -> void:
	log_events = events
	_render_log()


## The reports that pass the filter, newest first: 3 folded, 10 open.
func _render_log() -> void:
	for c in log_box.get_children():
		c.queue_free()
	var kinds: Array = LOG_FILTERS[log_filter][1]
	var limit := 10 if log_open else 3
	var shown := 0
	for e in log_events:
		if shown >= limit:
			break
		var kind: String = e["kind"]
		if not kinds.is_empty() and not kinds.has(kind):
			continue
		shown += 1
		var b := _button("%s   %s" % [String(e["time_string"]).substr(11), e["text"]])
		b.flat = true
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		b.clip_text = true # long reports end at the panel edge; the full text is in the tooltip
		b.tooltip_text = String(e["text"])
		b.add_theme_font_size_override("font_size", 13)
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
	if shown == 0:
		log_box.add_child(_label("   nothing yet" if kinds.is_empty() else "   none of these yet", 12, MUTED))
	_layout.call_deferred()


func show_unit(info: Dictionary, count: int) -> void:
	if info.is_empty():
		unit_panel.visible = false
		return
	unit_panel.visible = true
	var t := ""
	if int(info["side"]) != 0:
		orders_box.visible = false
		details_toggle.visible = false
		details_text.visible = false
		t += "[b]%s[/b]\n" % info["name"]
		if info.get("seen", false):
			t += "In sight now\n"
		else:
			t += "Last seen %d min ago\n" % int(info.get("last_seen_minutes_ago", 0))
		t += "%s\nStrength unknown." % info.get("location", "")
		unit_text.text = t
		return
	orders_box.visible = true
	details_toggle.visible = true
	t += "[b]%s[/b]   %s" % [info["name"], info["type_name"]]
	if count > 1:
		t += "   [color=#9fc8ff]+%d more selected[/color]" % (count - 1)
	t += "\n[color=#b8c0a8]%s · %s · %s[/color]\n" % [info["formation"], info["equipment"], info["location"]]
	if info.has("elements_max"):
		# Combat effectiveness: each element is in the fight or knocked out.
		var em := int(info["elements_max"])
		var ea := int(info["elements_active"])
		var pc := "#5fd35f" if ea >= em else ("#ffc040" if ea * 2 >= em else "#ff5040")
		t += "[color=%s]%s[/color][color=#666]%s[/color]  %d of %d %s in the fight\n" % [pc, "■".repeat(ea),
			"□".repeat(em - ea), ea, em, info["element_noun"]]
		var sp := float(info.get("suppression", 0.0))
		if sp >= 1.0:
			var scol := "#ff5040" if info.get("pinned", false) else ("#ffc040" if sp >= 30.0 else "#b8c0a8")
			t += "Suppression  [color=%s]%d%s[/color]\n" % [scol, int(sp),
				"  PINNED: gone to ground, won't move forward, spots at half range" if info.get("pinned", false) else ""]
	else:
		t += _stat("Strength", float(info["strength"])) + "\n"
	t += _stats_line(float(info["morale"]), float(info["fuel"]), float(info["ammo"]), float(info["readiness"]))
	t += "%s · %s\n" % [_situation(info), info["order"]]
	t += _replacements_line(info)
	t += _reinforcement_line(info)
	if info.get("pending", false):
		t += "[color=#ffd84a]%s order arrives in %d min[/color]\n" % [info["pending_kind"], int(info["pending_in"])]
	if info.has("eta_minutes"):
		var eta := int(info["eta_minutes"])
		t += "Route %.1f km, about %dh%02d left\n" % [float(info["route_km"]), eta / 60, eta % 60]
	t += _mounted_line(info)
	unit_text.text = t.strip_edges()

	var d := ""
	if info.has("elements_max"):
		var ef := float(info["engage_factor"])
		var ec := "#9fc8ff" if ef < 1.25 else ("#ffc040" if ef < 1.8 else "#ff5040")
		d += "Engagement   [color=%s]acquire %.1f min · fire every %.1f min[/color]\n" % [ec, float(info["acquire_min"]), float(info["cycle_min"])]
		d += "[color=#b8c0a8]  readiness ×%.2f · morale ×%.2f · ammo ×%.2f · suppressed ×%.2f · night ×%.2f[/color]\n" % [
			float(info["engage_readiness"]), float(info["engage_morale"]), float(info["engage_ammo"]), float(info.get("engage_supp", 1.0)),
			float(info.get("engage_night", 1.0))]
	if info.has("gun_name"):
		d += "%s · range %.1f km\n" % [info["gun_name"], float(info["gun_range_km"])]
	d += "Sight %.1f km · weapons %.1f km\n" % [float(info["spot_range_km"]), float(info["weapon_range_km"])]
	d += _fire_support_line(info)
	d += _standoff_text(info)
	d += _team_line(info)
	d += "%s · breaks contact: %s\n" % [info["roe_name"], String(info["break_contact_name"]).to_lower()]
	d += "New orders take [b]%d min[/b] to arrive" % int(info["command_delay"])
	details_text.text = d
	_set_fold_summary(details_toggle, "")
	_apply_folds()


## Morale, fuel, ammo, readiness on one line, each number coloured by how low it is.
func _stats_line(mor: float, fuel: float, ammo: float, ready: float) -> String:
	return "%s   %s   %s   %s\n" % [_stat("Morale", mor), _stat("Fuel", fuel), _stat("Ammo", ammo), _stat("Ready", ready)]


func _stat(name: String, v: float) -> String:
	var c := "#5fd35f" if v >= 60.0 else ("#ffc040" if v >= 30.0 else "#ff5040")
	return "[color=#b8c0a8]%s[/color] [color=%s]%d[/color]" % [name, c, int(round(v))]


## Several own units: totals and averages; the order of battle panel lists them. `company`
## is the company (or team) name when the selection is exactly one, else "".
func show_group(infos: Array, company: String) -> void:
	if infos.is_empty():
		unit_panel.visible = false
		return
	unit_panel.visible = true
	orders_box.visible = true
	details_toggle.visible = true
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
	elif company != "":
		t += "[b]%s[/b]   whole company\n" % company
	else:
		t += "[b]%d units selected[/b]\n" % n
	t += "[color=#b8c0a8]%d platoons and sections · %d men[/color]\n" % [n, men]
	var pc := "#5fd35f" if el_act >= el_max else ("#ffc040" if el_act * 2 >= el_max else "#ff5040")
	t += "[color=%s]%d of %d[/color] vehicles, guns and squads in the fight\n" % [pc, el_act, el_max]
	t += _stats_line(sums["morale"] / n, sums["fuel"] / n, sums["ammo"] / n, sums["readiness"] / n)
	var bits: Array[String] = []
	if engaged > 0:
		bits.append("[color=#ff6a50]%d in combat[/color]" % engaged)
	if hot > 0:
		bits.append("[color=#ff9a50]%d near enemies[/color]" % hot)
	if warm > 0:
		bits.append("[color=#ffc040]%d within 40 km[/color]" % warm)
	bits.append("%d moving, %d holding" % [moving, n - moving])
	if pending > 0:
		bits.append("[color=#ffd84a]%d with orders on the way[/color]" % pending)
	t += " · ".join(bits)
	unit_text.text = t
	var d := ""
	if company.begins_with("Team "):
		d += _team_line(infos[0])
	d += "Engagement   [color=#b8c0a8]average ×%.2f the time of fresh crews[/color]\n" % (ef_sum / n)
	d += "New orders take [b]%s[/b] to arrive" % (("%d min" % delay_lo) if delay_lo == delay_hi else ("%d-%d min" % [delay_lo, delay_hi]))
	details_text.text = d
	_apply_folds()


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
	if obj.get("sandbox", false):
		objective_label.text = "[b]Sandbox:[/b] no victory or defeat · hostile AI %s   ·   HQ [color=%s]%s[/color]   ·   kills %d, lost %d" % [
			"on" if obj.get("hostile_ai", true) else "[color=#ffc040]off[/color]", "#7fe07f" if hq_ok else "#ff6050", hq_text,
			int(obj.get("kills", 0)), int(obj.get("losses", 0))]
		return
	var t := ""
	var three: bool = obj.get("three_way", false)
	match int(obj.get("outcome", 0)):
		1: t += "[color=#7fe07f][b]Victory.[/b][/color]  "
		2: t += "[color=#ff6050][b]Defeat.[/b][/color]  "
		_: t += "[b]Three-way:[/b] " if three else "[b]Contract:[/b] "
	if three:
		var rs := int(obj.get("rival_start", 0))
		var re := int(obj.get("rival_effective", 0))
		t += "break the [color=#ff9a80]%s[/color] [color=%s]%d/%d[/color] and [color=#c9a6e8]%s[/color] [color=%s]%d/%d[/color]   ·   " % [
			obj.get("enemy_name", "enemy"), "#7fe07f" if ee == 0 else "#ffc040", ee, es,
			obj.get("rival_name", "rival"), "#7fe07f" if re == 0 else "#ffc040", re, rs]
	else:
		t += "break the %s: [color=%s]%d of %d[/color] fighting units left   ·   " % [obj.get("enemy_name", "enemy"),
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
	var three: bool = obj.get("three_way", false)
	end_title.text = ("Last one standing" if three else "Contract fulfilled") if victory else "Company Murks is finished"
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
	if three:
		t += "\n%s: %d of %d units still fighting" % [obj.get("rival_name", ""), int(obj.get("rival_effective", 0)),
			int(obj.get("rival_start", 0))]
	end_text.text = t
	end_load_button.disabled = not has_save
	end_panel.visible = true


func _bar(name: String, v: float) -> String:
	var n := int(round(clampf(v, 0, 100) / 10.0))
	var col := "#5fd35f" if v >= 60 else ("#ffc040" if v >= 30 else "#ff5040")
	return "%-10s [color=%s]%s[/color][color=#444]%s[/color] %d\n" % [name, col, "■".repeat(n), "■".repeat(10 - n), int(v)]


## " (12 d)" for a finite days-of-supply figure, "" when nothing is being used.
func _days_tip(d: float) -> String:
	return "not being used" if d >= 999.0 else ("%.1f d" % d if d < 10.0 else "%d d" % int(d))


func _days(d: float, shown_under := 30.0) -> String:
	if d >= shown_under:
		return "" # only short supplies are shown; the tooltip has every figure
	return " (%.1f d)" % d if d < 10.0 else " (%d d)" % int(d)


func _thousands(v: int) -> String:
	var s := str(abs(v))
	var out := ""
	while s.length() > 3:
		out = "," + s.substr(s.length() - 3) + out
		s = s.substr(0, s.length() - 3)
	return ("-" if v < 0 else "") + s + out
