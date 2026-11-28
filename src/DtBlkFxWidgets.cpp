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
#include "DtBlkFxWidgets.h"

#include "BinaryData.h"

namespace DtBlkFxUi {
namespace {
constexpr int sliderWidth = 205; // pixels from 0 to 1 on the coarse slider
constexpr int sliderHeight = 19;
constexpr int sliderBorder = 2;         // around the active area in the pictures
constexpr double fineMagnify = 15.0;    // the fine slider covers 1/15 of the range
constexpr float changeDistance = 12.0f; // up or down, to change to the other slider
constexpr int spectrogramRows = spectrogramHeight - 3;
constexpr int spectrogramLeft = (baseWidth - DtBlkFxSpectrum::columns) / 2;

// Black, blue, cyan, green, yellow, red.
juce::Colour spectrogramColour(float level)
{
  static const std::array<juce::Colour, 6> stops{juce::Colour(0xff000000),
                                                 juce::Colour(0xff0000ff),
                                                 juce::Colour(0xff00ffff),
                                                 juce::Colour(0xff00cc00),
                                                 juce::Colour(0xffffff00),
                                                 juce::Colour(0xffff0000)};
  const auto position = level * 5.0f;
  const auto index = juce::jlimit(0, 4, static_cast<int>(position));
  return stops[static_cast<size_t>(index)].interpolatedWith(stops[static_cast<size_t>(index + 1)],
                                                            position - static_cast<float>(index));
}

juce::Image picture(const char* data, int size)
{
  return juce::ImageCache::getFromMemory(data, size);
}
} // namespace

void drawOutlined(juce::Graphics& graphics,
                  const juce::Component* owner,
                  const juce::String& text,
                  juce::Rectangle<int> area)
{
  graphics.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
  const auto draw = [&](int dx, int dy) {
    graphics.drawFittedText(text, area.translated(dx, dy), juce::Justification::centred, 1, 0.6f);
  };
  graphics.setColour(juce::Colours::black);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx != 0 || dy != 0)
        draw(dx, dy);
  const auto active = owner != nullptr && owner->isEnabled() &&
                      (owner->isMouseOverOrDragging(true) || owner->hasKeyboardFocus(true));
  graphics.setColour(active ? juce::Colour(0xffff8080) : juce::Colours::white);
  draw(0, 0);
}

PopupSliders::PopupSliders()
    : bars{picture(BinaryData::stereo_hslider_bg_coarse_png,
                   BinaryData::stereo_hslider_bg_coarse_pngSize),
           picture(BinaryData::stereo_hslider_bg_fine_png,
                   BinaryData::stereo_hslider_bg_fine_pngSize)}
    , knobs{picture(BinaryData::stereo_hslider_knob_coarse_png,
                    BinaryData::stereo_hslider_knob_coarse_pngSize),
            picture(BinaryData::stereo_hslider_knob_fine_png,
                    BinaryData::stereo_hslider_knob_fine_pngSize)}
{
  setInterceptsMouseClicks(false, false);
}

void PopupSliders::open(juce::Rectangle<int> cell, double value, juce::Point<float> press)
{
  pressed = press;
  lastX = press.x;
  current = value;
  fineStart = value - 0.5 / fineMagnify;
  active = none;
  // Under the mouse sideways, but kept inside the window.
  const auto x = juce::jlimit(sliderBorder,
                              getWidth() - sliderWidth - sliderBorder,
                              juce::roundToInt(press.x) - sliderWidth / 2);
  const auto limit = getHeight() - sliderHeight - sliderBorder;
  area[coarse] = {x, juce::jmin(cell.getBottom() - sliderBorder, limit), sliderWidth, sliderHeight};
  area[fine] = {x, juce::jmax(sliderBorder, cell.getY() - 21), sliderWidth, sliderHeight};
}

double PopupSliders::drag(juce::Point<float> position)
{
  const auto dy = position.y - pressed.y;
  const auto before = active;
  if (active == none)
    active = dy < 0.0f ? fine : dy > 1.0f ? coarse : none;
  else if (dy < -changeDistance)
    active = fine;
  else if (dy > changeDistance)
    active = coarse;

  if (active != none) {
    const auto scale = active == fine ? sliderWidth * fineMagnify : sliderWidth;
    current = juce::jlimit(0.0, 1.0, current + (position.x - lastX) / scale);
    fineStart = juce::jlimit(current - 1.0 / fineMagnify, current, fineStart);
    setVisible(true);
    toFront(false);
    repaint();
  }
  else if (before == none)
    return current; // keep the sideways movement for the first slider
  lastX = position.x;
  return current;
}

void PopupSliders::close()
{
  active = none;
  setVisible(false);
}

void PopupSliders::paint(juce::Graphics& graphics)
{
  for (int i = coarse; i <= fine; ++i) {
    const auto index = static_cast<size_t>(i);
    // Each picture holds the plain strip above the highlighted one.
    const auto tile = active == i ? 1 : 0;
    const auto height = bars[index].getHeight() / 2;
    const auto box = area[index].expanded(sliderBorder);
    graphics.setOpacity(1.0f);
    graphics.drawImage(bars[index],
                       box.getX(),
                       box.getY(),
                       box.getWidth(),
                       height,
                       0,
                       tile * height,
                       box.getWidth(),
                       height);

    const auto position = i == coarse ? current : (current - fineStart) * fineMagnify;
    const auto width = knobs[index].getWidth();
    const auto x = area[index].getX() + juce::roundToInt(position * sliderWidth) - width / 2;
    graphics.drawImage(knobs[index], x, box.getY(), width, height, 0, tile * height, width, height);
  }
}

ValueCell::ValueCell()
{
  setSliderStyle(juce::Slider::RotaryHorizontalDrag);
  setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
  setRepaintsOnMouseActivity(true);

  entry.setFont(juce::Font(juce::FontOptions(12.0f)));
  entry.setJustificationType(juce::Justification::centred);
  entry.setColour(juce::Label::backgroundWhenEditingColourId, juce::Colours::white);
  entry.setColour(juce::Label::textWhenEditingColourId, juce::Colours::black);
  entry.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::black);
  entry.setColour(juce::Label::textColourId, juce::Colours::transparentBlack);
  entry.onTextChange = [this] { choose(getValueFromText(entry.getText())); };
  entry.onEditorHide = [this] { entry.setVisible(false); };
  addChildComponent(entry);
}

void ValueCell::paint(juce::Graphics& graphics)
{
  drawOutlined(graphics,
               this,
               isEnabled() ? getTextFromValue(getValue()) : juce::String("-"),
               getLocalBounds().removeFromBottom(textHeight));
}

void ValueCell::resized()
{
  juce::Slider::resized();
  entry.setBounds(getLocalBounds().removeFromBottom(textHeight));
}

void ValueCell::choose(double value)
{
  const juce::Slider::ScopedDragNotification change(*this);
  setValue(value, juce::sendNotificationSync);
}

// The mouse handling replaces Slider's.
void ValueCell::mouseDown(const juce::MouseEvent& event)
{
  gesture.reset();
  partnerGesture.reset();
  if (!isEnabled())
    return;

  if (partner != nullptr && partner->isEnabled() &&
      (event.mods.isPopupMenu() || event.mods.isShiftDown())) {
    startValue = getValue();
    partnerStartValue = partner->getValue();
    gesture = std::make_unique<juce::Slider::ScopedDragNotification>(*this);
    partnerGesture = std::make_unique<juce::Slider::ScopedDragNotification>(*partner);
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  }
  else if (event.mods.isPopupMenu())
    showChoices();
  else if (popups != nullptr)
    popups->open(getBounds(), getValue(), event.position + getPosition().toFloat());
}

void ValueCell::mouseDrag(const juce::MouseEvent& event)
{
  if (!isEnabled() || popups == nullptr)
    return;

  if (partnerGesture != nullptr) {
    // The spectrogram's width covers the whole range.
    const auto change =
        event.getDistanceFromDragStartX() / static_cast<double>(DtBlkFxSpectrum::columns);
    setValue(startValue + change, juce::sendNotificationSync);
    partner->setValue(partnerStartValue + change, juce::sendNotificationSync);
    return;
  }
  if (event.mods.isPopupMenu())
    return;

  const auto value = popups->drag(event.position + getPosition().toFloat());
  if (!popups->isVisible())
    return;
  if (gesture == nullptr)
    gesture = std::make_unique<juce::Slider::ScopedDragNotification>(*this);
  setValue(snap != nullptr ? snap(value) : value, juce::sendNotificationSync);
}

void ValueCell::mouseUp(const juce::MouseEvent&)
{
  release();
}

void ValueCell::release()
{
  gesture.reset();
  partnerGesture.reset();
  setMouseCursor(juce::MouseCursor::NormalCursor);
  if (popups != nullptr)
    popups->close();
}

// Not the base class: it resets to the default value, which would replace typing.
void ValueCell::mouseDoubleClick(const juce::MouseEvent& event)
{
  if (!isEnabled() || event.mods.isPopupMenu() || event.mods.isShiftDown())
    return;
  entry.setText(getTextFromValue(getValue()), juce::dontSendNotification);
  entry.setVisible(true);
  entry.showEditor();
}

void ValueCell::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
  auto scaled = wheel;
  if (event.mods.isCtrlDown() || event.mods.isCommandDown()) {
    scaled.deltaX /= static_cast<float>(fineMagnify);
    scaled.deltaY /= static_cast<float>(fineMagnify);
  }
  juce::Slider::mouseWheelMove(event, scaled);
}

void ValueCell::showChoices()
{
  if (choices == nullptr)
    return;
  const auto list = choices();
  if (list.empty())
    return;

  juce::PopupMenu menu;
  for (size_t i = 0; i < list.size(); ++i)
    menu.addItem(static_cast<int>(i) + 1,
                 juce::String(list[i].label),
                 true,
                 !list[i].relative && std::abs(list[i].value - getValue()) < 0.0002);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                     [cell = juce::Component::SafePointer<ValueCell>(this), list](int result) {
                       if (cell != nullptr && result > 0)
                         cell->choose(list[static_cast<size_t>(result - 1)].value);
                     });
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
  drawOutlined(graphics, this, getText(), getLocalBounds());
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
  drawOutlined(
      graphics, this, getToggleState() ? onText : offText, getLocalBounds().removeFromBottom(15));
}

Spectrogram::Spectrogram(const juce::Image& splash, int top, int initialSource)
    : image(juce::Image::RGB, DtBlkFxSpectrum::columns, spectrogramRows, true)
    , source(initialSource)
{
  setTitle(initialSource < 3 ? "Input spectrogram" : "Output spectrogram");
  juce::Graphics graphics(image);
  graphics.drawImageAt(splash, -spectrogramLeft, -top - 1);
}

void Spectrogram::addLine(const DtBlkFxSpectrum::Line& line)
{
  if (source > 5) // paused
    return;
  const auto& channels = source < 3 ? line.input : line.output;
  const auto which = source % 3; // L+R, left, right

  image.moveImageSection(0, 0, 0, 1, image.getWidth(), image.getHeight() - 1);
  juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
  for (int x = 0; x < image.getWidth(); ++x) {
    const auto column = static_cast<size_t>(x);
    const auto power = which == 1   ? channels[0][column]
                       : which == 2 ? channels[1][column]
                                    : juce::jmax(channels[0][column], channels[1][column]);
    pixels.setPixelColour(
        x, image.getHeight() - 1, spectrogramColour(DtBlkFxSpectrum::level(power)));
  }
  repaint();
}

void Spectrogram::setHighlight(juce::Range<int> first, juce::Range<int> second)
{
  if (highlight[0] == first && highlight[1] == second)
    return;
  highlight = {first, second};
  repaint();
}

void Spectrogram::setMarker(int column)
{
  if (std::exchange(marker, column) != column)
    repaint();
}

void Spectrogram::setReadout(double hz)
{
  if (!juce::exactlyEqual(std::exchange(readout, hz), hz))
    repaint();
}

juce::String Spectrogram::name() const
{
  if (source > 5)
    return "paused";
  const juce::String side = source < 3 ? "Input" : "Output";
  if (!stereo)
    return side;
  const std::array<const char*, 3> channels{" L+R", " Left", " Right"};
  return side + channels[static_cast<size_t>(source % 3)];
}

void Spectrogram::paint(juce::Graphics& graphics)
{
  graphics.fillAll(juce::Colours::black);
  graphics.setOpacity(1.0f);
  graphics.drawImageAt(image, spectrogramLeft, 1);

  // Highlighted columns and the mouse line show the picture inverted.
  const auto invert = [&](juce::Range<int> range) {
    range = range.getIntersectionWith({0, image.getWidth()});
    if (range.isEmpty())
      return;
    juce::Image negative(juce::Image::RGB, range.getLength(), image.getHeight(), false);
    {
      const juce::Image::BitmapData from(image, juce::Image::BitmapData::readOnly);
      juce::Image::BitmapData to(negative, juce::Image::BitmapData::writeOnly);
      for (int y = 0; y < negative.getHeight(); ++y)
        for (int x = 0; x < negative.getWidth(); ++x) {
          const auto colour = from.getPixelColour(range.getStart() + x, y);
          to.setPixelColour(x,
                            y,
                            juce::Colour(static_cast<juce::uint8>(255 - colour.getRed()),
                                         static_cast<juce::uint8>(255 - colour.getGreen()),
                                         static_cast<juce::uint8>(255 - colour.getBlue())));
        }
    }
    graphics.drawImageAt(negative, spectrogramLeft + range.getStart(), 1);
  };
  invert(highlight[0]);
  invert(highlight[1]);
  if (marker >= 0)
    invert({marker, marker + 1});

  // A tick at every octave from C0.
  const auto ticks = getHeight() - 1;
  graphics.setColour(juce::Colours::white);
  graphics.fillRect(0, ticks, getWidth(), 1);
  graphics.setColour(juce::Colours::black);
  for (int octave = 0; octave * 12.0 < 127.5; ++octave)
    graphics.fillRect(spectrogramLeft +
                          juce::roundToInt(octave * 12.0 / 127.5 * DtBlkFxSpectrum::columns),
                      ticks,
                      1,
                      1);

  if (readout >= 0.0) {
    const auto note =
        readout > 0.0
            ? DtBlkFxText::formatNote(12.0 * std::log2(readout / DtBlkFxText::c0Frequency))
            : std::string("-");
    drawOutlined(graphics, nullptr, note, {22, 3, 64, 18});
    drawOutlined(graphics, nullptr, DtBlkFxText::shortNumber(readout, 1.0) + "Hz", {86, 3, 66, 18});
  }
  drawOutlined(graphics, nullptr, name(), {280, 3, 108, 18});
}

void Spectrogram::mouseMove(const juce::MouseEvent& event)
{
  const auto column = juce::jlimit(0, DtBlkFxSpectrum::columns - 1, event.x - spectrogramLeft);
  setReadout(DtBlkFxText::c0Frequency *
             std::pow(2.0, column / static_cast<double>(DtBlkFxSpectrum::columns) * 127.5 / 12.0));
  if (onHover != nullptr)
    onHover(column);
}

void Spectrogram::mouseExit(const juce::MouseEvent&)
{
  setReadout(-1.0);
  if (onHover != nullptr)
    onHover(-1);
}

void Spectrogram::mouseDown(const juce::MouseEvent&)
{
  juce::PopupMenu menu;
  const auto add = [&](int choice, const juce::String& text) {
    menu.addItem(choice + 1, text, true, source == choice);
  };
  if (stereo) {
    const std::array<const char*, 6> names{
        "Input L+R", "Input Left", "Input Right", "Output L+R", "Output Left", "Output Right"};
    for (int i = 0; i < 6; ++i)
      add(i, names[static_cast<size_t>(i)]);
  }
  else {
    add(0, "Input");
    add(3, "Output");
  }
  add(6, "Paused");
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                     [view = juce::Component::SafePointer<Spectrogram>(this)](int result) {
                       if (view == nullptr || result <= 0)
                         return;
                       view->source = result - 1;
                       view->repaint();
                     });
}

} // namespace DtBlkFxUi
