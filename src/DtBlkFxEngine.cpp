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

#include "DtBlkFxEngine.h"
#include "DtBlkFxSweepTables.h"
#include "DtBlkFxLegacyMapping.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

DtBlkFxEngine::~DtBlkFxEngine()
{
  release();
}

bool DtBlkFxEngine::prepare(int fftSizeIndex, double sampleRate)
{
  release();
  if (fftSizeAt(fftSizeIndex) == 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0 ||
      sampleRate >
          static_cast<double>(std::numeric_limits<int>::max()) / maximumDelayMilliseconds * 1000.0)
    return false;

  timeBuffer = static_cast<float*>(fftwf_malloc(sizeof(float) * maxFftSize));
  frequencyBuffer = static_cast<float*>(fftwf_malloc(sizeof(float) * (maxFftSize + 2)));
  if (timeBuffer == nullptr || frequencyBuffer == nullptr) {
    release();
    return false;
  }

  // Build every supported plan up front because legacy automation can change FFT size.
  for (int i = 0; i < fftSizeCount; ++i) {
    const auto size = fftSizes[static_cast<size_t>(i)];
    auto* fftwFrequencyBuffer = reinterpret_cast<fftwf_complex*>(frequencyBuffer);
    plans[static_cast<size_t>(i)].forward =
        fftwf_plan_dft_r2c_1d(size, timeBuffer, fftwFrequencyBuffer, FFTW_ESTIMATE);
    plans[static_cast<size_t>(i)].inverse =
        fftwf_plan_dft_c2r_1d(size, fftwFrequencyBuffer, timeBuffer, FFTW_ESTIMATE);
    if (plans[static_cast<size_t>(i)].forward == nullptr ||
        plans[static_cast<size_t>(i)].inverse == nullptr) {
      release();
      return false;
    }
  }

  fftSize = fftSizeAt(fftSizeIndex);
  this->sampleRate = sampleRate;
  try {
    // Reserve peak capacities during setup to avoid vector growth on the audio thread.
    const auto maximumBinCount = static_cast<size_t>(fftBinCount(maxFftSize));
    bins.resize(static_cast<size_t>(fftBinCount(fftSize)));
    bins.reserve(maximumBinCount);
    remapSource.reserve(maximumBinCount);
    remapAccumulator.reserve(maximumBinCount);
    selectionMask.reserve(maximumBinCount);
    remapSource.resize(static_cast<size_t>(fftBinCount(fftSize)));
    remapAccumulator.resize(static_cast<size_t>(fftBinCount(fftSize)));
    selectionMask.resize(static_cast<size_t>(fftBinCount(fftSize)));
    effectMask.resize(static_cast<size_t>(fftBinCount(fftSize)), 1);
    parameterEvents.resize(parameterEventCapacity);
    const auto maximumDelay = std::max(
        static_cast<size_t>(100),
        static_cast<size_t>(std::ceil(sampleRate * maximumDelayMilliseconds / 1000.0)));
    ringSize = maximumDelay + static_cast<size_t>(maxFftSize) * 2 + 1;
    for (auto& channel : channelStates) {
      channel.inputFrame.resize(static_cast<size_t>(maxFftSize));
      channel.outputFrame.resize(static_cast<size_t>(maxFftSize));
      channel.spectrum.resize(static_cast<size_t>(fftBinCount(fftSize)));
      channel.spectrum.reserve(maximumBinCount);
      channel.outputRing.assign(ringSize, 0.0f);
      channel.outputRingPositions.assign(ringSize, std::numeric_limits<size_t>::max());
    }
  }
  catch (const std::bad_alloc&) {
    release();
    return false;
  }

  configureScheduler(0.0f, 0);
  return true;
}

void DtBlkFxEngine::release() noexcept
{
  for (auto& plan : plans) {
    if (plan.forward != nullptr)
      fftwf_destroy_plan(plan.forward);
    if (plan.inverse != nullptr)
      fftwf_destroy_plan(plan.inverse);
    plan = {};
  }

  if (timeBuffer != nullptr)
    fftwf_free(timeBuffer);
  if (frequencyBuffer != nullptr)
    fftwf_free(frequencyBuffer);

  timeBuffer = nullptr;
  frequencyBuffer = nullptr;
  fftSize = 0;
  requestedFftSize = 0;
  fixedLatencySamples = 0; // new: a released engine goes back to "FFT size minus one"
  timeWindowSamples = 0;
  preWindowSamples = 0;
  sampleRate = 44100.0;
  bins.clear();
  remapSource.clear();
  remapAccumulator.clear();
  selectionMask.clear();
  effectMask.clear();
  parameterEvents.clear();
  parameterEventHead = 0;
  parameterEventCount = 0;
  latestParameterInput = {};
  hasLegacyParameterInput = false;
  useLegacyParameterTimeline = false;
  parameterSyncEnabled = false;
  ringSize = 0;
  hopSize = 0;
  nextHopSize = 0;
  delaySamples = 0;
  activeDelaySamples = 0;
  delayTransitionFromSamples = 0;
  delayTransitionToSamples = 0;
  delayTransitionLength = 0;
  delayTransitionPosition = 0;
  minimumHopSamples = 64;
  legacyOverlapParameter = 0.0f;
  nextFrameStart = 0;
  samplesProcessed = 0;
  outputEndSample = 0;
  firstOutputSample = 0;
  hasOutputFrame = false;
  samplesPerBeat = 0.0;
  beatStartSample = 0.0;
  mixBack = 1.0f;
  powerMatch = false;
  smearRandomState = 1;
  for (auto& channel : channelStates) {
    channel.inputFrame.clear();
    channel.outputFrame.clear();
    channel.spectrum.clear();
    channel.outputRing.clear();
    channel.outputRingPositions.clear();
  }
}

void DtBlkFxEngine::configureScheduler(float overlapParameter,
                                       int delay,
                                       double beatLength,
                                       double beatPhase,
                                       int fftSizeIndex) noexcept
{
  if (!isPrepared())
    return;

  if (fftSizeIndex >= 0 && fftSizeIndex < fftSizeCount)
    requestedFftSize = fftSizeAt(fftSizeIndex);
  else if (requestedFftSize == 0)
    requestedFftSize = fftSize;

  legacyOverlapParameter =
      std::isfinite(overlapParameter) ? std::clamp(overlapParameter, 0.0f, 1.0f) : 0.0f;
  const auto maximumDelay =
      static_cast<int>(ringSize - static_cast<size_t>(maxFftSize) * 2 - 1);
  delaySamples = std::clamp(delay, 0, maximumDelay);
  if (samplesProcessed == 0)
    setActiveDelaySamples(delaySamples);
  samplesPerBeat = std::isfinite(beatLength) && beatLength > 0.0 ? beatLength : 0.0;
  beatStartSample =
      static_cast<double>(samplesProcessed) - (std::isfinite(beatPhase) ? beatPhase : 0.0);
  const auto minimumHopFromTempo =
      samplesPerBeat > 0.0 ? std::min(samplesPerBeat / 16.0, static_cast<double>(maxFftSize))
                           : 64.0;
  minimumHopSamples = std::max(64, static_cast<int>(minimumHopFromTempo));

  if (samplesProcessed == 0) {
    setFftSize(requestedFftSize);
    updateTimeWindow();
    updateHopSize();
    nextHopSize = getNextFrameHop(nextFrameStart);
  }
}

bool DtBlkFxEngine::configureLegacyParameters(const std::array<float, 44>& parameters,
                                              double beatLength,
                                              double beatPhase,
                                              bool syncParameters) noexcept
{
  if (!isPrepared() || parameterEvents.size() != parameterEventCapacity)
    return false;

  const auto parameterSyncChanged = parameterSyncEnabled != syncParameters;
  samplesPerBeat = std::isfinite(beatLength) && beatLength > 0.0 ? beatLength : 0.0;
  beatStartSample =
      static_cast<double>(samplesProcessed) - (std::isfinite(beatPhase) ? beatPhase : 0.0);
  const auto minimumHopFromTempo =
      samplesPerBeat > 0.0 ? std::min(samplesPerBeat / 16.0, static_cast<double>(maxFftSize))
                           : 64.0;
  minimumHopSamples = std::max(64, static_cast<int>(minimumHopFromTempo));
  useLegacyParameterTimeline = true;
  parameterSyncEnabled = syncParameters;

  // Clamp malformed or out-of-range host values before scheduling them for a frame.
  auto sanitized = parameters;
  for (auto& value : sanitized)
    value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;

  if (!hasLegacyParameterInput) {
    if (!appendParameterEvent(sanitized))
      return false;
    latestParameterInput = sanitized;
    hasLegacyParameterInput = true;
    updateFrameParameters(nextFrameStart);
    if (samplesProcessed == 0)
      setActiveDelaySamples(delaySamples);
    updateHopSize();
    nextHopSize = getNextFrameHop(nextFrameStart);
    return true;
  }

  const auto changed = sanitized != latestParameterInput;
  if (changed) {
    pruneParameterEvents();
    if (!appendParameterEvent(sanitized))
      return false;
    latestParameterInput = sanitized;
  }

  updateFrameParameters(nextFrameStart);
  updateHopSize();
  if (changed || parameterSyncChanged)
    nextHopSize = getNextFrameHop(nextFrameStart);
  return true;
}

void DtBlkFxEngine::configureEffects(const std::array<EffectParameters, 8>& newEffects,
                                     float newMixBack,
                                     bool newPowerMatch) noexcept
{
  effects = newEffects;
  mixBack = std::isfinite(newMixBack) ? std::clamp(newMixBack, 0.0f, 1.0f) : 1.0f;
  powerMatch = newPowerMatch;
}

void DtBlkFxEngine::reset() noexcept
{
  for (auto& channel : channelStates) {
    std::fill(channel.inputFrame.begin(), channel.inputFrame.end(), 0.0f);
    std::fill(channel.outputFrame.begin(), channel.outputFrame.end(), 0.0f);
    std::fill(channel.outputRing.begin(), channel.outputRing.end(), 0.0f);
    std::fill(channel.outputRingPositions.begin(),
              channel.outputRingPositions.end(),
              std::numeric_limits<size_t>::max());
  }

  nextFrameStart = 0;
  samplesProcessed = 0;
  outputEndSample = 0;
  firstOutputSample = 0;
  hasOutputFrame = false;
  setActiveDelaySamples(delaySamples);
  beatStartSample = 0.0;
  parameterEventHead = 0;
  parameterEventCount = 0;
  hasLegacyParameterInput = false;
  useLegacyParameterTimeline = false;
  parameterSyncEnabled = false;
  latestParameterInput = {};
  smearRandomState = 1; // new: makes smear repeatable after a reset
  setFftSize(requestedFftSize);
  std::fill(effectMask.begin(), effectMask.end(), 1);
  updateTimeWindow();
  updateHopSize();
  nextHopSize = getNextFrameHop(nextFrameStart);
}

int DtBlkFxEngine::fftSizeAt(int index) noexcept
{
  if (index < 0 || index >= fftSizeCount)
    return 0;
  return fftSizes[static_cast<size_t>(index)];
}

int DtBlkFxEngine::fftBinCount(int fftSize) noexcept
{
  return fftSize > 0 ? fftSize / 2 + 1 : 0;
}

int DtBlkFxEngine::findPlan(int fftSize) const noexcept
{
  const auto found = std::find(fftSizes.begin(), fftSizes.end(), fftSize);
  return found == fftSizes.end() ? -1 : static_cast<int>(found - fftSizes.begin());
}

bool DtBlkFxEngine::forward(const float* input, ComplexBin* output, int fftSize) noexcept
{
  const auto planIndex = findPlan(fftSize);
  if (!isPrepared() || input == nullptr || output == nullptr || planIndex < 0)
    return false;

  std::memcpy(timeBuffer, input, sizeof(float) * static_cast<size_t>(fftSize));
  fftwf_execute(plans[static_cast<size_t>(planIndex)].forward);
  for (int bin = 0; bin < fftBinCount(fftSize); ++bin) {
    output[bin].real = frequencyBuffer[bin * 2];
    output[bin].imaginary = frequencyBuffer[bin * 2 + 1];
  }
  return true;
}

bool DtBlkFxEngine::inverse(const ComplexBin* input, float* output, int fftSize) noexcept
{
  const auto planIndex = findPlan(fftSize);
  if (!isPrepared() || input == nullptr || output == nullptr || planIndex < 0)
    return false;

  const auto binCount = static_cast<size_t>(fftBinCount(fftSize));
  for (size_t bin = 0; bin < binCount; ++bin) {
    frequencyBuffer[bin * 2] = input[bin].real;
    frequencyBuffer[bin * 2 + 1] = input[bin].imaginary;
  }
  fftwf_execute(plans[static_cast<size_t>(planIndex)].inverse);
  const auto scale = 1.0f / static_cast<float>(fftSize);
  std::transform(
      timeBuffer, timeBuffer + fftSize, output, [scale](float sample) { return sample * scale; });
  return true;
}

bool DtBlkFxEngine::process(float* const* channels, int channelCount, int sampleCount) noexcept
{
  if (!isPrepared() || channels == nullptr || channelCount < 1 || channelCount > 2 ||
      sampleCount < 0)
    return false;

  for (int channel = 0; channel < channelCount; ++channel)
    if (channels[channel] == nullptr)
      return false;

  // Consume and emit one sample at a time so frame boundaries remain independent of host block size.
  for (int sample = 0; sample < sampleCount; ++sample) {
    std::array<double, 2> inputPowers{};
    for (int channel = 0; channel < channelCount; ++channel) {
      auto& state = channelStates[static_cast<size_t>(channel)];
      state.inputFrame[samplesProcessed % static_cast<size_t>(maxFftSize)] = channels[channel][sample];
    }

    const auto frameReadyAt =
        nextFrameStart + static_cast<size_t>(fftSize - preWindowSamples) - 1;
    if (samplesProcessed >= frameReadyAt) {
      if (!processFrame(channelCount))
        return false;
      nextFrameStart += static_cast<size_t>(nextHopSize);
      if (useLegacyParameterTimeline)
        updateFrameParameters(nextFrameStart);
      else {
        setFftSize(requestedFftSize);
        updateTimeWindow();
      }
      updateHopSize();
      nextHopSize = getNextFrameHop(nextFrameStart);
      updateDelayTransition();
      pruneParameterEvents();
    }

    const auto transitionActive = delayTransitionLength > 0;
    const auto transitionAlpha =
        transitionActive
            ? static_cast<float>(delayTransitionPosition) /
                  static_cast<float>(delayTransitionLength - 1)
            : 0.0f;
    auto advanceTransition = false;
    for (int channel = 0; channel < channelCount; ++channel) {
      const auto& state = channelStates[static_cast<size_t>(channel)];
      float output = 0.0f;
      if (transitionActive) {
        float fromSample = 0.0f;
        float toSample = 0.0f;
        const auto fromAvailable =
            samplesProcessed >= static_cast<size_t>(delayTransitionFromSamples) &&
            readOutputSample(
                state, samplesProcessed - static_cast<size_t>(delayTransitionFromSamples), fromSample);
        const auto toAvailable =
            samplesProcessed >= static_cast<size_t>(delayTransitionToSamples) &&
            readOutputSample(
                state, samplesProcessed - static_cast<size_t>(delayTransitionToSamples), toSample);
        if (channel == 0)
          advanceTransition = toAvailable;

        if (fromAvailable && toAvailable)
          output = fromSample + (toSample - fromSample) * transitionAlpha;
        else if (fromAvailable)
          output = fromSample;
        else if (toAvailable)
          output = toSample * transitionAlpha;
      }
      else if (samplesProcessed >= static_cast<size_t>(activeDelaySamples)) {
        readOutputSample(state,
                         samplesProcessed - static_cast<size_t>(activeDelaySamples),
                         output);
      }
      channels[channel][sample] = output;
    }

    if (transitionActive && advanceTransition) {
      ++delayTransitionPosition;
      if (delayTransitionPosition >= delayTransitionLength)
        setActiveDelaySamples(delayTransitionToSamples);
    }
    ++samplesProcessed;
  }

  return true;
}

bool DtBlkFxEngine::processFrame(int channelCount) noexcept
{
  const auto requestedStart = nextFrameStart + static_cast<size_t>(getLatencySamples());
  const auto destinationStart = std::max(requestedStart, samplesProcessed);
  if (!hasOutputFrame) {
    firstOutputSample = destinationStart;
    hasOutputFrame = true;
  }
  const auto frameEnd = destinationStart + static_cast<size_t>(timeWindowSamples);
  auto fadeInSamples = static_cast<std::int64_t>(outputEndSample) -
                       static_cast<std::int64_t>(destinationStart);
  auto fadeOutSamples = 0;
  const auto extendOutput = static_cast<std::int64_t>(frameEnd) -
                            static_cast<std::int64_t>(outputEndSample);
  if (fadeInSamples >= 0) {
    if (extendOutput < 0) {
      fadeInSamples = timeWindowSamples / 2;
      fadeOutSamples = timeWindowSamples - static_cast<int>(fadeInSamples);
    }
  }
  else {
    const auto gapStart = outputEndSample;
    const auto fadeOutStart = gapStart > 16 ? gapStart - 16 : 0;
    for (int channel = 0; channel < channelCount; ++channel) {
      auto& outputRing = channelStates[static_cast<size_t>(channel)].outputRing;
      for (auto position = std::max(fadeOutStart, samplesProcessed);
           position < outputEndSample;
           ++position) {
        const auto ringIndex = position % ringSize;
        if (channelStates[static_cast<size_t>(channel)].outputRingPositions[ringIndex] != position)
          continue;
        const auto fadeLength = outputEndSample - fadeOutStart;
        const auto fadePosition = position - fadeOutStart;
        const auto gain = fadeLength <= 1 ? 0.0f
                                          : 1.0f - static_cast<float>(fadePosition) /
                                                       static_cast<float>(fadeLength - 1);
        outputRing[position % ringSize] *= gain;
      }
      for (auto position = outputEndSample;
           position < std::min(frameEnd, destinationStart + 16);
           ++position) {
        const auto ringIndex = position % ringSize;
        outputRing[ringIndex] = 0.0f;
        channelStates[static_cast<size_t>(channel)].outputRingPositions[ringIndex] = position;
      }
    }
    fadeInSamples = 16;
  }
  fadeInSamples = std::clamp<std::int64_t>(fadeInSamples, 0, timeWindowSamples);

  std::array<double, 2> inputPowers{};
  for (int channel = 0; channel < channelCount; ++channel) {
    auto& state = channelStates[static_cast<size_t>(channel)];
    for (int sample = 0; sample < fftSize; ++sample) {
      if (sample < preWindowSamples && nextFrameStart < static_cast<size_t>(preWindowSamples)) {
        state.outputFrame[static_cast<size_t>(sample)] = 0.0f;
      }
      else {
        const auto inputPosition = nextFrameStart - static_cast<size_t>(preWindowSamples) +
                                   static_cast<size_t>(sample);
        state.outputFrame[static_cast<size_t>(sample)] =
            state.inputFrame[inputPosition % static_cast<size_t>(maxFftSize)];
      }
    }

    if (!forward(state.outputFrame.data(), bins.data(), fftSize))
      return false;

    for (const auto& bin : bins)
      inputPowers[static_cast<size_t>(channel)] +=
          static_cast<double>(bin.real) * bin.real +
          static_cast<double>(bin.imaginary) * bin.imaginary;
    std::copy(bins.begin(), bins.end(), state.spectrum.begin());
  }

  for (size_t effectIndex = 0; effectIndex < effects.size(); ++effectIndex) {
    const auto& effect = effects[effectIndex];
    std::fill(effectMask.begin(), effectMask.end(), 1);
    if (effectIndex > 0) {
      const auto previousType = effects[effectIndex - 1].type;
      if (previousType >= 20 && previousType <= 25) {
        std::copy(channelStates[0].spectrum.begin(),
                  channelStates[0].spectrum.end(),
                  bins.begin());
        updateEffectMask(effects[effectIndex - 1]);
      }
    }

    if (effect.type >= 20 && effect.type <= 25)
      continue;

    if (effect.type >= 26 && effect.type <= 30) {
      applyStereoEffect(effect, channelCount);
      continue;
    }

    if (effect.type < 0 || effect.type > 19)
      continue;

    for (int channel = 0; channel < channelCount; ++channel) {
      auto& state = channelStates[static_cast<size_t>(channel)];
      std::copy(state.spectrum.begin(), state.spectrum.end(), bins.begin());
      applyFilter(effect);
      applyContrast(effect);
      applySmear(effect);
      applyThreshold(effect);
      applyClip(effect);
      applyResize(effect, nextFrameStart);
      applyResample(effect, nextFrameStart);
      applyShift(effect, nextFrameStart);
      applyConstShift(effect, nextFrameStart);
      applyHarmonicEffect(effect, nextFrameStart);
      applySweep(effect);
      std::copy(bins.begin(), bins.end(), state.spectrum.begin());
    }
  }

  for (int channel = 0; channel < channelCount; ++channel) {
    auto& state = channelStates[static_cast<size_t>(channel)];
    std::copy(state.spectrum.begin(), state.spectrum.end(), bins.begin());
    double outputPower = 0.0;
    for (const auto& bin : bins)
      outputPower += static_cast<double>(bin.real) * bin.real +
                     static_cast<double>(bin.imaginary) * bin.imaginary;

    auto outputScale = 1.0f;
    if (powerMatch && outputPower > 1.0e-30 && inputPowers[static_cast<size_t>(channel)] > 0.0)
      outputScale =
          static_cast<float>(
              std::sqrt(std::clamp(inputPowers[static_cast<size_t>(channel)] / outputPower,
                                   0.0,
                                   1.0e30)));

    if (!inverse(bins.data(), state.outputFrame.data(), fftSize))
      return false;

    for (int sample = 0; sample < timeWindowSamples; ++sample) {
      const auto inputIndex =
          (nextFrameStart + static_cast<size_t>(sample)) % static_cast<size_t>(maxFftSize);
      const auto original = state.inputFrame[inputIndex];
      const auto processed =
          state.outputFrame[static_cast<size_t>(preWindowSamples + sample)] * outputScale;
      const auto mixed = original * mixBack + processed * (1.0f - mixBack);
      auto& outputValue = getOutputSampleForWrite(
          state, destinationStart + static_cast<size_t>(sample));

      const auto inFadeIn = sample < fadeInSamples;
      const auto fadeOutStart = timeWindowSamples - fadeOutSamples;
      const auto inFadeOut = fadeOutSamples > 0 && sample >= fadeOutStart;
      if (inFadeIn || inFadeOut) {
        const auto fadePosition = inFadeIn ? sample : sample - fadeOutStart;
        const auto fadeLength = inFadeIn ? fadeInSamples : fadeOutSamples;
        const auto fade = fadeLength <= 1
                              ? 1.0f
                              : static_cast<float>(fadePosition) /
                                    static_cast<float>(fadeLength - 1);
        outputValue = outputValue * (1.0f - fade) + mixed * fade;
      }
      else {
        outputValue = mixed;
      }
    }
  }

  outputEndSample = std::max(outputEndSample, frameEnd);
  return true;
}

void DtBlkFxEngine::setFftSize(int size) noexcept
{
  size = limitedFftSize(size);
  if (size <= 0 || size == fftSize)
    return;

  fftSize = size;
  const auto binCount = static_cast<size_t>(fftBinCount(fftSize));
  bins.resize(binCount);
  remapSource.resize(binCount);
  remapAccumulator.resize(binCount);
  selectionMask.resize(binCount);
  effectMask.resize(binCount, 1);
  std::fill(effectMask.begin(), effectMask.end(), 1);
  for (auto& channel : channelStates)
    channel.spectrum.resize(binCount);
}

int DtBlkFxEngine::limitedFftSize(int size) const noexcept
{
  if (fixedLatencySamples <= 0 || size <= 0)
    return size;

  // A frame of N samples needs N - 1 samples of latency to be written in time, so the largest
  // allowed size is the biggest one with N - 1 <= budget. Fall back to the smallest size if the
  // budget is below every supported size.
  int allowed = fftSizes.front();
  for (const auto candidate : fftSizes)
    if (candidate - 1 <= fixedLatencySamples)
      allowed = candidate;
  return std::min(size, allowed);
}

void DtBlkFxEngine::setFixedLatency(int latencySamples) noexcept
{
  const auto clamped = std::clamp(latencySamples, 0, maxFftSize);
  if (clamped == fixedLatencySamples)
    return;
  fixedLatencySamples = clamped;

  // Apply the new size limit now if the engine is running; the frame in progress keeps its size.
  if (isPrepared() && samplesProcessed == 0) {
    setFftSize(requestedFftSize > 0 ? requestedFftSize : fftSize);
    updateTimeWindow();
    updateHopSize();
    nextHopSize = getNextFrameHop(nextFrameStart);
  }
}

void DtBlkFxEngine::updateFrameParameters(size_t frameStart) noexcept
{
  if (!useLegacyParameterTimeline || parameterEventCount == 0)
    return;

  LegacyParameters current{};
  getInterpolatedParameters(frameStart, current);
  const auto plan = std::clamp(static_cast<int>(current[2] * 255.0f / 4.0f - 1.5f),
                               0,
                               fftSizeCount - 1);
  requestedFftSize = fftSizeAt(plan);
  legacyOverlapParameter = current[3];
  delaySamples = getDelaySamples(current[1]);

  const auto mixBackParameter = current[0];
  powerMatch = mixBackParameter <= 0.5f;
  mixBack = powerMatch ? mixBackParameter * 2.0f
                       : (1.0f - mixBackParameter) * (1.0f / 0.499f);

  for (int slot = 0; slot < static_cast<int>(effects.size()); ++slot) {
    const auto offset = static_cast<size_t>(4 + slot * 5);
    auto& effect = effects[static_cast<size_t>(slot)];
    effect.frequencyA = current[offset];
    effect.frequencyB = current[offset + 1];
    effect.amplitude = current[offset + 2];
    effect.type = DtBlkFxLegacy::legacyValueToEffectType(current[offset + 3]);
    effect.value = current[offset + 4];
  }

  setFftSize(requestedFftSize);
  updateTimeWindow();
}

void DtBlkFxEngine::getInterpolatedParameters(size_t samplePosition,
                                             LegacyParameters& result) const noexcept
{
  const auto eventAt = [this](size_t offset) -> const ParameterEvent& {
    return parameterEvents[(parameterEventHead + offset) % parameterEventCapacity];
  };

  auto previous = &eventAt(0);
  if (samplePosition <= previous->samplePosition) {
    result = previous->values;
    return;
  }

  for (size_t offset = 1; offset < parameterEventCount; ++offset) {
    const auto& next = eventAt(offset);
    if (samplePosition < next.samplePosition) {
      if (samplePosition <= next.rampStartSample ||
          next.samplePosition <= next.rampStartSample) {
        result = previous->values;
        return;
      }

      const auto fraction = static_cast<float>(samplePosition - next.rampStartSample) /
                            static_cast<float>(next.samplePosition - next.rampStartSample);
      for (size_t parameter = 0; parameter < result.size(); ++parameter)
        result[parameter] =
            previous->values[parameter] +
            (next.values[parameter] - previous->values[parameter]) * fraction;
      return;
    }
    previous = &next;
  }

  result = previous->values;
}

void DtBlkFxEngine::pruneParameterEvents() noexcept
{
  while (parameterEventCount > 2) {
    const auto nextHead = (parameterEventHead + 1) % parameterEventCapacity;
    if (parameterEvents[nextHead].samplePosition > nextFrameStart)
      break;
    parameterEventHead = nextHead;
    --parameterEventCount;
  }
}

bool DtBlkFxEngine::appendParameterEvent(const LegacyParameters& parameters) noexcept
{
  if (parameterEventCount > 0) {
    const auto previousIndex =
        (parameterEventHead + parameterEventCount - 1) % parameterEventCapacity;
    auto& previous = parameterEvents[previousIndex];
    if (samplesProcessed <= previous.samplePosition) {
      previous.values = parameters;
      if (parameterEventCount == 1) {
        previous.rampStartSample = previous.samplePosition;
      }
      else {
        const auto beforePreviousIndex =
            (parameterEventHead + parameterEventCount - 2) % parameterEventCapacity;
        const auto beforePreviousPosition = parameterEvents[beforePreviousIndex].samplePosition;
        const auto expectedDistance = std::max<size_t>(
            1, static_cast<size_t>(std::max(0.0, samplesPerBeat * 0.25)));
        const auto distance = previous.samplePosition - beforePreviousPosition;
        previous.rampStartSample = distance > expectedDistance
                                       ? previous.samplePosition - expectedDistance
                                       : beforePreviousPosition;
      }
      return true;
    }
  }

  if (parameterEventCount == parameterEventCapacity)
    pruneParameterEvents();
  if (parameterEventCount == parameterEventCapacity) {
    // The queue is still full: a very long frame with parameters changing every few samples.
    // Fold this update into the newest event instead of rejecting it, so the latest values are
    // never lost and the audio block is not dropped.
    const auto newestIndex = (parameterEventHead + parameterEventCount - 1) % parameterEventCapacity;
    parameterEvents[newestIndex].values = parameters;
    return true;
  }

  const auto eventIndex = (parameterEventHead + parameterEventCount) % parameterEventCapacity;
  auto& event = parameterEvents[eventIndex];
  event.samplePosition = samplesProcessed;
  event.values = parameters;
  if (parameterEventCount == 0) {
    event.rampStartSample = event.samplePosition;
    ++parameterEventCount;
    return true;
  }

  const auto previousIndex = (parameterEventHead + parameterEventCount - 1) % parameterEventCapacity;
  const auto previousPosition = parameterEvents[previousIndex].samplePosition;

  const auto expectedDistance = std::max<size_t>(
      1, static_cast<size_t>(std::max(0.0, samplesPerBeat * 0.25)));
  const auto distance = event.samplePosition - previousPosition;
  // Hold the previous value until the legacy interpolation lead-in begins.
  event.rampStartSample =
      distance > expectedDistance ? event.samplePosition - expectedDistance : previousPosition;
  ++parameterEventCount;
  return true;
}

int DtBlkFxEngine::getDelaySamples(float parameter) const noexcept
{
  const auto normalized = std::isfinite(parameter) ? std::clamp(parameter, 0.0f, 1.0f) : 0.0f;
  const auto milliseconds = normalized >= 0.5f;
  const auto fraction = milliseconds ? (normalized - 0.5f) * 2.0f : normalized * 2.0f;
  const auto amount = milliseconds ? fraction * 6000.0 : fraction * (0.5 * 255.0 / 16.0);
  const auto samples = milliseconds ? amount * sampleRate / 1000.0 : amount * samplesPerBeat;
  const auto maximumDelay =
      static_cast<double>(ringSize - static_cast<size_t>(maxFftSize) * 2 - 1);
  return static_cast<int>(std::lround(std::clamp(samples, 100.0, maximumDelay)));
}

void DtBlkFxEngine::updateTimeWindow() noexcept
{
  timeWindowSamples = fftSize;
  preWindowSamples = 0;
}

void DtBlkFxEngine::updateHopSize() noexcept
{
  const auto minimumHop = static_cast<float>(timeWindowSamples) * 0.15f;
  const auto maximumHop = static_cast<float>(timeWindowSamples - 16);
  hopSize = std::max(32,
                     static_cast<int>(std::floor(maximumHop + legacyOverlapParameter *
                                                                  (minimumHop - maximumHop))));
}

void DtBlkFxEngine::updateDelayTransition() noexcept
{
  if (delayTransitionLength > 0) {
    delayTransitionToSamples = delaySamples;
    return;
  }
  if (activeDelaySamples == delaySamples)
    return;

  delayTransitionFromSamples = activeDelaySamples;
  delayTransitionToSamples = delaySamples;
  const auto delayDifference = std::abs(delaySamples - activeDelaySamples);
  delayTransitionLength = std::clamp(delayDifference, 16, fftSize);
  delayTransitionPosition = 0;
}

void DtBlkFxEngine::setActiveDelaySamples(int delay) noexcept
{
  activeDelaySamples = delay;
  delayTransitionFromSamples = delay;
  delayTransitionToSamples = delay;
  delayTransitionLength = 0;
  delayTransitionPosition = 0;
}

bool DtBlkFxEngine::readOutputSample(const ChannelState& channel,
                                     size_t position,
                                     float& sample) const noexcept
{
  if (ringSize == 0 || !hasOutputFrame || position < firstOutputSample)
    return false;
  const auto ringIndex = position % ringSize;
  if (channel.outputRingPositions[ringIndex] != position)
    return false;
  sample = channel.outputRing[ringIndex];
  return true;
}

float& DtBlkFxEngine::getOutputSampleForWrite(ChannelState& channel,
                                              size_t position) noexcept
{
  const auto ringIndex = position % ringSize;
  if (channel.outputRingPositions[ringIndex] != position) {
    channel.outputRing[ringIndex] = 0.0f;
    channel.outputRingPositions[ringIndex] = position;
  }
  return channel.outputRing[ringIndex];
}

int DtBlkFxEngine::getNextFrameHop(size_t frameStart) const noexcept
{
  auto nextHop = hopSize;

  if (legacyOverlapParameter > 0.5f && samplesPerBeat > 16.0) {
    const auto syncUnits =
        std::ceil((beatStartSample - (static_cast<double>(frameStart) + nextHop)) /
                  samplesPerBeat);
    const auto syncSample = beatStartSample - syncUnits * samplesPerBeat;
    const auto syncForward = syncSample - static_cast<double>(frameStart);
    if (syncForward > static_cast<double>(minimumHopSamples) &&
        syncForward <= static_cast<double>(nextHop))
      nextHop = static_cast<int>(syncForward);
  }

  if (useLegacyParameterTimeline && parameterSyncEnabled) {
    for (size_t offset = 1; offset < parameterEventCount; ++offset) {
      const auto& event =
          parameterEvents[(parameterEventHead + offset) % parameterEventCapacity];
      if (event.samplePosition <= frameStart)
        continue;
      const auto syncForward = event.samplePosition - frameStart;
      if (syncForward >= static_cast<size_t>(nextHop))
        break;
      if (syncForward > static_cast<size_t>(minimumHopSamples)) {
        nextHop = static_cast<int>(syncForward);
        break;
      }
    }
  }

  return nextHop;
}

int DtBlkFxEngine::getFrequencyBin(float normalizedFrequency) const noexcept
{
  if (!std::isfinite(normalizedFrequency) || normalizedFrequency <= 0.0f)
    return 0;

  constexpr double c0Frequency = 16.35159898333181161;
  constexpr double noteSpan = 127.5;
  const auto frequency =
      c0Frequency * std::pow(2.0, static_cast<double>(normalizedFrequency) * noteSpan / 12.0);
  const auto bin = static_cast<double>(fftSize) * frequency / sampleRate;
  return static_cast<int>(std::lround(std::clamp(bin, 0.0, static_cast<double>(fftSize / 2))));
}

bool DtBlkFxEngine::isEffectBinSelected(int bin) const noexcept
{
  return bin >= 0 && static_cast<size_t>(bin) < effectMask.size() &&
         effectMask[static_cast<size_t>(bin)] != 0;
}

void DtBlkFxEngine::updateEffectMask(const EffectParameters& mask) noexcept
{
  std::fill(effectMask.begin(), effectMask.end(), 0);
  const auto binCount = fftBinCount(fftSize);
  const auto maximumBin = binCount - 1;
  const auto value = std::isfinite(mask.value) ? std::clamp(mask.value, 0.0f, 1.0f) : 0.5f;

  if (mask.type == 25) {
    const auto selectBelow = value >= 0.5f;
    const auto fraction = std::pow(selectBelow ? (value - 0.5f) * 2.0f : value * 2.0f, 0.8f);
    auto minimumPower = std::numeric_limits<float>::max();
    auto maximumPower = 1.0e-30f;
    for (int bin = 0; bin < binCount; ++bin) {
      const auto& valueAtBin = bins[static_cast<size_t>(bin)];
      const auto power = valueAtBin.real * valueAtBin.real +
                         valueAtBin.imaginary * valueAtBin.imaginary;
      minimumPower = std::min(minimumPower, power);
      maximumPower = std::max(maximumPower, power);
    }

    const auto threshold = std::exp(std::log(std::max(minimumPower, 1.0e-36f)) +
                                    fraction * (std::log(std::max(maximumPower, 1.0e-36f)) -
                                                std::log(std::max(minimumPower, 1.0e-36f))));
    std::fill(selectionMask.begin(), selectionMask.end(), 0);
    for (int bin = 0; bin < binCount; ++bin) {
      const auto& valueAtBin = bins[static_cast<size_t>(bin)];
      const auto power = valueAtBin.real * valueAtBin.real +
                         valueAtBin.imaginary * valueAtBin.imaginary;
      selectionMask[static_cast<size_t>(bin)] =
          static_cast<unsigned char>(selectBelow ? power < threshold : power >= threshold);
    }
    for (int bin = 0; bin < binCount; ++bin) {
      if (selectionMask[static_cast<size_t>(bin)] ||
          (bin > 0 && selectionMask[static_cast<size_t>(bin - 1)]) ||
          (bin + 1 < binCount && selectionMask[static_cast<size_t>(bin + 1)]))
        effectMask[static_cast<size_t>(bin)] = 1;
    }
    return;
  }

  float fundamental = static_cast<float>(getFrequencyBin(mask.frequencyA));
  if (mask.type != 20) {
    const auto firstBin = getFrequencyBin(mask.frequencyA);
    const auto secondBin = getFrequencyBin(mask.frequencyB);
    fundamental = findFundamentalBin(std::min(firstBin, secondBin),
                                     std::max(firstBin, secondBin));
    if (fundamental <= 0.0f)
      return;
    switch (mask.type) {
      case 22:
        fundamental *= 0.5f;
        break;
      case 23:
        fundamental /= 3.0f;
        break;
      case 24:
        fundamental *= 0.25f;
        break;
      default:
        break;
    }
  }
  if (!std::isfinite(fundamental) || fundamental < 1.0f)
    return;

  const auto splitValue = value >= 1.0f ? 4.0f : value * 4.0f;
  const auto harmonicType = std::min(static_cast<int>(splitValue), 3);
  const auto widthFraction = value >= 1.0f ? 1.0f : splitValue - harmonicType;
  auto offset = fundamental;
  auto spacing = fundamental;
  if (harmonicType == 1) {
    spacing *= 2.0f;
  }
  else if (harmonicType == 2) {
    offset *= 2.0f;
    spacing *= 2.0f;
  }
  else if (harmonicType == 3) {
    offset *= 0.5f;
  }
  spacing = std::max(spacing, 4.0f);
  const auto halfWidth = static_cast<int>(spacing * widthFraction) / 2;
  const auto lowestBin = std::max(1, static_cast<int>(offset) - halfWidth);
  const auto highestBin = std::min(maximumBin, static_cast<int>(spacing * 200.0f));
  for (int harmonic = -1; harmonic <= 200; ++harmonic) {
    const auto center = static_cast<int>(std::lround(offset + spacing * harmonic));
    const auto first = std::max(lowestBin, center - halfWidth);
    const auto last = std::min(highestBin, center + halfWidth);
    for (auto bin = first; bin <= last; ++bin)
      effectMask[static_cast<size_t>(bin)] = 1;
  }
}

void DtBlkFxEngine::applyFilter(const EffectParameters& effect) noexcept
{
  if (effect.type != 0)
    return;

  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  const auto firstBin = getFrequencyBin(effect.frequencyA);
  const auto secondBin = getFrequencyBin(effect.frequencyB);
  const auto binCount = fftBinCount(fftSize);

  for (int bin = 0; bin < binCount; ++bin) {
    const auto selected = firstBin <= secondBin ? bin >= firstBin && bin <= secondBin
                                                : bin <= secondBin || bin >= firstBin;
    if (selected && isEffectBinSelected(bin)) {
      bins[static_cast<size_t>(bin)].real *= gain;
      bins[static_cast<size_t>(bin)].imaginary *= gain;
    }
  }
}

void DtBlkFxEngine::applyContrast(const EffectParameters& effect) noexcept
{
  if (effect.type != 1)
    return;

  const auto binCount = fftBinCount(fftSize);
  const auto firstBin = getFrequencyBin(effect.frequencyA);
  const auto secondBin = getFrequencyBin(effect.frequencyB);
  const auto selected = [this, firstBin, secondBin](int bin) {
    return isEffectBinSelected(bin) &&
           (firstBin <= secondBin ? bin >= firstBin && bin <= secondBin
                                  : bin <= secondBin || bin >= firstBin);
  };
  const auto contrastValue =
      std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  auto raise = (contrastValue - 0.5f) * 2.0f;
  raise = raise >= 0.0f ? raise * raise * 4.0f : -raise * raise * 0.5f;

  const auto minValue = raise > 0.0f ? std::exp(-80.0f / (2.0f * raise + 1.0f)) : 1.0e-30f;
  const auto maxValue = raise > 0.0f ? std::exp(80.0f / (2.0f * raise + 1.0f)) : 1.0e30f;

  const auto effectAmplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto amplitude =
      effectAmplitude <= 0.0f ? 0.0f : std::pow(10.0f, (effectAmplitude * 100.0f - 60.0f) * 0.05f);
  const auto processRange = [this, minValue, maxValue, raise, amplitude](int lowerBin,
                                                                         int upperBin) {
    const auto count = upperBin - lowerBin + 1;
    double inputPower = 0.0;
    for (auto bin = lowerBin; bin <= upperBin; ++bin) {
      const auto& value = bins[static_cast<size_t>(bin)];
      inputPower += static_cast<double>(value.real) * value.real +
                    static_cast<double>(value.imaginary) * value.imaginary;
    }

    const auto inputScale = inputPower > 0.0 ? std::sqrt(count / inputPower) : 1.0;
    double outputPower = 0.0;
    for (auto bin = lowerBin; bin <= upperBin; ++bin) {
      auto& value = bins[static_cast<size_t>(bin)];
      auto real = static_cast<double>(value.real) * inputScale;
      auto imaginary = static_cast<double>(value.imaginary) * inputScale;
      const auto magnitudeSquared = real * real + imaginary * imaginary;

      if (magnitudeSquared < minValue) {
        real = 0.0;
        imaginary = 0.0;
      }
      else if (magnitudeSquared < maxValue) {
        const auto gain = std::pow(magnitudeSquared, raise);
        real *= gain;
        imaginary *= gain;
      }

      value.real = static_cast<float>(real);
      value.imaginary = static_cast<float>(imaginary);
      outputPower += real * real + imaginary * imaginary;
    }

    auto matchedAmplitude = static_cast<double>(amplitude);
    if (inputPower > 0.0 && outputPower > 0.0)
      matchedAmplitude *= std::sqrt(inputPower / outputPower);
    if (matchedAmplitude > 1.0e30)
      matchedAmplitude = 1.0;
    else if (matchedAmplitude < 1.0e-37)
      matchedAmplitude = 0.0;

    for (auto bin = lowerBin; bin <= upperBin; ++bin) {
      auto& value = bins[static_cast<size_t>(bin)];
      value.real = static_cast<float>(value.real * matchedAmplitude);
      value.imaginary = static_cast<float>(value.imaginary * matchedAmplitude);
    }
  };

  int rangeStart = -1;
  for (int bin = 0; bin <= binCount; ++bin) {
    const auto inRange = bin < binCount && selected(bin);
    if (inRange && rangeStart < 0)
      rangeStart = bin;
    else if (!inRange && rangeStart >= 0) {
      processRange(rangeStart, bin - 1);
      rangeStart = -1;
    }
  }
}

void DtBlkFxEngine::applySmear(const EffectParameters& effect) noexcept
{
  if (effect.type != 2)
    return;

  const auto smear = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.0f;
  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  const auto firstBin = getFrequencyBin(effect.frequencyA);
  const auto secondBin = getFrequencyBin(effect.frequencyB);
  const auto binCount = fftBinCount(fftSize);
  for (int bin = 0; bin < binCount; ++bin) {
    const auto selected =
        isEffectBinSelected(bin) &&
        (firstBin <= secondBin ? bin >= firstBin && bin <= secondBin
                               : bin <= secondBin || bin >= firstBin);
    if (!selected)
      continue;

    smearRandomState =
        (smearRandomState >> 1) ^
        (static_cast<std::uint32_t>(-static_cast<std::int32_t>(smearRandomState & 1u)) &
         0xd0000001u);
    constexpr double phaseStep = 2.0 * 3.14159265358979323846 / 4096.0;
    const auto phase = static_cast<double>(smearRandomState & 4095u) * phaseStep;
    const auto randomReal = static_cast<float>(std::cos(phase));
    const auto randomImaginary = static_cast<float>(std::sin(phase));
    const auto scaleReal = (1.0f - smear) + smear * randomReal;
    const auto scaleImaginary = smear * randomImaginary;
    const auto original = bins[static_cast<size_t>(bin)];
    bins[static_cast<size_t>(bin)] = {
        gain * (original.real * scaleReal - original.imaginary * scaleImaginary),
        gain * (original.real * scaleImaginary + original.imaginary * scaleReal)};
  }
}

void DtBlkFxEngine::applyThreshold(const EffectParameters& effect) noexcept
{
  if (effect.type != 3)
    return;

  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto thresholdType = value >= 0.5f;
  const auto thresholdFraction = thresholdType ? (value - 0.5f) * 2.0f : value * 2.0f;
  const auto thresholdPosition = std::pow(thresholdFraction, 0.8f);
  const auto binCount = fftBinCount(fftSize);
  const auto firstBin = getFrequencyBin(effect.frequencyA);
  const auto secondBin = getFrequencyBin(effect.frequencyB);
  const auto selected = [this, firstBin, secondBin](int bin) {
    return isEffectBinSelected(bin) &&
           (firstBin <= secondBin ? bin >= firstBin && bin <= secondBin
                                  : bin <= secondBin || bin >= firstBin);
  };
  float minimumPower = std::numeric_limits<float>::max();
  float maximumPower = 1.0e-30f;
  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;
    const auto& valueAtBin = bins[static_cast<size_t>(bin)];
    const auto power =
        valueAtBin.real * valueAtBin.real + valueAtBin.imaginary * valueAtBin.imaginary;
    minimumPower = std::min(minimumPower, power);
    maximumPower = std::max(maximumPower, power);
  }

  const auto minLogPower = std::log(std::max(minimumPower, 1.0e-36f));
  const auto maxLogPower = std::log(std::max(maximumPower, 1.0e-36f));
  const auto threshold = std::exp(minLogPower + thresholdPosition * (maxLogPower - minLogPower));
  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  std::fill(selectionMask.begin(), selectionMask.end(), 0);
  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;

    const auto& valueAtBin = bins[static_cast<size_t>(bin)];
    const auto power =
        valueAtBin.real * valueAtBin.real + valueAtBin.imaginary * valueAtBin.imaginary;
    selectionMask[static_cast<size_t>(bin)] =
        static_cast<unsigned char>(thresholdType ? power < threshold : power >= threshold);
  }

  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;
    if (selectionMask[static_cast<size_t>(bin)] ||
        (bin > 0 && selectionMask[static_cast<size_t>(bin - 1)]) ||
        (bin + 1 < binCount && selectionMask[static_cast<size_t>(bin + 1)])) {
      bins[static_cast<size_t>(bin)].real *= gain;
      bins[static_cast<size_t>(bin)].imaginary *= gain;
    }
  }
}

void DtBlkFxEngine::applyClip(const EffectParameters& effect) noexcept
{
  if (effect.type != 4)
    return;

  const auto clipValue = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto clipPosition = 1.0f - clipValue * clipValue;
  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  const auto binCount = fftBinCount(fftSize);
  const auto firstBin = getFrequencyBin(effect.frequencyA);
  const auto secondBin = getFrequencyBin(effect.frequencyB);
  const auto selected = [this, firstBin, secondBin](int bin) {
    return isEffectBinSelected(bin) &&
           (firstBin <= secondBin ? bin >= firstBin && bin <= secondBin
                                  : bin <= secondBin || bin >= firstBin);
  };
  float minimumPower = std::numeric_limits<float>::max();
  float maximumPower = 1.0e-30f;
  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;
    const auto& value = bins[static_cast<size_t>(bin)];
    const auto power = value.real * value.real + value.imaginary * value.imaginary;
    minimumPower = std::min(minimumPower, power);
    maximumPower = std::max(maximumPower, power);
  }
  if (minimumPower == std::numeric_limits<float>::max())
    return;

  const auto threshold = std::exp(clipPosition * (std::log(std::max(maximumPower, 1.0e-36f)) -
                                                  std::log(std::max(minimumPower, 1.0e-36f))) +
                                  std::log(std::max(minimumPower, 1.0e-36f)));
  double inputPower = 0.0;
  double outputPower = 0.0;
  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;
    auto& value = bins[static_cast<size_t>(bin)];
    const auto power = value.real * value.real + value.imaginary * value.imaginary;
    inputPower += power;
    if (power >= threshold && power > 0.0f) {
      const auto scale = std::sqrt(threshold / power);
      value.real *= static_cast<float>(scale);
      value.imaginary *= static_cast<float>(scale);
      outputPower += threshold;
    }
    else {
      outputPower += power;
    }
  }

  auto outputScale = static_cast<double>(gain);
  if (inputPower > 0.0 && outputPower > 0.0)
    outputScale *= std::sqrt(inputPower / outputPower);
  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;
    bins[static_cast<size_t>(bin)].real *= static_cast<float>(outputScale);
    bins[static_cast<size_t>(bin)].imaginary *= static_cast<float>(outputScale);
  }
}

void DtBlkFxEngine::remapFrequencies(const EffectParameters& effect,
                                     float scale,
                                     float offset,
                                     bool centerRange,
                                     bool conjugate,
                                     size_t frameStart) noexcept
{
  if (!std::isfinite(scale) || !std::isfinite(offset))
    return;

  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  const auto sourceGain = std::clamp(1.0f - gain, 0.0f, 1.0f);
  const auto binCount = fftBinCount(fftSize);
  const auto firstBin = getFrequencyBin(effect.frequencyA);
  const auto secondBin = getFrequencyBin(effect.frequencyB);
  const auto selected = [this, firstBin, secondBin](int bin) {
    return isEffectBinSelected(bin) &&
           (firstBin <= secondBin ? bin >= firstBin && bin <= secondBin
                                  : bin <= secondBin || bin >= firstBin);
  };

  std::copy(bins.begin(), bins.end(), remapSource.begin());
  std::fill(remapAccumulator.begin(), remapAccumulator.end(), ComplexBin{0.0f, 0.0f});
  std::fill(selectionMask.begin(), selectionMask.end(), 0);

  for (int bin = 0; bin < binCount; ++bin) {
    if (!selected(bin))
      continue;

    selectionMask[static_cast<size_t>(bin)] = 1;
    bins[static_cast<size_t>(bin)].real = remapSource[static_cast<size_t>(bin)].real * sourceGain;
    bins[static_cast<size_t>(bin)].imaginary =
        remapSource[static_cast<size_t>(bin)].imaginary * sourceGain;
  }

  constexpr double twoPi = 6.28318530717958647692;
  const auto framePhase = static_cast<double>(frameStart % static_cast<size_t>(fftSize));
  int rangeStart = -1;
  for (int bin = 0; bin <= binCount; ++bin) {
    const auto inRange = bin < binCount && selectionMask[static_cast<size_t>(bin)] != 0;
    if (inRange && rangeStart < 0) {
      rangeStart = bin;
      continue;
    }
    if (inRange || rangeStart < 0)
      continue;

    const auto rangeEnd = bin - 1;
    const auto firstDestination = centerRange
                                      ? (static_cast<float>(rangeStart + rangeEnd) -
                                         static_cast<float>(rangeEnd - rangeStart + 1) * scale) *
                                            0.5f
                                      : static_cast<float>(rangeStart) * scale + offset;
    for (auto sourceBin = rangeStart; sourceBin <= rangeEnd; ++sourceBin) {
      const auto sourceOffset = static_cast<float>(sourceBin - rangeStart);
      const auto destination = firstDestination + sourceOffset * scale;
      if (destination < 0.0f || destination > static_cast<float>(binCount - 1))
        continue;

      const auto centerDestination = static_cast<int>(std::floor(destination + 0.5f));
      const auto fractionalOffset = destination - static_cast<float>(centerDestination);
      constexpr int kernelRadius = 2;
      std::array<float, kernelRadius * 2 + 1> kernelWeights{};
      auto kernelSum = 0.0f;
      for (int tap = -kernelRadius; tap <= kernelRadius; ++tap) {
        const auto tapDestination = centerDestination + tap;
        auto weight = 1.0f;
        for (int otherTap = -kernelRadius; otherTap <= kernelRadius; ++otherTap) {
          if (otherTap == tap)
            continue;
          weight *= (fractionalOffset - static_cast<float>(otherTap)) /
                    static_cast<float>(tap - otherTap);
        }
        kernelWeights[static_cast<size_t>(tap + kernelRadius)] = weight;
        if (tapDestination >= 0 && tapDestination < binCount)
          kernelSum += weight;
      }
      if (std::abs(kernelSum) < 1.0e-6f)
        continue;

      const auto& source = remapSource[static_cast<size_t>(sourceBin)];
      const auto sourceImaginary = conjugate ? -source.imaginary : source.imaginary;
      const auto binShift = static_cast<double>(destination) - sourceBin;
      const auto turns = std::remainder(framePhase * binShift / static_cast<double>(fftSize), 1.0);
      const auto phase = twoPi * turns;
      const auto phaseReal = static_cast<float>(std::cos(phase));
      const auto phaseImaginary = static_cast<float>(std::sin(phase));
      const auto shiftedReal = gain * (source.real * phaseReal - sourceImaginary * phaseImaginary);
      const auto shiftedImaginary =
          gain * (source.real * phaseImaginary + sourceImaginary * phaseReal);
      for (int tap = -kernelRadius; tap <= kernelRadius; ++tap) {
        const auto tapDestination = centerDestination + tap;
        if (tapDestination < 0 || tapDestination >= binCount)
          continue;

        const auto weight = kernelWeights[static_cast<size_t>(tap + kernelRadius)] / kernelSum;
        auto& destinationBin = remapAccumulator[static_cast<size_t>(tapDestination)];
        destinationBin.real += shiftedReal * weight;
        destinationBin.imaginary += shiftedImaginary * weight;
      }
    }
    rangeStart = -1;
  }

  for (int bin = 0; bin < binCount; ++bin) {
    bins[static_cast<size_t>(bin)].real += remapAccumulator[static_cast<size_t>(bin)].real;
    bins[static_cast<size_t>(bin)].imaginary +=
        remapAccumulator[static_cast<size_t>(bin)].imaginary;
  }
  bins.front().imaginary = 0.0f;
  bins.back().imaginary = 0.0f;
}

void DtBlkFxEngine::applyResize(const EffectParameters& effect, size_t frameStart) noexcept
{
  if (effect.type != 5)
    return;

  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto scaledValue = value * 2.0f;
  const auto integerPart = std::min(static_cast<int>(scaledValue), 1);
  const auto fractionalPart = value >= 1.0f ? 1.0f : scaledValue - integerPart;
  const auto resizeScale = std::pow(fractionalPart * 4.0f - 2.0f, 3.0f);
  const auto scale = std::trunc(resizeScale * 4096.0f) / 4096.0f;
  const auto conjugate = (integerPart != 0) != (scale < 0.0f);
  remapFrequencies(effect, scale, 0.0f, true, conjugate, frameStart);
}

void DtBlkFxEngine::applyResample(const EffectParameters& effect, size_t frameStart) noexcept
{
  if (effect.type != 6)
    return;

  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto octaveShift = (value - 0.5f) * 6.0f;
  const auto multiplier = std::trunc(std::pow(2.0f, octaveShift) * 4096.0f) / 4096.0f;
  remapFrequencies(effect, multiplier, 0.0f, false, false, frameStart);
}

void DtBlkFxEngine::applyShift(const EffectParameters& effect, size_t frameStart) noexcept
{
  if (effect.type != 7)
    return;

  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto octaveShift = (value - 0.5f) * 6.0f;
  const auto multiplier = std::pow(2.0f, octaveShift);
  remapFrequencies(effect, multiplier, 0.0f, false, false, frameStart);
}

void DtBlkFxEngine::applyConstShift(const EffectParameters& effect, size_t frameStart) noexcept
{
  if (effect.type != 8)
    return;

  auto normalized = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  normalized = (normalized - 0.5f) * 2.0f;
  const auto direction = normalized < 0.0f ? -1.0f : 1.0f;
  normalized = std::abs(normalized);
  constexpr float linearFraction = 2048.0f / 32768.0f;
  constexpr int exponentialRaise = 8;
  constexpr float exponentialScale = (1.0f - linearFraction) / (1 << exponentialRaise);
  const auto shiftHz = direction *
                       ((std::pow(2.0f, normalized * exponentialRaise) - 1.0f) * exponentialScale +
                        normalized * linearFraction) *
                       22050.0f;
  const auto shiftBins = shiftHz / static_cast<float>(sampleRate) * static_cast<float>(fftSize);
  remapFrequencies(effect, 1.0f, shiftBins, false, false, frameStart);
}

float DtBlkFxEngine::estimatePeakBin(int centreBin) const noexcept
{
  // Port of the legacy EstFftBin(): the fractional position follows from the ratio of neighbouring
  // bins of a sinusoid, using the side that carries more energy.
  const auto binCount = static_cast<int>(bins.size());
  if (centreBin < 1 || centreBin + 1 >= binCount)
    return static_cast<float>(centreBin);

  const auto& before = bins[static_cast<size_t>(centreBin - 1)];
  const auto& centre = bins[static_cast<size_t>(centreBin)];
  const auto& after = bins[static_cast<size_t>(centreBin + 1)];

  const auto differenceBeforeReal = centre.real - before.real;
  const auto differenceBeforeImag = centre.imaginary - before.imaginary;
  const auto differenceAfterReal = after.real - centre.real;
  const auto differenceAfterImag = after.imaginary - centre.imaginary;

  const auto magnitudeBefore = std::hypot(before.real, before.imaginary);
  const auto magnitudeAfter = std::hypot(after.real, after.imaginary);
  const auto differenceBeforeMagnitude = std::hypot(differenceBeforeReal, differenceBeforeImag);
  const auto differenceAfterMagnitude = std::hypot(differenceAfterReal, differenceAfterImag);

  // Weighting between the two estimates.
  const auto weightBefore = magnitudeBefore * 0.4f + differenceBeforeMagnitude;
  const auto weightAfter = magnitudeAfter * 0.4f + differenceAfterMagnitude;
  const auto weightSum = weightBefore + weightAfter;
  if (!(weightSum > 0.0f) || !std::isfinite(weightSum))
    return static_cast<float>(centreBin);
  auto blend = weightBefore / weightSum;
  blend = 0.5f * (1.0f + std::clamp((blend - 0.5f) * 5.0f, -1.0f, 1.0f));

  // real(a / b) = (a.real * b.real + a.imag * b.imag) / |b|^2
  const auto normBefore = differenceBeforeReal * differenceBeforeReal + differenceBeforeImag * differenceBeforeImag;
  const auto normAfter = differenceAfterReal * differenceAfterReal + differenceAfterImag * differenceAfterImag;
  auto fractionBefore = 0.0f;
  auto fractionAfter = 0.0f;
  if (normBefore > 0.0f)
    fractionBefore =
        (before.real * differenceBeforeReal + before.imaginary * differenceBeforeImag) / normBefore;
  if (normAfter > 0.0f)
    fractionAfter =
        1.0f + (centre.real * differenceAfterReal + centre.imaginary * differenceAfterImag) / normAfter;

  const auto fraction = std::clamp(fractionBefore * blend + fractionAfter * (1.0f - blend), -0.5f, 0.5f);
  return std::isfinite(fraction) ? static_cast<float>(centreBin) + fraction : static_cast<float>(centreBin);
}

float DtBlkFxEngine::findFundamentalBin(int firstBin,
                                        int lastBin,
                                        int minimumSubharmonicBin) const noexcept
{
  // Port of the legacy PeakFindFft(): search whole bins and half bins for the strongest peak,
  // refine it to a fractional bin, then check whether it is really a harmonic of a lower
  // fundamental (1/2 up to 1/5 of the peak position).
  const auto maximumBin = fftBinCount(fftSize) - 1;
  firstBin = std::clamp(firstBin, 1, maximumBin);
  lastBin = std::clamp(lastBin, 1, maximumBin);
  if (firstBin > lastBin)
    std::swap(firstBin, lastBin);

  auto bestPower = -1.0f;
  auto peakBin = -1;
  auto previousReal = 0.0f;
  auto previousImaginary = 0.0f;
  auto previousPower = 0.0f;
  for (auto bin = firstBin; bin <= lastBin; ++bin) {
    const auto& value = bins[static_cast<size_t>(bin)];
    const auto power = value.real * value.real + value.imaginary * value.imaginary;

    // Power half way between the previous and the current bin.
    const auto halfReal = value.real - previousReal;
    const auto halfImaginary = value.imaginary - previousImaginary;
    const auto halfPower = 0.79f * 0.79f * (halfReal * halfReal + halfImaginary * halfImaginary);

    if (halfPower > bestPower) {
      bestPower = halfPower;
      peakBin = bin;
      if (previousPower > power)
        --peakBin;
    }
    if (power > bestPower) {
      bestPower = power;
      peakBin = bin;
    }
    previousReal = value.real;
    previousImaginary = value.imaginary;
    previousPower = power;
  }
  if (peakBin < 1 || !(bestPower > 1.0e-25f))
    return 0.0f;

  const auto estimate = estimatePeakBin(peakBin);

  // Is the peak a harmonic of a lower fundamental? Expect energy in the bin at, or just above,
  // the candidate fundamental position.
  const auto lowestBin = std::max(firstBin, minimumSubharmonicBin);
  const auto threshold = 0.1f * bestPower;
  auto harmonic = 1;
  for (auto divisor = 2; divisor <= 5; ++divisor) {
    const auto candidate = static_cast<int>(std::floor(estimate / static_cast<float>(divisor)));
    if (candidate < lowestBin)
      break;
    if (candidate + 1 > maximumBin)
      continue;
    const auto& low = bins[static_cast<size_t>(candidate)];
    const auto& high = bins[static_cast<size_t>(candidate + 1)];
    const auto lowPower = low.real * low.real + low.imaginary * low.imaginary;
    const auto highPower = high.real * high.real + high.imaginary * high.imaginary;
    if (lowPower > threshold || highPower > threshold)
      harmonic = divisor;
  }
  return estimate / static_cast<float>(harmonic);
}


void DtBlkFxEngine::applySweep(const EffectParameters& effect) noexcept
{
  if (effect.type < 15 || effect.type > 19)
    return;

  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto splitValue = value >= 1.0f ? 2.0f : value * 2.0f;
  const auto copyMode = splitValue >= 1.0f;
  const auto tablePosition = (copyMode ? splitValue - 1.0f : splitValue) * 119.0f;
  const auto firstTable = static_cast<int>(tablePosition);
  const auto secondTable = std::min(firstTable + 1, 119);
  const auto tableFraction = tablePosition - static_cast<float>(firstTable);
  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  const auto originalGain = std::max(1.0f - gain, 0.0f);
  const auto sweepIndex = effect.type - 15;
  const auto harmonicPower = [=](int harmonic) {
    const auto first = getSweepHarmonicPower(sweepIndex, firstTable, harmonic);
    const auto second = getSweepHarmonicPower(sweepIndex, secondTable, harmonic);
    return first + (second - first) * tableFraction;
  };
  const auto maximumBin = fftBinCount(fftSize) - 1;
  const auto fundamental = findFundamentalBin(1, std::max(1, fftSize / 8));
  if (fundamental < 1.0f)
    return;

  const auto spacing = std::max(fundamental, 4.0f);
  const auto halfWidth = static_cast<int>(spacing * 0.9996f) / 2;
  const auto harmonicZero = harmonicPower(0);
  if (harmonicZero <= 0.0f)
    return;

  const auto fundamentalCenter = static_cast<int>(std::lround(fundamental));
  const auto fundamentalStart = std::clamp(fundamentalCenter - halfWidth, 1, maximumBin);
  const auto fundamentalEnd = std::clamp(fundamentalCenter + halfWidth, 1, maximumBin);
  double fundamentalPower = 0.0;
  for (auto bin = fundamentalStart; bin <= fundamentalEnd; ++bin) {
    const auto& valueAtBin = bins[static_cast<size_t>(bin)];
    fundamentalPower += static_cast<double>(valueAtBin.real) * valueAtBin.real +
                        static_cast<double>(valueAtBin.imaginary) * valueAtBin.imaginary;
  }
  auto powerScale = fundamentalPower / harmonicZero;
  if (copyMode)
    powerScale *= static_cast<double>(gain) * gain;

  const auto firstFrequencyBin = getFrequencyBin(effect.frequencyA);
  const auto secondFrequencyBin = getFrequencyBin(effect.frequencyB);
  const auto processRange = [&](int rangeStart, int rangeEnd) {
    for (int harmonic = 0; harmonic < 128; ++harmonic) {
      const auto center = fundamental + spacing * static_cast<float>(harmonic);
      if (center > static_cast<float>(spacing * 200.0f) || center > maximumBin)
        break;

      const auto roundedCenter = static_cast<int>(std::lround(center));
      auto firstBin = std::max({1, rangeStart, roundedCenter - halfWidth});
      auto lastBin = std::min({maximumBin, rangeEnd, roundedCenter + halfWidth});
      if (lastBin < firstBin)
        continue;

      const auto targetPower = powerScale * harmonicPower(harmonic);
      if (copyMode) {
        const auto sourceOffset = static_cast<int>(std::lround(fundamental - center));
        if (firstBin + sourceOffset < 1)
          firstBin = 1 - sourceOffset;
        if (lastBin + sourceOffset > maximumBin)
          lastBin = maximumBin - sourceOffset;
        if (lastBin < firstBin)
          continue;

        // The copy gain is a spectrum amplitude. The legacy plugin worked on a spectrum
        // normalised by 1/fftSize, so divide here to get the same output level.
        const auto copyGain =
            std::sqrt(std::max(0.0, targetPower)) / static_cast<double>(fftSize);
        for (auto bin = firstBin; bin <= lastBin; ++bin) {
          if (!isEffectBinSelected(bin))
            continue;
          const auto source = bins[static_cast<size_t>(bin + sourceOffset)];
          auto& destination = bins[static_cast<size_t>(bin)];
          destination.real = destination.real * originalGain +
                             source.real * static_cast<float>(copyGain);
          destination.imaginary = destination.imaginary * originalGain +
                                  source.imaginary * static_cast<float>(copyGain);
        }
        continue;
      }

      double currentPower = 0.0;
      for (auto bin = firstBin; bin <= lastBin; ++bin) {
        if (!isEffectBinSelected(bin))
          continue;
        const auto& valueAtBin = bins[static_cast<size_t>(bin)];
        currentPower += static_cast<double>(valueAtBin.real) * valueAtBin.real +
                        static_cast<double>(valueAtBin.imaginary) * valueAtBin.imaginary;
      }

      auto matchGain = static_cast<double>(gain);
      if (targetPower <= 0.0)
        matchGain = 0.0;
      else if (currentPower > 0.0)
        matchGain *= std::sqrt(targetPower / currentPower);
      if (matchGain > 1.0e30)
        matchGain = 1.0;
      else if (matchGain < 1.0e-37)
        matchGain = 0.0;
      const auto totalGain = originalGain + matchGain;
      for (auto bin = firstBin; bin <= lastBin; ++bin) {
        if (!isEffectBinSelected(bin))
          continue;
        bins[static_cast<size_t>(bin)].real *= static_cast<float>(totalGain);
        bins[static_cast<size_t>(bin)].imaginary *= static_cast<float>(totalGain);
      }
    }
  };

  if (firstFrequencyBin <= secondFrequencyBin) {
    processRange(firstFrequencyBin, secondFrequencyBin);
  }
  else {
    processRange(0, secondFrequencyBin);
    processRange(firstFrequencyBin, maximumBin);
  }
}

void DtBlkFxEngine::applyStereoEffect(const EffectParameters& effect, int channelCount) noexcept
{
  if (channelCount != 2 || effect.type < 26 || effect.type > 30)
    return;

  const auto binCount = fftBinCount(fftSize);
  const auto maximumBin = binCount - 1;
  const auto firstFrequencyBin = getFrequencyBin(effect.frequencyA);
  const auto secondFrequencyBin = getFrequencyBin(effect.frequencyB);
  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  auto& left = channelStates[0].spectrum;
  auto& right = channelStates[1].spectrum;

  if (effect.type == 26) {
    const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
    const auto multiplyFraction = std::max((value - 0.875f) * 8.0f, 0.0f);
    const auto wetFraction = std::min(gain, 1.0f);
    const auto vocoderGain = gain * (1.0f - multiplyFraction);
    const auto vocoderMix = 1.0f - wetFraction * (1.0f - multiplyFraction);
    const auto multiplyGain = gain * multiplyFraction;
    const auto multiplyMix = 1.0f - wetFraction * multiplyFraction;
    int sourceStart = 1;
    int sourceEnd = maximumBin;
    int destinationStart = 1;
    int destinationEnd = maximumBin;
    if (firstFrequencyBin > secondFrequencyBin) {
      sourceStart = secondFrequencyBin;
      sourceEnd = firstFrequencyBin;
    }
    else {
      destinationStart = firstFrequencyBin;
      destinationEnd = secondFrequencyBin;
    }

    const auto destinationCount = destinationEnd - destinationStart + 1;
    const auto sourceCount = sourceEnd - sourceStart + 1;
    if (destinationCount > 0 && sourceCount > 0 && vocoderGain > 0.0f) {
      const auto requestedSegments =
          value < 0.875f ? value * (399.0f / 0.875f) + 1.0f : 400.0f;
      const auto segmentCount =
          std::clamp(static_cast<int>(requestedSegments), 1, destinationCount);
      for (int segment = 0; segment < segmentCount; ++segment) {
        const auto src0 =
            sourceStart + static_cast<int>(static_cast<double>(segment) * sourceCount /
                                           segmentCount);
        const auto src1 =
            sourceStart + static_cast<int>(static_cast<double>(segment + 1) * sourceCount /
                                           segmentCount) -
            1;
        const auto dst0 = destinationStart +
                          static_cast<int>(static_cast<double>(segment) * destinationCount /
                                           segmentCount);
        const auto dst1 =
            destinationStart + static_cast<int>(static_cast<double>(segment + 1) * destinationCount /
                                                segmentCount) -
            1;
        if (dst1 < dst0)
          continue;

        double sourcePower = 0.0;
        for (auto bin = src0; bin <= src1 && bin <= sourceEnd; ++bin) {
          const auto& source = left[static_cast<size_t>(bin)];
          sourcePower += static_cast<double>(source.real) * source.real +
                         static_cast<double>(source.imaginary) * source.imaginary;
        }
        double destinationPower = 0.0;
        for (auto bin = dst0; bin <= dst1 && bin <= destinationEnd; ++bin) {
          if (!isEffectBinSelected(bin))
            continue;
          const auto& destination = right[static_cast<size_t>(bin)];
          destinationPower += static_cast<double>(destination.real) * destination.real +
                              static_cast<double>(destination.imaginary) * destination.imaginary;
        }

        auto scale = static_cast<double>(vocoderMix);
        if (destinationPower > 0.0)
          scale += vocoderGain * std::sqrt(sourcePower / destinationPower);
        for (auto bin = dst0; bin <= dst1 && bin <= destinationEnd; ++bin) {
          if (!isEffectBinSelected(bin))
            continue;
          right[static_cast<size_t>(bin)].real *= static_cast<float>(scale);
          right[static_cast<size_t>(bin)].imaginary *= static_cast<float>(scale);
        }
      }
    }

    if (multiplyGain > 0.0f) {
      const auto first = std::min(firstFrequencyBin, secondFrequencyBin);
      const auto last = std::max(firstFrequencyBin, secondFrequencyBin);
      double inputPower = 0.0;
      double productPower = 0.0;
      for (auto bin = first; bin <= last; ++bin) {
        if (!isEffectBinSelected(bin))
          continue;
        auto& source = left[static_cast<size_t>(bin)];
        const auto& destination = right[static_cast<size_t>(bin)];
        inputPower += static_cast<double>(source.real) * source.real +
                      static_cast<double>(source.imaginary) * source.imaginary;
        const auto productReal =
            source.real * destination.real - source.imaginary * destination.imaginary;
        const auto productImaginary =
            source.real * destination.imaginary + source.imaginary * destination.real;
        source = {productReal, productImaginary};
        productPower += static_cast<double>(productReal) * productReal +
                        static_cast<double>(productImaginary) * productImaginary;
      }
      auto scale = 0.0;
      if (productPower > 0.0)
        scale = multiplyGain * std::sqrt(inputPower / productPower);
      for (auto bin = first; bin <= last; ++bin) {
        if (!isEffectBinSelected(bin))
          continue;
        auto& source = left[static_cast<size_t>(bin)];
        const auto& destination = right[static_cast<size_t>(bin)];
        source.real = static_cast<float>(source.real * scale +
                                         destination.real * multiplyMix);
        source.imaginary = static_cast<float>(source.imaginary * scale +
                                              destination.imaginary * multiplyMix);
      }
      right = left;
    }
    else {
      left = right;
    }
    return;
  }

  const auto firstBin = std::min(firstFrequencyBin, secondFrequencyBin);
  const auto lastBin = std::max(firstFrequencyBin, secondFrequencyBin);
  if (effect.type == 27 || effect.type == 28) {
    const auto sourceChannel = effect.type == 27 ? 0 : 1;
    const auto destinationChannel = 1 - sourceChannel;
    const auto& source = channelStates[static_cast<size_t>(sourceChannel)].spectrum;
    auto& destination = channelStates[static_cast<size_t>(destinationChannel)].spectrum;

    std::copy(source.begin(), source.end(), bins.begin());
    const auto sourceFundamental =
        findFundamentalBin(std::max(1, firstBin), std::max(1, lastBin));
    std::copy(destination.begin(), destination.end(), bins.begin());
    const auto destinationFundamental =
        findFundamentalBin(std::max(1, firstBin), std::max(1, lastBin));
    if (sourceFundamental <= 0.0f || destinationFundamental <= 0.0f) {
      left = destination;
      right = destination;
      return;
    }

    const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
    const auto splitValue = value >= 1.0f ? 4.0f : value * 4.0f;
    const auto harmonicType = std::min(static_cast<int>(splitValue), 3);
    const auto widthFraction = value >= 1.0f ? 1.0f : splitValue - harmonicType;
    auto spacing = destinationFundamental;
    auto offset = destinationFundamental;
    if (harmonicType == 1)
      spacing *= 2.0f;
    else if (harmonicType == 2) {
      offset *= 2.0f;
      spacing *= 2.0f;
    }
    else if (harmonicType == 3)
      offset *= 0.5f;
    spacing = std::max(spacing, 4.0f);
    const auto halfWidth = static_cast<int>(spacing * widthFraction) / 2;
    auto sourceSpacing = std::max(sourceFundamental, 4.0f);
    auto sourceOffset = sourceFundamental;
    if (harmonicType == 1)
      sourceSpacing *= 2.0f;
    else if (harmonicType == 2) {
      sourceOffset *= 2.0f;
      sourceSpacing *= 2.0f;
    }
    else if (harmonicType == 3)
      sourceOffset *= 0.5f;
    const auto mixBack = std::max(1.0f - gain, 0.0f);
    const auto firstHarmonic = static_cast<int>(std::floor((firstBin - offset) / spacing));
    const auto lastHarmonic = static_cast<int>(std::ceil((lastBin - offset) / spacing));
    for (auto harmonic = firstHarmonic; harmonic <= lastHarmonic; ++harmonic) {
      const auto destinationCenter =
          static_cast<int>(std::lround(offset + spacing * static_cast<float>(harmonic)));
      const auto rangeStart = std::max({1, firstBin, destinationCenter - halfWidth});
      const auto rangeEnd = std::min({maximumBin, lastBin, destinationCenter + halfWidth});
      if (rangeEnd < rangeStart)
        continue;
      const auto sourceCenter =
          static_cast<int>(std::lround(sourceOffset + sourceSpacing * static_cast<float>(harmonic)));
      const auto sourceStart = std::max(1, sourceCenter - halfWidth);
      const auto sourceEnd = std::min(maximumBin, sourceCenter + halfWidth);
      double sourcePower = 0.0;
      for (auto bin = sourceStart; bin <= sourceEnd; ++bin) {
        const auto& valueAtBin = source[static_cast<size_t>(bin)];
        sourcePower += static_cast<double>(valueAtBin.real) * valueAtBin.real +
                       static_cast<double>(valueAtBin.imaginary) * valueAtBin.imaginary;
      }
      double destinationPower = 0.0;
      for (auto bin = rangeStart; bin <= rangeEnd; ++bin) {
        if (!isEffectBinSelected(bin))
          continue;
        const auto& valueAtBin = destination[static_cast<size_t>(bin)];
        destinationPower += static_cast<double>(valueAtBin.real) * valueAtBin.real +
                            static_cast<double>(valueAtBin.imaginary) * valueAtBin.imaginary;
      }
      auto scale = mixBack;
      if (sourcePower > 0.0 && destinationPower > 0.0)
        scale += gain * std::sqrt(sourcePower / destinationPower);
      else if (sourcePower <= 0.0)
        scale = mixBack;
      for (auto bin = rangeStart; bin <= rangeEnd; ++bin) {
        if (!isEffectBinSelected(bin))
          continue;
        destination[static_cast<size_t>(bin)].real *= static_cast<float>(scale);
        destination[static_cast<size_t>(bin)].imaginary *= static_cast<float>(scale);
      }
    }
    left = destination;
    right = destination;
    return;
  }

  // CrossMix (29). WarpMix (30) shares this code as a stand-in: the legacy WarpMix is a different
  // algorithm that has not been ported.
  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto reverse = value >= 0.5f;
  const auto mix = reverse ? (value - 0.5f) * 2.0f : value * 2.0f;
  const auto& source = reverse ? right : left;
  auto& destination = reverse ? left : right;
  // Exponents on the squared magnitude, as in the legacy plugin: raise = clamp(1.6 * fraction, 0, 1) * 0.5.
  const auto sourceExponent = std::clamp(0.8f * (1.0f - mix), 0.0f, 0.5f);
  const auto destinationExponent = std::clamp(0.8f * mix, 0.0f, 0.5f);
  // The legacy CrossMix amplitude is a plain gain. Only the WarpMix stand-in blends the original back in.
  const auto mixBack = effect.type == 29 ? 0.0f : std::max(1.0f - gain, 0.0f);

  // Mixes the bins runFirst..runLast (one contiguous run of selected bins).
  const auto mixRun = [&](int runFirst, int runLast) {
    // First pass: the powers needed to match the level of the run.
    double sourceTotal = 0.0;
    double destinationTotal = 0.0;
    double mixedTotal = 0.0;
    for (int bin = runFirst; bin <= runLast; ++bin) {
      const auto& sourceValue = source[static_cast<size_t>(bin)];
      const auto& destinationValue = destination[static_cast<size_t>(bin)];
      const auto sourcePower = sourceValue.real * sourceValue.real +
                               sourceValue.imaginary * sourceValue.imaginary;
      const auto destinationPower = destinationValue.real * destinationValue.real +
                                    destinationValue.imaginary * destinationValue.imaginary;
      sourceTotal += sourcePower;
      destinationTotal += destinationPower;
      // The power law is applied to the squared magnitude and gives the magnitude directly.
      const auto magnitude = std::pow(std::max(sourcePower, 1.0e-30f), sourceExponent) *
                             std::pow(std::max(destinationPower, 1.0e-30f), destinationExponent);
      mixedTotal += static_cast<double>(magnitude) * magnitude;
    }

    // The output power of the run is the mix of the two input powers (legacy MatchPwr).
    const auto targetPower = sourceTotal * (1.0 - mix) + destinationTotal * mix;
    auto scale = 0.0;
    if (targetPower > 0.0) {
      scale = mixedTotal > 0.0 ? std::sqrt(targetPower / mixedTotal) : 1.0;
      if (scale > 1.0e30)
        scale = 1.0;
      else if (scale < 1.0e-37)
        scale = 0.0;
    }
    const auto outputGain = static_cast<float>(gain * scale);

    // Second pass: write the mixed bins.
    for (int bin = runFirst; bin <= runLast; ++bin) {
      const auto sourceValue = source[static_cast<size_t>(bin)];
      const auto destinationValue = destination[static_cast<size_t>(bin)];
      const auto sourcePower = sourceValue.real * sourceValue.real +
                               sourceValue.imaginary * sourceValue.imaginary;
      const auto destinationPower = destinationValue.real * destinationValue.real +
                                    destinationValue.imaginary * destinationValue.imaginary;
      const auto magnitude = std::pow(std::max(sourcePower, 1.0e-30f), sourceExponent) *
                             std::pow(std::max(destinationPower, 1.0e-30f), destinationExponent);
      const auto sourcePhase = std::atan2(sourceValue.imaginary, sourceValue.real);
      const auto destinationPhase = std::atan2(destinationValue.imaginary, destinationValue.real);
      const auto phase = sourcePhase * (1.0f - mix) + destinationPhase * mix;
      destination[static_cast<size_t>(bin)] = {
          destinationValue.real * mixBack + outputGain * magnitude * std::cos(phase),
          destinationValue.imaginary * mixBack + outputGain * magnitude * std::sin(phase)};
    }
  };

  int runStart = -1;
  for (int bin = std::max(1, firstBin); bin <= lastBin + 1; ++bin) {
    const auto selected = bin <= lastBin && isEffectBinSelected(bin);
    if (selected && runStart < 0) {
      runStart = bin;
      continue;
    }
    if (selected || runStart < 0)
      continue;
    mixRun(runStart, bin - 1);
    runStart = -1;
  }
  left = destination;
  right = destination;
}
void DtBlkFxEngine::applyHarmonicEffect(const EffectParameters& effect, size_t frameStart) noexcept
{
  if (effect.type != 11 && effect.type != 12 && effect.type != 13 && effect.type != 14)
    return;

  const auto value = std::isfinite(effect.value) ? std::clamp(effect.value, 0.0f, 1.0f) : 0.5f;
  const auto amplitude =
      std::isfinite(effect.amplitude) ? std::clamp(effect.amplitude, 0.0f, 1.0f) : 0.0f;
  const auto gain =
      amplitude <= 0.0f ? 0.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  const auto sourceGain = std::clamp(1.0f - gain, 0.0f, 1.0f);
  const auto binCount = fftBinCount(fftSize);
  const auto maximumBin = binCount - 1;
  const auto splitValue = value >= 1.0f ? 4.0f : value * 4.0f;
  const auto harmonicType = std::min(static_cast<int>(splitValue), 3);
  const auto typeFraction = value >= 1.0f ? 1.0f : splitValue - harmonicType;

  float fundamental = 0.0f;
  if (effect.type == 13) {
    fundamental = static_cast<float>(
        std::min(getFrequencyBin(effect.frequencyA), getFrequencyBin(effect.frequencyB)));
  }
  else {
    fundamental = findFundamentalBin(1, std::max(1, fftSize / 8));
  }

  if (!std::isfinite(fundamental) || fundamental < 1.0f)
    return;

  auto offset = fundamental;
  auto spacing = fundamental;
  switch (harmonicType) {
    case 1:
      spacing *= 2.0f;
      break;
    case 2:
      offset *= 2.0f;
      spacing *= 2.0f;
      break;
    case 3:
      offset *= 0.5f;
      break;
    default:
      break;
  }

  spacing = std::max(spacing, 4.0f);
  const auto halfWidth = static_cast<int>(spacing * typeFraction) / 2;
  const auto lowestBin = std::max(1, static_cast<int>(offset) - halfWidth);
  auto highestBin = std::min(maximumBin, static_cast<int>(spacing * 200.0f));
  if (effect.type == 13)
    highestBin =
        std::min(highestBin,
                 std::max(getFrequencyBin(effect.frequencyA), getFrequencyBin(effect.frequencyB)));
  if (lowestBin > highestBin)
    return;

  const auto repitchMode = effect.type == 12 && value < 0.7f;
  const auto harmonicShift = effect.type == 11 || effect.type == 12;
  auto frequencyMultiplier = 1.0f;
  if (effect.type == 11) {
    frequencyMultiplier = std::pow(2.0f, (value - 0.5f) * 6.0f);
  }
  else if (repitchMode) {
    constexpr double c0Frequency = 16.35159898333181161;
    constexpr double noteSpan = 127.5;
    const auto targetFrequency = c0Frequency * std::pow(2.0, value * noteSpan / 12.0);
    const auto sourceFrequency = fundamental * sampleRate / fftSize;
    if (sourceFrequency > 0.0)
      frequencyMultiplier = static_cast<float>(targetFrequency / sourceFrequency);
  }
  if (!std::isfinite(frequencyMultiplier) || frequencyMultiplier <= 0.0f)
    return;

  std::copy(bins.begin(), bins.end(), remapSource.begin());
  std::fill(remapAccumulator.begin(), remapAccumulator.end(), ComplexBin{0.0f, 0.0f});
  std::fill(selectionMask.begin(), selectionMask.end(), 0);

  const auto firstHarmonic = static_cast<int>(std::floor((lowestBin - offset) / spacing));
  const auto lastHarmonic = static_cast<int>(std::ceil((highestBin - offset) / spacing));
  for (auto harmonic = firstHarmonic; harmonic <= lastHarmonic; ++harmonic) {
    const auto center = offset + spacing * static_cast<float>(harmonic);
    const auto roundedCenter = static_cast<int>(std::lround(center));
    const auto start = std::max(lowestBin, roundedCenter - halfWidth);
    const auto end = std::min(highestBin, roundedCenter + halfWidth);
    if (end < start)
      continue;

    for (auto sourceBin = start; sourceBin <= end; ++sourceBin) {
      if (isEffectBinSelected(sourceBin))
        selectionMask[static_cast<size_t>(sourceBin)] = 1;
    }

    if (!harmonicShift)
      continue;

    const auto multiplier = frequencyMultiplier;
    const auto binShift = center * (multiplier - 1.0f);
    const auto shift = repitchMode ? binShift : center * (multiplier - 1.0f);
    const auto framePhase = static_cast<double>(frameStart % static_cast<size_t>(fftSize));
    for (auto sourceBin = start; sourceBin <= end; ++sourceBin) {
      if (!isEffectBinSelected(sourceBin))
        continue;
      const auto destination = static_cast<float>(sourceBin) + shift;
      if (destination < 0.0f || destination > static_cast<float>(maximumBin))
        continue;

      const auto lowerDestination = static_cast<int>(std::floor(destination));
      const auto fraction = destination - static_cast<float>(lowerDestination);
      const auto& source = remapSource[static_cast<size_t>(sourceBin)];
      const auto turns = std::remainder(framePhase * shift / static_cast<double>(fftSize), 1.0);
      constexpr double twoPi = 6.28318530717958647692;
      const auto phase = twoPi * turns;
      const auto phaseReal = static_cast<float>(std::cos(phase));
      const auto phaseImaginary = static_cast<float>(std::sin(phase));
      const auto shiftedReal = gain * (source.real * phaseReal - source.imaginary * phaseImaginary);
      const auto shiftedImaginary =
          gain * (source.real * phaseImaginary + source.imaginary * phaseReal);
      auto& lower = remapAccumulator[static_cast<size_t>(lowerDestination)];
      lower.real += shiftedReal * (1.0f - fraction);
      lower.imaginary += shiftedImaginary * (1.0f - fraction);
      if (fraction > 0.0f && lowerDestination + 1 <= maximumBin) {
        auto& upper = remapAccumulator[static_cast<size_t>(lowerDestination + 1)];
        upper.real += shiftedReal * fraction;
        upper.imaginary += shiftedImaginary * fraction;
      }
    }
  }

  for (int bin = lowestBin; bin <= highestBin; ++bin) {
    if (selectionMask[static_cast<size_t>(bin)] == 0)
      continue;
    if (harmonicShift) {
      bins[static_cast<size_t>(bin)].real = remapSource[static_cast<size_t>(bin)].real * sourceGain;
      bins[static_cast<size_t>(bin)].imaginary =
          remapSource[static_cast<size_t>(bin)].imaginary * sourceGain;
    }
  }
  if (harmonicShift) {
    for (int bin = 0; bin < binCount; ++bin) {
      bins[static_cast<size_t>(bin)].real += remapAccumulator[static_cast<size_t>(bin)].real;
      bins[static_cast<size_t>(bin)].imaginary +=
          remapAccumulator[static_cast<size_t>(bin)].imaginary;
    }
  }
  else {
    for (int bin = lowestBin; bin <= highestBin; ++bin) {
      if (selectionMask[static_cast<size_t>(bin)] == 0)
        continue;
      bins[static_cast<size_t>(bin)].real *= gain;
      bins[static_cast<size_t>(bin)].imaginary *= gain;
    }
  }

  bins.front().imaginary = 0.0f;
  bins.back().imaginary = 0.0f;
}
