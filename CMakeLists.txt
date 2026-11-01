# DtBlkFx
cmake_minimum_required(VERSION 3.19)

# Set the Vcpkg package manager as toolchain.
# The user is still able to set their own path or leave out vcpkg if desired.
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/modules/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "Vcpkg cmake toolchain file")

# The top-level CMakeLists.txt file for a project must contain a literal, direct call to the
# `project()` command. `project()` sets up some helpful variables that describe source/binary
# directories, and the current project version. This is a standard CMake command.
project(DTBLKFX VERSION 2.0.0)

# If you've installed JUCE somehow (via a package manager, or directly using the CMake install
# target), you'll need to tell this project that it depends on the installed copy of JUCE. If you've
# included JUCE directly in your source tree (perhaps as a submodule), you'll need to tell CMake to
# include that subdirectory as part of the build.

# find_package(JUCE CONFIG REQUIRED)        # If you've installed JUCE to your system
# or
add_subdirectory(modules/juce)

# Copy on build for development purposes
# set_directory_properties(PROPERTIES
#     JUCE_COPY_PLUGIN_AFTER_BUILD TRUE
#     JUCE_VST3_COPY_DIR "C:/Program Files/Common Files/VST3"
# )

# If you are building a VST2 or AAX plugin, CMake needs to be told where to find these SDKs on your
# system. This setup should be done before calling `juce_add_plugin`.

# juce_set_vst2_sdk_path(...)
# juce_set_aax_sdk_path(...)

# `juce_add_plugin` adds a static library target with the name passed as the first argument
# (AudioPluginExample here). This target is a normal CMake target, but has a lot of extra properties set
# up by default. As well as this shared code static library, this function adds targets for each of
# the formats specified by the FORMATS arguments. This function accepts many optional arguments.
# Check the readme at `docs/CMake API.md` in the JUCE repo for the full list.

juce_add_plugin(DtBlkFx
    PRODUCT_NAME "DtBlkFx"
    COMPANY_NAME "DtBlkFx"
    PLUGIN_MANUFACTURER_CODE DtBl
    PLUGIN_CODE DtBl
    FORMATS VST3 CLAP
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
)

# `juce_generate_juce_header` will create a JuceHeader.h for a given target, which will be generated
# into your build tree. This should be included with `#include <JuceHeader.h>`. The include path for
# this header will be automatically added to the target. The main function of the JuceHeader is to
# include all your JUCE module headers; if you're happy to include module headers directly, you
# probably don't need to call this.

# juce_generate_juce_header(AudioPluginExample)

find_package(FFTW3 CONFIG REQUIRED)

target_sources(DtBlkFx PRIVATE
    src/DtBlkFxEditor.cpp
    src/DtBlkFxProcessor.cpp
)

target_compile_definitions(DtBlkFx
    PUBLIC
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(DtBlkFx PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_warning_flags
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    FFTW3::fftw3
)

target_compile_features(DtBlkFx PRIVATE cxx_std_17)
