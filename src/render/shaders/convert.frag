#version 450
// A decoded picture, at its own size, into red, green and blue as it encodes them: its brightness
// and two color differences (each in a plane of its own) read at every brightness sample, banding
// smoothed out where asked for, then converted. The window draws the result (video.frag).
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D plane_y;
layout(set = 0, binding = 1) uniform sampler2D plane_cb;
layout(set = 0, binding = 2) uniform sampler2D plane_cr;

layout(push_constant) uniform Constants {
    mat4 to_rgb;   // from (y, cb, cr, 1) to red, green, blue as the picture encodes them
    vec4 options;  // x: what a sample is multiplied by first
                   // y: how far right of a color sample's place it is read, in picture widths
                   // z: above 0.5 for an HDR picture (its color samples are filtered here, see gathered_*)
                   // w: above 0.5 when both color differences are in one plane, side by side
                   //    (a graphics card's decoder): the second of them is that plane's green
    vec4 scale;    // xy: the share of the textures the picture fills (a decoder's image may be larger)
                   // zw: the picture's size in brightness samples
    vec4 limit;    // zw: the last color sample's middle in the textures' coordinates: nothing beyond
                   // the picture is blended in at its edges
    vec4 deband;   // x: the threshold (0: no debanding), y: the radius in samples, z: the grain,
                   // w: the picture's seed (the bits of an unsigned integer)
} constants;

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

// The samples at a brightness sample of the picture, as shares of their largest value.
vec3 samples_at(ivec2 place) {
    vec2 at = (vec2(place) + 0.5) / vec2(textureSize(plane_y, 0));
    vec2 color_at = min(at + vec2(constants.options.y, 0.0), constants.limit.zw);
    vec3 sampled;
    sampled.x = texelFetch(plane_y, place, 0).r;
    if (constants.options.z > 0.5) {
        vec2 cr = gathered_rg(plane_cr, color_at);
        sampled.yz = vec2(gathered_r(plane_cb, color_at), constants.options.w > 0.5 ? cr.g : cr.r);
    } else {
        vec2 cr = texture(plane_cr, color_at).rg;
        sampled.yz = vec2(texture(plane_cb, color_at).r, constants.options.w > 0.5 ? cr.g : cr.r);
    }
    return sampled * constants.options.x;
}

// The random numbers of media/scaler.h (hash32, place_hash): the same integers on both sides.
uint hash32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main() {
    ivec2 place = ivec2(gl_FragCoord.xy);
    vec3 sampled = samples_at(place);
    float grain = 0.0;
    if (constants.deband.x > 0.0) {
        // media::deband_plane: the mean of four samples at a random offset turned four quarter
        // turns; a sample that close to it takes it, channel by channel.
        uint seed = floatBitsToUint(constants.deband.w);
        uint first = hash32(uint(place.x) + hash32(uint(place.y) + hash32(seed)));
        uint second = hash32(first), third = hash32(second), fourth = hash32(third);
        int radius = int(constants.deband.y);
        uint span = uint(2 * radius + 1);
        ivec2 o = ivec2(int(second % span) - radius, int(third % span) - radius);
        ivec2 last = ivec2(constants.scale.zw) - 1;
        vec3 mean = (samples_at(clamp(place + o, ivec2(0), last)) + samples_at(clamp(place - o, ivec2(0), last)) +
                     samples_at(clamp(place + ivec2(-o.y, o.x), ivec2(0), last)) + samples_at(clamp(place + ivec2(o.y, -o.x), ivec2(0), last))) * 0.25;
        sampled = mix(mean, sampled, step(vec3(constants.deband.x), abs(sampled - mean)));
        grain = (float(fourth >> 8) * (1.0 / 16777216.0) - 0.5) * constants.deband.z;
    }
    vec3 rgb = clamp((constants.to_rgb * vec4(sampled, 1.0)).rgb, 0.0, 1.0);
    color = vec4(clamp(rgb + grain, 0.0, 1.0), 1.0);
}
