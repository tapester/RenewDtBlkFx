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

// Conversions between host-facing parameter values and the engine's legacy 44-float block.
// Header-only and free of JUCE so the engine test executable can use it.
namespace DtBlkFxLegacy {

// Number of selectable effect types (indices 0..30).
constexpr int effectTypeCount = 31;

// Encode an effect-type index as the legacy normalised value.
// The +4 centres the value inside its 8/255-wide bucket so float rounding cannot change the index.
inline float effectTypeToLegacyValue(int index) noexcept
{
  const auto clamped = std::clamp(index, 0, effectTypeCount - 1);
  return static_cast<float>(clamped * 8 + 4) / 255.0f;
}

// Decode a legacy normalised value back to an effect-type index.
// This is the exact formula the engine has always used.
inline int legacyValueToEffectType(float value) noexcept
{
  return static_cast<int>(value * 255.0f) / 8;
}

// Legacy FFT-length value (parameter 2) to an index into the engine's FFT plan table.
// Not clamped: callers clamp to [0, DtBlkFxEngine::fftSizeCount - 1].
inline int legacyValueToFftPlanIndex(float value) noexcept
{
  return static_cast<int>(value * 255.0f / 4.0f - 1.5f);
}

// Mix Back (parameter 0). Values up to 0.5 mean Power Match mode (mix 0..1); above 0.5 mean
// Filter mode (mix 1..0). Encoder and decoders live together so they cannot drift apart.
inline float mixBackToLegacyValue(float mixBack, bool powerMatch) noexcept
{
  return powerMatch ? mixBack * 0.5f : 1.0f - mixBack * 0.499f;
}

inline bool legacyValueIsPowerMatch(float value) noexcept
{
  return value <= 0.5f;
}

inline float legacyValueToMixBack(float value) noexcept
{
  return legacyValueIsPowerMatch(value) ? value * 2.0f : (1.0f - value) * (1.0f / 0.499f);
}

// Delay (parameter 1) below 0.5 is in beats; 0.5 and above is in milliseconds.
constexpr float delayBeatsAtHalf = 0.5f * 255.0f / 16.0f;

inline float legacyValueToDelayBeats(float value) noexcept
{
  return value * 2.0f * delayBeatsAtHalf;
}

// Stops below 0.5 even after snapping to the parameter step, or a long delay would become 0 ms.
inline float delayBeatsToLegacyValue(float beats) noexcept
{
  return std::clamp(beats / (2.0f * delayBeatsAtHalf), 0.0f, 0.4999f);
}

} // namespace DtBlkFxLegacy
