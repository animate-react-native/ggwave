#include "makeAudioBackend.hpp"

#include <stdexcept>

#if defined(__ANDROID__)
#include "OboeBackend.hpp"
#elif defined(__APPLE__)
#include "CoreAudioBackend.hpp"
#endif

namespace margelo::nitro::ggwave {

std::unique_ptr<AudioBackend> makeAudioBackend() {
#if defined(__ANDROID__)
  return std::make_unique<OboeBackend>();
#elif defined(__APPLE__)
  return std::make_unique<CoreAudioBackend>();
#else
  // The host tests build the modem without a platform, and construct
  // LoopbackBackend directly rather than coming through here.
  throw std::runtime_error("No audio backend on this platform");
#endif
}

} // namespace margelo::nitro::ggwave
