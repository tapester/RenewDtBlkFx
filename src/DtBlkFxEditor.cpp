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

#include "BinaryData.h"
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

// The original's size, plus a bar above it for what the original did not have.
constexpr int baseWidth = 410;
constexpr int barHeight = 28;
constexpr int globalHeight = 26;
constexpr int splashHeight = 191;
constexpr int rowHeight = 24;
constexpr int rowsTop = barHeight + globalHeight + splashHeight;
constexpr int baseHeight = rowsTop + 8 * rowHeight;
constexpr int maxScale = 3;

bool isMaskEffect(int type)
{
  return type >= 20 && type <= 25;
}

// White text with a black outline, pink on the control in use.
void drawOutlined(juce::Graphics& graphics,
                  const juce::Component& owner,
                  const juce::String& text,
                  juce::Rectangle<int> area)
{
  graphics.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
  const auto draw = [&](int dx, int dy) {
    graphics.drawFittedText(
        text, area.translated(dx, dy), juce::Justification::centred, 1, 0.6f);
  };
  graphics.setColour(juce::Colours::black);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx != 0 || dy != 0)
        draw(dx, dy);
  const auto active =
      owner.isEnabled() && (owner.isMouseOverOrDragging(true) || owner.hasKeyboardFocus(true));
  graphics.setColour(active ? juce::Colour(0xffff8080) : juce::Colours::white);
  draw(0, 0);
}
} // namespace

namespace DtBlkFxUi {

ValueCell::ValueCell()
{
  setSliderStyle(juce::Slider::RotaryHorizontalDrag);
  setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
  // A drag across half of the original's width covers the whole range.
  setMouseDragSensitivity(baseWidth / 2);
  setRepaintsOnMouseActivity(true);

  entry.setFont(juce::Font(juce::FontOptions(12.0f)));
  entry.setJustificationType(juce::Justification::centred);
  entry.setColour(juce::Label::backgroundWhenEditingColourId, juce::Colours::white);
  entry.setColour(juce::Label::textWhenEditingColourId, juce::Colours::black);
  entry.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::black);
  entry.setColour(juce::Label::textColourId, juce::Colours::transparentBlack);
  entry.onTextChange = [this] {
    const juce::Slider::ScopedDragNotification gesture(*this);
    setValue(getValueFromText(entry.getText()), juce::sendNotificationSync);
  };
  entry.onEditorHide = [this] { entry.setVisible(false); };
  addChildComponent(entry);
}

void ValueCell::paint(juce::Graphics& graphics)
{
  drawOutlined(graphics,
               *this,
               isEnabled() ? getTextFromValue(getValue()) : juce::String("-"),
               getLocalBounds().removeFromBottom(textHeight));
}

void ValueCell::resized()
{
  juce::Slider::resized();
  entry.setBounds(getLocalBounds().removeFromBottom(textHeight));
}

// Not the base class: it resets to the default value, which would replace typing.
void ValueCell::mouseDoubleClick(const juce::MouseEvent&)
{
  if (!isEnabled())
    return;
  entry.setText(getTextFromValue(getValue()), juce::dontSendNotification);
  entry.setVisible(true);
  entry.showEditor();
}

TypeCell::TypeCell()
{
  // The text is drawn by paint(), with its outline.
  for (const auto id : {juce::ComboBox::textColourId,
                        juce::ComboBox::backgroundColourId,
                        juce::ComboBox::outlineColourId,
                        juce::ComboBox::arrowColourId})
    setColour(id, juce::Colours::transparentBlack);
  setRepaintsOnMouseActivity(true);
}

void TypeCell::paint(juce::Graphics& graphics)
{
  drawOutlined(graphics, *this, getText(), getLocalBounds());
}

SwitchCell::SwitchCell(const juce::String& title, const juce::String& on, const juce::String& off)
    : juce::Button(title)
    , onText(on)
    , offText(off)
{
  setTitle(title);
  setClickingTogglesState(true);
}

void SwitchCell::paintButton(juce::Graphics& graphics, bool, bool)
{
  drawOutlined(graphics,
               *this,
               getToggleState() ? onText : offText,
               getLocalBounds().removeFromBottom(15));
}

} // namespace DtBlkFxUi

DtBlkFxAudioProcessorEditor::DtBlkFxAudioProcessorEditor(DtBlkFxAudioProcessor& processor)
    : AudioProcessorEditor(&processor)
    , processorRef(processor)
    , globalImage(juce::ImageCache::getFromMemory(BinaryData::stereo_global_ctrl_png,
                                                  BinaryData::stereo_global_ctrl_pngSize))
    , splashImage(juce::ImageCache::getFromMemory(BinaryData::stereo_splash_png,
                                                  BinaryData::stereo_splash_pngSize))
    , rowImage(juce::ImageCache::getFromMemory(BinaryData::stereo_fx_bg_png,
                                               BinaryData::stereo_fx_bg_pngSize))
{
  canvas.onPaint = [this](juce::Graphics& graphics) { paintCanvas(graphics); };
  canvas.setSize(baseWidth, baseHeight);
  canvas.addMouseListener(this, true);
  addAndMakeVisible(canvas);

  presetDropdown.setTextWhenNothingSelected("Presets");
  presetDropdown.onChange = [this] { loadPreset(presetDropdown.getSelectedId()); };
  factoryPresets = DtBlkFxFactory::parsePresets(DtBlkFxFactory::presetText());
  refreshPresets();
  presetDropdown.setBounds(4, 3, 130, 22);
  canvas.addAndMakeVisible(presetDropdown);

  presetMenuButton.onClick = [this] { showPresetMenu(); };
  presetMenuButton.setBounds(136, 3, 24, 22);
  canvas.addAndMakeVisible(presetMenuButton);

  latency.addItemList(DtBlkFxAudioProcessor::latencyChoiceNames(), 1);
  latency.setTitle("Latency (max block size)");
  latency.setBounds(210, 3, 102, 22);
  canvas.addAndMakeVisible(latency);
  // The Block Size readout says when the latency limits it.
  latency.onChange = [this] { globalSliders[2].repaint(); };
  latencyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      processorRef.parameters, "latency", latency);

  parameterSync.setTitle("Parameter Sync");
  parameterSync.setBounds(314, 3, 94, 22);
  canvas.addAndMakeVisible(parameterSync);

  // The cells of the original's global strip, left to right.
  const std::array<juce::Component*, 6> globalCells{&globalSliders[0],
                                                    &powerMode,
                                                    &globalSliders[1],
                                                    &globalSliders[3],
                                                    &blockSync,
                                                    &globalSliders[2]};
  for (size_t i = 0; i < globalCells.size(); ++i) {
    globalCells[i]->setBounds(static_cast<int>(i) * 68, barHeight, 68, globalHeight);
    canvas.addAndMakeVisible(globalCells[i]);
  }

  const std::array<juce::String, 3> buttonIDs{"power_match", "block_sync", "parameter_sync"};
  const std::array<juce::Button*, 3> buttons{&powerMode, &blockSync, &parameterSync};
  for (size_t i = 0; i < buttons.size(); ++i)
    buttonAttachments[i] =
        std::make_unique<ButtonAttachment>(processorRef.parameters, buttonIDs[i], *buttons[i]);

  const std::array<juce::String, 4> globalNames{"Mix Back", "Delay", "Block Size", "Overlap"};
  const std::array<juce::String, 4> globalIDs{"mixback", "delay", "fft_len", "overlap"};
  for (size_t i = 0; i < globalSliders.size(); ++i) {
    globalSliders[i].setTitle(globalNames[i]);
    globalSliders[i].textHeight = 15; // below the label in the picture
    globalSliders[i].resized();
    globalSliderAttachments[i] =
        std::make_unique<SliderAttachment>(processorRef.parameters, globalIDs[i], globalSliders[i]);
  }

  // Set after the attachments, which install their own text functions.
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
  globalSliders[2].textFromValueFunction = [this, sampleRate](double value) {
    const auto limit =
        DtBlkFxAudioProcessor::latencySamplesForChoice(latency.getSelectedItemIndex()) + 1;
    return juce::String(DtBlkFxText::formatBlockSize(value, sampleRate())) +
           (DtBlkFxText::blockSizeSamples(value) > limit ? " *" : "");
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
  globalSliders[2].onValueChange = [this] { globalSliders[3].repaint(); };

  // Column edges of an effect row: frequency A and B, amplitude, type, value.
  constexpr std::array<int, 6> columns{0, 82, 164, 213, 311, baseWidth};
  const std::array<juce::String, 4> effectControlIDs{"freq_a", "freq_b", "amp", "value"};
  constexpr std::array<size_t, 4> effectControlColumns{0, 1, 2, 4};
  for (size_t slot = 0; slot < effectTypes.size(); ++slot) {
    const auto y = rowsTop + static_cast<int>(slot) * rowHeight;
    const auto slotName = "Slot " + juce::String(static_cast<int>(slot + 1)) + " ";

    const auto& effectNames = DtBlkFxAudioProcessor::effectTypeNames();
    effectTypes[slot].addItem(effectNames[9], 10);
    for (int type = 0; type <= 8; ++type)
      effectTypes[slot].addItem(effectNames[type], type + 1);
    for (int type = 11; type < effectNames.size(); ++type)
      effectTypes[slot].addItem(effectNames[type], type + 1);
    effectTypes[slot].setTitle(slotName + "Effect");
    effectTypes[slot].setBounds(columns[3], y, columns[4] - columns[3], rowHeight);

    for (size_t control = 0; control < effectSliders[slot].size(); ++control) {
      auto& slider = effectSliders[slot][control];
      const auto column = effectControlColumns[control];
      slider.setBounds(columns[column], y, columns[column + 1] - columns[column], rowHeight);
      // The row picture shows the values, so it follows them.
      slider.onValueChange = [this] { canvas.repaint(); };
      canvas.addAndMakeVisible(slider);
      effectSliderAttachments[slot][control] = std::make_unique<SliderAttachment>(
          processorRef.parameters,
          DtBlkFxAudioProcessor::fxParameterID(static_cast<int>(slot), effectControlIDs[control]),
          slider);
    }
    canvas.addAndMakeVisible(effectTypes[slot]);

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
    effectTypes[slot].onChange = [this, slot] {
      const auto selectedID = effectTypes[slot].getSelectedId();
      if (selectedID != 0)
        effectTypeAttachments[slot]->setValueAsCompleteGesture(static_cast<float>(selectedID - 1));
    };
    effectTypeAttachments[slot]->sendInitialUpdate();
  }

  // Read before setting the limits, which already resizes. The shape is fixed.
  const int width = juce::jlimit(
      baseWidth,
      baseWidth * maxScale,
      static_cast<int>(processorRef.parameters.state.getProperty("editorWidth", baseWidth * 3 / 2)));
  setResizable(true, true);
  setResizeLimits(baseWidth, baseHeight, baseWidth * maxScale, baseHeight * maxScale);
  getConstrainer()->setFixedAspectRatio(static_cast<double>(baseWidth) / baseHeight);
  setSize(width, juce::roundToInt(width * static_cast<double>(baseHeight) / baseWidth));
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
    // A preset sets the sound, not the window size.
    processorRef.parameters.state.setProperty("editorWidth", getWidth(), nullptr);
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

void DtBlkFxAudioProcessorEditor::updateEffectControls(size_t slot)
{
  const auto type = effectTypes[slot].getSelectedId() - 1;
  const auto used = usedEffectControls(type);
  const std::array<juce::String, 4> names{"Frequency A (Hz)",
                                          "Frequency B (Hz)",
                                          DtBlkFxText::effectUsesMixAmplitude(type) ? "Mix / Gain"
                                                                                    : "Gain",
                                          effectValueLabel(type)};
  const auto slotName = "Slot " + juce::String(static_cast<int>(slot + 1)) + " ";

  for (size_t control = 0; control < effectSliders[slot].size(); ++control) {
    effectSliders[slot][control].setEnabled(used[control]);
    effectSliders[slot][control].setTitle(slotName + names[control]);
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
  }
  canvas.repaint();
}

void DtBlkFxAudioProcessorEditor::paintCanvas(juce::Graphics& graphics)
{
  graphics.fillAll(juce::Colour(0xff17191d));
  graphics.setColour(juce::Colours::white);
  graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
  graphics.drawText("Latency", 162, 0, 46, barHeight, juce::Justification::centredRight);
  graphics.drawImageAt(globalImage, 0, barHeight);
  graphics.drawImageAt(splashImage, 0, barHeight + globalHeight);
  for (size_t slot = 0; slot < effectTypes.size(); ++slot)
    paintEffectRow(graphics, slot);
}

// The row picture holds five strips: plain, row under the mouse, control under the mouse,
// the amplitude ramp and the frequency scale.
void DtBlkFxAudioProcessorEditor::paintEffectRow(juce::Graphics& graphics, size_t slot)
{
  const auto y = rowsTop + static_cast<int>(slot) * rowHeight;
  const auto typeOf = [this](size_t index) { return effectTypes[index].getSelectedId() - 1; };
  const auto type = typeOf(slot);
  const auto used = usedEffectControls(type);
  const auto& controls = effectSliders[slot];

  const auto strip = [&](int index, int left, int right, float opacity) {
    if (right <= left)
      return;
    graphics.setOpacity(opacity);
    graphics.drawImage(rowImage,
                       left,
                       y,
                       right - left,
                       rowHeight,
                       left,
                       index * rowHeight,
                       right - left,
                       rowHeight);
  };

  // A mask effect limits the next one; with no effect to limit it does nothing.
  const auto mask = isMaskEffect(type) && slot + 1 < effectTypes.size() &&
                    !isMaskEffect(typeOf(slot + 1));

  const juce::Component* hovered = nullptr;
  if (!isMaskEffect(type) || mask) {
    for (const auto& control : controls)
      if (control.isMouseOverOrDragging(true))
        hovered = &control;
    if (effectTypes[slot].isMouseOverOrDragging(true))
      hovered = &effectTypes[slot];
  }

  strip(hovered != nullptr ? 1 : 0, 0, baseWidth, 1.0f);
  if (hovered != nullptr)
    strip(2, hovered->getX(), hovered->getRight(), 1.0f);

  // Join a mask row to the row below: copy the second last line over the separator.
  if (mask) {
    graphics.setOpacity(1.0f);
    graphics.drawImage(
        rowImage, 0, y + rowHeight - 1, baseWidth, 1, 0, rowHeight - 2, baseWidth, 1);
  }

  const auto position = [](double value) {
    return juce::roundToInt(5.0 + value * (baseWidth - 10));
  };

  strip(3,
        0,
        position(controls[2].getValue()),
        !used[2]                                 ? 0.25f
        : controls[2].isMouseOverOrDragging(true) ? 1.0f
                                                  : 0.5f);

  // The range between the two frequencies, or outside them when A is above B.
  const auto a = controls[0].getValue();
  const auto b = controls[1].getValue();
  const auto scaleOpacity = used[0] || used[1] ? 1.0f : 0.5f;
  if (a <= b)
    strip(4, position(a), position(b), scaleOpacity);
  else {
    strip(4, 0, position(b), scaleOpacity);
    strip(4, position(a), baseWidth, scaleOpacity);
  }
}

void DtBlkFxAudioProcessorEditor::resized()
{
  canvas.setTransform(juce::AffineTransform::scale(
      juce::jmin(static_cast<float>(getWidth()) / baseWidth,
                 static_cast<float>(getHeight()) / baseHeight)));
  // Saved with the project, so the window opens at the same size again.
  processorRef.parameters.state.setProperty("editorWidth", getWidth(), nullptr);
}
