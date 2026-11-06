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

// Tests for the fixed latency budget: the delay between input and output must not depend on the
// FFT size, and changing the FFT size while playing must not move, double or drop audio.
// These tests do not link JUCE.

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

constexpr double kRate = 44100.0;
constexpr double kSamplesPerBeat = kRate * 0.5;
constexpr int kBlock = 256;
constexpr int kSpacing = 2000;
constexpr int kFloorDelay = 100; // the engine never delays by less than this

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

float fftLenParam(int planIndex)
{
  return static_cast<float>((planIndex + 2) * 4) / 255.0f;
}

float delayParamMs(double ms)
{
  return static_cast<float>(0.5 + ms / 6000.0 * 0.5);
}

Params makeParams(int plan, float delayParam, int effectType = 9)
{
  Params p{};
  p[0] = 1.0f; // fully wet, no power match
  p[1] = delayParam;
  p[2] = fftLenParam(plan);
  p[3] = 0.0f;
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = 0.0f;
    p[offset + 1] = 1.0f;
    p[offset + 2] = 0.6f;
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(slot == 0 ? effectType : 9);
    p[offset + 4] = 0.5f;
  }
  return p;
}

struct Click {
  int position;
  float height;
};

std::vector<Click> findClicks(const std::vector<float>& signal)
{
  std::vector<Click> clicks;
  const auto total = static_cast<int>(signal.size());
  for (int i = 1; i + 1 < total; ++i) {
    const auto value = std::abs(signal[static_cast<size_t>(i)]);
    if (value > 0.1f && value >= std::abs(signal[static_cast<size_t>(i - 1)]) &&
        value > std::abs(signal[static_cast<size_t>(i + 1)]))
      clicks.push_back({i, value});
  }
  return clicks;
}

template <typename PlanFor, typename LatencyFor>
std::vector<float> run(DtBlkFxEngine& engine,
                       int total,
                       const std::vector<int>& clickPositions,
                       const PlanFor& planFor,
                       float delayParam,
                       const LatencyFor& latencyFor,
                       int startPlan = 8)
{
  std::vector<float> left(static_cast<size_t>(total), 0.0f);
  std::vector<float> right(static_cast<size_t>(total), 0.0f);
  for (const auto position : clickPositions) {
    left[static_cast<size_t>(position)] = 0.5f;
    right[static_cast<size_t>(position)] = 0.5f;
  }

  if (!engine.prepare(startPlan, kRate))
    return {};
  engine.setFixedLatency(latencyFor(0));
  engine.reset();
  engine.configureLegacyParameters(makeParams(planFor(0), delayParam), kSamplesPerBeat, 0.0, false);

  std::array<float*, 2> pointers{};
  for (int position = 0; position < total; position += kBlock) {
    engine.setFixedLatency(latencyFor(position));
    engine.configureLegacyParameters(makeParams(planFor(position), delayParam), kSamplesPerBeat, 0.0, false);
    pointers[0] = left.data() + position;
    pointers[1] = right.data() + position;
    if (!engine.process(pointers.data(), 2, std::min(kBlock, total - position)))
      return {};
  }
  return left;
}

// The nominal delay of a millisecond setting, as the engine computes it.
int nominalDelay(double ms)
{
  return std::max(kFloorDelay, static_cast<int>(std::lround(ms * kRate / 1000.0)));
}

// With a fixed latency budget, the delay of a single click is the budget plus the Delay setting,
// at every FFT size that fits the budget.
void testSteadyDelayIsLatencyPlusDelay()
{
  struct Setting {
    int latency;
    double ms;
  };
  const std::vector<Setting> settings{{4095, 2.0}, {4095, 50.0}, {16383, 50.0}, {1023, 2.0}, {80639, 2.0}};

  for (const auto& setting : settings) {
    for (int plan = 0; plan < DtBlkFxEngine::fftSizeCount; ++plan) {
      DtBlkFxEngine engine;
      constexpr int clickAt = 1000;
      const auto total = clickAt + setting.latency + nominalDelay(setting.ms) + 6000;
      const auto signal = run(engine, total, {clickAt}, [plan](int) { return plan; }, delayParamMs(setting.ms),
                              [&](int) { return setting.latency; });
      const auto clicks = findClicks(signal);

      const auto label = "latency " + std::to_string(setting.latency) + ", delay " + std::to_string(setting.ms) +
                         " ms, FFT " + std::to_string(DtBlkFxEngine::fftSizeAt(plan));
      if (!check(!clicks.empty(), label + ": no output click"))
        continue;
      check(clicks.size() == 1, label + ": " + std::to_string(clicks.size()) + " output clicks for one input click");
      const auto delay = clicks.front().position - clickAt;
      const auto expected = setting.latency + nominalDelay(setting.ms);
      check(std::abs(delay - expected) <= 1,
            label + ": delay " + std::to_string(delay) + ", expected " + std::to_string(expected));
    }
  }
}

// Changing the FFT size in the middle of a click train must keep every click where it belongs.
void testFftChangeKeepsTiming()
{
  struct Change {
    const char* name;
    int from;
    int to;
    bool ramp;
    double ms;
  };
  const std::vector<Change> changes{{"shrink", 16, 8, false, 50.0},
                                    {"grow", 8, 16, false, 50.0},
                                    {"shrink, ramped", 16, 8, true, 50.0},
                                    {"grow, ramped", 8, 16, true, 50.0},
                                    {"shrink at the floor delay", 16, 8, false, 2.0},
                                    {"large to small", 24, 4, false, 50.0}};
  const int latency = 16383;
  const int switchAt = 80000;
  const int total = 200000;
  const int firstClick = 1000;

  std::vector<int> clickPositions;
  for (int position = firstClick; position < total; position += kSpacing)
    clickPositions.push_back(position);

  for (const auto& change : changes) {
    DtBlkFxEngine engine;
    const int from = change.from;
    const int to = change.to;
    const bool ramp = change.ramp;
    const auto signal = run(
        engine, total, clickPositions,
        [=](int position) {
          if (position < switchAt)
            return from;
          if (!ramp)
            return to;
          const auto steps = std::abs(to - from);
          const auto done = std::min(steps, (position - switchAt) / 1500 + 1);
          return from + (to > from ? done : -done);
        },
        delayParamMs(change.ms), [latency](int) { return latency; });
    const auto clicks = findClicks(signal);

    const int delay = latency + nominalDelay(change.ms);
    const auto label = std::string(change.name);

    // Every input click must come out, at full height, exactly one delay later.
    int matched = 0;
    int missing = 0;
    int firstMissing = -1;
    for (const auto input : clickPositions) {
      const int expected = input + delay;
      if (expected + 3 >= total)
        break;
      const auto found = std::find_if(clicks.begin(), clicks.end(), [expected](const Click& click) {
        return std::abs(click.position - expected) <= 1;
      });
      if (found != clicks.end() && found->height > 0.45f)
        ++matched;
      else {
        if (firstMissing < 0)
          firstMissing = expected;
        ++missing;
      }
    }
    check(missing == 0,
          label + ": " + std::to_string(missing) + " clicks missing or too quiet, the first expected at output sample " +
              std::to_string(firstMissing));

    // No extra clicks: every output click must belong to an input click.
    int extra = 0;
    for (const auto& click : clicks) {
      const int input = click.position - delay;
      const int offset = ((input - firstClick) % kSpacing + kSpacing) % kSpacing;
      if (input < firstClick - 2 || (offset > 1 && offset < kSpacing - 1))
        ++extra;
    }
    check(extra == 0, label + ": " + std::to_string(extra) + " output clicks that do not belong to an input click");
    info(label + ": " + std::to_string(matched) + " of " + std::to_string(clickPositions.size()) +
         " clicks at their expected position");
  }
}

// Changing the latency budget while playing: the output stays finite, and once the old pipeline
// has drained every click comes out at the new latency plus the delay.
void testLatencyChangeWhilePlaying()
{
  const int total = 450000;
  const int switchAt = 60000;
  const int firstClick = 1000;
  std::vector<int> clickPositions;
  for (int position = firstClick; position < total; position += kSpacing)
    clickPositions.push_back(position);

  for (const auto& pair :
       std::vector<std::pair<int, int>>{{4095, 16383}, {16383, 4095}, {1023, 80639}, {80639, 1023}}) {
    DtBlkFxEngine engine;
    const int first = pair.first;
    const int second = pair.second;
    const auto signal = run(
        engine, total, clickPositions, [](int) { return 16; }, delayParamMs(2.0),
        [=](int position) { return position < switchAt ? first : second; });
    const auto label = "latency " + std::to_string(first) + " -> " + std::to_string(second);

    bool finite = !signal.empty();
    for (const auto sample : signal)
      if (!std::isfinite(sample))
        finite = false;
    check(finite, label + ": non-finite output or processing failure");

    // Look only at clicks that left the engine after everything written under the old latency
    // has come out.
    const int windowStart = switchAt + std::max(first, second) + 20000;
    const int expectedDelay = second + nominalDelay(2.0);
    const auto clicks = findClicks(signal);
    int onGrid = 0;
    int considered = 0;
    for (const auto& click : clicks) {
      if (click.position < windowStart || click.position >= total - 3)
        continue;
      ++considered;
      const int input = click.position - expectedDelay;
      if (((input - firstClick) % kSpacing + kSpacing) % kSpacing <= 1)
        ++onGrid;
    }
    check(considered > 10,
          label + ": only " + std::to_string(considered) + " clicks found after the change, expected many more");
    check(onGrid * 10 >= considered * 9,
          label + ": clicks after the change are not at the new delay (" + std::to_string(onGrid) + " of " +
              std::to_string(considered) + " on the expected grid)");
  }
}

// The FFT size is limited to what fits the budget.
void testFftSizeIsLimitedToBudget()
{
  struct Limit {
    int latency;
    int expectedSize;
  };
  // The size list is 256 320 384 448 512 640 768 896 1024 1280 1536 ... 3072 3584 4096 ...,
  // and a size N fits a budget B when N - 1 <= B.
  const std::vector<Limit> limits{{255, 256},   {1023, 1024}, {1278, 1024}, {1279, 1280},   {1500, 1280},
                                  {4095, 4096}, {4094, 3584}, {9599, 9600}, {80639, 80640}, {100, 256}};
  for (const auto& limit : limits) {
    DtBlkFxEngine engine;
    if (!check(engine.prepare(33, kRate), "prepare failed"))
      return;
    engine.setFixedLatency(limit.latency);
    engine.reset();
    engine.configureLegacyParameters(makeParams(33, delayParamMs(2.0)), kSamplesPerBeat, 0.0, false);
    check(engine.getFftSize() == limit.expectedSize,
          "latency budget " + std::to_string(limit.latency) + " allows FFT " + std::to_string(engine.getFftSize()) +
              ", expected " + std::to_string(limit.expectedSize));
    check(engine.getLatencySamples() == limit.latency, "reported latency does not match the budget");
  }

  // Without a budget the old behaviour stays: latency is the FFT size minus one.
  DtBlkFxEngine engine;
  if (check(engine.prepare(16, kRate), "prepare failed")) {
    check(engine.getFixedLatency() == 0, "a new engine should have no fixed latency");
    check(engine.getLatencySamples() == 4095, "without a budget the latency should be FFT size minus one");
    engine.setFixedLatency(8191);
    check(engine.getLatencySamples() == 8191, "the budget should be reported");
    engine.setFixedLatency(0);
    check(engine.getLatencySamples() == engine.getFftSize() - 1, "budget 0 should restore the old behaviour");
    engine.setFixedLatency(-5);
    check(engine.getFixedLatency() == 0, "a negative budget should mean no budget");
    engine.setFixedLatency(10000000);
    check(engine.getFixedLatency() == DtBlkFxEngine::maxFftSize, "a huge budget should be clamped");
  }
}

// Random latency budgets, FFT sizes and effects: no crash, no NaN.
void testFuzz()
{
  std::uint32_t state = 99u;
  const auto next = [&state]() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  };

  DtBlkFxEngine engine;
  for (int round = 0; round < 40; ++round) {
    const auto latency = static_cast<int>(next() % 90000u);
    const auto plan = static_cast<int>(next() % 34u);
    const auto effect = static_cast<int>(next() % 31u);
    const auto signal = run(
        engine, 40000, {1000, 5000, 9000},
        [&](int position) { return (plan + position / 4000) % 34; }, delayParamMs(2.0 + (next() % 200u)),
        [&](int position) { return position < 20000 ? latency : latency / 2; }, 8);
    (void)effect;
    bool finite = !signal.empty();
    for (const auto sample : signal)
      if (!std::isfinite(sample))
        finite = false;
    check(finite, "random round " + std::to_string(round) + " (latency " + std::to_string(latency) +
                      "): non-finite output or processing failure");
  }
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
  runTest("steady delay is latency plus delay at every FFT size", testSteadyDelayIsLatencyPlusDelay);
  runTest("FFT size change keeps every click in place", testFftChangeKeepsTiming);
  runTest("latency change while playing", testLatencyChangeWhilePlaying);
  runTest("FFT size is limited to the budget", testFftSizeIsLimitedToBudget);
  runTest("random latency budgets and sizes", testFuzz);

  std::cout << (failureCount == 0 ? "All fixed latency tests passed" : "Fixed latency tests failed") << " ("
            << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}
