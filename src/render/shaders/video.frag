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
} constants;

// From the screen's encoding to linear light (IEC 61966-2-1).
vec3 to_linear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

void main() {
    vec2 color_uv = uv + vec2(constants.options.y, 0.0);
    vec3 sampled = vec3(texture(plane_y, uv).r, texture(plane_cb, color_uv).r, texture(plane_cr, color_uv).r);
    vec3 rgb = clamp((constants.to_rgb * vec4(sampled * constants.options.x, 1.0)).rgb, 0.0, 1.0);
    if (constants.options.z > 0.5) rgb = to_linear(rgb);
    color = vec4(rgb, 1.0);
}
