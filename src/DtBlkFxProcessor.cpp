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

#include "DtBlkFxProcessor.h"
#include "DtBlkFxEditor.h"
#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"

#include <cmath>

namespace {
std::unique_ptr<juce::AudioParameterFloat>
makeUnitParameter(const juce::String& id, const juce::String& name, float defaultValue)
{
  // 0.0001 keeps legacy preset values (up to 4 decimals) within about 0.6 cents.
  return std::make_unique<juce::AudioParameterFloat>(
      id, name, juce::NormalisableRange<float>{0.0f, 1.0f, 0.0001f}, defaultValue);
}

int getLegacyFftPlanIndex(float parameter)
{
  return juce::jlimit(0,
                      DtBlkFxEngine::fftSizeCount - 1,
                      static_cast<int>(parameter * 255.0f / 4.0f - 1.5f));
}

bool isOn(const std::atomic<float>* handle) noexcept
{
  return handle != nullptr && handle->load(std::memory_order_relaxed) >= 0.5f;
}

// Latency choices in samples of "block size covered" (the latency is one less). Chosen from the
// engine's FFT sizes; 4096 is the default and matches what earlier versions reported.
constexpr std::array<int, 19> latencyChoiceSizes{256,  512,  1024, 2048,  4096,  8192,  12288, 16384, 24576, 32768,
                                                 40500, 49152, 57600, 65536, 80640, 3072,  5120,  6144,  9600};
constexpr int defaultLatencyChoice = 4; // 4096

} // namespace

const juce::StringArray& DtBlkFxAudioProcessor::effectTypeNames()
{
  // Keep the choice order aligned with the legacy effect codes.
  static const juce::StringArray effectTypes{"Filter",
                                             "Contrast",
                                             "Smear",
                                             "Threshold",
                                             "Clip",
                                             "Resize",
                                             "Resample",
                                             "Shift",
                                             "ConstShift",
                                             "None",
                                             "None",
                                             "HarmShift",
                                             "HarmRepitch",
                                             "HarmFilt",
                                             "AutoHarm",
                                             "Triangles",
                                             "Squares",
                                             "Saws",
                                             "Pointy",
                                             "Sweep",
                                             "HarmMask",
                                             "AutoHarmMask",
                                             "AutoSubharm1Mask",
                                             "AutoSubharm2Mask",
                                             "AutoSubharm3Mask",
                                             "ThreshMask",
                                             "Vocode",
                                             "HarmMatch L>R",
                                             "HarmMatch R>L",
                                             "CrossMix",
                                             "WarpMix"};
  return effectTypes;
}

const juce::StringArray& DtBlkFxAudioProcessor::latencyChoiceNames()
{
  static const juce::StringArray names = [] {
    juce::StringArray result;
    for (const auto size : latencyChoiceSizes)
      result.add(juce::String(size) + " samples");
    return result;
  }();
  return names;
}

int DtBlkFxAudioProcessor::latencySamplesForChoice(int choiceIndex)
{
  const auto index = juce::jlimit(0, static_cast<int>(latencyChoiceSizes.size()) - 1, choiceIndex);
  // A block of N samples needs N - 1 samples of latency.
  return latencyChoiceSizes[static_cast<size_t>(index)] - 1;
}

DtBlkFxAudioProcessor::DtBlkFxAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#  if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
#  endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
                         )
    , parameters(*this, nullptr, "Parameters", createParameterLayout())
    , engine(std::make_unique<DtBlkFxEngine>())
{
  cacheParameterHandles();
}

DtBlkFxAudioProcessor::~DtBlkFxAudioProcessor() {}

const juce::String DtBlkFxAudioProcessor::getName() const
{
  return JucePlugin_Name;
}

bool DtBlkFxAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
  return true;
#else
  return false;
#endif
}

bool DtBlkFxAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
  return true;
#else
  return false;
#endif
}

bool DtBlkFxAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
  return true;
#else
  return false;
#endif
}

double DtBlkFxAudioProcessor::getTailLengthSeconds() const
{
  // Longest possible delay between input and output: the engine's 6 s delay limit plus the
  // latency of the largest FFT size (the latency can be raised to that while playing).
  constexpr double maximumDelaySeconds = 6.0;
  const auto maximumFrameLatency =
      currentSampleRate > 0.0
          ? static_cast<double>(DtBlkFxEngine::maxFftSize - 1) / currentSampleRate
          : 0.0;
  return maximumDelaySeconds + maximumFrameLatency;
}

int DtBlkFxAudioProcessor::getNumPrograms()
{
  return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
            // so this should be at least 1, even if you're not really implementing programs.
}

int DtBlkFxAudioProcessor::getCurrentProgram()
{
  return 0;
}

void DtBlkFxAudioProcessor::setCurrentProgram(int index)
{
  juce::ignoreUnused(index);
}

const juce::String DtBlkFxAudioProcessor::getProgramName(int index)
{
  juce::ignoreUnused(index);
  return {};
}

void DtBlkFxAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
  juce::ignoreUnused(index, newName);
}

juce::AudioProcessorValueTreeState::ParameterLayout DtBlkFxAudioProcessor::createParameterLayout()
{
  juce::AudioProcessorValueTreeState::ParameterLayout layout;

  layout.add(makeUnitParameter("mixback", "Mix Back", 1.0f));
  layout.add(std::make_unique<juce::AudioParameterBool>("power_match", "Power Match", false));
  layout.add(makeUnitParameter("delay", "Delay", 0.0627451f));
  layout.add(makeUnitParameter("fft_len", "Block Size", 72.0f / 255.0f));
  layout.add(makeUnitParameter("overlap", "Overlap", 0.35f));
  layout.add(std::make_unique<juce::AudioParameterBool>("block_sync", "Block Sync", false));
  layout.add(std::make_unique<juce::AudioParameterBool>("parameter_sync", "Parameter Sync", false));

  for (int slot = 0; slot < 8; ++slot) {
    const auto slotName = "FX " + juce::String(slot + 1);
    layout.add(makeUnitParameter(fxParameterID(slot, "freq_a"), slotName + " Frequency A", 0.0f));
    layout.add(makeUnitParameter(fxParameterID(slot, "freq_b"), slotName + " Frequency B", 1.0f));
    layout.add(makeUnitParameter(fxParameterID(slot, "amp"), slotName + " Amplitude", 0.6f));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        fxParameterID(slot, "type"), slotName + " Effect", effectTypeNames(), 9));
    layout.add(makeUnitParameter(fxParameterID(slot, "value"), slotName + " Value", 0.5f));
  }

  // Added last so the existing parameter order and IDs stay as they were. The latency is the
  // fixed delay between input and output that the host compensates for; Block Size is limited
  // to what fits into it.
  layout.add(std::make_unique<juce::AudioParameterChoice>(
      "latency", "Latency (max block size)", latencyChoiceNames(), defaultLatencyChoice));

  return layout;
}

juce::String DtBlkFxAudioProcessor::fxParameterID(int slot, const juce::String& parameter)
{
  return "fx" + juce::String(slot + 1) + "_" + parameter;
}

void DtBlkFxAudioProcessor::cacheParameterHandles()
{
  // The pointers stay valid for the lifetime of the value tree state.
  const auto find = [this](const juce::String& id) {
    auto* value = parameters.getRawParameterValue(id);
    jassert(value != nullptr);
    return value;
  };

  handles.mixBack = find("mixback");
  handles.powerMatch = find("power_match");
  handles.delay = find("delay");
  handles.fftLength = find("fft_len");
  handles.overlap = find("overlap");
  handles.blockSync = find("block_sync");
  handles.parameterSync = find("parameter_sync");
  handles.latency = find("latency");

  for (int slot = 0; slot < 8; ++slot) {
    auto& slotHandles = handles.slots[static_cast<size_t>(slot)];
    slotHandles.freqA = find(fxParameterID(slot, "freq_a"));
    slotHandles.freqB = find(fxParameterID(slot, "freq_b"));
    slotHandles.amplitude = find(fxParameterID(slot, "amp"));
    slotHandles.type = find(fxParameterID(slot, "type"));
    slotHandles.value = find(fxParameterID(slot, "value"));
  }
}

int DtBlkFxAudioProcessor::getSelectedLatencySamples() const noexcept
{
  float choice = static_cast<float>(defaultLatencyChoice);
  if (handles.latency != nullptr) {
    const auto raw = handles.latency->load(std::memory_order_relaxed);
    if (std::isfinite(raw))
      choice = raw;
  }
  return latencySamplesForChoice(juce::roundToInt(choice));
}

DtBlkFxAudioProcessor::LegacyParameterBlock DtBlkFxAudioProcessor::getLegacyParameterBlock() const
{
  // A damaged state can leave a non-finite value in a parameter; never pass it on.
  const auto readFloat = [](const std::atomic<float>* handle) {
    if (handle == nullptr)
      return 0.0f;
    const auto current = handle->load(std::memory_order_relaxed);
    return std::isfinite(current) ? current : 0.0f;
  };
  const auto readBool = [&readFloat](const std::atomic<float>* handle) {
    return readFloat(handle) >= 0.5f;
  };

  // The engine consumes the original compact 44-float parameter representation.
  LegacyParameterBlock legacy{};
  const auto mixBack = juce::jlimit(0.0f, 1.0f, readFloat(handles.mixBack));
  legacy[0] = readBool(handles.powerMatch) ? mixBack * 0.5f : 1.0f - mixBack * 0.499f;
  legacy[1] = readFloat(handles.delay);
  legacy[2] = readFloat(handles.fftLength);

  const auto overlap = juce::jlimit(0.0f, 1.0f, readFloat(handles.overlap));
  legacy[3] = readBool(handles.blockSync) ? juce::jlimit(0.501f, 1.0f, 1.0f - overlap * 0.5f)
                                          : juce::jlimit(0.0f, 0.499f, overlap * 0.5f);

  for (int slot = 0; slot < 8; ++slot) {
    const auto offset = static_cast<size_t>(4 + slot * 5);
    const auto& slotHandles = handles.slots[static_cast<size_t>(slot)];
    legacy[offset] = readFloat(slotHandles.freqA);
    legacy[offset + 1] = readFloat(slotHandles.freqB);
    legacy[offset + 2] = readFloat(slotHandles.amplitude);

    // The raw value of a choice parameter is already the choice index (0..30). Do not rescale it.
    const auto typeIndex = juce::roundToInt(readFloat(slotHandles.type));
    legacy[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(typeIndex);

    legacy[offset + 4] = readFloat(slotHandles.value);
  }

  return legacy;
}

void DtBlkFxAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
  juce::ignoreUnused(samplesPerBlock);
  currentSampleRate = sampleRate;
  beatTracker.reset(sampleRate);

  // FFT plans and buffers are prepared here so processBlock does not allocate them.
  const auto legacyParameters = getLegacyParameterBlock();
  const auto fftPlanIndex = getLegacyFftPlanIndex(legacyParameters[2]);
  if (!engine->prepare(fftPlanIndex, sampleRate)) {
    juce::Logger::writeToLog("DtBlkFx: failed to prepare FFT engine");
    appliedLatencySamples = 0;
    setLatencySamples(0);
    return;
  }

  // The latency is fixed for the whole run (until the Latency parameter changes), so the delay
  // between input and output does not depend on the Block Size.
  appliedLatencySamples = getSelectedLatencySamples();
  engine->setFixedLatency(appliedLatencySamples);
  engine->reset();
  const auto defaultSamplesPerBeat = sampleRate * 60.0 / DtBlkFxHost::BeatTracker::defaultBpm;
  if (!engine->configureLegacyParameters(
          legacyParameters, defaultSamplesPerBeat, 0.0, isOn(handles.parameterSync))) {
    juce::Logger::writeToLog("DtBlkFx: failed to initialize parameter interpolation");
    engine->release();
    appliedLatencySamples = 0;
    setLatencySamples(0);
    return;
  }
  setLatencySamples(engine->getLatencySamples());
}

void DtBlkFxAudioProcessor::releaseResources()
{
  engine->release();
  appliedLatencySamples = 0;
  setLatencySamples(0);
}

bool DtBlkFxAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
  juce::ignoreUnused(layouts);
  return true;
#else
  // This is the place where you check if the layout is supported.
  // In this template code we only support mono or stereo.
  // Some plugin hosts, such as certain GarageBand versions, will only
  // load plugins that support stereo bus layouts.
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

  // This checks if the input layout matches the output layout
#  if !JucePlugin_IsSynth
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
#  endif

  return true;
#endif
}

void DtBlkFxAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
  juce::ignoreUnused(midiMessages);

  juce::ScopedNoDenormals noDenormals;
  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  // In case we have more outputs than inputs, this code clears any output
  // channels that didn't contain input data, (because these aren't
  // guaranteed to be empty - they may contain garbage).
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  std::array<float*, 2> channels{};
  const auto channelCount = juce::jmin(totalNumInputChannels, static_cast<int>(channels.size()));
  for (int channel = 0; channel < channelCount; ++channel)
    channels[static_cast<size_t>(channel)] = buffer.getWritePointer(channel);

  // The Latency parameter decides the fixed delay the host compensates for. Only a change of the
  // parameter touches the engine and tells the host; Block Size automation never does.
  const auto selectedLatency = getSelectedLatencySamples();
  if (selectedLatency != appliedLatencySamples) {
    appliedLatencySamples = selectedLatency;
    engine->setFixedLatency(selectedLatency);
    setLatencySamples(engine->getLatencySamples());
  }

  // Host transport: tempo and beat position drive the beat-synchronised frame scheduling.
  // Hosts may leave any of these out (the optionals are then empty); the tracker keeps the
  // last tempo and a running beat grid.
  DtBlkFxHost::TransportInfo transport;
  if (auto* audioPlayHead = getPlayHead()) {
    if (const auto position = audioPlayHead->getPosition()) {
      if (const auto bpm = position->getBpm())
        transport.bpm = *bpm;
      if (const auto ppq = position->getPpqPosition())
        transport.ppqPosition = *ppq;
      transport.isPlaying = position->getIsPlaying();
    }
  }
  const auto timing = beatTracker.update(transport, buffer.getNumSamples());

  const auto legacyParameters = getLegacyParameterBlock();
  if (!engine->configureLegacyParameters(legacyParameters,
                                         timing.samplesPerBeat,
                                         timing.beatPhaseSamples,
                                         isOn(handles.parameterSync))) {
    juce::Logger::writeToLog("DtBlkFx: failed to queue legacy parameter snapshot");
    buffer.clear();
    return;
  }

  if (channelCount != totalNumInputChannels ||
      !engine->process(channels.data(), channelCount, buffer.getNumSamples())) {
    buffer.clear();
    jassertfalse;
  }
}

bool DtBlkFxAudioProcessor::hasEditor() const
{
  return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* DtBlkFxAudioProcessor::createEditor()
{
  return new DtBlkFxAudioProcessorEditor(*this);
}

void DtBlkFxAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
  // Store the full parameter tree so automation values survive project reloads.
  if (const auto xml = parameters.copyState().createXml())
    copyXmlToBinary(*xml, destData);
}

void DtBlkFxAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
  if (const auto xml = getXmlFromBinary(data, sizeInBytes)) {
    if (xml->hasTagName(parameters.state.getType())) {
      parameters.replaceState(juce::ValueTree::fromXml(*xml));

      // A damaged or hand-edited state can contain text such as "nan". JUCE parses it into a
      // non-finite value that its range clamping does not catch, so put such parameters back
      // to their defaults.
      for (auto* parameter : getParameters()) {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter);
        if (ranged == nullptr)
          continue;
        const juce::String id = ranged->paramID;
        const auto* raw = parameters.getRawParameterValue(id);
        if (raw != nullptr && !std::isfinite(raw->load()))
          ranged->setValueNotifyingHost(ranged->getDefaultValue());
      }
    }
  }
}

// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new DtBlkFxAudioProcessor();
}
