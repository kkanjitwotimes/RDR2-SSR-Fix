#version 450
// Zoom resample (bilinear, hardware) + contrast adaptive sharpening. CAS algorithm: AMD FidelityFX CAS (MIT licence).
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(push_constant) uniform PC {
  vec4 rect;      // source rectangle in UV units: xy = origin, zw = size
  vec2 dstInv;    // 1 / destination size in pixels
  float sharp;    // 0..1
  float pad;
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oCol;
vec4 tap(vec2 px) { return texture(uTex, pc.rect.xy + (vUv + px * pc.dstInv) * pc.rect.zw); }
void main() {
  vec4 e4 = tap(vec2(0.0));
  vec3 a = tap(vec2(-1.0, -1.0)).rgb, b = tap(vec2(0.0, -1.0)).rgb, c = tap(vec2(1.0, -1.0)).rgb;
  vec3 d = tap(vec2(-1.0, 0.0)).rgb,  e = e4.rgb,                  f = tap(vec2(1.0, 0.0)).rgb;
  vec3 g = tap(vec2(-1.0, 1.0)).rgb,  h = tap(vec2(0.0, 1.0)).rgb, i = tap(vec2(1.0, 1.0)).rgb;
  vec3 mn = min(min(min(d, e), min(f, b)), h);
  vec3 mn2 = min(mn, min(min(a, c), min(g, i)));
  mn += mn2;
  vec3 mx = max(max(max(d, e), max(f, b)), h);
  vec3 mx2 = max(mx, max(max(a, c), max(g, i)));
  mx += mx2;
  vec3 amp = clamp(min(mn, vec3(2.0) - mx) / max(mx, vec3(1e-4)), 0.0, 1.0);
  amp = sqrt(amp);
  float peak = -1.0 / mix(8.0, 5.0, clamp(pc.sharp, 0.0, 1.0));
  vec3 w = amp * peak;
  vec3 outc = clamp((b * w + d * w + f * w + h * w + e) / (vec3(1.0) + 4.0 * w), 0.0, 1.0);
  oCol = vec4(outc, e4.a);
}
