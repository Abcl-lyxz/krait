#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_fg;
layout(location = 2) in vec4 v_bg;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D atlas;

void main()
{
    float coverage = texture(atlas, v_uv).r;
    fragColor = mix(v_bg, v_fg, coverage);
}
