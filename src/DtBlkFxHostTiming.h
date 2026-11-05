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

#include <cmath>
#include <cstdint>
#include <optional>

// Turns what a host reports about its transport (tempo, beat position, play state) into the
// two numbers the engine needs: samples per beat and the position inside the current beat.
// Header-only and free of JUCE so the engine tests can use it.
namespace DtBlkFxHost {

struct TransportInfo {
  // Empty when the host does not provide the value.
  std::optional<double> bpm;
  std::optional<double> ppqPosition;
  bool isPlaying = false;
};

struct BeatTiming {
  double samplesPerBeat = 0.0;
  // Samples from the start of the current beat to the start of this block.
  double beatPhaseSamples = 0.0;
  // True when this block's phase came from the host's beat position.
  bool followingHost = false;
};

class BeatTracker {
public:
  static constexpr double defaultBpm = 120.0;
  static constexpr double minimumBpm = 1.0;
  static constexpr double maximumBpm = 1000.0;

  // Call from prepareToPlay.
  void reset(double newSampleRate) noexcept
  {
    sampleRate = std::isfinite(newSampleRate) && newSampleRate > 0.0 ? newSampleRate : 44100.0;
    bpm = defaultBpm;
    blockStart = 0;
    gridStart = 0.0;
    lastPpq.reset();
  }

  static bool isUsableBpm(double value) noexcept
  {
    return std::isfinite(value) && value >= minimumBpm && value <= maximumBpm;
  }

  double getBpm() const noexcept { return bpm; }

  // Call once per audio block, with the block's length, before processing it. Like the legacy
  // plugin it keeps the last valid tempo, and it keeps the beat grid running when the host
  // gives no beat position (or the transport is stopped and the position does not move).
  BeatTiming update(const TransportInfo& info, int blockSamples) noexcept
  {
    const auto previousSamplesPerBeat = samplesPerBeat(bpm);
    const auto previousPhase =
        wrapPhase(static_cast<double>(blockStart) - gridStart, previousSamplesPerBeat);

    if (info.bpm.has_value() && isUsableBpm(*info.bpm))
      bpm = *info.bpm;
    const auto currentSamplesPerBeat = samplesPerBeat(bpm);

    // A tempo change keeps the position inside the beat (as a fraction), not in samples.
    if (currentSamplesPerBeat != previousSamplesPerBeat)
      gridStart = static_cast<double>(blockStart) -
                  previousPhase / previousSamplesPerBeat * currentSamplesPerBeat;

    const auto ppqUsable = info.ppqPosition.has_value() && std::isfinite(*info.ppqPosition);
    const auto followHost =
        ppqUsable && (info.isPlaying || !lastPpq.has_value() || *info.ppqPosition != *lastPpq);
    if (followHost) {
      const auto beatFraction = *info.ppqPosition - std::floor(*info.ppqPosition);
      gridStart = static_cast<double>(blockStart) - beatFraction * currentSamplesPerBeat;
    }
    if (ppqUsable)
      lastPpq = *info.ppqPosition;
    else
      lastPpq.reset();

    BeatTiming timing;
    timing.samplesPerBeat = currentSamplesPerBeat;
    timing.beatPhaseSamples =
        wrapPhase(static_cast<double>(blockStart) - gridStart, currentSamplesPerBeat);
    timing.followingHost = followHost;

    if (blockSamples > 0)
      blockStart += blockSamples;
    return timing;
  }

private:
  // Value wrapped into [0, period).
  static double wrapPhase(double value, double period) noexcept
  {
    auto result = std::fmod(value, period);
    if (result < 0.0)
      result += period;
    if (!(result < period)) // also catches NaN
      result = 0.0;
    return result;
  }

  double samplesPerBeat(double bpmValue) const noexcept { return sampleRate * 60.0 / bpmValue; }

  double sampleRate = 44100.0;
  double bpm = defaultBpm;
  std::int64_t blockStart = 0; // samples since reset(), at the start of the next block
  double gridStart = 0.0;      // sample position of a beat start, on the running grid
  std::optional<double> lastPpq;
};

} // namespace DtBlkFxHost
