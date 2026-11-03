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

#include <cmath>

namespace {
std::unique_ptr<juce::AudioParameterFloat>
makeUnitParameter(const juce::String& id, const juce::String& name, float defaultValue)
{
  return std::make_unique<juce::AudioParameterFloat>(
      id, name, juce::NormalisableRange<float>{0.0f, 1.0f, 0.001f}, defaultValue);
}

int getLegacyFftPlanIndex(float parameter)
{
  return juce::jlimit(0,
                      DtBlkFxEngine::fftSizeCount - 1,
                      static_cast<int>(parameter * 255.0f / 4.0f - 1.5f));
}

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
  const auto frameLatency =
      currentSampleRate > 0.0 ? static_cast<double>(engine->getLatencySamples()) / currentSampleRate
                              : 0.0;
  return 6.0 + frameLatency;
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

  return layout;
}

juce::String DtBlkFxAudioProcessor::fxParameterID(int slot, const juce::String& parameter)
{
  return "fx" + juce::String(slot + 1) + "_" + parameter;
}

DtBlkFxAudioProcessor::LegacyParameterBlock DtBlkFxAudioProcessor::getLegacyParameterBlock() const
{
  const auto readFloat = [this](const juce::String& id) {
    const auto* value = parameters.getRawParameterValue(id);
    jassert(value != nullptr);
    return value != nullptr ? value->load(std::memory_order_relaxed) : 0.0f;
  };
  const auto readBool = [&readFloat](const juce::String& id) { return readFloat(id) >= 0.5f; };

  // The engine consumes the original compact 44-float parameter representation.
  LegacyParameterBlock legacy{};
  const auto mixBack = juce::jlimit(0.0f, 1.0f, readFloat("mixback"));
  legacy[0] = readBool("power_match") ? mixBack * 0.5f : 1.0f - mixBack * 0.499f;
  legacy[1] = readFloat("delay");
  legacy[2] = readFloat("fft_len");

  const auto overlap = juce::jlimit(0.0f, 1.0f, readFloat("overlap"));
  legacy[3] = readBool("block_sync") ? juce::jlimit(0.501f, 1.0f, 1.0f - overlap * 0.5f)
                                     : juce::jlimit(0.0f, 0.499f, overlap * 0.5f);

  for (int slot = 0; slot < 8; ++slot) {
    const auto offset = static_cast<size_t>(4 + slot * 5);
    legacy[offset] = readFloat(fxParameterID(slot, "freq_a"));
    legacy[offset + 1] = readFloat(fxParameterID(slot, "freq_b"));
    legacy[offset + 2] = readFloat(fxParameterID(slot, "amp"));

    const auto typeIndex =
        juce::jlimit(0, 30, juce::roundToInt(readFloat(fxParameterID(slot, "type")) * 30.0f));
    legacy[offset + 3] = static_cast<float>(typeIndex * 8 + 4) / 255.0f;
    legacy[offset + 4] = readFloat(fxParameterID(slot, "value"));
  }

  return legacy;
}

void DtBlkFxAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
  juce::ignoreUnused(samplesPerBlock);
  currentSampleRate = sampleRate;

  // FFT plans and buffers are prepared here so processBlock does not allocate them.
  const auto legacyParameters = getLegacyParameterBlock();
  const auto fftPlanIndex = getLegacyFftPlanIndex(legacyParameters[2]);
  if (!engine->prepare(fftPlanIndex, sampleRate)) {
    juce::Logger::writeToLog("DtBlkFx: failed to prepare FFT engine");
    setLatencySamples(0);
    return;
  }

  engine->reset();
  const auto parameterSync =
      parameters.getRawParameterValue("parameter_sync")->load(std::memory_order_relaxed) >= 0.5f;
  if (!engine->configureLegacyParameters(legacyParameters, sampleRate * 0.5, 0.0, parameterSync)) {
    juce::Logger::writeToLog("DtBlkFx: failed to initialize parameter interpolation");
    engine->release();
    setLatencySamples(0);
    return;
  }
  setLatencySamples(engine->getLatencySamples());
}

void DtBlkFxAudioProcessor::releaseResources()
{
  engine->release();
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
  // This is here to avoid people getting screaming feedback
  // when they first compile a plugin, but obviously you don't need to keep
  // this code if your algorithm always overwrites all the output channels.
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  std::array<float*, 2> channels{};
  const auto channelCount = juce::jmin(totalNumInputChannels, static_cast<int>(channels.size()));
  for (int channel = 0; channel < channelCount; ++channel)
    channels[static_cast<size_t>(channel)] = buffer.getWritePointer(channel);

  // Capture host transport timing for beat-synchronized frame scheduling.
  const auto legacyParameters = getLegacyParameterBlock();
  auto tempo = 120.0;
  auto beatPhaseSamples = 0.0;
  if (auto* audioPlayHead = getPlayHead()) {
    juce::AudioPlayHead::CurrentPositionInfo position;
    position.resetToDefault();
    if (audioPlayHead->getCurrentPosition(position)) {
      if (std::isfinite(position.bpm) && position.bpm > 0.0)
        tempo = position.bpm;
      if (std::isfinite(position.ppqPosition)) {
        const auto beatFraction = position.ppqPosition - std::floor(position.ppqPosition);
        beatPhaseSamples = beatFraction * currentSampleRate * 60.0 / tempo;
      }
    }
  }
  const auto samplesPerBeat = currentSampleRate * 60.0 / tempo;
  const auto parameterSync =
      parameters.getRawParameterValue("parameter_sync")->load(std::memory_order_relaxed) >= 0.5f;
  if (!engine->configureLegacyParameters(
          legacyParameters, samplesPerBeat, beatPhaseSamples, parameterSync)) {
    juce::Logger::writeToLog("DtBlkFx: failed to queue legacy parameter snapshot");
    buffer.clear();
    return;
  }

  if (channelCount != totalNumInputChannels ||
      !engine->process(channels.data(), channelCount, buffer.getNumSamples())) {
    buffer.clear();
    jassertfalse;
  }
  else {
    setLatencySamples(engine->getLatencySamples());
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
    if (xml->hasTagName(parameters.state.getType()))
      parameters.replaceState(juce::ValueTree::fromXml(*xml));
  }
}

// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new DtBlkFxAudioProcessor();
}
