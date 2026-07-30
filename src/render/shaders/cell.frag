#version 440

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 fragColor;

void main()
{
    // Premultiplied out, to match the One / OneMinusSrcAlpha blend the pipeline
    // sets. A translucent selection rect is the only case where alpha < 1.
    fragColor = vec4(v_color.rgb * v_color.a, v_color.a);
}
