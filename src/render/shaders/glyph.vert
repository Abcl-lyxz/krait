#version 440

// One textured quad per shaped glyph, positioned in PIXEL space. The position
// already includes the cluster's column, the shaper's offsets and the glyph's
// bearing, so nothing here needs to know about cells.
layout(location = 0) in vec2 corner;
layout(location = 1) in vec4 rect;   // x, y, w, h in pixels
layout(location = 2) in vec4 uv;     // u0, v0, u1, v1 normalised
layout(location = 3) in vec4 color;  // glyph colour, rgba

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(std140, binding = 0) uniform buf {
    vec2 viewport;
    vec2 reserved;
} ubuf;

void main()
{
    vec2 px = rect.xy + corner * rect.zw;
    gl_Position = vec4((px.x / ubuf.viewport.x) * 2.0 - 1.0,
                       1.0 - (px.y / ubuf.viewport.y) * 2.0,
                       0.0, 1.0);
    v_uv = mix(uv.xy, uv.zw, corner);
    v_color = color;
}
