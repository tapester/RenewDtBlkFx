
# ==============================================================================
# RenewDtBlkFx
# Cross-platform JUCE audio plugin
#
# Formats:
#   - VST3 (JUCE native)
#   - CLAP (clap-juce-extensions)
#
# Platforms:
#   - Windows (MSVC)
#   - macOS (Apple Clang)
#
# Dependencies:
#   - JUCE (Git submodule)
#   - clap-juce-extensions (Git submodule)
#   - FFTW3 (vcpkg)
#
# Build system: CMake >= 3.22
# C++ standard: C++17
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

project(DTBLKFX
    VERSION 2.0.0
    DESCRIPTION "RenewDtBlkFx audio plugin"
    LANGUAGES C CXX
)

# ==============================================================================
# Build configuration
# ==============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position-independent code is required by some plugin build configurations.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ==============================================================================
# Dependency locations
# ==============================================================================

set(JUCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/juce"
)

set(CLAP_JUCE_EXTENSIONS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/clap-juce-extensions"
)

# Check that Git submodules have been initialized.

if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found. Initialize Git submodules:\n"
        "git submodule update --init --recursive"
    )
endif()

if(NOT EXISTS "${CLAP_JUCE_EXTENSIONS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "clap-juce-extensions not found.\n"
        "Add and initialize the CLAP JUCE Extensions submodule."
    )
endif()

# ==============================================================================
# JUCE
# ==============================================================================

add_subdirectory(
    "${JUCE_DIR}"
    "${CMAKE_BINARY_DIR}/JUCE"
)

# ==============================================================================
# CLAP JUCE Extensions
# ==============================================================================

add_subdirectory(
    "${CLAP_JUCE_EXTENSIONS_DIR}"
    "${CMAKE_BINARY_DIR}/clap-juce-extensions"
    EXCLUDE_FROM_ALL
)

# ==============================================================================
# External dependencies
# ==============================================================================

find_package(FFTW3f CONFIG REQUIRED)

# ==============================================================================
# Legacy DSP engine foundation
# ==============================================================================

add_library(DtBlkFxEngine STATIC
    src/DtBlkFxEngine.cpp
    src/DtBlkFxSweepTables.cpp
)

target_include_directories(DtBlkFxEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(DtBlkFxEngine PUBLIC cxx_std_17)
target_link_libraries(DtBlkFxEngine PUBLIC FFTW3::fftw3f)

include(CTest)
if(BUILD_TESTING)
    add_executable(DtBlkFxEngineTests
        tests/DtBlkFxEngineTests.cpp
    )
    target_link_libraries(DtBlkFxEngineTests PRIVATE DtBlkFxEngine)
    add_test(NAME DtBlkFxEngineTests COMMAND DtBlkFxEngineTests)
endif()

# ==============================================================================
# Plugin definition
# ==============================================================================

juce_add_plugin(DtBlkFx

    # Plugin identity
    COMPANY_NAME "DtBlkFx"
    PRODUCT_NAME "DtBlkFx"

    # Keep these codes stable once the plugin is released.
    PLUGIN_MANUFACTURER_CODE DtBl
    PLUGIN_CODE DtBl

    # Native JUCE plugin formats.
    # CLAP is added separately below.
    FORMATS VST3

    # Plugin characteristics
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE

    # VST3 category
    VST3_CATEGORIES Fx

    # Plugin configuration
    COPY_PLUGIN_AFTER_BUILD FALSE
)

# ==============================================================================
# CLAP plugin definition
# ==============================================================================

clap_juce_extensions_plugin(
    TARGET DtBlkFx

    # IMPORTANT:
    # Replace this with a permanent, globally unique reverse-domain ID
    # controlled by the plugin publisher.
    CLAP_ID "com.tapester.dtblkfx"
    CLAP_FEATURES audio-effect filter frequency-shifter glitch
)

# ==============================================================================
# Plugin source files
# ==============================================================================

target_sources(DtBlkFx PRIVATE

    src/DtBlkFxEditor.cpp
    src/DtBlkFxProcessor.cpp
)

# ==============================================================================
# JUCE compile definitions
# ==============================================================================

target_compile_definitions(DtBlkFx PUBLIC

    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# ==============================================================================
# Compiler and platform configuration
# ==============================================================================

target_compile_features(DtBlkFx PRIVATE cxx_std_17)

# JUCE recommended compiler settings.
target_link_libraries(DtBlkFx PRIVATE

    juce::juce_audio_utils

    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    juce::juce_recommended_lto_flags

    DtBlkFxEngine
)

# ==============================================================================
# Build information
# ==============================================================================

message(STATUS "========================================")
message(STATUS "RenewDtBlkFx configuration")
message(STATUS "========================================")
message(STATUS "Version:       ${PROJECT_VERSION}")
message(STATUS "C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "System:        ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor:     ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "VST3:          Enabled")
message(STATUS "CLAP:          Enabled")
message(STATUS "FFTW3:         Required")
message(STATUS "========================================")
