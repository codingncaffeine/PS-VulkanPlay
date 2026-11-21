#version 450
// Quads in window pixels (y down), as src/overlay lays them out.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 atlas_position;
layout(location = 2) in vec4 color_in;
layout(location = 3) in float glyph_in;
layout(push_constant) uniform Constants { vec2 target; } constants;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;
layout(location = 2) out float glyph;

void main() {
    uv = atlas_position;
    color = color_in;
    glyph = glyph_in;
    gl_Position = vec4(position / constants.target * 2.0 - 1.0, 0.0, 1.0);
}
