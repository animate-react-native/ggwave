#pragma once

#include "AudioBackend.hpp"
#include "RingBuffer.hpp"

#include "../vendor/ggwave/include/ggwave/ggwave.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace margelo::nitro::ggwave {

/**
 * Data over sound, above whatever audio layer a platform provides.
 *
 * Owns the ggwave instance and both directions of the link. Nothing in here is
 * platform specific, and nothing in here calls into JavaScript: a completed
 * payload is handed to `onPayload` on the modem's own worker thread, and it is
 * the caller's job to marshal from there.
 *
 * Threading, which is the whole design:
 *
 * - **The audio thread** calls `render` and `capture`. Both are allocation free
 *   and lock free. `capture` calls `ggwave_ndecode` directly, which is safe:
 *   ggwave allocates everything in `ggwave_init` and its logging is compiled to
 *   a null pointer check.
 * - **The caller's thread** calls `send`, which encodes the whole waveform up
 *   front and hands the samples to a ring buffer. Encoding is far too heavy for
 *   an audio callback, and doing it here means the audio thread only ever
 *   copies floats.
 * - **The worker thread** polls for completed payloads and invokes `onPayload`.
 *   It polls rather than waiting on a condition variable because notifying one
 *   would mean taking a lock on the audio thread.
 */
class Modem final : public AudioSink {
public:
  /**
   * ggwave's operating rate and frame size.
   *
   * Public because they are the only sensible place to convert a protocol's FFT
   * bin index into Hz, and a second copy of these numbers elsewhere would be a
   * second thing to keep in step.
   */
  static constexpr int kOperatingSampleRate = 48000;
  static constexpr int kSamplesPerFrame = 1024;

  /** How the modem is set up. One struct, so adding a knob is not a new arity. */
  struct Config {
    ::ggwave_ProtocolId protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST;
    /**
     * Exactly how many bytes every message carries, or the largest one when
     * `variableLength` is set. The transmit buffers are sized from it.
     */
    int payloadLength = 3;
    /**
     * Whether shorter messages are allowed.
     *
     * Off by default and worth keeping off: a fixed length lets ggwave skip the
     * sound markers it otherwise needs to find a boundary, which is the
     * difference between 256 ms and 1067 ms for three bytes.
     */
    bool variableLength = false;
    /** 0 to 1, or 0 for ggwave's own default. */
    float soundMarkerThreshold = 0.0f;
  };

  Modem() = default;
  ~Modem() override;

  Modem(const Modem&) = delete;
  Modem& operator=(const Modem&) = delete;

  /// Invoked on the worker thread with a completed payload. Set before `start`.
  std::function<void(const uint8_t* payload, int length)> onPayload;

  /**
   * Opens the audio backend, then creates the ggwave instance from the rates the
   * backend actually got, in that order.
   *
   * @param protocol which ggwave protocol to transmit and to listen for
   * @param payloadLength fixed payload size in bytes, 1 to 64. Fixed rather than
   * variable because the sound markers a variable length payload needs cost more
   * airtime than a three byte payload does: 1067 ms against 256 ms.
   */
  void start(const Config& config, std::unique_ptr<AudioBackend> backend);

  /// Idempotent, and safe from the destructor.
  void stop();

  bool isListening() const { return _isListening.load(std::memory_order_acquire); }

  /// Copies the payload and queues its waveform. Never retains the pointer.
  void send(const uint8_t* payload, int length, int volume);

  /// True while the tail of a transmission is still going out.
  bool isTransmitting() const { return _isTransmitting.load(std::memory_order_acquire); }

  /**
   * Lets the modem decode its own transmission. Off by default, because in
   * normal use hearing your own chirp is how a phone plays its own move twice.
   *
   * It exists because step 3 of the ladder is one phone, speaker to its own
   * microphone, and that test is impossible while the gate is closed. The host
   * loopback suite uses it for the same reason. Not something an app should turn
   * on.
   */
  void setSelfReceptionAllowed(bool allowed) {
    _allowSelfReception.store(allowed, std::memory_order_release);
  }

  const char* describeRoute() const { return _backend ? _backend->describe() : "not started"; }

  // ── AudioSink, called on the audio thread only
  void render(float* output, int frames) override;
  void capture(const float* input, int frames) override;

private:
  static constexpr int kMaxPayload = 256; // GGWave::kMaxDataSize
  static constexpr int kQueueSlots = 8;   // completed payloads awaiting delivery

  struct Slot {
    uint8_t bytes[kMaxPayload];
    int length = 0;
  };

  void runWorker();
  void publish(const uint8_t* payload, int length);
  void freeInstance();

  std::unique_ptr<AudioBackend> _backend;
  /**
   * Two instances, not one, and this is not an optimisation.
   *
   * `ggwave_encode` mutates state the receive pipeline is using, so encoding a
   * second payload part way through receiving one destroys the reception:
   * measured, a 12 frame payload is lost when an encode lands on frames 4, 6 or
   * 8 of it. On one phone that meant a move sent while another was still going
   * out simply vanished, with no error anywhere.
   *
   * Rx only and Tx only also means ggwave allocates just the buffers each one
   * needs, per the note on `operatingMode` in ggwave.h. Two here plus the
   * codec's lazy one is three of the four instances ggwave allows.
   */
  ::ggwave_Instance _rxInstance = -1;
  ::ggwave_Instance _txInstance = -1;
  Config _config;

  RingBuffer _tx;
  /// Pre-sized in `start` so `send` never allocates on a hot path either.
  std::vector<uint8_t> _txWaveform;

  std::atomic<bool> _allowSelfReception{false};
  std::atomic<bool> _isListening{false};
  std::atomic<bool> _isTransmitting{false};
  /// Audio thread only. Keeps the gate closed for a moment after the ring drains,
  /// so the tail of our own chirp leaving the speaker is not decoded as a message.
  int _silentFramesAfterTx = 0;

  Slot _queue[kQueueSlots];
  std::atomic<int> _queueWrite{0};
  std::atomic<int> _queueRead{0};

  /// Audio thread only. One transmission decodes three to five times as it slides
  /// through the analysis window, so an identical payload inside the window is
  /// the same message arriving again, not a new one.
  Slot _lastPublished;
  int _framesSincePublished = 0;
  int _dedupeFrames = 0;

  std::thread _worker;
  std::atomic<bool> _workerShouldRun{false};
};

} // namespace margelo::nitro::ggwave
