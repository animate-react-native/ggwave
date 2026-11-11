#include "HybridGgwave.hpp"

#include "modem/makeAudioBackend.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace margelo::nitro::ggwave {

// The ids in src/index.ts are the header's enum, so a shift in either would be
// silent: every protocol would move by six. Fail the build instead.
static_assert(GGWAVE_PROTOCOL_AUDIBLE_NORMAL == 0, "ggwave protocol ids shifted");
static_assert(GGWAVE_PROTOCOL_ULTRASOUND_FAST == 4, "ggwave protocol ids shifted");
static_assert(GGWAVE_PROTOCOL_MT_FASTEST == 11, "ggwave protocol ids shifted");

static ::ggwave_ProtocolId toProtocol(double protocolId) {
  const int id = static_cast<int>(protocolId);
  if (id < 0 || id >= GGWAVE_PROTOCOL_COUNT) {
    throw std::runtime_error("Unknown ggwave protocol id: " + std::to_string(id));
  }
  return static_cast<::ggwave_ProtocolId>(id);
}

// `HybridObject` is a *virtual* base of the generated spec, so the most derived
// class has to initialise it: the `HybridObject(TAG)` in HybridGgwaveSpec's own
// constructor is ignored when constructing a HybridGgwave. Leaving it off
// compiles cleanly and then fails at runtime with "Cannot default-construct
// HybridObject!" on the first createHybridObject call.
HybridGgwave::HybridGgwave() : HybridObject(TAG) {
  // ggwave printfs on every successful decode, which on the modem path would be
  // a printf on a real time audio thread 47 times a second.
  ::ggwave_setLogFile(nullptr);
}

HybridGgwave::~HybridGgwave() {
  stop();
  freeCodecInstance();
}

// ── Properties

std::optional<std::function<void(const std::shared_ptr<ArrayBuffer>&)>>
HybridGgwave::getOnMessage() {
  std::lock_guard lock(_callbackMutex);
  return _onMessage;
}

void HybridGgwave::setOnMessage(
    const std::optional<std::function<void(const std::shared_ptr<ArrayBuffer>&)>>& onMessage) {
  std::lock_guard lock(_callbackMutex);
  _onMessage = onMessage;
}

bool HybridGgwave::getIsListening() {
  return _modem.isListening();
}

bool HybridGgwave::getAllowSelfReception() {
  return _allowSelfReception;
}

void HybridGgwave::setAllowSelfReception(bool allowSelfReception) {
  _allowSelfReception = allowSelfReception;
  _modem.setSelfReceptionAllowed(allowSelfReception);
}

std::string HybridGgwave::getRoute() {
  return _modem.describeRoute();
}

/**
 * How many Hz one FFT bin covers.
 *
 * ggwave stores a protocol's carrier as a bin index, which means nothing on its
 * own: it is the sample rate divided by the frame size. At 48 kHz and 1024
 * samples that is 46.875 Hz, and a protocol spans 96 bins.
 */
static constexpr double kBinHz =
    static_cast<double>(Modem::kOperatingSampleRate) / Modem::kSamplesPerFrame;

/**
 * How many FFT bins a protocol occupies.
 *
 * Sixteen per data bit, and two bits per byte of a chunk: `ggwave.cpp:1675`
 * walks `bin = freqStart + 16*i` for `i` up to `2 * bytesPerTx`. That is 96 bins
 * for the audible and ultrasound protocols, and only 32 for the DT and MT ones,
 * which carry a byte per chunk rather than three. A flat 96 would overstate
 * their band threefold.
 */
static int binsUsedBy(const GGWave::Protocol& p) {
  return 16 * 2 * p.bytesPerTx;
}

std::vector<ProtocolInfo> HybridGgwave::getProtocols() {
  std::vector<ProtocolInfo> all;
  all.reserve(GGWAVE_PROTOCOL_COUNT);

  const auto& table = GGWave::Protocols::tx();
  for (int id = 0; id < GGWAVE_PROTOCOL_COUNT; id++) {
    const auto& p = table[id];
    // The custom slots have no name and are not usable without being defined.
    if (p.name == nullptr) continue;
    all.push_back(ProtocolInfo(
        static_cast<double>(id), std::string(p.name), p.freqStart * kBinHz,
        binsUsedBy(p) * kBinHz, static_cast<double>(p.framesPerTx),
        static_cast<double>(p.bytesPerTx), static_cast<double>(p.nTones()),
        static_cast<double>(p.txDuration_ms(Modem::kSamplesPerFrame, Modem::kOperatingSampleRate))));
  }
  return all;
}

double HybridGgwave::getMaxPayloadFixed() {
  return GGWave::kMaxLengthFixed;
}

double HybridGgwave::getMaxPayloadVariable() {
  return GGWave::kMaxLengthVariable;
}

double HybridGgwave::getMaxInstances() {
  return GGWAVE_MAX_INSTANCES;
}

double HybridGgwave::durationMs(double protocolId, double payloadBytes, bool variableLength) {
  const auto protocol = toProtocol(protocolId);
  const int length = static_cast<int>(payloadBytes);
  const int ceiling = variableLength ? GGWave::kMaxLengthVariable : GGWave::kMaxLengthFixed;
  if (length < 1 || length > ceiling) {
    throw std::runtime_error("A payload of " + std::to_string(length) +
                             " bytes is outside 1.." + std::to_string(ceiling));
  }

  // Its own instance, built and freed here. Reusing the codec's would rebuild it
  // for this protocol and silently change what the app is listening for, which
  // is far too much for a question about timing.
  ::ggwave_Parameters parameters = ::ggwave_getDefaultParameters();
  parameters.payloadLength = variableLength ? -1 : length;
  parameters.sampleRate = static_cast<float>(Modem::kOperatingSampleRate);
  parameters.sampleRateInp = static_cast<float>(Modem::kOperatingSampleRate);
  parameters.sampleRateOut = static_cast<float>(Modem::kOperatingSampleRate);
  parameters.samplesPerFrame = Modem::kSamplesPerFrame;
  parameters.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32;
  parameters.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;
  // Transmit only: nothing is being received, and it allocates less.
  parameters.operatingMode = GGWAVE_OPERATING_MODE_TX;

  const ::ggwave_Instance instance = ::ggwave_init(parameters);
  if (instance < 0) {
    throw std::runtime_error("ggwave_init failed: all " +
                             std::to_string(GGWAVE_MAX_INSTANCES) + " instances are in use");
  }

  const std::vector<uint8_t> probe(static_cast<size_t>(length), 0);
  // Query mode returns the size of the waveform in bytes without building it.
  const int bytes = ::ggwave_encode(instance, probe.data(), length, protocol, 25, nullptr, 1);
  ::ggwave_free(instance);

  if (bytes <= 0) {
    throw std::runtime_error("ggwave_encode could not size that payload for this protocol");
  }
  // Bytes of float samples, at the operating rate.
  return 1000.0 * (bytes / static_cast<double>(sizeof(float))) / Modem::kOperatingSampleRate;
}

void HybridGgwave::setFreqStart(double protocolId, double freqStartHz) {
  const auto protocol = toProtocol(protocolId);
  const int bin = static_cast<int>(freqStartHz / kBinHz + 0.5);
  // Above this the protocol's 96 bins run past the Nyquist limit.
  // The widest protocol needs 96 bins of room below Nyquist.
  const int highest = Modem::kSamplesPerFrame / 2 - 96;
  if (bin < 1 || bin > highest) {
    throw std::runtime_error("A carrier of " + std::to_string(static_cast<int>(freqStartHz)) +
                             " Hz does not leave room for the protocol's bins below Nyquist");
  }
  // Both directions, or the two phones would no longer agree where to listen.
  ::ggwave_rxProtocolSetFreqStart(protocol, bin);
  ::ggwave_txProtocolSetFreqStart(protocol, bin);
}

double HybridGgwave::getSamplesPerFrame() {
  std::lock_guard lock(_mutex);
  codecInstance(-1);
  return static_cast<double>(_samplesPerFrame);
}

// ── The modem

void HybridGgwave::start(double protocolId, double payloadLength, bool variableLength,
                         double soundMarkerThreshold) {
  const auto protocol = toProtocol(protocolId);

  // Set before starting, so a payload completing on the very first frames still
  // has somewhere to go. The modem calls this on its worker thread, never on the
  // audio thread, and Nitro dispatches a void callback to the JS thread from
  // there: a `void` return makes it an AsyncJSCallback, which is safe from any
  // thread. Were onMessage to return anything else it would be a synchronous
  // call straight into the runtime, and this would be a crash.
  _modem.onPayload = [this](const uint8_t* payload, int length) {
    // Copied out under the lock and called outside it, so JavaScript reassigning
    // onMessage cannot tear the function out from under this thread, and a slow
    // dispatch cannot block a reassignment.
    std::optional<std::function<void(const std::shared_ptr<ArrayBuffer>&)>> callback;
    {
      std::lock_guard lock(_callbackMutex);
      callback = _onMessage;
    }
    if (!callback.has_value()) return;

    // An owning buffer, because JavaScript outlives this call.
    auto buffer = ArrayBuffer::allocate(static_cast<size_t>(length));
    std::memcpy(buffer->data(), payload, static_cast<size_t>(length));
    (*callback)(buffer);
  };

  _modem.setSelfReceptionAllowed(_allowSelfReception);
  _modem.start({.protocol = protocol,
                .payloadLength = static_cast<int>(payloadLength),
                .variableLength = variableLength,
                .soundMarkerThreshold = static_cast<float>(soundMarkerThreshold)},
               makeAudioBackend());
}

void HybridGgwave::stop() {
  // Deliberately not a throw: the destructor calls this, and a destructor that
  // throws terminates the process.
  _modem.stop();
}

void HybridGgwave::send(const std::shared_ptr<ArrayBuffer>& payload, double volume) {
  if (payload == nullptr || payload->size() == 0) {
    throw std::runtime_error("Cannot send an empty payload");
  }
  // Copied inside Modem::send before it returns. This buffer arrives non owning
  // from JavaScript and the audio thread reads the samples later, so retaining
  // it would be a use after free the collector decides the timing of.
  _modem.send(payload->data(), static_cast<int>(payload->size()), static_cast<int>(volume));
}

// ── The codec

std::shared_ptr<ArrayBuffer> HybridGgwave::encode(const std::shared_ptr<ArrayBuffer>& payload,
                                                  double protocolId, double volume) {
  std::lock_guard lock(_mutex);
  const auto protocol = toProtocol(protocolId);
  const auto instance = codecInstance(static_cast<int>(protocol));

  if (payload == nullptr || payload->size() == 0) {
    throw std::runtime_error("Cannot encode an empty payload");
  }
  // kMaxDataSize (256) is the size of the decode output buffer, not the limit on
  // what may be transmitted. A variable length payload caps at 140 bytes.
  if (payload->size() > static_cast<size_t>(GGWave::kMaxLengthVariable)) {
    throw std::runtime_error("Payload is " + std::to_string(payload->size()) +
                             " bytes, which is over ggwave's variable length maximum of " +
                             std::to_string(GGWave::kMaxLengthVariable));
  }

  // Pass 1: ask how many bytes the waveform needs. The header is explicit that
  // this is a byte count, not a sample count.
  const int size = ::ggwave_encode(instance, payload->data(), static_cast<int>(payload->size()),
                                   protocol, static_cast<int>(volume), nullptr, 1);
  if (size <= 0) {
    throw std::runtime_error("ggwave_encode failed to size the waveform (returned " +
                             std::to_string(size) + ")");
  }

  // Pass 2: fill a buffer we own, because JavaScript is going to hold it.
  auto waveform = ArrayBuffer::allocate(static_cast<size_t>(size));
  const int written =
      ::ggwave_encode(instance, payload->data(), static_cast<int>(payload->size()), protocol,
                      static_cast<int>(volume), waveform->data(), 0);
  if (written <= 0) {
    throw std::runtime_error("ggwave_encode failed (returned " + std::to_string(written) + ")");
  }
  return waveform;
}

std::optional<std::shared_ptr<ArrayBuffer>>
HybridGgwave::decode(const std::shared_ptr<ArrayBuffer>& samples) {
  std::lock_guard lock(_mutex);
  // -1: decode alone cannot know the protocol, so it takes whatever instance the
  // last encode left narrowed, and only builds an unnarrowed one if there is
  // none. The modem path never relies on this, because start() names a protocol.
  const auto instance = codecInstance(-1);

  if (samples == nullptr || samples->size() == 0) {
    return std::nullopt;
  }

  // `samples` may be a non owning buffer from a JS Float32Array, valid only
  // until this returns. ggwave_ndecode copies what it keeps, and this function
  // is synchronous, so reading it here is safe. Never hand this pointer to
  // anything asynchronous.
  uint8_t payload[GGWave::kMaxDataSize];
  const int decoded = ::ggwave_ndecode(instance, samples->data(), static_cast<int>(samples->size()),
                                       payload, static_cast<int>(sizeof(payload)));
  if (decoded <= 0) {
    // -1 is a failed decode attempt, which is the normal case for noise, and 0
    // is "nothing complete yet". Neither is an error worth throwing over.
    return std::nullopt;
  }

  auto result = ArrayBuffer::allocate(static_cast<size_t>(decoded));
  std::memcpy(result->data(), payload, static_cast<size_t>(decoded));
  return result;
}

// ── Instance lifecycle

::ggwave_Instance HybridGgwave::codecInstance(int protocol) {
  if (_codec >= 0 && (protocol < 0 || protocol == _codecProtocol)) {
    return _codec;
  }
  if (_codec >= 0) {
    // A different protocol was asked for. Rx narrowing is only read by
    // ggwave_init, so the instance has to be rebuilt rather than adjusted.
    ::ggwave_free(_codec);
    _codec = -1;
  }

  // Global, and only read by instances created after it. So this has to happen
  // between the free above and the init below, every time.
  for (int i = 0; i < GGWAVE_PROTOCOL_COUNT; i++) {
    const int enabled = protocol < 0 || i == protocol ? 1 : 0;
    ::ggwave_rxToggleProtocol(static_cast<::ggwave_ProtocolId>(i), enabled);
  }

  ::ggwave_Parameters parameters = ::ggwave_getDefaultParameters();
  parameters.payloadLength = -1; // variable length, so any payload can be encoded
  parameters.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32;
  parameters.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;
  parameters.operatingMode = GGWAVE_OPERATING_MODE_RX_AND_TX;

  _codec = ::ggwave_init(parameters);
  if (_codec < 0) {
    throw std::runtime_error(
        "ggwave_init failed. There are at most " + std::to_string(GGWAVE_MAX_INSTANCES) +
        " ggwave instances, so this usually means an earlier one was never freed.");
  }
  _samplesPerFrame = parameters.samplesPerFrame;
  _codecProtocol = protocol;
  return _codec;
}

void HybridGgwave::freeCodecInstance() {
  std::lock_guard lock(_mutex);
  if (_codec >= 0) {
    ::ggwave_free(_codec);
    _codec = -1;
    _codecProtocol = -1;
  }
}

} // namespace margelo::nitro::ggwave
