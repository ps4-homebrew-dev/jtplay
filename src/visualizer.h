// SPDX-License-Identifier: GPL-3.0-or-later
//
// The background for audio-only playback: an OpenGL scene that moves with
// the music.
//
// mpv has no PCM tap, so the audio reaches us as a picture: for audio-only
// files the player's lavfi graph renders a tiny white-on-black bar spectrum
// (Player::kSpectrumWidth x kSpectrumHeight) as the "video". mpv presents
// those frames against its audio clock, so whatever frame it hands over is
// the spectrum of what is audible right now. Each frame the visualizer has
// mpv render that frame into an offscreen framebuffer, reads the few KiB
// back, and turns bar heights into band levels.
//
// From there it is the PS5 visualizer's analysis (fast attack / slow
// release, peak caps, broadband energy, spectral centroid for the hue, a
// bass-over-its-mean beat detector) driving a full-screen shader instead of
// software-rendered geometry:
//
//   - a night sky with drifting aurora and twinkling stars
//   - a soft glow behind the now-playing card, pulsing with the bass
//   - the spectrum as a mirrored mountain range on the horizon (bass in the
//     middle, treble towards the edges), with its reflection
//   - a perspective grid floor rolling towards the viewer, faster when the
//     music is louder, flashing on beats
//
// The scene is drawn at half resolution and scaled up with linear
// filtering: it is all soft light, and the saved fill rate is what keeps it
// cheap on the console's GPU.
//
// Main thread only, with the window's GL context current.
#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <cstdint>
#include <vector>

class Player;

class Visualizer {
public:
  static constexpr int kBands = 64;

  Visualizer();
  ~Visualizer();

  // Draws the scene into framebuffer 0 at width x height, from the
  // player's current spectrum. 'paused' settles it into a slow idle swell.
  // Leaves GL with no program, VAO, texture or framebuffer of its own bound.
  void Render(Player& player, int width, int height, bool paused);

  // Drops the analysis state, e.g. when a new track starts.
  void Reset();

  // Releases the GL objects; call while the context is still current.
  void Shutdown();

private:
  bool EnsureGl();
  bool EnsureScene(int width, int height);
  // Reads the spectrum mpv rendered and fills 'target' (0..1 per band).
  bool ReadSpectrum(Player& player, float* target);
  void Analyze(const float* target, bool have, float dt, bool paused);

  // GL objects (0 = not created).
  unsigned program_ = 0;
  unsigned vao_ = 0;
  unsigned spec_fbo_ = 0, spec_tex_ = 0;    // mpv renders the spectrum here
  unsigned bands_tex_ = 0;                  // kBands x 2: levels, peaks
  unsigned scene_fbo_ = 0, scene_tex_ = 0;  // half-resolution scene
  int scene_w_ = 0, scene_h_ = 0;
  bool gl_failed_ = false;

  // Uniform locations.
  int u_time_ = -1, u_aspect_ = -1, u_energy_ = -1, u_bass_ = -1;
  int u_beat_ = -1, u_hue_ = -1, u_bands_ = -1, u_idle_ = -1;

  std::vector<uint8_t> pixels_;

  // Analysis.
  float band_[kBands] = {};
  float peak_[kBands] = {};
  float energy_ = 0.f;
  float bass_ = 0.f;
  float bass_avg_ = 0.f;
  float centroid_ = 0.3f;
  float beat_ = 0.f;
  float since_beat_ = 0.f;
  float idle_ = 1.f;          // 1 = no audio data, 0 = live
  float time_ = 0.f;
  uint64_t last_tick_ = 0;
};

#endif
