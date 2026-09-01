#import "../cpp/modem/CoreAudioBackend.hpp"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <stdexcept>
#include <string>

namespace margelo::nitro::ggwave {

namespace {

constexpr int kInputBus = 1;  // what comes from the microphone
constexpr int kOutputBus = 0; // what goes to the speaker

std::string describeStatus(const char* what, OSStatus status) {
  return std::string(what) + " failed with OSStatus " + std::to_string(status);
}

void require(OSStatus status, const char* what) {
  if (status != noErr) {
    throw std::runtime_error(describeStatus(what, status));
  }
}

AudioStreamBasicDescription monoFloatFormat(double sampleRate) {
  AudioStreamBasicDescription format = {};
  format.mSampleRate = sampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  format.mBitsPerChannel = 32;
  format.mChannelsPerFrame = 1;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(Float32);
  format.mBytesPerPacket = sizeof(Float32);
  return format;
}

/// Playback. Runs on the audio thread.
OSStatus renderCallback(void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                        UInt32 frames, AudioBufferList* data) {
  auto* backend = static_cast<CoreAudioBackend*>(refCon);
  if (data == nullptr || data->mNumberBuffers == 0) return noErr;

  auto* output = static_cast<float*>(data->mBuffers[0].mData);
  if (output == nullptr) return noErr;

  backend->renderInto(output, static_cast<int>(frames));
  return noErr;
}

/// Capture. Runs on the audio thread, and has to pull the samples itself.
OSStatus inputCallback(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time,
                       UInt32 bus, UInt32 frames, AudioBufferList*) {
  auto* backend = static_cast<CoreAudioBackend*>(refCon);
  auto* unit = static_cast<AudioUnit>(backend->unit());
  if (unit == nullptr) return noErr;

  std::vector<float>& scratch = backend->captureScratch();
  if (scratch.size() < frames) {
    // Never on a healthy stream: `start` sizes this well past the block size the
    // session was asked for. Dropping the block is the only real time safe
    // response, since growing the buffer here would allocate.
    return noErr;
  }

  AudioBufferList list = {};
  list.mNumberBuffers = 1;
  list.mBuffers[0].mNumberChannels = 1;
  list.mBuffers[0].mDataByteSize = frames * sizeof(float);
  list.mBuffers[0].mData = scratch.data();

  const OSStatus status = AudioUnitRender(unit, flags, time, bus, frames, &list);
  if (status != noErr) return status;

  backend->captureFrom(scratch.data(), static_cast<int>(frames));
  return noErr;
}

} // namespace

CoreAudioBackend::~CoreAudioBackend() {
  stop();
}

void CoreAudioBackend::start(AudioSink& sink, int sampleRate, int framesPerCallback) {
  _sink = &sink;

  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;

  // PlayAndRecord because both directions are needed at once. DefaultToSpeaker
  // because PlayAndRecord otherwise routes playback to the receiver, which is
  // useless for a phone lying on a table between two players. Measurement to
  // suppress as much system signal processing as iOS will allow.
  if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                       mode:AVAudioSessionModeMeasurement
                    options:AVAudioSessionCategoryOptionDefaultToSpeaker
                      error:&error]) {
    throw std::runtime_error(std::string("AVAudioSession setCategory failed: ") +
                             error.localizedDescription.UTF8String);
  }

  [session setPreferredSampleRate:static_cast<double>(sampleRate) error:nil];
  [session setPreferredIOBufferDuration:static_cast<double>(framesPerCallback) /
                                        static_cast<double>(sampleRate)
                                  error:nil];

  if (![session setActive:YES error:&error]) {
    // The usual cause is a missing NSMicrophoneUsageDescription, or the user
    // having declined the microphone.
    throw std::runtime_error(std::string("AVAudioSession setActive failed: ") +
                             error.localizedDescription.UTF8String);
  }

  // What the session actually got, which is not always what was asked for.
  _sampleRate = static_cast<int>(session.sampleRate);
  const int actualFrames =
      static_cast<int>(session.IOBufferDuration * session.sampleRate + 0.5);

  // Generous headroom: iOS can hand over a larger block than the buffer
  // duration implies, and the audio thread must never grow this.
  const size_t scratchFrames = static_cast<size_t>(actualFrames > 0 ? actualFrames : framesPerCallback) * 4;
  _captureScratch.assign(scratchFrames < 8192 ? 8192 : scratchFrames, 0.0f);

  AudioComponentDescription description = {};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_RemoteIO; // not VoiceProcessingIO
  description.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (component == nullptr) {
    throw std::runtime_error("No RemoteIO audio component on this device");
  }

  AudioUnit unit = nullptr;
  require(AudioComponentInstanceNew(component, &unit), "AudioComponentInstanceNew");
  _unit = unit;

  try {
    UInt32 enable = 1;
    require(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                                 kInputBus, &enable, sizeof(enable)),
            "EnableIO on the input bus");
    require(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                 kOutputBus, &enable, sizeof(enable)),
            "EnableIO on the output bus");

    AudioStreamBasicDescription format = monoFloatFormat(session.sampleRate);
    // Output scope of the input bus is what the unit hands us from the mic.
    require(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                 kInputBus, &format, sizeof(format)),
            "StreamFormat on the input bus");
    // Input scope of the output bus is what we hand the unit for the speaker.
    require(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                                 kOutputBus, &format, sizeof(format)),
            "StreamFormat on the output bus");

    AURenderCallbackStruct render = {};
    render.inputProc = renderCallback;
    render.inputProcRefCon = this;
    require(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                 kOutputBus, &render, sizeof(render)),
            "SetRenderCallback");

    AURenderCallbackStruct input = {};
    input.inputProc = inputCallback;
    input.inputProcRefCon = this;
    require(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                                 kAudioUnitScope_Global, 0, &input, sizeof(input)),
            "SetInputCallback");

    require(AudioUnitInitialize(unit), "AudioUnitInitialize");
    require(AudioOutputUnitStart(unit), "AudioOutputUnitStart");
  } catch (...) {
    AudioComponentInstanceDispose(unit);
    _unit = nullptr;
    throw;
  }

  _route = "RemoteIO, PlayAndRecord + Measurement, " + std::to_string(_sampleRate) + " Hz, " +
           std::to_string(actualFrames) + " frames";
}

void CoreAudioBackend::stop() {
  if (_unit != nullptr) {
    auto unit = static_cast<AudioUnit>(_unit);
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    _unit = nullptr;
  }
  _sink = nullptr;

  // Handing the session back matters: leaving it active on Measurement keeps
  // other audio on this phone quiet and the route pinned to the speaker.
  [[AVAudioSession sharedInstance] setActive:NO
                                 withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                       error:nil];
  _route = "not started";
}

void CoreAudioBackend::renderInto(float* output, int frames) {
  if (_sink != nullptr) {
    _sink->render(output, frames);
    return;
  }
  for (int i = 0; i < frames; i++) output[i] = 0.0f;
}

void CoreAudioBackend::captureFrom(const float* input, int frames) {
  if (_sink != nullptr) {
    _sink->capture(input, frames);
  }
}

} // namespace margelo::nitro::ggwave

#endif // __APPLE__
