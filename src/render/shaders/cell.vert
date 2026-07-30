#version 440

// Solid rectangles in PIXEL space: cell backgrounds, selection, cursor,
// underlines, strikethrough. One pipeline for all of them — they differ only in
// extent and colour.
layout(location = 0) in vec2 corner;  // per-vertex quad corner, 0..1
layout(location = 1) in vec4 rect;    // per-instance: x, y, w, h in pixels
layout(location = 2) in vec4 color;   // per-instance: rgba, straight alpha

layout(location = 0) out vec4 v_color;

layout(std140, binding = 0) uniform buf {
    vec2 viewport;  // pixels
    vec2 reserved;
} ubuf;

void main()
{
    vec2 px = rect.xy + corner * rect.zw;
    gl_Position = vec4((px.x / ubuf.viewport.x) * 2.0 - 1.0,
                       1.0 - (px.y / ubuf.viewport.y) * 2.0,
                       0.0, 1.0);
    v_color = color;
}
