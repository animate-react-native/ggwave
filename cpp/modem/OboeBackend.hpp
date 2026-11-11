#pragma once

#if defined(__ANDROID__)

#include "AudioBackend.hpp"

#include <oboe/Oboe.h>

#include <string>
#include <vector>

namespace margelo::nitro::ggwave {

/**
 * Android capture and playback through Oboe, which picks AAudio or OpenSL ES
 * per device.
 *
 * Two settings here are the whole point of owning the audio layer:
 *
 * - **`InputPreset::Unprocessed`.** Oboe's default is `VoiceRecognition`, chosen
 *   for latency, and it is a *processed* preset. A tone detector at 15 to
 *   19.5 kHz wants no processing at all. The attribute only takes effect on API
 *   28 and above; below that it is a request the system may ignore, which is why
 *   `describe()` reports what was actually granted.
 * - **`setFramesPerDataCallback`.** Oboe's own documentation gives block
 *   oriented FFT work as the reason this exists, so ggwave's frame size is asked
 *   for directly rather than reassembled from whatever the device offers.
 *
 * Oboe has no duplex stream, so this is two streams. The input stream drives the
 * clock, and the output stream is the one that can glitch, which is the right
 * way round: a dropped output frame corrupts one transmission, a dropped input
 * frame could lose a move.
 */
class OboeBackend final : public AudioBackend,
                          public oboe::AudioStreamDataCallback,
                          public oboe::AudioStreamErrorCallback {
public:
  OboeBackend() = default;
  ~OboeBackend() override;

  void start(AudioSink& sink, int sampleRate, int framesPerCallback) override;
  void stop() override;

  int inputSampleRate() const override { return _inputSampleRate; }
  int outputSampleRate() const override { return _outputSampleRate; }
  const char* describe() const override { return _route.c_str(); }

  // ── oboe::AudioStreamDataCallback, on the audio thread
  oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData,
                                        int32_t numFrames) override;

  // ── oboe::AudioStreamErrorCallback
  void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result result) override;

private:
  void openOutput(int sampleRate, int framesPerCallback);
  void openInput(int sampleRate, int framesPerCallback);

  AudioSink* _sink = nullptr;
  std::shared_ptr<oboe::AudioStream> _input;
  std::shared_ptr<oboe::AudioStream> _output;
  int _inputSampleRate = 48000;
  int _outputSampleRate = 48000;
  std::string _route = "not started";
};

} // namespace margelo::nitro::ggwave

#endif // __ANDROID__
