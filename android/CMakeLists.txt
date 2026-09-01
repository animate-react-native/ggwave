project(NitroGgwave)
cmake_minimum_required(VERSION 3.9.0)

set (PACKAGE_NAME NitroGgwave)
set (CMAKE_VERBOSE_MAKEFILE ON)
set (CMAKE_CXX_STANDARD 20)

# Define C++ library and add all sources
add_library(${PACKAGE_NAME} SHARED
        src/main/cpp/cpp-adapter.cpp
        ../cpp/HybridGgwave.cpp
        # The modem: platform neutral, plus the Android backend.
        ../cpp/modem/Modem.cpp
        ../cpp/modem/makeAudioBackend.cpp
        ../cpp/modem/OboeBackend.cpp
        # Vendored ggwave. One translation unit, no external dependencies.
        # See ../cpp/vendor/ggwave/UPSTREAM.md.
        ../cpp/vendor/ggwave/src/ggwave.cpp
)

# Add Nitrogen specs :)
include(${CMAKE_SOURCE_DIR}/../nitrogen/generated/android/NitroGgwave+autolinking.cmake)

# Set up local includes
include_directories(
        "src/main/cpp"
        "../cpp"
        "../cpp/modem"
        # ggwave.cpp includes "ggwave/ggwave.h", which only resolves from here.
        "../cpp/vendor/ggwave/include"
)

find_library(LOG_LIB log)

# Oboe arrives as a prefab AAR from Google's Maven, so it is linked rather than
# compiled. `buildFeatures { prefab true }` in build.gradle is what exposes it
# here, and the template already sets that.
find_package(oboe REQUIRED CONFIG)

# Link all libraries together
target_link_libraries(
        ${PACKAGE_NAME}
        ${LOG_LIB}
        android                                   # <-- Android core
        oboe::oboe                                # <-- capture and playback
)
