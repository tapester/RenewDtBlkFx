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

// JUCE-linked tests: they exercise the real processor, so they cover the path from host
// parameter value to engine behaviour that the engine-only tests cannot reach.

#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"
#include "DtBlkFxProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Processor = DtBlkFxAudioProcessor;

constexpr double testSampleRate = 44100.0;
constexpr int testBlockSize = 512;
constexpr int renderSamples = testBlockSize * 32; // 16384 samples
constexpr int slotCount = 8;
// The engine clamps the delay to at least 100 samples, so delay parameter 0 gives 100.
constexpr int minimumDelaySamples = 100;

int failureCount = 0;

bool check(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << "  FAIL: " << message << '\n';
    ++failureCount;
  }
  return condition;
}

bool nearlyEqual(float a, float b, float tolerance)
{
  return std::abs(a - b) <= tolerance;
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

void setPlain(Processor& processor, const juce::String& id, float plainValue)
{
  auto* parameter = processor.parameters.getParameter(id);
  if (!check(parameter != nullptr, "missing parameter " + id.toStdString()))
    return;
  // setValueNotifyingHost takes a normalised 0..1 value; convert from the plain value first.
  parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
}

float readRaw(Processor& processor, const juce::String& id)
{
  const auto* value = processor.parameters.getRawParameterValue(id);
  if (!check(value != nullptr, "missing parameter " + id.toStdString()))
    return -1.0f;
  return value->load();
}

void setEffectSlot(Processor& processor,
                   int slot,
                   int type,
                   float frequencyA,
                   float frequencyB,
                   float amplitude,
                   float value)
{
  setPlain(processor, Processor::fxParameterID(slot, "type"), static_cast<float>(type));
  setPlain(processor, Processor::fxParameterID(slot, "freq_a"), frequencyA);
  setPlain(processor, Processor::fxParameterID(slot, "freq_b"), frequencyB);
  setPlain(processor, Processor::fxParameterID(slot, "amp"), amplitude);
  setPlain(processor, Processor::fxParameterID(slot, "value"), value);
}

std::unique_ptr<Processor> makeProcessor(int channels)
{
  auto processor = std::make_unique<Processor>();
  if (channels != 2) {
    const auto channelSet =
        channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    auto layout = processor->getBusesLayout();
    layout.inputBuses.set(0, channelSet);
    layout.outputBuses.set(0, channelSet);
    if (!processor->setBusesLayout(layout))
      return nullptr;
  }

  // Make sure the layout really changed, so a silent no-op cannot pass as a mono test.
  if (processor->getTotalNumInputChannels() != channels ||
      processor->getTotalNumOutputChannels() != channels)
    return nullptr;
  return processor;
}


std::vector<float> makeNoise(int count, std::uint32_t seed)
{
  std::vector<float> data(static_cast<size_t>(count));
  auto state = seed;
  for (auto& sample : data) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<float>(state >> 8) / 16777216.0f - 0.5f; // -0.5 .. 0.5
  }
  return data;
}

struct RenderResult {
  bool ok = false;
  int latency = 0;
  std::vector<std::vector<float>> input;
  std::vector<std::vector<float>> output;
};

// Run noise through processBlock in host-sized blocks, like a DAW would.
RenderResult render(Processor& processor, int channels)
{
  RenderResult result;
  processor.setRateAndBufferSizeDetails(testSampleRate, testBlockSize);
  processor.prepareToPlay(testSampleRate, testBlockSize);

  for (int channel = 0; channel < channels; ++channel) {
    result.input.push_back(makeNoise(renderSamples, 1234u + static_cast<std::uint32_t>(channel) * 77u));
    result.output.emplace_back(static_cast<size_t>(renderSamples), 0.0f);
  }

  juce::AudioBuffer<float> buffer(channels, testBlockSize);
  juce::MidiBuffer midi;
  for (int position = 0; position < renderSamples; position += testBlockSize) {
    for (int channel = 0; channel < channels; ++channel)
      buffer.copyFrom(channel,
                      0,
                      result.input[static_cast<size_t>(channel)].data() + position,
                      testBlockSize);
    processor.processBlock(buffer, midi);
    for (int channel = 0; channel < channels; ++channel)
      std::copy(buffer.getReadPointer(channel),
                buffer.getReadPointer(channel) + testBlockSize,
                result.output[static_cast<size_t>(channel)].begin() + position);
  }

  result.latency = processor.getLatencySamples();
  processor.releaseResources();
  result.ok = true;
  return result;
}

struct LagMatch {
  int lag = -1;
  float error = 1.0e9f;
};

// Find the delay (in samples) at which output best reproduces input, and the worst-case error there.
LagMatch findBestLag(const std::vector<float>& input,
                     const std::vector<float>& output,
                     int minLag,
                     int maxLag,
                     int compareFrom)
{
  LagMatch best;
  const auto count = static_cast<int>(output.size());
  for (int lag = minLag; lag <= maxLag; ++lag) {
    float error = 0.0f;
    for (int n = compareFrom; n < count; ++n)
      error = std::max(error,
                       std::abs(output[static_cast<size_t>(n)] - input[static_cast<size_t>(n - lag)]));
    if (error < best.error) {
      best.lag = lag;
      best.error = error;
    }
  }
  return best;
}

float maxAbs(const std::vector<float>& data, int from = 0)
{
  float result = 0.0f;
  for (size_t i = static_cast<size_t>(from); i < data.size(); ++i)
    result = std::max(result, std::abs(data[i]));
  return result;
}

void checkDelayedCopy(const RenderResult& result, const std::string& label)
{
  if (!check(result.ok, label + ": render failed"))
    return;

  const auto expectedLag = result.latency + minimumDelaySamples;
  const auto maxLag = expectedLag + 4;
  const auto compareFrom = maxLag + 256;

  for (size_t channel = 0; channel < result.output.size(); ++channel) {
    const auto match =
        findBestLag(result.input[channel], result.output[channel], result.latency, maxLag, compareFrom);
    const auto suffix = " (channel " + std::to_string(channel) + ", best lag " +
                        std::to_string(match.lag) + ", expected " + std::to_string(expectedLag) +
                        ", error " + std::to_string(match.error) + ")";
    check(match.error < 1.0e-3f, label + ": output is not a delayed copy of the input" + suffix);
    check(std::abs(match.lag - expectedLag) <= 1, label + ": unexpected total delay" + suffix);
    check(maxAbs(result.output[channel], compareFrom) > 0.2f,
          label + ": output is unexpectedly quiet" + suffix);
  }
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

void testParameterLayout()
{
  Processor processor;

  const std::array<const char*, 7> globalIds{
      "mixback", "power_match", "delay", "fft_len", "overlap", "block_sync", "parameter_sync"};
  for (const auto* id : globalIds)
    check(processor.parameters.getParameter(id) != nullptr, std::string("missing parameter ") + id);

  const std::array<const char*, 5> slotSuffixes{"freq_a", "freq_b", "amp", "type", "value"};
  for (int slot = 0; slot < slotCount; ++slot)
    for (const auto* suffix : slotSuffixes) {
      const auto id = Processor::fxParameterID(slot, suffix);
      check(processor.parameters.getParameter(id) != nullptr,
            "missing parameter " + id.toStdString());
    }

  check(processor.getParameters().size() == 7 + slotCount * 5,
        "unexpected parameter count " + std::to_string(processor.getParameters().size()));

  const auto& names = Processor::effectTypeNames();
  check(names.size() == DtBlkFxLegacy::effectTypeCount,
        "effect name count differs from DtBlkFxLegacy::effectTypeCount");

  auto* choice =
      dynamic_cast<juce::AudioParameterChoice*>(processor.parameters.getParameter("fx1_type"));
  if (check(choice != nullptr, "fx1_type is not a choice parameter"))
    check(choice->choices.size() == DtBlkFxLegacy::effectTypeCount,
          "fx1_type choice count differs from DtBlkFxLegacy::effectTypeCount");

  // The engine interprets these index ranges (masks 20-25, stereo effects 26-30), so the
  // names must stay at these positions.
  if (names.size() == DtBlkFxLegacy::effectTypeCount) {
    check(names[0] == "Filter", "index 0 should be Filter");
    check(names[9] == "None", "index 9 should be None");
    check(names[20] == "HarmMask", "index 20 should be HarmMask");
    check(names[25] == "ThreshMask", "index 25 should be ThreshMask");
    check(names[26] == "Vocode", "index 26 should be Vocode");
    check(names[30] == "WarpMix", "index 30 should be WarpMix");
  }
}

void testEveryEffectTypeInEverySlot()
{
  Processor processor;
  for (int slot = 0; slot < slotCount; ++slot) {
    const auto typeId = Processor::fxParameterID(slot, "type");
    for (int index = 0; index < DtBlkFxLegacy::effectTypeCount; ++index) {
      setPlain(processor, typeId, static_cast<float>(index));

      // For a choice parameter the raw value is the choice index, not a 0..1 number.
      check(nearlyEqual(readRaw(processor, typeId), static_cast<float>(index), 1.0e-3f),
            "raw value of " + typeId.toStdString() + " is not the choice index " +
                std::to_string(index));

      const auto block = processor.getLegacyParameterBlock();
      const auto decoded =
          DtBlkFxLegacy::legacyValueToEffectType(block[static_cast<size_t>(4 + slot * 5 + 3)]);
      check(decoded == index,
            "slot " + std::to_string(slot + 1) + ": effect index " + std::to_string(index) +
                " reached the engine as " + std::to_string(decoded));
    }
  }
}

void testSlotOffsets()
{
  Processor processor;
  for (int slot = 0; slot < slotCount; ++slot) {
    const auto step = 0.01f * static_cast<float>(slot);
    setEffectSlot(processor, slot, (slot * 4 + 3) % DtBlkFxLegacy::effectTypeCount,
                  0.10f + step, 0.50f + step, 0.20f + step, 0.30f + step);
  }

  const auto block = processor.getLegacyParameterBlock();
  constexpr float tolerance = 0.0011f; // parameters are quantised to 0.001 steps
  for (int slot = 0; slot < slotCount; ++slot) {
    const auto step = 0.01f * static_cast<float>(slot);
    const auto offset = static_cast<size_t>(4 + slot * 5);
    const auto name = "slot " + std::to_string(slot + 1);
    check(nearlyEqual(block[offset], 0.10f + step, tolerance), name + ": frequency A misplaced");
    check(nearlyEqual(block[offset + 1], 0.50f + step, tolerance), name + ": frequency B misplaced");
    check(nearlyEqual(block[offset + 2], 0.20f + step, tolerance), name + ": amplitude misplaced");
    check(DtBlkFxLegacy::legacyValueToEffectType(block[offset + 3]) ==
              (slot * 4 + 3) % DtBlkFxLegacy::effectTypeCount,
          name + ": type misplaced");
    check(nearlyEqual(block[offset + 4], 0.30f + step, tolerance), name + ": value misplaced");
  }
}

void testGlobalParameterMapping()
{
  Processor processor;
  constexpr float tolerance = 0.0011f;

  setPlain(processor, "mixback", 0.4f);
  setPlain(processor, "delay", 0.25f);
  setPlain(processor, "fft_len", 0.5f);
  setPlain(processor, "overlap", 0.6f);
  setPlain(processor, "block_sync", 0.0f);

  setPlain(processor, "power_match", 1.0f);
  auto block = processor.getLegacyParameterBlock();
  check(nearlyEqual(block[0], 0.2f, tolerance), "power match on: mix back should map to 0.2");
  check(nearlyEqual(block[1], 0.25f, tolerance), "delay should pass through");
  check(nearlyEqual(block[2], 0.5f, tolerance), "FFT length should pass through");
  check(nearlyEqual(block[3], 0.3f, tolerance), "overlap 0.6 without sync should map to 0.3");

  setPlain(processor, "power_match", 0.0f);
  block = processor.getLegacyParameterBlock();
  check(nearlyEqual(block[0], 1.0f - 0.4f * 0.499f, tolerance),
        "power match off: mix back should map to 0.8004");

  setPlain(processor, "block_sync", 1.0f);
  block = processor.getLegacyParameterBlock();
  check(nearlyEqual(block[3], 0.7f, tolerance), "overlap 0.6 with sync should map to 0.7");

  // The engine reads "overlap > 0.5" as block sync, so the sync flag must decide the side.
  for (int step = 0; step <= 20; ++step) {
    const auto overlap = static_cast<float>(step) / 20.0f;
    setPlain(processor, "overlap", overlap);
    setPlain(processor, "block_sync", 0.0f);
    const auto free = processor.getLegacyParameterBlock()[3];
    setPlain(processor, "block_sync", 1.0f);
    const auto synced = processor.getLegacyParameterBlock()[3];
    check(free >= 0.0f && free <= 0.5f, "sync off: overlap " + std::to_string(overlap) + " mapped above 0.5");
    check(synced > 0.5f && synced <= 1.0f, "sync on: overlap " + std::to_string(overlap) + " mapped to 0.5 or below");
  }
}

void testEngineAcceptsProcessorBlock()
{
  Processor processor;
  setPlain(processor, "delay", 0.0f);
  const auto block = processor.getLegacyParameterBlock();

  constexpr int defaultPlanIndex = 16; // 4096 samples
  DtBlkFxEngine engine;
  if (!check(engine.prepare(defaultPlanIndex, testSampleRate), "engine failed to prepare"))
    return;

  check(engine.configureLegacyParameters(block, testSampleRate * 0.5, 0.0, false),
        "engine rejected the processor's parameter block");
  check(engine.getFftSize() == DtBlkFxEngine::fftSizeAt(defaultPlanIndex),
        "default FFT length does not select the expected size");
  check(engine.getDelaySamples() == minimumDelaySamples, "delay 0 should give the 100-sample minimum");
  check(nearlyEqual(engine.getLegacyOverlapParameter(), block[3], 1.0e-6f),
        "overlap did not reach the engine unchanged");
}

void testBusLayouts()
{
  Processor processor;
  const auto base = processor.getBusesLayout();
  const auto supported = [&](const juce::AudioChannelSet& in, const juce::AudioChannelSet& out) {
    auto layout = base;
    layout.inputBuses.set(0, in);
    layout.outputBuses.set(0, out);
    return processor.isBusesLayoutSupported(layout);
  };

  const auto mono = juce::AudioChannelSet::mono();
  const auto stereo = juce::AudioChannelSet::stereo();
  const auto surround = juce::AudioChannelSet::create5point1();
  check(supported(stereo, stereo), "stereo/stereo should be supported");
  check(supported(mono, mono), "mono/mono should be supported");
  check(!supported(mono, stereo), "mono in / stereo out should be rejected");
  check(!supported(surround, surround), "5.1 should be rejected");
}


void testReportedLatency()
{
  Processor processor;
  processor.setRateAndBufferSizeDetails(testSampleRate, testBlockSize);
  processor.prepareToPlay(testSampleRate, testBlockSize);

  // Default FFT length is 4096. Only the FFT latency is reported; the user delay (1 beat by
  // default) is not, which matches the legacy plugin.
  check(processor.getLatencySamples() == 4095,
        "unexpected latency " + std::to_string(processor.getLatencySamples()));
  check(processor.getTailLengthSeconds() > 6.0, "tail length should cover the 6 s maximum delay");

  processor.releaseResources();
  check(processor.getLatencySamples() == 0, "latency should be 0 after releaseResources");
}

void testWetAndDryPassThrough()
{
  for (const auto channels : {2, 1}) {
    for (const auto mixBack : {0.0f, 1.0f}) {
      const auto label = std::string(channels == 2 ? "stereo" : "mono") +
                         (mixBack == 0.0f ? " wet" : " dry") + " pass-through";
      auto processor = makeProcessor(channels);
      if (!check(processor != nullptr, label + ": could not set the bus layout"))
        continue;

      // Every effect slot defaults to "None", so the signal must come out unchanged apart from
      // the delay. With the old type mapping the "None" slots became WarpMix and this failed.
      setPlain(*processor, "mixback", mixBack);
      setPlain(*processor, "delay", 0.0f);
      checkDelayedCopy(render(*processor, channels), label);
    }
  }
}

void testFilterInEverySlot()
{
  for (int slot = 0; slot < slotCount; ++slot) {
    const auto label = "filter in slot " + std::to_string(slot + 1);

    {
      // Amplitude 0 across the full band removes everything (fully wet).
      Processor processor;
      setPlain(processor, "mixback", 0.0f);
      setPlain(processor, "delay", 0.0f);
      setEffectSlot(processor, slot, 0, 0.0f, 1.0f, 0.0f, 0.5f);
      const auto result = render(processor, 2);
      if (check(result.ok, label + ": render failed"))
        for (const auto& channel : result.output)
          check(maxAbs(channel) < 1.0e-4f, label + " with amplitude 0 should be silent");
    }

    {
      // Amplitude 0.6 is 0 dB, so the same filter must pass the signal through unchanged.
      Processor processor;
      setPlain(processor, "mixback", 0.0f);
      setPlain(processor, "delay", 0.0f);
      setEffectSlot(processor, slot, 0, 0.0f, 1.0f, 0.6f, 0.5f);
      checkDelayedCopy(render(processor, 2), label + " at 0 dB");
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
  juce::ScopedJuceInitialiser_GUI juceInitialiser;

  runTest("parameter layout", testParameterLayout);
  runTest("every effect type in every slot", testEveryEffectTypeInEverySlot);
  runTest("slot parameter offsets", testSlotOffsets);
  runTest("global parameter mapping", testGlobalParameterMapping);
  runTest("engine accepts processor block", testEngineAcceptsProcessorBlock);
  runTest("bus layouts", testBusLayouts);
  runTest("reported latency", testReportedLatency);
  runTest("wet and dry pass-through", testWetAndDryPassThrough);
  runTest("filter in every slot", testFilterInEverySlot);

  std::cout << (failureCount == 0 ? "All processor tests passed" : "Processor tests failed")
            << " (" << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}
