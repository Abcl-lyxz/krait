#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D atlas;

void main()
{
    // The atlas is R8 coverage. Output premultiplied so the blend leaves the
    // background pass showing through the antialiased edges.
    float coverage = texture(atlas, v_uv).r * v_color.a;
    fragColor = vec4(v_color.rgb * coverage, coverage);
}
