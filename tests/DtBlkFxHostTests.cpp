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

// JUCE-linked host integration tests: a fake playhead drives the real processor.
// Written against the playhead interface of JUCE 9 (getPosition and PositionInfo).

#include "DtBlkFxProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Processor = DtBlkFxAudioProcessor;
using Position = juce::AudioPlayHead::PositionInfo;

constexpr double testSampleRate = 48000.0;
constexpr int testBlockSize = 512;

int failureCount = 0;

bool check(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << "  FAIL: " << message << '\n';
    ++failureCount;
  }
  return condition;
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

// What a fake host reports for one block. An empty field means the host does not provide it.
struct HostPosition {
  std::optional<double> bpm;
  std::optional<double> ppq;
  bool playing = true;
};

// Gives the position for a block; no value means the host cannot provide a position at all.
using PositionScript = std::function<std::optional<HostPosition>(int)>;

struct FakePlayHead : juce::AudioPlayHead {
  std::optional<HostPosition> current;

  juce::Optional<Position> getPosition() const override
  {
    if (!current.has_value())
      return {};

    Position info;
    if (current->bpm.has_value())
      info.setBpm(juce::makeOptional(*current->bpm));
    if (current->ppq.has_value())
      info.setPpqPosition(juce::makeOptional(*current->ppq));
    info.setIsPlaying(current->playing);
    return juce::makeOptional(info);
  }
};

HostPosition host(std::optional<double> bpm, std::optional<double> ppq, bool playing = true)
{
  HostPosition position;
  position.bpm = bpm;
  position.ppq = ppq;
  position.playing = playing;
  return position;
}

void setPlain(Processor& processor, const juce::String& id, float plainValue)
{
  if (auto* parameter = processor.parameters.getParameter(id))
    parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
  else
    check(false, "missing parameter " + id.toStdString());
}

// FFT 1024, block sync on (overlap 0.75 in the engine), fully wet, a Shift and a Filter.
void configureBlockSync(Processor& processor)
{
  setPlain(processor, "fft_len", 40.0f / 255.0f);
  setPlain(processor, "overlap", 0.5f);
  setPlain(processor, "block_sync", 1.0f);
  setPlain(processor, "delay", 0.0f);
  setPlain(processor, "mixback", 0.0f);

  setPlain(processor, "fx1_type", 7.0f);
  setPlain(processor, "fx1_freq_a", 0.2f);
  setPlain(processor, "fx1_freq_b", 0.7f);
  setPlain(processor, "fx1_amp", 0.6f);
  setPlain(processor, "fx1_value", 0.6f);

  setPlain(processor, "fx2_type", 0.0f);
  setPlain(processor, "fx2_freq_a", 0.1f);
  setPlain(processor, "fx2_freq_b", 0.6f);
  setPlain(processor, "fx2_amp", 0.3f);
}

struct RenderOutput {
  bool finite = true;
  std::vector<float> left;
};

// Renders noise block by block. The script gives the playhead position for each block; an empty
// script means the processor has no playhead at all.
RenderOutput render(Processor& processor, double sampleRate, int blocks, const PositionScript& script)
{
  FakePlayHead playHead;
  processor.setPlayHead(script ? &playHead : nullptr);
  processor.setRateAndBufferSizeDetails(sampleRate, testBlockSize);
  processor.prepareToPlay(sampleRate, testBlockSize);

  juce::AudioBuffer<float> buffer(2, testBlockSize);
  juce::MidiBuffer midi;
  Rng noise(4321u);
  RenderOutput result;
  for (int block = 0; block < blocks; ++block) {
    if (script)
      playHead.current = script(block);

    for (int channel = 0; channel < 2; ++channel)
      for (int i = 0; i < testBlockSize; ++i)
        buffer.setSample(channel, i, noise.nextFloat() - 0.5f);
    processor.processBlock(buffer, midi);

    for (int i = 0; i < testBlockSize; ++i) {
      const auto sample = buffer.getSample(0, i);
      if (!std::isfinite(sample) || !std::isfinite(buffer.getSample(1, i)))
        result.finite = false;
      result.left.push_back(sample);
    }
  }
  processor.releaseResources();
  processor.setPlayHead(nullptr);
  return result;
}

// A host that reports tempo and a beat position that advances with the audio.
PositionScript gridScript(double bpm, double offsetSamples)
{
  const auto samplesPerBeat = testSampleRate * 60.0 / bpm;
  return [=](int block) -> std::optional<HostPosition> {
    const auto ppq = (static_cast<double>(block) * testBlockSize + offsetSamples) / samplesPerBeat;
    return host(bpm, ppq, true);
  };
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

void testPlayHeadVariants()
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  std::vector<std::pair<std::string, PositionScript>> cases;
  cases.emplace_back("no playhead", PositionScript{});
  cases.emplace_back("position not available",
                     [](int) -> std::optional<HostPosition> { return std::nullopt; });
  cases.emplace_back("host reports a position but fills nothing",
                     [](int) -> std::optional<HostPosition> { return host(std::nullopt, std::nullopt); });
  cases.emplace_back("tempo only",
                     [](int) -> std::optional<HostPosition> { return host(140.0, std::nullopt); });
  cases.emplace_back("beat position only", [](int block) -> std::optional<HostPosition> {
    return host(std::nullopt, 0.1 * block);
  });
  cases.emplace_back("NaN tempo", [=](int) -> std::optional<HostPosition> { return host(nan, 1.0); });
  cases.emplace_back("infinite tempo", [=](int) -> std::optional<HostPosition> { return host(inf, 1.0); });
  cases.emplace_back("zero tempo", [](int) -> std::optional<HostPosition> { return host(0.0, 1.0); });
  cases.emplace_back("negative tempo", [](int) -> std::optional<HostPosition> { return host(-120.0, 1.0); });
  cases.emplace_back("huge tempo", [](int) -> std::optional<HostPosition> { return host(1.0e9, 1.0); });
  cases.emplace_back("NaN beat position", [=](int) -> std::optional<HostPosition> { return host(120.0, nan); });
  cases.emplace_back("huge beat position",
                     [](int) -> std::optional<HostPosition> { return host(120.0, 1.0e15); });
  cases.emplace_back("stopped, position does not move",
                     [](int) -> std::optional<HostPosition> { return host(120.0, 3.5, false); });
  cases.emplace_back("random jumps", [](int block) -> std::optional<HostPosition> {
    Rng rng(static_cast<std::uint32_t>(block) * 977u + 5u);
    const auto bpm = 60.0 + 140.0 * static_cast<double>(rng.nextFloat());
    const auto ppq = 500.0 * static_cast<double>(rng.nextFloat()) - 50.0;
    const auto playing = rng.nextFloat() < 0.5f;
    return host(bpm, ppq, playing);
  });

  for (const auto& entry : cases) {
    Processor processor;
    configureBlockSync(processor);
    const auto output = render(processor, testSampleRate, 40, entry.second);
    check(output.finite, "playhead variant '" + entry.first + "' produced non-finite audio");
  }
  std::cout << "  " << cases.size() << " playhead variants\n";
}

void testHostPositionAndTempoReachTheEngine()
{
  const auto run = [](double bpm, double offsetSamples) {
    Processor processor;
    configureBlockSync(processor);
    return render(processor, testSampleRate, 24, gridScript(bpm, offsetSamples));
  };

  const auto a = run(720.0, 0.0);
  const auto b = run(720.0, 0.0);
  const auto shifted = run(720.0, 2000.0);
  const auto slower = run(600.0, 0.0);

  check(a.finite && b.finite && shifted.finite && slower.finite, "non-finite audio with a host grid");
  check(a.left == b.left, "the same host transport should render identically");
  check(a.left != shifted.left,
        "moving the host beat position by half a beat did not change the output (the position does not reach the engine)");
  check(a.left != slower.left,
        "changing the host tempo from 720 to 600 BPM did not change the output (the tempo does not reach the engine)");
}

void testLatencyAndTail()
{
  Processor processor;
  processor.setRateAndBufferSizeDetails(44100.0, testBlockSize);
  processor.prepareToPlay(44100.0, testBlockSize);

  check(processor.getLatencySamples() == 4095,
        "unexpected latency " + std::to_string(processor.getLatencySamples()));
  const auto expectedTail = 6.0 + 80639.0 / 44100.0;
  check(processor.getTailLengthSeconds() >= expectedTail - 1.0e-6,
        "tail length " + std::to_string(processor.getTailLengthSeconds()) +
            " does not cover the largest FFT (expected at least " + std::to_string(expectedTail) + ")");

  processor.releaseResources();
  check(processor.getLatencySamples() == 0, "latency should be 0 after releaseResources");
}

void testSampleRates()
{
  // The reported latency is the Latency parameter's value (default 4096 samples -> 4095) and no
  // longer follows the Block Size, which configureBlockSync() sets to the 1024-point FFT.
  constexpr int expectedLatency = 4095;

  for (const auto rate : {8000.0, 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
    Processor processor;
    configureBlockSync(processor);
    const auto output = render(processor, rate, 6, gridScript(120.0, 0.0));
    check(output.finite, "rate " + std::to_string(rate) + ": non-finite audio");

    processor.setRateAndBufferSizeDetails(rate, testBlockSize);
    processor.prepareToPlay(rate, testBlockSize);
    check(processor.getLatencySamples() == expectedLatency,
          "rate " + std::to_string(rate) + ": unexpected latency " + std::to_string(processor.getLatencySamples()) +
              " (expected " + std::to_string(expectedLatency) + ")");

    const auto expectedTail = 6.0 + 80639.0 / rate;
    check(std::isfinite(processor.getTailLengthSeconds()) &&
              processor.getTailLengthSeconds() >= expectedTail - 1.0e-6,
          "rate " + std::to_string(rate) + ": tail length " + std::to_string(processor.getTailLengthSeconds()) +
              " does not cover the largest FFT (expected at least " + std::to_string(expectedTail) + ")");
    processor.releaseResources();
  }
}

// Every choice of the Latency parameter is reported to the host, and Block Size does not change it.
void testLatencyChoices()
{
  const auto count = Processor::latencyChoiceNames().size();
  for (int choice = 0; choice < count; ++choice) {
    Processor processor;
    setPlain(processor, "latency", static_cast<float>(choice));
    setPlain(processor, "fft_len", 0.9f); // a large Block Size
    processor.setRateAndBufferSizeDetails(testSampleRate, testBlockSize);
    processor.prepareToPlay(testSampleRate, testBlockSize);

    const auto expected = Processor::latencySamplesForChoice(choice);
    check(processor.getLatencySamples() == expected,
          "latency choice " + std::to_string(choice) + ": reported " + std::to_string(processor.getLatencySamples()) +
              ", expected " + std::to_string(expected));

    // Moving Block Size while playing must not change the reported latency.
    FakePlayHead playHead;
    processor.setPlayHead(&playHead);
    juce::AudioBuffer<float> buffer(2, testBlockSize);
    juce::MidiBuffer midi;
    buffer.clear();
    for (const auto size : {0.1f, 0.9f, 0.3f, 0.7f}) {
      setPlain(processor, "fft_len", size);
      processor.processBlock(buffer, midi);
      check(processor.getLatencySamples() == expected,
            "latency choice " + std::to_string(choice) + ": Block Size " + std::to_string(size) +
                " changed the reported latency to " + std::to_string(processor.getLatencySamples()));
    }
    processor.setPlayHead(nullptr);
    processor.releaseResources();
  }
}

// Changing the Latency parameter while playing updates the reported latency, and changing the
// Delay parameter does not.
void testLatencyFollowsOnlyTheLatencyParameter()
{
  Processor processor;
  processor.setRateAndBufferSizeDetails(testSampleRate, testBlockSize);
  processor.prepareToPlay(testSampleRate, testBlockSize);
  check(processor.getLatencySamples() == 4095, "default latency should be 4095");

  juce::AudioBuffer<float> buffer(2, testBlockSize);
  juce::MidiBuffer midi;
  buffer.clear();

  setPlain(processor, "delay", 0.9f);
  processor.processBlock(buffer, midi);
  check(processor.getLatencySamples() == 4095, "changing Delay changed the reported latency");

  setPlain(processor, "latency", 7.0f); // 16384 samples
  processor.processBlock(buffer, midi);
  check(processor.getLatencySamples() == Processor::latencySamplesForChoice(7),
        "changing Latency while playing did not update the reported latency (" +
            std::to_string(processor.getLatencySamples()) + ")");

  setPlain(processor, "latency", 0.0f); // 256 samples
  processor.processBlock(buffer, midi);
  check(processor.getLatencySamples() == Processor::latencySamplesForChoice(0),
        "lowering Latency while playing did not update the reported latency");

  processor.releaseResources();
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

  runTest("playhead variants", testPlayHeadVariants);
  runTest("host position and tempo reach the engine", testHostPositionAndTempoReachTheEngine);
  runTest("latency and tail length", testLatencyAndTail);
  runTest("sample rates", testSampleRates);
  runTest("latency choices", testLatencyChoices);
  runTest("latency follows only the Latency parameter", testLatencyFollowsOnlyTheLatencyParameter);


  std::cout << (failureCount == 0 ? "All host tests passed" : "Host tests failed") << " ("
            << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}
