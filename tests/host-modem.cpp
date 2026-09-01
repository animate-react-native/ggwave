// The modem's logic, off device, through a loopback backend.
//
// Everything except the platform audio layer is exercised here: encode, the
// ring buffer, the self reception gate, decode on the "audio thread", dedupe,
// and delivery on the worker thread. What this cannot prove is real audio
// hardware, which is ladder steps 3 and 4.
//
// Build and run: bun run test:modem

#include "../cpp/modem/LoopbackBackend.hpp"
#include "../cpp/modem/Modem.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace margelo::nitro::ggwave;

static int failures = 0;

static void check(bool ok, const std::string& what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) failures++;
}

/// Collects what the worker thread delivers.
struct Received {
  std::mutex mutex;
  std::vector<std::vector<uint8_t>> payloads;

  void add(const uint8_t* bytes, int length) {
    std::lock_guard lock(mutex);
    payloads.emplace_back(bytes, bytes + length);
  }

  size_t count() {
    std::lock_guard lock(mutex);
    return payloads.size();
  }

  std::vector<uint8_t> first() {
    std::lock_guard lock(mutex);
    return payloads.empty() ? std::vector<uint8_t>{} : payloads.front();
  }
};

/// The worker polls every 5 ms, so give it room to drain before asserting.
static void letWorkerCatchUp() {
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

int main() {
  ::ggwave_setLogFile(nullptr);
  std::printf("ggwave modem, loopback\n\n");

  const std::vector<uint8_t> move = {0x2a, 0x01, 0xc3};

  // ── 1. A move survives the whole path
  {
    std::printf("A three byte move through the loopback:\n");
    Received received;
    Modem modem;
    modem.onPayload = [&](const uint8_t* b, int n) { received.add(b, n); };

    auto backend = std::make_unique<LoopbackBackend>();
    LoopbackBackend* loopback = backend.get();
    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::move(backend));
    // Step 3 of the ladder in software: a phone hearing itself.
    modem.setSelfReceptionAllowed(true);

    check(modem.isListening(), "isListening after start");
    check(std::string(modem.describeRoute()) == "loopback", "route is the loopback");

    modem.send(move.data(), 3, 50);
    check(modem.isTransmitting(), "isTransmitting once a payload is queued");

    loopback->pump(40); // 40 blocks of 1024 is ~850 ms, well past a 256 ms chirp
    letWorkerCatchUp();

    check(received.count() >= 1, "the payload arrived");
    check(received.first() == move, "the bytes are the ones that were sent");
    check(received.count() == 1, "delivered exactly once, so dedupe held");
    check(!modem.isTransmitting(), "isTransmitting clears once the ring drains");

    modem.stop();
    check(!modem.isListening(), "isListening after stop");
  }

  // ── 2. The gate: by default a phone must not hear itself
  {
    std::printf("\nWith the self reception gate closed, which is the default:\n");
    Received received;
    Modem modem;
    modem.onPayload = [&](const uint8_t* b, int n) { received.add(b, n); };

    auto backend = std::make_unique<LoopbackBackend>();
    LoopbackBackend* loopback = backend.get();
    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::move(backend));

    modem.send(move.data(), 3, 50);
    loopback->pump(40);
    letWorkerCatchUp();

    check(received.count() == 0, "nothing arrived, so the gate blocks our own chirp");
    modem.stop();
  }

  // ── 3. Two moves in a row, each delivered once
  {
    std::printf("\nTwo moves in a row:\n");
    Received received;
    Modem modem;
    modem.onPayload = [&](const uint8_t* b, int n) { received.add(b, n); };

    auto backend = std::make_unique<LoopbackBackend>();
    LoopbackBackend* loopback = backend.get();
    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::move(backend));
    modem.setSelfReceptionAllowed(true);

    const std::vector<uint8_t> second = {0x2a, 0x02, 0x77};
    modem.send(move.data(), 3, 50);
    loopback->pump(40);
    letWorkerCatchUp();
    modem.send(second.data(), 3, 50);
    loopback->pump(40);
    letWorkerCatchUp();

    check(received.count() == 2, "two payloads, not one and not five");
    {
      std::lock_guard lock(received.mutex);
      const bool ordered = received.payloads.size() == 2 && received.payloads[0] == move &&
                           received.payloads[1] == second;
      check(ordered, "in the order they were sent");
    }
    modem.stop();
  }

  // ── 3b. Two moves closer together than one transmission.
  //
  // Reproduces a real device log: a move sent 184 ms after another, while the
  // first was still going out at 256 ms, queued behind it in the ring and was
  // never heard. Only the second decoded. No error was raised, which makes a
  // lost move indistinguishable from a delivered one.
  {
    std::printf("\nA second move queued while the first is still transmitting:\n");
    Received received;
    Modem modem;
    modem.onPayload = [&](const uint8_t* b, int n) { received.add(b, n); };

    auto backend = std::make_unique<LoopbackBackend>();
    LoopbackBackend* loopback = backend.get();
    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::move(backend));
    modem.setSelfReceptionAllowed(true);

    const std::vector<uint8_t> first = {0x2a, 0x06, 0x2c};
    const std::vector<uint8_t> second = {0x2a, 0x07, 0x2d};

    modem.send(first.data(), 3, 50);
    loopback->pump(8); // ~170 ms, so the first is still going out
    modem.send(second.data(), 3, 50);
    loopback->pump(60);
    letWorkerCatchUp();

    check(received.count() == 2, "both moves arrive, neither is swallowed");
    {
      std::lock_guard lock(received.mutex);
      const bool bothPresent =
          received.payloads.size() == 2 && received.payloads[0] == first &&
          received.payloads[1] == second;
      check(bothPresent, "in order, and the first is not the one that is lost");
    }
    modem.stop();
  }

  // ── 4. A quiet, noisy room, which is what an unprocessed input path gives
  {
    std::printf("\nAt a quarter of the volume with noise, since both platforms\n"
                "warn that an unprocessed path is quiet:\n");
    Received received;
    Modem modem;
    modem.onPayload = [&](const uint8_t* b, int n) { received.add(b, n); };

    auto backend = std::make_unique<LoopbackBackend>(0.25f, 0.01f);
    LoopbackBackend* loopback = backend.get();
    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::move(backend));
    modem.setSelfReceptionAllowed(true);

    modem.send(move.data(), 3, 50);
    loopback->pump(40);
    letWorkerCatchUp();

    check(received.first() == move, "still decodes at 0.25 gain with noise");
    modem.stop();
  }

  // ── 4b. Variable length, which trades airtime for a bigger ceiling and for
  // messages that need not all be the same size.
  {
    std::printf("\nVariable length, up to %d bytes rather than %d:\n", GGWave::kMaxLengthVariable,
                GGWave::kMaxLengthFixed);
    Received received;
    Modem modem;
    modem.onPayload = [&](const uint8_t* b, int n) { received.add(b, n); };

    auto backend = std::make_unique<LoopbackBackend>();
    LoopbackBackend* loopback = backend.get();
    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST,
                 .payloadLength = 80,
                 .variableLength = true},
                std::move(backend));
    modem.setSelfReceptionAllowed(true);

    // Over the fixed ceiling, which is the whole point of the mode.
    std::vector<uint8_t> long_(80);
    for (size_t i = 0; i < long_.size(); i++) long_[i] = (uint8_t) (i * 3 + 5);
    modem.send(long_.data(), (int) long_.size(), 50);
    loopback->pump(400);
    letWorkerCatchUp();
    check(received.count() == 1 && received.payloads.front() == long_,
          "80 bytes, which a fixed length modem could not carry at all");

    // And a shorter one on the same modem, which fixed length forbids.
    received.payloads.clear();
    const std::vector<uint8_t> shortOne = {0x01, 0x02};
    modem.send(shortOne.data(), 2, 50);
    loopback->pump(200);
    letWorkerCatchUp();
    check(received.count() == 1 && received.payloads.front() == shortOne,
          "and two bytes on the same modem, which a fixed length one would refuse");

    try {
      std::vector<uint8_t> tooLong(81);
      modem.send(tooLong.data(), 81, 50);
      check(false, "over the declared maximum is refused");
    } catch (const std::exception&) {
      check(true, "over the declared maximum is refused");
    }
    modem.stop();
  }

  // ── 5. The refusals
  {
    std::printf("\nRefusals:\n");
    Modem modem;
    try {
      modem.send(move.data(), 3, 50);
      check(false, "send before start throws");
    } catch (const std::exception&) {
      check(true, "send before start throws");
    }

    try {
      modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 0}, std::make_unique<LoopbackBackend>());
      check(false, "a zero payload length is refused");
    } catch (const std::exception&) {
      check(true, "a zero payload length is refused");
    }

    try {
      modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 65},
                  std::make_unique<LoopbackBackend>());
      check(false, "a payload length over kMaxLengthFixed is refused");
    } catch (const std::exception&) {
      check(true, "a payload length over kMaxLengthFixed is refused");
    }
    try {
      modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST,
                   .payloadLength = GGWave::kMaxLengthVariable + 1,
                   .variableLength = true},
                  std::make_unique<LoopbackBackend>());
      check(false, "and over kMaxLengthVariable even in variable mode");
    } catch (const std::exception&) {
      check(true, "and over kMaxLengthVariable even in variable mode");
    }
    try {
      modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST,
                   .payloadLength = 3,
                   .soundMarkerThreshold = 2.0f},
                  std::make_unique<LoopbackBackend>());
      check(false, "a sound marker threshold outside 0 to 1 is refused");
    } catch (const std::exception&) {
      check(true, "a sound marker threshold outside 0 to 1 is refused");
    }

    modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::make_unique<LoopbackBackend>());
    try {
      modem.send(move.data(), 2, 50);
      check(false, "a payload of the wrong length is refused");
    } catch (const std::exception&) {
      check(true, "a payload of the wrong length is refused");
    }
    try {
      modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::make_unique<LoopbackBackend>());
      check(false, "starting twice is refused");
    } catch (const std::exception&) {
      check(true, "starting twice is refused");
    }
    modem.stop();
    modem.stop(); // idempotent
    check(true, "stop twice does not crash");
  }

  // ── 6. Instances are actually returned. ggwave allows only four, ever.
  {
    std::printf("\nInstance lifetime, since ggwave allows only %d:\n", GGWAVE_MAX_INSTANCES);
    bool allStarted = true;
    for (int i = 0; i < GGWAVE_MAX_INSTANCES + 2; i++) {
      Modem modem;
      try {
        modem.start({.protocol = GGWAVE_PROTOCOL_ULTRASOUND_FAST, .payloadLength = 3}, std::make_unique<LoopbackBackend>());
      } catch (const std::exception& e) {
        std::printf("      start %d failed: %s\n", i + 1, e.what());
        allStarted = false;
        break;
      }
      modem.stop();
    }
    check(allStarted, "start and stop six times over, so nothing leaks");
  }

  std::printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
