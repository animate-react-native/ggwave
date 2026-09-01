#include "OboeBackend.hpp"

#if defined(__ANDROID__)

#include <android/log.h>

#include <stdexcept>
#include <string>

namespace margelo::nitro::ggwave {

namespace {
constexpr const char* kTag = "NitroGgwave";
}

OboeBackend::~OboeBackend() {
  stop();
}

void OboeBackend::start(AudioSink& sink, int sampleRate, int framesPerCallback) {
  _sink = &sink;
  try {
    // Output first: the input callback drives the clock and will start pulling
    // from the modem immediately, so the speaker path has to exist by then.
    openOutput(sampleRate, framesPerCallback);
    openInput(sampleRate, framesPerCallback);
  } catch (...) {
    stop();
    throw;
  }

  const auto preset = _input->getInputPreset();
  _route = std::string("Oboe ") +
           (_input->getAudioApi() == oboe::AudioApi::AAudio ? "AAudio" : "OpenSL ES") + ", input " +
           std::to_string(_inputSampleRate) + " Hz, output " + std::to_string(_outputSampleRate) +
           " Hz, preset " +
           (preset == oboe::InputPreset::Unprocessed ? "Unprocessed"
                                                     : "processed (Unprocessed was refused)");
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s", _route.c_str());
}

void OboeBackend::openOutput(int sampleRate, int framesPerCallback) {
  oboe::AudioStreamBuilder builder;
  oboe::Result result =
      builder.setDirection(oboe::Direction::Output)
          ->setSharingMode(oboe::SharingMode::Exclusive)
          ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
          ->setFormat(oboe::AudioFormat::Float)
          ->setChannelCount(oboe::ChannelCount::Mono)
          ->setSampleRate(sampleRate)
          // Resampling belongs to ggwave, which is told the real rate, so Oboe
          // is asked not to interpose its own converter.
          ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::None)
          ->setUsage(oboe::Usage::Media)
          ->setErrorCallback(this)
          ->openStream(_output);
  if (result != oboe::Result::OK) {
    throw std::runtime_error(std::string("Could not open the output stream: ") +
                             oboe::convertToText(result));
  }
  _outputSampleRate = _output->getSampleRate();

  result = _output->requestStart();
  if (result != oboe::Result::OK) {
    throw std::runtime_error(std::string("Could not start the output stream: ") +
                             oboe::convertToText(result));
  }
}

void OboeBackend::openInput(int sampleRate, int framesPerCallback) {
  oboe::AudioStreamBuilder builder;
  oboe::Result result =
      builder.setDirection(oboe::Direction::Input)
          ->setSharingMode(oboe::SharingMode::Exclusive)
          ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
          ->setFormat(oboe::AudioFormat::Float)
          ->setChannelCount(oboe::ChannelCount::Mono)
          ->setSampleRate(sampleRate)
          ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::None)
          // The reason this class exists. Oboe defaults to VoiceRecognition,
          // which is processed.
          ->setInputPreset(oboe::InputPreset::Unprocessed)
          // Oboe's docs name block oriented FFT work as the reason for this.
          ->setFramesPerDataCallback(framesPerCallback)
          ->setDataCallback(this)
          ->setErrorCallback(this)
          ->openStream(_input);
  if (result != oboe::Result::OK) {
    throw std::runtime_error(std::string("Could not open the input stream: ") +
                             oboe::convertToText(result) +
                             ". A missing RECORD_AUDIO permission is the usual cause.");
  }
  _inputSampleRate = _input->getSampleRate();

  result = _input->requestStart();
  if (result != oboe::Result::OK) {
    throw std::runtime_error(std::string("Could not start the input stream: ") +
                             oboe::convertToText(result));
  }
}

void OboeBackend::stop() {
  // Input first, so no capture callback can run while the output is closing.
  for (auto* stream : {&_input, &_output}) {
    if (*stream != nullptr) {
      (*stream)->requestStop();
      (*stream)->close();
      stream->reset();
    }
  }
  _sink = nullptr;
  _route = "not started";
}

oboe::DataCallbackResult OboeBackend::onAudioReady(oboe::AudioStream* stream, void* audioData,
                                                   int32_t numFrames) {
  auto* samples = static_cast<float*>(audioData);
  if (_sink == nullptr || samples == nullptr) {
    return oboe::DataCallbackResult::Continue;
  }

  // Only the input stream has a data callback. It drives both directions: the
  // captured block goes to the modem, and the same number of frames is pulled
  // for playback and written to the output stream without blocking.
  if (stream->getDirection() == oboe::Direction::Input) {
    if (_output != nullptr) {
      // A zero timeout makes this a non blocking write, which is what an audio
      // callback requires. A short write means the speaker is behind, and the
      // dropped tail costs one transmission rather than the whole stream.
      float scratch[2048];
      const int32_t frames = numFrames > 2048 ? 2048 : numFrames;
      _sink->render(scratch, frames);
      _output->write(scratch, frames, 0);
    }
    _sink->capture(samples, numFrames);
  }

  return oboe::DataCallbackResult::Continue;
}

void OboeBackend::onErrorAfterClose(oboe::AudioStream*, oboe::Result result) {
  // Disconnects happen: headphones in, a call arriving, Bluetooth taking the
  // route. Recovery is the caller's decision, so this reports and stops rather
  // than reopening underneath them.
  __android_log_print(ANDROID_LOG_WARN, kTag, "Audio stream closed: %s",
                      oboe::convertToText(result));
  _route = std::string("disconnected: ") + oboe::convertToText(result);
}

} // namespace margelo::nitro::ggwave

#endif // __ANDROID__
