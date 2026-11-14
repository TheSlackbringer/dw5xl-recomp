#version 450

// SPDX-License-Identifier: GPL-3.0-only
//
// DW5XL (issue 047, PC SETTINGS' SHARPEN; issue 049 amendment v3.12): the game's picture drawn into the window with
// contrast-adaptive sharpening, for the window the renderer draws itself. The same sum as the shader of the OpenGL
// window (kPresentSharpenShader in ps2_runtime.cpp): the picture's value and its four neighbours one picture pixel
// away, each read through the picture's bilinear sampler; the less contrast around a pixel, the stronger.
//
// It is used with the renderer library's quad.vert (three corners that cover the viewport, vUV from 0 to 1) and is
// kept compiled in ../gs_vulkan_window_shaders.h; after a change compile it again as that file says.

layout(set = 0, binding = 0) uniform sampler2D uPicture;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform Registers
{
    vec2 texel; // one picture pixel in vUV's units
} registers;

void main()
{
    vec3 c = textureLod(uPicture, vUV, 0.0).rgb;
    vec3 n = textureLod(uPicture, vUV + vec2(0.0, -registers.texel.y), 0.0).rgb;
    vec3 e = textureLod(uPicture, vUV + vec2(registers.texel.x, 0.0), 0.0).rgb;
    vec3 w = textureLod(uPicture, vUV + vec2(-registers.texel.x, 0.0), 0.0).rgb;
    vec3 s = textureLod(uPicture, vUV + vec2(0.0, registers.texel.y), 0.0).rgb;
    vec3 lo = min(c, min(n, min(e, min(w, s))));
    vec3 hi = max(c, max(n, max(e, max(w, s))));
    vec3 a = clamp(min(lo, 1.0 - hi) / max(hi, vec3(0.001)), 0.0, 1.0);
    vec3 k = -sqrt(a) * 0.2;
    FragColor = vec4(clamp((c + (n + e + w + s) * k) / (1.0 + 4.0 * k), 0.0, 1.0), 1.0);
}
