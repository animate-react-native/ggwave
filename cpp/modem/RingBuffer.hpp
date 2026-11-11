#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace margelo::nitro::ggwave {

/**
 * A single producer, single consumer float ring buffer.
 *
 * The producer is whatever thread called `send`, the consumer is the audio
 * thread. Neither locks and neither allocates: the storage is sized once, by
 * `reset`, before the audio stream is started.
 *
 * Capacity is one slot larger than requested so that full and empty stay
 * distinguishable without a count, which is what keeps both ends to a single
 * atomic each.
 */
class RingBuffer {
public:
  /// Sizes the buffer. Not safe to call while the audio thread is running.
  void reset(size_t capacity) {
    _storage.assign(capacity + 1, 0.0f);
    _write.store(0, std::memory_order_relaxed);
    _read.store(0, std::memory_order_relaxed);
  }

  /// Producer side. Returns how many samples were taken; short writes mean full.
  size_t write(const float* samples, size_t count) {
    if (_storage.empty()) return 0;
    const size_t size = _storage.size();
    const size_t write = _write.load(std::memory_order_relaxed);
    const size_t read = _read.load(std::memory_order_acquire);

    size_t written = 0;
    size_t cursor = write;
    while (written < count) {
      const size_t next = (cursor + 1) % size;
      if (next == read) break; // full
      _storage[cursor] = samples[written];
      cursor = next;
      written++;
    }
    _write.store(cursor, std::memory_order_release);
    return written;
  }

  /**
   * Consumer side, called from the audio thread. Always fills `count` samples,
   * padding with silence when the buffer runs dry, because an audio callback
   * must never leave its output buffer untouched.
   *
   * Returns how many of them were real.
   */
  size_t readOrSilence(float* out, size_t count) {
    const size_t size = _storage.size();
    if (size == 0) {
      for (size_t i = 0; i < count; i++) out[i] = 0.0f;
      return 0;
    }
    const size_t write = _write.load(std::memory_order_acquire);
    size_t cursor = _read.load(std::memory_order_relaxed);

    size_t read = 0;
    while (read < count && cursor != write) {
      out[read] = _storage[cursor];
      cursor = (cursor + 1) % size;
      read++;
    }
    _read.store(cursor, std::memory_order_release);

    for (size_t i = read; i < count; i++) out[i] = 0.0f;
    return read;
  }

  /// Consumer or producer side. Approximate by nature, which is all the
  /// transmitting flag needs.
  bool isEmpty() const {
    return _read.load(std::memory_order_acquire) == _write.load(std::memory_order_acquire);
  }

  size_t capacity() const { return _storage.empty() ? 0 : _storage.size() - 1; }

private:
  std::vector<float> _storage;
  std::atomic<size_t> _write{0};
  std::atomic<size_t> _read{0};
};

} // namespace margelo::nitro::ggwave
