require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "NitroGgwave"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported, :visionos => 1.0 }
  s.source       = { :git => "https://github.com/animate-react-native/ggwave.git", :tag => "#{s.version}" }

  s.source_files = [
    # Implementation (Swift)
    "ios/**/*.{swift}",
    # Autolinking/Registration (Objective-C++)
    "ios/**/*.{m,mm}",
    # Implementation (C++ objects), including vendored ggwave. Its public API
    # is a .h, so the glob has to cover .h as well as .hpp.
    "cpp/**/*.{h,hpp,cpp}",
  ]

  # ggwave.cpp includes "ggwave/ggwave.h", which only resolves from here.
  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => [
      "\"$(PODS_TARGET_SRCROOT)/cpp\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/modem\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/vendor/ggwave/include\""
    ].join(" ")
  }

  load 'nitrogen/generated/ios/NitroGgwave+autolinking.rb'
  add_nitrogen_files(s)

  s.frameworks = "AudioToolbox", "AVFoundation"

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  install_modules_dependencies(s)
end
