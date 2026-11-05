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

// Tests for the host timing tracker and for how the beat grid reaches the engine's block-sync
// scheduler. These tests do not link JUCE.

#include "DtBlkFxEngine.h"
#include "DtBlkFxHostTiming.h"
#include "DtBlkFxLegacyMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using Tracker = DtBlkFxHost::BeatTracker;
using Transport = DtBlkFxHost::TransportInfo;
using Params = std::array<float, 44>;
using Channels = std::vector<std::vector<float>>;

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

bool nearlyEqual(double a, double b, double tolerance)
{
  return std::abs(a - b) <= tolerance;
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
  int nextInt(int count) { return static_cast<int>(nextBits() >> 8) % count; }
};

Transport makeTransport(std::optional<double> bpm, std::optional<double> ppq, bool playing)
{
  Transport transport;
  transport.bpm = bpm;
  transport.ppqPosition = ppq;
  transport.isPlaying = playing;
  return transport;
}

float fftLenParam(int planIndex)
{
  return static_cast<float>((planIndex + 2) * 4) / 255.0f;
}

Params makeParams(int plan, float overlap, const std::array<int, 8>& types)
{
  Params p{};
  p[0] = 1.0f; // fully wet
  p[1] = 0.0f; // minimum delay
  p[2] = fftLenParam(plan);
  p[3] = overlap;
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = 0.2f;
    p[offset + 1] = 0.7f;
    p[offset + 2] = 0.6f;
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(types[slot]);
    p[offset + 4] = 0.6f;
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

bool identical(const Channels& a, const Channels& b, std::string& detail)
{
  if (a.size() != b.size()) {
    detail = "different channel counts";
    return false;
  }
  for (size_t channel = 0; channel < a.size(); ++channel) {
    if (a[channel].size() != b[channel].size()) {
      detail = "different lengths";
      return false;
    }
    for (size_t i = 0; i < a[channel].size(); ++i)
      if (a[channel][i] != b[channel][i]) {
        detail = "first difference in channel " + std::to_string(channel) + " at sample " +
                 std::to_string(i) + " (" + std::to_string(a[channel][i]) + " vs " +
                 std::to_string(b[channel][i]) + ")";
        return false;
      }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Tracker tests
// ---------------------------------------------------------------------------------------------

void testDefaultsWithoutHost()
{
  Tracker tracker;
  tracker.reset(44100.0);

  const auto first = tracker.update(Transport{}, 512);
  check(nearlyEqual(first.samplesPerBeat, 22050.0, 1.0e-9), "without a host the tempo should be 120 BPM");
  check(nearlyEqual(first.beatPhaseSamples, 0.0, 1.0e-9), "without a host the first phase should be 0");
  check(!first.followingHost, "without a host the tracker should not claim to follow it");

  const auto second = tracker.update(Transport{}, 512);
  check(nearlyEqual(second.beatPhaseSamples, 512.0, 1.0e-9),
        "the grid should keep running by the block length (got " +
            std::to_string(second.beatPhaseSamples) + ")");
}

void testHostTempoAndInvalidTempo()
{
  Tracker tracker;
  tracker.reset(44100.0);
  const auto at90 = tracker.update(makeTransport(90.0, std::nullopt, true), 512);
  check(nearlyEqual(at90.samplesPerBeat, 29400.0, 1.0e-6), "90 BPM at 44.1 kHz should be 29400 samples per beat");

  tracker.update(makeTransport(140.0, std::nullopt, true), 512);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const auto bad : {nan, inf, -inf, 0.0, -5.0, 0.5, 1001.0, 1.0e9}) {
    const auto timing = tracker.update(makeTransport(bad, std::nullopt, true), 512);
    check(nearlyEqual(timing.samplesPerBeat, 44100.0 * 60.0 / 140.0, 1.0e-6),
          "an unusable tempo (" + std::to_string(bad) + ") should keep the last valid one");
  }
  const auto absent = tracker.update(Transport{}, 512);
  check(nearlyEqual(absent.samplesPerBeat, 44100.0 * 60.0 / 140.0, 1.0e-6),
        "a block without tempo should keep the last valid one");
}

void testPhaseFromHostPosition()
{
  const auto phaseFor = [](std::optional<double> ppq) {
    Tracker tracker;
    tracker.reset(44100.0);
    return tracker.update(makeTransport(120.0, ppq, true), 512);
  };

  const auto quarter = phaseFor(1.25);
  check(nearlyEqual(quarter.beatPhaseSamples, 5512.5, 1.0e-6), "position 1.25 should give a phase of 5512.5");
  check(quarter.followingHost, "a playing host with a position should be followed");

  check(nearlyEqual(phaseFor(-0.25).beatPhaseSamples, 16537.5, 1.0e-6),
        "a negative position (pre-roll) should wrap to the end of the beat");

  const auto huge = phaseFor(1.0e15);
  check(std::isfinite(huge.beatPhaseSamples) && huge.beatPhaseSamples >= 0.0 &&
            huge.beatPhaseSamples < 22050.0,
        "a huge position should still give a phase inside the beat");

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const auto bad : {nan, inf}) {
    const auto timing = phaseFor(bad);
    check(!timing.followingHost && nearlyEqual(timing.beatPhaseSamples, 0.0, 1.0e-9),
          "a non-finite position should be ignored");
  }
}

void testFreeRunAndStoppedTransport()
{
  {
    Tracker tracker;
    tracker.reset(44100.0);
    const auto first = tracker.update(makeTransport(120.0, 0.5, true), 512);
    check(nearlyEqual(first.beatPhaseSamples, 11025.0, 1.0e-6), "first block should start at phase 11025");
    bool ok = true;
    for (int k = 1; k <= 200 && ok; ++k) {
      const auto timing = tracker.update(makeTransport(120.0, std::nullopt, true), 512);
      const auto expected = std::fmod(11025.0 + 512.0 * k, 22050.0);
      ok = check(nearlyEqual(timing.beatPhaseSamples, expected, 1.0e-6),
                 "block " + std::to_string(k) + ": free-running phase " +
                     std::to_string(timing.beatPhaseSamples) + ", expected " + std::to_string(expected));
    }
  }

  {
    // A stopped transport that keeps reporting the same position must not freeze the grid.
    Tracker tracker;
    tracker.reset(44100.0);
    const auto a = tracker.update(makeTransport(120.0, 3.5, false), 512);
    const auto b = tracker.update(makeTransport(120.0, 3.5, false), 512);
    check(a.followingHost, "the first position should be followed");
    check(!b.followingHost, "a repeated position while stopped should not be followed");
    check(nearlyEqual(b.beatPhaseSamples, std::fmod(a.beatPhaseSamples + 512.0, 22050.0), 1.0e-6),
          "a stopped transport should let the grid run on");

    // Locating while stopped, and a jump while playing, must be followed.
    const auto located = tracker.update(makeTransport(120.0, 7.25, false), 512);
    check(located.followingHost && nearlyEqual(located.beatPhaseSamples, 5512.5, 1.0e-6),
          "a changed position while stopped (locate) should be followed");
    const auto jumped = tracker.update(makeTransport(120.0, 40.75, true), 512);
    check(jumped.followingHost && nearlyEqual(jumped.beatPhaseSamples, 16537.5, 1.0e-6),
          "a jump while playing should be followed");
  }
}

void testTempoChangeKeepsBeatFraction()
{
  Tracker tracker;
  tracker.reset(44100.0);
  tracker.update(makeTransport(120.0, 0.5, true), 512);
  // Before the change the phase is 11025 + 512 = 11537 of 22050; at 60 BPM the beat is twice as long.
  const auto changed = tracker.update(makeTransport(60.0, std::nullopt, false), 512);
  check(nearlyEqual(changed.samplesPerBeat, 44100.0, 1.0e-6), "60 BPM should be 44100 samples per beat");
  check(nearlyEqual(changed.beatPhaseSamples, 23074.0, 1.0e-6),
        "a tempo change should keep the position inside the beat (got " +
            std::to_string(changed.beatPhaseSamples) + ", expected 23074)");
}

void testFuzzedInput()
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const std::array<double, 12> special{nan, inf, -inf, 0.0, -1.0, 1.0e-300,
                                       1.0e300, 1.0, 1000.0, 1001.0, 120.0, -1.0e15};
  const std::array<int, 7> blockSizes{-5, 0, 1, 64, 512, 4096, 1 << 20};

  for (const auto rate : {44100.0, 48000.0, 96000.0, 0.0, nan}) {
    Rng rng(2026u);
    Tracker tracker;
    tracker.reset(rate);
    bool ok = true;
    for (int i = 0; i < 3000 && ok; ++i) {
      std::optional<double> bpm;
      std::optional<double> ppq;
      switch (rng.nextInt(4)) {
        case 1: bpm = special[static_cast<size_t>(rng.nextInt(12))]; break;
        case 2: bpm = 20.0 + 280.0 * static_cast<double>(rng.nextFloat()); break;
        case 3: bpm = special[static_cast<size_t>(rng.nextInt(12))]; break;
        default: break;
      }
      switch (rng.nextInt(4)) {
        case 1: ppq = special[static_cast<size_t>(rng.nextInt(12))]; break;
        case 2: ppq = 1000.0 * static_cast<double>(rng.nextFloat()) - 100.0; break;
        case 3: ppq = special[static_cast<size_t>(rng.nextInt(12))]; break;
        default: break;
      }
      const auto timing = tracker.update(makeTransport(bpm, ppq, rng.nextInt(2) == 0),
                                         blockSizes[static_cast<size_t>(rng.nextInt(7))]);
      ok = check(std::isfinite(timing.samplesPerBeat) && timing.samplesPerBeat > 0.0 &&
                     std::isfinite(timing.beatPhaseSamples) && timing.beatPhaseSamples >= 0.0 &&
                     timing.beatPhaseSamples < timing.samplesPerBeat,
                 "rate " + std::to_string(rate) + ", update " + std::to_string(i) +
                     ": unusable timing (samples per beat " + std::to_string(timing.samplesPerBeat) +
                     ", phase " + std::to_string(timing.beatPhaseSamples) + ")");
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Engine tests: does the beat grid reach the block-sync scheduler?
// ---------------------------------------------------------------------------------------------

// Hop (in samples) the engine plans for its first frame, for a host that reports the given beat
// position at 720 BPM and 48 kHz (4000 samples per beat) with block sync on and FFT 1024.
int firstHopFor(double ppq)
{
  DtBlkFxEngine engine;
  if (!engine.prepare(8, 48000.0))
    return -1;
  Tracker tracker;
  tracker.reset(48000.0);
  const auto timing = tracker.update(makeTransport(720.0, ppq, true), 512);
  const auto params = makeParams(8, 0.75f, {9, 9, 9, 9, 9, 9, 9, 9});
  if (!engine.configureLegacyParameters(params, timing.samplesPerBeat, timing.beatPhaseSamples, false))
    return -1;
  return engine.getNextHopSize();
}

void testBlockSyncAlignmentReachesEngine()
{
  // Without sync the hop is 367 samples at overlap 0.75. A beat boundary 251..367 samples ahead
  // shortens the first hop to land on it.
  check(firstHopFor(10.0) == 367, "no beat boundary ahead: expected the normal hop of 367, got " +
                                      std::to_string(firstHopFor(10.0)));
  check(firstHopFor(10.5) == 367, "beat boundary too far: expected the normal hop of 367, got " +
                                      std::to_string(firstHopFor(10.5)));
  check(std::abs(firstHopFor(10.925) - 300) <= 1,
        "beat boundary 300 samples ahead: expected a hop near 300, got " + std::to_string(firstHopFor(10.925)));
  check(std::abs(firstHopFor(10.92) - 320) <= 1,
        "beat boundary 320 samples ahead: expected a hop near 320, got " + std::to_string(firstHopFor(10.92)));
}

// Renders 12000 samples of stereo noise with block sync on (FFT 1024, 4000 samples per beat).
// With useTracker the beat phase comes from the tracker (host position only in the first block);
// without it the phase is 0 in every block, as the plugin used to do without a host position.
Channels renderBlockSync(const std::vector<int>& chunks, double firstPpq, bool useTracker)
{
  constexpr int plan = 8;
  constexpr double rate = 48000.0;
  constexpr double bpm = 720.0;
  constexpr int total = 12000;

  DtBlkFxEngine engine;
  if (!engine.prepare(plan, rate))
    return {};
  Tracker tracker;
  tracker.reset(rate);
  const auto params = makeParams(plan, 0.75f, {0, 7, 9, 9, 9, 9, 9, 9});

  Channels audio{makeNoise(total, 1u), makeNoise(total, 2u)};
  std::array<float*, 2> pointers{};
  int position = 0;
  size_t chunkIndex = 0;
  while (position < total) {
    const auto count = std::min(chunks[chunkIndex % chunks.size()], total - position);
    ++chunkIndex;

    auto samplesPerBeat = rate * 60.0 / bpm;
    auto phase = 0.0;
    if (useTracker) {
      std::optional<double> ppq;
      if (position == 0)
        ppq = firstPpq;
      const auto timing = tracker.update(makeTransport(bpm, ppq, true), count);
      samplesPerBeat = timing.samplesPerBeat;
      phase = timing.beatPhaseSamples;
    }
    if (!engine.configureLegacyParameters(params, samplesPerBeat, phase, false))
      return {};

    pointers[0] = audio[0].data() + position;
    pointers[1] = audio[1].data() + position;
    if (!engine.process(pointers.data(), 2, count))
      return {};
    position += count;
  }
  return audio;
}

void testChunkIndependenceWithTracker()
{
  const std::vector<std::vector<int>> patterns{
      {1}, {7}, {64}, {513}, {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233}};

  const auto reference = renderBlockSync({12000}, 0.0, true);
  if (!check(!reference.empty(), "reference render failed"))
    return;

  for (const auto& pattern : patterns) {
    const auto chunked = renderBlockSync(pattern, 0.0, true);
    std::string detail;
    check(!chunked.empty() && identical(reference, chunked, detail),
          "block sync with chunks starting " + std::to_string(pattern.front()) +
              ": output depends on block size: " + detail);
  }

  // Moving the beat grid must change what block sync does, otherwise sync is not active.
  const auto shifted = renderBlockSync({12000}, 0.5, true);
  std::string detail;
  check(!shifted.empty() && !identical(reference, shifted, detail),
        "moving the host beat position by half a beat did not change the output; block sync is not active");

  const auto phaseZero = renderBlockSync({513}, 0.0, false);
  info(std::string("re-sending beat phase 0 with every block (the old behaviour without a host "
                   "position): output ") +
       (!phaseZero.empty() && identical(reference, phaseZero, detail)
            ? "matches the stable grid in this setup"
            : "DIFFERS from the stable grid (" + detail + ")"));
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
  runTest("defaults without a host", testDefaultsWithoutHost);
  runTest("host tempo and invalid tempo", testHostTempoAndInvalidTempo);
  runTest("phase from the host position", testPhaseFromHostPosition);
  runTest("free-running grid and stopped transport", testFreeRunAndStoppedTransport);
  runTest("tempo change keeps the beat fraction", testTempoChangeKeepsBeatFraction);
  runTest("fuzzed host input", testFuzzedInput);
  runTest("block sync alignment reaches the engine", testBlockSyncAlignmentReachesEngine);
  runTest("block sync is independent of block size with the tracker", testChunkIndependenceWithTracker);

  std::cout << (failureCount == 0 ? "All host timing tests passed" : "Host timing tests failed") << " ("
            << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}
