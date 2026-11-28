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

#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

// The editor's readouts and typed values. Free of JUCE so the engine tests can check them.
// Numbers always use a decimal point, whatever the host's locale is.
namespace DtBlkFxText {

constexpr double c0Frequency = 16.35159898333181161;

inline std::string fixed(double value, int decimals, bool showSign = false)
{
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  if (showSign)
    stream << std::showpos;
  stream << std::fixed << std::setprecision(decimals) << value;
  return stream.str();
}

inline std::string rounded(double value)
{
  return std::to_string(std::lrint(value));
}

// The number at the start of the text, or 0 if there is none.
inline double typedNumber(const std::string& text)
{
  const auto start = std::min(text.find_first_not_of(" \t"), text.size());
  auto end = start;
  if (end < text.size() && (text[end] == '+' || text[end] == '-'))
    ++end;
  end = std::min(text.find_first_not_of("0123456789.", end), text.size());
  std::istringstream stream(text.substr(start, end - start));
  stream.imbue(std::locale::classic());
  double value = 0.0;
  return stream >> value ? value : 0.0;
}

inline std::string lowerCase(std::string text)
{
  for (auto& character : text)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return text;
}

// The word arguments of these three are lower case.
inline bool contains(const std::string& text, const char* word)
{
  return lowerCase(text).find(word) != std::string::npos;
}

inline bool startsWith(const std::string& text, const char* word)
{
  return lowerCase(text).rfind(word, 0) == 0;
}

inline bool endsWith(const std::string& text, const std::string& word)
{
  const auto lowered = lowerCase(text);
  return lowered.size() >= word.size() &&
         lowered.compare(lowered.size() - word.size(), word.size(), word) == 0;
}

// Some readouts put words first ("below 40%", "copy0/50%"), so the typed number is the last one.
inline std::string lastTypedNumber(const std::string& text)
{
  const auto last = text.find_last_of("0123456789");
  if (last == std::string::npos)
    return {};
  const auto before = text.find_last_not_of("0123456789.,+-", last);
  const auto start = before == std::string::npos ? 0 : before + 1;
  return text.substr(start, last + 1 - start);
}

// The original's number format: three digits with u, m, k or M ("81.3m", "1.51k"). Only units
// whose limit lies above minUnit are used.
inline std::string shortNumber(double value, double minUnit = 0.0)
{
  static const std::array<const char*, 5> units{"u", "m", "", "k", "M"};
  int step = 0;
  auto limit = 1e-5;
  while (step < 14 && !(limit > minUnit * 1.5 && std::abs(value) < limit)) {
    ++step;
    limit *= 10.0;
  }
  return fixed(value * std::pow(1000.0, 2 - step / 3), 2 - step % 3) +
         units[static_cast<size_t>(step / 3)];
}

// Seconds from typed text such as "81.3msec", "20 ms" or "1.5 sec".
inline double typedSeconds(const std::string& text)
{
  return typedNumber(text) * (contains(text, "ms") ? 1e-3 : contains(text, "us") ? 1e-6 : 1.0);
}

inline std::string formatFrequency(double normalized)
{
  if (normalized <= 0.0)
    return "0Hz";

  return shortNumber(c0Frequency * std::pow(2.0, normalized * 127.5 / 12.0), 1.0) + "Hz";
}

inline double parseFrequency(const std::string& text)
{
  const auto frequency = typedNumber(text) * (contains(text, "khz") ? 1000.0 : 1.0);
  return frequency <= 0.0 ? 0.0
                          : std::clamp(12.0 * std::log2(frequency / c0Frequency) / 127.5, 0.0, 1.0);
}

inline bool effectUsesMixAmplitude(int type)
{
  return type == 5 || type == 6 || type == 7 || type == 8 || type == 11 || type == 12 ||
         (type >= 15 && type <= 19) || (type >= 26 && type <= 28) || type == 30;
}

inline std::string formatAmplitude(int type, double normalized)
{
  if (effectUsesMixAmplitude(type) && normalized <= 0.6)
    return fixed(normalized * 100.0 / 0.6, 1) + " %";
  if (normalized <= 0.0)
    return "-inf dB";
  return fixed(-60.0 + normalized * 100.0, 1) + " dB";
}

inline double parseAmplitude(int type, const std::string& text)
{
  const auto value = typedNumber(text);
  if (contains(text, "%") && effectUsesMixAmplitude(type))
    return std::clamp(value * 0.006, 0.0, 0.6);
  if (contains(text, "inf"))
    return 0.0;
  return std::clamp((value + 60.0) / 100.0, 0.0, 1.0);
}

inline std::string formatNote(double semitonesFromC0)
{
  static const std::array<const char*, 12> noteNames{
      "c-", "c#", "d-", "d#", "e-", "f-", "f#", "g-", "g#", "a-", "a#", "b-"};
  const auto nearest = static_cast<int>(std::lrint(semitonesFromC0));
  const auto cents = static_cast<int>((semitonesFromC0 - nearest) * 100.0);
  const auto octave = nearest / 12;
  const auto note = (nearest % 12 + 12) % 12;
  char centsText[8];
  std::snprintf(centsText, sizeof(centsText), "%+03d", cents);
  return noteNames[static_cast<size_t>(note)] + std::to_string(octave) + ":" + centsText;
}

inline std::string formatEffectValue(int type, double normalized)
{
  static const std::array<const char*, 4> harmonicModes{"both", "odd", "even", "between"};
  normalized = std::clamp(normalized, 0.0, 1.0);

  switch (type) {
    case 0:
    case 9:
    case 10:
      return "-";
    case 1:
      return fixed((normalized * 2.0 - 1.0) * 100.0, 0, true) + "%";
    case 2:
    case 4:
      return rounded(normalized * 100.0) + "%";
    case 3:
    case 25: {
      const auto below = normalized >= 0.5;
      const auto fraction = below ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return std::string(below ? "below " : "above ") + rounded(fraction * 100.0) + "%";
    }
    case 5: {
      const auto scaled = normalized * 2.0;
      const auto direction = normalized >= 0.5 ? "rev" : "fwd";
      const auto fraction = normalized >= 1.0 ? 1.0 : scaled - std::min(std::floor(scaled), 1.0);
      const auto percent = std::pow(fraction * 4.0 - 2.0, 3.0) * 100.0;
      return fixed(percent, 0, true) + "% time " + direction;
    }
    case 6:
    case 7:
    case 11:
      return fixed((normalized - 0.5) * 72.0, 2, true) + " notes";
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
      return fixed(hz, 1) + " Hz";
    }
    case 12:
      // From 0.7 up HarmRepitch follows the right channel: -24..+24 notes. Float, as the engine.
      if (static_cast<float>(normalized) >= 0.7f)
        return "right " + fixed((normalized - 0.7) / 0.3 * 48.0 - 24.0, 2, true) + " notes";
      return formatNote(normalized * 127.5);
    case 13:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 27:
    case 28: {
      const auto scaled = normalized * 4.0;
      const auto mode = std::min(static_cast<int>(scaled), 3);
      const auto fraction = scaled - mode;
      return rounded(fraction * 100.0) + "% " + harmonicModes[static_cast<size_t>(mode)];
    }
    case 15:
    case 16:
    case 17:
    case 18:
    case 19: {
      const auto copy = normalized >= 0.5;
      const auto fraction = copy ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return std::string(copy ? "copy0/" : "scale/") + rounded(fraction * 100.0) + "%";
    }
    case 26:
      if (normalized > 0.875)
        return rounded((normalized - 0.875) * 800.0) + "% multiply";
      return rounded(normalized * (399.0 / 0.875) + 1.0) + " segs";
    case 29: {
      const auto rightOnLeft = normalized >= 0.5;
      const auto fraction = rightOnLeft ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return rounded((1.0 - fraction) * 100.0) + (rightOnLeft ? "% R on L" : "% L on R");
    }
    case 30: {
      const auto mode = normalized >= 0.5 ? 1 : 0;
      const auto fraction = normalized >= 0.5 ? (normalized - 0.5) * 2.0 : normalized * 2.0;
      return rounded(fraction * 100.0) + " mode " + std::to_string(mode);
    }
    default:
      return fixed(normalized, 3);
  }
}

inline double parseEffectValue(int type, const std::string& text)
{
  const auto value = typedNumber(text);
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
    case 5: {
      const auto scale = std::cbrt(value / 100.0);
      const auto fraction = std::clamp((scale + 2.0) * 0.25, 0.0, 1.0);
      return contains(text, "rev") ? std::clamp(0.5 + fraction * 0.5, 0.5, 1.0)
                                   : std::clamp(fraction * 0.5, 0.0, 0.4999);
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
        const auto display = typedNumber(formatEffectValue(type, middle));
        if (display < targetHz)
          low = middle;
        else
          high = middle;
      }
      return (low + high) * 0.5;
    }
    case 12: {
      if (startsWith(text, "right")) {
        const auto notes = typedNumber(lastTypedNumber(text));
        return std::clamp(0.7 + (notes + 24.0) / 48.0 * 0.3, 0.7, 1.0);
      }
      const auto lowered = lowerCase(text);
      static const std::array<const char*, 12> noteNames{
          "c-", "c#", "d-", "d#", "e-", "f-", "f#", "g-", "g#", "a-", "a#", "b-"};
      for (size_t note = 0; note < noteNames.size(); ++note) {
        if (lowered.rfind(noteNames[note], 0) != 0)
          continue;
        const auto rest = lowered.substr(2);
        const auto octave = std::trunc(typedNumber(rest));
        const auto colon = rest.find(':');
        const auto cents = colon == std::string::npos ? 0.0 : typedNumber(rest.substr(colon + 1));
        return std::clamp(
            (octave * 12.0 + static_cast<double>(note) + cents / 100.0) / 127.5, 0.0, 0.6999);
      }
      return std::clamp(value / 127.5, 0.0, 0.6999);
    }
    case 13:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 27:
    case 28: {
      auto mode = 0;
      if (endsWith(text, "odd"))
        mode = 1;
      else if (endsWith(text, "even"))
        mode = 2;
      else if (endsWith(text, "between"))
        mode = 3;
      // 100% must not turn into 0% of the next mode.
      return std::clamp(
          (mode + value / 100.0) * 0.25, mode * 0.25, mode == 3 ? 1.0 : (mode + 1) * 0.25 - 0.0001);
    }
    case 3:
    case 25:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19: {
      const auto typed = typedNumber(lastTypedNumber(text)) / 200.0;
      // The first half stops below 0.5, or 100% would flip to the other mode.
      return startsWith(text, "below") || startsWith(text, "copy0")
                 ? std::clamp(0.5 + typed, 0.5, 1.0)
                 : std::clamp(typed, 0.0, 0.4999);
    }
    case 26:
      return contains(text, "multiply") ? std::clamp(0.875 + value / 800.0, 0.8751, 1.0)
                                        : std::clamp((value - 1.0) * 0.875 / 399.0, 0.0, 0.875);
    case 29:
      return contains(text, "r on l") ? std::clamp(0.5 + (1.0 - value / 100.0) * 0.5, 0.5, 1.0)
                                      : std::clamp((1.0 - value / 100.0) * 0.5, 0.0, 0.4999);
    case 30:
      return contains(text, "mode 1") ? std::clamp(0.5 + value / 200.0, 0.5, 1.0)
                                      : std::clamp(value / 200.0, 0.0, 0.4999);
    default:
      return std::clamp(value, 0.0, 1.0);
  }
}

inline std::string formatMixBack(double value)
{
  return rounded(value * 100.0) + "%";
}

inline double parseMixBack(const std::string& text)
{
  return std::clamp(typedNumber(text) / 100.0, 0.0, 1.0);
}

inline std::string formatDelay(double value)
{
  if (value < 0.5)
    return fixed(DtBlkFxLegacy::legacyValueToDelayBeats(static_cast<float>(value)), 2) + " beats";
  return shortNumber((value - 0.5) * 12.0) + "sec";
}

inline double parseDelay(const std::string& text)
{
  const auto value = typedNumber(text);
  if (contains(text, "sec") || contains(text, "ms"))
    return std::clamp(0.5 + typedSeconds(text) / 12.0, 0.5, 1.0);
  return DtBlkFxLegacy::delayBeatsToLegacyValue(static_cast<float>(value));
}

// The FFT size selected by a Block Size parameter value.
inline int blockSizeSamples(double value)
{
  return DtBlkFxEngine::fftSizeAt(
      std::clamp(DtBlkFxLegacy::legacyValueToFftPlanIndex(static_cast<float>(value)),
                 0,
                 DtBlkFxEngine::fftSizeCount - 1));
}

inline std::string formatBlockSize(double value, double sampleRate)
{
  return shortNumber(blockSizeSamples(value) / sampleRate) + "sec";
}

inline double parseBlockSize(const std::string& text, double sampleRate)
{
  // A bare number is a size in samples.
  const auto requestedSize = contains(text, "sec") || contains(text, "ms")
                                 ? typedSeconds(text) * sampleRate
                                 : typedNumber(text);
  auto closestIndex = 0;
  auto closestDistance = std::numeric_limits<double>::max();
  for (int index = 0; index < DtBlkFxEngine::fftSizeCount; ++index) {
    const auto distance = std::abs(DtBlkFxEngine::fftSizeAt(index) - requestedSize);
    if (distance < closestDistance) {
      closestIndex = index;
      closestDistance = distance;
    }
  }
  return DtBlkFxLegacy::fftPlanIndexToLegacyValue(closestIndex);
}

// Overlap depends on the Block Size parameter value as well.
inline std::string formatOverlap(double value, double blockSizeValue)
{
  const auto blockSize = blockSizeSamples(blockSizeValue);
  const auto minimumHop = static_cast<double>(blockSize) * 0.15;
  const auto maximumHop = static_cast<double>(blockSize - 16);
  const auto hop = std::max(32.0, std::floor(maximumHop + value * (minimumHop - maximumHop)));
  return rounded((1.0 - hop / blockSize) * 100.0) + "%";
}

inline double parseOverlap(const std::string& text, double blockSizeValue)
{
  const auto blockSize = blockSizeSamples(blockSizeValue);
  const auto minimumHop = static_cast<double>(blockSize) * 0.15;
  const auto maximumHop = static_cast<double>(blockSize - 16);
  const auto requestedHop = blockSize * (1.0 - typedNumber(text) / 100.0);
  return std::clamp((maximumHop - requestedHop) / (maximumHop - minimumHop), 0.0, 1.0);
}

// ---- The original's right-click lists ----

struct Choice {
  std::string label;
  double value;
  bool relative = false; // a step from the current value, never shown as ticked
};

inline std::vector<Choice> effectValueChoices(int type, double current)
{
  std::vector<Choice> choices;
  const auto add = [&](double value) { choices.push_back({formatEffectValue(type, value), value}); };
  // Evenly spaced, both ends included.
  const auto spread = [&](int count, double first, double last) {
    for (int i = 0; i < count; ++i)
      add(first + (last - first) * i / (count - 1));
  };
  const auto steps = [&](const char* before, double perNote, double lowest, double highest) {
    for (const int notes : {-12, -9, -7, -5, -3, -2, -1, 1, 2, 3, 5, 7, 9, 12})
      choices.push_back({std::string(before) + (notes > 0 ? "+" : "") + std::to_string(notes) +
                             " notes",
                         std::clamp(current + notes * perNote, lowest, highest),
                         true});
  };

  switch (type) {
    case 1:
      spread(9, 0.0, 1.0);
      break;
    case 2:
    case 4:
      spread(5, 0.0, 1.0);
      break;
    case 3:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 25:
    case 29:
    case 30:
      spread(5, 0.0, 0.4999);
      spread(5, 0.5, 1.0);
      break;
    case 5:
      for (const double value :
           {0.0,      0.051575, 0.092510, 0.125,    0.150787, 0.171255, 0.191980,
            0.308020, 0.328745, 0.349213, 0.375,    0.407490, 0.448425, 0.4999,
            0.5,      0.551575, 0.592510, 0.625,    0.650787, 0.671255, 0.691980,
            0.808020, 0.828745, 0.849213, 0.875,    0.907490, 0.948425, 1.0})
        add(value);
      break;
    case 6:
    case 7:
    case 11:
      // 0, -12, +12, -24 and +24 notes, then steps.
      for (const int notes : {0, -12, 12, -24, 24})
        add(0.5 + notes / 72.0);
      steps("Change shift by ", 1.0 / 72.0, 0.0, 1.0);
      break;
    case 8:
      for (const double value :
           {0.321054, 0.389299, 0.473659, 0.476198, 0.478758, 0.481339, 0.483942,
            0.486566, 0.489211, 0.491877, 0.494564, 0.497272, 0.5,      0.502728,
            0.505436, 0.508123, 0.510789, 0.513434, 0.516058, 0.518661, 0.521242,
            0.523802, 0.526341, 0.610701, 0.678946})
        add(value);
      break;
    case 12:
      // C2 to C6, then following the right channel at 0, +12 and -12 notes.
      for (int octave = 2; octave <= 6; ++octave)
        add(12.0 * octave / 127.5);
      for (const double value : {0.85, 0.925, 0.775})
        add(value);
      if (static_cast<float>(current) >= 0.7f)
        steps("Change by ", 0.3 / 48.0, 0.7, 1.0);
      else
        steps("Change by ", 1.0 / 127.5, 0.0, 0.699);
      break;
    case 13:
    case 14:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 27:
    case 28:
      for (int mode = 0; mode < 4; ++mode)
        spread(5, mode * 0.25, mode == 3 ? 1.0 : mode * 0.25 + 0.2499);
      break;
    case 26:
      for (const int segments : {1, 10, 20, 40, 100, 200, 300})
        add((segments - 1) * 0.8749 / 399.0);
      for (const int percent : {0, 25, 50, 75, 100})
        add(0.875 + percent / 100.0 * 0.125);
      break;
    default:
      break;
  }
  return choices;
}

inline std::vector<Choice> amplitudeChoices(int type)
{
  std::vector<Choice> choices;
  const auto mix = effectUsesMixAmplitude(type);
  if (mix)
    for (int percent = 0; percent <= 100; percent += 25)
      choices.push_back({std::to_string(percent) + "%", percent * 0.006});
  else
    choices.push_back({"-inf", 0.0});
  for (int dB = mix ? 10 : -40; dB <= 40; dB += 10)
    choices.push_back({std::to_string(dB) + " dB", (dB + 60) / 100.0});
  return choices;
}

inline std::vector<Choice> mixBackChoices()
{
  std::vector<Choice> choices;
  for (int percent = 0; percent <= 100; percent += 25)
    choices.push_back({std::to_string(percent) + "%", percent / 100.0});
  return choices;
}

inline std::vector<Choice> overlapChoices()
{
  std::vector<Choice> choices{{"minimum", 0.0}};
  for (int percent = 10; percent <= 80; percent += 10)
    choices.push_back({std::to_string(percent) + "%", percent / 85.0});
  choices.push_back({"maximum", 1.0});
  return choices;
}

inline std::string describeLength(double samples, double sampleRate, double samplesPerBeat)
{
  return shortNumber(samples / sampleRate, 1e-3) + "sec, " + std::to_string(std::lrint(samples)) +
         " samps, (" + fixed(samples / samplesPerBeat, 2) + " beats)";
}

inline std::vector<Choice> blockSizeChoices(double sampleRate, double samplesPerBeat)
{
  std::vector<Choice> choices;
  for (int index = 0; index < DtBlkFxEngine::fftSizeCount; ++index)
    choices.push_back(
        {describeLength(DtBlkFxEngine::fftSizeAt(index), sampleRate, samplesPerBeat),
         DtBlkFxLegacy::fftPlanIndexToLegacyValue(index)});
  return choices;
}

// The current delay in the other unit, the current block size, then beat presets.
inline std::vector<Choice> delayChoices(double current,
                                        double blockSizeValue,
                                        double sampleRate,
                                        double samplesPerBeat)
{
  const auto timeValue = [&](double samples) {
    return std::clamp(0.5 + samples / sampleRate / 12.0, 0.5, 1.0);
  };
  const auto beatsValue = [](double beats) {
    return static_cast<double>(
        DtBlkFxLegacy::delayBeatsToLegacyValue(static_cast<float>(beats)));
  };
  const auto inBeats = current < 0.5;
  const auto samples =
      inBeats ? DtBlkFxLegacy::legacyValueToDelayBeats(static_cast<float>(current)) * samplesPerBeat
              : (current - 0.5) * 12.0 * sampleRate;

  std::vector<Choice> choices;
  choices.push_back(
      {(inBeats ? "As time: " : "As beats: ") + describeLength(samples, sampleRate, samplesPerBeat),
       inBeats ? timeValue(samples) : beatsValue(samples / samplesPerBeat),
       true});
  const auto block = static_cast<double>(blockSizeSamples(blockSizeValue));
  choices.push_back({"Block size: " + describeLength(block, sampleRate, samplesPerBeat),
                     timeValue(block),
                     true});
  for (const double beats :
       {0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0})
    choices.push_back({fixed(beats, 2) + " beats, (" +
                           std::to_string(static_cast<long>(beats * samplesPerBeat)) + " samps, " +
                           shortNumber(beats * samplesPerBeat / sampleRate, 1e-3) + "sec)",
                       beatsValue(beats)});
  return choices;
}

} // namespace DtBlkFxText
