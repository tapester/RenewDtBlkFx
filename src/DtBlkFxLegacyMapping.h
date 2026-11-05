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

} // namespace DtBlkFxLegacy
