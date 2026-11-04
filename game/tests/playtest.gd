extends SceneTree
## Headless playtest: runs the real-map scenario for several game days with a
## scripted player and reports anything that would hurt play.
## godot --headless --path game --script res://tests/playtest.gd -- [days] [mode]
##   mode: passive (never give orders) | active (scripted company orders)

func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var days := int(args[0]) if args.size() > 0 else 3
	var mode := args[1] if args.size() > 1 else "active"
	var w := MurksWorld.new()
	var data := FileAccess.get_file_as_bytes("res://data/poland_1989.murkz")
	var raw := data.slice(16).decompress(data.decode_u64(8), FileAccess.COMPRESSION_DEFLATE)
	w.load_map_bytes(raw)
	w.setup_scenario(24)
	var stride := w.get_unit_stride()
	var t0 := Time.get_ticks_msec()
	var kinds := {}
	var pauses_by_hour := {}
	var no_route := 0
	var max_tick_ms := 0.0
	var own_ids: Array[int] = []
	var snap := w.get_units_snapshot()
	for i in snap.size() / stride:
		if int(snap[i * stride + 1]) == 0:
			own_ids.append(int(snap[i * stride]))
	var home: Vector2i = w.get_unit_info(0)["hex"]
	var log_lines: Array = []
	var contact_min := 0
	var outcome_at := ""
	for minute in days * 1440:
		# Scripted player: recon forward at 07:00, company follows at 09:00, regroup at 18:00, rest next morning.
		if mode == "active":
			var tod := (w.get_time_minutes()) % 1440
			var day := w.get_time_minutes() / 1440
			if tod == 7 * 60 and day == 0:
				for id in own_ids:
					if String(w.get_unit_info(id)["type_name"]) == "Recon Sect":
						w.set_stance(id, 1, 1)
						w.order_move(id, home + Vector2i(40, 0), 2)
			if tod == 9 * 60 and day == 0:
				for id in own_ids:
					if String(w.get_unit_info(id)["type_name"]) != "Recon Sect":
						w.order_move(id, home + Vector2i(25, 0), 1)
			if tod == 18 * 60:
				for id in own_ids:
					w.order_regroup(id, false)
			if tod == 6 * 60 + 30 and day >= 1:
				for id in own_ids:
					w.order_regroup(id, true)
		var tt := Time.get_ticks_usec()
		w.advance(1, 1000.0)
		if w.player_in_contact():
			contact_min += 1
		if outcome_at == "" and int(w.get_objectives()["outcome"]) != 0:
			outcome_at = "%s  %s" % [w.get_time_string(), w.get_objectives()["outcome_text"]]
		max_tick_ms = maxf(max_tick_ms, (Time.get_ticks_usec() - tt) / 1000.0)
		for e in w.poll_events():
			kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
			if e["pause"]:
				var h := int(e["time"]) / 60
				pauses_by_hour[h] = pauses_by_hour.get(h, 0) + 1
			if String(e["text"]).contains("no route"):
				no_route += 1
			if e["kind"] in ["destroyed", "retreat", "supply"] and log_lines.size() < 60:
				log_lines.append("%s  %s" % [e["time_string"], e["text"]])
		if minute % 720 == 719:
			var st: Dictionary = w.get_stats()
			var own_alive := 0
			var own_str := 0.0
			var stuck := 0
			var out_fuel := 0
			for id in own_ids:
				var info: Dictionary = w.get_unit_info(id)
				if info["alive"]:
					own_alive += 1
					own_str += float(info["strength"])
					if float(info["fuel"]) <= 0.0:
						out_fuel += 1
			var ob: Dictionary = w.get_objectives()
			var sup: Dictionary = w.get_supply()
			print("%s  own alive %d (avg str %.0f, fuel-out %d)  enemy fighting %d/%d  own fighting %d  contact %d min  hot %d  trains %.0f L %.1f t  depot %.0f L %.1f t  column %d/%d trucks %d trips  tick %.0f us" % [
				w.get_time_string(), own_alive, own_str / maxf(1, own_alive), out_fuel, int(ob["enemy_effective"]), int(ob["enemy_start"]),
				int(ob["own_effective"]), contact_min, int(st["hot"]), float(st["diesel_l"]), float(st["ammo_t"]),
				float(sup.get("depot_diesel_l", 0)), float(sup.get("depot_ammo_t", 0)), int(sup.get("trucks", 0)),
				int(sup.get("trucks_max", 0)), int(sup.get("trips", 0)), float(st["avg_tick_us"])])
			contact_min = 0
	var hours_with_pause := pauses_by_hour.size()
	var total_pauses := 0
	var worst_hour := 0
	for h in pauses_by_hour:
		total_pauses += pauses_by_hour[h]
		worst_hour = maxi(worst_hour, pauses_by_hour[h])
	print("events by kind: ", kinds)
	print("pause-worthy events: %d over %d game hours (worst hour %d); 'no route' %d; max tick %.1f ms; wall %d ms" % [
		total_pauses, hours_with_pause, worst_hour, no_route, max_tick_ms, Time.get_ticks_msec() - t0])
	print("outcome: ", outcome_at if outcome_at != "" else "none")
	var sup_end: Dictionary = w.get_supply()
	print("supply: %d trips, %.1f t delivered; column %s; cash $%.0f" % [int(sup_end["trips"]), float(sup_end["delivered_t"]),
		sup_end["convoy_status"], float(sup_end["cash"])])
	for l in log_lines:
		print("   ", l)
	# Save / load round trip: the same two hours must play out identically after a load.
	var saved := w.save_state()
	w.advance(120, 1e9)
	var a := w.get_units_snapshot()
	var t_a := w.get_time_minutes()
	var err := w.load_state(saved)
	w.advance(120, 1e9)
	var b := w.get_units_snapshot()
	print("save/load: %d bytes, load error '%s', deterministic replay %s" % [saved.size(), err,
		"OK" if a == b and t_a == w.get_time_minutes() else "MISMATCH"])
	print("bad save rejected: '%s'" % w.load_state(saved.slice(0, 100)))
	quit()
