#pragma once

#include "HybridGgwaveSpec.hpp"

#include "modem/Modem.hpp"
#include "vendor/ggwave/include/ggwave/ggwave.h"

#include <mutex>
#include <optional>
#include <vector>

namespace margelo::nitro::ggwave {

/**
 * Data over sound, in two layers over one ggwave codec.
 *
 * The codec layer (`encode`, `decode`, `samplesPerFrame`) needs no audio
 * hardware and holds its own ggwave instance. The modem layer (`start`, `stop`,
 * `send`) is `Modem`, which owns a second instance plus the platform's audio
 * backend, and calls back on its own worker thread. This class does nothing but
 * translate between that and JSI.
 *
 * Instances are cheap to talk to and expensive to hold: ggwave keeps them in a
 * private fixed size map (`GGWAVE_MAX_INSTANCES` is 4), so every one taken here
 * must be given back.
 */
class HybridGgwave : public HybridGgwaveSpec {
public:
  // Nitro's autolinking requires a default constructible class, so this takes
  // no arguments and creates no ggwave instance.
  HybridGgwave();
  ~HybridGgwave() override;

  // ── Properties
  std::optional<std::function<void(const std::shared_ptr<ArrayBuffer>&)>> getOnMessage() override;
  void setOnMessage(
      const std::optional<std::function<void(const std::shared_ptr<ArrayBuffer>&)>>& onMessage)
      override;
  bool getIsListening() override;
  std::vector<ProtocolInfo> getProtocols() override;
  double getMaxPayloadFixed() override;
  double getMaxPayloadVariable() override;
  double getMaxInstances() override;
  bool getAllowSelfReception() override;
  void setAllowSelfReception(bool allowSelfReception) override;
  std::string getRoute() override;
  double getSamplesPerFrame() override;

  // ── The modem
  void start(double protocolId, double payloadLength, bool variableLength,
             double soundMarkerThreshold) override;
  double durationMs(double protocolId, double payloadBytes, bool variableLength) override;
  void setFreqStart(double protocolId, double freqStartHz) override;
  void stop() override;
  void send(const std::shared_ptr<ArrayBuffer>& payload, double volume) override;

  // ── The codec
  std::shared_ptr<ArrayBuffer> encode(const std::shared_ptr<ArrayBuffer>& payload,
                                      double protocolId, double volume) override;
  std::optional<std::shared_ptr<ArrayBuffer>>
  decode(const std::shared_ptr<ArrayBuffer>& samples) override;

private:
  /// Returns a codec instance whose Rx is narrowed to `protocol`, rebuilding it
  /// if the last one was for a different protocol. Pass -1 to accept whatever
  /// instance exists, building an all protocols one if there is none.
  ///
  /// Narrowing is not an optimisation. With all twelve Rx protocols enabled, a
  /// 48 byte variable length payload does not decode at all: it is misattributed
  /// and dropped. `tests/host-roundtrip.cpp` covers this.
  ::ggwave_Instance codecInstance(int protocol);
  void freeCodecInstance();

  ::ggwave_Instance _codec = -1;
  /// Which protocol `_codec`'s Rx is narrowed to, or -1 for all of them.
  int _codecProtocol = -1;
  int _samplesPerFrame = 1024;

  std::optional<std::function<void(const std::shared_ptr<ArrayBuffer>&)>> _onMessage;
  Modem _modem;
  bool _allowSelfReception = false;

  /// Guards the codec instance. ggwave instances are not thread safe, and
  /// `encode` and `decode` are synchronous JSI calls that may arrive from more
  /// than one runtime.
  std::mutex _mutex;

  /// Guards `_onMessage` alone. It is written from JavaScript and read from the
  /// modem's worker thread, and a `std::function` read while it is being
  /// assigned is a torn read, which crashes rather than misbehaves.
  std::mutex _callbackMutex;
};

} // namespace margelo::nitro::ggwave
