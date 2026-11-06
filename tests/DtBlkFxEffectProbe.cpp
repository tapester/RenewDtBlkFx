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

// Effect probe, not a pass/fail test: plays the same audio through one effect of the legacy
// 64-bit VST2 plugin and of the new engine and prints where the energy goes, so that a difference
// can be understood instead of only measured. Optionally writes WAV files for listening.
//
// Usage: DtBlkFxEffectProbe.exe <path\to\legacy\DtBlkFx.dll> --effect N [--value V] [--amp A]
//                               [--input tones|noise] [--channel 0|1] [--wav folder]
//
// The legacy plugin is driven through a hand-written description of the VST2 interface (no SDK).

#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

constexpr double kRate = 44100.0;
constexpr double kBpm = 120.0;
constexpr double kSamplesPerBeat = kRate * 60.0 / kBpm;
constexpr int kBlock = 256;
constexpr int kTotal = 264600;
constexpr int kLatency = 4095;
constexpr int kTargetDelay = 22050;
constexpr int kAnalysisFft = 4096;
constexpr int kFrames = 8;
constexpr int kAnalysisAt = 150000;
constexpr double pi = 3.14159265358979323846;

const std::array<const char*, 31> kEffectNames{
    "Filter",      "Contrast",    "Smear",           "Threshold",        "Clip",         "Resize",
    "Resample",    "Shift",       "ConstShift",      "None",             "None",         "HarmShift",
    "HarmRepitch", "HarmFilt",    "AutoHarm",        "Triangles",        "Squares",      "Saws",
    "Pointy",      "Sweep",       "HarmMask",        "AutoHarmMask",     "AutoSubharm1Mask", "AutoSubharm2Mask",
    "AutoSubharm3Mask", "ThreshMask", "Vocode",      "HarmMatch L>R",    "HarmMatch R>L", "CrossMix",
    "WarpMix"};

using Params = std::array<float, 44>;

float fftLenParam(int planIndex)
{
  return static_cast<float>((planIndex + 2) * 4) / 255.0f;
}
float delayParamBeats(double beats)
{
  return static_cast<float>(beats / (0.5 * 255.0 / 16.0) / 2.0);
}
float delayParamMs(double ms)
{
  return static_cast<float>(0.5 + ms / 6000.0 * 0.5);
}
const double kNewDelayMs = static_cast<double>(kTargetDelay - kLatency) * 1000.0 / kRate;

struct Audio {
  std::vector<float> left;
  std::vector<float> right;
};

struct Rng {
  std::uint32_t state;
  explicit Rng(std::uint32_t seed)
      : state(seed)
  {
  }
  float next()
  {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 16777216.0f;
  }
};

Audio makeInput(bool tones)
{
  Audio audio;
  audio.left.resize(kTotal);
  audio.right.resize(kTotal);
  if (tones) {
    // Harmonic tones with fundamentals exactly on bins 20 and 30 of a 4096-point FFT.
    const double f0 = 20.0 * kRate / kAnalysisFft;
    const double f1 = 30.0 * kRate / kAnalysisFft;
    for (int i = 0; i < kTotal; ++i) {
      double left = 0.0;
      double right = 0.0;
      for (int harmonic = 1; harmonic <= 20; ++harmonic) {
        left += (0.15 / harmonic) * std::sin(2.0 * pi * f0 * harmonic * i / kRate);
        right += (0.15 / harmonic) * std::sin(2.0 * pi * f1 * harmonic * i / kRate);
      }
      audio.left[static_cast<size_t>(i)] = static_cast<float>(left);
      audio.right[static_cast<size_t>(i)] = static_cast<float>(right);
    }
  }
  else {
    Rng a(11u);
    Rng b(22u);
    for (int i = 0; i < kTotal; ++i) {
      audio.left[static_cast<size_t>(i)] = (a.next() - 0.5f) * 0.5f;
      audio.right[static_cast<size_t>(i)] = (b.next() - 0.5f) * 0.5f;
    }
  }
  return audio;
}

struct Backend {
  virtual ~Backend() = default;
  virtual bool restart(const Params& params) = 0;
  virtual bool process(float* left, float* right, int count) = 0;
};

struct NewEngineBackend : Backend {
  DtBlkFxEngine engine;
  bool restart(const Params& params) override
  {
    if (!engine.prepare(8, kRate))
      return false;
    engine.setFixedLatency(kLatency);
    engine.reset();
    return engine.configureLegacyParameters(params, kSamplesPerBeat, 0.0, false);
  }
  bool process(float* left, float* right, int count) override
  {
    std::array<float*, 2> channels{left, right};
    return engine.process(channels.data(), 2, count);
  }
};

#pragma pack(push, 8)
struct AEffect;
using AudioMasterCallback = std::intptr_t (*)(AEffect*, std::int32_t, std::int32_t, std::intptr_t, void*, float);
using DispatcherProc = std::intptr_t (*)(AEffect*, std::int32_t, std::int32_t, std::intptr_t, void*, float);
using ProcessProc = void (*)(AEffect*, float**, float**, std::int32_t);
using SetParameterProc = void (*)(AEffect*, std::int32_t, float);
using GetParameterProc = float (*)(AEffect*, std::int32_t);

struct AEffect {
  std::int32_t magic;
  DispatcherProc dispatcher;
  ProcessProc processDeprecated;
  SetParameterProc setParameter;
  GetParameterProc getParameter;
  std::int32_t numPrograms;
  std::int32_t numParams;
  std::int32_t numInputs;
  std::int32_t numOutputs;
  std::int32_t flags;
  std::intptr_t resvd1;
  std::intptr_t resvd2;
  std::int32_t initialDelay;
  std::int32_t realQualities;
  std::int32_t offQualities;
  float ioRatio;
  void* object;
  void* user;
  std::int32_t uniqueID;
  std::int32_t version;
  ProcessProc processReplacing;
  void* processDoubleReplacing;
  char future[56];
};

struct VstTimeInfo {
  double samplePos;
  double sampleRate;
  double nanoSeconds;
  double ppqPos;
  double tempo;
  double barStartPos;
  double cycleStartPos;
  double cycleEndPos;
  std::int32_t timeSigNumerator;
  std::int32_t timeSigDenominator;
  std::int32_t smpteOffset;
  std::int32_t smpteFrameRate;
  std::int32_t samplesToNextClock;
  std::int32_t flags;
};
#pragma pack(pop)

constexpr std::int32_t kEffectMagic = 0x56737450;
constexpr std::int32_t kAudioMasterVersion = 1;
constexpr std::int32_t kAudioMasterGetTime = 7;
constexpr std::int32_t kAudioMasterGetSampleRate = 16;
constexpr std::int32_t kAudioMasterGetBlockSize = 17;
constexpr std::int32_t kEffOpen = 0;
constexpr std::int32_t kEffClose = 1;
constexpr std::int32_t kEffSetSampleRate = 10;
constexpr std::int32_t kEffSetBlockSize = 11;
constexpr std::int32_t kEffMainsChanged = 12;
constexpr std::int32_t kVstPpqPosValid = 1 << 9;
constexpr std::int32_t kVstTempoValid = 1 << 10;

VstTimeInfo g_timeInfo{};
double g_hostPosition = 0.0;

std::intptr_t hostCallback(AEffect*, std::int32_t opcode, std::int32_t, std::intptr_t, void*, float)
{
  switch (opcode) {
    case kAudioMasterVersion:
      return 2400;
    case kAudioMasterGetTime:
      g_timeInfo = VstTimeInfo{};
      g_timeInfo.samplePos = g_hostPosition;
      g_timeInfo.sampleRate = kRate;
      g_timeInfo.tempo = kBpm;
      g_timeInfo.ppqPos = g_hostPosition / kSamplesPerBeat;
      g_timeInfo.timeSigNumerator = 4;
      g_timeInfo.timeSigDenominator = 4;
      g_timeInfo.flags = kVstPpqPosValid | kVstTempoValid;
      return reinterpret_cast<std::intptr_t>(&g_timeInfo);
    case kAudioMasterGetSampleRate:
      return static_cast<std::intptr_t>(kRate);
    case kAudioMasterGetBlockSize:
      return kBlock;
    default:
      return 0;
  }
}

#ifdef _WIN32
struct LegacyBackend : Backend {
  HMODULE module = nullptr;
  AEffect* effect = nullptr;
  bool opened = false;
  std::string error;
  std::vector<float> outLeft;
  std::vector<float> outRight;

  ~LegacyBackend() override
  {
    if (effect != nullptr && opened) {
      effect->dispatcher(effect, kEffMainsChanged, 0, 0, nullptr, 0.0f);
      effect->dispatcher(effect, kEffClose, 0, 0, nullptr, 0.0f);
    }
    if (module != nullptr)
      FreeLibrary(module);
  }

  bool load(const std::string& path)
  {
    static_assert(sizeof(void*) == 8, "this harness describes the 64-bit VST2 layout");
    static_assert(sizeof(AEffect) == 192, "AEffect layout does not match the 64-bit VST2 structure");
    static_assert(sizeof(VstTimeInfo) == 88, "VstTimeInfo layout does not match the VST2 structure");
    module = LoadLibraryA(path.c_str());
    if (module == nullptr) {
      error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    using MainProc = AEffect* (*)(AudioMasterCallback);
    auto mainProc = reinterpret_cast<MainProc>(GetProcAddress(module, "VSTPluginMain"));
    if (mainProc == nullptr)
      mainProc = reinterpret_cast<MainProc>(GetProcAddress(module, "main"));
    if (mainProc == nullptr) {
      error = "no VSTPluginMain export";
      return false;
    }
    effect = mainProc(reinterpret_cast<AudioMasterCallback>(hostCallback));
    if (effect == nullptr) {
      error = "the plugin refused to start (it needs its dtblkfx folder next to the DLL)";
      return false;
    }
    if (effect->magic != kEffectMagic || effect->numParams < 44 || effect->numInputs != 2 ||
        effect->numOutputs != 2 || effect->processReplacing == nullptr) {
      error = "this is not the stereo build with 44+ parameters";
      return false;
    }
    effect->dispatcher(effect, kEffOpen, 0, 0, nullptr, 0.0f);
    opened = true;
    effect->dispatcher(effect, kEffSetSampleRate, 0, 0, nullptr, static_cast<float>(kRate));
    effect->dispatcher(effect, kEffSetBlockSize, 0, kBlock, nullptr, 0.0f);
    return true;
  }

  bool restart(const Params& params) override
  {
    effect->dispatcher(effect, kEffMainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, kEffMainsChanged, 0, 1, nullptr, 0.0f);
    g_hostPosition = 0.0;
    for (int i = 0; i < 44; ++i)
      effect->setParameter(effect, i, params[static_cast<size_t>(i)]);
    return true;
  }
  bool process(float* left, float* right, int count) override
  {
    outLeft.resize(static_cast<size_t>(count));
    outRight.resize(static_cast<size_t>(count));
    std::array<float*, 2> in{left, right};
    std::array<float*, 2> out{outLeft.data(), outRight.data()};
    effect->processReplacing(effect, in.data(), out.data(), count);
    std::memcpy(left, outLeft.data(), sizeof(float) * static_cast<size_t>(count));
    std::memcpy(right, outRight.data(), sizeof(float) * static_cast<size_t>(count));
    g_hostPosition += count;
    return true;
  }
};
#endif

bool render(Backend& backend, const Params& params, const Audio& input, Audio& output)
{
  output = input;
  if (!backend.restart(params))
    return false;
  for (int position = 0; position < kTotal; position += kBlock) {
    const auto count = std::min(kBlock, kTotal - position);
    if (!backend.process(output.left.data() + position, output.right.data() + position, count))
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------------------

// Averaged power spectrum of 8 Hann-windowed 4096-point frames, in dB relative to a full-scale sine.
struct Spectrum {
  std::vector<double> power; // per bin, linear (amplitude squared of a unit sine = 1)
};

Spectrum analyse(const std::vector<float>& signal)
{
  Spectrum result;
  result.power.assign(kAnalysisFft / 2 + 1, 0.0);

  DtBlkFxEngine engine;
  if (!engine.prepare(16, kRate))
    return result;
  std::vector<float> window(kAnalysisFft);
  double windowSum = 0.0;
  for (int i = 0; i < kAnalysisFft; ++i) {
    window[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / kAnalysisFft));
    windowSum += window[static_cast<size_t>(i)];
  }
  std::vector<float> frame(kAnalysisFft);
  std::vector<DtBlkFxEngine::ComplexBin> bins(kAnalysisFft / 2 + 1);
  for (int f = 0; f < kFrames; ++f) {
    const auto start = kAnalysisAt + f * (kAnalysisFft / 2);
    for (int i = 0; i < kAnalysisFft; ++i)
      frame[static_cast<size_t>(i)] = signal[static_cast<size_t>(start + i)] * window[static_cast<size_t>(i)];
    if (!engine.forward(frame.data(), bins.data(), kAnalysisFft))
      return result;
    for (int bin = 0; bin <= kAnalysisFft / 2; ++bin) {
      const auto& v = bins[static_cast<size_t>(bin)];
      // Amplitude of a sine in this bin is 2 * |X| / sum(window).
      const double amplitude = 2.0 * std::sqrt(static_cast<double>(v.real) * v.real + static_cast<double>(v.imaginary) * v.imaginary) / windowSum;
      result.power[static_cast<size_t>(bin)] += amplitude * amplitude / kFrames;
    }
  }
  return result;
}

double toDb(double power)
{
  return 10.0 * std::log10(std::max(power, 1.0e-18));
}

struct Peak {
  double bin;
  double db;
};

std::vector<Peak> topPeaks(const Spectrum& spectrum, int count)
{
  std::vector<Peak> peaks;
  const auto n = static_cast<int>(spectrum.power.size());
  for (int bin = 2; bin + 2 < n; ++bin) {
    const auto p = spectrum.power[static_cast<size_t>(bin)];
    if (p > spectrum.power[static_cast<size_t>(bin - 1)] && p >= spectrum.power[static_cast<size_t>(bin + 1)] &&
        p > 1.0e-12) {
      // Parabolic interpolation on the dB values for a fractional bin position.
      const auto a = toDb(spectrum.power[static_cast<size_t>(bin - 1)]);
      const auto b = toDb(p);
      const auto c = toDb(spectrum.power[static_cast<size_t>(bin + 1)]);
      const auto denominator = a - 2.0 * b + c;
      const auto fraction = denominator != 0.0 ? 0.5 * (a - c) / denominator : 0.0;
      peaks.push_back({bin + std::max(-0.5, std::min(0.5, fraction)), b});
    }
  }
  std::sort(peaks.begin(), peaks.end(), [](const Peak& x, const Peak& y) { return x.db > y.db; });
  if (static_cast<int>(peaks.size()) > count)
    peaks.resize(static_cast<size_t>(count));
  return peaks;
}

double totalPower(const std::vector<float>& signal)
{
  double sum = 0.0;
  int count = 0;
  for (int i = kAnalysisAt; i < kAnalysisAt + kFrames * (kAnalysisFft / 2) + kAnalysisFft && i < kTotal; ++i) {
    sum += static_cast<double>(signal[static_cast<size_t>(i)]) * signal[static_cast<size_t>(i)];
    ++count;
  }
  return count > 0 ? sum / count : 0.0;
}

std::string fixed(double value, int digits)
{
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

void writeWav(const std::string& path, const Audio& audio)
{
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return;
  const std::uint32_t frames = static_cast<std::uint32_t>(audio.left.size());
  const std::uint32_t dataBytes = frames * 2u * 4u;
  const auto put32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
  const auto put16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
  file.write("RIFF", 4);
  put32(36u + dataBytes);
  file.write("WAVEfmt ", 8);
  put32(16u);
  put16(3); // IEEE float
  put16(2);
  put32(static_cast<std::uint32_t>(kRate));
  put32(static_cast<std::uint32_t>(kRate) * 2u * 4u);
  put16(8);
  put16(32);
  file.write("data", 4);
  put32(dataBytes);
  for (std::uint32_t i = 0; i < frames; ++i) {
    file.write(reinterpret_cast<const char*>(&audio.left[i]), 4);
    file.write(reinterpret_cast<const char*>(&audio.right[i]), 4);
  }
}

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
  (void)argc;
  (void)argv;
  std::cout << "This probe needs the 64-bit Windows VST2 plugin.\n";
  return 0;
#else
  std::string dll;
  int effect = -1;
  float value = 0.5f;
  float amplitude = 0.6f;
  bool tones = true;
  int channel = 0;
  std::string wavFolder;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--effect" && i + 1 < argc)
      effect = std::atoi(argv[++i]);
    else if (argument == "--value" && i + 1 < argc)
      value = static_cast<float>(std::atof(argv[++i]));
    else if (argument == "--amp" && i + 1 < argc)
      amplitude = static_cast<float>(std::atof(argv[++i]));
    else if (argument == "--input" && i + 1 < argc)
      tones = std::string(argv[++i]) != "noise";
    else if (argument == "--channel" && i + 1 < argc)
      channel = std::atoi(argv[++i]) == 1 ? 1 : 0;
    else if (argument == "--wav" && i + 1 < argc)
      wavFolder = argv[++i];
    else if (dll.empty())
      dll = argument;
  }
  if (dll.empty() || effect < 0 || effect > 30) {
    std::cout << "Usage: DtBlkFxEffectProbe.exe <path\\to\\legacy\\DtBlkFx.dll> --effect N [--value V] [--amp A] "
                 "[--input tones|noise] [--channel 0|1] [--wav folder]\n";
    return 0;
  }

  LegacyBackend legacy;
  if (!legacy.load(dll)) {
    std::cout << "legacy plugin could not be used: " << legacy.error << "\n";
    return 0;
  }
  NewEngineBackend fresh;

  // One active slot with the effect; the mask effects get a filter after them to act on.
  Params base{};
  base[0] = 1.0f;
  base[2] = fftLenParam(16);
  base[3] = 0.175f;
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    base[offset] = 0.0f;
    base[offset + 1] = 1.0f;
    base[offset + 2] = 0.6f;
    base[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(9);
    base[offset + 4] = 0.5f;
  }
  base[4] = 0.2f;
  base[5] = 0.8f;
  base[6] = amplitude;
  base[7] = DtBlkFxLegacy::effectTypeToLegacyValue(effect);
  base[8] = value;
  if (effect >= 20 && effect <= 25) {
    base[9] = 0.0f;
    base[10] = 1.0f;
    base[11] = 0.3f;
    base[12] = DtBlkFxLegacy::effectTypeToLegacyValue(0);
    base[13] = 0.5f;
  }

  auto forLegacy = base;
  forLegacy[1] = delayParamBeats(1.0);
  auto forNew = base;
  forNew[1] = delayParamMs(kNewDelayMs);

  const auto input = makeInput(tones);
  Audio legacyOut;
  Audio freshOut;
  if (!render(legacy, forLegacy, input, legacyOut) || !render(fresh, forNew, input, freshOut)) {
    std::cout << "rendering failed\n";
    return 0;
  }

  // The new engine's output is delayed by the same total as the legacy one, so the same analysis
  // position applies to the three signals (the input is analysed at the position it came from).
  const auto& inSignal = channel == 0 ? input.left : input.right;
  const auto& legSignal = channel == 0 ? legacyOut.left : legacyOut.right;
  const auto& newSignal = channel == 0 ? freshOut.left : freshOut.right;

  std::cout << "Effect " << effect << " (" << kEffectNames[static_cast<size_t>(effect)] << "), value " << value
            << ", amplitude " << amplitude << ", input " << (tones ? "tones" : "noise") << ", channel " << channel
            << ", FFT 4096\n";
  std::cout << "Mean power per sample (dB re full scale): input " << fixed(toDb(totalPower(inSignal)), 2) << ", legacy "
            << fixed(toDb(totalPower(legSignal)), 2) << ", new " << fixed(toDb(totalPower(newSignal)), 2) << "\n";

  const auto inSpectrum = analyse(inSignal);
  const auto legSpectrum = analyse(legSignal);
  const auto newSpectrum = analyse(newSignal);

  if (tones) {
    std::cout << "\nStrongest spectral peaks (bin number with fraction; dB relative to a full-scale sine).\n";
    std::cout << "Input fundamentals sit exactly on bin " << (channel == 0 ? 20 : 30) << ".\n";
    const auto printPeaks = [&](const char* name, const Spectrum& spectrum) {
      std::cout << "\n" << name << ":\n  ";
      int printed = 0;
      for (const auto& peak : topPeaks(spectrum, 16)) {
        std::cout << std::setw(8) << fixed(peak.bin, 2) << " @" << std::setw(7) << fixed(peak.db, 1) << "   ";
        if (++printed % 4 == 0)
          std::cout << "\n  ";
      }
      std::cout << "\n";
    };
    printPeaks("input", inSpectrum);
    printPeaks("legacy output", legSpectrum);
    printPeaks("new output", newSpectrum);
  }
  else {
    std::cout << "\nOctave bands (dB relative to a full-scale sine, summed power).\n";
    std::cout << std::setw(14) << "band" << std::setw(10) << "input" << std::setw(10) << "legacy" << std::setw(10) << "new"
              << std::setw(12) << "new-legacy" << "\n";
    double low = 62.5;
    while (low < 16000.0) {
      const auto high = low * 2.0;
      double a = 0.0;
      double b = 0.0;
      double c = 0.0;
      for (int bin = 1; bin <= kAnalysisFft / 2; ++bin) {
        const auto frequency = bin * kRate / kAnalysisFft;
        if (frequency >= low && frequency < high) {
          a += inSpectrum.power[static_cast<size_t>(bin)];
          b += legSpectrum.power[static_cast<size_t>(bin)];
          c += newSpectrum.power[static_cast<size_t>(bin)];
        }
      }
      std::cout << std::setw(7) << static_cast<int>(low) << "-" << std::setw(5) << static_cast<int>(high) << std::setw(10)
                << fixed(toDb(a), 1) << std::setw(10) << fixed(toDb(b), 1) << std::setw(10) << fixed(toDb(c), 1)
                << std::setw(12) << fixed(toDb(c) - toDb(b), 1) << "\n";
      low = high;
    }
  }

  if (!wavFolder.empty()) {
    const auto stem = wavFolder + "\\effect" + std::to_string(effect) + "_v" + fixed(value, 2) + "_" + (tones ? "tones" : "noise");
    writeWav(stem + "_input.wav", input);
    writeWav(stem + "_legacy.wav", legacyOut);
    writeWav(stem + "_new.wav", freshOut);
    std::cout << "\nWAV files written: " << stem << "_{input,legacy,new}.wav (44.1 kHz, 32-bit float, stereo).\n"
              << "The legacy and new files are aligned to each other; the input is not delayed.\n";
  }
  return 0;
#endif
}
