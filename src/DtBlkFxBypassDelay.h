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
#include <cstddef>
#include <vector>

// Keeps the recent input so that bypassed audio can be delayed by the reported latency.
// Free of JUCE so the engine test executable can use it. Up to two channels.
class DtBlkFxBypassDelay {
public:
  // Allocates; the capacity has to cover the longest delay plus the largest block.
  void prepare(std::size_t capacity)
  {
    for (auto& ring : rings)
      ring.assign(capacity, 0.0f);
    position = 0;
  }

  // Call with every block's input, bypassed or not, before anything overwrites it.
  void write(const float* const* channels, int channelCount, int sampleCount) noexcept
  {
    const auto capacity = rings[0].size();
    if (capacity == 0)
      return;
    for (int sample = 0; sample < sampleCount; ++sample) {
      for (int channel = 0; channel < channelCount; ++channel)
        rings[static_cast<std::size_t>(channel)][position] = channels[channel][sample];
      position = (position + 1) % capacity;
    }
  }

  // Replaces the block passed to the last write() with the input from `delay` samples earlier.
  // Leaves the block alone if prepare() has not been called.
  void read(float* const* channels, int channelCount, int sampleCount, int delay) const noexcept
  {
    const auto capacity = rings[0].size();
    if (capacity == 0)
      return;
    const auto back = static_cast<std::size_t>(sampleCount) + static_cast<std::size_t>(delay);
    for (int channel = 0; channel < channelCount; ++channel) {
      // shortcut: a block that does not fit is silenced, enlarge the capacity if a host sends one.
      if (back > capacity) {
        std::fill_n(channels[channel], sampleCount, 0.0f);
        continue;
      }
      const auto& ring = rings[static_cast<std::size_t>(channel)];
      for (int sample = 0; sample < sampleCount; ++sample)
        channels[channel][sample] =
            ring[(position + capacity - back + static_cast<std::size_t>(sample)) % capacity];
    }
  }

private:
  std::array<std::vector<float>, 2> rings;
  std::size_t position = 0;
};
