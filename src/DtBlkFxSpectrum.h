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

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

// Spectrogram lines on their way from the audio thread to the editor, as the original made them.
// One writer and one reader; no locks and no allocation. Free of JUCE so the engine tests can
// check it.
class DtBlkFxSpectrum {
public:
  // One column per pixel of the original display, on the frequency scale of Frequency A and B.
  static constexpr int columns = 400;
  static constexpr size_t minimumSamplesPerLine = 600;

  // The largest bin power in each column; a full-scale sine reads 0.25.
  struct Line {
    std::array<float, columns> input{};
    std::array<float, columns> output{};
  };

  // Set by the reader; the engine does no work for the spectrogram while this is false.
  std::atomic<bool> enabled{false};

  // 0 (-80 dB, black) to 1 (-14 dB, red), the original's colour range.
  static float level(float power) noexcept
  {
    constexpr float minimum = 1e-8f;
    constexpr float maximum = 0.04f;
    if (!(power > minimum))
      return 0.0f;
    return std::min(1.0f, std::log(power / minimum) / std::log(maximum / minimum));
  }

  // Writer: keep the largest power of each column until the line is done. A column without a
  // bin of its own shows the nearest one.
  template <typename Bin>
  void add(bool output,
           const Bin* bins,
           int binCount,
           int fftSize,
           double sampleRate,
           float powerScale) noexcept
  {
    auto& line = output ? pending.output : pending.input;
    // Column edges lie half a column below each column's frequency.
    const auto step = std::exp2(127.5 / 12.0 / columns);
    auto edge = c0Frequency * std::sqrt(step) * fftSize / sampleRate;
    int first = 0;
    for (int column = 0; column < columns; ++column, edge *= step) {
      const auto next = column + 1 < columns
                            ? std::clamp(static_cast<int>(edge + 0.5), first, binCount - 1)
                            : binCount;
      auto power = 0.0f;
      for (int bin = first; bin < std::max(next, first + 1); ++bin)
        power = std::max(power,
                         bins[bin].real * bins[bin].real + bins[bin].imaginary * bins[bin].imaginary);
      line[static_cast<size_t>(column)] =
          std::max(line[static_cast<size_t>(column)], power * powerScale);
      first = next;
    }
  }

  // Writer: hand the line over once it covers enough audio. Dropped if the reader is behind.
  void frameDone(size_t frameEnd) noexcept
  {
    // A smaller end than before means the engine was reset.
    if (frameEnd >= lineEnd && frameEnd - lineEnd < minimumSamplesPerLine)
      return;
    lineEnd = frameEnd;
    const auto write = writeCount.load(std::memory_order_relaxed);
    if (write - readCount.load(std::memory_order_acquire) < lines.size()) {
      lines[write % lines.size()] = pending;
      writeCount.store(write + 1, std::memory_order_release);
    }
    pending = {};
  }

  // Reader.
  bool pop(Line& line) noexcept
  {
    const auto read = readCount.load(std::memory_order_relaxed);
    if (read == writeCount.load(std::memory_order_acquire))
      return false;
    line = lines[read % lines.size()];
    readCount.store(read + 1, std::memory_order_release);
    return true;
  }

private:
  static constexpr double c0Frequency = 16.35159898333181161;

  Line pending;
  size_t lineEnd = 0;
  std::vector<Line> lines{128}; // on the heap: too large for a stack
  std::atomic<size_t> writeCount{0};
  std::atomic<size_t> readCount{0};
};
