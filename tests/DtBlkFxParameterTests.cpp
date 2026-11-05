/*
 * See LICENSE.md for copyright and licensing information.
 *
 * This file is part of DtBlkFx.
 *
 * DtBlkFx is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DtBlkFx is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DtBlkFx.  If not, see <https://www.gnu.org/licenses/>.
 */

// Engine-level parameter tests: hostile values, automation storms, extreme tempo, FFT size and
// delay sweeps. These tests do not link JUCE.

#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using Params = std::array<float, 44>;

constexpr double pi = 3.14159265358979323846;

int failureCount = 0;

bool check(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << "  FAIL: " << message << '\n';
    ++failureCount;
  }
  return condition;
}

void info(const std::string& message)
{
  std::cout << "  INFO: " << message << '\n';
}

struct Rng {
  std::uint32_t state;
  explicit Rng(std::uint32_t seed)
      : state(seed)
  {
  }
  std::uint32_t nextBits()
  {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  float nextFloat() { return static_cast<float>(nextBits() >> 8) / 16777216.0f; }
};

// Legacy FFT length parameter that selects the given plan index.
float fftLenParam(int planIndex)
{
  return static_cast<float>((planIndex + 2) * 4) / 255.0f;
}

// Eight real effects: Filter, Shift, HarmShift, Contrast, Smear, Threshold, Clip, Vocode.
constexpr std::array<int, 8> baseTypes{0, 7, 11, 1, 2, 3, 4, 26};

Params baseParams(int plan, float overlap, float delay)
{
  Params p{};
  p[0] = 1.0f;
  p[1] = delay;
  p[2] = fftLenParam(plan);
  p[3] = overlap;
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = 0.15f + 0.05f * static_cast<float>(slot);
    p[offset + 1] = 0.70f + 0.03f * static_cast<float>(slot);
    p[offset + 2] = 0.6f;
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(baseTypes[slot]);
    p[offset + 4] = 0.55f;
  }
  return p;
}

std::vector<float> makeNoise(int count, std::uint32_t seed)
{
  std::vector<float> data(static_cast<size_t>(count));
  Rng rng(seed);
  for (auto& sample : data)
    sample = rng.nextFloat() - 0.5f;
  return data;
}

struct Run {
  bool processOk = true;
  int configFailures = 0;
  int firstConfigFailure = -1;
  bool finite = true;
  float peak = 0.0f;
};

// Process stereo noise in blocks. beforeBlock(position) is called before every block, may change
// parameters and returns whether the engine accepted them.
template <typename BeforeBlock>
Run runEngine(DtBlkFxEngine& engine, int count, int block, BeforeBlock&& beforeBlock)
{
  Run run;
  auto left = makeNoise(count, 11u);
  auto right = makeNoise(count, 22u);
  std::array<float*, 2> pointers{};

  for (int position = 0; position < count; position += block) {
    const auto size = std::min(block, count - position);
    if (!beforeBlock(position)) {
      ++run.configFailures;
      if (run.firstConfigFailure < 0)
        run.firstConfigFailure = position;
    }
    pointers[0] = left.data() + position;
    pointers[1] = right.data() + position;
    if (!engine.process(pointers.data(), 2, size)) {
      run.processOk = false;
      return run;
    }
  }

  for (const auto* channel : {&left, &right})
    for (const auto sample : *channel) {
      if (!std::isfinite(sample))
        run.finite = false;
      else
        run.peak = std::max(run.peak, std::abs(sample));
    }
  return run;
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

// Every one of the 44 legacy values, set to something hostile, must be sanitised.
void testHostileParameterValues()
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const std::array<float, 8> hostile{nan, inf, -inf, -1.0f, 2.0f, 1.0e30f, -1.0e30f, 1.0e-40f};
  const std::array<const char*, 8> names{"NaN", "+inf", "-inf", "-1", "2", "1e30", "-1e30", "1e-40"};
  constexpr int plan = 8; // FFT 1024

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, 44100.0), "prepare failed"))
    return;

  int runs = 0;
  for (size_t index = 0; index <= 44; ++index) { // 44 = all values hostile at once
    for (size_t h = 0; h < hostile.size(); ++h) {
      auto p = baseParams(plan, 0.3f, 0.0f);
      if (index < 44)
        p[index] = hostile[h];
      else
        for (auto& value : p)
          value = hostile[h];

      const auto label = std::string(index < 44 ? "parameter " + std::to_string(index) : "all parameters") +
                         " = " + names[h];
      engine.reset();
      check(engine.configureLegacyParameters(p, 22050.0, 0.0, false), label + ": parameters rejected");
      const auto run = runEngine(engine, 6000, 256, [](int) { return true; });
      check(run.processOk, label + ": processing failed");
      check(run.finite, label + ": non-finite output");
      ++runs;
    }
  }
  std::cout << "  " << runs << " runs\n";
}

// Parameters that change before every block, down to one sample, at the largest FFT size.
void testAutomationStorm()
{
  // Parameter changes before every block must always be accepted, even at the largest FFT size,
  // where a single frame spans tens of thousands of samples.
  const std::array<int, 4> blocks{64, 32, 16, 1};
  constexpr int plan = 33;
  constexpr int fft = 80640;
  constexpr int count = fft * 2 + 2000;

  for (const auto block : blocks) {
    DtBlkFxEngine engine;
    if (!check(engine.prepare(plan, 44100.0), "prepare failed"))
      return;

    Rng rng(777u);
    auto params = baseParams(plan, 0.0f, 0.0f);
    engine.reset();
    const auto run = runEngine(engine, count, block, [&](int position) {
      if (position > 0) {
        params[6] = 0.3f + 0.4f * rng.nextFloat(); // slot 1 amplitude
        params[8] = rng.nextFloat();               // slot 1 value
        params[9] = rng.nextFloat();               // slot 2 frequency A
        params[13] = rng.nextFloat();              // slot 2 value
      }
      return engine.configureLegacyParameters(params, 22050.0, 0.0, false);
    });

    const auto label = "automation every " + std::to_string(block) + " samples at FFT " +
                       std::to_string(fft);
    check(run.processOk, label + ": processing failed");
    check(run.finite, label + ": non-finite output");
    check(run.configFailures == 0,
          label + ": the engine rejected " + std::to_string(run.configFailures) +
              " parameter updates (first at sample " + std::to_string(run.firstConfigFailure) + ")");
  }
}

// Strange tempo and beat phase values, with block sync and parameter sync active.
void testExtremeTempoAndBeatPhase()
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const std::array<double, 9> tempos{0.0, -1.0, nan, inf, 1.0e-9, 1.0e-3, 1.0, 4000.0, 1.0e12};
  const std::array<double, 5> phases{0.0, -5000.0, 123456.0, 1.0e12, nan};
  constexpr int plan = 8;

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, 44100.0), "prepare failed"))
    return;

  int runs = 0;
  for (const auto tempo : tempos)
    for (const auto phase : phases)
      for (const auto sync : {false, true}) {
        auto p = baseParams(plan, 0.75f, 0.0627451f); // overlap above 0.5 = block sync
        const auto label = "samples per beat " + std::to_string(tempo) + ", phase " +
                           std::to_string(phase) + (sync ? ", parameter sync" : "");
        engine.reset();
        if (!check(engine.configureLegacyParameters(p, tempo, phase, sync), label + ": parameters rejected"))
          continue;
        const auto run = runEngine(engine, 8000, 100, [&](int position) {
          if (position > 0)
            p[6] = 0.3f + 0.4f * static_cast<float>((position / 100) % 2);
          return engine.configureLegacyParameters(p, tempo, phase, sync);
        });
        check(run.configFailures == 0, label + ": a parameter update was rejected");
        check(run.processOk, label + ": processing failed");
        check(run.finite, label + ": non-finite output");
        ++runs;
      }
  std::cout << "  " << runs << " runs\n";
}

// The FFT size changes during playback, and the delay moves through its whole range.
void testFftSizeAndDelaySweeps()
{
  DtBlkFxEngine engine;
  if (!check(engine.prepare(0, 44100.0), "prepare failed"))
    return;

  {
    auto p = baseParams(0, 0.3f, 0.02f);
    constexpr int step = 3000;
    constexpr int count = step * DtBlkFxEngine::fftSizeCount;
    engine.reset();
    const auto run = runEngine(engine, count, 128, [&](int position) {
      p[2] = fftLenParam((position / step) % DtBlkFxEngine::fftSizeCount);
      return engine.configureLegacyParameters(p, 22050.0, 0.0, false);
    });
    check(run.configFailures == 0, "FFT size sweep: a parameter update was rejected");
    check(run.processOk, "FFT size sweep: processing failed");
    check(run.finite, "FFT size sweep: non-finite output");
  }

  const std::array<std::pair<const char*, std::pair<float, float>>, 2> delayRanges{
      {{"beat-based delay sweep", {0.0f, 0.49f}}, {"millisecond delay sweep", {0.5f, 1.0f}}}};
  for (const auto& range : delayRanges) {
    auto p = baseParams(8, 0.3f, range.second.first);
    constexpr int count = 60000;
    engine.reset();
    const auto run = runEngine(engine, count, 128, [&](int position) {
      const auto fraction = static_cast<float>(position) / static_cast<float>(count);
      p[1] = range.second.first + (range.second.second - range.second.first) * fraction;
      return engine.configureLegacyParameters(p, 22050.0, 0.0, false);
    });
    const std::string label = range.first;
    check(run.configFailures == 0, label + ": a parameter update was rejected");
    check(run.processOk, label + ": processing failed");
    check(run.finite, label + ": non-finite output");
  }
}

// A moderate gain change on a steady tone must not produce a sudden jump.
void testNoClickOnGainChange()
{
  constexpr double rate = 44100.0;
  constexpr int plan = 8;
  constexpr int count = 1024 * 40;

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, rate), "prepare failed"))
    return;

  Params p{};
  p[0] = 1.0f;
  p[2] = fftLenParam(plan);
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = 0.0f;
    p[offset + 1] = 1.0f;
    p[offset + 2] = 0.6f; // 0 dB
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(slot == 0 ? 0 : 9); // Filter, then None
    p[offset + 4] = 0.5f;
  }

  std::vector<float> signal(static_cast<size_t>(count));
  for (int n = 0; n < count; ++n)
    signal[static_cast<size_t>(n)] = 0.5f * static_cast<float>(std::sin(2.0 * pi * 440.0 * n / rate));

  engine.reset();
  if (!check(engine.configureLegacyParameters(p, rate * 0.5, 0.0, false), "parameters rejected"))
    return;

  // From the middle on, the filter amplitude changes from 0.6 to 0.64 (about +4 dB).
  for (int position = 0; position < count; position += 128) {
    if (position >= count / 2)
      p[6] = 0.64f;
    check(engine.configureLegacyParameters(p, rate * 0.5, 0.0, false), "parameter update rejected");
    float* channel = signal.data() + position;
    if (!check(engine.process(&channel, 1, 128), "processing failed"))
      return;
  }

  const int start = 1023 + 100 + 64; // skip the startup latency and fade-in
  float maxStep = 0.0f;
  float maxStepBefore = 0.0f;
  for (int n = start + 1; n < count; ++n) {
    const auto step = std::abs(signal[static_cast<size_t>(n)] - signal[static_cast<size_t>(n - 1)]);
    maxStep = std::max(maxStep, step);
    if (n < count / 2)
      maxStepBefore = std::max(maxStepBefore, step);
  }
  info("largest sample-to-sample step: " + std::to_string(maxStepBefore) + " before the change, " +
       std::to_string(maxStep) + " overall");
  check(maxStep < 0.12f, "a gain change produced a jump of " + std::to_string(maxStep) +
                             " (steady tone: about " + std::to_string(maxStepBefore) + ")");
}

void runTest(const char* name, void (*test)())
{
  const auto failuresBefore = failureCount;
  test();
  std::cout << (failureCount == failuresBefore ? "[ PASS ] " : "[ FAIL ] ") << name << std::endl;
}

} // namespace

int main()
{
  runTest("hostile parameter values", testHostileParameterValues);
  runTest("automation storm", testAutomationStorm);
  runTest("extreme tempo and beat phase", testExtremeTempoAndBeatPhase);
  runTest("FFT size and delay sweeps", testFftSizeAndDelaySweeps);
  runTest("no click on a gain change", testNoClickOnGainChange);

  std::cout << (failureCount == 0 ? "All parameter tests passed" : "Parameter tests failed") << " ("
            << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}
