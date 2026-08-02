#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D image;

void main()
{
    // .bgra, not .rgba. The decoders hand over 0xAARRGGBB as a uint32, which on
    // a little-endian host is B,G,R,A in memory, and the texture is uploaded as
    // RGBA8 — the one format QRhi documents as "always supported", so there is
    // no isTextureFormatSupported() branch and no BGRA8 fallback path to test.
    // Swizzling here costs nothing and keeps the CPU out of the pixels.
    vec4 texel = texture(image, v_uv).bgra;
    // Straight alpha in, premultiplied out, matching the One /
    // OneMinusSrcAlpha blend the pipeline sets. Sixel's transparency is a
    // whole-pixel "never written" rather than a blend, so the colour of a fully
    // transparent pixel must not leak — multiplying by alpha is what drops it.
    float alpha = texel.a * v_color.a;
    fragColor = vec4(texel.rgb * v_color.rgb * alpha, alpha);
}
