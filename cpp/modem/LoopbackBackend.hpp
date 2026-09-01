#pragma once

#include "AudioBackend.hpp"

#include <cstdio>
#include <vector>

namespace margelo::nitro::ggwave {

/**
 * An audio backend with no audio in it. Playback is wired straight back into
 * capture, and the test drives the clock by calling `pump`.
 *
 * Deliberately synchronous and single threaded. A backend that spawned a thread
 * would make the suite timing dependent, and what is worth proving here is the
 * modem's logic, not the scheduler: whether a payload survives encode, the ring
 * buffer, decode and dedupe. Thread behaviour on a real audio callback cannot be
 * proven on a laptop anyway.
 *
 * `attenuation` and `noise` are here because a real room is neither silent nor
 * unity gain, and both platforms warn that an unprocessed input path is quiet.
 */
class LoopbackBackend final : public AudioBackend {
public:
  explicit LoopbackBackend(float attenuation = 1.0f, float noise = 0.0f)
      : _attenuation(attenuation), _noise(noise) {}

  void start(AudioSink& sink, int sampleRate, int framesPerCallback) override {
    _sink = &sink;
    _sampleRate = sampleRate;
    _frames = framesPerCallback;
    _buffer.assign(static_cast<size_t>(framesPerCallback), 0.0f);
  }

  void stop() override { _sink = nullptr; }

  int inputSampleRate() const override { return _sampleRate; }
  int outputSampleRate() const override { return _sampleRate; }
  const char* describe() const override { return "loopback"; }

  /// One audio callback: render a block, then hand that same block back.
  void pump() {
    if (_sink == nullptr) return;
    _sink->render(_buffer.data(), _frames);
    if (_attenuation != 1.0f || _noise != 0.0f) {
      for (int i = 0; i < _frames; i++) {
        _buffer[static_cast<size_t>(i)] =
            _buffer[static_cast<size_t>(i)] * _attenuation + nextNoise();
      }
    }
    _sink->capture(_buffer.data(), _frames);
  }

  void pump(int blocks) {
    for (int i = 0; i < blocks; i++) pump();
  }

private:
  /// A deterministic sign flipping trickle, so a failure is reproducible.
  float nextNoise() {
    if (_noise == 0.0f) return 0.0f;
    _seed = _seed * 1103515245u + 12345u;
    const float unit = static_cast<float>((_seed >> 16) & 0x7fff) / 16383.5f - 1.0f;
    return unit * _noise;
  }

  AudioSink* _sink = nullptr;
  int _sampleRate = 48000;
  int _frames = 1024;
  std::vector<float> _buffer;
  float _attenuation;
  float _noise;
  uint32_t _seed = 2463534242u;
};

} // namespace margelo::nitro::ggwave
