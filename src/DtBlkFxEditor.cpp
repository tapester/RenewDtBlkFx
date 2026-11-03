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

#include "DtBlkFxEditor.h"

DtBlkFxAudioProcessorEditor::DtBlkFxAudioProcessorEditor(DtBlkFxAudioProcessor& processor)
    : AudioProcessorEditor(&processor)
    , processorRef(processor)
{
  title.setText("DtBlkFx  |  FFT EFFECT CHAIN", juce::dontSendNotification);
  title.setFont(juce::Font(20.0f, juce::Font::bold));
  title.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(title);

  viewport.setViewedComponent(&content, false);
  viewport.setScrollBarsShown(true, false);
  addAndMakeVisible(viewport);

  globalGroup.setText("Global");
  content.addAndMakeVisible(globalGroup);

  const std::array<juce::String, 4> globalNames{"Mix Back", "Delay", "Block Size", "Overlap"};
  const std::array<juce::String, 4> globalIDs{"mixback", "delay", "fft_len", "overlap"};
  for (size_t i = 0; i < globalSliders.size(); ++i) {
    configureSlider(globalSliders[i], globalLabels[i], globalNames[i]);
    content.addAndMakeVisible(globalLabels[i]);
    content.addAndMakeVisible(globalSliders[i]);
    globalSliderAttachments[i] =
        std::make_unique<SliderAttachment>(processorRef.parameters, globalIDs[i], globalSliders[i]);
  }

  const std::array<juce::String, 3> toggleNames{"Power Match", "Block Sync", "Parameter Sync"};
  const std::array<juce::String, 3> toggleIDs{"power_match", "block_sync", "parameter_sync"};
  for (size_t i = 0; i < globalToggles.size(); ++i) {
    globalToggles[i].setButtonText(toggleNames[i]);
    content.addAndMakeVisible(globalToggles[i]);
    globalButtonAttachments[i] =
        std::make_unique<ButtonAttachment>(processorRef.parameters, toggleIDs[i], globalToggles[i]);
  }

  const std::array<juce::String, 4> effectControlNames{
      "Frequency A", "Frequency B", "Amplitude", "Value"};
  const std::array<juce::String, 4> effectControlIDs{"freq_a", "freq_b", "amp", "value"};
  for (size_t slot = 0; slot < effectGroups.size(); ++slot) {
    effectGroups[slot].setText("Effect Slot " + juce::String(static_cast<int>(slot + 1)));
    content.addAndMakeVisible(effectGroups[slot]);

    content.addAndMakeVisible(effectTypes[slot]);
    effectTypeAttachments[slot] = std::make_unique<ComboBoxAttachment>(
        processorRef.parameters,
        DtBlkFxAudioProcessor::fxParameterID(static_cast<int>(slot), "type"),
        effectTypes[slot]);

    for (size_t control = 0; control < effectSliders[slot].size(); ++control) {
      configureSlider(
          effectSliders[slot][control], effectLabels[slot][control], effectControlNames[control]);
      content.addAndMakeVisible(effectLabels[slot][control]);
      content.addAndMakeVisible(effectSliders[slot][control]);
      effectSliderAttachments[slot][control] = std::make_unique<SliderAttachment>(
          processorRef.parameters,
          DtBlkFxAudioProcessor::fxParameterID(static_cast<int>(slot), effectControlIDs[control]),
          effectSliders[slot][control]);
    }
  }

  setSize(1100, 780);
}

DtBlkFxAudioProcessorEditor::~DtBlkFxAudioProcessorEditor()
{
  viewport.setViewedComponent(nullptr, false);
}

void DtBlkFxAudioProcessorEditor::configureSlider(juce::Slider& slider,
                                                  juce::Label& label,
                                                  const juce::String& text)
{
  label.setText(text, juce::dontSendNotification);
  label.setJustificationType(juce::Justification::centred);
  slider.setSliderStyle(juce::Slider::LinearHorizontal);
  slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
}

void DtBlkFxAudioProcessorEditor::paint(juce::Graphics& graphics)
{
  graphics.fillAll(juce::Colour(0xff17191d));
  graphics.setColour(juce::Colour(0xffe6e8eb));
  graphics.setFont(juce::Font(20.0f, juce::Font::bold));
  graphics.drawText("DtBlkFx", 16, 5, 120, 36, juce::Justification::centredLeft);
}

void DtBlkFxAudioProcessorEditor::resized()
{
  title.setBounds(136, 5, getWidth() - 152, 36);
  viewport.setBounds(8, 44, getWidth() - 16, getHeight() - 52);

  const int width = juce::jmax(600, viewport.getWidth() - 20);
  const int margin = 12;
  const int globalHeight = 166;
  const int effectHeight = 142;
  const int effectGap = 8;
  const int contentHeight = margin + globalHeight + 10 +
                            static_cast<int>(effectGroups.size()) * (effectHeight + effectGap);
  content.setSize(width, contentHeight);
  globalGroup.setBounds(margin, margin, width - margin * 2, globalHeight);

  const int globalColumnWidth = (width - margin * 2 - 20) / 4;
  for (size_t i = 0; i < globalSliders.size(); ++i) {
    const int x = margin + 10 + static_cast<int>(i) * globalColumnWidth;
    globalLabels[i].setBounds(x, margin + 28, globalColumnWidth - 8, 20);
    globalSliders[i].setBounds(x, margin + 49, globalColumnWidth - 8, 76);
  }
  globalToggles[0].setBounds(margin + 14, margin + 128, 180, 24);
  globalToggles[1].setBounds(margin + 204, margin + 128, 180, 24);
  globalToggles[2].setBounds(margin + 394, margin + 128, 180, 24);

  const int effectWidth = width - margin * 2;
  const int typeWidth = 190;
  const int controlGap = 8;
  const int controlWidth = (effectWidth - typeWidth - 24 - 4 * controlGap) / 4;
  const int controlsY = margin + globalHeight + 10;

  for (size_t slot = 0; slot < effectGroups.size(); ++slot) {
    const int y = controlsY + static_cast<int>(slot) * (effectHeight + effectGap);
    effectGroups[slot].setBounds(margin, y, effectWidth, effectHeight);
    effectTypes[slot].setBounds(margin + 12, y + 48, typeWidth, 28);

    for (size_t control = 0; control < effectSliders[slot].size(); ++control) {
      const int x = margin + 12 + typeWidth + controlGap +
                    static_cast<int>(control) * (controlWidth + controlGap);
      effectLabels[slot][control].setBounds(x, y + 34, controlWidth, 20);
      effectSliders[slot][control].setBounds(x, y + 56, controlWidth, 76);
    }
  }
}
