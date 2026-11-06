# Murks 1989 — proof of concept

A one-map mercenary wargame prototype: one continuous 500 m hex map, one pausable clock, C++ simulation core running inside Godot 4 through GDExtension.

This build proves the risky technical core of the design plan (roadmap phase 1, plus a first cut of phase 2):

- **One map, no battle screen.** The real Poland and East Germany, about 1,050 × 780 km in 3.8 million 500 m hexes, built from satellite elevation and land-cover data. Zooming in on a contact *is* the battle.
- **Company grouping on the theater map.** Zoomed out, each company's nearby platoons merge into one company counter. It shows the unit count, average strength and the company name. Detached platoons keep their own counter. Spotted hostile units are grouped by proximity only, because you don't know their organisation. Overlapping counters are pushed apart, with a leader line to their true position.
- **Command panel.** The selection window (one unit or a whole company) has an Orders section:
  - **Move modes.** Move, road march, deliberate or assault. The mode decides what a right-click orders.
  - **Group orders.** Hold, fall back, regroup on the HQ, and rest & resupply.
  - **Rules of engagement.** Fire at will, return fire only, or hold fire.
  - **Break contact.** At heavy losses, at light losses, or never.

  Every order and setting goes down the chain of command and arrives after the command delay. Counters show road-march, deliberate, assault and fire tags.
- **Counters and movement in the Forward Defense style.** Each unit is a square counter in its side's colour (NATO blue, hostile red) with a white NATO symbol box and echelon marks (dots for sections and platoons, a bar for a company or battery). Own counters show how many elements are in the fight bottom-left (white when all are, yellow from half, red below half), one pip under the counter per vehicle, gun or squad (hollow once knocked out), and a short name when zoomed in. They also show an order badge top-left (M move, R road march, D deliberate, A assault, F falling back, G regroup, S rest); a yellow corner flag when morale is shaken (under 45) and white hatching when broken (under 25) or falling back; and a bar along the top while under fire, red when just hit. Orders still on their way show a yellow "+12′" tag. A unit sits in the hex the simulation has it in; when it moves a hex the counter glides there in a quarter second. Counters grow to fill the hex when zoomed in, units sharing a hex fan out diagonally inside it, and each can be clicked. Last-seen positions are faded with a dashed outline and "?".
- **Combat effectiveness (binary per element).** Every unit is made of elements: vehicles, guns or squads ("4x T-55A" is 4 tanks, "35 men" is 3 squads; types without a count use 3 tanks or vehicles, 2 recon vehicles, 6 guns (a Warsaw Pact battery), 2 HQ vehicles). Each element is either in the fight or knocked out. Direct fire is resolved per element: every vehicle or squad still in the fight fires its aimed rounds each fire cycle (tanks 2, others 1); each round hits with the range-and-cover hit chance (about 0.32 at 1.5 km in the open, close to the 0.33–0.40 first-round hit chance in US gunnery data), and a hit knocks out one element with a chance set by the shooter's firepower (× anti-armour against armour, × 0.6–1.4, × the calibrated weight `DIRECT_FIRE_WEIGHT`; about 0.66 for a tank gun against a tank). A fire mission is one sheaf: one hit roll whose weight is resolved as whole elements, as before. Strength is 100 × elements ÷ full complement, so the break-contact and victory rules still read it.
- **Calibrated attrition.** `tests/attrition_test.cpp` stands two platoons up against each other on flat open ground and measures kills per firing element-minute. Tank against tank at 1.5 km it now gives 0.11 (recalibrated with suppression in, weight 0.073) (the Historical Values target band is 0.09–0.13: Soviet planning norms degraded to real-combat effectiveness); before this change it was 0.005. Other pairings follow from firepower and their unsourced 3-minute cycles.
- **Engagement time.** Crew condition doesn't change hit chance; it changes how long a unit takes to acquire a new target and to work through each fire cycle. Values follow the Historical Values doc: direct fire acquires in 1 minute (first round in 8–24 s for trained crews) and tanks fire a 1-minute cycle of 2 aimed rounds (other direct-fire types keep 3 minutes until sourced); a fire mission on an unplanned target takes 4 minutes for a battery, 1.25 minutes for a gun platoon of up to 3 guns, and 2 minutes to shift to a target within 1 km of the last one; guns fire 4-minute cycles. Both times are multiplied by readiness (1 + (100 − readiness) ÷ 100), morale (1 + (60 − morale) ÷ 60 below 60), ammo (up to × 2 below a quarter load), suppression (1 + suppression ÷ 50, up to × 3; see Suppression) and night (× 1.2). The morale and ammo factors are still estimates with no measured source. The unit panel shows "acquire 1.3 min · fire every 1.2 min" and each factor. Tanks use 2.5% of their load per round (about 40 rounds).
- **Suppression.** Each unit carries a suppression level from 0 to 100. Every fire cycle aimed at it raises the level, hit or miss: by (100 − level) × (1 − e^(−S/3)), where S adds up the shooters' elements times a weight per element and cycle in 155 mm-round equivalents. The weights scale from measured suppression per round (155 mm 1, 105 mm tank round 0.8, 81 mm mortar 0.7, a 5-round .50 cal burst 0.06): a tank's 2 rounds 1.6, a rifle squad's rifles and MG 1.0, an IFV 0.6, a recon vehicle 0.5, a gun by calibre (122 mm 0.79, 152 mm 0.98), a rocket launcher 4× that; elements knocked out add 1 each. Armour (× 0.3 against direct fire, × 0.5 against artillery) and woods or towns (× 0.6) shield. The level holds as long as the fire goes on (until the shooter's next cycle) and then fades by 40% a minute, so most of it is gone 2 minutes after the fire stops, which matches what's reported for artillery. Suppression slows engagement time by 1 + level ÷ 50 (in place of the old flat × 1.3 for being under fire). Soft units (not tanks or APCs) are pinned at 60 and get up again below 35: pinned, they stop moving (falling back still works, at half speed) and spot at half range, which also blinds observers calling for fire. It doesn't change hit chance, following the rule that crew condition sets timing, not effectiveness. Counters show it as an amber bar along the top edge, red with "PIN" when pinned. The unit panel shows the level. The pin thresholds and the S0 = 3 scale are estimates.
- **Gun ranges by equipment.** Guns and mortars fire to the range of the weapon in their equipment string, with the standard HE round (FM 100-2-3, Jane's): 2S1 15.3 km, D-30 15.4 km, 2S3 18.5 km, D-20 17.4 km, 2S19 and 2A65 24.7 km, 2S5 and 2A36 27 km, 2S7 37.5 km, BM-21 and RM-70 20.4 km, BM-27 35 km, 2S9 8.8 km, 120 mm mortars 5.7–7.1 km, 82 mm 3–4.3 km, and the NATO M109 family, M110, M198, FH-70, L118, M270 and others. Names match as whole words, so "2S1" doesn't catch "2S19". Rocket-assisted rounds (2S1 21.9 km, 2S3 24 km) are left out: few were issued. Guns whose equipment names none of them keep the type's 15 km. The table is `kGuns` in `core/src/sim.cpp`; the unit panel shows the gun and its range. Guns now run the full model whenever an enemy is inside their range, not only inside the 10 km hot zone, so they can fire to their full range.
- **Fire support net.** Calls for fire go over a radio band of their own, separate from the command net. The guns, the HQ and the recon sections (the artillery observers) are on it. Guns fire only on a target that someone in their formation can see and get a call through for: an observer on the net within radio range of the guns (vehicle sets 20 km, manpack 6 km) calls directly; anyone else, or an observer beyond range, passes the call to someone on the net within radio range who reaches the guns, which adds 2 minutes to the fire mission (an estimate); a unit that can reach nobody is cut off, and what only it can see can't be shelled. Both sides use the same rule.
- **Command overlays.** Two toggles outline every own counter and draw its links. *Direct command* (T): each unit is linked to the HQ or team leader it takes orders from and ringed in that commander's colour (white for the HQ, the team colour for a team leader): solid inside the commander's zone, amber dashes and "×1.5" outside it, red dashes and "COURIER" beyond radio range, "NO HQ" with no commander; the HQ has a double white ring; zone rings as before. *Fire support* (I): the guns are ringed thick with a dashed range ring and their range; observers on the net ("FO") are ringed and linked to the guns they reach; relayed callers are dashed and linked to the observer who passes their calls on; units cut off from the net are dashed grey ("NO FS"); a battery's current fire mission is an arrow to its target. With both on, the fire support ring sits outside the command ring. A selected battery shows its range ring even with the overlay off.
- **Mounted infantry.** Every APC/IFV platoon carries its own squads, one per vehicle (BMP-1 8 men, BMP-2 7, BTR-60PB 8, BTR-70/80 7, MT-LB 11, M113 11, M2 Bradley 6, Marder 6, Warrior 7 and others; the count of men only changes rations and the unit panel). *Dismount* (button or the comma key): after the command delay the vehicles halt and the squads get out into the same hex as a rifle unit of their own ("1st Mech Dismounts"), able to fire a minute later. They move, fight, take orders and break contact like any infantry while the vehicles go on as a platoon of armed carriers. *Mount up* (order either the vehicles or the squads): the vehicles halt and wait, the squads walk back to them (following if the vehicles are sent on), and two minutes after they're together and halted they're aboard. Squads aboard a knocked-out vehicle are lost with it, and squads can crowd into the vehicles left only up to two to a vehicle. While dismounted, a platoon still counts once for the victory and defeat rules (its squads count only once its vehicles no longer do). Counters show squads aboard as "+3" in green, squads walking back carry an "E" badge, and a dotted green line links squads and vehicles when either is selected. The hostile AI uses the same drill: halted within 2 km of a known enemy its squads get out and the vehicles stay back; with nothing seen within 5 km they mount up again before moving on. The 1- and 2-minute drill times and losing the squad with its vehicle are estimates.
- **Battery displacement.** Guns fire from one position for 15–20 minutes, then displace 1–1.5 km, away from the nearest known enemy and into cover when they can, before counter-battery fire would find them. It is a standing procedure, so it needs no order. Half an hour without firing also ends the fire assault.
- **Deployment.** A unit on road march deploys into a normal move once an enemy is known within 5 km (Soviet norm 4–6 km), as well as when no road is left. A team moving in column deploys into line once an enemy is known within 1.5 km.
- **Teams, command zones and formations.** Select two or more of your units and press J (or Form team in the Orders panel) to lock them under one leader: the company HQ if it is in the selection, otherwise the first unit you selected (a platoon commander). Each team gets a colour (Team A amber, B teal, C purple, ...): its counters wear a frame in that colour and the leader carries a pennant.
  - **Zone of command.** An HQ commands effectively within 4 km (a battalion area), a team leader within 1 km (a company area). Team members take orders through their leader (one more step in the chain: +3 minutes), everyone else through the company HQ. A unit outside its commander's zone gets orders 1.5 times slower; beyond radio range (20 km for vehicle sets, 6 km for units on foot with manpack sets) a courier carries them. The rule applies to both sides. The Direct command toggle (T) shows every zone and links every unit to its commander (see Command overlays); otherwise zones and links show for the selection. The unit panel says who a unit answers to, how far away, and whether it is inside the zone.
  - **Formations.** A team keeps one of four formations around its leader: Keep (the layout it had when formed or when Keep was pressed), Column, Line or Wedge, facing the direction of travel, with 500 m (the doctrinal platoon spacing), 1 km or 2 km between places. Right-click with the leader selected moves the whole team: the order goes to the leader through the command delay, and the members keep station on it, re-taking their places as it moves. The team moves at the pace of its slowest member: the leader halts while anyone who can move is more than 1.5 km out of place. Hold with the leader selected halts the team, which forms up on the leader. A member given its own order leaves the formation (thin frame) until the next team order. If the leader is destroyed, the next unit (an HQ first) takes command.
  - **Selecting.** Double-click a team member (or press L) for its whole team; K for the whole company. On the theater map each team gets its own counter.
  - **Formation pathfinding rules.** (1) Column: members follow the leader's own track, each rank (spacing × rank) hexes behind, and queue along it when the leader halts. (2) Line, wedge and keep: each member takes its place around the leader's present hex, laid out along the leader's route a few hexes ahead (so the formation wheels with the road), and re-takes it whenever the place has moved 1 km; re-routing keeps the progress already made into the next hex. (3) If a member's place is water, cut off or held by the enemy, it files in on the leader's track instead (the formation closes up, e.g. onto a bridge). (4) Defiles: if a member's own route to its place would take more than twice as long as the leader's going (a river without a bridge, marsh, deep forest beside the road), it files in on the leader's track and opens out again once past. (5) The team keeps its slowest member's pace: the leader halts while anyone who can move is more than 3 + spacing ÷ 2 hexes from its place. On a 50 km road route a column team took about a third longer than its leader alone, mostly forming up.
- **Enemy-held hexes.** Only a unit on an assault order may move into a hex the enemy holds. Anyone else halts in the hex before it, and both sides now know about each other ("1st Tank halts ...: hostile Tank Plt in the next hex. Only an assault can go in."). Routes avoid hexes held by enemies the side knows about (seen now or in the last 30 minutes), except the destination itself, so an order onto an enemy brings the unit up to it. Applies to both sides.
- **Stand-off.** Each unit can stay and fight or keep known enemies at a distance: 1, 2 or 3 km, or its own weapon range. With a stand-off, a unit on the move halts before closing inside it, and a unit an enemy comes closer to pulls back out to it (checked every 5 minutes). It is a standing order: the per-unit setting travels down the chain like fire orders, and "Default" follows the company default, which the player sets in the Orders panel and which applies at once. An assault ignores stand-off; team members go by their leader's. Counters show "S/O 2.0km" and the unit panel says what the unit does. The company default starts at "stay and fight".
- **Map marks.** Selected units show their route in blue with an arrowhead and arrival time, a dashed arrow and yellow tag for an order still travelling ("Road march +35′"), and a dashed line to their company HQ. Shots in sight draw a tracer and an impact flash, artillery a muzzle flash and a spread of bursts, fire from shooters you can't see shows as incoming bursts on your unit, and a destroyed unit explodes and leaves a wreck mark. The hex under the mouse is outlined. These are display only; they don't change any simulation result.
- **Streaming.** Far out you see a one-pixel-per-hex overview; closer in, 64 × 64-hex chunk meshes are built in C++ and streamed in and out as the camera moves.
- **One clock.** A fixed one-minute tick, pause, five speeds, auto-pause on contact, a speed cap while units are in contact, and an optional WeGo pulse.
- **Command friction.** Orders take 5–30+ minutes to reach units, longer out of radio range, in combat, or when tired.
- **Flashpoint-style combat.** Line of sight from elevation and cover, concealment in forests and towns, night spotting, fog of war with last-known positions, direct and indirect fire, morale, and standing orders that make hurt units fall back.
- **Simulation fidelity zones.** Hot (within 10 km of an enemy, or within a gun's range), warm (10–40 km) and cold zones.
- **A thin logistics loop.** Fuel and ammo are carried per unit and refilled from company stocks near the HQ (or, once the HQ is lost, near the platoon that took over the trains, at half rate); food is drawn daily.
- **A scenario you can win or lose.** The 7th Guards Remnant hunts the company. It follows the freshest sighting any hostile has made and gets word of the company's whereabouts every six hours or so. You win by breaking it: none of its six fighting platoons left with 30% strength or more. You lose when fewer than three of your fighting units are left. Losing Murks HQ doesn't end the game, but orders then take 25 minutes longer, and after an hour's handover the senior platoon still able to fight (first in the order of battle, at 30% strength or more) takes over the company trains: units halted near it resupply at half the usual rate, and regroup and rest & resupply form up on it. If that platoon is lost or falls below 30%, the next one takes over. Hostile formations regroup the same way on whoever takes over from a lost HQ. The contract and score sit top left; an end screen offers keep playing, load, new game or quit.
- **Quick save and load** (F5 / F9). A save holds the whole simulation; loading it and playing on gives exactly the same result. Save format v6 (suppression; structs are written field by field, so equal states always save to equal bytes); older saves are refused.

The map is baked from real data by `tools/bake_region.py` (see "The real map" below). If the baked file is missing, or you pass `--procedural`, the game generates a fictional map instead.

![Whole map](docs/screenshots/03_whole_map_overview.png)

![Company at Sulechów](docs/screenshots/01_company_close.png)

## Run it (Windows)

1. Install **Godot 4.5 or newer** (built and tested with 4.7.2): https://godotengine.org/download
2. Open Godot, choose **Import**, and select `game/project.godot`.
3. Press **F5** (Run Project). The map loads in under a second.

The prebuilt extension is in `game/bin/` (`libmurks.windows.template_debug.x86_64.dll`, plus a Linux `.so`), so no compiler is needed just to play.

### Controls

| Input | Action |
| --- | --- |
| Left click | Select unit (Shift adds to the selection). On the theater map, clicking a company counter selects the whole group |
| Double-click a unit / K | Select its whole company. The panel shows averages, men, contact status and a roster |
| Right click | Move the selected units to that hex. A company keeps its layout, with spread capped at 3 km. Orders arrive after the command delay |
| Left or middle drag, WASD / arrows | Pan |
| Mouse wheel | Zoom |
| Space | Pause / resume |
| 1–5 | Speed: 1 min/s, 5 min/s, 30 min/s, 2 h/s, max |
| . (period) | Step 15 minutes while paused |
| H | Hold position |
| Tab / C | Next unit / centre on the selection |
| M | Whole map |
| O | Company grouping on or off. When on, it applies automatically once hexes are smaller than 14 px on screen |
| F / Q / E / R | Right-click mode for the next order: move (hasty) / road march / deliberate / assault. After the order it goes back to move |
| B / Y / N | Fall back / regroup on the company HQ / rest & resupply at the HQ (or on whoever took over from it) |
| X | Cycle fire: at will, return fire, hold fire |
| G / V / Z | Hex grid / line of sight of the selected unit / fidelity zones |
| P | WeGo pulse: pause each time the HQ can issue orders |
| U | Auto-pause on contacts and losses |
| F1 | Show / hide all controls |
| F2 | Map data credits |
| F5 / F9 | Quick save / quick load (one slot, in Godot's user folder) |
| J / Shift+J | Form a team from the selection / disband the selection's teams |
| L | Select the team of the selected unit (double-click does the same) |
| , (comma) | Dismount the squads of the selected APC/IFV platoons, or mount them up again |
| T | Direct command overlay: every unit outlined and linked to its commander, with zones |
| I | Fire support overlay: the fire net, range rings, relays, units cut off, fire missions |

**Try this:** press Tab until **1st Recon** is selected, right-click about 30 km east, press 4 and watch. When the hostile strike group is spotted, the game pauses and the fight starts on the same map.

**Time compression.** The 30 min/s, 2 h/s and Max speeds drop to 5 min/s while the company is in contact: an own unit fired, was fired at or was hit in the last 15 minutes, or a hostile is in sight within 5 km. Fast-forward also stops on the minute anything pause-worthy happens, so Max never runs past a contact.

### What the orders do

| Order | Effect in the simulation |
| --- | --- |
| Move (hasty) | Normal speed. Fires on the move at 75% accuracy |
| Road march | Routes keep to roads wherever they can, and road steps are 15% faster. Off-road legs (to reach the road, or the last stretch) move at the normal rate. Fires at 50% on the move, and is 30% easier to hit while marching. When no road is left on its route, or once an enemy is known within 5 km, the column deploys into a normal move, and it leaves road march when it arrives |
| Deliberate | 1.5× slower. Stays concealed in forest and towns while moving and shoots as if halted |
| Assault | 1.15× slower. Fires at 85% on the move. Break-contact thresholds drop by 40% while advancing |
| Fall back | Withdraws about 6 km directly away from the nearest enemy seen in the last hour, or back to the HQ if none |
| Regroup | Forms up within 2 hexes of the company HQ (or where the HQ is heading); with the HQ lost, on the platoon that took over from it |
| Rest & resupply | Regroups, then rests: fuel and ammo refill at 50 points an hour instead of 25, and morale and readiness recover twice as fast |
| Fire: return fire | Shoots only within 5 minutes of being fired at. Firing gives a unit away, so this keeps scouts hidden |
| Fire: hold fire | Never shoots |
| Break contact | When the unit falls back on its own: below 30% strength or morale 25 (heavy losses), below 60% or morale 45 (light losses), or only when morale drops under 8 (never) |

## Measured performance

Measured in a 2-core cloud Linux VM. Your PC will be faster.

| What | Result |
| --- | --- |
| Loading the real map (3.8 M hexes, 4,687 towns; 6.8 MB compressed) | ~0.4 s at startup |
| Baking the real map from scratch (downloads included) | ~1-2 min; re-bakes from cache ~45 s |
| Procedural map generation (2.2 M hexes) | ~2.6 s at startup |
| Map memory | 30.3 MB real map, 17.3 MB procedural (8 bytes per hex) |
| Chunk mesh build (4,096 hexes) | ~0.5 ms in C++ |
| Worst single frame while panning 720 km at chunk zoom | ~7 ms of chunk building |
| Simulation, 212 units | ~0.2–0.5 ms per game minute (2 days in 0.85 s) |
| Simulation, 499 units (`--groups=60`) | ~0.8 ms per game minute |
| GDScript per frame (overlay drawing + glue) | ~0.5–1 ms |

Scaled up, the full Atlantic-to-Urals map (about 47 M hexes) needs about 380 MB of hex data. That is why chunks are stored contiguously, so they can be paged from disk.

## The real map

`game/data/poland_1989.murkz` covers longitude 10–24.2°E and latitude 49–55°N (East and West Germany's border region, Poland, Bohemia, Kaliningrad and the edges of the Baltic states, Belarus and Ukraine). It is projected on EPSG:3035 (Lambert Azimuthal Equal Area), so grid north is true north only along 10°E. The status bar shows the latitude and longitude under the cursor.

How it is built (`tools/bake_region.py`):

1. Build a 125 m raster in EPSG:3035, aligned with the hex grid.
2. **Elevation.** Warp the Copernicus GLO-90 tiles into it. They are read in place from AWS.
3. **Land cover.** Do the same with ESA WorldCover, read from its built-in 1/8 overviews, so each 3° tile is a few MB.
4. **Terrain per hex.** Average about 14 samples per 500 m hex. Elevation is the mean. The terrain type comes from land-cover shares: water, built-up, forest, marsh, farmland, otherwise open. Partial tree cover adds some line-of-sight cover.
5. **Networks.** Trace Natural Earth rivers, main roads and railways through the hexes. Each network is stored per hex edge, and zigzags are removed.
6. **Towns.** Place GeoNames towns of 2,000+ people, with 1989 names where they differed (Karl-Marx-Stadt, Lvov, Grodno).
7. **Finish.** `finish_map` (C++) adds A*-routed local roads between towns that join the real network. The result is then compressed.

To re-bake it, or bake another region:

```
pip install numpy rasterio pyproj shapely pyogrio pillow
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
python tools/bake_region.py --bbox 10 49 24.2 55 --out game/data/poland_1989.murkz --finish-map build/finish_map
```

On Windows the `--finish-map` path is `build\Release\finish_map.exe`. The first run reads several hundred MB of source data and caches the processed tiles in `~/murks_cache`.

Not yet corrected to 1989:

- Borders. There is no inner-German border yet.
- Motorways built after 1989. Natural Earth shows today's network.
- Closed railway lines.

That is the hand-made "1989 correction layer" from the design plan.

### Map data credits

The same text is in the game (F2):

- **Elevation:** Copernicus DEM GLO-90, © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018, provided under COPERNICUS by the European Union and ESA; all rights reserved.
- **Land cover:** © ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) processed by ESA WorldCover consortium. CC BY 4.0.
- **Rivers, roads, railways:** Made with Natural Earth (public domain).
- **Place names and populations:** GeoNames (geonames.org), CC BY 4.0.

## Layout

```
core/         Pure C++17 simulation (no Godot code): hex maths, map (+ file format), generator, A*, sim, render data, geo
extension/    GDExtension binding: one class, MurksWorld, with coarse calls (whole chunks, snapshots, many ticks)
game/         Godot project: scripts/main.gd (input, clock), map_view.gd (streaming), overlay.gd (symbols), hud.gd
game/data/    Baked real map (.murkz = zlib-compressed MURKSMAP)
tools/        bake_region.py (real-data pipeline), finish_map.cpp (adds local roads)
tests/        core_bench.cpp: headless run of map + sim with timings; hq_loss_test.cpp: checks the HQ-loss rules; rules_test.cpp: checks the Historical Values rules; fire_support_test.cpp: checks gun ranges and the fire support net; dismount_test.cpp: checks mounted infantry; suppression_test.cpp: checks suppression; attrition_test.cpp: kill-rate harness (`attrition_test calibrate`); playtest.cpp: real-map 4-day scripted runs over many seeds (`playtest game/data/poland_1989.murkz 4 active 1989 12`, needs zlib)
SConstruct    Builds the extension into game/bin/
CMakeLists.txt  Builds the core, benchmark and finish_map on their own (handy in Visual Studio)
```

## Rebuild the C++ (Windows)

You only need this to change the C++ code.

1. Install **Python 3**, then run `pip install scons`.
2. Install **Visual Studio 2022 Build Tools** with the "Desktop development with C++" workload.
3. From the repository root, fetch the Godot bindings (once):
   ```
   git clone --depth 1 --branch godot-4.5-stable https://github.com/godotengine/godot-cpp.git godot-cpp
   ```
4. Open **"x64 Native Tools Command Prompt for VS 2022"**, `cd` into the repository and run:
   ```
   scons                          (debug build, used by the editor; the first build takes 10-20 min)
   scons target=template_release  (release build, needed for exported games)
   ```
5. Restart the Godot editor, or let it hot-reload the extension.

The core has no Godot dependency, so you can also open the folder in Visual Studio (CMake) and run `core_bench` to test simulation changes in seconds (`hq_loss_test`, `rules_test`, `fire_support_test`, `dismount_test` and `suppression_test` check the rules and exit non-zero on a failure; `attrition_test calibrate` checks the kill rate is inside the historical band).

On Linux, the same `scons` works. To cross-compile Windows from Linux, use `scons platform=windows use_mingw=yes`.

## Command-line options

Pass these after `--` when launching Godot:

- `--groups=N`: number of hostile bands (default 24, about 212 units). Use 60 or more as a stress test.
- `--procedural`: ignore the baked map and generate a fictional one.
- `--autotest=DIR`: scripted run that saves screenshots and timing to `DIR`, then quits.

Headless balance check (no window): `godot --headless --path game --script res://tests/playtest.gd -- 4 active` (or `passive`). It plays the scenario for N days and prints contact time, losses, supplies, the outcome and a save/load replay check.

## Known limits of this proof of concept

- Map data is not yet corrected to 1989 (see above). Natural Earth has main roads only; local roads are generated.
- One scripted contract. No contract board, holdings, wages, convoys or replacements yet (design plan phases 3–4), so company ammo and diesel stocks only go down.
- Combat values follow simulated real-world performance (weapons, cover, concealment, morale), not game balance, and are not tuned to make fights even. A company defending a town against an attacker of similar size usually holds, as the attacker's rule of thumb of needing about 3:1 at the point of attack would predict.
- Pathfinding is plain A* on hexes. Very long cross-map orders cost up to ~10 ms; next comes the hierarchical road graph and the 5 km operational layer. Routes to another piece of land (islands, across the sea) are refused at once.
- One quick-save slot. A save only loads on the map it was made on.
- Exporting a game: add `*.murkz` to the export preset's "Filters to export non-resource files" so the map is included.
- Running `godot --import` from the command line on a project with no `.godot` folder can crash on exit with this godot-cpp/Godot pairing. Run it again and it works. Opening the project normally in the editor is not affected.
