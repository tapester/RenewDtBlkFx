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

#include <fftw3.h>

#include <array>
#include <cstdint>
#include <vector>

class DtBlkFxEngine {
public:
  // FFT settings are indexed by the legacy parameter format; keep their order stable.
  static constexpr int fftSizeCount = 34;
  static constexpr int maxFftSize = 80640;

  struct ComplexBin {
    float real;
    float imaginary;
  };

  struct EffectParameters {
    int type = 9;
    float frequencyA = 0.0f;
    float frequencyB = 1.0f;
    float amplitude = 0.6f;
    float value = 0.5f;
  };

  DtBlkFxEngine() = default;
  ~DtBlkFxEngine();

  DtBlkFxEngine(const DtBlkFxEngine&) = delete;
  DtBlkFxEngine& operator=(const DtBlkFxEngine&) = delete;

  // Allocate FFT plans and working buffers before audio processing begins.
  bool prepare(int fftSizeIndex = 0, double sampleRate = 44100.0);
  void release() noexcept;
  bool isPrepared() const noexcept { return timeBuffer != nullptr; }
  int getFftSize() const noexcept { return fftSize; }
  int getTimeWindowSamples() const noexcept { return timeWindowSamples; }
  // Reported latency. With a fixed latency budget (see setFixedLatency) this is the budget and
  // does not change with the FFT size; without one it is the current FFT size minus one.
  int getLatencySamples() const noexcept
  {
    if (fixedLatencySamples > 0)
      return fixedLatencySamples;
    return fftSize > 0 ? fftSize - 1 : 0;
  }
  int getFixedLatency() const noexcept { return fixedLatencySamples; }
  // Sets a fixed latency budget in samples, or 0 to go back to "FFT size minus one". Every frame
  // is written at its input start plus the budget, so the delay between input and output no
  // longer depends on the FFT size, and the FFT size is limited to the largest size that fits
  // the budget (size - 1 <= budget). Can be called while audio is running; call it before the
  // first sample (or after reset()) for a clean start.
  void setFixedLatency(int latencySamples) noexcept;
  int getHopSize() const noexcept { return hopSize; }
  int getNextHopSize() const noexcept { return nextHopSize; }
  size_t getNextFrameStartSamples() const noexcept { return nextFrameStart; }
  int getDelaySamples() const noexcept { return delaySamples; }
  int getActiveDelaySamples() const noexcept { return activeDelaySamples; }
  float getLegacyOverlapParameter() const noexcept { return legacyOverlapParameter; }
  void configureScheduler(float legacyOverlapParameter,
                          int delaySamples,
                          double samplesPerBeat = 0.0,
                          double beatPhaseSamples = 0.0,
                          int fftSizeIndex = -1) noexcept;
  bool configureLegacyParameters(const std::array<float, 44>& parameters,
                                double samplesPerBeat,
                                double beatPhaseSamples,
                                bool parameterSyncEnabled = false) noexcept;
  void configureEffects(const std::array<EffectParameters, 8>& effects,
                        float mixBack,
                        bool powerMatch) noexcept;
  void reset() noexcept;

  static int fftSizeAt(int index) noexcept;
  static int fftBinCount(int fftSize) noexcept;

  bool forward(const float* input, ComplexBin* output, int fftSize) noexcept;
  bool inverse(const ComplexBin* input, float* output, int fftSize) noexcept;
  bool process(float* const* channels, int channelCount, int sampleCount) noexcept;

private:
  static constexpr int maximumDelayMilliseconds = 6000;
  static constexpr size_t parameterEventCapacity = 4096;

  using LegacyParameters = std::array<float, 44>;

  struct ParameterEvent {
    // Parameter snapshots are timestamped so each FFT frame uses values at its start.
    size_t samplePosition = 0;
    size_t rampStartSample = 0;
    LegacyParameters values{};
  };

  struct Plans {
    fftwf_plan forward = nullptr;
    fftwf_plan inverse = nullptr;
  };

  struct ChannelState {
    std::vector<float> inputFrame;
    std::vector<float> outputFrame;
    std::vector<ComplexBin> spectrum;
    std::vector<float> outputRing;
    // Absolute sample tags prevent stale ring entries from being mistaken for live audio.
    std::vector<size_t> outputRingPositions;
  };

  bool processFrame(int channelCount) noexcept;
  void setFftSize(int size) noexcept;
  // The largest FFT size that fits the fixed latency budget (or the size itself without one).
  int limitedFftSize(int size) const noexcept;
  void updateFrameParameters(size_t frameStart) noexcept;
  void getInterpolatedParameters(size_t samplePosition, LegacyParameters& result) const noexcept;
  void pruneParameterEvents() noexcept;
  bool appendParameterEvent(const LegacyParameters& parameters) noexcept;
  int getDelaySamples(float parameter) const noexcept;
  void updateTimeWindow() noexcept;
  void updateHopSize() noexcept;
  void updateDelayTransition() noexcept;
  void setActiveDelaySamples(int delaySamples) noexcept;
  bool readOutputSample(const ChannelState& channel,
                        size_t position,
                        float& sample) const noexcept;
  float& getOutputSampleForWrite(ChannelState& channel, size_t position) noexcept;
  int getNextFrameHop(size_t frameStart) const noexcept;
  int getFrequencyBin(float normalizedFrequency) const noexcept;
  bool isEffectBinSelected(int bin) const noexcept;
  void updateEffectMask(const EffectParameters& mask) noexcept;
  void applyFilter(const EffectParameters& effect) noexcept;
  void applyContrast(const EffectParameters& effect) noexcept;
  void applySmear(const EffectParameters& effect) noexcept;
  void applyThreshold(const EffectParameters& effect) noexcept;
  void applyClip(const EffectParameters& effect) noexcept;
  void applyResize(const EffectParameters& effect, size_t frameStart) noexcept;
  void applyResample(const EffectParameters& effect, size_t frameStart) noexcept;
  void applyShift(const EffectParameters& effect, size_t frameStart) noexcept;
  void applyConstShift(const EffectParameters& effect, size_t frameStart) noexcept;
  void applyHarmonicEffect(const EffectParameters& effect, size_t frameStart) noexcept;
  void applySweep(const EffectParameters& effect) noexcept;
  void applyStereoEffect(const EffectParameters& effect, int channelCount) noexcept;
  float findFundamentalBin(int firstBin,
                           int lastBin,
                           int minimumSubharmonicBin = 3) const noexcept;
  // Refines an integer peak bin to a fractional position from the phase of the three bins around it.
  float estimatePeakBin(int centreBin) const noexcept;
  void remapFrequencies(const EffectParameters& effect,
                        float scale,
                        float offset,
                        bool centerRange,
                        bool conjugate,
                        size_t frameStart) noexcept;

  static constexpr std::array<int, fftSizeCount> fftSizes{
      256,   320,   384,   448,   512,   640,   768,   896,   1024,  1280, 1536,  1792,
      2048,  2560,  3072,  3584,  4096,  5120,  6144,  7168,  8192,  9600, 12288, 13440,
      16384, 20480, 24576, 28672, 32768, 40500, 49152, 57600, 65536, 80640};

  int findPlan(int fftSize) const noexcept;

  float* timeBuffer = nullptr;
  float* frequencyBuffer = nullptr;
  std::array<Plans, fftSizeCount> plans{};
  std::array<ChannelState, 2> channelStates{};
  std::vector<ComplexBin> bins;
  std::vector<ComplexBin> remapSource;
  std::vector<ComplexBin> remapAccumulator;
  std::vector<unsigned char> selectionMask;
  std::vector<unsigned char> effectMask;
  std::vector<ParameterEvent> parameterEvents;
  LegacyParameters latestParameterInput{};
  size_t parameterEventHead = 0;
  size_t parameterEventCount = 0;
  bool hasLegacyParameterInput = false;
  bool useLegacyParameterTimeline = false;
  bool parameterSyncEnabled = false;
  std::array<EffectParameters, 8> effects{};
  int fftSize = 0;
  int requestedFftSize = 0;
  int fixedLatencySamples = 0;
  int timeWindowSamples = 0;
  int preWindowSamples = 0;
  int hopSize = 0;
  int nextHopSize = 0;
  int delaySamples = 0;
  int activeDelaySamples = 0;
  int delayTransitionFromSamples = 0;
  int delayTransitionToSamples = 0;
  int delayTransitionLength = 0;
  int delayTransitionPosition = 0;
  double sampleRate = 44100.0;
  float legacyOverlapParameter = 0.0f;
  size_t ringSize = 0;
  size_t nextFrameStart = 0;
  size_t samplesProcessed = 0;
  size_t outputEndSample = 0;
  size_t firstOutputSample = 0;
  bool hasOutputFrame = false;
  double samplesPerBeat = 0.0;
  double beatStartSample = 0.0;
  int minimumHopSamples = 64;
  float mixBack = 1.0f;
  bool powerMatch = false;
  std::uint32_t smearRandomState = 1;
};
