#pragma once

namespace margelo::nitro::ggwave {

/**
 * What the modem needs from a platform's audio layer, and nothing more.
 *
 * The implementations are `CoreAudioBackend` (iOS, AudioUnit RemoteIO) and
 * `OboeBackend` (Android), plus `LoopbackBackend` for the tests. Keeping this
 * interface to three methods is what lets the modem's logic be proven on a
 * laptop with no audio hardware in the loop.
 */
class AudioSink {
public:
  virtual ~AudioSink() = default;

  /**
   * Called on the audio thread to fill `frames` mono samples of playback.
   * Must always write every sample, silence included.
   */
  virtual void render(float* output, int frames) = 0;

  /**
   * Called on the audio thread with `frames` mono captured samples.
   * Must not allocate, lock, or call into JavaScript.
   */
  virtual void capture(const float* input, int frames) = 0;
};

class AudioBackend {
public:
  virtual ~AudioBackend() = default;

  /**
   * Opens capture and playback and begins calling the sink.
   *
   * `sampleRate` and `framesPerCallback` are requests, not guarantees: a device
   * may open at a different rate or hand over a different block size, so the
   * accessors below report what actually happened and the modem configures
   * ggwave from those rather than from what it asked for.
   *
   * Throws `std::runtime_error` with a platform message on failure.
   */
  virtual void start(AudioSink& sink, int sampleRate, int framesPerCallback) = 0;

  /// Idempotent. Must be safe to call from a destructor.
  virtual void stop() = 0;

  virtual int inputSampleRate() const = 0;
  virtual int outputSampleRate() const = 0;

  /// A human readable description of the route, for logs and for the tests.
  virtual const char* describe() const = 0;
};

} // namespace margelo::nitro::ggwave
