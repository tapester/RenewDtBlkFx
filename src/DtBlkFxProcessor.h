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

#pragma once

#include "DtBlkFxHostTiming.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

class DtBlkFxEngine;

class DtBlkFxAudioProcessor : public juce::AudioProcessor {
public:
  using LegacyParameterBlock = std::array<float, 44>;

  DtBlkFxAudioProcessor();
  ~DtBlkFxAudioProcessor() override;

  juce::AudioProcessorValueTreeState parameters;

  // Translate the stable host-facing parameter IDs into the engine's legacy 44-value layout.
  static juce::String fxParameterID(int slot, const juce::String& parameter);
  static const juce::StringArray& effectTypeNames();
  LegacyParameterBlock getLegacyParameterBlock() const;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  using AudioProcessor::processBlock;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override;

  const juce::String getName() const override;

  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override;

  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String& newName) override;

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

private:
  // Pointers to the parameters' raw values, looked up once in the constructor so that the
  // audio thread never builds parameter IDs or searches by name.
  struct SlotHandles {
    std::atomic<float>* freqA = nullptr;
    std::atomic<float>* freqB = nullptr;
    std::atomic<float>* amplitude = nullptr;
    std::atomic<float>* type = nullptr;
    std::atomic<float>* value = nullptr;
  };

  struct ParameterHandles {
    std::atomic<float>* mixBack = nullptr;
    std::atomic<float>* powerMatch = nullptr;
    std::atomic<float>* delay = nullptr;
    std::atomic<float>* fftLength = nullptr;
    std::atomic<float>* overlap = nullptr;
    std::atomic<float>* blockSync = nullptr;
    std::atomic<float>* parameterSync = nullptr;
    std::array<SlotHandles, 8> slots{};
  };

  // One normalized host parameter block is adapted to the engine's original control ranges.
  static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
  void cacheParameterHandles();

  std::unique_ptr<DtBlkFxEngine> engine;
  ParameterHandles handles;
  DtBlkFxHost::BeatTracker beatTracker;
  double currentSampleRate = 44100.0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DtBlkFxAudioProcessor)
};
