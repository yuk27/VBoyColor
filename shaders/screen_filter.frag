#version 450

// The game screen with a "Look" (Settings > Screen > Look) - drawn by
// Emulator::DrawScreen instead of ui_image.frag / screen_pattern.frag when
// the look isn't Sharp:
//
//   Smooth (filterMode 1): OmniScale by Lior Halphon (SameBoy, Expat/MIT
//     license - see licenses/Expat-SameBoy-OmniScale.txt), a pattern-based
//     pixel-art scaler for any output size: edges and diagonals become
//     smooth lines, flat areas stay flat.
//   LED (filterMode 2): how the Virtual Boy itself showed a picture - a
//     column of LEDs swept sideways by a mirror: rows with dark gaps between
//     them, columns blending into each other along the sweep, and a soft
//     glow around what's lit.
//
// It reads the picture already in its colors (Emulator::PrepareScreen) and
// as stored - sRGB-encoded, as SameBoy's OmniScale expects its input.
//
// Both eyes: everything here looks only at a pixel's own small
// neighborhood (at most 2 pixels away) inside its own eye's picture, with
// the same output size and pixel phase for both eyes, and positions
// rounded so they're the very same numbers in both (see main). The Virtual
// Boy makes its 3D by shifting whole layers sideways by whole pixels, so
// wherever a thing looks the same in both eyes its neighborhood is the same
// too, and it comes out exactly the same in both - the two pictures only
// differ where they already differ: along the outline of something in
// front of something else, a pixel or two wider than the sharp look.
//
// filterMode: 1 Smooth / 2 LED.

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

// This eye's part of the screen texture, in texels: [gRectMin, gRectMax].
ivec2 gRectMin;
ivec2 gRectMax;

// A pixel of this eye's picture, in its colors, sRGB-encoded - clamped to
// the eye's own picture, so nothing of the other eye's (or of the texture's
// unused rest) ever leaks in at the edges.
vec3 Fetch(ivec2 p) {
    return texelFetch(uImage, clamp(p, gRectMin, gRectMax), 0).rgb;
}

// ---------------------------------------------------------------------------
// Smooth: OmniScale (SameBoy's Shaders/OmniScale.fsh, ported: texel
// fetches through Fetch, vec3 colors).

vec3 HqColorspace(vec3 rgb) {
    return vec3(0.250 * rgb.r + 0.250 * rgb.g + 0.250 * rgb.b,
                0.250 * rgb.r - 0.000 * rgb.g - 0.250 * rgb.b,
                -0.125 * rgb.r + 0.250 * rgb.g - 0.125 * rgb.b);
}

bool IsDifferent(vec3 a, vec3 b) {
    vec3 diff = abs(HqColorspace(a - b)); // (linear: one transform, not two)
    return diff.x > 0.018 || diff.y > 0.002 || diff.z > 0.005;
}

#define P(m, r) ((pattern & (m)) == (r))

vec3 OmniScale(vec2 position, float outPixel) {
    ivec2 base = ivec2(floor(position));
    ivec2 o = ivec2(1, 1);
    vec2 p = fract(position);
    // We always calculate the top left quarter. If we need a different
    // quarter, we flip our co-ordinates.
    if (p.x > 0.5) {
        o.x = -o.x;
        p.x = 1.0 - p.x;
    }
    if (p.y > 0.5) {
        o.y = -o.y;
        p.y = 1.0 - p.y;
    }

    vec3 w0 = Fetch(base + ivec2(-o.x, -o.y));
    vec3 w1 = Fetch(base + ivec2(0, -o.y));
    vec3 w2 = Fetch(base + ivec2(o.x, -o.y));
    vec3 w3 = Fetch(base + ivec2(-o.x, 0));
    vec3 w4 = Fetch(base);
    vec3 w5 = Fetch(base + ivec2(o.x, 0));
    vec3 w6 = Fetch(base + ivec2(-o.x, o.y));
    vec3 w7 = Fetch(base + ivec2(0, o.y));
    vec3 w8 = Fetch(base + ivec2(o.x, o.y));

    int pattern = 0;
    if (IsDifferent(w0, w4)) pattern |= 1 << 0;
    if (IsDifferent(w1, w4)) pattern |= 1 << 1;
    if (IsDifferent(w2, w4)) pattern |= 1 << 2;
    if (IsDifferent(w3, w4)) pattern |= 1 << 3;
    if (IsDifferent(w5, w4)) pattern |= 1 << 4;
    if (IsDifferent(w6, w4)) pattern |= 1 << 5;
    if (IsDifferent(w7, w4)) pattern |= 1 << 6;
    if (IsDifferent(w8, w4)) pattern |= 1 << 7;

    if ((P(0xBF, 0x37) || P(0xDB, 0x13)) && IsDifferent(w1, w5))
        return mix(w4, w3, 0.5 - p.x);
    if ((P(0xDB, 0x49) || P(0xEF, 0x6D)) && IsDifferent(w7, w3))
        return mix(w4, w1, 0.5 - p.y);
    if ((P(0x0B, 0x0B) || P(0xFE, 0x4A) || P(0xFE, 0x1A)) && IsDifferent(w3, w1))
        return w4;
    if ((P(0x6F, 0x2A) || P(0x5B, 0x0A) || P(0xBF, 0x3A) || P(0xDF, 0x5A) ||
         P(0x9F, 0x8A) || P(0xCF, 0x8A) || P(0xEF, 0x4E) || P(0x3F, 0x0E) ||
         P(0xFB, 0x5A) || P(0xBB, 0x8A) || P(0x7F, 0x5A) || P(0xAF, 0x8A) ||
         P(0xEB, 0x8A)) && IsDifferent(w3, w1))
        return mix(w4, mix(w4, w0, 0.5 - p.x), 0.5 - p.y);
    if (P(0x0B, 0x08))
        return mix(mix(w0 * 0.375 + w1 * 0.25 + w4 * 0.375, w4 * 0.5 + w1 * 0.5, p.x * 2.0), w4, p.y * 2.0);
    if (P(0x0B, 0x02))
        return mix(mix(w0 * 0.375 + w3 * 0.25 + w4 * 0.375, w4 * 0.5 + w3 * 0.5, p.y * 2.0), w4, p.x * 2.0);
    if (P(0x2F, 0x2F)) {
        float dist = length(p - vec2(0.5));
        float pixelSize = outPixel;
        if (dist < 0.5 - pixelSize / 2.0)
            return w4;
        vec3 r;
        if (IsDifferent(w0, w1) || IsDifferent(w0, w3))
            r = mix(w1, w3, p.y - p.x + 0.5);
        else
            r = mix(mix(w1 * 0.375 + w0 * 0.25 + w3 * 0.375, w3, p.y * 2.0), w1, p.x * 2.0);
        if (dist > 0.5 + pixelSize / 2.0)
            return r;
        return mix(w4, r, (dist - 0.5 + pixelSize / 2.0) / pixelSize);
    }
    if (P(0xBF, 0x37) || P(0xDB, 0x13)) {
        float dist = p.x - 2.0 * p.y;
        float pixelSize = outPixel * sqrt(5.0);
        if (dist > pixelSize / 2.0)
            return w1;
        vec3 r = mix(w3, w4, p.x + 0.5);
        if (dist < -pixelSize / 2.0)
            return r;
        return mix(r, w1, (dist + pixelSize / 2.0) / pixelSize);
    }
    if (P(0xDB, 0x49) || P(0xEF, 0x6D)) {
        float dist = p.y - 2.0 * p.x;
        float pixelSize = outPixel * sqrt(5.0);
        if (p.y - 2.0 * p.x > pixelSize / 2.0)
            return w3;
        vec3 r = mix(w1, w4, p.x + 0.5);
        if (dist < -pixelSize / 2.0)
            return r;
        return mix(r, w3, (dist + pixelSize / 2.0) / pixelSize);
    }
    if (P(0xBF, 0x8F) || P(0x7E, 0x0E)) {
        float dist = p.x + 2.0 * p.y;
        float pixelSize = outPixel * sqrt(5.0);
        if (dist > 1.0 + pixelSize / 2.0)
            return w4;
        vec3 r;
        if (IsDifferent(w0, w1) || IsDifferent(w0, w3))
            r = mix(w1, w3, p.y - p.x + 0.5);
        else
            r = mix(mix(w1 * 0.375 + w0 * 0.25 + w3 * 0.375, w3, p.y * 2.0), w1, p.x * 2.0);
        if (dist < 1.0 - pixelSize / 2.0)
            return r;
        return mix(r, w4, (dist + pixelSize / 2.0 - 1.0) / pixelSize);
    }
    if (P(0x7E, 0x2A) || P(0xEF, 0xAB)) {
        float dist = p.y + 2.0 * p.x;
        float pixelSize = outPixel * sqrt(5.0);
        if (p.y + 2.0 * p.x > 1.0 + pixelSize / 2.0)
            return w4;
        vec3 r;
        if (IsDifferent(w0, w1) || IsDifferent(w0, w3))
            r = mix(w1, w3, p.y - p.x + 0.5);
        else
            r = mix(mix(w1 * 0.375 + w0 * 0.25 + w3 * 0.375, w3, p.y * 2.0), w1, p.x * 2.0);
        if (dist < 1.0 - pixelSize / 2.0)
            return r;
        return mix(r, w4, (dist + pixelSize / 2.0 - 1.0) / pixelSize);
    }
    if (P(0x1B, 0x03) || P(0x4F, 0x43) || P(0x8B, 0x83) || P(0x6B, 0x43))
        return mix(w4, w3, 0.5 - p.x);
    if (P(0x4B, 0x09) || P(0x8B, 0x89) || P(0x1F, 0x19) || P(0x3B, 0x19))
        return mix(w4, w1, 0.5 - p.y);
    if (P(0xFB, 0x6A) || P(0x6F, 0x6E) || P(0x3F, 0x3E) || P(0xFB, 0xFA) ||
        P(0xDF, 0xDE) || P(0xDF, 0x1E))
        return mix(w4, w0, (1.0 - p.x - p.y) / 2.0);
    if (P(0x4F, 0x4B) || P(0x9F, 0x1B) || P(0x2F, 0x0B) ||
        P(0xBE, 0x0A) || P(0xEE, 0x0A) || P(0x7E, 0x0A) || P(0xEB, 0x4B) ||
        P(0x3B, 0x1B)) {
        float dist = p.x + p.y;
        float pixelSize = outPixel;
        if (dist > 0.5 + pixelSize / 2.0)
            return w4;
        vec3 r;
        if (IsDifferent(w0, w1) || IsDifferent(w0, w3))
            r = mix(w1, w3, p.y - p.x + 0.5);
        else
            r = mix(mix(w1 * 0.375 + w0 * 0.25 + w3 * 0.375, w3, p.y * 2.0), w1, p.x * 2.0);
        if (dist < 0.5 - pixelSize / 2.0)
            return r;
        return mix(r, w4, (dist + pixelSize / 2.0 - 0.5) / pixelSize);
    }
    if (P(0x0B, 0x01))
        return mix(mix(w4, w3, 0.5 - p.x), mix(w1, (w1 + w3) / 2.0, 0.5 - p.x), 0.5 - p.y);
    if (P(0x0B, 0x00))
        return mix(mix(w4, w3, 0.5 - p.x), mix(w1, w0, 0.5 - p.x), 0.5 - p.y);

    float dist = p.x + p.y;
    float pixelSize = outPixel;
    if (dist > 0.5 + pixelSize / 2.0)
        return w4;

    // We need more samples to "solve" this diagonal.
    vec3 x0 = Fetch(base + ivec2(-o.x * 2, -o.y * 2));
    vec3 x1 = Fetch(base + ivec2(-o.x, -o.y * 2));
    vec3 x2 = Fetch(base + ivec2(0, -o.y * 2));
    vec3 x3 = Fetch(base + ivec2(o.x, -o.y * 2));
    vec3 x4 = Fetch(base + ivec2(-o.x * 2, -o.y));
    vec3 x5 = Fetch(base + ivec2(-o.x * 2, 0));
    vec3 x6 = Fetch(base + ivec2(-o.x * 2, o.y));
    if (IsDifferent(x0, w4)) pattern |= 1 << 8;
    if (IsDifferent(x1, w4)) pattern |= 1 << 9;
    if (IsDifferent(x2, w4)) pattern |= 1 << 10;
    if (IsDifferent(x3, w4)) pattern |= 1 << 11;
    if (IsDifferent(x4, w4)) pattern |= 1 << 12;
    if (IsDifferent(x5, w4)) pattern |= 1 << 13;
    if (IsDifferent(x6, w4)) pattern |= 1 << 14;

    int diagonalBias = -7;
    while (pattern != 0) {
        diagonalBias += pattern & 1;
        pattern >>= 1;
    }
    if (diagonalBias <= 0) {
        vec3 r = mix(w1, w3, p.y - p.x + 0.5);
        if (dist < 0.5 - pixelSize / 2.0)
            return r;
        return mix(r, w4, (dist + pixelSize / 2.0 - 0.5) / pixelSize);
    }
    return w4;
}

// ---------------------------------------------------------------------------
// LED: rows of light with dark gaps, blended along the sweep, a soft glow.

vec3 Led(vec2 position, vec2 outPixel) {
    // Along a row (the mirror's sweep): neighbouring columns blend softly.
    // (Light adds up linearly: blended decoded.)
    float x = position.x - 0.5;
    ivec2 cell = ivec2(int(floor(x)), int(floor(position.y)));
    float fx = smoothstep(0.2, 0.8, x - floor(x));
    vec3 lit = mix(SrgbToLinear(Fetch(cell)), SrgbToLinear(Fetch(cell + ivec2(1, 0))), fx);

    // Across rows: each LED's line is brightest in its middle, with a dark
    // gap to the next - fading out when a row covers too few pixels of the
    // display to show it (a small window, a small screen in the headset),
    // so it never turns into stripes or moire. In the headset what's drawn
    // here is resampled onto the display, so the app tells us how big a
    // row ends up there (cornerRadiusPx, which this shader never rounds).
    float dy = fract(position.y) - 0.5;
    float beam = exp(-dy * dy / (2.0 * 0.26 * 0.26));
    float shown = pc.cornerRadiusPx > 0.0 ? pc.cornerRadiusPx : 1.0 / outPixel.y;
    float structure = clamp((shown - 2.0) / 2.0, 0.0, 1.0);
    lit *= mix(1.0, beam * 1.3, structure);

    // The glow: the 3x3 around it, weighted by distance.
    vec3 glow = vec3(0.0);
    float total = 0.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec2 d = vec2(float(i), float(j)) + vec2(0.5) - fract(position);
            float w = exp(-dot(d, d) / (2.0 * 0.9 * 0.9));
            vec3 c = Fetch(ivec2(floor(position)) + ivec2(i, j));
            glow += c * c * w; // (decoded near enough for a soft glow)
            total += w;
        }
    return lit + glow / total * 0.35;
}

void main() {
    vec2 texSize = vec2(textureSize(uImage, 0));
    gRectMin = ivec2(floor(pc.uvRect.xy * texSize + 0.5));
    gRectMax = ivec2(floor(pc.uvRect.zw * texSize + 0.5)) - ivec2(1);
    int mode = int(pc.filterMode + 0.5);

    // Where this output pixel is in the picture, in its pixels, and how big
    // an output pixel is there (derivatives first - outside any branch).
    vec2 position = vUV * texSize;
    vec2 outPixel = fwidth(position);
    // Rounded to 1/256 of a pixel (and the size to 1/4096), measured from
    // the eye's own corner: the two eyes' quads sit at different places on
    // the screen, so their interpolated positions differ in the last few
    // bits - enough to flip a decision made exactly on a pixel's middle
    // (where every odd scale, like the headset's 5x, puts an output pixel).
    // Rounded, the same spot in both eyes is the very same number.
    position = floor((position - vec2(gRectMin)) * 256.0 + 0.5) / 256.0 + vec2(gRectMin);
    outPixel = floor(outPixel * 4096.0 + 0.5) / 4096.0;

    // (The target is _SRGB: what's written is linear.)
    vec3 color = mode == 2 ? Led(position, outPixel) : SrgbToLinear(clamp(OmniScale(position, length(outPixel)), 0.0, 1.0));
    outColor = vec4(color, vColor.a);
}
