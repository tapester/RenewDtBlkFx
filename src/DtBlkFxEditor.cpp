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
#include "DtBlkFxFactoryPresets.h"
#include "DtBlkFxValueText.h"

#include <algorithm>

namespace {
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
  title.setText("Fast-Fourier-Transform spectral processing", juce::dontSendNotification);
  title.setFont(juce::Font(20.0f, juce::Font::bold));
  title.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(title);

  presetDropdown.setTextWhenNothingSelected("Presets");
  presetDropdown.onChange = [this] { loadPreset(presetDropdown.getSelectedId()); };
  factoryPresets = DtBlkFxFactory::parsePresets(DtBlkFxFactory::presetText());
  refreshPresets();
  addAndMakeVisible(presetDropdown);

  presetMenuButton.onClick = [this] { showPresetMenu(); };
  addAndMakeVisible(presetMenuButton);

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

  const auto sampleRate = [this] {
    return processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 44100.0;
  };
  globalSliders[0].textFromValueFunction = [](double value) {
    return juce::String(DtBlkFxText::formatMixBack(value));
  };
  globalSliders[0].valueFromTextFunction = [](const juce::String& text) {
    return DtBlkFxText::parseMixBack(text.toStdString());
  };
  globalSliders[1].textFromValueFunction = [](double value) {
    return juce::String(DtBlkFxText::formatDelay(value));
  };
  globalSliders[1].valueFromTextFunction = [](const juce::String& text) {
    return DtBlkFxText::parseDelay(text.toStdString());
  };
  globalSliders[2].textFromValueFunction = [sampleRate](double value) {
    return juce::String(DtBlkFxText::formatBlockSize(value, sampleRate()));
  };
  globalSliders[2].valueFromTextFunction = [sampleRate](const juce::String& text) {
    return DtBlkFxText::parseBlockSize(text.toStdString(), sampleRate());
  };
  globalSliders[3].textFromValueFunction = [this](double value) {
    return juce::String(DtBlkFxText::formatOverlap(value, globalSliders[2].getValue()));
  };
  globalSliders[3].valueFromTextFunction = [this](const juce::String& text) {
    return DtBlkFxText::parseOverlap(text.toStdString(), globalSliders[2].getValue());
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
  // Minimum size keeps the 600 px content column and the Global section fully visible; the
  // default maximum (50000) leaves the window free to grow.
  setResizable(true, true);
  setResizeLimits(700, 500, 2400, 1800);
  setSize(1100, 780);
}

DtBlkFxAudioProcessorEditor::~DtBlkFxAudioProcessorEditor()
{
  viewport.setViewedComponent(nullptr, false);
}

namespace {
bool writeXml(const juce::XmlElement* xml, const juce::File& file)
{
  if (xml != nullptr && file.getParentDirectory().createDirectory().wasOk() && xml->writeTo(file))
    return true;
  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                         "Preset error",
                                         "Could not write " + file.getFullPathName());
  return false;
}
} // namespace

juce::File DtBlkFxAudioProcessorEditor::userPresetDirectory()
{
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("DtBlkFx/Presets");
}

void DtBlkFxAudioProcessorEditor::refreshPresets(const juce::File& select)
{
  userPresets = userPresetDirectory().findChildFiles(juce::File::findFiles, false, "*.xml");
  std::sort(userPresets.begin(), userPresets.end(), [](const juce::File& a, const juce::File& b) {
    return a.getFileName().compareNatural(b.getFileName()) < 0;
  });

  presetDropdown.clear(juce::dontSendNotification);
  presetDropdown.addItem("Default", 1);
  for (size_t i = 0; i < factoryPresets.size(); ++i)
    presetDropdown.addItem(factoryPresets[i].name, static_cast<int>(i) + 2);
  if (!userPresets.isEmpty())
    presetDropdown.addSeparator();
  for (int i = 0; i < userPresets.size(); ++i)
    presetDropdown.addItem(userPresets[i].getFileNameWithoutExtension(), i + firstUserId());
  const auto selectedIndex = userPresets.indexOf(select);
  // Otherwise select nothing: the current sound is not known to be any preset, and a selected
  // "Default" could not be picked.
  if (selectedIndex >= 0)
    presetDropdown.setSelectedId(selectedIndex + firstUserId(), juce::dontSendNotification);
}

void DtBlkFxAudioProcessorEditor::loadPreset(int itemId)
{
  if (itemId == 1) {
    for (auto* parameter : processorRef.getParameters())
      parameter->setValueNotifyingHost(parameter->getDefaultValue());
    return;
  }

  if (itemId < firstUserId()) {
    if (juce::isPositiveAndBelow(itemId - 2, factoryPresets.size()))
      processorRef.setLegacyParameterBlock(factoryPresets[static_cast<size_t>(itemId - 2)].values);
    return;
  }

  if (!juce::isPositiveAndBelow(itemId - firstUserId(), userPresets.size()))
    return;
  // Go through the host-state path so presets get the same validation and NaN repair.
  if (const auto xml = juce::XmlDocument::parse(userPresets[itemId - firstUserId()])) {
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary(*xml, block);
    processorRef.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
  }
}

void DtBlkFxAudioProcessorEditor::saveCurrentTo(const juce::File& file)
{
  const auto xml = processorRef.parameters.copyState().createXml();
  if (writeXml(xml.get(), file))
    refreshPresets(file);
}

void DtBlkFxAudioProcessorEditor::promptForName(std::function<void(const juce::String&)> onName)
{
  auto* window = new juce::AlertWindow(
      "Save Preset", "Preset name:", juce::MessageBoxIconType::NoIcon, this);
  window->addTextEditor("name", presetDropdown.getSelectedId() > 1 ? presetDropdown.getText() : "");
  window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
  window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  window->enterModalState(
      true,
      juce::ModalCallbackFunction::create(
          [self = juce::Component::SafePointer<juce::Component>(this), window, onName](int result) {
            // The window is still alive here; it is deleted after the callback returns.
            if (result == 1 && self != nullptr)
              onName(window->getTextEditorContents("name"));
          }),
      true);
}

void DtBlkFxAudioProcessorEditor::confirm(const juce::String& message, std::function<void()> onOk)
{
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions::makeOptionsOkCancel(
          juce::MessageBoxIconType::QuestionIcon, "DtBlkFx", message, "OK", "Cancel", this),
      [self = juce::Component::SafePointer<juce::Component>(this), onOk](int result) {
        if (result == 1 && self != nullptr)
          onOk();
      });
}

void DtBlkFxAudioProcessorEditor::showPresetMenu()
{
  const auto selectedId = presetDropdown.getSelectedId();
  const auto userIndex = selectedId - firstUserId();
  const auto isUserPreset = juce::isPositiveAndBelow(userIndex, userPresets.size());

  juce::PopupMenu menu;
  menu.addItem(1, "Save", isUserPreset);
  menu.addItem(2, "Save As...");
  menu.addItem(3, "Delete", isUserPreset);
  menu.addSeparator();
  menu.addItem(4, "Import...");
  menu.addItem(5, "Export...");

  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(presetMenuButton),
      [self = juce::Component::SafePointer<DtBlkFxAudioProcessorEditor>(this),
       selected = isUserPreset ? userPresets[userIndex] : juce::File()](int result) {
        if (self == nullptr)
          return;
        switch (result) {
          case 1:
            self->saveCurrentTo(selected);
            break;
          case 2:
            self->promptForName([self](const juce::String& name) {
              const auto legal = juce::File::createLegalFileName(name.trim()).trim();
              if (legal.isEmpty())
                return;
              const auto file = userPresetDirectory().getChildFile(legal + ".xml");
              if (file.existsAsFile())
                self->confirm("Overwrite preset \"" + legal + "\"?",
                              [self, file] { self->saveCurrentTo(file); });
              else
                self->saveCurrentTo(file);
            });
            break;
          case 3:
            self->confirm("Delete preset \"" + selected.getFileNameWithoutExtension() + "\"?",
                          [self, selected] {
                            if (selected.deleteFile())
                              // Label only: the sound stays as it is, so nothing is lost.
                              self->refreshPresets();
                          });
            break;
          case 4:
            self->importPreset();
            break;
          case 5:
            self->exportPreset();
            break;
          default:
            break;
        }
      });
}

void DtBlkFxAudioProcessorEditor::importPreset()
{
  fileChooser = std::make_unique<juce::FileChooser>(
      "Import preset", userPresetDirectory(), "*.xml;*.blkfx");
  fileChooser->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [this](const juce::FileChooser& chooser) {
        const auto source = chooser.getResult();
        if (!source.existsAsFile())
          return;

        const auto xml = juce::XmlDocument::parse(source);
        if (xml == nullptr || !xml->hasTagName(processorRef.parameters.state.getType())) {
          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                 "Import failed",
                                                 "This is not a DtBlkFx preset.");
          return;
        }

        // Copy into the user folder under a free name so an import never overwrites a preset.
        const auto copy = userPresetDirectory().getNonexistentChildFile(
            juce::File::createLegalFileName(source.getFileNameWithoutExtension()), ".xml", false);
        if (!writeXml(xml.get(), copy))
          return;
        refreshPresets(copy);
        loadPreset(presetDropdown.getSelectedId());
      });
}

void DtBlkFxAudioProcessorEditor::exportPreset()
{
  fileChooser = std::make_unique<juce::FileChooser>(
      "Export preset", userPresetDirectory().getChildFile("preset.xml"), "*.xml;*.blkfx");
  fileChooser->launchAsync(
      juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
          juce::FileBrowserComponent::warnAboutOverwriting,
      [this](const juce::FileChooser& chooser) {
        auto target = chooser.getResult();
        if (target == juce::File())
          return;
        if (!target.hasFileExtension("xml;blkfx"))
          target = target.withFileExtension("xml");
        const auto xml = processorRef.parameters.copyState().createXml();
        writeXml(xml.get(), target);
      });
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
                                           DtBlkFxText::effectUsesMixAmplitude(type) ? "Mix / Gain"
                                                                                     : "Gain",
                                           effectValueLabel(type)};

  for (size_t control = 0; control < effectSliders[slot].size(); ++control) {
    effectLabels[slot][control].setText(used[control] ? labels[control]
                                                      : labels[control] + " (unused)",
                                        juce::dontSendNotification);
    effectLabels[slot][control].setEnabled(used[control]);
    effectSliders[slot][control].setEnabled(used[control]);
    auto& slider = effectSliders[slot][control];
    if (control < 2) {
      slider.textFromValueFunction = [](double value) {
        return juce::String(DtBlkFxText::formatFrequency(value));
      };
      slider.valueFromTextFunction = [](const juce::String& text) {
        return DtBlkFxText::parseFrequency(text.toStdString());
      };
    }
    else if (control == 2) {
      slider.textFromValueFunction = [type](double value) {
        return juce::String(DtBlkFxText::formatAmplitude(type, value));
      };
      slider.valueFromTextFunction = [type](const juce::String& text) {
        return DtBlkFxText::parseAmplitude(type, text.toStdString());
      };
    }
    else {
      slider.textFromValueFunction = [type](double value) {
        return juce::String(DtBlkFxText::formatEffectValue(type, value));
      };
      slider.valueFromTextFunction = [type](const juce::String& text) {
        return DtBlkFxText::parseEffectValue(type, text.toStdString());
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
  const int presetWidth = 200;
  const int headerRightMargin = 16;
  const int headerGap = 12;
  const int menuButtonWidth = 36;
  const int menuButtonGap = 6;
  const int presetRight = getWidth() - headerRightMargin - menuButtonWidth - menuButtonGap;
  title.setBounds(136, 5, juce::jmax(120, presetRight - presetWidth - headerGap - 136), 36);
  presetDropdown.setBounds(presetRight - presetWidth, 9, presetWidth, 28);
  presetMenuButton.setBounds(presetRight + menuButtonGap, 9, menuButtonWidth, 28);
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
