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

// JUCE-linked state and parameter tests: saving and loading, damaged state, parameter
// resolution and automation through the real processor.

#include "DtBlkFxLegacyMapping.h"
#include "DtBlkFxProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using Processor = DtBlkFxAudioProcessor;

constexpr double testSampleRate = 44100.0;
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

void info(const std::string& message)
{
  std::cout << "  INFO: " << message << '\n';
}

bool nearlyEqual(float a, float b, float tolerance)
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
};

// Gives the tests access to AudioProcessor's static XML helper (it may be protected).
struct XmlAccess : Processor {
  static void toBinary(const juce::XmlElement& xml, juce::MemoryBlock& block)
  {
    copyXmlToBinary(xml, block);
  }
};

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

using StateEntries = std::vector<std::pair<juce::String, juce::String>>;

juce::MemoryBlock makeStateBlock(const StateEntries& entries, const juce::String& rootTag = "Parameters")
{
  juce::XmlElement root(rootTag);
  for (const auto& entry : entries) {
    auto* child = root.createNewChildElement("PARAM");
    child->setAttribute("id", entry.first);
    child->setAttribute("value", entry.second);
  }
  juce::MemoryBlock block;
  XmlAccess::toBinary(root, block);
  return block;
}

void loadState(Processor& processor, const juce::MemoryBlock& block)
{
  processor.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
}

float readRaw(Processor& processor, const juce::String& id)
{
  const auto* value = processor.parameters.getRawParameterValue(id);
  if (!check(value != nullptr, "missing parameter " + id.toStdString()))
    return -1.0f;
  return value->load();
}

void setPlain(Processor& processor, const juce::String& id, float plainValue)
{
  if (auto* parameter = processor.parameters.getParameter(id))
    parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
  else
    check(false, "missing parameter " + id.toStdString());
}

// Random normalised value for every parameter; amplitudes stay moderate so that eight stacked
// slots cannot overflow.
void randomiseParameters(Processor& processor, Rng& rng)
{
  for (auto* parameter : processor.getParameters())
    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
      auto value = rng.nextFloat();
      if (ranged->paramID.endsWith("_amp"))
        value *= 0.7f;
      ranged->setValueNotifyingHost(value);
    }
}

void prepare(Processor& processor)
{
  processor.setRateAndBufferSizeDetails(testSampleRate, testBlockSize);
  processor.prepareToPlay(testSampleRate, testBlockSize);
}

// Runs noise through processBlock; beforeBlock(blockIndex) may change parameters. Returns
// whether every output sample was finite.
template <typename BeforeBlock>
bool processBlocks(Processor& processor, int blocks, BeforeBlock&& beforeBlock)
{
  juce::AudioBuffer<float> buffer(2, testBlockSize);
  juce::MidiBuffer midi;
  Rng noise(99u);
  bool finite = true;
  for (int block = 0; block < blocks; ++block) {
    beforeBlock(block);
    for (int channel = 0; channel < 2; ++channel)
      for (int i = 0; i < testBlockSize; ++i)
        buffer.setSample(channel, i, noise.nextFloat() - 0.5f);
    processor.processBlock(buffer, midi);
    for (int channel = 0; channel < 2; ++channel)
      for (int i = 0; i < testBlockSize; ++i)
        if (!std::isfinite(buffer.getSample(channel, i)))
          finite = false;
  }
  return finite;
}

// After any state, every parameter must be in range and the engine block usable.
void checkProcessorIsSane(Processor& processor, const std::string& label)
{
  for (auto* parameter : processor.getParameters())
    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
      const auto normalised = ranged->getValue();
      check(std::isfinite(normalised) && normalised >= 0.0f && normalised <= 1.0f,
            label + ": parameter " + ranged->paramID.toStdString() + " is out of range");
      const auto raw = readRaw(processor, ranged->paramID);
      const auto range = processor.parameters.getParameterRange(ranged->paramID);
      check(std::isfinite(raw) && raw >= range.start - 1.0e-4f && raw <= range.end + 1.0e-4f,
            label + ": raw value of " + ranged->paramID.toStdString() + " is out of range");
    }

  const auto block = processor.getLegacyParameterBlock();
  for (size_t i = 0; i < block.size(); ++i)
    check(std::isfinite(block[i]) && block[i] >= 0.0f && block[i] <= 1.0f,
          label + ": legacy value " + std::to_string(i) + " is out of 0..1");

  prepare(processor);
  check(processBlocks(processor, 3, [](int) {}), label + ": non-finite audio after loading the state");
  processor.releaseResources();
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

void testStateFormat()
{
  Processor processor;
  loadState(processor, makeStateBlock({{"mixback", "0.25"}, {"fx3_freq_a", "0.4"}}));
  check(nearlyEqual(readRaw(processor, "mixback"), 0.25f, 1.0e-3f),
        "a hand-built state did not set mixback (state layout assumption is wrong)");
  check(nearlyEqual(readRaw(processor, "fx3_freq_a"), 0.4f, 1.0e-3f),
        "a hand-built state did not set fx3_freq_a");

  // A state that lacks a parameter: report what happens (informational).
  Processor other;
  setPlain(other, "delay", 0.9f);
  loadState(other, makeStateBlock({{"mixback", "0.25"}}));
  info(std::string("a parameter missing from a loaded state ") +
       (nearlyEqual(readRaw(other, "delay"), 0.0627451f, 1.0e-3f) ? "returns to its default"
                                                                   : "keeps its previous value"));
}

void testRoundTrip()
{
  Rng rng(4242u);
  for (int round = 0; round < 5; ++round) {
    Processor source;
    randomiseParameters(source, rng);

    juce::MemoryBlock block;
    source.getStateInformation(block);
    if (!check(block.getSize() > 0, "saved state is empty"))
      continue;

    Processor target;
    loadState(target, block);

    int compared = 0;
    for (auto* parameter : source.getParameters())
      if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
        const auto a = readRaw(source, ranged->paramID);
        const auto b = readRaw(target, ranged->paramID);
        check(nearlyEqual(a, b, 1.0e-6f),
              "round " + std::to_string(round) + ": " + ranged->paramID.toStdString() + " is " +
                  std::to_string(b) + " after loading, was " + std::to_string(a));
        ++compared;
      }
    check(compared == 48, "expected 48 parameters, compared " + std::to_string(compared));
    check(source.getLegacyParameterBlock() == target.getLegacyParameterBlock(),
          "round " + std::to_string(round) + ": the engine block differs after a round trip");
  }

  // Fresh processors must save identical states and reload to the same engine block.
  Processor first;
  Processor second;
  juce::MemoryBlock firstBlock;
  juce::MemoryBlock secondBlock;
  first.getStateInformation(firstBlock);
  second.getStateInformation(secondBlock);
  check(firstBlock == secondBlock, "two fresh processors saved different states");
  Processor reloaded;
  setPlain(reloaded, "mixback", 0.1f);
  loadState(reloaded, firstBlock);
  check(reloaded.getLegacyParameterBlock() == first.getLegacyParameterBlock(),
        "loading the default state did not restore the defaults");
}

void testDamagedState()
{
  Rng rng(31337u);
  struct NamedState {
    std::string name;
    juce::MemoryBlock block;
  };
  std::vector<NamedState> states;

  states.push_back({"empty", juce::MemoryBlock()});
  {
    juce::MemoryBlock three;
    three.append("abc", 3);
    states.push_back({"3 bytes", three});
  }
  {
    juce::MemoryBlock noise(256, false);
    for (size_t i = 0; i < noise.getSize(); ++i)
      noise[i] = static_cast<char>(rng.nextBits() >> 16);
    states.push_back({"256 random bytes", noise});
  }
  {
    Processor source;
    juce::MemoryBlock valid;
    source.getStateInformation(valid);
    juce::MemoryBlock truncated;
    truncated.append(valid.getData(), valid.getSize() / 2);
    states.push_back({"truncated valid state", truncated});
  }
  states.push_back({"wrong root tag", makeStateBlock({{"mixback", "0.3"}}, "Other")});
  states.push_back({"out-of-range values",
                    makeStateBlock({{"mixback", "5.0"},
                                    {"delay", "-3.0"},
                                    {"fft_len", "1e30"},
                                    {"fx1_type", "99"},
                                    {"fx1_freq_a", "-1e30"},
                                    {"power_match", "7"},
                                    {"overlap", "-0.5"}})});
  states.push_back({"non-numeric values",
                    makeStateBlock({{"mixback", "abc"}, {"delay", "nan"}, {"fft_len", "inf"}, {"overlap", ""}})});
  states.push_back({"unknown and empty ids",
                    makeStateBlock({{"does_not_exist", "1.0"}, {"", "1.0"}, {"fx9_type", "3"}})});
  states.push_back({"duplicate entries", makeStateBlock({{"mixback", "0.2"}, {"mixback", "0.9"}})});
  {
    StateEntries many;
    const std::array<const char*, 4> ids{"mixback", "delay", "fx1_freq_a", "fx2_amp"};
    for (int i = 0; i < 5000; ++i)
      many.emplace_back(ids[static_cast<size_t>(i) % ids.size()], juce::String(rng.nextFloat()));
    states.push_back({"5000 entries", makeStateBlock(many)});
  }
  {
    const juce::String longId = juce::String::repeatedString("x", 10000);
    states.push_back({"10000-character id", makeStateBlock({{longId, "1.0"}})});
  }

  for (const auto& state : states) {
    Processor processor;
    loadState(processor, state.block);
    checkProcessorIsSane(processor, state.name);
  }
  std::cout << "  " << states.size() << " damaged states\n";
}

void testParameterResolution()
{
  // Half of a 0.0001 step plus float rounding. A 0.001 step (12.8 cents) fails this.
  constexpr float tolerance = 6.0e-5f;

  Processor processor;
  int checkedParameters = 0;
  float worstOverall = 0.0f;
  for (auto* parameter : processor.getParameters()) {
    auto* floatParameter = dynamic_cast<juce::AudioParameterFloat*>(parameter);
    if (floatParameter == nullptr)
      continue;

    float worst = 0.0f;
    for (int k = 0; k <= 993; ++k) {
      const auto requested = static_cast<float>(k) * 0.001007f; // deliberately off the grid
      setPlain(processor, floatParameter->paramID, requested);
      worst = std::max(worst, std::abs(readRaw(processor, floatParameter->paramID) - requested));
    }
    worstOverall = std::max(worstOverall, worst);
    check(worst <= tolerance,
          "parameter " + floatParameter->paramID.toStdString() + " resolves values only to within " +
              std::to_string(worst) + " (parameter interval too coarse for legacy presets)");
    ++checkedParameters;
  }
  // 4 global floats (mixback, delay, fft_len, overlap) + 8 slots x 4 floats (freq_a, freq_b, amp, value)
  check(checkedParameters == 36, "expected 36 float parameters, found " + std::to_string(checkedParameters));
  info("worst resolution error " + std::to_string(worstOverall) + " = " +
       std::to_string(worstOverall * 127.5f * 100.0f) + " cents on a frequency control");

  // Real values from the legacy preset files.
  const std::array<float, 18> presetValues{0.0627451f, 0.282353f, 0.295625f, 0.635f,   0.67451f,  0.2355f,
                                           0.885f,     0.6038f,   0.392157f, 0.37647f, 0.97727f,  0.53409f,
                                           0.65875f,   0.92614f,  0.06275f,  0.4549f,  0.5735f,   0.8125f};
  for (const auto value : presetValues) {
    setPlain(processor, "fx1_freq_a", value);
    check(nearlyEqual(readRaw(processor, "fx1_freq_a"), value, tolerance),
          "legacy preset value " + std::to_string(value) + " became " +
              std::to_string(readRaw(processor, "fx1_freq_a")));
  }
}

void testStateChangeWhilePlaying()
{
  Rng rng(555u);
  Processor processor;
  prepare(processor);
  check(processBlocks(processor, 8, [](int) {}), "non-finite audio before loading a state");

  Processor donor;
  randomiseParameters(donor, rng);
  juce::MemoryBlock randomState;
  donor.getStateInformation(randomState);
  loadState(processor, randomState);
  check(processBlocks(processor, 40, [](int) {}), "non-finite audio after loading a random state");

  Processor defaults;
  juce::MemoryBlock defaultState;
  defaults.getStateInformation(defaultState);
  loadState(processor, defaultState);
  check(processBlocks(processor, 8, [](int) {}), "non-finite audio after loading the default state");

  check(std::isfinite(processor.getTailLengthSeconds()), "tail length is not finite");
  processor.releaseResources();
}

void testAutomationStorm()
{
  Rng rng(2026u);
  Processor processor;
  prepare(processor);
  const auto finite = processBlocks(processor, 400, [&](int) { randomiseParameters(processor, rng); });
  check(finite, "randomised parameters before every block produced non-finite audio");
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

  runTest("state format", testStateFormat);
  runTest("save and load round trip", testRoundTrip);
  runTest("damaged state", testDamagedState);
  runTest("parameter resolution", testParameterResolution);
  runTest("state change while playing", testStateChangeWhilePlaying);
  runTest("automation storm through the processor", testAutomationStorm);

  std::cout << (failureCount == 0 ? "All state tests passed" : "State tests failed") << " ("
            << failureCount << " failed checks)" << std::endl;
  return failureCount == 0 ? 0 : 1;
}
