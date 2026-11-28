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

#include "DtBlkFxSpectrum.h"
#include "DtBlkFxValueText.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

// The widgets of the original look: text painted over the background pictures, in the original's
// pixel coordinates.
namespace DtBlkFxUi {

// The original's size, plus a slim bar above it for what the original did not have.
constexpr int baseWidth = 410;
constexpr int barHeight = 20;
constexpr int globalHeight = 26;
constexpr int splashHeight = 191;
constexpr int rowHeight = 24;
constexpr int rowsTop = barHeight + globalHeight + splashHeight;
constexpr int baseHeight = rowsTop + 8 * rowHeight;
// Each spectrogram: a line of frame, the picture, a line of frame and the octave ticks.
constexpr int spectrogramHeight = splashHeight / 2;

// The two sliders that pop up while a value is dragged: coarse below it, fine above it. The one
// in use follows the mouse sideways; moving up or down changes over.
class PopupSliders : public juce::Component {
public:
  PopupSliders();
  void open(juce::Rectangle<int> cell, double value, juce::Point<float> press);
  // Returns the new value for the mouse position.
  double drag(juce::Point<float> position);
  void close();
  void paint(juce::Graphics&) override;

private:
  enum Slider { none = -1, coarse, fine };
  const std::array<juce::Image, 2> bars, knobs;
  std::array<juce::Rectangle<int>, 2> area;
  juce::Point<float> pressed;
  float lastX = 0.0f;
  double current = 0.0;
  double fineStart = 0.0; // the value at the left end of the fine slider
  Slider active = none;
};

// A value of the original: drag down for the coarse slider or up for the fine one, right-click
// for its list, double-click to type. Wheel (Ctrl: fine), keys and accessibility are Slider's.
class ValueCell : public juce::Slider {
public:
  ValueCell();
  void paint(juce::Graphics&) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDrag(const juce::MouseEvent&) override;
  void mouseUp(const juce::MouseEvent&) override;
  void mouseDoubleClick(const juce::MouseEvent&) override;
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
  // Ends a drag. Also needed before either cell of a pair is destroyed.
  void release();
  // One complete change, as the host sees it.
  void choose(double value);

  PopupSliders* popups = nullptr;
  std::function<std::vector<DtBlkFxText::Choice>()> choices;
  // Rounds a dragged value to the ones that exist.
  std::function<double(double)> snap;
  // Frequency A and B: right-drag or Shift-drag moves both.
  ValueCell* partner = nullptr;
  int textHeight = rowHeight; // the text sits in this much of the bottom of the cell

private:
  void showChoices();

  juce::Label entry;
  std::unique_ptr<juce::Slider::ScopedDragNotification> gesture, partnerGesture;
  double startValue = 0.0, partnerStartValue = 0.0;
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

// A scrolling waterfall, newest line at the bottom, starting as the splash picture. Hovering
// shows the note and frequency; a click chooses what it shows or pauses it.
class Spectrogram : public juce::Component {
public:
  // top: where it sits on the splash picture. source: 0..2 input L+R, left, right; 3..5 output.
  Spectrogram(const juce::Image& splash, int top, int source);
  void setStereo(bool isStereo) { stereo = isStereo; }
  void addLine(const DtBlkFxSpectrum::Line&);
  // Column ranges shown inverted.
  void setHighlight(juce::Range<int> first, juce::Range<int> second = {});
  // The column of the vertical line, or -1.
  void setMarker(int column);
  // The frequency named at the top left, or below 0 for none.
  void setReadout(double hz);

  void paint(juce::Graphics&) override;
  void mouseMove(const juce::MouseEvent&) override;
  void mouseExit(const juce::MouseEvent&) override;
  void mouseDown(const juce::MouseEvent&) override;

  // Called with the hovered column, or -1 on leaving.
  std::function<void(int)> onHover;

private:
  juce::String name() const;

  juce::Image image;
  std::array<juce::Range<int>, 2> highlight;
  double readout = -1.0;
  int marker = -1;
  int source;
  bool stereo = true;
};

// White text with a black outline, pink when owner is in use. No owner: always white.
void drawOutlined(juce::Graphics&,
                  const juce::Component* owner,
                  const juce::String& text,
                  juce::Rectangle<int> area);

} // namespace DtBlkFxUi
