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

#include "DtBlkFxProcessor.h"

#include <array>
#include <memory>

class DtBlkFxAudioProcessorEditor : public juce::AudioProcessorEditor {
public:
  explicit DtBlkFxAudioProcessorEditor(DtBlkFxAudioProcessor&);
  ~DtBlkFxAudioProcessorEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;

private:
  using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
  using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
  using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

  void configureSlider(juce::Slider&, juce::Label&, const juce::String&);

  DtBlkFxAudioProcessor& processorRef;
  juce::Viewport viewport;
  juce::Component content;
  juce::Label title;
  juce::GroupComponent globalGroup;
  std::array<juce::Slider, 4> globalSliders;
  std::array<juce::Label, 4> globalLabels;
  std::array<juce::ToggleButton, 3> globalToggles;
  std::array<juce::GroupComponent, 8> effectGroups;
  std::array<juce::ComboBox, 8> effectTypes;
  std::array<std::array<juce::Slider, 4>, 8> effectSliders;
  std::array<std::array<juce::Label, 4>, 8> effectLabels;
  std::array<std::unique_ptr<SliderAttachment>, 4> globalSliderAttachments;
  std::array<std::unique_ptr<ButtonAttachment>, 3> globalButtonAttachments;
  std::array<std::unique_ptr<ComboBoxAttachment>, 8> effectTypeAttachments;
  std::array<std::array<std::unique_ptr<SliderAttachment>, 4>, 8> effectSliderAttachments;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DtBlkFxAudioProcessorEditor)
};
