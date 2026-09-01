#pragma once

#if defined(__APPLE__)

#include "AudioBackend.hpp"

#include <string>
#include <vector>

namespace margelo::nitro::ggwave {

/**
 * iOS capture and playback through one AudioUnit.
 *
 * Deliberately `kAudioUnitSubType_RemoteIO` and **not**
 * `kAudioUnitSubType_VoiceProcessingIO`: the voice processing unit applies echo
 * cancellation and gain control tuned for speech, which is precisely the
 * processing a 15 to 19.5 kHz tone detector needs to avoid. The session mode is
 * `AVAudioSessionModeMeasurement` for the same reason, documented in
 * AVAudioSessionTypes.h as minimising "the effect of system-supplied signal
 * processing for input and/or output audio signals".
 *
 * The implementation is Objective-C++ in `ios/CoreAudioBackend.mm`, because
 * `AVAudioSession` has no C API. Everything else about it is plain C++.
 */
class CoreAudioBackend final : public AudioBackend {
public:
  CoreAudioBackend() = default;
  ~CoreAudioBackend() override;

  void start(AudioSink& sink, int sampleRate, int framesPerCallback) override;
  void stop() override;

  int inputSampleRate() const override { return _sampleRate; }
  int outputSampleRate() const override { return _sampleRate; }
  const char* describe() const override { return _route.c_str(); }

  // ── Called from the audio thread by the C callbacks in the .mm
  void renderInto(float* output, int frames);
  void captureFrom(const float* input, int frames);

  AudioSink* sink() const { return _sink; }
  void* unit() const { return _unit; }
  std::vector<float>& captureScratch() { return _captureScratch; }

private:
  AudioSink* _sink = nullptr;
  void* _unit = nullptr; // AudioUnit, kept opaque so this header stays C++
  int _sampleRate = 48000;
  std::string _route = "not started";
  /// Sized once in `start`, so the audio thread never allocates.
  std::vector<float> _captureScratch;
};

} // namespace margelo::nitro::ggwave

#endif // __APPLE__
