#include "Modem.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace margelo::nitro::ggwave {

Modem::~Modem() {
  stop();
}

void Modem::start(const Config& config, std::unique_ptr<AudioBackend> backend) {
  if (_isListening.load(std::memory_order_acquire)) {
    throw std::runtime_error("The modem is already listening. Call stop() first.");
  }
  // A variable length transmission may carry more, because ggwave puts sound
  // markers around it rather than relying on both ends agreeing the size.
  const int ceiling =
      config.variableLength ? GGWave::kMaxLengthVariable : GGWave::kMaxLengthFixed;
  if (config.payloadLength < 1 || config.payloadLength > ceiling) {
    throw std::runtime_error("payloadLength must be 1 to " + std::to_string(ceiling) +
                             " in this mode, got " + std::to_string(config.payloadLength));
  }
  if (config.soundMarkerThreshold < 0.0f || config.soundMarkerThreshold > 1.0f) {
    throw std::runtime_error("soundMarkerThreshold must be 0 to 1");
  }
  if (backend == nullptr) {
    throw std::runtime_error("The modem needs an audio backend");
  }

  _config = config;
  _backend = std::move(backend);

  // The streams first: their real rates are arguments to ggwave_init, and on a
  // phone they are routinely not the 48 kHz that was asked for.
  _backend->start(*this, kOperatingSampleRate, kSamplesPerFrame);

  // Only the protocol in use listens. Not a tuning choice: with all twelve
  // enabled a 48 byte payload is misattributed and never decodes at all.
  // Global, and read only by instances created after it, so it belongs here.
  for (int i = 0; i < GGWAVE_PROTOCOL_COUNT; i++) {
    ::ggwave_rxToggleProtocol(static_cast<::ggwave_ProtocolId>(i), i == config.protocol ? 1 : 0);
  }

  ::ggwave_Parameters parameters = ::ggwave_getDefaultParameters();
  // Negative means variable, and then payloadLength is only our buffer sizing.
  parameters.payloadLength = config.variableLength ? -1 : config.payloadLength;
  if (config.soundMarkerThreshold > 0.0f) {
    parameters.soundMarkerThreshold = config.soundMarkerThreshold;
  }
  parameters.sampleRate = static_cast<float>(kOperatingSampleRate);
  parameters.sampleRateInp = static_cast<float>(_backend->inputSampleRate());
  parameters.sampleRateOut = static_cast<float>(_backend->outputSampleRate());
  parameters.samplesPerFrame = kSamplesPerFrame;
  parameters.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32;
  parameters.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;

  // One instance cannot do both: see the comment on _rxInstance.
  parameters.operatingMode = GGWAVE_OPERATING_MODE_RX;
  _rxInstance = ::ggwave_init(parameters);
  parameters.operatingMode = GGWAVE_OPERATING_MODE_TX;
  _txInstance = ::ggwave_init(parameters);

  if (_rxInstance < 0 || _txInstance < 0) {
    freeInstance();
    _backend->stop();
    _backend.reset();
    throw std::runtime_error(
        "ggwave_init failed. The modem needs two of the " +
        std::to_string(GGWAVE_MAX_INSTANCES) +
        " ggwave instances that exist, so this usually means earlier ones were never freed.");
  }

  // Size the transmit path from what a payload of this exact length costs, so
  // send() never allocates and never has to refuse a queued message. Two
  // waveforms of headroom lets a second move be queued while the first plays.
  // Sized for the longest message allowed, so send() never allocates and never
  // has to refuse one it was told to expect.
  std::vector<uint8_t> probe(static_cast<size_t>(config.payloadLength), 0);
  const int waveformBytes = ::ggwave_encode(_txInstance, probe.data(), config.payloadLength,
                                            config.protocol, 25, nullptr, 1);
  if (waveformBytes <= 0) {
    freeInstance();
    _backend->stop();
    _backend.reset();
    throw std::runtime_error("ggwave_encode could not size a " +
                             std::to_string(config.payloadLength) +
                             " byte payload for this protocol");
  }
  _txWaveform.assign(static_cast<size_t>(waveformBytes), 0);

  // Room for two transmissions, so a move sent while another is going out queues
  // behind it instead of being refused. A third in the same window is refused
  // loudly, which is the honest answer.
  //
  // No silence is inserted between them. An earlier version did, on the theory
  // that two fixed length payloads running together were indistinguishable; the
  // real cause of the lost move was a shared ggwave instance, and once Rx and Tx
  // were separated the gap made no difference at any spacing from 0 to 853 ms.
  const size_t waveformSamples = static_cast<size_t>(waveformBytes) / sizeof(float);
  _tx.reset(waveformSamples * 2);

  // A transmission decodes repeatedly for as long as it sits in the analysis
  // window, so dedupe over roughly the length of one, plus a margin.
  _dedupeFrames = (waveformBytes / static_cast<int>(sizeof(float))) / kSamplesPerFrame + 4;
  _framesSincePublished = _dedupeFrames;
  _lastPublished.length = 0;
  _silentFramesAfterTx = 0;

  _queueWrite.store(0, std::memory_order_relaxed);
  _queueRead.store(0, std::memory_order_relaxed);

  _isListening.store(true, std::memory_order_release);
  _workerShouldRun.store(true, std::memory_order_release);
  _worker = std::thread([this] { runWorker(); });
}

void Modem::stop() {
  // The backend goes first, so no audio callback can be in flight while the
  // ggwave instance is being freed underneath it.
  if (_backend != nullptr) {
    _backend->stop();
  }

  _workerShouldRun.store(false, std::memory_order_release);
  if (_worker.joinable()) {
    _worker.join();
  }

  _isListening.store(false, std::memory_order_release);
  _isTransmitting.store(false, std::memory_order_release);
  freeInstance();
  _backend.reset();
}

void Modem::freeInstance() {
  for (auto* instance : {&_rxInstance, &_txInstance}) {
    if (*instance >= 0) {
      ::ggwave_free(*instance);
      *instance = -1;
    }
  }
}

void Modem::send(const uint8_t* payload, int length, int volume) {
  if (!_isListening.load(std::memory_order_acquire)) {
    throw std::runtime_error("The modem is not started, so there is nothing to send on");
  }
  if (_config.variableLength ? length < 1 || length > _config.payloadLength
                            : length != _config.payloadLength) {
    throw std::runtime_error(
        _config.variableLength
            ? "This modem was started for payloads up to " +
                  std::to_string(_config.payloadLength) + " bytes, got " + std::to_string(length)
            : "This modem was started for payloads of exactly " +
                  std::to_string(_config.payloadLength) + " bytes, got " + std::to_string(length));
  }

  // Encoded here rather than on the audio thread. Generating a whole waveform is
  // orders of magnitude too slow for an audio callback, so the audio thread only
  // ever copies floats out of the ring.
  const int written = ::ggwave_encode(_txInstance, payload, length, _config.protocol, volume,
                                      _txWaveform.data(), 0);
  if (written <= 0) {
    throw std::runtime_error("ggwave_encode failed (returned " + std::to_string(written) + ")");
  }

  const auto* samples = reinterpret_cast<const float*>(_txWaveform.data());
  const size_t count = static_cast<size_t>(written) / sizeof(float);

  _isTransmitting.store(true, std::memory_order_release);
  const size_t accepted = _tx.write(samples, count);
  if (accepted < count) {
    throw std::runtime_error("The transmit buffer is full: two messages are already going out");
  }
}

// ── The audio thread from here down. No allocation, no locks, no JS.

void Modem::render(float* output, int frames) {
  const size_t real = _tx.readOrSilence(output, static_cast<size_t>(frames));

  if (real > 0) {
    _isTransmitting.store(true, std::memory_order_release);
    // Stay deaf a little longer than we are loud: the tail of this chirp is still
    // travelling from the speaker to our own microphone.
    _silentFramesAfterTx = 4;
    return;
  }

  if (_silentFramesAfterTx > 0) {
    _silentFramesAfterTx--;
    if (_silentFramesAfterTx == 0) {
      _isTransmitting.store(false, std::memory_order_release);
    }
  }
}

void Modem::capture(const float* input, int frames) {
  if (_rxInstance < 0) return;

  // Our own transmission is not a message. ggwave refuses to decode while its
  // own tx queue has data, but this modem pre-encodes and plays the samples
  // itself, so that guard never fires and this one has to exist.
  if (_isTransmitting.load(std::memory_order_acquire) &&
      !_allowSelfReception.load(std::memory_order_acquire)) {
    return;
  }

  if (_framesSincePublished < _dedupeFrames) {
    _framesSincePublished++;
  }

  // Safe on this thread: ggwave allocates in init, and its logging is a null
  // pointer check once ggwave_setLogFile(nullptr) has been called.
  uint8_t payload[kMaxPayload];
  const int decoded =
      ::ggwave_ndecode(_rxInstance, input, frames * static_cast<int>(sizeof(float)), payload,
                       static_cast<int>(sizeof(payload)));
  if (decoded <= 0) {
    return;
  }

  const bool sameAsLast = _lastPublished.length == decoded &&
                          std::memcmp(_lastPublished.bytes, payload,
                                      static_cast<size_t>(decoded)) == 0;
  if (sameAsLast && _framesSincePublished < _dedupeFrames) {
    return; // the same transmission decoding again as it slides out of the window
  }

  std::memcpy(_lastPublished.bytes, payload, static_cast<size_t>(decoded));
  _lastPublished.length = decoded;
  _framesSincePublished = 0;

  publish(payload, decoded);
}

void Modem::publish(const uint8_t* payload, int length) {
  const int write = _queueWrite.load(std::memory_order_relaxed);
  const int next = (write + 1) % kQueueSlots;
  if (next == _queueRead.load(std::memory_order_acquire)) {
    return; // the queue is full, which means nobody is draining it. Drop rather than block.
  }

  Slot& slot = _queue[write];
  std::memcpy(slot.bytes, payload, static_cast<size_t>(length));
  slot.length = length;
  _queueWrite.store(next, std::memory_order_release);
}

// ── The worker thread

void Modem::runWorker() {
  while (_workerShouldRun.load(std::memory_order_acquire)) {
    int read = _queueRead.load(std::memory_order_relaxed);
    while (read != _queueWrite.load(std::memory_order_acquire)) {
      const Slot& slot = _queue[read];
      if (onPayload) {
        // Off the audio thread, so this may allocate and may take locks, which
        // is exactly what dispatching to a JS runtime does.
        onPayload(slot.bytes, slot.length);
      }
      read = (read + 1) % kQueueSlots;
      _queueRead.store(read, std::memory_order_release);
    }
    // Polling, because notifying a condition variable from the audio thread
    // would mean locking on it. 5 ms is nothing next to a 256 ms transmission.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

} // namespace margelo::nitro::ggwave
