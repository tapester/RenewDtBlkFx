#include "DtBlkFxEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
bool check(bool condition, int fftSize, const char* message)
{
  if (!condition)
    std::cerr << "FFT size " << fftSize << ": " << message << '\n';
  return condition;
}

float maxAbsDifference(const std::vector<float>& left, const std::vector<float>& right)
{
  float error = 0.0f;
  for (size_t i = 0; i < left.size(); ++i)
    error = std::max(error, std::abs(left[i] - right[i]));
  return error;
}

bool renderStartupTiming(int delaySamples,
                         const std::array<int, 5>& chunkSizes,
                         std::vector<float>& output)
{
  DtBlkFxEngine engine;
  if (!engine.prepare(0))
    return false;

  engine.configureScheduler(0.0f, delaySamples);
  const auto expectedFirstOutput =
      engine.getLatencySamples() + delaySamples + 1;
  output.assign(static_cast<size_t>(expectedFirstOutput + engine.getFftSize() + 32), 0.25f);

  int position = 0;
  size_t chunkIndex = 0;
  while (position < static_cast<int>(output.size())) {
    const auto count = std::min(chunkSizes[chunkIndex % chunkSizes.size()],
                                static_cast<int>(output.size()) - position);
    auto* channel = output.data() + position;
    if (!engine.process(&channel, 1, count))
      return false;
    position += count;
    ++chunkIndex;
  }

  const auto firstNonzero = std::find_if(output.begin(), output.end(), [](float sample) {
    return std::abs(sample) > 0.0001f;
  });
  if (firstNonzero == output.end() ||
      std::distance(output.begin(), firstNonzero) != expectedFirstOutput) {
    std::cerr << "Startup output did not begin at frame latency + delay + 1\n";
    return false;
  }

  const auto settledAt = expectedFirstOutput + 16;
  if (std::abs(output[static_cast<size_t>(settledAt)] - 0.25f) > 0.0001f) {
    std::cerr << "Startup fade did not settle by the sixteenth output sample\n";
    return false;
  }
  return true;
}

bool testStartupOutputTiming()
{
  constexpr std::array<int, 5> chunkSizes{17, 64, 5, 128, 31};
  constexpr std::array<int, 4> delays{0, 1, 256, 1024};
  bool passed = true;
  for (const auto delay : delays) {
    std::vector<float> wholeBlock;
    std::vector<float> chunked;
    DtBlkFxEngine engine;
    if (!engine.prepare(0))
      return false;

    engine.configureScheduler(0.0f, delay);
    const auto expectedFirstOutput = engine.getLatencySamples() + delay + 1;
    wholeBlock.assign(static_cast<size_t>(expectedFirstOutput + engine.getFftSize() + 32), 0.25f);
    auto* channel = wholeBlock.data();
    passed &= engine.process(&channel, 1, static_cast<int>(wholeBlock.size()));
    passed &= renderStartupTiming(delay, chunkSizes, chunked);
    passed &= check(maxAbsDifference(wholeBlock, chunked) < 0.0001f,
                    DtBlkFxEngine::fftSizeAt(0),
                    "startup timing depends on host chunk sizes");
  }
  return passed;
}

bool renderDelayTransition(const std::array<int, 5>& chunkSizes,
                           std::vector<float>& output,
                           bool sinusoidal = false)
{
  DtBlkFxEngine engine;
  if (!engine.prepare(0))
    return false;

  constexpr int increaseAt = 1024;
  constexpr int decreaseAt = 10000;
  constexpr int totalSamples = 18000;
  engine.configureScheduler(0.0f, 0);
  output.resize(totalSamples);
  constexpr float pi = 3.14159265358979323846f;
  for (int sample = 0; sample < totalSamples; ++sample)
    output[static_cast<size_t>(sample)] =
        sinusoidal ? 0.25f * std::sin(2.0f * pi * sample / 61.0f) : 0.25f;

  int position = 0;
  size_t chunkIndex = 0;
  while (position < totalSamples) {
    if (position == increaseAt)
      engine.configureScheduler(0.0f, 512);
    else if (position == decreaseAt) {
      if (engine.getActiveDelaySamples() != 512) {
        std::cerr << "Delay increase did not reach its target before the return transition\n";
        return false;
      }
      engine.configureScheduler(0.0f, 0);
    }

    auto count = std::min(chunkSizes[chunkIndex % chunkSizes.size()], totalSamples - position);
    if (position < increaseAt)
      count = std::min(count, increaseAt - position);
    else if (position < decreaseAt)
      count = std::min(count, decreaseAt - position);

    auto* channel = output.data() + position;
    if (!engine.process(&channel, 1, count))
      return false;
    position += count;
    ++chunkIndex;
  }
  return engine.getActiveDelaySamples() == 0;
}

bool testDelayTransitionsRemainContinuous()
{
  constexpr std::array<int, 5> firstChunkSizes{17, 64, 5, 128, 31};
  constexpr std::array<int, 5> secondChunkSizes{256, 73, 19, 512, 47};
  std::vector<float> first;
  std::vector<float> second;
  std::vector<float> sinusoidalFirst;
  std::vector<float> sinusoidalSecond;
  if (!renderDelayTransition(firstChunkSizes, first) ||
      !renderDelayTransition(secondChunkSizes, second) ||
      !renderDelayTransition(firstChunkSizes, sinusoidalFirst, true) ||
      !renderDelayTransition(secondChunkSizes, sinusoidalSecond, true))
    return false;

  if (maxAbsDifference(first, second) > 0.0001f ||
      maxAbsDifference(sinusoidalFirst, sinusoidalSecond) > 0.0001f) {
    std::cerr << "Delay transitions depend on host chunk sizes\n";
    return false;
  }

  const auto startupSettledAt = DtBlkFxEngine::fftSizeAt(0) - 1 + 16;
  for (size_t sample = startupSettledAt; sample < first.size(); ++sample) {
    if (std::abs(first[sample] - 0.25f) > 0.01f) {
      std::cerr << "Delay transition produced a gap or discontinuity at sample " << sample
                << ": " << first[sample] << '\n';
      return false;
    }
  }

  for (size_t sample = startupSettledAt + 1; sample < sinusoidalFirst.size(); ++sample) {
    if (std::abs(sinusoidalFirst[sample] - sinusoidalFirst[sample - 1]) > 0.1f) {
      std::cerr << "Delay transition produced an abrupt sinusoidal discontinuity at sample "
                << sample << '\n';
      return false;
    }
  }
  return true;
}

bool testScheduler(int fftSizeIndex, float overlapParameter, int delaySamples)
{
  DtBlkFxEngine wholeBlockEngine;
  DtBlkFxEngine chunkedEngine;
  if (!wholeBlockEngine.prepare(fftSizeIndex) || !chunkedEngine.prepare(fftSizeIndex)) {
    std::cerr << "Failed to prepare scheduler test engines\n";
    return false;
  }

  wholeBlockEngine.configureScheduler(overlapParameter, delaySamples);
  chunkedEngine.configureScheduler(overlapParameter, delaySamples);
  const auto fftSize = wholeBlockEngine.getFftSize();
  const auto sampleCount = fftSize * 3 + 37;
  const auto latency = wholeBlockEngine.getLatencySamples() + delaySamples;
  const auto windowSize = wholeBlockEngine.getTimeWindowSamples();
  const auto expectedHop = std::max(
      32,
      static_cast<int>(std::floor(static_cast<float>(windowSize - 16) +
                                  overlapParameter * (static_cast<float>(windowSize) * 0.15f -
                                                     static_cast<float>(windowSize - 16)))));
  std::array<std::vector<float>, 2> wholeInput;
  std::array<std::vector<float>, 2> chunkedInput;
  std::array<std::vector<float>, 2> wholeOutput;

  constexpr float pi = 3.14159265358979323846f;
  for (int channel = 0; channel < 2; ++channel) {
    wholeInput[static_cast<size_t>(channel)].resize(static_cast<size_t>(sampleCount));
    chunkedInput[static_cast<size_t>(channel)].resize(static_cast<size_t>(sampleCount));
    wholeOutput[static_cast<size_t>(channel)].resize(static_cast<size_t>(sampleCount));
    for (int sample = 0; sample < sampleCount; ++sample) {
      const auto phase = 2.0f * pi * static_cast<float>(sample) / static_cast<float>(fftSize);
      const auto value = channel == 0 ? std::sin(phase * 3.0f) : 0.5f * std::cos(phase * 5.0f);
      wholeInput[static_cast<size_t>(channel)][static_cast<size_t>(sample)] = value;
      wholeOutput[static_cast<size_t>(channel)][static_cast<size_t>(sample)] = value;
      chunkedInput[static_cast<size_t>(channel)][static_cast<size_t>(sample)] = value;
    }
  }

  std::array<float*, 2> wholeChannels{wholeInput[0].data(), wholeInput[1].data()};
  if (!wholeBlockEngine.process(wholeChannels.data(), 2, sampleCount)) {
    std::cerr << "Whole-block scheduler processing failed\n";
    return false;
  }

  int position = 0;
  constexpr std::array<int, 5> chunkSizes{17, 64, 5, 128, 31};
  int chunkIndex = 0;
  while (position < sampleCount) {
    const auto count = std::min(chunkSizes[static_cast<size_t>(chunkIndex % chunkSizes.size())],
                                sampleCount - position);
    std::array<float*, 2> chunkChannels{chunkedInput[0].data() + position,
                                        chunkedInput[1].data() + position};
    if (!chunkedEngine.process(chunkChannels.data(), 2, count)) {
      std::cerr << "Chunked scheduler processing failed\n";
      return false;
    }
    position += count;
    ++chunkIndex;
  }

  bool passed = true;
  constexpr float tolerance = 0.0001f;
  passed &= check(wholeBlockEngine.getHopSize() == expectedHop,
                  fftSize,
                  "overlap parameter did not set the expected legacy hop size");
  passed &= check(wholeBlockEngine.getDelaySamples() == delaySamples,
                  fftSize,
                  "scheduler delay was not applied");
  for (int channel = 0; channel < 2; ++channel) {
    const auto channelIndex = static_cast<size_t>(channel);
    passed &=
        check(maxAbsDifference(wholeInput[channelIndex], chunkedInput[channelIndex]) < tolerance,
              fftSize,
              "scheduler output depends on host block size");
    for (int sample = 0; sample < sampleCount; ++sample) {
      const auto expected =
          sample < latency
              ? 0.0f
              : wholeOutput[channelIndex][static_cast<size_t>(sample - latency)] *
                    (sample < latency + 16 ? static_cast<float>(sample - latency) / 15.0f : 1.0f);
      passed &= check(std::abs(wholeInput[channelIndex][static_cast<size_t>(sample)] - expected) <
                          tolerance,
                      fftSize,
                      "scheduler latency or bypass output is incorrect");
    }
  }

  return passed;
}

bool testSchedulerReset()
{
  DtBlkFxEngine engine;
  if (!engine.prepare(0))
    return false;

  std::vector<float> signal(static_cast<size_t>(engine.getFftSize() + 16), 1.0f);
  float* signalChannel = signal.data();
  if (!engine.process(&signalChannel, 1, static_cast<int>(signal.size())) ||
      std::abs(signal[static_cast<size_t>(engine.getLatencySamples() + 15)] - 1.0f) > 0.0001f) {
    std::cerr << "Scheduler failed to emit the end of its first frame\n";
    return false;
  }

  engine.reset();
  std::vector<float> silence(static_cast<size_t>(engine.getFftSize() * 2), 0.0f);
  float* silenceChannel = silence.data();
  if (!engine.process(&silenceChannel, 1, static_cast<int>(silence.size())) ||
      std::any_of(silence.begin(), silence.end(), [](float sample) { return sample != 0.0f; })) {
    std::cerr << "Scheduler reset retained buffered audio\n";
    return false;
  }
  return true;
}

bool testBeatSynchronizedHop()
{
  DtBlkFxEngine engine;
  if (!engine.prepare(8))
    return false;

  constexpr float overlapParameter = 0.8f;
  engine.configureScheduler(overlapParameter, 0, 287.5, 200.0);
  const auto nominalHop = engine.getHopSize();
  if (nominalHop != 324 || engine.getNextHopSize() != 87) {
    std::cerr << "Scheduler failed to align its first hop to the legacy beat grid\n";
    return false;
  }

  std::vector<float> silence(static_cast<size_t>(engine.getFftSize()), 0.0f);
  float* channel = silence.data();
  if (!engine.process(&channel, 1, static_cast<int>(silence.size())) ||
      engine.getNextHopSize() != 288) {
    std::cerr << "Scheduler failed to align a hop to the legacy beat grid: "
              << engine.getNextHopSize() << " instead of 288\n";
    return false;
  }
  return true;
}

bool testSchedulerFftSizeTransition()
{
  DtBlkFxEngine engine;
  constexpr int requestedPlan = 8;
  constexpr int nextPlan = 7;
  constexpr int shortDelay = 200;
  if (!engine.prepare(requestedPlan))
    return false;

  engine.configureScheduler(0.0f, shortDelay, 0.0, 0.0, requestedPlan);
  if (engine.getFftSize() != DtBlkFxEngine::fftSizeAt(requestedPlan) ||
      engine.getTimeWindowSamples() != DtBlkFxEngine::fftSizeAt(requestedPlan)) {
    std::cerr << "Runtime FFT plan or window was incorrectly reduced by output delay\n";
    return false;
  }

  std::vector<float> firstSample(1, 0.0f);
  float* channel = firstSample.data();
  if (!engine.process(&channel, 1, 1)) {
    std::cerr << "Scheduler failed while collecting a partial frame\n";
    return false;
  }

  engine.configureScheduler(0.5f, 1000, 0.0, 0.0, nextPlan);
  if (engine.getFftSize() != DtBlkFxEngine::fftSizeAt(requestedPlan) ||
      engine.getTimeWindowSamples() != DtBlkFxEngine::fftSizeAt(requestedPlan)) {
    std::cerr << "Scheduler changed FFT size in the middle of an input frame\n";
    return false;
  }

  std::vector<float> remaining(
      static_cast<size_t>(DtBlkFxEngine::fftSizeAt(requestedPlan) - 1), 0.0f);
  channel = remaining.data();
  if (!engine.process(&channel, 1, static_cast<int>(remaining.size())) ||
      engine.getFftSize() != DtBlkFxEngine::fftSizeAt(nextPlan) ||
      engine.getTimeWindowSamples() != DtBlkFxEngine::fftSizeAt(nextPlan)) {
    std::cerr << "Scheduler did not apply its new FFT size at the next frame boundary\n";
    return false;
  }
  return true;
}

bool testParameterDrivenDelayChangeRemainsContinuous()
{
  DtBlkFxEngine engine;
  if (!engine.prepare(0))
    return false;

  std::array<float, 44> parameters{};
  parameters[0] = 0.501f;
  parameters[2] = 8.0f / 255.0f;
  for (int slot = 0; slot < 8; ++slot)
    parameters[static_cast<size_t>(4 + slot * 5 + 3)] = 76.0f / 255.0f;

  constexpr double samplesPerBeat = 128.0;
  if (!engine.configureLegacyParameters(parameters, samplesPerBeat, 0.0) ||
      engine.getDelaySamples() != 100)
    return false;

  constexpr int initialSamples = 868;
  constexpr int totalSamples = 7000;
  std::vector<float> input(static_cast<size_t>(totalSamples), 0.25f);
  float* channel = input.data();
  if (!engine.process(&channel, 1, initialSamples))
    return false;

  parameters[1] = 0.51f;
  if (!engine.configureLegacyParameters(parameters, samplesPerBeat, 0.0))
    return false;

  channel = input.data() + initialSamples;
  if (!engine.process(&channel, 1, totalSamples - initialSamples) ||
      engine.getDelaySamples() != 5292) {
    std::cerr << "Delay increase was not applied at the next frame boundary\n";
    return false;
  }

  if (engine.getActiveDelaySamples() != engine.getDelaySamples()) {
    std::cerr << "Parameter-driven delay crossfade did not reach its target\n";
    return false;
  }

  const auto settledAt = engine.getLatencySamples() + 100 + 16;
  for (int sample = settledAt; sample < totalSamples; ++sample) {
    if (std::abs(input[static_cast<size_t>(sample)] - 0.25f) > 0.01f) {
      std::cerr << "Parameter-driven delay transition produced a gap at sample " << sample
                << ": " << input[static_cast<size_t>(sample)] << '\n';
      return false;
    }
  }
  return true;
}

bool testLegacyParameterInterpolation()
{
  DtBlkFxEngine engine;
  constexpr int requestedPlan = 8;
  if (!engine.prepare(requestedPlan))
    return false;

  std::array<float, 44> initialParameters{};
  initialParameters[0] = 1.0f;
  initialParameters[2] = static_cast<float>((requestedPlan + 2) * 4) / 255.0f;
  initialParameters[3] = 0.5f;
  for (int slot = 0; slot < 8; ++slot)
    initialParameters[static_cast<size_t>(4 + slot * 5 + 3)] = 76.0f / 255.0f;

  constexpr double samplesPerBeat = 22050.0;
  const auto expectedWindow = DtBlkFxEngine::fftSizeAt(requestedPlan);
  if (!engine.configureLegacyParameters(initialParameters, samplesPerBeat, 0.0) ||
      engine.getFftSize() != DtBlkFxEngine::fftSizeAt(requestedPlan) ||
      engine.getTimeWindowSamples() != expectedWindow ||
      engine.getDelaySamples() != 100) {
    std::cerr << "Legacy delay incorrectly changed the selected runtime FFT window\n";
    return false;
  }

  std::vector<float> firstBlock(1700, 0.0f);
  float* channel = firstBlock.data();
  if (!engine.process(&channel, 1, static_cast<int>(firstBlock.size()))) {
    std::cerr << "Short-window parameter interpolation setup failed\n";
    return false;
  }

  auto targetParameters = initialParameters;
  targetParameters[1] = 1.0f;
  if (!engine.configureLegacyParameters(targetParameters, samplesPerBeat, 0.0) ||
      engine.getFftSize() != DtBlkFxEngine::fftSizeAt(requestedPlan) ||
      engine.getTimeWindowSamples() != expectedWindow ||
      engine.getDelaySamples() <= 5000 || engine.getDelaySamples() >= 264600) {
    std::cerr << "Parameter changes were not interpolated at the pending frame's sample time\n";
    return false;
  }

  std::vector<float> secondBlock(1692, 0.0f);
  channel = secondBlock.data();
  if (!engine.process(&channel, 1, static_cast<int>(secondBlock.size())) ||
      engine.getDelaySamples() != 264600) {
    std::cerr << "Interpolated legacy parameter did not reach its target sample value\n";
    return false;
  }
  return true;
}

bool testParameterSynchronizedFrameStart()
{
  DtBlkFxEngine engine;
  if (!engine.prepare(0))
    return false;

  std::array<float, 44> parameters{};
  parameters[0] = 1.0f;
  parameters[2] = 8.0f / 255.0f;
  parameters[3] = 0.0f;
  constexpr double samplesPerBeat = 128.0;
  if (!engine.configureLegacyParameters(parameters, samplesPerBeat, 0.0))
    return false;

  std::vector<float> input(64, 0.0f);
  float* channel = input.data();
  if (!engine.process(&channel, 1, static_cast<int>(input.size())))
    return false;

  parameters[4 + 2] = 0.75f;
  if (!engine.configureLegacyParameters(parameters, samplesPerBeat, 0.0) ||
      engine.getNextHopSize() == 64 ||
      !engine.process(&channel, 1, 1)) {
    std::cerr << "Parameter sync should ignore events at the minimum-hop threshold\n";
    return false;
  }

  parameters[4 + 3] = 0.5f;
  if (!engine.configureLegacyParameters(parameters, samplesPerBeat, 0.0, true) ||
      engine.getNextHopSize() != 65) {
    std::cerr << "Parameter sync did not align to an event beyond the minimum-hop threshold\n";
    return false;
  }

  std::vector<float> remaining(191, 0.0f);
  channel = remaining.data();
  if (!engine.process(&channel, 1, static_cast<int>(remaining.size())) ||
      engine.getNextFrameStartSamples() != 65) {
    std::cerr << "Scheduler did not advance to the synchronized parameter sample\n";
    return false;
  }
  return true;
}

bool testFilterEffect(float bandEnd, float amplitude, bool powerMatch, int signalBin)
{
  DtBlkFxEngine engine;
  if (!engine.prepare(0))
    return false;

  std::array<DtBlkFxEngine::EffectParameters, 8> effects{};
  effects[0] = {0, 0.0f, bandEnd, amplitude};
  engine.configureEffects(effects, 0.0f, powerMatch);
  engine.configureScheduler(0.0f, 0);

  const auto fftSize = engine.getFftSize();
  const auto sampleCount = fftSize * 4;
  const auto latency = engine.getLatencySamples();
  std::vector<float> input(static_cast<size_t>(sampleCount));
  std::vector<float> original(static_cast<size_t>(sampleCount));
  constexpr float pi = 3.14159265358979323846f;
  for (int sample = 0; sample < sampleCount; ++sample) {
    const auto value = std::sin(2.0f * pi * signalBin * sample / fftSize);
    input[static_cast<size_t>(sample)] = value;
    original[static_cast<size_t>(sample)] = value;
  }

  float* channel = input.data();
  if (!engine.process(&channel, 1, sampleCount)) {
    std::cerr << "Filter effect processing failed\n";
    return false;
  }

  const auto expectedGain =
      powerMatch || bandEnd < 0.1f ? 1.0f : std::pow(10.0f, (amplitude * 100.0f - 60.0f) * 0.05f);
  constexpr float tolerance = 0.0002f;
  for (int sample = latency + 16; sample < sampleCount; ++sample) {
    const auto expected = original[static_cast<size_t>(sample - latency)] * expectedGain;
    if (std::abs(input[static_cast<size_t>(sample)] - expected) > tolerance) {
      std::cerr << "Filter output mismatch at sample " << sample << ": "
                << input[static_cast<size_t>(sample)] << " != " << expected << '\n';
      return false;
    }
  }
  return true;
}

bool testContrastEffect()
{
  constexpr int fftSize = 256;
  constexpr int sampleCount = fftSize * 4;
  constexpr int firstToneBin = 3;
  constexpr int secondToneBin = 11;
  constexpr float firstToneAmplitude = 0.6f;
  constexpr float secondToneAmplitude = 0.15f;
  constexpr float pi = 3.14159265358979323846f;

  const auto render = [=](float contrastValue, std::vector<float>& output) {
    DtBlkFxEngine engine;
    if (!engine.prepare(0))
      return false;

    std::array<DtBlkFxEngine::EffectParameters, 8> effects{};
    effects[0] = {1, 0.0f, 1.0f, 0.6f, contrastValue};
    engine.configureEffects(effects, 0.0f, false);

    output.resize(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
      output[static_cast<size_t>(sample)] =
          firstToneAmplitude * std::sin(2.0f * pi * firstToneBin * sample / fftSize) +
          secondToneAmplitude * std::sin(2.0f * pi * secondToneBin * sample / fftSize);
    }

    float* channel = output.data();
    return engine.process(&channel, 1, sampleCount);
  };

  std::vector<float> neutralOutput;
  std::vector<float> contrastedOutput;
  if (!render(0.5f, neutralOutput) || !render(0.8f, contrastedOutput)) {
    std::cerr << "Contrast effect processing failed\n";
    return false;
  }

  const auto measureToneAmplitude = [=](const std::vector<float>& signal, int bin) {
    double projection = 0.0;
    const auto start = 271;
    for (int sample = 0; sample < fftSize; ++sample)
      projection += signal[static_cast<size_t>(start + sample)] *
                    std::sin(2.0 * pi * bin * (sample + start - 255) / fftSize);
    return static_cast<float>(2.0 * projection / fftSize);
  };

  const auto neutralFirst = measureToneAmplitude(neutralOutput, firstToneBin);
  const auto neutralSecond = measureToneAmplitude(neutralOutput, secondToneBin);
  const auto contrastedFirst = measureToneAmplitude(contrastedOutput, firstToneBin);
  const auto contrastedSecond = measureToneAmplitude(contrastedOutput, secondToneBin);
  const auto neutralRatio = neutralSecond / neutralFirst;
  const auto contrastedRatio = contrastedSecond / contrastedFirst;

  if (std::abs(neutralFirst - firstToneAmplitude) > 0.002f ||
      std::abs(neutralSecond - secondToneAmplitude) > 0.002f ||
      contrastedRatio >= neutralRatio * 0.5f) {
    std::cerr << "Contrast did not preserve neutral output and increase spectral contrast\n";
    return false;
  }

  double inputPower = 0.0;
  double outputPower = 0.0;
  for (int sample = 271; sample < 271 + fftSize; ++sample) {
    const auto sourceSample = sample - 255;
    const auto original =
        firstToneAmplitude * std::sin(2.0f * pi * firstToneBin * sourceSample / fftSize) +
        secondToneAmplitude * std::sin(2.0f * pi * secondToneBin * sourceSample / fftSize);
    inputPower += original * original;
    outputPower += contrastedOutput[static_cast<size_t>(sample)] *
                   contrastedOutput[static_cast<size_t>(sample)];
  }

  if (std::abs(outputPower - inputPower) > inputPower * 0.01) {
    std::cerr << "Contrast failed to preserve the effect-band power\n";
    return false;
  }
  return true;
}

float frequencyParameterForBin(int bin, int fftSize, double sampleRate)
{
  constexpr double c0Frequency = 16.35159898333181161;
  const auto frequency = static_cast<double>(bin) * sampleRate / fftSize;
  return static_cast<float>(12.0 * std::log2(frequency / c0Frequency) / 127.5);
}

bool renderTestEffect(int effectType,
                      float effectValue,
                      float effectAmplitude,
                      std::vector<float>& output)
{
  constexpr int fftSize = 256;
  constexpr int sampleCount = fftSize * 8;
  constexpr double sampleRate = 44100.0;
  constexpr float pi = 3.14159265358979323846f;
  DtBlkFxEngine engine;
  if (!engine.prepare(0, sampleRate))
    return false;

  std::array<DtBlkFxEngine::EffectParameters, 8> effects{};
  effects[0] = {effectType,
                frequencyParameterForBin(3, fftSize, sampleRate),
                frequencyParameterForBin(16, fftSize, sampleRate),
                effectAmplitude,
                effectValue};
  engine.configureEffects(effects, 0.0f, false);
  engine.configureScheduler(0.0f, 0);

  output.resize(sampleCount);
  for (int sample = 0; sample < sampleCount; ++sample) {
    float value = 0.0f;
    for (int bin = 3; bin <= 13; ++bin) {
      auto amplitude = 0.01f;
      if (bin == 4)
        amplitude = 0.15f;
      else if (bin >= 10 && bin <= 14)
        amplitude = 0.8f;
      value += amplitude * std::sin(2.0f * pi * bin * sample / fftSize);
    }
    output[static_cast<size_t>(sample)] = value;
  }

  float* channel = output.data();
  return engine.process(&channel, 1, sampleCount);
}

float measureToneAmplitude(const std::vector<float>& signal, int bin, int start, int latency)
{
  constexpr int fftSize = 256;
  constexpr double pi = 3.14159265358979323846;
  double sineProjection = 0.0;
  double cosineProjection = 0.0;
  for (int sample = 0; sample < fftSize; ++sample) {
    const auto sourceSample = start + sample - latency;
    const auto phase = 2.0 * pi * bin * sourceSample / fftSize;
    const auto value = signal[static_cast<size_t>(start + sample)];
    sineProjection += value * std::sin(phase);
    cosineProjection += value * std::cos(phase);
  }
  return static_cast<float>(2.0 * std::hypot(sineProjection, cosineProjection) / fftSize);
}

bool renderMaskedFilter(int maskType, float maskValue, std::vector<float>& output)
{
  constexpr int fftSize = 256;
  constexpr int sampleCount = fftSize * 8;
  constexpr double sampleRate = 44100.0;
  constexpr float pi = 3.14159265358979323846f;
  DtBlkFxEngine engine;
  if (!engine.prepare(0, sampleRate))
    return false;

  std::array<DtBlkFxEngine::EffectParameters, 8> effects{};
  effects[0] = {maskType,
                frequencyParameterForBin(4, fftSize, sampleRate),
                frequencyParameterForBin(16, fftSize, sampleRate),
                0.6f,
                maskValue};
  effects[1] = {0,
                frequencyParameterForBin(3, fftSize, sampleRate),
                frequencyParameterForBin(16, fftSize, sampleRate),
                0.0f,
                0.5f};
  engine.configureEffects(effects, 0.0f, false);
  engine.configureScheduler(0.0f, 0);

  output.resize(sampleCount);
  for (int sample = 0; sample < sampleCount; ++sample) {
    output[static_cast<size_t>(sample)] =
        0.15f * std::sin(2.0f * pi * 4 * sample / fftSize) +
        0.2f * std::sin(2.0f * pi * 8 * sample / fftSize) +
        0.8f * std::sin(2.0f * pi * 12 * sample / fftSize);
  }
  float* channel = output.data();
  return engine.process(&channel, 1, sampleCount);
}

bool testFrequencyMasks()
{
  std::vector<float> harmonicMask;
  std::vector<float> thresholdMask;
  if (!renderMaskedFilter(20, 0.625f, harmonicMask) ||
      !renderMaskedFilter(25, 0.49f, thresholdMask)) {
    std::cerr << "Frequency mask processing failed\n";
    return false;
  }

  constexpr int latency = 255;
  constexpr int measurementStart = latency + 512;
  const auto harmonicUnselected = measureToneAmplitude(harmonicMask, 4, measurementStart, latency);
  const auto harmonicSelected = measureToneAmplitude(harmonicMask, 8, measurementStart, latency);
  const auto thresholdUnselected =
      measureToneAmplitude(thresholdMask, 4, measurementStart, latency);
  const auto thresholdSelected =
      measureToneAmplitude(thresholdMask, 12, measurementStart, latency);
  if (harmonicSelected > 0.03f || harmonicUnselected < 0.1f || thresholdUnselected < 0.1f ||
      thresholdSelected > 0.03f) {
    std::cerr << "Frequency mask mismatch: harmonic(" << harmonicSelected << ", "
              << harmonicUnselected << "), threshold(" << thresholdUnselected << ", "
              << thresholdSelected << ")\n";
    return false;
  }
  return true;
}

bool testStereoEffects()
{
  constexpr int fftSize = 256;
  constexpr int sampleCount = fftSize * 8;
  constexpr int latency = fftSize - 1;
  constexpr double sampleRate = 44100.0;
  constexpr float pi = 3.14159265358979323846f;
  const std::array<float, 5> values{0.5f, 0.125f, 0.125f, 0.2f, 0.7f};

  for (int effectType = 26; effectType <= 30; ++effectType) {
    DtBlkFxEngine engine;
    if (!engine.prepare(0, sampleRate))
      return false;

    std::array<DtBlkFxEngine::EffectParameters, 8> effects{};
    effects[0] = {effectType,
                  frequencyParameterForBin(4, fftSize, sampleRate),
                  frequencyParameterForBin(32, fftSize, sampleRate),
                  0.7f,
                  values[static_cast<size_t>(effectType - 26)]};
    engine.configureEffects(effects, 0.0f, false);
    engine.configureScheduler(0.0f, 0);

    std::array<std::vector<float>, 2> channels;
    for (auto& channel : channels)
      channel.resize(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
      channels[0][static_cast<size_t>(sample)] =
          0.5f * std::sin(2.0f * pi * 4 * sample / fftSize) +
          0.2f * std::sin(2.0f * pi * 8 * sample / fftSize) +
          0.1f * std::sin(2.0f * pi * 12 * sample / fftSize);
      channels[1][static_cast<size_t>(sample)] =
          0.1f * std::sin(2.0f * pi * 4 * sample / fftSize) +
          0.4f * std::sin(2.0f * pi * 8 * sample / fftSize) +
          0.6f * std::sin(2.0f * pi * 12 * sample / fftSize);
    }

    const auto leftInput = channels[0];
    const auto rightInput = channels[1];
    std::array<float*, 2> channelPointers{channels[0].data(), channels[1].data()};
    if (!engine.process(channelPointers.data(), 2, sampleCount)) {
      std::cerr << "Stereo effect " << effectType << " processing failed\n";
      return false;
    }

    auto channelsMatch = true;
    auto changed = false;
    for (int sample = latency + 512; sample < sampleCount; ++sample) {
      const auto index = static_cast<size_t>(sample);
      channelsMatch &= std::abs(channels[0][index] - channels[1][index]) < 1.0e-5f;
      changed |= std::abs(channels[0][index] -
                          leftInput[static_cast<size_t>(sample - latency)]) > 0.001f;
      if (!std::isfinite(channels[0][index]) || !std::isfinite(channels[1][index])) {
        std::cerr << "Stereo effect " << effectType << " produced a non-finite sample\n";
        return false;
      }
    }
    if (!channelsMatch || !changed ||
        leftInput[static_cast<size_t>(latency + 512)] ==
            rightInput[static_cast<size_t>(latency + 512)]) {
      std::cerr << "Stereo effect " << effectType
                << " did not mix the input channels as expected\n";
      return false;
    }
  }
  return true;
}

bool testSmearEffect()
{
  std::vector<float> noSmear;
  std::vector<float> smear;
  std::vector<float> repeatedSmear;
  if (!renderTestEffect(2, 0.0f, 0.6f, noSmear) || !renderTestEffect(2, 1.0f, 0.6f, smear) ||
      !renderTestEffect(2, 1.0f, 0.6f, repeatedSmear)) {
    std::cerr << "Smear effect processing failed\n";
    return false;
  }

  const auto latency = 255;
  bool changed = false;
  for (size_t sample = static_cast<size_t>(latency + 16); sample < smear.size(); ++sample) {
    changed |= std::abs(smear[sample] - noSmear[sample]) > 0.001f;
    if (smear[sample] != repeatedSmear[sample]) {
      std::cerr << "Smear output is not deterministic after engine preparation\n";
      return false;
    }
  }
  if (!changed) {
    std::cerr << "Full Smear did not alter the signal phase\n";
    return false;
  }
  return true;
}

bool testThresholdEffect()
{
  std::vector<float> aboveThreshold;
  std::vector<float> belowThreshold;
  if (!renderTestEffect(3, 0.49f, 0.0f, aboveThreshold) ||
      !renderTestEffect(3, 0.99f, 0.0f, belowThreshold)) {
    std::cerr << "Threshold effect processing failed\n";
    return false;
  }

  constexpr int latency = 255;
  constexpr int measurementStart = latency + 512;
  const auto aboveLow = measureToneAmplitude(aboveThreshold, 4, measurementStart, latency);
  const auto aboveHigh = measureToneAmplitude(aboveThreshold, 12, measurementStart, latency);
  const auto belowLow = measureToneAmplitude(belowThreshold, 4, measurementStart, latency);
  const auto belowHigh = measureToneAmplitude(belowThreshold, 12, measurementStart, latency);
  if (aboveLow < 0.12f || aboveHigh > 0.08f || belowLow > 0.08f || belowHigh < 0.65f) {
    std::cerr << "Threshold output mismatch: above(" << aboveLow << ", " << aboveHigh << "), below("
              << belowLow << ", " << belowHigh << ")\n";
    return false;
  }
  return true;
}

bool testClipEffect()
{
  std::vector<float> unprocessed;
  std::vector<float> clipped;
  if (!renderTestEffect(9, 0.5f, 0.6f, unprocessed) || !renderTestEffect(4, 0.8f, 0.6f, clipped)) {
    std::cerr << "Clip effect processing failed\n";
    return false;
  }

  constexpr int latency = 255;
  constexpr int measurementStart = latency + 512;
  const auto originalLow = measureToneAmplitude(unprocessed, 4, measurementStart, latency);
  const auto originalHigh = measureToneAmplitude(unprocessed, 12, measurementStart, latency);
  const auto clippedLow = measureToneAmplitude(clipped, 4, measurementStart, latency);
  const auto clippedHigh = measureToneAmplitude(clipped, 12, measurementStart, latency);
  if (clippedLow <= 0.0f || clippedHigh / clippedLow >= originalHigh / originalLow * 0.6f) {
    std::cerr << "Clip did not reduce the relative level of the strongest spectral bin\n";
    return false;
  }
  return true;
}

bool testFrequencyMappingEffects()
{
  constexpr int latency = 255;
  constexpr int measurementStart = latency + 512;
  std::vector<float> resized;
  std::vector<float> resampled;
  std::vector<float> shifted;
  std::vector<float> constantShifted;
  std::vector<float> downsampled;
  std::vector<float> downshifted;
  std::vector<float> constantDownshifted;
  if (!renderTestEffect(5, 0.625f, 0.6f, resized) ||
      !renderTestEffect(6, 0.5833333f, 0.6f, resampled) ||
      !renderTestEffect(7, 0.5833333f, 0.6f, shifted) ||
      !renderTestEffect(8, 0.610701f, 0.6f, constantShifted) ||
      !renderTestEffect(6, 0.4166667f, 0.6f, downsampled) ||
      !renderTestEffect(7, 0.4166667f, 0.6f, downshifted) ||
      !renderTestEffect(8, 0.389299f, 0.6f, constantDownshifted)) {
    std::cerr << "Frequency mapping effect processing failed\n";
    return false;
  }

  const auto resizedTone = std::max(measureToneAmplitude(resized, 7, measurementStart, latency),
                                    measureToneAmplitude(resized, 8, measurementStart, latency));
  const auto resampledTone = measureToneAmplitude(resampled, 17, measurementStart, latency);
  const auto shiftedTone = measureToneAmplitude(shifted, 17, measurementStart, latency);
  const auto constantShiftedTone =
      measureToneAmplitude(constantShifted, 15, measurementStart, latency);
  const auto downsampledTone = measureToneAmplitude(downsampled, 8, measurementStart, latency);
  const auto downshiftedTone = measureToneAmplitude(downshifted, 8, measurementStart, latency);
  const auto constantDownshiftedTone =
      measureToneAmplitude(constantDownshifted, 9, measurementStart, latency);
  if (resizedTone < 0.2f || resampledTone < 0.3f || shiftedTone < 0.3f ||
      constantShiftedTone < 0.3f || downsampledTone < 0.3f || downshiftedTone < 0.3f ||
      constantDownshiftedTone < 0.3f) {
    std::cerr << "Resize/Resample/Shift/ConstShift did not move spectral energy as expected: "
              << resizedTone << ", " << resampledTone << ", " << shiftedTone << ", "
              << constantShiftedTone << ", " << downsampledTone << ", " << downshiftedTone << ", "
              << constantDownshiftedTone << '\n';
    return false;
  }
  return true;
}

bool testHarmonicEffects()
{
  constexpr int fftSize = 256;
  constexpr int sampleCount = fftSize * 8;
  constexpr int latency = fftSize - 1;
  constexpr int measurementStart = latency + 512;
  constexpr double sampleRate = 44100.0;
  constexpr float pi = 3.14159265358979323846f;
  const auto render = [=](int effectType, float value, std::vector<float>& output) {
    DtBlkFxEngine engine;
    if (!engine.prepare(0, sampleRate))
      return false;

    std::array<DtBlkFxEngine::EffectParameters, 8> effects{};
    effects[0] = {effectType,
                  frequencyParameterForBin(4, fftSize, sampleRate),
                  frequencyParameterForBin(32, fftSize, sampleRate),
                  0.6f,
                  value};
    engine.configureEffects(effects, 0.0f, false);
    engine.configureScheduler(0.0f, 0);

    output.resize(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
      output[static_cast<size_t>(sample)] = 0.3f * std::sin(2.0f * pi * 4 * sample / fftSize) +
                                            0.2f * std::sin(2.0f * pi * 8 * sample / fftSize) +
                                            0.1f * std::sin(2.0f * pi * 12 * sample / fftSize) +
                                            0.05f * std::sin(2.0f * pi * 16 * sample / fftSize);
    }

    float* channel = output.data();
    return engine.process(&channel, 1, sampleCount);
  };

  std::vector<float> harmonicFiltered;
  std::vector<float> autoHarm;
  std::vector<float> harmonicShifted;
  std::vector<float> harmonicRepitched;
  if (!render(13, 0.1f, harmonicFiltered) || !render(14, 0.1f, autoHarm) ||
      !render(11, 0.5833333f, harmonicShifted) || !render(12, 0.552f, harmonicRepitched)) {
    std::cerr << "Harmonic effect processing failed\n";
    return false;
  }

  const auto filteredFundamental =
      measureToneAmplitude(harmonicFiltered, 4, measurementStart, latency);
  const auto filteredSecond = measureToneAmplitude(harmonicFiltered, 8, measurementStart, latency);
  const auto autoFundamental = measureToneAmplitude(autoHarm, 4, measurementStart, latency);
  const auto shiftedSource = measureToneAmplitude(harmonicShifted, 8, measurementStart, latency);
  const auto shiftedTarget = measureToneAmplitude(harmonicShifted, 11, measurementStart, latency);
  const auto repitchedSource =
      measureToneAmplitude(harmonicRepitched, 8, measurementStart, latency);
  const auto repitchedTarget =
      measureToneAmplitude(harmonicRepitched, 11, measurementStart, latency);
  if (filteredFundamental < 0.2f || filteredSecond < 0.12f || autoFundamental < 0.2f ||
      shiftedSource > 0.1f || shiftedTarget < 0.1f || repitchedSource > 0.1f ||
      repitchedTarget < 0.1f) {
    std::cerr << "Harmonic filtering/tracking/shift/re-pitch output mismatch: "
              << filteredFundamental << ", " << filteredSecond << ", " << autoFundamental << ", "
              << shiftedSource << ", " << shiftedTarget << ", " << repitchedSource << ", "
              << repitchedTarget << '\n';
    return false;
  }
  return true;
}

bool testSweepEffects()
{
  std::vector<float> bypass;
  if (!renderTestEffect(9, 0.5f, 0.6f, bypass)) {
    std::cerr << "Sweep bypass render failed\n";
    return false;
  }

  constexpr auto firstMeasuredSample = 255 + 16;
  std::array<std::array<std::vector<float>, 2>, 5> sweepRenders;
  for (int effectType = 15; effectType <= 19; ++effectType) {
    for (const auto value : {0.1f, 0.6f}) {
      std::vector<float> swept;
      std::vector<float> repeatedSweep;
      if (!renderTestEffect(effectType, value, 0.6f, swept) ||
          !renderTestEffect(effectType, value, 0.6f, repeatedSweep)) {
        std::cerr << "Sweep effect " << effectType << " processing failed\n";
        return false;
      }

      const auto modeIndex = value < 0.5f ? 0u : 1u;
      sweepRenders[static_cast<size_t>(effectType - 15)][modeIndex] = swept;
      auto changed = false;
      for (size_t sample = firstMeasuredSample; sample < swept.size(); ++sample) {
        if (!std::isfinite(swept[sample]) || swept[sample] != repeatedSweep[sample]) {
          std::cerr << "Sweep effect " << effectType << " was non-finite or non-deterministic\n";
          return false;
        }
        changed |= std::abs(swept[sample] - bypass[sample]) > 0.001f;
      }
      if (!changed) {
        std::cerr << "Sweep effect " << effectType << " did not alter the selected harmonics\n";
        return false;
      }
    }
  }

  for (size_t sweep = 0; sweep < sweepRenders.size(); ++sweep) {
    auto modesDiffer = false;
    for (size_t sample = firstMeasuredSample; sample < bypass.size(); ++sample) {
      modesDiffer |= std::abs(sweepRenders[sweep][0][sample] -
                              sweepRenders[sweep][1][sample]) > 0.001f;
    }
    if (!modesDiffer) {
      std::cerr << "Sweep effect " << sweep + 15 << " did not distinguish scale and copy modes\n";
      return false;
    }

    if (sweep > 0) {
      auto profileDiffers = false;
      for (size_t sample = firstMeasuredSample; sample < bypass.size(); ++sample) {
        profileDiffers |= std::abs(sweepRenders[0][0][sample] -
                                   sweepRenders[sweep][0][sample]) > 0.001f;
      }
      if (!profileDiffers) {
        std::cerr << "Sweep effect " << sweep + 15 << " did not use a distinct coefficient profile\n";
        return false;
      }
    }
  }
  return true;
}
} // namespace

int main()
{
  DtBlkFxEngine engine;
  if (engine.forward(nullptr, nullptr, 256)) {
    std::cerr << "Unprepared engine accepted an FFT\n";
    return 1;
  }
  if (!engine.prepare()) {
    std::cerr << "Failed to prepare FFTW plans\n";
    return 1;
  }

  bool passed = true;
  constexpr float tolerance = 0.0001f;
  constexpr float pi = 3.14159265358979323846f;

  for (int planIndex = 0; planIndex < DtBlkFxEngine::fftSizeCount; ++planIndex) {
    const auto fftSize = DtBlkFxEngine::fftSizeAt(planIndex);
    const auto binCount = DtBlkFxEngine::fftBinCount(fftSize);
    std::vector<float> input(static_cast<size_t>(fftSize), 0.0f);
    std::vector<float> output(static_cast<size_t>(fftSize), 0.0f);
    std::vector<DtBlkFxEngine::ComplexBin> bins(static_cast<size_t>(binCount));

    input[0] = 1.0f;
    passed &=
        check(engine.forward(input.data(), bins.data(), fftSize), fftSize, "impulse FFT failed");
    for (const auto& bin : bins)
      passed &= check(std::abs(bin.real - 1.0f) < tolerance && std::abs(bin.imaginary) < tolerance,
                      fftSize,
                      "impulse spectrum is incorrect");
    passed &= check(
        engine.inverse(bins.data(), output.data(), fftSize), fftSize, "impulse inverse FFT failed");
    passed &= check(maxAbsDifference(output, input) < tolerance,
                    fftSize,
                    "impulse round trip differs from input");

    constexpr int testBin = 7;
    for (int sample = 0; sample < fftSize; ++sample)
      input[static_cast<size_t>(sample)] = std::sin(2.0f * pi * testBin * sample / fftSize);
    passed &= check(engine.forward(input.data(), bins.data(), fftSize), fftSize, "sine FFT failed");
    passed &= check(std::abs(bins[static_cast<size_t>(testBin)].real) < 0.1f &&
                        std::abs(bins[static_cast<size_t>(testBin)].imaginary +
                                 static_cast<float>(fftSize) * 0.5f) < 0.1f,
                    fftSize,
                    "sine spectrum peak is incorrect");
    passed &= check(
        engine.inverse(bins.data(), output.data(), fftSize), fftSize, "sine inverse FFT failed");
    passed &= check(
        maxAbsDifference(output, input) < tolerance, fftSize, "sine round trip differs from input");
  }

  passed &= check(!engine.forward(nullptr, nullptr, 256), 256, "null buffers should be rejected");
  passed &= check(!engine.forward(nullptr, nullptr, 1000), 1000, "unsupported FFT size accepted");
  passed &= testStartupOutputTiming();
  passed &= testDelayTransitionsRemainContinuous();
  passed &= testScheduler(0, 0.0f, 0);
  passed &= testScheduler(8, 0.72f, 37);
  passed &= testScheduler(0, 1.0f, 0);
  passed &= testSchedulerReset();
  passed &= testBeatSynchronizedHop();
  passed &= testSchedulerFftSizeTransition();
  passed &= testParameterDrivenDelayChangeRemainsContinuous();
  passed &= testLegacyParameterInterpolation();
  passed &= testParameterSynchronizedFrameStart();
  passed &= testFilterEffect(1.0f, 0.5f, false, 8);
  passed &= testFilterEffect(0.05f, 0.0f, false, 8);
  passed &= testFilterEffect(1.0f, 0.5f, true, 8);
  passed &= testFrequencyMasks();
  passed &= testContrastEffect();
  passed &= testSmearEffect();
  passed &= testThresholdEffect();
  passed &= testClipEffect();
  passed &= testFrequencyMappingEffects();
  passed &= testHarmonicEffects();
  passed &= testSweepEffects();
  passed &= testStereoEffects();
  return passed ? 0 : 1;
}
