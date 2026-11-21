#version 450
// A decoded picture: brightness and two color differences, each in a plane of
// its own, turned into the red, green and blue the screen takes.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D plane_y;
layout(set = 0, binding = 1) uniform sampler2D plane_cb;
layout(set = 0, binding = 2) uniform sampler2D plane_cr;

layout(push_constant) uniform Constants {
    mat4 to_rgb;   // from (y, cb, cr, 1) to red, green, blue as the screen encodes them
    vec4 options;  // x: what a sample is multiplied by first
                   // y: how far right of a color sample's place it is read, in picture widths
                   // z: above 0.5 when the target encodes what is written to it
                   // w: above 0.5 when both color differences are in one plane, side by side
                   //    (a graphics card's decoder): the second of them is that plane's green
    vec4 scale;    // xy: the share of the textures the picture fills (a decoder's image may be larger)
                   // z: the picture's transfer function: 0 SDR, 1 PQ, 2 HLG
                   // w: for PQ, the light the picture is taken to reach at its brightest, in cd/m²
    vec4 limit;    // xy: the last brightness sample's middle, zw: the last color sample's, in the textures'
                   // coordinates: nothing beyond the picture is blended in at its edges
} constants;

// From the screen's encoding to linear light (IEC 61966-2-1).
vec3 to_linear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

// From an HDR picture's colors to SDR, the steps of media/tone_map.cpp (whose numbers these
// pixels are checked against): the transfer function undone, BT.2020 primaries to BT.709, the
// brightest channel through the EETF of ITU-R BT.2390, HDR reference white (203 cd/m²) as white.
const float kSdrWhite = 203.0;
const float kHlgPeak = 1000.0;
const float kM1 = 2610.0 / 16384.0;
const float kM2 = 2523.0 / 4096.0 * 128.0;
const float kC1 = 3424.0 / 4096.0;
const float kC2 = 2413.0 / 4096.0 * 32.0;
const float kC3 = 2392.0 / 4096.0 * 32.0;

vec3 pq_to_nits(vec3 signal) {
    vec3 p = pow(clamp(signal, 0.0, 1.0), vec3(1.0 / kM2));
    return 10000.0 * pow(max(p - kC1, 0.0) / (kC2 - kC3 * p), vec3(1.0 / kM1));
}

float nits_to_pq(float nits) {
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), kM1);
    return pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2);
}

vec3 hlg_to_nits(vec3 signal) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    vec3 e = clamp(signal, 0.0, 1.0);
    // The two pieces meet at 0.5 (both give 1/12).
    vec3 scene = mix(e * e / 3.0, (exp((e - c) / a) + b) / 12.0, step(vec3(0.5), e));
    float ys = dot(scene, vec3(0.2627, 0.6780, 0.0593));
    return ys > 0.0 ? kHlgPeak * pow(ys, 0.2) * scene : vec3(0.0);
}

float bt2390_eetf(float signal, float source_peak, float target_peak) {
    float max_lum = target_peak / source_peak;
    if (max_lum >= 1.0) return clamp(signal, 0.0, source_peak);
    float e1 = clamp(signal / source_peak, 0.0, 1.0);
    float ks = max(1.5 * max_lum - 0.5, 0.0);
    float e2 = e1;
    if (e1 >= ks) {
        float t = (e1 - ks) / (1.0 - ks);
        float t2 = t * t, t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * max_lum;
    }
    return e2 * source_peak;
}

vec3 hdr_to_sdr_linear(vec3 encoded, float transfer, float source_peak_nits) {
    vec3 nits;
    float source_peak = kHlgPeak;
    if (transfer > 1.5) {
        nits = hlg_to_nits(encoded);
    } else {
        source_peak = clamp(source_peak_nits, 400.0, 10000.0);
        nits = pq_to_nits(encoded);
    }
    // GLSL matrices are given column by column: these columns are the rows of media::bt2020_to_bt709's.
    const mat3 to_bt709 = mat3(1.6604910021, -0.1245504745, -0.0181507634,
                               -0.5876411388, 1.1328998971, -0.1005788980,
                               -0.0728498633, -0.0083494226, 1.1187296614);
    vec3 light = max(to_bt709 * nits, 0.0);
    float brightest = max(light.r, max(light.g, light.b));
    if (brightest <= 0.0) return vec3(0.0);
    float mapped = pq_to_nits(vec3(bt2390_eetf(nits_to_pq(brightest), nits_to_pq(source_peak), nits_to_pq(kSdrWhite)))).r;
    return clamp(light * (mapped / brightest / kSdrWhite), 0.0, 1.0);
}

// From linear light to the screen's encoding (IEC 61966-2-1).
vec3 to_encoded(vec3 linear) {
    return mix(12.92 * linear, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), linear));
}

// Linear filtering worked here in single precision from the four samples around a place, read as
// they are stored (textureGather does not filter). A graphics card's own filtering of 16-bit samples
// can be coarser: seen 2026-11-21 on an RTX 5080, a card decoder's samples (in the high ten bits of
// sixteen) came out up to half a step of ten bits off, which tone mapping made up to 4 steps of 255
// in a bright sky. The same places as the sampler's: half a sample in, the edges repeated.
vec4 corners(vec2 at, vec2 size, out vec2 weight) {
    vec2 t = at * size - 0.5;
    weight = fract(t);
    return vec4((floor(t) + 1.0) / size, 0.0, 0.0);
}

float blend(vec4 g, vec2 weight) {
    // textureGather's order: (left, below), (right, below), (right, above), (left, above).
    return mix(mix(g.w, g.z, weight.x), mix(g.x, g.y, weight.x), weight.y);
}

float gathered_r(sampler2D plane, vec2 at) {
    vec2 weight;
    vec2 middle = corners(at, vec2(textureSize(plane, 0)), weight).xy;
    return blend(textureGather(plane, middle, 0), weight);
}

vec2 gathered_rg(sampler2D plane, vec2 at) {
    vec2 weight;
    vec2 middle = corners(at, vec2(textureSize(plane, 0)), weight).xy;
    return vec2(blend(textureGather(plane, middle, 0), weight), blend(textureGather(plane, middle, 1), weight));
}

void main() {
    vec2 picture_uv = min(uv * constants.scale.xy, constants.limit.xy);
    vec2 color_uv = min(uv * constants.scale.xy + vec2(constants.options.y, 0.0), constants.limit.zw);
    vec3 sampled;
    if (constants.scale.z > 0.5) {
        // HDR: filtered here (see gathered_*); the tone curve makes small errors of the samples large.
        vec2 cr = gathered_rg(plane_cr, color_uv);
        sampled = vec3(gathered_r(plane_y, picture_uv), gathered_r(plane_cb, color_uv), constants.options.w > 0.5 ? cr.g : cr.r);
    } else {
        vec2 cr = texture(plane_cr, color_uv).rg;
        sampled = vec3(texture(plane_y, picture_uv).r, texture(plane_cb, color_uv).r, constants.options.w > 0.5 ? cr.g : cr.r);
    }
    vec3 rgb = clamp((constants.to_rgb * vec4(sampled * constants.options.x, 1.0)).rgb, 0.0, 1.0);
    if (constants.scale.z > 0.5) {
        rgb = hdr_to_sdr_linear(rgb, constants.scale.z, constants.scale.w);
        if (constants.options.z < 0.5) rgb = to_encoded(rgb);
    } else if (constants.options.z > 0.5) {
        rgb = to_linear(rgb);
    }
    color = vec4(rgb, 1.0);
}
