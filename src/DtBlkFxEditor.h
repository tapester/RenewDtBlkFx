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

#include "DtBlkFxFactoryPresets.h"
#include "DtBlkFxProcessor.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

// The widgets of the original look: text painted over the background pictures.
namespace DtBlkFxUi {

// Drag sideways to change, double-click to type. Wheel, keys and accessibility are the Slider's.
class ValueCell : public juce::Slider {
public:
  ValueCell();
  void paint(juce::Graphics&) override;
  void resized() override;
  void mouseDoubleClick(const juce::MouseEvent&) override;

  int textHeight = 24; // the text sits in this much of the bottom of the cell

private:
  juce::Label entry;
};

class TypeCell : public juce::ComboBox {
public:
  TypeCell();
  void paint(juce::Graphics&) override;
};

class SwitchCell : public juce::Button {
public:
  SwitchCell(const juce::String& title, const juce::String& on, const juce::String& off);
  void paintButton(juce::Graphics&, bool, bool) override;

private:
  juce::String onText, offText;
};

struct Canvas : public juce::Component {
  std::function<void(juce::Graphics&)> onPaint;
  void paint(juce::Graphics& graphics) override { onPaint(graphics); }
};

// A scrolling waterfall of one spectrum, newest line at the bottom. Starts as the splash picture.
class Spectrogram {
public:
  Spectrogram(const juce::Image& splash, int top);
  void addLine(const std::array<float, DtBlkFxSpectrum::columns>& power);
  void paint(juce::Graphics&, const juce::String& name) const;

private:
  juce::Image image;
  int top;
};

} // namespace DtBlkFxUi

class DtBlkFxAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
  explicit DtBlkFxAudioProcessorEditor(DtBlkFxAudioProcessor&);
  ~DtBlkFxAudioProcessorEditor() override;

  // Fills what a host window of another shape leaves around the canvas.
  void paint(juce::Graphics& graphics) override { graphics.fillAll(juce::Colours::black); }
  void resized() override;
  // Heard from the canvas and everything on it, to highlight the row under the mouse.
  void mouseEnter(const juce::MouseEvent&) override { canvas.repaint(); }
  void mouseExit(const juce::MouseEvent&) override { canvas.repaint(); }
  void mouseUp(const juce::MouseEvent&) override { canvas.repaint(); }

private:
  using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
  using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
  using ParameterAttachment = juce::ParameterAttachment;

  void timerCallback() override;
  void paintCanvas(juce::Graphics&);
  void paintEffectRow(juce::Graphics&, size_t slot);
  void updateEffectControls(size_t slot);

  // User presets are plain XML files holding the parameter tree (same payload as host state).
  static juce::File userPresetDirectory();
  void refreshPresets(const juce::File& select = {});
  void loadPreset(int itemId);
  void showPresetMenu();
  void saveCurrentTo(const juce::File&);
  void promptForName(std::function<void(const juce::String&)> onName);
  void confirm(const juce::String& message, std::function<void()> onOk);
  void importPreset();
  void exportPreset();

  DtBlkFxAudioProcessor& processorRef;
  // Everything is laid out once on the canvas at the original size; resizing only scales it.
  DtBlkFxUi::Canvas canvas;
  const juce::Image globalImage, splashImage, rowImage;
  // Input above, output below.
  std::array<DtBlkFxUi::Spectrogram, 2> spectrograms;
  juce::ComboBox presetDropdown;
  juce::TextButton presetMenuButton{"..."};
  // Dropdown item ids: 1 is "Default", then the factory presets, then the user presets.
  std::vector<DtBlkFxFactory::Preset> factoryPresets;
  juce::Array<juce::File> userPresets;
  int firstUserId() const { return 2 + static_cast<int>(factoryPresets.size()); }
  std::unique_ptr<juce::FileChooser> fileChooser;
  juce::ComboBox latency;
  juce::ToggleButton parameterSync{"Param sync"};
  // Mix Back, Delay, Block Size, Overlap.
  std::array<DtBlkFxUi::ValueCell, 4> globalSliders;
  DtBlkFxUi::SwitchCell powerMode{"Power Mode", "match", "filter"};
  DtBlkFxUi::SwitchCell blockSync{"Block Sync", "on", "off"};
  std::array<DtBlkFxUi::TypeCell, 8> effectTypes;
  // Frequency A, Frequency B, Amplitude, Value.
  std::array<std::array<DtBlkFxUi::ValueCell, 4>, 8> effectSliders;
  // Keep each UI control bound to its matching host parameter for automation and state restore.
  std::array<std::unique_ptr<SliderAttachment>, 4> globalSliderAttachments;
  std::array<std::unique_ptr<ButtonAttachment>, 3> buttonAttachments;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> latencyAttachment;
  std::array<std::unique_ptr<ParameterAttachment>, 8> effectTypeAttachments;
  std::array<std::array<std::unique_ptr<SliderAttachment>, 4>, 8> effectSliderAttachments;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DtBlkFxAudioProcessorEditor)
};
