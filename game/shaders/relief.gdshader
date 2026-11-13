// Murks 1989 — height bands for the topographic mode (work board MRK-23).
//
// Drawn as one big sprite over the terrain: its texture is the height raster in metres.
// Nothing is drawn as a line. Instead the ground itself steps at every `interval` metres:
// along each step the lower band falls into shadow and the upper band catches a lit lip,
// so the border between two bands is the contour, and it is plain which side is higher.
// Steps close together mean steep ground, which also darkens a little with its grade.
shader_type canvas_item;
render_mode blend_mix, unshaded;

uniform float interval = 20.0;        // metres per height band
uniform float heavy_every = 5.0;      // every n-th step is drawn heavier (100 m at 20 m bands)
uniform float metres_per_pixel = 20.0; // ground distance one screen pixel covers
uniform float strength = 1.0;

// Smooth (bicubic B-spline) height from four bilinear taps, so band edges are curves and
// not the facets of the 180 m raster.
float height_at(sampler2D tex, vec2 uv, vec2 texel) {
	vec2 st = uv / texel - 0.5;
	vec2 f = fract(st);
	vec2 base = floor(st);
	vec2 w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
	vec2 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
	vec2 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
	vec2 w3 = f * f * f / 6.0;
	vec2 g0 = w0 + w1;
	vec2 g1 = w2 + w3;
	vec2 h0 = (base - 0.5 + w1 / g0) * texel;
	vec2 h1 = (base + 1.5 + w3 / g1) * texel;
	return textureLod(tex, vec2(h0.x, h0.y), 0.0).r * g0.x * g0.y
		+ textureLod(tex, vec2(h1.x, h0.y), 0.0).r * g1.x * g0.y
		+ textureLod(tex, vec2(h0.x, h1.y), 0.0).r * g0.x * g1.y
		+ textureLod(tex, vec2(h1.x, h1.y), 0.0).r * g1.x * g1.y;
}

void fragment() {
	float h = height_at(TEXTURE, UV, TEXTURE_PIXEL_SIZE);
	float f = h / interval;
	float per_px = max(fwidth(f), 1e-6);          // bands crossed per screen pixel
	float band = floor(f);
	float fr = f - band;
	float px_below_step = (1.0 - fr) / per_px;    // pixels up to the next step (we are on the lower band)
	float px_above_step = fr / per_px;            // pixels down to the step we stand above
	bool heavy_up = mod(band + 1.0, heavy_every) < 0.5;
	bool heavy_dn = mod(band, heavy_every) < 0.5;

	// Steps closer than a few pixels would turn to noise: fade them out (the heavy ones last).
	float room = 1.0 / per_px;                    // pixels per band
	float fade = smoothstep(2.5, 7.0, room);
	float fade_heavy = smoothstep(2.5, 7.0, room * heavy_every);

	float shadow = smoothstep(heavy_up ? 6.0 : 3.5, 0.0, px_below_step) * (heavy_up ? 0.62 * max(fade, fade_heavy) : 0.42 * fade);
	float lip = smoothstep(heavy_dn ? 3.0 : 2.0, 0.0, px_above_step) * (heavy_dn ? 0.34 * max(fade, fade_heavy) : 0.24 * fade);

	// Grade of the ground (rise over run): steeper ground is darker.
	float grade = per_px * interval / metres_per_pixel;
	float steep = clamp((grade - 0.03) * 1.6, 0.0, 0.34);
	// Alternate bands differ a touch in tone, so a band reads as one surface.
	float tone = (mod(band, 2.0) < 0.5 ? 0.05 : 0.0) * fade;

	float below_sea = step(h, 0.5);               // no relief on the sea
	float dark = max(shadow, steep + tone) * (1.0 - below_sea) * strength;
	float light = lip * (1.0 - below_sea) * strength;
	if (light > dark) {
		COLOR = vec4(1.0, 0.98, 0.88, light - dark * 0.5);
	} else {
		COLOR = vec4(0.13, 0.08, 0.04, dark - light * 0.5);
	}
}
