#version 450
// A panel is its color; a glyph is cut out of the distance field: 0.5 is its outline, more is inside.
// The edge is smoothed over about one pixel of the screen, whatever the text's size.
layout(binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in float glyph;
layout(location = 0) out vec4 result;

void main() {
    float coverage = 1.0;
    if (glyph > 0.5) {
        float distance = texture(atlas, uv).r;
        float width = max(fwidth(distance) * 0.7, 1.0 / 255.0);
        coverage = smoothstep(0.5 - width, 0.5 + width, distance);
    }
    result = vec4(color.rgb, color.a * coverage);
}
