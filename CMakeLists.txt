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

    # Robustness tests: all effects, FFT sizes, sample rates, block sizes, reset, API misuse.
    add_executable(DtBlkFxRobustnessTests
        tests/DtBlkFxRobustnessTests.cpp
    )
    target_link_libraries(DtBlkFxRobustnessTests PRIVATE DtBlkFxEngine)
    add_test(NAME DtBlkFxRobustnessTests COMMAND DtBlkFxRobustnessTests)
    set_tests_properties(DtBlkFxRobustnessTests PROPERTIES TIMEOUT 900)

    # Parameter tests: hostile values, automation storms, extreme tempo, FFT and delay sweeps.
    add_executable(DtBlkFxParameterTests
        tests/DtBlkFxParameterTests.cpp
    )
    target_link_libraries(DtBlkFxParameterTests PRIVATE DtBlkFxEngine)
    add_test(NAME DtBlkFxParameterTests COMMAND DtBlkFxParameterTests)
    set_tests_properties(DtBlkFxParameterTests PROPERTIES TIMEOUT 900)

    # Host timing tests: tempo and beat-grid tracking, and block-sync alignment in the engine.
    add_executable(DtBlkFxHostTimingTests
        tests/DtBlkFxHostTimingTests.cpp
    )
    target_link_libraries(DtBlkFxHostTimingTests PRIVATE DtBlkFxEngine)
    add_test(NAME DtBlkFxHostTimingTests COMMAND DtBlkFxHostTimingTests)
    set_tests_properties(DtBlkFxHostTimingTests PROPERTIES TIMEOUT 600)

    # Fixed latency tests: the delay does not depend on the FFT size, and changing the FFT size
    # while playing does not move, double or drop audio.
    add_executable(DtBlkFxFixedLatencyTests
        tests/DtBlkFxFixedLatencyTests.cpp
    )
    target_link_libraries(DtBlkFxFixedLatencyTests PRIVATE DtBlkFxEngine)
    add_test(NAME DtBlkFxFixedLatencyTests COMMAND DtBlkFxFixedLatencyTests)
    set_tests_properties(DtBlkFxFixedLatencyTests PROPERTIES TIMEOUT 900)

    # Diagnostic (not a test, never run by ctest): measures the delay of the new engine and of
    # the legacy VST2 plugin at steady settings and while the FFT size changes.
    add_executable(DtBlkFxFftSwitchDiagnostic
        tests/DtBlkFxFftSwitchDiagnostic.cpp
    )
    target_link_libraries(DtBlkFxFftSwitchDiagnostic PRIVATE DtBlkFxEngine)
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

if(WIN32 AND VCPKG_TARGET_TRIPLET STREQUAL "x64-windows")
    foreach(plugin_target IN ITEMS DtBlkFx_VST3 DtBlkFx_CLAP)
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:FFTW3::fftw3f>"
                "$<TARGET_FILE_DIR:${plugin_target}>"
            COMMENT "Staging FFTW runtime for ${plugin_target}"
            VERBATIM
        )
    endforeach()
endif()

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
# JUCE-linked tests (they compile the real processor, unlike the engine tests)
#   - DtBlkFxProcessorTests: parameter mapping and end-to-end audio
#   - DtBlkFxStateTests:     state save/load, damaged state, parameter resolution, automation
#   - DtBlkFxHostTests:      host playhead (tempo, beat position), latency, tail, sample rates
# ==============================================================================

if(BUILD_TESTING)
    foreach(juce_test_name IN ITEMS DtBlkFxProcessorTests DtBlkFxStateTests DtBlkFxHostTests)
        juce_add_console_app(${juce_test_name}
            PRODUCT_NAME "${juce_test_name}"
        )

        # The processor is compiled again here so the tests do not depend on plugin packaging.
        target_sources(${juce_test_name} PRIVATE
            tests/${juce_test_name}.cpp
            src/DtBlkFxProcessor.cpp
            src/DtBlkFxEditor.cpp
        )

        target_include_directories(${juce_test_name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )

        # These come from juce_add_plugin for the real plugin; supply them for the test build.
        target_compile_definitions(${juce_test_name} PRIVATE
            "JucePlugin_Name=\"DtBlkFx\""
            JucePlugin_IsMidiEffect=0
            JucePlugin_IsSynth=0
            JucePlugin_WantsMidiInput=0
            JucePlugin_ProducesMidiOutput=0
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
        )

        target_link_libraries(${juce_test_name} PRIVATE
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
            DtBlkFxEngine
        )

        add_test(NAME ${juce_test_name} COMMAND ${juce_test_name})
        set_tests_properties(${juce_test_name} PROPERTIES TIMEOUT 600)
    endforeach()
endif()

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
if(BUILD_TESTING)
    message(STATUS "Tests:         Engine + robustness + parameter + host timing + fixed latency + processor + state + host")
else()
    message(STATUS "Tests:         Disabled (BUILD_TESTING is off)")
endif()
message(STATUS "========================================")
