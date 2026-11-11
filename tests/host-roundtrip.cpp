// Step 1 of the ladder in GGWAVE-NITRO-MODULE.md: off device, no React Native.
//
// Proves the vendored ggwave source compiles and round trips, and measures the
// two numbers the header does not tell you: how long a Chalk move takes on the
// air, and what the effective byte rate is after Reed Solomon.
//
// Build and run:
//   c++ -std=c++20 -O2 -I cpp/vendor/ggwave/include \
//       tests/host-roundtrip.cpp cpp/vendor/ggwave/src/ggwave.cpp -o /tmp/rt && /tmp/rt

#include "ggwave/ggwave.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string& what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) failures++;
}

struct Result {
  int waveformBytes = 0;
  double durationMs = 0;
  bool decoded = false;
};

// Encodes a payload, feeds the waveform back in one frame at a time, and
// reports what came out. Nothing here touches audio hardware.
static Result roundTrip(const std::vector<uint8_t>& payload, ggwave_ProtocolId protocol) {
  Result r;

  ggwave_Parameters p = ggwave_getDefaultParameters();
  p.payloadLength = (int) payload.size();  // fixed length, so no sound markers
  p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32;
  p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;
  p.operatingMode = GGWAVE_OPERATING_MODE_RX_AND_TX;

  // Only the protocol under test, which is what the module will do too: it cuts
  // false positives and it is the cheaper instance.
  for (int i = 0; i < GGWAVE_PROTOCOL_COUNT; i++) {
    ggwave_rxToggleProtocol((ggwave_ProtocolId) i, i == protocol ? 1 : 0);
  }

  ggwave_Instance inst = ggwave_init(p);
  if (inst < 0) {
    std::printf("  FAIL ggwave_init returned %d\n", inst);
    failures++;
    return r;
  }

  const int n = ggwave_encode(inst, payload.data(), (int) payload.size(), protocol, 25, nullptr, 1);
  if (n <= 0) {
    std::printf("  FAIL ggwave_encode query returned %d\n", n);
    failures++;
    ggwave_free(inst);
    return r;
  }
  r.waveformBytes = n;

  std::vector<uint8_t> waveform(n);
  const int written = ggwave_encode(inst, payload.data(), (int) payload.size(), protocol, 25,
                                   waveform.data(), 0);
  if (written <= 0) {
    std::printf("  FAIL ggwave_encode returned %d\n", written);
    failures++;
    ggwave_free(inst);
    return r;
  }

  const int samplesPerFrame = p.samplesPerFrame;
  const int frameBytes = samplesPerFrame * (int) sizeof(float);
  r.durationMs = 1000.0 * ((double) n / sizeof(float)) / p.sampleRate;

  uint8_t out[256];
  int decodedBytes = 0;
  for (int off = 0; off < n; off += frameBytes) {
    const int chunk = (n - off) < frameBytes ? (n - off) : frameBytes;
    const int ret = ggwave_ndecode(inst, waveform.data() + off, chunk, out, (int) sizeof(out));
    if (ret > 0) decodedBytes = ret;
  }
  // Flush: the last payload frames are only complete once more frames arrive.
  std::vector<float> silence(samplesPerFrame, 0.0f);
  for (int i = 0; i < 8 && decodedBytes == 0; i++) {
    const int ret = ggwave_ndecode(inst, silence.data(), frameBytes, out, (int) sizeof(out));
    if (ret > 0) decodedBytes = ret;
  }

  r.decoded = decodedBytes == (int) payload.size() &&
              std::memcmp(out, payload.data(), payload.size()) == 0;
  ggwave_free(inst);
  return r;
}

// The configuration the module itself uses: a variable payload length, so any
// payload can be encoded through one instance, and no Rx protocol narrowing.
// Different enough from roundTrip() above to be worth its own harness, because
// variable length is what puts ggwave's sound markers in the waveform.
static Result roundTripAsTheModuleDoes(const std::vector<uint8_t>& payload,
                                      ggwave_ProtocolId protocol, bool narrowRx = true) {
  Result r;

  ggwave_Parameters p = ggwave_getDefaultParameters();
  p.payloadLength = -1; // variable, so the waveform carries sound markers
  p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32;
  p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32;
  p.operatingMode = GGWAVE_OPERATING_MODE_RX_AND_TX;

  for (int i = 0; i < GGWAVE_PROTOCOL_COUNT; i++) {
    ggwave_rxToggleProtocol((ggwave_ProtocolId) i, narrowRx ? (i == protocol ? 1 : 0) : 1);
  }

  ggwave_Instance inst = ggwave_init(p);
  if (inst < 0) {
    std::printf("  FAIL ggwave_init returned %d\n", inst);
    failures++;
    return r;
  }

  const int n = ggwave_encode(inst, payload.data(), (int) payload.size(), protocol, 25, nullptr, 1);
  if (n <= 0) {
    std::printf("  FAIL ggwave_encode query returned %d\n", n);
    failures++;
    ggwave_free(inst);
    return r;
  }
  r.waveformBytes = n;
  r.durationMs = 1000.0 * ((double) n / sizeof(float)) / p.sampleRate;

  std::vector<uint8_t> waveform(n);
  ggwave_encode(inst, payload.data(), (int) payload.size(), protocol, 25, waveform.data(), 0);

  const int frameBytes = p.samplesPerFrame * (int) sizeof(float);
  uint8_t out[256];
  int decodedBytes = 0;
  for (int off = 0; off < n; off += frameBytes) {
    const int chunk = (n - off) < frameBytes ? (n - off) : frameBytes;
    const int ret = ggwave_ndecode(inst, waveform.data() + off, chunk, out, (int) sizeof(out));
    if (ret > 0) decodedBytes = ret;
  }
  std::vector<float> silence(p.samplesPerFrame, 0.0f);
  for (int i = 0; i < 16 && decodedBytes == 0; i++) {
    const int ret = ggwave_ndecode(inst, silence.data(), frameBytes, out, (int) sizeof(out));
    if (ret > 0) decodedBytes = ret;
  }

  r.decoded = decodedBytes == (int) payload.size() &&
              std::memcmp(out, payload.data(), payload.size()) == 0;
  ggwave_free(inst);
  return r;
}

int main() {
  // The ids in src/index.ts must match the header, or every protocol shifts.
  static_assert(GGWAVE_PROTOCOL_AUDIBLE_NORMAL == 0, "protocol ids shifted");
  static_assert(GGWAVE_PROTOCOL_ULTRASOUND_FAST == 4, "protocol ids shifted");
  static_assert(GGWAVE_PROTOCOL_MT_FASTEST == 11, "protocol ids shifted");

  // ggwave printfs on every successful decode. Silenced here so the results
  // are readable, and silenced in the module because on the modem path it
  // would be a printf on a real time audio thread.
  ggwave_setLogFile(nullptr);

  std::printf("ggwave host round trip\n\n");

  // A Chalk move: kind and move in byte 0, sequence in byte 1, CRC in byte 2.
  const std::vector<uint8_t> move = {0x2A, 0x01, 0xC3};

  struct Case { const char* name; ggwave_ProtocolId id; };
  const Case cases[] = {
      {"AUDIBLE_NORMAL", GGWAVE_PROTOCOL_AUDIBLE_NORMAL},
      {"AUDIBLE_FAST", GGWAVE_PROTOCOL_AUDIBLE_FAST},
      {"AUDIBLE_FASTEST", GGWAVE_PROTOCOL_AUDIBLE_FASTEST},
      {"ULTRASOUND_NORMAL", GGWAVE_PROTOCOL_ULTRASOUND_NORMAL},
      {"ULTRASOUND_FAST", GGWAVE_PROTOCOL_ULTRASOUND_FAST},
      {"ULTRASOUND_FASTEST", GGWAVE_PROTOCOL_ULTRASOUND_FASTEST},
  };

  std::printf("A three byte move, per protocol:\n");
  std::printf("  %-20s %10s %10s %12s\n", "protocol", "bytes", "duration", "effective");
  for (const auto& c : cases) {
    const Result r = roundTrip(move, c.id);
    const double bytesPerSec = r.durationMs > 0 ? 1000.0 * move.size() / r.durationMs : 0;
    std::printf("  %-20s %10d %8.0f ms %8.1f B/s%s\n", c.name, r.waveformBytes, r.durationMs,
                bytesPerSec, r.decoded ? "" : "   <- DID NOT DECODE");
    if (!r.decoded) failures++;
  }

  std::printf("\nA 48 byte resync at ULTRASOUND_FAST:\n");
  std::vector<uint8_t> resync(48);
  for (size_t i = 0; i < resync.size(); i++) resync[i] = (uint8_t) (i * 7 + 1);
  const Result big = roundTrip(resync, GGWAVE_PROTOCOL_ULTRASOUND_FAST);
  std::printf("  %d bytes, %.0f ms, %.1f B/s effective\n", big.waveformBytes, big.durationMs,
              big.durationMs > 0 ? 1000.0 * resync.size() / big.durationMs : 0);
  check(big.decoded, "48 byte payload round trips");

  std::printf("\nEdge cases:\n");
  const Result one = roundTrip({0xFF}, GGWAVE_PROTOCOL_ULTRASOUND_FAST);
  check(one.decoded, "single byte round trips");
  const Result maxed = roundTrip(std::vector<uint8_t>(64, 0xA5), GGWAVE_PROTOCOL_ULTRASOUND_FAST);
  check(maxed.decoded, "64 identical bytes round trip");

  std::printf("\nAs the module configures it, variable payload length:\n");
  for (const auto& c : cases) {
    const Result r = roundTripAsTheModuleDoes(move, c.id);
    std::printf("  %-20s %10d %8.0f ms   %s\n", c.name, r.waveformBytes, r.durationMs,
                r.decoded ? "ok" : "DID NOT DECODE");
    if (!r.decoded) failures++;
  }
  const Result varBig = roundTripAsTheModuleDoes(resync, GGWAVE_PROTOCOL_ULTRASOUND_FAST);
  std::printf("  %-20s %10d %8.0f ms   %s\n", "48 bytes [U] Fast", varBig.waveformBytes,
              varBig.durationMs, varBig.decoded ? "ok" : "DID NOT DECODE");
  if (!varBig.decoded) failures++;

  // Why the module narrows Rx to one protocol before creating an instance.
  // With all twelve enabled, a 48 byte variable length payload is misattributed
  // and lost, while a 3 byte one survives. If a ggwave upgrade ever fixes this,
  // this check is what will say so.
  std::printf("\nWith every Rx protocol enabled instead:\n");
  const Result wideBig =
      roundTripAsTheModuleDoes(resync, GGWAVE_PROTOCOL_ULTRASOUND_FAST, false);
  check(!wideBig.decoded, "48 bytes still does not decode, which is why Rx is narrowed");
  const Result wideSmall = roundTripAsTheModuleDoes(move, GGWAVE_PROTOCOL_ULTRASOUND_FAST, false);
  check(wideSmall.decoded, "3 bytes decodes even unnarrowed");

  // A fixed payload length is what the modem will use, and it is four times
  // faster for a move: the sound markers a variable length payload needs cost
  // more than the payload does.
  std::printf("\nFixed against variable, three bytes at ULTRASOUND_FAST:\n");
  const Result fixed = roundTrip(move, GGWAVE_PROTOCOL_ULTRASOUND_FAST);
  const Result variable = roundTripAsTheModuleDoes(move, GGWAVE_PROTOCOL_ULTRASOUND_FAST);
  std::printf("  fixed %.0f ms, variable %.0f ms, a %.1fx difference\n", fixed.durationMs,
              variable.durationMs, variable.durationMs / fixed.durationMs);
  check(fixed.durationMs * 2 < variable.durationMs, "fixed length is much faster");

  std::printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
