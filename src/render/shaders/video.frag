#version 450
// The picture (red, green and blue as it encodes them, at its own size: convert.frag made it) drawn
// at the size it takes in the window, by the scaler chosen, then into the colors the screen takes.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;  // read sample by sample (texelFetch) only
layout(std430, set = 0, binding = 1) readonly buffer Weights {
    float weights[];  // media::weight_tables(): each kernel's weights from distance 0 to its radius
};

layout(push_constant) uniform Constants {
    vec4 size;     // xy: the picture's size in samples, zw: the size it is drawn at in pixels
    vec4 kernel;   // x: the kernel's radius in samples, y: its first entry in the weights,
                   // z: 0 drawn as it is (as large as the picture), 1 separable, 2 polar,
                   // w: entries per sample of distance
    vec4 stretch;  // xy: how much the kernel is stretched across and down (media::stretch_for)
                   // z: the picture's transfer function: 0 SDR, 1 PQ, 2 HLG
                   // w: for PQ, the light the picture is taken to reach at its brightest, in cd/m²
    vec4 options;  // x: above 0.5 when the target encodes what is written to it
} constants;

// media::kTableEntries.
const int kTableEntries = 1024;

// media::table_weight: linear between the table's entries, 0 from the radius on. A distance just
// short of the radius can come out at the last entry: read between the last two (its weight is 0).
float weight_at(float distance) {
    if (!(distance < constants.kernel.x)) return 0.0;
    float at = distance * constants.kernel.w;
    int i = min(int(at), kTableEntries - 2);
    float f = at - float(i);
    int base = int(constants.kernel.y);
    return weights[base + i] * (1.0 - f) + weights[base + i + 1] * f;
}

// media::scale_plane: the samples inside the stretched radius around the place, edges repeated.
vec3 scaled() {
    ivec2 last = ivec2(constants.size.xy) - 1;
    if (constants.kernel.z < 0.5) return texelFetch(picture, clamp(ivec2(uv * constants.size.xy), ivec2(0), last), 0).rgb;
    vec2 place = uv * constants.size.xy - 0.5;
    vec2 reach = constants.kernel.x * constants.stretch.xy;
    ivec2 from = ivec2(ceil(place - reach)), to = ivec2(floor(place + reach));
    bool polar = constants.kernel.z > 1.5;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = from.y; y <= to.y; ++y) {
        float dy = (float(y) - place.y) / constants.stretch.y;
        float wy = polar ? 1.0 : weight_at(abs(dy));
        if (wy == 0.0) continue;
        for (int x = from.x; x <= to.x; ++x) {
            float dx = (float(x) - place.x) / constants.stretch.x;
            float w = polar ? weight_at(sqrt(dx * dx + dy * dy)) : wy * weight_at(abs(dx));
            sum += w * texelFetch(picture, clamp(ivec2(x, y), ivec2(0), last), 0).rgb;
            total += w;
        }
    }
    return total != 0.0 ? sum / total : vec3(0.0);
}

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
        source_peak = clamp(source_peak_nits, kSdrWhite, 10000.0);  // a picture no brighter than white is left alone
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


void main() {
    vec3 rgb = scaled();
    if (constants.stretch.z > 0.5) {
        // Ringing of the sharper kernels can reach a little past the signal's range: hdr_to_sdr_linear cuts it.
        rgb = hdr_to_sdr_linear(rgb, constants.stretch.z, constants.stretch.w);
        if (constants.options.x < 0.5) rgb = to_encoded(rgb);
    } else {
        rgb = clamp(rgb, 0.0, 1.0);
        if (constants.options.x > 0.5) rgb = to_linear(rgb);
    }
    color = vec4(rgb, 1.0);
}
