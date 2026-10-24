// SPDX-License-Identifier: GPL-3.0-or-later
//
// See visualizer.h for where the audio comes from and what is drawn.
#include "visualizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL.h>

#include "RmlUi/RmlUi_Include_GL3.h"
#include "player.h"

namespace {

// --- Shaders ----------------------------------------------------------------

// One triangle covering the screen, no vertex buffer needed.
const char* kVertexShader = R"(#version 330
out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The scene. Coordinates: uv in 0..1 (y up), p = uv centred on x with the
// aspect ratio applied, so circles stay round.
const char* kFragmentShader = R"(#version 330
in vec2 v_uv;
out vec4 frag;

uniform sampler2D u_bands; // x: band (low..high), row 0 level, row 1 peak
uniform float u_time;
uniform float u_aspect;
uniform float u_energy;    // broadband level, 0..1
uniform float u_bass;      // low-band level, 0..1
uniform float u_beat;      // 1 on a beat, decaying
uniform float u_hue;       // spectral centroid, 0..1: teal .. violet
uniform float u_idle;      // 1 = no audio (paused, loading)

const float kHorizon = 0.30;
const vec3 kTeal   = vec3(0.30, 0.84, 0.72);   // the UI accent, #4dd6b8
const vec3 kViolet = vec3(0.49, 0.42, 0.94);   // #7c6cf0
const vec3 kNight  = vec3(0.020, 0.028, 0.060);

float hash(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x),
             mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}

float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 4; i++) {
    v += a * noise(p);
    p = p * 2.03 + vec2(1.7, 9.2);
    a *= 0.5;
  }
  return v;
}

float level(float x) { return texture(u_bands, vec2(x, 0.25)).r; }
float peak(float x)  { return texture(u_bands, vec2(x, 0.75)).r; }

// Height of the spectrum range above the horizon at screen x (0..1):
// mirrored, the bass at both edges framing the card, the treble meeting in
// the middle behind it.
float ridgeAt(float x, out float f) {
  f = (1.0 - clamp(abs(x - 0.5) * 2.0, 0.0, 1.0)) * 0.97 + 0.015;
  return kHorizon + 0.012 + 0.32 * level(f);
}

vec3 tint(float f) { return mix(kTeal, kViolet, clamp(f, 0.0, 1.0)); }

void main() {
  vec2 uv = v_uv;
  vec2 p = vec2((uv.x - 0.5) * u_aspect, uv.y);
  vec3 accent = mix(kTeal, kViolet, clamp(u_hue * 1.4 - 0.2, 0.0, 1.0));
  float live = 1.0 - u_idle;
  float t = u_time;

  float f;
  float ridge = ridgeAt(uv.x, f);
  vec3 col;

  if (uv.y >= kHorizon) {
    // --- Sky: gradient, aurora, stars --------------------------------------
    float h = (uv.y - kHorizon) / (1.0 - kHorizon);
    col = mix(kNight * 1.6 + accent * 0.035, kNight * 0.6, h);

    vec2 q = vec2(p.x * 0.9, h);
    float warp = fbm(q * 2.0 + vec2(t * 0.03, -t * 0.02));
    float curtain = fbm(vec2(q.x * 2.6 + warp * 2.2 + t * 0.05, q.y * 0.7 - t * 0.01));
    float shape = smoothstep(0.05, 0.45, h) * (1.0 - smoothstep(0.55, 1.0, h));
    float aurora = pow(curtain, 2.6) * shape * (0.55 + 1.6 * u_energy * live);
    col += mix(kTeal, kViolet, curtain * 1.3 - 0.2 + 0.2 * sin(t * 0.1 + p.x)) * aurora * 0.9;

    vec2 sp = p * 70.0;
    vec2 cell = floor(sp);
    float r = hash(cell);
    if (r > 0.975) {
      float d = length(fract(sp) - 0.5 - (vec2(hash(cell + 3.1), hash(cell + 7.7)) - 0.5) * 0.6);
      float tw = 0.55 + 0.45 * sin(t * (1.5 + r * 3.0) + r * 40.0);
      col += vec3(0.85, 0.9, 1.0) * smoothstep(0.12, 0.0, d) * tw * smoothstep(0.0, 0.35, h) * 0.8;
    }

    // Glow behind the now-playing card, breathing with the bass.
    float dg = length((p - vec2(0.0, 0.58)) * vec2(0.8, 1.0));
    col += accent * exp(-dg * dg * 5.5) * (0.10 + 0.32 * u_bass * live + 0.22 * u_beat);

    // --- The spectrum range on the horizon ------------------------------------
    if (uv.y < ridge) {
      // Lit from the top, with a faint bar structure so it reads as the
      // spectrum it is.
      float k = (uv.y - kHorizon) / max(ridge - kHorizon, 1e-3);
      float bars = 0.7 + 0.3 * smoothstep(0.08, 0.2, abs(fract(f * 32.0) - 0.5));
      vec3 body = mix(kNight * 2.0, tint(f) * (0.20 + 0.25 * level(f)), k * k) * bars;
      col = mix(col, body, 0.92);
    }
    float e = abs(uv.y - ridge);
    col += tint(f) * (0.0035 / (e + 0.0035)) * (0.35 + 0.9 * level(f) + 0.4 * u_beat);
    // Peak caps: a faint line where each band last peaked.
    float pk = kHorizon + 0.012 + 0.32 * peak(f);
    col += tint(f) * 0.5 * smoothstep(0.004, 0.0, abs(uv.y - pk - 0.006)) * live;
  } else {
    // --- Floor: a perspective grid rolling towards the viewer -------------------
    float dy = kHorizon - uv.y;
    float z = 0.09 / dy;
    float speed = 0.35 + 2.2 * u_energy * live;
    vec2 g = vec2(p.x * z * 1.6, z + t * speed);
    vec2 d = abs(fract(g - 0.5) - 0.5) / fwidth(g);
    float line = 1.0 - min(min(d.x, d.y), 1.0);
    float fade = exp(-z * 0.11) * smoothstep(0.0, 0.03, dy);
    col = kNight * 0.8;
    col += accent * line * fade * (0.45 + 0.9 * u_beat + 0.5 * u_energy * live);

    // The range, mirrored in the floor.
    float my = kHorizon + dy * 1.3;
    if (my < ridge)
      col += tint(f) * 0.16 * (1.0 - dy / kHorizon) * (0.5 + level(f));
    // Haze where floor meets sky.
    col += accent * exp(-dy * 22.0) * (0.22 + 0.35 * u_energy * live);
  }

  // Vignette, and a little grain against banding in the dark gradients.
  vec2 v = uv - 0.5;
  col *= 1.0 - 0.55 * dot(v, v);
  col += (hash(uv * 1000.0 + fract(t)) - 0.5) / 255.0;
  frag = vec4(col, 1.0);
}
)";

// --- Analysis constants (from the PS5 visualizer) ----------------------------

inline float Approach(float dt, float tau) { return 1.f - std::exp(-dt / tau); }

unsigned Compile(GLenum type, const char* source) {
  const GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &source, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    std::fprintf(stderr, "visualizer: shader: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

bool MakeTarget(GLuint& fbo, GLuint& tex, int w, int h, GLint filter) {
  if (!tex)
    glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (!fbo)
    glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  return ok;
}

} // namespace

Visualizer::Visualizer() = default;

Visualizer::~Visualizer() = default; // GL objects go in Shutdown()

void Visualizer::Shutdown() {
  if (program_) glDeleteProgram(program_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  GLuint fbos[] = {spec_fbo_, scene_fbo_};
  GLuint texs[] = {spec_tex_, scene_tex_, bands_tex_};
  for (GLuint f : fbos)
    if (f) glDeleteFramebuffers(1, &f);
  for (GLuint t : texs)
    if (t) glDeleteTextures(1, &t);
  program_ = vao_ = spec_fbo_ = spec_tex_ = scene_fbo_ = scene_tex_ = bands_tex_ = 0;
  scene_w_ = scene_h_ = 0;
}

void Visualizer::Reset() {
  std::fill(std::begin(band_), std::end(band_), 0.f);
  std::fill(std::begin(peak_), std::end(peak_), 0.f);
  energy_ = bass_ = bass_avg_ = beat_ = 0.f;
  idle_ = 1.f;
}

bool Visualizer::EnsureGl() {
  if (gl_failed_)
    return false;
  if (program_)
    return true;

  const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vs || !fs) {
    gl_failed_ = true;
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    std::fprintf(stderr, "visualizer: program did not link\n");
    gl_failed_ = true;
    return false;
  }
  u_bands_ = glGetUniformLocation(program_, "u_bands");
  u_time_ = glGetUniformLocation(program_, "u_time");
  u_aspect_ = glGetUniformLocation(program_, "u_aspect");
  u_energy_ = glGetUniformLocation(program_, "u_energy");
  u_bass_ = glGetUniformLocation(program_, "u_bass");
  u_beat_ = glGetUniformLocation(program_, "u_beat");
  u_hue_ = glGetUniformLocation(program_, "u_hue");
  u_idle_ = glGetUniformLocation(program_, "u_idle");

  glGenVertexArrays(1, &vao_);

  // Band levels as a texture: linear filtering interpolates between bands
  // for free, so the range is a smooth curve rather than steps.
  glGenTextures(1, &bands_tex_);
  glBindTexture(GL_TEXTURE_2D, bands_tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, kBands, 2, 0, GL_RED, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!MakeTarget(spec_fbo_, spec_tex_, Player::kSpectrumWidth,
                  Player::kSpectrumHeight, GL_NEAREST)) {
    std::fprintf(stderr, "visualizer: no offscreen framebuffer\n");
    gl_failed_ = true;
    return false;
  }
  pixels_.resize((size_t)Player::kSpectrumWidth * Player::kSpectrumHeight * 4);
  return true;
}

bool Visualizer::EnsureScene(int width, int height) {
  const int w = std::max(16, width / 2), h = std::max(16, height / 2);
  if (scene_fbo_ && w == scene_w_ && h == scene_h_)
    return true;
  if (!MakeTarget(scene_fbo_, scene_tex_, w, h, GL_LINEAR))
    return false;
  scene_w_ = w;
  scene_h_ = h;
  return true;
}

bool Visualizer::ReadSpectrum(Player& player, float* target) {
  // mpv draws the current spectrum frame: white bars on black, one column
  // per frequency (log spaced), height = level (dB, log scaled).
  glBindFramebuffer(GL_FRAMEBUFFER, spec_fbo_);
  glViewport(0, 0, Player::kSpectrumWidth, Player::kSpectrumHeight);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!player.RenderSpectrum(spec_fbo_)) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, spec_fbo_);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, Player::kSpectrumWidth, Player::kSpectrumHeight, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels_.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Column height = lit pixels in it, whichever way up mpv drew; two
  // columns to a band.
  constexpr int kPerBand = Player::kSpectrumWidth / kBands;
  bool any = false;
  for (int b = 0; b < kBands; b++) {
    int lit = 0;
    for (int c = 0; c < kPerBand; c++) {
      const int x = b * kPerBand + c;
      for (int y = 0; y < Player::kSpectrumHeight; y++)
        lit += pixels_[((size_t)y * Player::kSpectrumWidth + x) * 4] > 96;
    }
    // Bars span -120..0 dB; music lives in the top part of that. Map
    // -70..-10 dB onto 0..1, tilted up with frequency (+3 dB/octave from
    // the lower mids, as music falls off towards the treble and the top
    // of the range would otherwise never move).
    const float frac = (float)lit / (float)(kPerBand * Player::kSpectrumHeight);
    const float db = (frac - 1.f) * 120.f;
    const float octaves = 9.f * (float)b / kBands; // ~20 Hz .. 10 kHz
    const float tilt = std::min(14.f, std::max(0.f, 3.f * (octaves - 3.5f)));
    target[b] = std::min(1.f, std::max(0.f, (db + tilt + 70.f) / 60.f));
    any |= lit > 0;
  }
  return any;
}

void Visualizer::Analyze(const float* target, bool have, float dt, bool paused) {
  float idle[kBands];
  float smooth[kBands];
  if (have) {
    // A little blur across neighbouring bands: one noisy FFT bin should
    // not make a needle, the range is to read as a landscape.
    for (int b = 0; b < kBands; b++) {
      const float l = target[std::max(0, b - 1)], r = target[std::min(kBands - 1, b + 1)];
      smooth[b] = 0.25f * l + 0.5f * target[b] + 0.25f * r;
    }
    target = smooth;
  } else {
    // No audio: a slow travelling swell, so the scene breathes rather than
    // freezing (paused, loading, between tracks).
    for (int b = 0; b < kBands; b++) {
      const float t = (float)b / kBands;
      const float s = 0.5f + 0.5f * std::sin(time_ * 0.8f - t * 4.0f);
      idle[b] = (paused ? 0.06f : 0.10f) * s * (1.f - 0.5f * t);
    }
    target = idle;
  }
  idle_ += ((have ? 0.f : 1.f) - idle_) * Approach(dt, 0.4f);

  // Fast attack, slow release: transients read as hits, tails as glow.
  const float up = Approach(dt, 0.025f);
  const float down = Approach(dt, 0.22f);
  float sum = 0.f, weighted = 0.f;
  for (int b = 0; b < kBands; b++) {
    band_[b] += (target[b] - band_[b]) * (target[b] > band_[b] ? up : down);
    peak_[b] = band_[b] > peak_[b] ? band_[b] : std::max(band_[b], peak_[b] - dt * 0.35f);
    sum += band_[b];
    weighted += band_[b] * (float)b;
  }
  energy_ += (sum / kBands - energy_) * Approach(dt, 0.12f);
  if (sum > 0.0001f)
    centroid_ += ((weighted / sum) / kBands - centroid_) * Approach(dt, 0.6f);

  // Beat: bass jumping above its own running mean.
  float bass = 0.f;
  for (int b = 0; b < 6; b++)
    bass += band_[b];
  bass_ = bass / 6.f;
  since_beat_ += dt;
  if (have && bass_ > bass_avg_ * 1.35f + 0.04f && since_beat_ > 0.15f) {
    since_beat_ = 0.f;
    beat_ = 1.f;
  }
  bass_avg_ += (bass_ - bass_avg_) * Approach(dt, 0.4f);
  beat_ = std::max(0.f, beat_ - dt * 2.8f);
}

void Visualizer::Render(Player& player, int width, int height, bool paused) {
  if (width <= 0 || height <= 0 || !EnsureGl() || !EnsureScene(width, height))
    return;

  const uint64_t now = SDL_GetPerformanceCounter();
  float dt = 1.f / 60.f;
  if (last_tick_)
    dt = (float)((double)(now - last_tick_) / (double)SDL_GetPerformanceFrequency());
  last_tick_ = now;
  dt = std::min(std::max(dt, 0.f), 0.1f); // a stall must not teleport it
  time_ += dt;

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);

  float target[kBands] = {};
  const bool have = !paused && ReadSpectrum(player, target);
  Analyze(target, have, dt, paused);

  float rows[kBands * 2];
  std::copy(std::begin(band_), std::end(band_), rows);
  std::copy(std::begin(peak_), std::end(peak_), rows + kBands);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, bands_tex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBands, 2, GL_RED, GL_FLOAT, rows);

  // The scene, at half resolution...
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
  glViewport(0, 0, scene_w_, scene_h_);
  glUseProgram(program_);
  glUniform1i(u_bands_, 0);
  glUniform1f(u_time_, time_);
  glUniform1f(u_aspect_, (float)width / (float)height);
  glUniform1f(u_energy_, std::min(1.f, energy_ * 2.2f));
  glUniform1f(u_bass_, std::min(1.f, bass_ * 1.6f));
  glUniform1f(u_beat_, beat_);
  glUniform1f(u_hue_, centroid_);
  glUniform1f(u_idle_, idle_);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // ...scaled up onto the screen.
  glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo_);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, scene_w_, scene_h_, 0, 0, width, height,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);

  glBindVertexArray(0);
  glUseProgram(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width, height);
}
