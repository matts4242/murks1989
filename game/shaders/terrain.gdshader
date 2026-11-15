// Murks 1989 — the painted ground (work board MRK-29).
//
// One sprite covers the whole map and this shader paints every pixel of it. For each
// pixel it finds the hex underneath in a per-hex data texture (terrain, density,
// elevation, cover), paints that terrain with a material computed in world space (so a
// wood or a field runs on seamlessly from one hex into the next), lights it with hill
// shading from the baked 180 m height raster, and adds the hex grid and, in topographic
// mode, contour lines printed into the ground in a darker tone of whatever lies there.
//
// Natural edges: instead of taking the terrain of the hex a pixel falls in, the three hex
// centres around the pixel vote for their terrain, each with its barycentric weight, plus
// a little noise per terrain type. Borders between terrains then run smoothly across the
// grid (a jagged row of hex edges becomes one curving wood edge) and wander like real
// ones, while the centre of every hex always keeps its own terrain. The grid, the hover
// readout and the rules use the true hexes. `edge_noise` sets how far borders wander;
// 0 gives crisp hex borders.
//
// Far out, where a pixel covers several hexes, the shader fades to the sprite's own
// texture: one average colour per hex with hill shading baked in, mipmapped, so nothing
// shimmers. Painted detail that would be smaller than a few pixels fades to its average
// in the same way. The averages are in core/src/render_data.cpp (base_color): keep the
// two in step.
shader_type canvas_item;
render_mode unshaded;

uniform sampler2D hex_data : filter_nearest, repeat_disable;
uniform ivec2 hex_count = ivec2(1, 1);
uniform sampler2D height_tex : filter_nearest, repeat_disable;
uniform bool has_height = false;
uniform float height_px = 180.0;

uniform float metres_per_pixel = 10.0;
uniform float detail = 1.0;            // 0: flat colour per terrain (F6 off)
uniform float edge_noise = 0.3;        // 0 = crisp hex borders
uniform float grid_alpha = 0.0;
uniform float relief = 1.0;            // hill shading strength
uniform float contour_interval = 0.0;  // metres between contour lines; 0 = none
uniform float contour_heavy = 5.0;     // every n-th line is heavier

// Planning map (MRK-25): 1 = a whole-map tint per hex from plan_tex (going, cover, slope),
// 2 = a local tint per hex from local_tex (line of sight), out of range outside its box.
// The ground under a mode is a little greyer so the tints read; tints are crisp per hex
// close in (the rules are per hex) and averaged (mipmapped) far out.
uniform int plan_mode = 0;
uniform sampler2D plan_tex : filter_linear_mipmap, repeat_disable;
uniform sampler2D local_tex : filter_nearest, repeat_disable;
uniform ivec2 local_origin = ivec2(0, 0);
uniform ivec2 local_size = ivec2(1, 1);
uniform vec4 local_outside = vec4(0.22, 0.22, 0.24, 0.43);

// Legend swatches: paint terrain (swatch & 7, +8 for dense) across the rect, no relief.
uniform int swatch = -1;
uniform float swatch_size = 600.0;     // metres across a swatch

varying vec2 world;

const float HEX_W = 500.0;
const float HEX_S = 288.6751346;  // centre to corner
const float HEX_R = 433.0127019;  // row spacing
const int T_WATER = 0;
const int T_OPEN = 1;
const int T_FIELDS = 2;
const int T_FOREST = 3;
const int T_WETLAND = 4;
const int T_URBAN = 5;

void vertex() {
	world = (MODEL_MATRIX * vec4(VERTEX, 0.0, 1.0)).xy;
}

// --- Noise -----------------------------------------------------------------------------

uint hash_u(ivec2 p, uint seed) {
	uvec2 q = uvec2(p + ivec2(1048576));
	uint h = (q.x * 1597334673u) ^ (q.y * 3812015801u) ^ (seed * 2654435769u);
	h ^= h >> 16u;
	h *= 2246822519u;
	h ^= h >> 13u;
	h *= 3266489917u;
	h ^= h >> 16u;
	return h;
}

float rnd(ivec2 p, uint seed) {
	return float(hash_u(p, seed) >> 8u) * (1.0 / 16777216.0);
}

float vnoise(vec2 x, uint seed) {
	vec2 i = floor(x);
	vec2 f = x - i;
	vec2 u = f * f * (3.0 - 2.0 * f);
	ivec2 b = ivec2(i);
	float a = rnd(b, seed);
	float c = rnd(b + ivec2(1, 0), seed);
	float d = rnd(b + ivec2(0, 1), seed);
	float e = rnd(b + ivec2(1, 1), seed);
	return mix(mix(a, c, u.x), mix(d, e, u.x), u.y);
}

// How visible a feature of `size` metres is at this zoom: 0 below ~1.5 px, 1 above ~4 px.
float vis(float size) {
	return smoothstep(1.5, 4.0, size / metres_per_pixel);
}

// Fractal noise in 0..1 whose finer octaves fade to their mean once they would be smaller
// than a few pixels, so zooming out never makes it shimmer. `period` in metres.
float fbm(vec2 p, float period, uint seed, int octaves) {
	float sum = 0.0;
	float norm = 0.0;
	float amp = 1.0;
	for (int k = 0; k < octaves; k++) {
		float v = vis(period);
		float n = v > 0.0 ? vnoise(p / period, seed + uint(k) * 97u) : 0.5;
		sum += amp * mix(0.5, n, v);
		norm += amp;
		amp *= 0.5;
		period *= 0.5;
	}
	return sum / norm;
}

// Nearest of jittered points on a grid of `cell` metres: returns the point; `id` its cell.
vec2 voronoi(vec2 q, float cell, uint seed, out ivec2 id) {
	ivec2 gi = ivec2(floor(q / cell));
	float best = 1e20;
	vec2 bp = vec2(0.0);
	id = gi;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			ivec2 c = gi + ivec2(i, j);
			vec2 pt = (vec2(c) + 0.1 + 0.8 * vec2(rnd(c, seed), rnd(c, seed + 1u))) * cell;
			vec2 d = q - pt;
			float dd = dot(d, d);
			if (dd < best) {
				best = dd;
				bp = pt;
				id = c;
			}
		}
	}
	return bp;
}

// --- Hexes -----------------------------------------------------------------------------

ivec2 world_to_hex(vec2 p) {
	vec2 q = p - vec2(HEX_W * 0.5, HEX_S);
	float qf = (0.5773502692 * q.x - 0.3333333333 * q.y) / HEX_S;
	float rf = (0.6666666667 * q.y) / HEX_S;
	vec3 c = vec3(qf, -qf - rf, rf);
	vec3 r = round(c);
	vec3 d = abs(r - c);
	if (d.x > d.y && d.x > d.z) {
		r.x = -r.y - r.z;
	} else if (d.y > d.z) {
		r.y = -r.x - r.z;
	} else {
		r.z = -r.x - r.y;
	}
	int row = int(r.z);
	int col = int(r.x) + (row - (row & 1)) / 2;
	return ivec2(col, row);
}

vec2 hex_center(ivec2 h) {
	return vec2(HEX_W * (float(h.x) + 0.5 * float(h.y & 1)) + HEX_W * 0.5, HEX_R * float(h.y) + HEX_S);
}

// Per-hex record: x terrain, y 1 if dense, z elevation in metres, w cover in metres.
vec4 hex_rec(ivec2 h) {
	if (h.x < 0 || h.y < 0 || h.x >= hex_count.x || h.y >= hex_count.y) {
		return vec4(0.0);
	}
	vec4 t = texelFetch(hex_data, h, 0);
	int code = int(t.r * 255.0 + 0.5);
	float elev = floor(t.g * 255.0 + 0.5) * 256.0 + floor(t.b * 255.0 + 0.5) - 32768.0;
	return vec4(float(code & 7), (code & 8) != 0 ? 1.0 : 0.0, elev, t.a * 255.0);
}

ivec2 axial_to_offset(ivec2 a) {
	return ivec2(a.x + (a.y - (a.y & 1)) / 2, a.y);
}

// Noise for one terrain type's vote, centred on 0.
float vote_noise(vec2 p, int t) {
	uint seed = 700u + uint(t) * 31u;
	float n = vnoise(p / 230.0, seed) - 0.5;
	n += 0.55 * (vnoise(p / 95.0, seed + 7u) - 0.5);
	n += 0.35 * vis(14.0) * (vnoise(p / 38.0, seed + 13u) - 0.5);
	return n * 2.0;
}

// The hex record whose terrain covers `p` (see "natural edges" at the top).
vec4 terrain_rec(vec2 p) {
	if (edge_noise <= 0.0) {
		return hex_rec(world_to_hex(p));
	}
	vec2 q = p - vec2(HEX_W * 0.5, HEX_S);
	vec2 ax = vec2((0.5773502692 * q.x - 0.3333333333 * q.y) / HEX_S, (0.6666666667 * q.y) / HEX_S);
	vec2 b = floor(ax);
	vec2 f = ax - b;
	ivec2 ib = ivec2(b);
	ivec2 a0;
	vec3 w;
	if (f.x + f.y < 1.0) {
		a0 = ib;
		w = vec3(1.0 - f.x - f.y, f.x, f.y);
	} else {
		a0 = ib + ivec2(1, 1);
		w = vec3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
	}
	vec4 r0 = hex_rec(axial_to_offset(a0));
	vec4 r1 = hex_rec(axial_to_offset(ib + ivec2(1, 0)));
	vec4 r2 = hex_rec(axial_to_offset(ib + ivec2(0, 1)));
	int t0 = int(r0.x + 0.5);
	int t1 = int(r1.x + 0.5);
	int t2 = int(r2.x + 0.5);
	// Each vertex scores the summed weight of the vertices sharing its terrain.
	float s0 = w.x + (t1 == t0 ? w.y : 0.0) + (t2 == t0 ? w.z : 0.0) + edge_noise * vote_noise(p, t0);
	float s1 = w.y + (t0 == t1 ? w.x : 0.0) + (t2 == t1 ? w.z : 0.0) + edge_noise * vote_noise(p, t1);
	float s2 = w.z + (t0 == t2 ? w.x : 0.0) + (t1 == t2 ? w.y : 0.0) + edge_noise * vote_noise(p, t2);
	int tw = t0;
	float bs = s0 + w.x * 0.001;
	if (s1 + w.y * 0.001 > bs) {
		tw = t1;
		bs = s1 + w.y * 0.001;
	}
	if (s2 + w.z * 0.001 > bs) {
		tw = t2;
	}
	// Density, elevation and cover blend between the hexes of the winning terrain, so a
	// dense wood also fades into a light one along a wandering line, not at a hex edge.
	vec3 m = vec3(t0 == tw ? w.x : 0.0, t1 == tw ? w.y : 0.0, t2 == tw ? w.z : 0.0);
	m /= max(m.x + m.y + m.z, 1e-4);
	float dense = dot(m, vec3(r0.y, r1.y, r2.y)) + 0.6 * edge_noise * vote_noise(p, 9);
	return vec4(float(tw), dense > 0.5 ? 1.0 : 0.0, dot(m, vec3(r0.z, r1.z, r2.z)), dot(m, vec3(r0.w, r1.w, r2.w)));
}

// --- Materials -------------------------------------------------------------------------

// What each painted material averages to, measured with --views (they print it); also
// the far-zoom palette in core/src/render_data.cpp (base_color). Keep all three in step.
const vec3 FLAT_OPEN = vec3(124.0, 149.0, 77.0) / 255.0;
const vec3 FLAT_FIELDS = vec3(139.0, 137.0, 88.0) / 255.0;
const vec3 FLAT_FOREST = vec3(58.0, 91.0, 47.0) / 255.0;
const vec3 FLAT_FOREST_DENSE = vec3(44.0, 72.0, 40.0) / 255.0;
const vec3 FLAT_WETLAND = vec3(118.0, 137.0, 109.0) / 255.0;
const vec3 FLAT_TOWN = vec3(124.0, 132.0, 88.0) / 255.0;
const vec3 FLAT_TOWN_DENSE = vec3(135.0, 129.0, 109.0) / 255.0;

vec3 flat_colour(int t, bool dense, float depth) {
	if (t == T_WATER) {
		float k = clamp(depth / 40.0, 0.0, 1.0);
		return vec3(102.0 - 34.0 * k, 143.0 - 34.0 * k, 179.0 - 30.0 * k) / 255.0;
	}
	if (t == T_FIELDS) return FLAT_FIELDS;
	if (t == T_FOREST) return dense ? FLAT_FOREST_DENSE : FLAT_FOREST;
	if (t == T_WETLAND) return FLAT_WETLAND;
	if (t == T_URBAN) return dense ? FLAT_TOWN_DENSE : FLAT_TOWN;
	return FLAT_OPEN;
}

// Small round things (bushes, garden trees, reed tufts) scattered on a grid of `cell`
// metres with `chance` per cell, each with a shadow to the south-east.
vec3 scatter(vec2 p, vec3 c, float cell, float chance, uint seed, vec3 ink, float size) {
	float v = vis(cell * size * 2.0);
	if (v <= 0.0) {
		return c;
	}
	ivec2 gi = ivec2(floor(p / cell));
	float aa = metres_per_pixel * 0.7;
	float body = 0.0;
	float shade = 0.0;
	float lit = 0.0;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			ivec2 q = gi + ivec2(i, j);
			if (rnd(q, seed) > chance) {
				continue;
			}
			vec2 ctr = (vec2(q) + 0.2 + 0.6 * vec2(rnd(q, seed + 1u), rnd(q, seed + 2u))) * cell;
			float r = cell * size * (0.7 + 0.6 * rnd(q, seed + 3u));
			vec2 d = p - ctr;
			float b = 1.0 - smoothstep(r - aa, r + aa, length(d));
			body = max(body, b);
			shade = max(shade, 1.0 - smoothstep(r - aa, r + aa, length(d - vec2(r * 0.55, r * 0.65))));
			lit = max(lit, b * clamp(-(d.x + d.y) / (r * 1.4), 0.0, 1.0));
		}
	}
	c *= 1.0 - 0.32 * shade * (1.0 - body) * v;
	vec3 t = mix(ink, ink * 1.45 + 0.03, lit);
	return mix(c, t, body * v);
}

vec3 paint_open(vec2 p) {
	// Meadow and rough grazing: broad drifts of greener and drier grass, finer mottling,
	// a grass texture close in, and copses or single trees here and there.
	float big = fbm(p, 1500.0, 101u, 3);
	float mid = fbm(p, 240.0, 103u, 3);
	vec3 c = mix(vec3(0.43, 0.56, 0.28), vec3(0.55, 0.62, 0.33), smoothstep(0.3, 0.7, big));
	c *= 0.90 + 0.18 * mid;
	c *= 0.95 + 0.10 * fbm(p, 32.0, 105u, 2);
	return scatter(p, c, 64.0, 0.13, 107u, vec3(0.21, 0.33, 0.15), 0.16);
}

vec3 crop_colour(uint h) {
	uint k = h % 20u;
	if (k < 8u) return vec3(0.58, 0.48, 0.36);   // ploughed
	if (k < 10u) return vec3(0.49, 0.42, 0.32);  // harrowed, darker
	if (k < 15u) return vec3(0.50, 0.61, 0.31);  // young winter wheat
	if (k < 17u) return vec3(0.43, 0.56, 0.29);  // greener crop
	if (k < 19u) return vec3(0.72, 0.68, 0.48);  // stubble, fallow
	return vec3(0.63, 0.67, 0.31);               // rapeseed coming up
}

vec3 paint_fields(vec2 p) {
	// Farm districts about a kilometre across, with wandering borders: each has its own
	// field direction and size, from long narrow strips to big collective-farm blocks.
	vec2 q = p + 220.0 * (vec2(vnoise(p / 700.0, 201u), vnoise(p / 700.0, 203u)) - 0.5);
	ivec2 did;
	vec2 dp = voronoi(q, 1000.0, 211u, did);
	uint dh = hash_u(did, 217u);
	float ang = float(dh % 18u) * (3.14159265 / 18.0);
	float w = 60.0 + float((dh >> 5u) % 5u) * 40.0;
	float l = w * (1.8 + float((dh >> 9u) % 4u) * 0.9);
	vec2 cs = vec2(cos(ang), sin(ang));
	vec2 lp = vec2(dot(p - dp, cs), dot(p - dp, vec2(-cs.y, cs.x)));
	float row = floor(lp.y / w);
	float shift = rnd(ivec2(int(row), int(dh & 4095u)), 219u) * l;
	float colm = floor((lp.x + shift) / l);
	uint ph = hash_u(ivec2(int(colm), int(row)) + ivec2(int(dh & 1023u), int((dh >> 10u) & 1023u)), 223u);
	// A district leans to one crop; parcels too thin to see fade to its tone.
	vec3 district = mix(vec3(0.53, 0.52, 0.34), crop_colour(dh >> 3u), 0.5);
	vec3 crop = mix(crop_colour(ph), district, 0.32);
	crop = mix(district, crop, vis(w * 0.9));
	crop *= 0.93 + 0.14 * fbm(p, 190.0, 227u, 2);
	// Drill rows along the parcel, only when close enough to see them.
	crop *= 1.0 - 0.08 * (0.5 + 0.5 * sin(lp.y * 6.2831853 / 9.0)) * vis(9.0);
	// A grass strip between parcels.
	vec2 fe = vec2((lp.x + shift) - colm * l, lp.y - row * w);
	float edge_m = min(min(fe.x, l - fe.x), min(fe.y, w - fe.y));
	float edge = 1.0 - smoothstep(2.0, 2.0 + max(metres_per_pixel, 1.0), edge_m);
	return mix(crop, vec3(0.47, 0.56, 0.31), edge * 0.75 * vis(22.0));
}

vec3 paint_forest(vec2 p, bool dense) {
	// Stands change over a few kilometres between broadleaf and conifer; dense woods lean
	// to conifer and are darker.
	float stand = fbm(p, 2600.0, 301u, 2);
	float con = smoothstep(0.40, 0.60, stand);
	if (dense) {
		con = mix(con, 1.0, 0.45);
	}
	vec3 lit = mix(vec3(0.40, 0.55, 0.27), vec3(0.25, 0.42, 0.26), con);
	vec3 dark = mix(vec3(0.15, 0.26, 0.12), vec3(0.08, 0.17, 0.10), con);
	if (dense) {
		lit *= 0.88;
		dark *= 0.85;
	}
	// Canopy in clumps of ~60 m, lit from the north-west by the slope of the noise.
	float e = 10.0;
	float n0 = fbm(p, 90.0, 307u, 3);
	float nx = fbm(p + vec2(e, 0.0), 90.0, 307u, 3);
	float ny = fbm(p + vec2(0.0, e), 90.0, 307u, 3);
	float slope_lit = (nx - n0 + ny - n0) / e * 60.0 * vis(40.0);
	float shade = clamp(0.48 + 0.35 * (n0 - 0.5) + slope_lit, 0.0, 1.0);
	// Single crowns when close in: domes on a 22 m grid, gaps between them dark.
	float cv = vis(16.0);
	if (cv > 0.0) {
		ivec2 gi = ivec2(floor(p / 22.0));
		float best = 9.0;
		vec2 bv = vec2(0.0);
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				ivec2 q = gi + ivec2(i, j);
				vec2 ctr = (vec2(q) + 0.15 + 0.7 * vec2(rnd(q, 311u), rnd(q, 313u))) * 22.0;
				float rr = (con > 0.5 ? 10.0 : 13.0) * (0.75 + 0.5 * rnd(q, 317u));
				vec2 d = (p - ctr) / rr;
				float dd = length(d);
				if (dd < best) {
					best = dd;
					bv = d;
				}
			}
		}
		float inside = 1.0 - smoothstep(0.8, 1.05, best);
		float z = sqrt(max(0.0, 1.0 - min(best * best, 1.0)));
		float lam = clamp(dot(normalize(vec3(bv, z + 0.05)), vec3(-0.55, -0.55, 0.63)), 0.0, 1.0);
		float crown = mix(0.12, 0.25 + 0.85 * lam, inside);
		shade = mix(shade, clamp(shade * 0.5 + crown * 0.6, 0.0, 1.0), cv);
	}
	return mix(dark, lit, shade);
}

vec3 paint_wetland(vec2 p) {
	float n = fbm(p, 240.0, 401u, 3);
	vec3 c = mix(vec3(0.43, 0.51, 0.37), vec3(0.55, 0.59, 0.42), n);
	float pools = fbm(p, 130.0, 403u, 3);
	float edge = 0.012 + metres_per_pixel * 0.0012;
	float pool = smoothstep(0.60 - edge, 0.60 + edge, pools);
	c = mix(c, vec3(0.36, 0.48, 0.55), pool * 0.85);
	return scatter(p, c, 18.0, 0.28 * (1.0 - pool), 409u, vec3(0.30, 0.38, 0.24), 0.16);
}

vec3 roof_colour(float s) {
	if (s < 0.40) return vec3(0.66, 0.37, 0.28);   // red tile
	if (s < 0.58) return vec3(0.52, 0.31, 0.25);   // old tile
	if (s < 0.78) return vec3(0.48, 0.47, 0.48);   // slate, eternit
	if (s < 0.90) return vec3(0.40, 0.40, 0.42);   // flat tar
	return vec3(0.70, 0.67, 0.61);                 // concrete
}

const vec3 STREET = vec3(0.62, 0.61, 0.57);
const vec3 YARD = vec3(0.55, 0.54, 0.49);
const vec3 GARDEN = vec3(0.47, 0.53, 0.33);

// What stands at `lp` (metres in the quarter's street grid) in a quarter of `kind`:
// rgb the roof colour and a = 1 where a building stands; otherwise `ground` is the colour
// of the street, yard or garden there. Kinds: 0 city blocks round courtyards, 1 slab
// housing estate, 2 works, 3 village streets with houses and gardens, 4 farmsteads.
vec4 town_at(int kind, vec2 lp, uint seed, out vec3 ground) {
	ground = GARDEN;
	if (kind == 0) {
		float b = 92.0 + float((seed >> 3u) % 4u) * 14.0;
		ivec2 bi = ivec2(floor(lp / b));
		vec2 f = lp - vec2(bi) * b;
		float ex = min(f.x, b - f.x);
		float ey = min(f.y, b - f.y);
		float e = min(ex, ey);
		if (e < 4.5) {
			ground = STREET;
			return vec4(0.0);
		}
		if (rnd(bi, seed) < 0.08) {
			return vec4(0.0);   // a square or a little park
		}
		float depth = 13.0 + 6.0 * rnd(bi, seed + 1u);
		if (e > 4.5 + depth) {
			ground = mix(YARD, GARDEN, 0.5 * rnd(bi, seed + 5u));
			return vec4(0.0);   // courtyard
		}
		float along = ex < ey ? f.y : f.x;
		float seg = 16.0 + 14.0 * rnd(bi, seed + 2u);
		float s = rnd(bi * 37 + ivec2(int(floor(along / seg)), ex < ey ? 1 : 0), seed + 3u);
		if (s < 0.05) {
			ground = YARD;
			return vec4(0.0);
		}
		return vec4(roof_colour(s), 1.0);
	}
	if (kind == 1) {
		float pitch = 54.0 + float((seed >> 3u) % 3u) * 8.0;
		float row = floor(lp.y / pitch);
		float fy = lp.y - row * pitch;
		if (fy < 3.5) {
			ground = STREET;
			return vec4(0.0);
		}
		ivec2 ri = ivec2(int(row), 11);
		float len = 70.0 + 50.0 * rnd(ri, seed);
		float span = len + 26.0;
		float fx = lp.x + rnd(ri, seed + 1u) * span;
		fx -= floor(fx / span) * span;
		if (fy > 22.0 && fy < 34.0 && fx < len) {
			return vec4(vec3(0.71, 0.70, 0.67) * (0.94 + 0.08 * rnd(ri, seed + 2u)), 1.0);
		}
		ground = GARDEN * 1.04;
		return vec4(0.0);
	}
	if (kind == 2) {
		float b = 130.0;
		ivec2 bi = ivec2(floor(lp / b));
		vec2 f = lp - vec2(bi) * b;
		float e = min(min(f.x, b - f.x), min(f.y, b - f.y));
		ground = YARD;
		if (e < 6.0) {
			ground = STREET;
			return vec4(0.0);
		}
		vec2 sz = vec2(26.0 + 30.0 * rnd(bi, seed), 20.0 + 26.0 * rnd(bi, seed + 1u));
		vec2 at = vec2(b * 0.5) + (vec2(rnd(bi, seed + 2u), rnd(bi, seed + 3u)) - 0.5) * (b - 2.0 * sz - 16.0);
		vec2 d = abs(f - at);
		if (d.x < sz.x && d.y < sz.y) {
			float r = rnd(bi, seed + 4u);
			vec3 roof = r < 0.4 ? vec3(0.50, 0.53, 0.57) : (r < 0.75 ? vec3(0.54, 0.54, 0.52) : vec3(0.56, 0.43, 0.35));
			// Saw-tooth roof lights.
			roof *= 0.92 + 0.1 * step(0.6, fract((f.x - at.x) / 9.0));
			return vec4(roof, 1.0);
		}
		return vec4(0.0);
	}
	if (kind == 3) {
		// Village lanes: one lane between every row of blocks, a cross lane only every
		// third block; houses face the nearest lane with gardens behind.
		float b = 116.0 + float((seed >> 3u) % 4u) * 18.0;
		ivec2 bi = ivec2(floor(lp / b));
		vec2 f = lp - vec2(bi) * b;
		bool cross_lane = (bi.x % 3) == 0;
		float ex = cross_lane ? min(f.x, b - f.x) : 1e4;
		float ey = min(f.y, b - f.y);
		float e = min(ex, ey);
		if (e < 2.6) {
			ground = vec3(0.56, 0.54, 0.47);
			return vec4(0.0);
		}
		// Gardens in strips behind the houses, each its own shade of green.
		float along = ex < ey ? f.y : f.x;
		float lot = floor(along / 22.0);
		ivec2 li = bi * 41 + ivec2(int(lot), ex < ey ? 1 : 0);
		ground = GARDEN * (0.9 + 0.2 * rnd(li, seed + 4u));
		if (e > 6.5 && e < 17.0 && rnd(li, seed + 5u) < 0.86) {
			float w = 4.5 + 2.5 * rnd(li, seed + 6u);
			if (abs(along - (lot + 0.5) * 22.0) < w) {
				return vec4(roof_colour(rnd(li, seed + 7u) * 0.8), 1.0);
			}
		}
		return vec4(0.0);
	}
	// Farmsteads: house, barn and shed round a yard, an orchard beside them.
	float b = 150.0;
	ivec2 bi = ivec2(floor(lp / b));
	vec2 f = lp - vec2(bi) * b;
	if (rnd(bi, seed) > 0.7) {
		return vec4(0.0);
	}
	vec2 y = vec2(b * 0.5) + (vec2(rnd(bi, seed + 1u), rnd(bi, seed + 2u)) - 0.5) * 30.0;
	vec2 d = f - y;
	if (abs(d.x) < 22.0 && abs(d.y) < 18.0) {
		ground = YARD * 1.05;
	}
	if (abs(d.x) < 17.0 && d.y > -26.0 && d.y < -16.0) return vec4(roof_colour(0.2), 1.0);           // house
	if (d.x > 22.0 && d.x < 34.0 && abs(d.y) < 20.0) return vec4(vec3(0.50, 0.40, 0.34), 1.0);     // barn
	if (d.x < -22.0 && d.x > -31.0 && abs(d.y + 4.0) < 13.0) return vec4(vec3(0.46, 0.45, 0.45), 1.0); // shed
	return vec4(0.0);
}

vec3 paint_town(vec2 p, bool dense) {
	// Quarters ~600 m across, each with its own kind, street-grid direction and size.
	vec2 q = p + 180.0 * (vec2(vnoise(p / 520.0, 601u), vnoise(p / 520.0, 603u)) - 0.5);
	ivec2 qid;
	vec2 qp = voronoi(q, 600.0, 607u, qid);
	uint qh = hash_u(qid, 611u);
	uint pick = (qh >> 8u) % 20u;
	int kind = dense ? (pick < 11u ? 0 : (pick < 16u ? 1 : 2)) : (pick < 14u ? 3 : 4);
	float ang = (float(qh % 32u) / 32.0 - 0.5) * 1.2;
	vec2 cs = vec2(cos(ang), sin(ang));
	vec2 ax = vec2(-cs.y, cs.x);
	vec2 lp = vec2(dot(p - qp, cs), dot(p - qp, ax)) + vec2(10000.0);
	vec3 avg = dense ? FLAT_TOWN_DENSE : FLAT_TOWN;
	float v = vis(kind == 0 ? 26.0 : 30.0);
	if (v <= 0.0) {
		return avg * (0.95 + 0.1 * fbm(p, 400.0, 613u, 2));
	}
	vec3 ground;
	vec4 here = town_at(kind, lp, qh, ground);
	vec3 c = here.a > 0.5 ? here.rgb : ground * (0.95 + 0.1 * fbm(p, 60.0, 617u, 2));
	if (here.a < 0.5) {
		// Shadow of a building a few metres toward the light (north-west).
		vec2 to_light = vec2(dot(vec2(-5.0, -6.0), cs), dot(vec2(-5.0, -6.0), ax));
		vec3 g2;
		if (town_at(kind, lp + to_light, qh, g2).a > 0.5) {
			c *= 0.62;
		}
		// Trees in gardens, parks and courtyards.
		if (kind != 2) {
			c = scatter(p, c, 30.0, kind == 0 ? 0.12 : 0.3, qh + 19u, vec3(0.22, 0.35, 0.17), 0.22);
		}
	} else {
		// The half of a pitched roof facing the light is a touch brighter.
		c *= 0.94 + 0.1 * step(0.5, fract(lp.x / 8.0));
	}
	return mix(avg, c, v);
}

vec3 paint_water(vec2 p, float depth) {
	vec3 c = mix(vec3(0.42, 0.58, 0.72), vec3(0.25, 0.41, 0.57), clamp(depth / 40.0, 0.0, 1.0));
	return c * (0.97 + 0.06 * fbm(p, 320.0, 501u, 2));
}

vec3 paint(int t, bool dense, vec2 p, float depth) {
	if (t == T_FOREST) return paint_forest(p, dense);
	if (t == T_FIELDS) return paint_fields(p);
	if (t == T_URBAN) return paint_town(p, dense);
	if (t == T_WETLAND) return paint_wetland(p);
	if (t == T_WATER) return paint_water(p, depth);
	return paint_open(p);
}

// How tall a terrain stands for casting shadows on its neighbours.
float tallness(int t, bool dense) {
	if (t == T_FOREST) return 1.0;
	if (t == T_URBAN) return dense ? 0.55 : 0.25;
	return 0.0;
}

// --- Relief ----------------------------------------------------------------------------

// Height and its slope (x: metres, y: dh/dx, z: dh/dy) from the height raster, as a cubic
// B-spline through the 16 surrounding samples: smooth, so shading has no facets.
vec3 relief_at(vec2 p) {
	vec2 t = p / height_px - 0.5;
	vec2 b = floor(t);
	vec2 f = t - b;
	vec2 g = 1.0 - f;
	float wx[4] = float[4](g.x * g.x * g.x / 6.0, (3.0 * f.x * f.x * f.x - 6.0 * f.x * f.x + 4.0) / 6.0,
		(-3.0 * f.x * f.x * f.x + 3.0 * f.x * f.x + 3.0 * f.x + 1.0) / 6.0, f.x * f.x * f.x / 6.0);
	float wy[4] = float[4](g.y * g.y * g.y / 6.0, (3.0 * f.y * f.y * f.y - 6.0 * f.y * f.y + 4.0) / 6.0,
		(-3.0 * f.y * f.y * f.y + 3.0 * f.y * f.y + 3.0 * f.y + 1.0) / 6.0, f.y * f.y * f.y / 6.0);
	float dx[4] = float[4](-0.5 * g.x * g.x, (3.0 * f.x * f.x - 4.0 * f.x) * 0.5, (-3.0 * f.x * f.x + 2.0 * f.x + 1.0) * 0.5, 0.5 * f.x * f.x);
	float dy[4] = float[4](-0.5 * g.y * g.y, (3.0 * f.y * f.y - 4.0 * f.y) * 0.5, (-3.0 * f.y * f.y + 2.0 * f.y + 1.0) * 0.5, 0.5 * f.y * f.y);
	ivec2 base = ivec2(b) - ivec2(1);
	ivec2 top = textureSize(height_tex, 0) - ivec2(1);
	float h = 0.0;
	float gx = 0.0;
	float gy = 0.0;
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			float s = texelFetch(height_tex, clamp(base + ivec2(i, j), ivec2(0), top), 0).r;
			h += wy[j] * wx[i] * s;
			gx += wy[j] * dx[i] * s;
			gy += dy[j] * wx[i] * s;
		}
	}
	return vec3(h, gx / height_px, gy / height_px);
}

// Hill shading: mostly from the north-west, a little from the west, plus a touch of
// darkening on steep ground. 1.0 on level ground.
float hill_shade(vec3 hg) {
	vec3 n = normalize(vec3(-hg.y * 4.0, -hg.z * 4.0, 1.0));
	vec3 l1 = normalize(vec3(-1.0, -1.0, 1.15));
	vec3 l2 = normalize(vec3(-1.0, 0.3, 1.5));
	float lam = 0.75 * (dot(n, l1) - l1.z) + 0.25 * (dot(n, l2) - l2.z);
	float steep = min(length(hg.yz) * 0.9, 0.18);
	// Zoomed far out the raster is finer than a pixel: soften rather than speckle.
	float k = relief * (1.0 - 0.45 * smoothstep(250.0, 800.0, metres_per_pixel));
	return clamp(1.0 + (lam * 1.7 - steep) * k, 0.5, 1.4);
}

// High ground grows a little paler and browner.
vec3 highland(vec3 c, float h) {
	float hi = smoothstep(350.0, 1400.0, h) * 0.25;
	return mix(c, vec3(dot(c, vec3(0.35, 0.45, 0.20))) * vec3(1.12, 1.0, 0.84), hi);
}

// Contour lines printed into the ground: a darker, browner tone of the colour beneath,
// about a pixel wide, every n-th line heavier. Lines that would crowd closer than a few
// pixels fade out (the heavy ones last).
vec3 contours(vec3 col, float h) {
	float f = h / contour_interval;
	float fw = max(fwidth(f), 1e-5);
	float d_px = abs(fract(f + 0.5) - 0.5) / fw;
	bool heavy = abs(mod(floor(f + 0.5), contour_heavy)) < 0.5;
	float w = heavy ? 1.1 : 0.55;
	float line = 1.0 - smoothstep(w - 0.5, w + 0.6, d_px);
	float room = 1.0 / fw;   // pixels between lines
	line *= heavy ? smoothstep(2.0, 4.0, room * contour_heavy) : smoothstep(3.0, 6.0, room);
	vec3 ink = col * vec3(0.50, 0.40, 0.30);
	return mix(col, ink, line * (heavy ? 0.9 : 0.65));
}

void fragment() {
	if (swatch >= 0) {
		vec2 p = vec2(40000.0, 30000.0) + UV * swatch_size;
		COLOR = vec4(paint(swatch & 7, (swatch & 8) != 0, p, 5.0), 1.0);
	} else {
		vec3 far = texture(TEXTURE, UV).rgb;
		float t_far = smoothstep(40.0, 80.0, metres_per_pixel);
		vec3 hg = has_height ? relief_at(world) : vec3(0.0);
		float shade = has_height ? hill_shade(hg) : 1.0;
		vec3 col = highland(far * shade, hg.x);
		if (t_far < 1.0) {
			vec4 rec = terrain_rec(world);
			int t = int(rec.x + 0.5);
			bool dense = rec.y > 0.5;
			float depth = max(0.0, -rec.z);
			vec3 near = detail > 0.5 ? paint(t, dense, world, depth) : flat_colour(t, dense, depth);
			// Woods and towns shade the ground beside them on the side away from the light.
			float own = tallness(t, dense);
			float sh = 0.0;
			for (int k = 1; k <= 2; k++) {
				vec4 o = terrain_rec(world + vec2(-0.7071, -0.7071) * 17.0 * float(k));
				sh += max(0.0, tallness(int(o.x + 0.5), o.y > 0.5) - own) * 0.5;
			}
			near *= 1.0 - 0.3 * sh * vis(12.0);
			if (t != T_WATER) {
				near = highland(near * shade, hg.x);
			}
			col = mix(near, col, t_far);
		}
		if (contour_interval > 0.0 && has_height) {
			col = contours(col, hg.x);
		}
		if (plan_mode > 0) {
			ivec2 ph = world_to_hex(world);
			vec4 tint;
			if (plan_mode == 1) {
				vec4 near_t = (ph.x >= 0 && ph.y >= 0 && ph.x < hex_count.x && ph.y < hex_count.y) ? texelFetch(plan_tex, ph, 0) : vec4(0.0);
				tint = mix(near_t, texture(plan_tex, UV), smoothstep(25.0, 60.0, metres_per_pixel));
			} else {
				ivec2 lh = ph - local_origin;
				tint = (lh.x >= 0 && lh.y >= 0 && lh.x < local_size.x && lh.y < local_size.y) ? texelFetch(local_tex, lh, 0) : local_outside;
			}
			col = mix(col, vec3(dot(col, vec3(0.30, 0.55, 0.15))), 0.35);
			col = mix(col, tint.rgb, tint.a);
		}
		if (grid_alpha > 0.0) {
			ivec2 gh = world_to_hex(world);
			vec2 l = abs(world - hex_center(gh));
			float hd = max(l.x, 0.5 * l.x + 0.8660254 * l.y);
			float e_px = (HEX_W * 0.5 - hd) / metres_per_pixel;
			float g = 1.0 - smoothstep(0.0, 1.1, e_px);
			col = mix(col, col * 0.5, g * grid_alpha);
		}
		COLOR = vec4(col, 1.0);
	}
}
