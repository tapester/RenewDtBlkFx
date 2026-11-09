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

// Engine-level robustness tests: every effect, many FFT sizes and sample rates, odd host
// block sizes, reset behaviour, API misuse and randomised configurations.
// These tests do not link JUCE; the processor-level tests live in DtBlkFxProcessorTests.cpp.

#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using Params = std::array<float, 44>;
using Channels = std::vector<std::vector<float>>;

constexpr double pi = 3.14159265358979323846;
constexpr int effectCount = DtBlkFxLegacy::effectTypeCount;

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

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

// Small deterministic generator so failures are reproducible on every machine.
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

// Legacy FFT length parameter that selects the given plan index (see updateFrameParameters).
float fftLenParam(int planIndex)
{
  return static_cast<float>((planIndex + 2) * 4) / 255.0f;
}

struct Slot {
  int type = 9; // None
  float freqA = 0.0f;
  float freqB = 1.0f;
  float amplitude = 0.6f; // 0 dB
  float value = 0.5f;
};
using Slots = std::array<Slot, 8>;

// Build the engine's 44-float parameter block. mixParameter 1.0 means fully wet, no power match.
Params makeParams(const Slots& slots, int plan, float overlap, float delay, float mixParameter = 1.0f)
{
  Params p{};
  p[0] = mixParameter;
  p[1] = delay;
  p[2] = fftLenParam(plan);
  p[3] = overlap;
  for (size_t slot = 0; slot < slots.size(); ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = slots[slot].freqA;
    p[offset + 1] = slots[slot].freqB;
    p[offset + 2] = slots[slot].amplitude;
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(slots[slot].type);
    p[offset + 4] = slots[slot].value;
  }
  return p;
}

// Delay in samples for a beat-based delay parameter (< 0.5); the engine never goes below 100.
int estimateDelaySamples(float delayParameter, double samplesPerBeat)
{
  const auto beats = static_cast<double>(delayParameter) * 2.0 * (0.5 * 255.0 / 16.0);
  return std::max(100, static_cast<int>(std::lround(beats * samplesPerBeat)));
}

int totalSamples(int plan, int delaySamples, int frames)
{
  const auto fft = DtBlkFxEngine::fftSizeAt(plan);
  return fft - 1 + delaySamples + frames * fft + 1024;
}

enum class Signal { Silence, Impulse, Dc, NoiseHalf, NoiseFull, Sine };

const char* signalName(Signal signal)
{
  switch (signal) {
    case Signal::Silence: return "silence";
    case Signal::Impulse: return "impulse";
    case Signal::Dc: return "DC";
    case Signal::NoiseHalf: return "noise (half scale)";
    case Signal::NoiseFull: return "noise (full scale)";
    case Signal::Sine: return "sine";
  }
  return "?";
}

std::vector<float> makeSignal(Signal signal, int count, std::uint32_t seed, double sampleRate)
{
  std::vector<float> data(static_cast<size_t>(count), 0.0f);
  Rng rng(seed);
  for (int i = 0; i < count; ++i) {
    float value = 0.0f;
    switch (signal) {
      case Signal::Silence: value = 0.0f; break;
      case Signal::Impulse: value = i == 100 ? 1.0f : 0.0f; break;
      case Signal::Dc: value = 0.5f; break;
      case Signal::NoiseHalf: value = rng.nextFloat() - 0.5f; break;
      case Signal::NoiseFull: value = rng.nextFloat() * 2.0f - 1.0f; break;
      case Signal::Sine:
        value = 0.5f * static_cast<float>(std::sin(2.0 * pi * 440.0 * i / sampleRate));
        break;
    }
    data[static_cast<size_t>(i)] = value;
  }
  return data;
}

Channels makeInput(Signal signal, int channels, int count, double sampleRate)
{
  Channels input;
  for (int channel = 0; channel < channels; ++channel)
    input.push_back(makeSignal(signal, count, 1000u + static_cast<std::uint32_t>(channel) * 31u, sampleRate));
  return input;
}

bool allFinite(const std::vector<float>& data)
{
  for (const auto sample : data)
    if (!std::isfinite(sample))
      return false;
  return true;
}

float peakOf(const std::vector<float>& data, int from = 0)
{
  float result = 0.0f;
  for (size_t i = static_cast<size_t>(from); i < data.size(); ++i)
    result = std::max(result, std::abs(data[i]));
  return result;
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

// Compares first, then builds the failure message, so the detail text is never empty.
bool checkIdentical(const Channels& a, const Channels& b, const std::string& label)
{
  std::string detail;
  const auto same = identical(a, b, detail);
  check(same, label + ": " + detail);
  return same;
}

struct RenderSetup {
  Params params{};
  double samplesPerBeat = 22050.0;
  bool parameterSync = false;
  // The plugin re-sends its parameters before every block; this mimics that.
  bool reconfigurePerBlock = false;
  // When true, the beat phase passed with each block is derived from the absolute position,
  // as a host that reports its beat position would. When false the phase is always 0.
  bool useBeatPhase = false;
};

// Process 'input' through the engine in chunks of the given sizes (cycled). The engine must
// already be prepared; call reset() beforehand when reusing it.
bool renderEngine(DtBlkFxEngine& engine,
                  const RenderSetup& setup,
                  const Channels& input,
                  Channels& output,
                  const std::vector<int>& chunks)
{
  const auto channelCount = static_cast<int>(input.size());
  const auto total = static_cast<int>(input[0].size());
  output = input;

  if (!engine.configureLegacyParameters(setup.params, setup.samplesPerBeat, 0.0, setup.parameterSync))
    return false;

  std::array<float*, 2> pointers{};
  int position = 0;
  size_t chunkIndex = 0;
  while (position < total) {
    const auto count = std::min(chunks[chunkIndex % chunks.size()], total - position);
    ++chunkIndex;

    if (setup.reconfigurePerBlock && position > 0) {
      const auto phase =
          setup.useBeatPhase ? std::fmod(static_cast<double>(position), setup.samplesPerBeat) : 0.0;
      if (!engine.configureLegacyParameters(
              setup.params, setup.samplesPerBeat, phase, setup.parameterSync))
        return false;
    }

    for (int channel = 0; channel < channelCount; ++channel)
      pointers[static_cast<size_t>(channel)] = output[static_cast<size_t>(channel)].data() + position;
    if (!engine.process(pointers.data(), channelCount, count))
      return false;
    position += count;
  }
  return true;
}

std::string describe(double rate, int channels, int fft, int type, const char* variant)
{
  return "rate " + std::to_string(static_cast<int>(rate)) + ", " + std::to_string(channels) +
         " ch, FFT " + std::to_string(fft) + ", effect " + std::to_string(type) + " (" + variant + ")";
}

// Peak limit for "neutral" settings. Effect 19 ("Sweep") has a coefficient table with extreme
// harmonic ratios (identical to the legacy table), so large levels may be legacy behaviour.
// The limit for it is provisional until the legacy comparison (roadmap step 5).
float neutralPeakLimit(int type)
{
  return type == 19 ? 1.0e6f : 1000.0f;
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

// Every effect, at several FFT sizes and sample rates, mono and stereo.
void testEffectMatrix()
{
  const std::array<int, 5> plans{0, 8, 16, 24, 33}; // 256, 1024, 4096, 16384, 80640
  const std::array<double, 3> rates{44100.0, 48000.0, 96000.0};
  const std::vector<int> hostBlocks{512};

  int runs = 0;
  int silentNeutral = 0;
  float largestNeutralPeak = 0.0f;
  float largestSweepEffectPeak = 0.0f;

  for (const auto rate : rates) {
    for (const auto plan : plans) {
      DtBlkFxEngine engine;
      if (!check(engine.prepare(plan, rate), "prepare failed at rate " + std::to_string(rate)))
        continue;

      const auto fft = DtBlkFxEngine::fftSizeAt(plan);
      const auto total = totalSamples(plan, 100, 2);

      for (int channels = 1; channels <= 2; ++channels) {
        const auto input = makeInput(Signal::NoiseHalf, channels, total, rate);

        for (int type = 0; type < effectCount; ++type) {
          for (int variant = 0; variant < 2; ++variant) {
            const auto neutral = variant == 0;
            Slots slots{};
            slots[0] = neutral ? Slot{type, 0.2f, 0.8f, 0.6f, 0.5f} : Slot{type, 0.9f, 0.1f, 0.7f, 1.0f};
            // A filter after slot 0, so the masks of the mask effects (20-25) are used.
            slots[1] = Slot{0, 0.0f, 1.0f, neutral ? 0.6f : 0.3f, 0.5f};

            RenderSetup setup;
            setup.params = makeParams(slots, plan, 0.0f, 0.0f);
            setup.samplesPerBeat = rate * 0.5;

            const auto label = describe(rate, channels, fft, type, neutral ? "neutral" : "extreme");
            engine.reset();
            Channels output;
            if (!check(renderEngine(engine, setup, input, output, hostBlocks), label + ": processing failed"))
              continue;
            ++runs;

            for (size_t channel = 0; channel < output.size(); ++channel) {
              const auto finite = allFinite(output[channel]);
              check(finite, label + ": non-finite output in channel " + std::to_string(channel));
              if (finite && neutral) {
                const auto peak = peakOf(output[channel]);
                largestNeutralPeak = std::max(largestNeutralPeak, peak);
                if (type == 19)
                  largestSweepEffectPeak = std::max(largestSweepEffectPeak, peak);
                check(peak < neutralPeakLimit(type),
                      label + ": output peak " + std::to_string(peak) + " is runaway-large");
              }
            }

            if (neutral && rate == 44100.0 && plan == 8 && channels == 2 &&
                peakOf(output[0], total - fft) < 1.0e-6f) {
              ++silentNeutral;
              info("effect type " + std::to_string(type) +
                   " produced silence with neutral settings (1024 FFT, 44.1 kHz, stereo)");
            }
          }
        }
      }
    }
  }

  std::cout << "  " << runs << " runs, largest neutral peak " << largestNeutralPeak << ", "
            << silentNeutral << " effect(s) silent with neutral settings\n";
  info("largest neutral peak of effect 19 (Sweep, provisional limit 1e6): " +
       std::to_string(largestSweepEffectPeak));
}

// The sweep effects' copy mode must produce a level that does not depend on the FFT size.
// (The legacy plugin worked on a spectrum normalised by 1/fftSize; without that the level grew
// in proportion to the FFT size.)
void testSweepCopyLevelIndependentOfFftSize()
{
  constexpr double rate = 44100.0;
  const std::array<int, 2> plans{8, 24}; // 1024 and 16384
  std::array<float, 2> peaks{};

  for (size_t index = 0; index < plans.size(); ++index) {
    const auto plan = plans[index];
    DtBlkFxEngine engine;
    if (!check(engine.prepare(plan, rate), "prepare failed"))
      return;

    const auto total = totalSamples(plan, 100, 3);
    std::vector<float> tone(static_cast<size_t>(total), 0.0f);
    for (int n = 0; n < total; ++n) {
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 10; ++harmonic)
        value += (0.25 / harmonic) * std::sin(2.0 * pi * 220.0 * harmonic * n / rate);
      tone[static_cast<size_t>(n)] = static_cast<float>(value);
    }
    const Channels input{tone};

    Slots slots{};
    slots[0] = Slot{15, 0.0f, 1.0f, 0.6f, 0.6f}; // Triangles, value 0.6 = copy mode
    RenderSetup setup;
    setup.params = makeParams(slots, plan, 0.0f, 0.0f);
    setup.samplesPerBeat = rate * 0.5;

    Channels output;
    if (!check(renderEngine(engine, setup, input, output, std::vector<int>{512}),
               "sweep level render failed"))
      return;
    check(allFinite(output[0]), "sweep level: non-finite output");
    peaks[index] = peakOf(output[0], total / 2);
  }

  info("sweep copy mode peak: " + std::to_string(peaks[0]) + " at FFT 1024, " +
       std::to_string(peaks[1]) + " at FFT 16384");
  if (check(peaks[0] > 1.0e-6f && peaks[1] > 1.0e-6f, "sweep level: output is silent")) {
    const auto ratio = peaks[1] / peaks[0];
    check(ratio > 0.25f && ratio < 4.0f,
          "sweep copy level depends on the FFT size (ratio 16384/1024 = " + std::to_string(ratio) + ")");
  }
}

// Different input signals through every effect.
void testInputSignals()
{
  constexpr double rate = 44100.0;
  constexpr int plan = 8;
  const std::vector<int> hostBlocks{512};
  const auto fft = DtBlkFxEngine::fftSizeAt(plan);
  const auto total = totalSamples(plan, 100, 3);

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, rate), "prepare failed"))
    return;

  const std::array<Signal, 6> signals{
      Signal::Silence, Signal::Impulse, Signal::Dc, Signal::NoiseHalf, Signal::NoiseFull, Signal::Sine};
  for (const auto signal : signals) {
    const auto input = makeInput(signal, 2, total, rate);
    for (int type = 0; type < effectCount; ++type) {
      Slots slots{};
      slots[0] = Slot{type, 0.2f, 0.8f, 0.6f, 0.5f};
      slots[1] = Slot{0, 0.0f, 1.0f, 0.6f, 0.5f};

      RenderSetup setup;
      setup.params = makeParams(slots, plan, 0.0f, 0.0f);
      setup.samplesPerBeat = rate * 0.5;

      const auto label = std::string(signalName(signal)) + ", FFT " + std::to_string(fft) +
                         ", effect " + std::to_string(type);
      engine.reset();
      Channels output;
      if (!check(renderEngine(engine, setup, input, output, hostBlocks), label + ": processing failed"))
        continue;

      for (size_t channel = 0; channel < output.size(); ++channel) {
        check(allFinite(output[channel]), label + ": non-finite output");
        if (signal == Signal::Silence)
          check(peakOf(output[channel]) < 1.0e-6f,
                label + ": silence in produced sound out (peak " +
                    std::to_string(peakOf(output[channel])) + ")");
      }
    }
  }
}

struct BlockCase {
  const char* name;
  Slots slots;
  int channels;
  float overlap;
  float delay;
  bool sync;
  double samplesPerBeat;
};

Slots slotsOf(const Slot& first, const Slot& second)
{
  Slots slots{};
  slots[0] = first;
  slots[1] = second;
  return slots;
}

// The result must not depend on how the host splits the audio into blocks.
void testBlockSizeIndependence()
{
  constexpr double rate = 44100.0;
  constexpr double normalBeat = 22050.0;
  // Block sync only acts when the minimum hop (beat / 16) is shorter than the hop at the chosen
  // FFT size, so that case uses a short beat.
  constexpr double syncBeat = 4000.0;
  constexpr int plan = 8;

  const std::vector<BlockCase> cases{
      {"no effects", slotsOf(Slot{}, Slot{}), 2, 0.3f, 0.0f, false, normalBeat},
      {"filter and shift", slotsOf(Slot{0, 0.1f, 0.6f, 0.3f, 0.5f}, Slot{7, 0.2f, 0.7f, 0.6f, 0.6f}), 2, 0.3f, 0.0f, false, normalBeat},
      {"harmonic effects", slotsOf(Slot{11, 0.0f, 1.0f, 0.6f, 0.6f}, Slot{14, 0.0f, 1.0f, 0.6f, 0.5f}), 2, 0.3f, 0.0f, false, normalBeat},
      {"smear and contrast", slotsOf(Slot{2, 0.1f, 0.9f, 0.6f, 0.5f}, Slot{1, 0.2f, 0.8f, 0.6f, 0.5f}), 2, 0.3f, 0.0f, false, normalBeat},
      {"stereo vocode and crossmix", slotsOf(Slot{26, 0.3f, 0.9f, 0.6f, 0.5f}, Slot{29, 0.0f, 1.0f, 0.6f, 0.5f}), 2, 0.3f, 0.0f, false, normalBeat},
      {"delay", slotsOf(Slot{0, 0.1f, 0.6f, 0.3f, 0.5f}, Slot{}), 2, 0.3f, 0.03f, false, normalBeat},
      {"block sync", slotsOf(Slot{0, 0.1f, 0.6f, 0.3f, 0.5f}, Slot{7, 0.2f, 0.7f, 0.6f, 0.6f}), 2, 0.75f, 0.0f, true, syncBeat},
  };
  const std::vector<std::vector<int>> patterns{
      {1}, {7}, {64}, {513}, {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233}};

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, rate), "prepare failed"))
    return;

  for (const auto& testCase : cases) {
    const auto total =
        totalSamples(plan, estimateDelaySamples(testCase.delay, testCase.samplesPerBeat), 6);
    const auto input = makeInput(Signal::NoiseHalf, testCase.channels, total, rate);

    RenderSetup setup;
    setup.params = makeParams(testCase.slots, plan, testCase.overlap, testCase.delay);
    setup.samplesPerBeat = testCase.samplesPerBeat;

    // Reference: one single call, parameters configured once.
    engine.reset();
    Channels reference;
    if (!check(renderEngine(engine, setup, input, reference, std::vector<int>{total}),
               std::string(testCase.name) + ": reference render failed"))
      continue;
    for (const auto& channel : reference)
      check(allFinite(channel), std::string(testCase.name) + ": reference output not finite");

    for (const auto& pattern : patterns) {
      const auto chunkLabel = "chunks starting " + std::to_string(pattern.front()) +
                              (pattern.size() > 1 ? " (irregular)" : "");

      // (a) parameters configured once, audio split into chunks
      engine.reset();
      Channels chunked;
      if (check(renderEngine(engine, setup, input, chunked, pattern),
                std::string(testCase.name) + ", " + chunkLabel + ": processing failed"))
        checkIdentical(reference, chunked,
                       std::string(testCase.name) + ", " + chunkLabel + ": output depends on block size");

      // (b) parameters re-sent before every block, as the plugin does. A host that reports its
      // beat position gives the engine a consistent beat grid, so the result must be unchanged.
      auto perBlock = setup;
      perBlock.reconfigurePerBlock = true;
      perBlock.useBeatPhase = testCase.sync;
      engine.reset();
      Channels resent;
      if (check(renderEngine(engine, perBlock, input, resent, pattern),
                std::string(testCase.name) + ", " + chunkLabel + " with per-block parameters: processing failed"))
        checkIdentical(reference, resent,
                       std::string(testCase.name) + ", " + chunkLabel +
                           " with per-block parameters: output depends on block size");
    }

    if (testCase.sync) {
      // Informational: a host without beat position leaves the phase at 0 in every block.
      auto noPhase = setup;
      noPhase.reconfigurePerBlock = true;
      noPhase.useBeatPhase = false;
      engine.reset();
      Channels resent;
      std::string detail;
      if (renderEngine(engine, noPhase, input, resent, std::vector<int>{513}))
        info(std::string("block sync without a host beat position: output ") +
             (identical(reference, resent, detail) ? "does not depend on block size"
                                                   : "DEPENDS on block size (" + detail + ")"));
    }
  }
}

// After reset() the engine must behave exactly like a new one.
void testResetReproducesFreshOutput()
{
  constexpr double rate = 44100.0;
  constexpr double samplesPerBeat = 22050.0;
  constexpr int plan = 8;
  const std::vector<int> hostBlocks{256};
  const auto total = totalSamples(plan, 100, 6);
  const auto input = makeInput(Signal::NoiseHalf, 2, total, rate);

  struct ResetCase {
    const char* name;
    Slots slots;
  };
  const std::vector<ResetCase> cases{
      {"filter, shift and contrast",
       slotsOf(Slot{0, 0.1f, 0.6f, 0.3f, 0.5f}, Slot{7, 0.2f, 0.7f, 0.6f, 0.6f})},
      {"harmonic effects", slotsOf(Slot{11, 0.0f, 1.0f, 0.6f, 0.6f}, Slot{1, 0.2f, 0.8f, 0.6f, 0.5f})},
      {"smear", slotsOf(Slot{2, 0.1f, 0.9f, 0.6f, 0.5f}, Slot{})},
  };

  for (const auto& testCase : cases) {
    RenderSetup setup;
    setup.params = makeParams(testCase.slots, plan, 0.3f, 0.0f);
    setup.samplesPerBeat = samplesPerBeat;

    DtBlkFxEngine fresh;
    DtBlkFxEngine used;
    if (!check(fresh.prepare(plan, rate) && used.prepare(plan, rate), "prepare failed"))
      return;

    Channels expected;
    if (!check(renderEngine(fresh, setup, input, expected, hostBlocks),
               std::string(testCase.name) + ": fresh render failed"))
      continue;

    // Run something different first (other FFT size, other effects including Smear), then reset.
    Slots other{};
    other[0] = Slot{15, 0.0f, 1.0f, 0.6f, 0.5f};
    other[1] = Slot{26, 0.0f, 1.0f, 0.6f, 0.5f};
    other[2] = Slot{2, 0.0f, 1.0f, 0.6f, 1.0f};
    RenderSetup otherSetup;
    otherSetup.params = makeParams(other, 16, 0.6f, 0.05f);
    otherSetup.samplesPerBeat = samplesPerBeat;
    Channels discarded;
    renderEngine(used, otherSetup, makeInput(Signal::NoiseFull, 2, totalSamples(16, 2000, 2), rate),
                 discarded, hostBlocks);

    used.reset();
    Channels afterReset;
    if (!check(renderEngine(used, setup, input, afterReset, hostBlocks),
               std::string(testCase.name) + ": render after reset failed"))
      continue;

    checkIdentical(expected, afterReset,
                   std::string(testCase.name) + ": reset() does not restore a fresh state");
  }
}

void testApiMisuse()
{
  const Params params = makeParams(Slots{}, 8, 0.0f, 0.0f);

  // An unprepared engine must refuse to work, not crash.
  {
    DtBlkFxEngine engine;
    std::array<float, 64> buffer{};
    float* channel = buffer.data();
    check(!engine.process(&channel, 1, 64), "unprepared engine processed audio");
    check(!engine.configureLegacyParameters(params, 22050.0, 0.0), "unprepared engine accepted parameters");
  }

  // Invalid prepare() arguments.
  {
    DtBlkFxEngine engine;
    check(!engine.prepare(-1, 44100.0), "prepare accepted FFT index -1");
    check(!engine.prepare(DtBlkFxEngine::fftSizeCount, 44100.0), "prepare accepted FFT index past the end");
    check(!engine.prepare(0, 0.0), "prepare accepted sample rate 0");
    check(!engine.prepare(0, -44100.0), "prepare accepted a negative sample rate");
    check(!engine.prepare(0, std::numeric_limits<double>::quiet_NaN()), "prepare accepted NaN sample rate");
    check(!engine.prepare(0, std::numeric_limits<double>::infinity()), "prepare accepted infinite sample rate");
    check(!engine.prepare(0, 1.0e9), "prepare accepted an absurd sample rate");
    check(!engine.isPrepared(), "engine reports prepared after failed prepare");
  }

  // Valid but unusual sample rates.
  for (const auto rate : {8000.0, 11025.0, 22050.0, 88200.0, 176400.0, 192000.0}) {
    DtBlkFxEngine engine;
    if (!check(engine.prepare(8, rate), "prepare failed at rate " + std::to_string(rate)))
      continue;
    std::vector<float> buffer(4096, 0.25f);
    float* channel = buffer.data();
    check(engine.configureLegacyParameters(params, rate * 0.5, 0.0) && engine.process(&channel, 1, 4096) &&
              allFinite(buffer),
          "rate " + std::to_string(rate) + ": processing failed or produced non-finite output");
  }

  // Invalid process() arguments on a prepared engine.
  {
    DtBlkFxEngine engine;
    if (!check(engine.prepare(8, 44100.0), "prepare failed"))
      return;
    check(engine.configureLegacyParameters(params, 22050.0, 0.0), "parameters rejected");
    std::array<float, 64> buffer{};
    std::array<float*, 3> pointers{buffer.data(), buffer.data(), buffer.data()};
    std::array<float*, 2> withNull{buffer.data(), nullptr};
    check(!engine.process(nullptr, 1, 64), "null channel array accepted");
    check(!engine.process(withNull.data(), 2, 64), "null channel pointer accepted");
    check(!engine.process(pointers.data(), 0, 64), "0 channels accepted");
    check(!engine.process(pointers.data(), 3, 64), "3 channels accepted");
    check(!engine.process(pointers.data(), 1, -1), "negative sample count accepted");
    check(engine.process(pointers.data(), 1, 0), "zero samples should succeed");
  }
}

// Random 8-slot configurations with random FFT size, rate, tempo, overlap, mix and chunking.
void testRandomisedConfigurations()
{
  constexpr int caseCount = 120;
  constexpr std::uint32_t seed = 20261105u;
  const std::array<int, 7> plans{0, 4, 8, 12, 16, 20, 24};
  const std::array<double, 3> rates{44100.0, 48000.0, 96000.0};
  const std::vector<std::vector<int>> patterns{{512}, {64}, {333}, {1, 3, 7, 129, 1024}};

  std::vector<std::unique_ptr<DtBlkFxEngine>> engines;
  for (const auto rate : rates) {
    engines.push_back(std::make_unique<DtBlkFxEngine>());
    if (!check(engines.back()->prepare(0, rate), "prepare failed at rate " + std::to_string(rate)))
      return;
  }

  Rng rng(seed);
  for (int index = 0; index < caseCount; ++index) {
    const auto rateIndex = static_cast<size_t>(rng.nextInt(3));
    const auto rate = rates[rateIndex];
    const auto channels = 1 + rng.nextInt(2);
    const auto plan = plans[static_cast<size_t>(rng.nextInt(7))];
    const auto samplesPerBeat = rate * 0.5 * (0.5 + static_cast<double>(rng.nextFloat()));

    Slots slots{};
    std::string types;
    for (auto& slot : slots) {
      slot.type = rng.nextInt(effectCount);
      slot.freqA = rng.nextFloat();
      slot.freqB = rng.nextFloat();
      slot.amplitude = rng.nextFloat() * 0.7f; // keep gains finite when 8 slots stack up
      slot.value = rng.nextFloat();
      types += std::to_string(slot.type) + " ";
    }
    const auto overlap =
        rng.nextFloat() < 0.5f ? rng.nextFloat() * 0.499f : 0.501f + rng.nextFloat() * 0.499f;
    const auto delay = rng.nextFloat() * 0.08f;
    const auto mix = rng.nextFloat();

    RenderSetup setup;
    setup.params = makeParams(slots, plan, overlap, delay, mix);
    setup.samplesPerBeat = samplesPerBeat;
    setup.parameterSync = rng.nextInt(2) == 0;
    setup.reconfigurePerBlock = rng.nextInt(2) == 0;

    const auto& pattern = patterns[static_cast<size_t>(rng.nextInt(4))];
    const auto total = totalSamples(plan, estimateDelaySamples(delay, samplesPerBeat), 3);
    const auto input = makeInput(Signal::NoiseHalf, channels, total, rate);

    const auto label = "random case " + std::to_string(index) + " (seed " + std::to_string(seed) +
                       ", rate " + std::to_string(static_cast<int>(rate)) + ", " +
                       std::to_string(channels) + " ch, FFT " +
                       std::to_string(DtBlkFxEngine::fftSizeAt(plan)) + ", effects " + types + ")";

    auto& engine = *engines[rateIndex];
    engine.reset();
    Channels output;
    if (!check(renderEngine(engine, setup, input, output, pattern), label + ": processing failed"))
      continue;
    for (const auto& channel : output)
      check(allFinite(channel), label + ": non-finite output");
  }
}
// A harmonic mask built from an off-bin fundamental must still cover the high harmonics. This
// fails when the fundamental is only found to whole bins, because the error grows with the
// harmonic number.
void testSubharmonicMaskCoversOffBinHarmonics()
{
  constexpr double rate = 44100.0;
  constexpr int plan = 8; // FFT 1024
  const auto fft = DtBlkFxEngine::fftSizeAt(plan);
  constexpr double fundamentalBin = 20.37;

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, rate), "prepare failed"))
    return;

  // Slot 0: AutoSubharm1Mask (x0.5), all harmonics, width 80%. Slot 1: a Filter with amplitude 0
  // over the whole range, which removes whatever the mask selects.
  Slots slots{};
  slots[0] = Slot{22, 0.0f, 1.0f, 0.6f, 0.2f};
  slots[1] = Slot{0, 0.0f, 1.0f, 0.0f, 0.5f};

  RenderSetup setup;
  setup.params = makeParams(slots, plan, 0.0f, 0.0f);
  setup.samplesPerBeat = rate * 0.5;

  const int total = totalSamples(plan, 100, 6);
  std::vector<float> tone(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    double sum = 0.0;
    for (int harmonic = 1; harmonic <= 16; ++harmonic)
      sum += (0.15 / harmonic) * std::sin(2.0 * pi * fundamentalBin * harmonic * i / fft);
    tone[static_cast<size_t>(i)] = static_cast<float>(sum);
  }
  const Channels input{tone};

  engine.reset();
  Channels output;
  if (!check(renderEngine(engine, setup, input, output, std::vector<int>{512}), "processing failed"))
    return;

  // Energy at the 14th harmonic, in the last frames, compared with the input.
  const auto energyAt = [&](const std::vector<float>& signal, double bin) {
    double sine = 0.0;
    double cosine = 0.0;
    const int start = total - 3 * fft;
    for (int i = start; i < total; ++i) {
      sine += signal[static_cast<size_t>(i)] * std::sin(2.0 * pi * bin * i / fft);
      cosine += signal[static_cast<size_t>(i)] * std::cos(2.0 * pi * bin * i / fft);
    }
    return std::sqrt(sine * sine + cosine * cosine);
  };
  for (const int harmonic : {10, 13, 14}) {
    const auto bin = fundamentalBin * harmonic;
    const auto before = energyAt(input[0], bin);
    const auto after = energyAt(output[0], bin);
    check(before > 0.0 && after < 0.2 * before,
          "harmonic " + std::to_string(harmonic) + " of an off-bin fundamental was not removed by the mask (" +
              std::to_string(after / std::max(before, 1.0e-12)) + " of its input energy is left)");
  }
}

// CrossMix must keep the signal level: the output power of the mixed range is the mix of the two
// input powers. (The engine used to take a second square root and had no power match, which made
// the output level depend on the signal level.) Type 30 uses the same code as a stand-in.
void testCrossMixKeepsLevel()
{
  constexpr double rate = 44100.0;
  constexpr int plan = 16; // FFT 4096
  constexpr int fft = 4096;
  constexpr int hop = fft - 16; // overlap 0
  constexpr int latency = fft - 1 + 100;
  const std::array<float, 5> values{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

  DtBlkFxEngine engine;
  if (!check(engine.prepare(plan, rate), "prepare failed"))
    return;

  for (const int type : {29, 30}) {
    for (const float scale : {1.0f, 0.05f}) {
      for (const float value : values) {
        Slots slots{};
        slots[0] = Slot{type, 0.0f, 1.0f, 0.6f, value}; // 0 dB, the whole frequency range

        RenderSetup setup;
        setup.params = makeParams(slots, plan, 0.0f, 0.0f);
        setup.samplesPerBeat = rate * 0.5;

        const int total = totalSamples(plan, 100, 8);
        Channels input{makeSignal(Signal::NoiseFull, total, 11u, rate), makeSignal(Signal::NoiseHalf, total, 22u, rate)};
        for (auto& channel : input)
          for (auto& sample : channel)
            sample *= scale;

        engine.reset();
        Channels output;
        const auto label = "effect " + std::to_string(type) + ", value " + std::to_string(value) + ", noise scale " +
                           std::to_string(scale);
        if (!check(renderEngine(engine, setup, input, output, std::vector<int>{512}), label + ": processing failed"))
          continue;

        const auto power = [](const std::vector<float>& data, int from, int to) {
          double sum = 0.0;
          for (int i = from; i < to; ++i)
            sum += static_cast<double>(data[static_cast<size_t>(i)]) * data[static_cast<size_t>(i)];
          return sum / (to - from);
        };

        // Input power over the stretch that produced the measured output.
        const auto mix = value >= 0.5f ? (value - 0.5f) * 2.0f : value * 2.0f;
        const auto leftPower = power(input[0], 2 * hop, 6 * hop);
        const auto rightPower = power(input[1], 2 * hop, 6 * hop);
        const auto sourcePower = value >= 0.5f ? rightPower : leftPower;
        const auto destinationPower = value >= 0.5f ? leftPower : rightPower;
        const auto expected = (1.0 - mix) * sourcePower + mix * destinationPower;
        const auto measured = power(output[0], latency + 2 * hop, latency + 6 * hop);

        const auto errorDb = 10.0 * std::log10(measured / expected);
        check(std::abs(errorDb) < 0.5,
              label + ": output power is " + std::to_string(errorDb) + " dB away from the mix of the input powers");
        check(std::abs(power(output[0], latency + 2 * hop, latency + 6 * hop) -
                       power(output[1], latency + 2 * hop, latency + 6 * hop)) <= 1.0e-6 * std::max(measured, 1.0e-12),
              label + ": the two channels differ after the mix");
      }
    }
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
  runTest("effect matrix (effects x FFT sizes x rates x channels)", testEffectMatrix);
  runTest("sweep copy level independent of FFT size", testSweepCopyLevelIndependentOfFftSize);
  runTest("input signals", testInputSignals);
  runTest("block-size independence", testBlockSizeIndependence);
  runTest("reset reproduces fresh output", testResetReproducesFreshOutput);
  runTest("API misuse", testApiMisuse);
  runTest("randomised configurations", testRandomisedConfigurations);
  runTest("CrossMix keeps the signal level", testCrossMixKeepsLevel);
  runTest("subharmonic mask covers off-bin harmonics", testSubharmonicMaskCoversOffBinHarmonics);

  std::cout << (failureCount == 0 ? "All robustness tests passed" : "Robustness tests failed") << " ("
            << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}