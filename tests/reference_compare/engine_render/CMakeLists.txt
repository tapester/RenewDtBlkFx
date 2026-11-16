cmake_minimum_required(VERSION 3.20)
project(dtblkfx_reference_render LANGUAGES CXX)

# Standalone build of the headless renderer (no JUCE needed):
#   cmake -S tests/reference_compare/engine_render -B build-refrender \
#         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   cmake --build build-refrender --config Release
find_package(FFTW3f CONFIG REQUIRED)

set(SRC ${CMAKE_CURRENT_LIST_DIR}/../../../src)
add_executable(render_engine
    render_engine.cpp
    ${SRC}/DtBlkFxEngine.cpp
    ${SRC}/DtBlkFxSweepTables.cpp)
target_compile_features(render_engine PRIVATE cxx_std_17)
target_include_directories(render_engine PRIVATE ${SRC})
target_link_libraries(render_engine PRIVATE FFTW3::fftw3f)
