#pragma once

#include "AudioBackend.hpp"

#include <memory>

namespace margelo::nitro::ggwave {

/// The platform's audio backend. Throws where there is not one.
std::unique_ptr<AudioBackend> makeAudioBackend();

} // namespace margelo::nitro::ggwave
