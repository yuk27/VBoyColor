#version 450

// Both eyes' pictures as one, for colored 3D glasses (Settings > Screen > 3D
// on a flat screen - see Screen3D in Settings.h): each lens lets through
// only its own colors, so each eye sees only its own picture.
//
// The two pictures are mixed with Eric Dubois' least-squares matrices
// ("A Projection Method to Generate Anaglyph Stereo Images", ICASSP 2001;
// the matrices as published on his site, also used by Bino and other
// players), which keep the colors as close to the original as the glasses
// allow - the games are in color, and simply giving each eye its own
// channels would turn red things invisible to one eye. They work on linear
// light.
//
// uImage: the colored picture of both eyes side by side (Emulator::
// PrepareScreen's), read as stored - sRGB-encoded. uvRect: the left eye's
// part of it; the right eye's is the same shifted by patternColors[0].
// filterMode: the glasses - 1 red/cyan, 2 green/magenta, 3 amber/blue.
// Drawn straight to the window (scaled, pixel-sharp) or 1:1 into a texture
// that a Look (screen_filter.frag) then draws.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uImage;

// Full redeclaration matching ui.vert's PushConstants field-for-field (see
// screen_pattern.frag for why it's never partial).
layout(push_constant) uniform PushConstants {
    vec2 posPx;
    vec2 sizePx;
    vec4 uvRect;
    vec4 color;
    vec2 screenSizePx;
    float cornerRadiusPx;
    float pixelScale;
    float patternColors[15];
    float filterMode;
} pc;

vec3 SrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

// A pixel of one eye's picture at uv (that eye's coordinates): sharp pixels
// with an antialiased seam between them when scaled (like ui_image.frag's
// SamplePixelPerfectAA), kept inside the eye's own part of the texture so
// nothing of the other eye bleeds in at its edge.
vec3 SampleEye(vec2 t, vec2 dt, vec2 texSize, vec2 rectMin, vec2 rectMax) {
    vec2 seam = floor(t + 0.5);
    vec2 p = (t - seam) / dt + seam;
    p = clamp(p, seam - 0.5, seam + 0.5);
    p = clamp(p, rectMin + 0.5, rectMax - 0.5);
    return texture(uImage, p / texSize).rgb;
}

// Row r of a 3x3 matrix given row by row.
float Row(vec3 row, vec3 c) { return dot(row, c); }

void main() {
    vec2 texSize = vec2(textureSize(uImage, 0));
    vec2 t = vUV * texSize;
    vec2 dt = max(fwidth(t), vec2(1e-5)); // (once, in uniform control flow)
    vec2 offset = vec2(pc.patternColors[0] * texSize.x, 0.0);
    vec2 rectMin = pc.uvRect.xy * texSize, rectMax = pc.uvRect.zw * texSize;

    vec3 l = SrgbToLinear(SampleEye(t, dt, texSize, rectMin, rectMax));
    vec3 r = SrgbToLinear(SampleEye(t + offset, dt, texSize, rectMin + offset, rectMax + offset));

    vec3 c;
    int glasses = int(pc.filterMode + 0.5);
    if (glasses == 2) {
        // green (left) / magenta (right)
        c.r = Row(vec3(-0.062, -0.158, -0.039), l) + Row(vec3(0.529, 0.705, 0.024), r);
        c.g = Row(vec3(0.284, 0.668, 0.143), l) + Row(vec3(-0.016, -0.015, -0.065), r);
        c.b = Row(vec3(-0.015, -0.027, 0.021), l) + Row(vec3(0.009, 0.075, 0.937), r);
    } else if (glasses == 3) {
        // amber (left) / blue (right) - ColorCode 3-D
        c.r = Row(vec3(1.062, -0.205, 0.299), l) + Row(vec3(-0.016, -0.123, -0.017), r);
        c.g = Row(vec3(-0.026, 0.908, 0.068), l) + Row(vec3(0.006, 0.062, -0.017), r);
        c.b = Row(vec3(-0.038, -0.173, 0.022), l) + Row(vec3(0.094, 0.185, 0.911), r);
    } else {
        // red (left) / cyan (right)
        c.r = Row(vec3(0.4561, 0.500484, 0.176381), l) + Row(vec3(-0.0434706, -0.0879388, -0.00155529), r);
        c.g = Row(vec3(-0.0400822, -0.0378246, -0.0157589), l) + Row(vec3(0.378476, 0.73364, -0.0184503), r);
        c.b = Row(vec3(-0.0152161, -0.0205971, -0.00546856), l) + Row(vec3(-0.0721527, -0.112961, 1.2264), r);
    }
    // Linear out - the target is sRGB and encodes it.
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
