#version 440

layout(location = 0) in vec2 corner;  // per-vertex quad corner, 0..1
layout(location = 1) in float glyph;  // per-instance: atlas cell index
layout(location = 2) in vec4 fg;      // per-instance
layout(location = 3) in vec4 bg;      // per-instance

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_fg;
layout(location = 2) out vec4 v_bg;

layout(std140, binding = 0) uniform buf {
    vec2 gridSize;  // cols, rows
    vec2 reserved;
} ubuf;

void main()
{
    float idx = float(gl_InstanceIndex);
    float col = mod(idx, ubuf.gridSize.x);
    float row = floor(idx / ubuf.gridSize.x);
    vec2 cell = vec2(col, row) + corner;
    vec2 ndc = vec2(cell.x / ubuf.gridSize.x * 2.0 - 1.0,
                    1.0 - cell.y / ubuf.gridSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = vec2((glyph + corner.x) / 95.0, corner.y);
    v_fg = fg;
    v_bg = bg;
}
