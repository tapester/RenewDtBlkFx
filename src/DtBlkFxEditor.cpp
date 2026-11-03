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
#include "DtBlkFxEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double c0Frequency = 16.35159898333181161;

juce::String formatFrequency(double normalized)
{
  if (normalized <= 0.0)
    return "0 Hz";

  const auto frequency = c0Frequency * std::pow(2.0, normalized * 127.5 / 12.0);
  return frequency >= 1000.0 ? juce::String(frequency / 1000.0, 2) + " kHz"
                             : juce::String(frequency, 1) + " Hz";
}

double parseFrequency(const juce::String& text)
{
  const auto frequency = text.getDoubleValue() * (text.containsIgnoreCase("khz") ? 1000.0 : 1.0);
  return frequency <= 0.0 ? 0.0
                          : std::clamp(12.0 * std::log2(frequency / c0Frequency) / 127.5, 0.0, 1.0);
}

bool effectUsesMixAmplitude(int type)
{
  return type == 5 || type == 6 || type == 7 || type == 8 || type == 11 || type == 12 ||
         (type >= 15 && type <= 19) || (type >= 26 && type <= 28) || type == 30;
}

juce::String formatAmplitude(int type, double normalized)
{
  if (effectUsesMixAmplitude(type) && normalized <= 0.6)
    return juce::String(juce::roundToInt(normalized * 100.0 / 0.6)) + "%";
  if (normalized <= 0.0)
    return "-inf dB";
  return juce::String(-60.0 + normalized * 100.0, 1) + " dB";
}

double parseAmplitude(int type, const juce::String& text)
{
  const auto value = text.getDoubleValue();
  if (text.contains("%") && effectUsesMixAmplitude(type))
    return std::clamp(value * 0.006, 0.0, 0.6);
  if (text.containsIgnoreCase("inf"))
    return 0.0;
  return std::clamp((value + 60.0) / 100.0, 0.0, 1.0);
}

juce::String formatNote(double semitonesFromC0)
{
  static const std::array<const char*, 12> noteNames{
      "c-", "c#", "d-", "d#", "e-", "f-", "f#", "g-", "g#", "a-", "a#", "b-"};
  const auto nearest = juce::roundToInt(semitonesFromC0);
  const auto cents = static_cast<int>((semitonesFromC0 - nearest) * 100.0);
  const auto octave = nearest / 12;
  const auto note = (nearest % 12 + 12) % 12;
  return juce::String(noteNames[static_cast<size_t>(note)]) + juce::String(octave) + ":" +
         juce::String::formatted("%+03d", cents);
}

juce::String effectValueLabel(int type)
{
  switch (type) {
    case 0:
    case 9:
    case 10:
      return "Value (unused)";
    case 1:
      return "Contrast";
    case 2:
      return "Smear";
    case 3:
      return "Threshold";
    case 4:
      return "Clip threshold";
    case 5:
      return "Resize";
    case 6:
    case 7:
    case 11:
      return "Pitch shift";
    case 8:
      return "Frequency shift";
    case 12:
      return "Target note";
    case 13:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
      return "Harmonic mode";
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 27:
    case 28:
      return "Harmonic mix";
    case 25:
      return "Threshold mask";
    case 26:
      return "Vocode mode";
    case 29:
      return "Channel mix";
    case 30:
      return "WarpMix mode";
    default:
      return "Value";
  }
}

juce::String formatEffectValue(int type, double normalized)
{
  static const std::array<const char*, 4> harmonicModes{"both", "odd", "even", "between"};
  normalized = std::clamp(normalized, 0.0, 1.0);

  switch (type) {
    case 0:
    case 9:
    case 10:
      return "-";
    case 1:
      return juce::String::formatted("%+.0f%%", (normalized * 2.0 - 1.0) * 100.0);
    case 2:
    case 4:
      return juce::String(juce::roundToInt(normalized * 100.0)) + "%";
    case 3:
    case 25: {
      const auto below = normalized >= 0.5;
      const auto fraction = below ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return juce::String(below ? "below " : "above ") +
             juce::String(juce::roundToInt(fraction * 100.0)) + "%";
    }
    case 5: {
      const auto scaled = normalized * 2.0;
      const auto direction = normalized >= 0.5 ? "rev" : "fwd";
      const auto fraction = normalized >= 1.0 ? 1.0 : scaled - std::min(std::floor(scaled), 1.0);
      const auto percent = std::pow(fraction * 4.0 - 2.0, 3.0) * 100.0;
      return juce::String::formatted("%+.0f%% time %s", percent, direction);
    }
    case 6:
    case 7:
    case 11:
      return juce::String::formatted("%+.2f notes", (normalized - 0.5) * 72.0);
    case 8: {
      auto shift = (normalized - 0.5) * 2.0;
      const auto direction = shift < 0.0 ? -1.0 : 1.0;
      shift = std::abs(shift);
      constexpr double linearFraction = 2048.0 / 32768.0;
      constexpr double exponentialScale = (1.0 - linearFraction) / 256.0;
      const auto hz =
          direction *
          ((std::pow(2.0, shift * 8.0) - 1.0) * exponentialScale + shift * linearFraction) *
          22050.0;
      return juce::String(hz, 1) + " Hz";
    }
    case 12:
      return formatNote(normalized * 127.5);
    case 13:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24: {
      const auto scaled = normalized * 4.0;
      const auto mode = std::min(static_cast<int>(scaled), 3);
      const auto fraction = scaled - mode;
      return juce::String(juce::roundToInt(fraction * 100.0)) + "% " +
             harmonicModes[static_cast<size_t>(mode)];
    }
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 27:
    case 28: {
      const auto copy = normalized >= 0.5;
      const auto fraction = copy ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return juce::String(copy ? "copy0/" : "scale/") +
             juce::String(juce::roundToInt(fraction * 100.0)) + "%";
    }
    case 26:
      if (normalized > 0.875)
        return juce::String(juce::roundToInt((normalized - 0.875) * 800.0)) + "% multiply";
      return juce::String(juce::roundToInt(normalized * (399.0 / 0.875) + 1.0)) + " segs";
    case 29: {
      const auto rightOnLeft = normalized >= 0.5;
      const auto fraction = rightOnLeft ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return juce::String(juce::roundToInt((1.0 - fraction) * 100.0)) +
             (rightOnLeft ? "% R on L" : "% L on R");
    }
    case 30: {
      const auto mode = normalized >= 0.5 ? 1 : 0;
      const auto fraction = normalized >= 0.5 ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return juce::String(juce::roundToInt(fraction * 100.0)) + " mode " + juce::String(mode);
    }
    default:
      return juce::String(normalized, 3);
  }
}

double parseEffectValue(int type, const juce::String& text)
{
  const auto value = text.getDoubleValue();
  switch (type) {
    case 0:
    case 9:
    case 10:
      return 0.5;
    case 1:
      return std::clamp((value / 100.0 + 1.0) * 0.5, 0.0, 1.0);
    case 2:
    case 4:
      return std::clamp(value / 100.0, 0.0, 1.0);
    case 3:
    case 25:
      return std::clamp(value / 200.0 + (text.startsWithIgnoreCase("below") ? 0.5 : 0.0), 0.0, 1.0);
    case 5: {
      const auto scale = std::cbrt(value / 100.0);
      const auto fraction = std::clamp((scale + 2.0) * 0.25, 0.0, 1.0);
      return std::clamp(fraction * 0.5 + (text.containsIgnoreCase("rev") ? 0.5 : 0.0), 0.0, 1.0);
    }
    case 6:
    case 7:
    case 11:
      return std::clamp(value / 72.0 + 0.5, 0.0, 1.0);
    case 8: {
      const auto targetHz = value;
      auto low = 0.0;
      auto high = 1.0;
      for (int iteration = 0; iteration < 32; ++iteration) {
        const auto middle = (low + high) * 0.5;
        const auto display = formatEffectValue(type, middle).getDoubleValue();
        if (display < targetHz)
          low = middle;
        else
          high = middle;
      }
      return (low + high) * 0.5;
    }
    case 12: {
      const auto lowered = text.toLowerCase();
      static const std::array<const char*, 12> noteNames{
          "c-", "c#", "d-", "d#", "e-", "f-", "f#", "g-", "g#", "a-", "a#", "b-"};
      for (size_t note = 0; note < noteNames.size(); ++note) {
        const juce::String prefix(noteNames[note]);
        if (!lowered.startsWith(prefix))
          continue;
        const auto rest = lowered.substring(prefix.length());
        const auto octave = rest.getIntValue();
        const auto centsPosition = rest.indexOfChar(':');
        const auto cents = centsPosition >= 0 ? rest.substring(centsPosition + 1).getIntValue() : 0;
        return std::clamp(
            (octave * 12.0 + static_cast<double>(note) + cents / 100.0) / 127.5, 0.0, 1.0);
      }
      return std::clamp(value / 127.5, 0.0, 1.0);
    }
    case 13:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24: {
      auto mode = 0;
      if (text.endsWithIgnoreCase("odd"))
        mode = 1;
      else if (text.endsWithIgnoreCase("even"))
        mode = 2;
      else if (text.endsWithIgnoreCase("between"))
        mode = 3;
      return std::clamp((mode + value / 100.0) * 0.25, 0.0, 1.0);
    }
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 27:
    case 28:
      return std::clamp(value / 200.0 + (text.startsWithIgnoreCase("copy0") ? 0.5 : 0.0), 0.0, 1.0);
    case 26:
      return text.containsIgnoreCase("multiply")
                 ? std::clamp(0.875 + value / 800.0, 0.875, 1.0)
                 : std::clamp((value - 1.0) * 0.875 / 399.0, 0.0, 0.875);
    case 29:
      return std::clamp(
          (text.containsIgnoreCase("R on L") ? 0.5 : 0.0) + (1.0 - value / 100.0) * 0.5, 0.0, 1.0);
    case 30:
      return std::clamp(value / 200.0 + (text.containsIgnoreCase("mode 1") ? 0.5 : 0.0), 0.0, 1.0);
    default:
      return std::clamp(value, 0.0, 1.0);
  }
}

std::array<bool, 4> usedEffectControls(int type)
{
  if (type == 0)
    return {true, true, true, false};
  if (type == 9 || type == 10)
    return {false, false, false, false};
  if (type == 20)
    return {true, false, false, true};
  if (type >= 21 && type <= 24)
    return {true, true, false, true};
  if (type == 25)
    return {false, false, false, true};
  return {true, true, true, true};
}
} // namespace

DtBlkFxAudioProcessorEditor::DtBlkFxAudioProcessorEditor(DtBlkFxAudioProcessor& processor)
    : AudioProcessorEditor(&processor)
    , processorRef(processor)
{
  title.setText("DtBlkFx  |  FFT EFFECT CHAIN", juce::dontSendNotification);
  title.setFont(juce::Font(20.0f, juce::Font::bold));
  title.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(title);

  viewport.setViewedComponent(&content, false);
  // The eight effect slots exceed the editor height, so the content scrolls vertically.
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

  globalSliders[0].textFromValueFunction = [](double value) {
    return juce::String(juce::roundToInt(value * 100.0)) + "%";
  };
  globalSliders[0].valueFromTextFunction = [](const juce::String& text) {
    return std::clamp(text.getDoubleValue() / 100.0, 0.0, 1.0);
  };
  globalSliders[1].textFromValueFunction = [](double value) {
    if (value < 0.5)
      return juce::String(value * 31.875, 2) + " beats";
    return juce::String((value - 0.5) * 12.0, 3) + " sec";
  };
  globalSliders[1].valueFromTextFunction = [](const juce::String& text) {
    const auto value = text.getDoubleValue();
    if (text.containsIgnoreCase("sec"))
      return std::clamp(0.5 + value / 12.0, 0.5, 1.0);
    if (text.containsIgnoreCase("ms"))
      return std::clamp(0.5 + value / 12000.0, 0.5, 1.0);
    return std::clamp(value / 31.875, 0.0, 0.5);
  };
  globalSliders[2].textFromValueFunction = [this](double value) {
    const auto index = juce::jlimit(
        0, DtBlkFxEngine::fftSizeCount - 1, static_cast<int>(value * 255.0 / 4.0 - 1.5));
    const auto sampleRate =
        processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 44100.0;
    return juce::String(DtBlkFxEngine::fftSizeAt(index) / sampleRate, 3) + " sec";
  };
  globalSliders[2].valueFromTextFunction = [this](const juce::String& text) {
    const auto sampleRate =
        processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 44100.0;
    const auto requestedSize =
        text.getDoubleValue() * (text.containsIgnoreCase("sec") ? sampleRate : 1.0);
    auto closestIndex = 0;
    auto closestDistance = std::numeric_limits<double>::max();
    for (int index = 0; index < DtBlkFxEngine::fftSizeCount; ++index) {
      const auto distance = std::abs(DtBlkFxEngine::fftSizeAt(index) - requestedSize);
      if (distance < closestDistance) {
        closestIndex = index;
        closestDistance = distance;
      }
    }
    return std::clamp((closestIndex + 1.5) * 4.0 / 255.0, 0.0, 1.0);
  };
  globalSliders[3].textFromValueFunction = [this](double value) {
    const auto plan =
        juce::jlimit(0,
                     DtBlkFxEngine::fftSizeCount - 1,
                     static_cast<int>(globalSliders[2].getValue() * 255.0 / 4.0 - 1.5));
    const auto blockSize = DtBlkFxEngine::fftSizeAt(plan);
    const auto minimumHop = static_cast<double>(blockSize) * 0.15;
    const auto maximumHop = static_cast<double>(blockSize - 16);
    const auto hop = std::max(32.0, std::floor(maximumHop + value * (minimumHop - maximumHop)));
    const auto overlap = 1.0 - hop / blockSize;
    return juce::String(juce::roundToInt(overlap * 100.0)) + "%";
  };
  globalSliders[3].valueFromTextFunction = [this](const juce::String& text) {
    const auto plan =
        juce::jlimit(0,
                     DtBlkFxEngine::fftSizeCount - 1,
                     static_cast<int>(globalSliders[2].getValue() * 255.0 / 4.0 - 1.5));
    const auto blockSize = DtBlkFxEngine::fftSizeAt(plan);
    const auto minimumHop = static_cast<double>(blockSize) * 0.15;
    const auto maximumHop = static_cast<double>(blockSize - 16);
    const auto requestedHop = blockSize * (1.0 - text.getDoubleValue() / 100.0);
    return std::clamp((maximumHop - requestedHop) / (maximumHop - minimumHop), 0.0, 1.0);
  };
  globalSliders[2].onValueChange = [this] { globalSliders[3].updateText(); };

  powerModeLabel.setText("Power", juce::dontSendNotification);
  powerModeLabel.setJustificationType(juce::Justification::centredRight);
  content.addAndMakeVisible(powerModeLabel);
  powerMode.addItem("Match", 1);
  powerMode.addItem("Filter", 2);
  content.addAndMakeVisible(powerMode);
  powerModeAttachment = std::make_unique<ParameterAttachment>(
      *processorRef.parameters.getParameter("power_match"), [this](float value) {
        const auto match = value >= 0.5f;
        powerMode.setSelectedId(match ? 1 : 2, juce::dontSendNotification);
        globalLabels[0].setText(match ? "Mix Back (Match)" : "Mix Back (Filter)",
                                juce::dontSendNotification);
      });
  powerMode.onChange = [this] {
    if (powerMode.getSelectedId() != 0)
      powerModeAttachment->setValueAsCompleteGesture(powerMode.getSelectedId() == 1 ? 1.0f : 0.0f);
  };
  powerModeAttachment->sendInitialUpdate();

  const std::array<juce::String, 2> toggleNames{"Block Sync", "Parameter Sync"};
  const std::array<juce::String, 2> toggleIDs{"block_sync", "parameter_sync"};
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

    const auto& effectNames = DtBlkFxAudioProcessor::effectTypeNames();
    effectTypes[slot].addItem(effectNames[9], 10);
    for (int type = 0; type <= 8; ++type)
      effectTypes[slot].addItem(effectNames[type], type + 1);
    for (int type = 11; type < effectNames.size(); ++type)
      effectTypes[slot].addItem(effectNames[type], type + 1);
    content.addAndMakeVisible(effectTypes[slot]);
    const auto typeParameterID =
        DtBlkFxAudioProcessor::fxParameterID(static_cast<int>(slot), "type");
    effectTypeAttachments[slot] = std::make_unique<ParameterAttachment>(
        *processorRef.parameters.getParameter(typeParameterID), [this, slot](float value) {
          auto type = juce::roundToInt(value);
          if (type == 10)
            type = 9;
          effectTypes[slot].setSelectedId(type + 1, juce::dontSendNotification);
          updateEffectControls(slot);
        });

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

    effectTypes[slot].onChange = [this, slot] {
      const auto selectedID = effectTypes[slot].getSelectedId();
      if (selectedID != 0)
        effectTypeAttachments[slot]->setValueAsCompleteGesture(static_cast<float>(selectedID - 1));
    };
    effectTypeAttachments[slot]->sendInitialUpdate();
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

void DtBlkFxAudioProcessorEditor::updateEffectControls(size_t slot)
{
  const auto type = effectTypes[slot].getSelectedId() - 1;
  const auto used = usedEffectControls(type);
  const std::array<juce::String, 4> labels{"Frequency A (Hz)",
                                           "Frequency B (Hz)",
                                           effectUsesMixAmplitude(type) ? "Mix / Gain" : "Gain",
                                           effectValueLabel(type)};

  for (size_t control = 0; control < effectSliders[slot].size(); ++control) {
    effectLabels[slot][control].setText(used[control] ? labels[control]
                                                      : labels[control] + " (unused)",
                                        juce::dontSendNotification);
    effectLabels[slot][control].setEnabled(used[control]);
    effectSliders[slot][control].setEnabled(used[control]);
    auto& slider = effectSliders[slot][control];
    if (control < 2) {
      slider.textFromValueFunction = [](double value) { return formatFrequency(value); };
      slider.valueFromTextFunction = [](const juce::String& text) { return parseFrequency(text); };
    }
    else if (control == 2) {
      slider.textFromValueFunction = [type](double value) { return formatAmplitude(type, value); };
      slider.valueFromTextFunction = [type](const juce::String& text) {
        return parseAmplitude(type, text);
      };
    }
    else {
      slider.textFromValueFunction = [type](double value) {
        return formatEffectValue(type, value);
      };
      slider.valueFromTextFunction = [type](const juce::String& text) {
        return parseEffectValue(type, text);
      };
    }
    slider.updateText();
  }
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

  // Keep controls usable at narrow sizes; the viewport supplies scrolling for vertical overflow.
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
  powerModeLabel.setBounds(margin + 12, margin + 128, 52, 24);
  powerMode.setBounds(margin + 68, margin + 128, 116, 24);
  globalToggles[0].setBounds(margin + 204, margin + 128, 180, 24);
  globalToggles[1].setBounds(margin + 394, margin + 128, 180, 24);

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
