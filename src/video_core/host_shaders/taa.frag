// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Temporal resolve without motion vectors: the previous output is blended into the current
// frame after being clipped to the colour range of the current pixel's 3x3 neighbourhood, so
// stale history (moving objects, scene cuts) is rejected instead of smearing.

#version 460

layout (location = 0) in vec2 texcoord;

layout (location = 0) out vec4 frag_color;

layout (binding = 0) uniform sampler2D current_texture;
layout (binding = 1) uniform sampler2D history_texture;

// Weight of the current frame when the history agrees with it, and when it had to be clipped
const float CURRENT_WEIGHT_STABLE = 0.12;
const float CURRENT_WEIGHT_CLIPPED = 0.5;
// Width of the neighbourhood colour box in standard deviations
const float CLIP_GAMMA = 1.25;

vec3 RgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 YCoCgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main() {
    const ivec2 size = textureSize(current_texture, 0);
    const ivec2 pos = clamp(ivec2(texcoord * vec2(size)), ivec2(0), size - 1);

    const vec4 center = texelFetch(current_texture, pos, 0);
    const vec3 current = RgbToYCoCg(center.rgb);

    vec3 moment1 = vec3(0.0);
    vec3 moment2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const ivec2 sample_pos = clamp(pos + ivec2(x, y), ivec2(0), size - 1);
            const vec3 s = RgbToYCoCg(texelFetch(current_texture, sample_pos, 0).rgb);
            moment1 += s;
            moment2 += s * s;
        }
    }
    const vec3 mean = moment1 / 9.0;
    const vec3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, vec3(0.0)));
    const vec3 box_min = mean - CLIP_GAMMA * sigma;
    const vec3 box_max = mean + CLIP_GAMMA * sigma;

    const vec3 history = RgbToYCoCg(texelFetch(history_texture, pos, 0).rgb);
    const vec3 clipped = clamp(history, box_min, box_max);

    // The further the history was outside the box, the less it is trusted
    const float box_size = length(box_max - box_min) + 1e-4;
    const float rejection = clamp(length(history - clipped) / box_size, 0.0, 1.0);
    const float current_weight = mix(CURRENT_WEIGHT_STABLE, CURRENT_WEIGHT_CLIPPED, rejection);

    // Luma-weighted blend keeps bright pixels from flickering through the history
    const float weight_current = current_weight / (1.0 + current.x);
    const float weight_history = (1.0 - current_weight) / (1.0 + clipped.x);
    const vec3 resolved =
        (current * weight_current + clipped * weight_history) / (weight_current + weight_history);

    frag_color = vec4(YCoCgToRgb(resolved), center.a);
}
