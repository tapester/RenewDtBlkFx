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

// Effect comparison, not a pass/fail test: renders the same audio through the legacy 64-bit VST2
// plugin and through the new engine and compares level, waveform correlation and spectrum.
//
// Usage: DtBlkFxEffectComparison.exe <path\to\legacy\DtBlkFx.dll> [--full] [--effect N]
//                                    [--csv file] [--no-presets]
//
// Tolerance: level within 1 dB and correlation of at least 0.9. The legacy plugin is driven through
// a hand-written description of the VST2 interface (no SDK). It always exits with 0.

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
#include <map>
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
constexpr double kSamplesPerBeat = kRate * 60.0 / kBpm; // 22050
constexpr int kBlock = 256;
constexpr int kTotal = 264600;    // 6 seconds
constexpr int kLatency = 4095;    // fixed latency budget of the new engine (FFT sizes up to 4096)
constexpr int kTargetDelay = 22050; // total delay of both plugins: one beat at 120 BPM
constexpr int kAnalysisStart = 38434;
constexpr int kMaxLag = 128;
constexpr int kCoarseStart = 100000;
constexpr int kCoarseLength = 20000;
constexpr int kBands = 20;
constexpr int kAnalysisFft = 4096;
constexpr double kLevelToleranceDb = 1.0;
constexpr double kCorrelationMinimum = 0.9;
constexpr double kSpectrumMeanMaximumDb = 1.5;
constexpr double kSpectrumPeakMaximumDb = 4.0;
constexpr double pi = 3.14159265358979323846;

const std::array<const char*, 31> kEffectNames{
    "Filter",      "Contrast",    "Smear",           "Threshold",        "Clip",         "Resize",
    "Resample",    "Shift",       "ConstShift",      "None",             "None",         "HarmShift",
    "HarmRepitch", "HarmFilt",    "AutoHarm",        "Triangles",        "Squares",      "Saws",
    "Pointy",      "Sweep",       "HarmMask",        "AutoHarmMask",     "AutoSubharm1Mask", "AutoSubharm2Mask",
    "AutoSubharm3Mask", "ThreshMask", "Vocode",      "HarmMatch L>R",    "HarmMatch R>L", "CrossMix",
    "WarpMix"};

// ---------------------------------------------------------------------------------------------
// Parameters (the legacy order: 4 global values, then 8 effect slots of 5 values each)
// ---------------------------------------------------------------------------------------------

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

// Delay of the new engine that adds up to the target together with its fixed latency.
const double kNewDelayMs = static_cast<double>(kTargetDelay - kLatency) * 1000.0 / kRate;

struct Slot {
  float freqA = 0.0f;
  float freqB = 1.0f;
  float amplitude = 0.6f;
  int type = 9; // None
  float value = 0.5f;
};

// plan 16 is the 4096-point FFT, plan 8 the 1024-point FFT. Overlap 0.175 is the plugin default
// (0.35 in the plugin maps to 0.175 in the legacy value).
Params makeBase(int plan, float overlap, const std::array<Slot, 8>& slots)
{
  Params p{};
  p[0] = 1.0f; // fully wet, no power match
  p[1] = 0.0f; // set per backend
  p[2] = fftLenParam(plan);
  p[3] = overlap;
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = slots[slot].freqA;
    p[offset + 1] = slots[slot].freqB;
    p[offset + 2] = slots[slot].amplitude;
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(slots[slot].type);
    p[offset + 4] = slots[slot].value;
  }
  return p;
}

Params forLegacy(Params p)
{
  p[1] = delayParamBeats(1.0);
  return p;
}
Params forNew(Params p)
{
  p[1] = delayParamMs(kNewDelayMs);
  return p;
}

// ---------------------------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------------------------

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
    return static_cast<float>(state >> 8) / 16777216.0f; // 0..1
  }
};

Audio makeNoise()
{
  Audio audio;
  audio.left.resize(kTotal);
  audio.right.resize(kTotal);
  Rng a(11u);
  Rng b(22u);
  for (int i = 0; i < kTotal; ++i) {
    audio.left[static_cast<size_t>(i)] = (a.next() - 0.5f) * 0.5f;
    audio.right[static_cast<size_t>(i)] = (b.next() - 0.5f) * 0.5f;
  }
  return audio;
}

// Harmonic tones whose fundamentals sit exactly on bins 20 and 30 of a 4096-point FFT.
Audio makeTones()
{
  Audio audio;
  audio.left.resize(kTotal);
  audio.right.resize(kTotal);
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
  return audio;
}

// ---------------------------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------------------------

struct Backend {
  virtual ~Backend() = default;
  // Brings the backend to a clean state with the given parameters.
  virtual bool restart(const Params& params) = 0;
  virtual bool process(float* left, float* right, int count) = 0;
};

struct NewEngineBackend : Backend {
  DtBlkFxEngine engine;

  bool restart(const Params& params) override
  {
    // prepare() releases everything first, so every run starts from a fresh engine.
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

// Hand-written description of the VST2 plugin interface (64-bit Windows). Only what is needed to
// load a plugin, set parameters and process audio.
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

constexpr std::int32_t kEffectMagic = 0x56737450; // 'VstP'
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

    // The plugin finds its FFTW library and images relative to its own folder.
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
    if (effect->magic != kEffectMagic) {
      error = "unexpected plugin signature " + std::to_string(effect->magic);
      return false;
    }
    if (effect->numParams < 44 || effect->numInputs != 2 || effect->numOutputs != 2) {
      error = "this is not the stereo build with 44+ parameters";
      return false;
    }
    if (effect->processReplacing == nullptr) {
      error = "no processReplacing";
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
    // Suspend, then resume: the plugin clears its buffers when it is suspended.
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
// Metrics
// ---------------------------------------------------------------------------------------------

// Band power of a signal: Hann-windowed 4096-point FFTs, 20 log-spaced bands from 60 Hz to 16 kHz.
struct Analyzer {
  DtBlkFxEngine engine;
  std::vector<float> window;
  std::vector<float> frame;
  std::vector<DtBlkFxEngine::ComplexBin> bins;
  std::vector<int> bandOfBin;
  bool ok = false;

  Analyzer()
  {
    ok = engine.prepare(16, kRate); // plan 16 is the 4096-point FFT
    window.resize(kAnalysisFft);
    frame.resize(kAnalysisFft);
    bins.resize(kAnalysisFft / 2 + 1);
    bandOfBin.assign(kAnalysisFft / 2 + 1, -1);
    for (int i = 0; i < kAnalysisFft; ++i)
      window[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / kAnalysisFft));
    for (int bin = 1; bin <= kAnalysisFft / 2; ++bin) {
      const auto frequency = bin * kRate / kAnalysisFft;
      if (frequency >= 60.0 && frequency < 16000.0) {
        auto band = static_cast<int>(std::log(frequency / 60.0) / std::log(16000.0 / 60.0) * kBands);
        bandOfBin[static_cast<size_t>(bin)] = std::min(band, kBands - 1);
      }
    }
  }

  bool spectrum(const std::vector<float>& signal, std::array<double, kBands>& power)
  {
    power.fill(0.0);
    const auto count = static_cast<int>(signal.size());
    for (int start = kAnalysisStart; start + kAnalysisFft <= count; start += kAnalysisFft / 2) {
      for (int i = 0; i < kAnalysisFft; ++i)
        frame[static_cast<size_t>(i)] = signal[static_cast<size_t>(start + i)] * window[static_cast<size_t>(i)];
      if (!engine.forward(frame.data(), bins.data(), kAnalysisFft))
        return false;
      for (int bin = 1; bin <= kAnalysisFft / 2; ++bin) {
        const auto band = bandOfBin[static_cast<size_t>(bin)];
        if (band >= 0) {
          const auto& value = bins[static_cast<size_t>(bin)];
          power[static_cast<size_t>(band)] +=
              static_cast<double>(value.real) * value.real + static_cast<double>(value.imaginary) * value.imaginary;
        }
      }
    }
    return true;
  }
};

struct Metrics {
  bool valid = false;
  bool silentBoth = false;
  bool oneSilent = false;
  double levelDb = 0.0;
  double corr = 0.0;
  double specMean = 0.0;
  double specMax = 0.0;
  int lag = 0;
};

// a is the legacy signal, b the new one.
Metrics compareChannel(const std::vector<float>& a, const std::vector<float>& b, Analyzer& analyzer)
{
  Metrics m;
  const auto n = static_cast<int>(a.size());
  double ea = 0.0;
  double eb = 0.0;
  for (int i = kAnalysisStart; i < n; ++i) {
    ea += static_cast<double>(a[static_cast<size_t>(i)]) * a[static_cast<size_t>(i)];
    eb += static_cast<double>(b[static_cast<size_t>(i)]) * b[static_cast<size_t>(i)];
  }
  const auto threshold = 1.0e-12 * static_cast<double>(n - kAnalysisStart);
  if (ea < threshold && eb < threshold) {
    m.silentBoth = true;
    return m;
  }
  if (ea < threshold || eb < threshold) {
    m.oneSilent = true;
    m.levelDb = eb < threshold ? -120.0 : 120.0;
    return m;
  }

  m.valid = true;
  m.levelDb = 10.0 * std::log10(eb / ea);

  // Best lag on a short window, then the correlation at that lag over the whole analysis range.
  double bestSum = -1.0e300;
  int bestLag = 0;
  for (int lag = -kMaxLag; lag <= kMaxLag; ++lag) {
    double sum = 0.0;
    for (int i = kCoarseStart; i < kCoarseStart + kCoarseLength; ++i)
      sum += static_cast<double>(a[static_cast<size_t>(i)]) * b[static_cast<size_t>(i + lag)];
    if (sum > bestSum) {
      bestSum = sum;
      bestLag = lag;
    }
  }
  m.lag = bestLag;
  double sab = 0.0;
  double saa = 0.0;
  double sbb = 0.0;
  for (int i = kAnalysisStart + kMaxLag; i < n - kMaxLag; ++i) {
    const double x = a[static_cast<size_t>(i)];
    const double y = b[static_cast<size_t>(i + bestLag)];
    sab += x * y;
    saa += x * x;
    sbb += y * y;
  }
  m.corr = (saa > 0.0 && sbb > 0.0) ? sab / std::sqrt(saa * sbb) : 0.0;

  // Spectrum: difference in dB per band, over the bands that carry energy.
  std::array<double, kBands> pa{};
  std::array<double, kBands> pb{};
  if (analyzer.spectrum(a, pa) && analyzer.spectrum(b, pb)) {
    double totalA = 0.0;
    double totalB = 0.0;
    for (int k = 0; k < kBands; ++k) {
      totalA += pa[static_cast<size_t>(k)];
      totalB += pb[static_cast<size_t>(k)];
    }
    const auto biggest = std::max(totalA, totalB);
    const auto floorPower = 1.0e-9 * biggest / kBands;
    double sum = 0.0;
    int used = 0;
    for (int k = 0; k < kBands; ++k) {
      const auto bandA = pa[static_cast<size_t>(k)];
      const auto bandB = pb[static_cast<size_t>(k)];
      if (std::max(bandA, bandB) < 1.0e-6 * biggest)
        continue; // this band carries no energy in either signal
      const auto difference = std::abs(10.0 * std::log10((bandB + floorPower) / (bandA + floorPower)));
      sum += difference;
      m.specMax = std::max(m.specMax, difference);
      ++used;
    }
    m.specMean = used > 0 ? sum / used : 0.0;
  }
  return m;
}

struct Combined {
  bool silentBoth = true;
  bool oneSilent = false;
  double levelDb = 0.0;
  double corr = 1.0;
  double specMean = 0.0;
  double specMax = 0.0;
  int lag = 0;
};

Combined combine(const Metrics& left, const Metrics& right)
{
  Combined c;
  bool haveLag = false;
  for (const auto* m : {&left, &right}) {
    if (m->oneSilent) {
      c.oneSilent = true;
      c.silentBoth = false;
      c.levelDb = m->levelDb;
    }
    if (!m->valid)
      continue;
    c.silentBoth = false;
    if (!c.oneSilent && std::abs(m->levelDb) >= std::abs(c.levelDb))
      c.levelDb = m->levelDb;
    c.corr = std::min(c.corr, m->corr);
    c.specMean = std::max(c.specMean, m->specMean);
    c.specMax = std::max(c.specMax, m->specMax);
    if (!haveLag) {
      c.lag = m->lag;
      haveLag = true;
    }
  }
  return c;
}

enum class Verdict { Inside, SpectrumOnly, Differs, Silent, RandomOk };

const char* verdictName(Verdict v)
{
  switch (v) {
    case Verdict::Inside: return "inside";
    case Verdict::SpectrumOnly: return "spectrum-only";
    case Verdict::Differs: return "DIFFERS";
    case Verdict::Silent: return "silent";
    case Verdict::RandomOk: return "ok(random)";
  }
  return "?";
}

Verdict judge(const Combined& c, bool random)
{
  if (c.silentBoth)
    return Verdict::Silent;
  if (c.oneSilent)
    return Verdict::Differs;
  const auto levelOk = std::abs(c.levelDb) <= kLevelToleranceDb;
  const auto spectrumOk = c.specMean <= kSpectrumMeanMaximumDb && c.specMax <= kSpectrumPeakMaximumDb;
  if (random)
    return levelOk && spectrumOk ? Verdict::RandomOk : Verdict::Differs;
  if (levelOk && c.corr >= kCorrelationMinimum)
    return Verdict::Inside;
  if (levelOk && spectrumOk)
    return Verdict::SpectrumOnly;
  return Verdict::Differs;
}

// ---------------------------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------------------------

struct Case {
  std::string part; // "A" or "B"
  std::string label;
  int type = -1;      // the effect under test (part A)
  float value = 0.0f; // part A
  int input = 0;      // 0 noise, 1 tones
  int fft = 4096;
  float amplitude = 0.6f;
  Params params{};
  bool random = false;
  std::string slotTypes; // types in use, for reporting
};

struct Outcome {
  Case source;
  Combined metrics;
  Verdict verdict = Verdict::Silent;
  bool failed = false;
};

std::string describeTypes(const Params& p)
{
  std::string result;
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto type = DtBlkFxLegacy::legacyValueToEffectType(p[4 + slot * 5 + 3]);
    if (type == 9 || type == 10)
      continue;
    if (!result.empty())
      result += ", ";
    result += std::to_string(type) + " " + (type >= 0 && type < 31 ? kEffectNames[static_cast<size_t>(type)] : "?");
  }
  return result.empty() ? "none" : result;
}

bool usesSmear(const Params& p)
{
  for (size_t slot = 0; slot < 8; ++slot)
    if (DtBlkFxLegacy::legacyValueToEffectType(p[4 + slot * 5 + 3]) == 2)
      return true;
  return false;
}

std::string fixed(double value, int digits)
{
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

Outcome runCase(Backend& legacy, Backend& fresh, Analyzer& analyzer, const Case& testCase, const Audio& input)
{
  Outcome outcome;
  outcome.source = testCase;
  Audio legacyOut;
  Audio freshOut;
  if (!render(legacy, forLegacy(testCase.params), input, legacyOut) ||
      !render(fresh, forNew(testCase.params), input, freshOut)) {
    outcome.failed = true;
    return outcome;
  }
  const auto left = compareChannel(legacyOut.left, freshOut.left, analyzer);
  const auto right = compareChannel(legacyOut.right, freshOut.right, analyzer);
  outcome.metrics = combine(left, right);
  outcome.verdict = judge(outcome.metrics, testCase.random);
  return outcome;
}

std::string csvRow(const Outcome& o)
{
  std::ostringstream out;
  out << o.source.part << ',' << '"' << o.source.label << '"' << ',' << o.source.type << ',' << fixed(o.source.value, 2)
      << ',' << (o.source.input == 0 ? "noise" : "tones") << ',' << o.source.fft << ',' << fixed(o.source.amplitude, 2) << ','
      << fixed(o.metrics.levelDb, 2) << ',' << fixed(o.metrics.corr, 3) << ',' << o.metrics.lag << ','
      << fixed(o.metrics.specMean, 2) << ',' << fixed(o.metrics.specMax, 2) << ',' << verdictName(o.verdict) << ','
      << '"' << o.source.slotTypes << '"';
  return out.str();
}

// Presets are parsed the way the legacy plugin does it: a name up to ':', then up to 44 numbers;
// reading stops at the first thing that is not a number (for example a comma), and every value
// that was not read stays 0.
bool parsePreset(const std::string& line, std::string& name, Params& params)
{
  const auto colon = line.find(':');
  if (colon == std::string::npos)
    return false;
  name = line.substr(0, colon);
  params.fill(0.0f);
  const char* text = line.c_str() + colon + 1;
  for (size_t i = 0; i < params.size(); ++i) {
    char* next = nullptr;
    const auto value = std::strtod(text, &next);
    if (next == text)
      break;
    params[i] = static_cast<float>(std::min(1.0, std::max(0.0, value)));
    text = next;
  }
  return true;
}

struct Options {
  std::string dll;
  bool full = false;
  int onlyEffect = -1;
  std::string csv;
  bool presets = true;
};

Options parseOptions(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--full")
      options.full = true;
    else if (argument == "--no-presets")
      options.presets = false;
    else if (argument == "--effect" && i + 1 < argc)
      options.onlyEffect = std::atoi(argv[++i]);
    else if (argument == "--csv" && i + 1 < argc)
      options.csv = argv[++i];
    else if (options.dll.empty())
      options.dll = argument;
  }
  return options;
}

struct Tally {
  int inside = 0;
  int spectrumOnly = 0;
  int differs = 0;
  int silent = 0;
  int randomOk = 0;
  int failed = 0;
  double worstLevel = 0.0;
  double lowestCorr = 1.0;
};

void count(Tally& tally, const Outcome& o)
{
  if (o.failed) {
    ++tally.failed;
    return;
  }
  switch (o.verdict) {
    case Verdict::Inside: ++tally.inside; break;
    case Verdict::SpectrumOnly: ++tally.spectrumOnly; break;
    case Verdict::Differs: ++tally.differs; break;
    case Verdict::Silent: ++tally.silent; break;
    case Verdict::RandomOk: ++tally.randomOk; break;
  }
  if (!o.metrics.silentBoth) {
    if (std::abs(o.metrics.levelDb) > std::abs(tally.worstLevel))
      tally.worstLevel = o.metrics.levelDb;
    tally.lowestCorr = std::min(tally.lowestCorr, o.metrics.corr);
  }
}

std::string detailLine(const Outcome& o)
{
  std::ostringstream out;
  out << "  " << std::left << std::setw(14) << verdictName(o.verdict) << ' ' << o.source.label << ": level "
      << fixed(o.metrics.levelDb, 2) << " dB, corr " << fixed(o.metrics.corr, 3) << ", lag " << o.metrics.lag
      << ", spectrum mean/max " << fixed(o.metrics.specMean, 2) << "/" << fixed(o.metrics.specMax, 2) << " dB";
  if (o.source.part == "B")
    out << "  [" << o.source.slotTypes << "]";
  return out.str();
}

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
  (void)argc;
  (void)argv;
  std::cout << "This comparison needs the 64-bit Windows VST2 plugin.\n";
  return 0;
#else
  const auto options = parseOptions(argc, argv);
  if (options.dll.empty()) {
    std::cout << "Usage: DtBlkFxEffectComparison.exe <path\\to\\legacy\\DtBlkFx.dll> [--full] [--effect N] [--csv file] "
                 "[--no-presets]\n";
    return 0;
  }

  LegacyBackend legacy;
  if (!legacy.load(options.dll)) {
    std::cout << "legacy plugin could not be used: " << legacy.error << "\n";
    return 0;
  }
  NewEngineBackend fresh;
  Analyzer analyzer;
  if (!analyzer.ok) {
    std::cout << "the spectrum analyser could not be prepared\n";
    return 0;
  }

  const std::array<Audio, 2> inputs{makeNoise(), makeTones()};
  std::cout << "Effect comparison: level within " << kLevelToleranceDb << " dB and correlation of at least "
            << kCorrelationMinimum << " count as 'inside'.\n";
  std::cout << "Both plugins are aligned to a total delay of " << kTargetDelay << " samples (legacy: Delay 1 beat; new: "
            << "fixed latency " << kLatency << " plus " << fixed(kNewDelayMs, 2) << " ms).\n";

  // Sanity check: a Filter with amplitude 0 over the whole band must silence a wet output.
  {
    std::array<Slot, 8> slots{};
    slots[0] = Slot{0.0f, 1.0f, 0.0f, 0, 0.5f};
    const auto base = makeBase(16, 0.175f, slots);
    Audio legacyOut;
    Audio freshOut;
    if (render(legacy, forLegacy(base), inputs[0], legacyOut) && render(fresh, forNew(base), inputs[0], freshOut)) {
      double ei = 0.0;
      double el = 0.0;
      double en = 0.0;
      for (int i = kAnalysisStart; i < kTotal; ++i) {
        ei += static_cast<double>(inputs[0].left[static_cast<size_t>(i)]) * inputs[0].left[static_cast<size_t>(i)];
        el += static_cast<double>(legacyOut.left[static_cast<size_t>(i)]) * legacyOut.left[static_cast<size_t>(i)];
        en += static_cast<double>(freshOut.left[static_cast<size_t>(i)]) * freshOut.left[static_cast<size_t>(i)];
      }
      const auto toDb = [&](double energy) { return 10.0 * std::log10(std::max(energy, 1.0e-30) / ei); };
      std::cout << "Wet-path check (Filter amplitude 0, output relative to input): legacy " << fixed(toDb(el), 1)
                << " dB, new " << fixed(toDb(en), 1) << " dB"
                << (toDb(el) < -60.0 && toDb(en) < -60.0 ? "  -> both wet, as intended\n"
                                                          : "  -> NOT both silent: the mix setting is not fully wet\n");
    }
  }

  // ---- Part A: every effect ----
  std::vector<Case> cases;
  const std::vector<int> plans = options.full ? std::vector<int>{16, 8} : std::vector<int>{16};
  const std::vector<float> amplitudes = options.full ? std::vector<float>{0.6f, 0.4f} : std::vector<float>{0.6f};
  for (int type = 0; type < 31; ++type) {
    if (type == 9 || type == 10)
      continue;
    if (options.onlyEffect >= 0 && type != options.onlyEffect)
      continue;
    for (const auto plan : plans)
      for (const auto amplitude : amplitudes)
        for (const auto value : {0.2f, 0.5f, 0.8f})
          for (int input = 0; input < 2; ++input) {
            std::array<Slot, 8> slots{};
            slots[0] = Slot{0.2f, 0.8f, amplitude, type, value};
            if (type >= 20 && type <= 25)
              slots[1] = Slot{0.0f, 1.0f, 0.3f, 0, 0.5f}; // a filter for the mask to act on
            Case c;
            c.part = "A";
            c.type = type;
            c.value = value;
            c.input = input;
            c.fft = DtBlkFxEngine::fftSizeAt(plan);
            c.amplitude = amplitude;
            c.params = makeBase(plan, 0.175f, slots);
            c.random = type == 2;
            c.slotTypes = std::to_string(type) + " " + kEffectNames[static_cast<size_t>(type)];
            c.label = std::string(kEffectNames[static_cast<size_t>(type)]) + " v" + fixed(value, 2) + " " +
                      (input == 0 ? "noise" : "tones") + " FFT" + std::to_string(c.fft) + " amp" + fixed(amplitude, 2);
            cases.push_back(c);
          }
  }

  // ---- Part B: the legacy presets ----
  if (options.presets && options.onlyEffect < 0) {
    auto folder = options.dll;
    const auto slash = folder.find_last_of("\\/");
    folder = slash == std::string::npos ? std::string() : folder.substr(0, slash + 1);
    std::ifstream file(folder + "dtblkfx\\stereo_presets.txt");
    if (!file)
      std::cout << "(stereo_presets.txt not found next to the DLL; skipping the presets)\n";
    std::string line;
    int presetCount = 0;
    while (file && std::getline(file, line)) {
      std::string name;
      Params params{};
      if (!parsePreset(line, name, params) || name.rfind("---", 0) == 0)
        continue;
      // Same block size and delay for both plugins; block sync becomes plain overlap.
      params[2] = fftLenParam(16);
      params[3] = params[3] > 0.5f ? 1.0f - params[3] : params[3];
      params[3] = std::min(params[3], 0.499f);
      for (int input = 0; input < 2; ++input) {
        Case c;
        c.part = "B";
        c.input = input;
        c.params = params;
        c.random = usesSmear(params);
        c.slotTypes = describeTypes(params);
        c.label = "preset '" + name + "' " + (input == 0 ? "noise" : "tones");
        cases.push_back(c);
      }
      ++presetCount;
    }
    std::cout << presetCount << " presets read\n";
  }

  std::cout << cases.size() << " cases, rendering both plugins for each (this takes a few minutes)...\n";

  std::vector<Outcome> outcomes;
  outcomes.reserve(cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    outcomes.push_back(runCase(legacy, fresh, analyzer, cases[i], inputs[static_cast<size_t>(cases[i].input)]));
    if ((i + 1) % 25 == 0)
      std::cerr << "  " << i + 1 << " / " << cases.size() << "\n";
  }

  // ---- Report ----
  std::map<int, Tally> perEffect;
  Tally presets;
  std::map<int, int> lagHistogram;
  for (const auto& o : outcomes) {
    if (!o.failed && !o.metrics.silentBoth && !o.metrics.oneSilent)
      ++lagHistogram[o.metrics.lag];
    if (o.source.part == "A")
      count(perEffect[o.source.type], o);
    else
      count(presets, o);
  }

  std::cout << "\n=== Part A: per effect (" << (options.full ? "full" : "quick") << " set) ===\n";
  std::cout << std::left << std::setw(5) << "type" << std::setw(18) << "effect" << std::right << std::setw(8) << "inside"
            << std::setw(9) << "spec-only" << std::setw(8) << "DIFFERS" << std::setw(8) << "silent" << std::setw(8) << "random"
            << std::setw(12) << "worst dB" << std::setw(10) << "min corr" << "\n";
  for (const auto& entry : perEffect) {
    const auto& t = entry.second;
    std::cout << std::left << std::setw(5) << entry.first << std::setw(18) << kEffectNames[static_cast<size_t>(entry.first)]
              << std::right << std::setw(8) << t.inside << std::setw(9) << t.spectrumOnly << std::setw(8) << t.differs
              << std::setw(8) << t.silent << std::setw(8) << t.randomOk << std::setw(12) << fixed(t.worstLevel, 2)
              << std::setw(10) << fixed(t.lowestCorr, 3) << (t.failed > 0 ? "  (render failed)" : "") << "\n";
  }

  std::cout << "\nLag between the two outputs (samples; 0 means perfectly aligned):";
  for (const auto& entry : lagHistogram)
    std::cout << "  " << entry.first << " x" << entry.second;
  std::cout << "\n";

  std::cout << "\n=== Part A: cases that are not inside ===\n";
  int shown = 0;
  for (const auto& o : outcomes)
    if (o.source.part == "A" && (o.failed || o.verdict == Verdict::Differs || o.verdict == Verdict::SpectrumOnly)) {
      if (shown++ < 140)
        std::cout << (o.failed ? "  RENDER FAILED " + o.source.label : detailLine(o)) << "\n";
    }
  if (shown > 140)
    std::cout << "  ... and " << shown - 140 << " more (see the CSV file)\n";
  if (shown == 0)
    std::cout << "  none\n";

  if (options.presets && options.onlyEffect < 0) {
    std::cout << "\n=== Part B: presets (" << presets.inside + presets.spectrumOnly + presets.differs + presets.silent + presets.randomOk
              << " runs): inside " << presets.inside << ", spectrum-only " << presets.spectrumOnly << ", DIFFERS " << presets.differs
              << ", silent " << presets.silent << ", ok(random) " << presets.randomOk << " ===\n";
    int shownPresets = 0;
    for (const auto& o : outcomes)
      if (o.source.part == "B" && (o.failed || o.verdict == Verdict::Differs || o.verdict == Verdict::SpectrumOnly)) {
        if (shownPresets++ < 120)
          std::cout << (o.failed ? "  RENDER FAILED " + o.source.label : detailLine(o)) << "\n";
      }
    if (shownPresets > 120)
      std::cout << "  ... and " << shownPresets - 120 << " more (see the CSV file)\n";
    if (shownPresets == 0)
      std::cout << "  none\n";
  }

  if (!options.csv.empty()) {
    std::ofstream csv(options.csv);
    csv << "part,case,type,value,input,fft,amplitude,level_db,corr,lag,spec_mean_db,spec_max_db,verdict,slots\n";
    for (const auto& o : outcomes)
      csv << csvRow(o) << "\n";
    std::cout << "\nAll cases written to " << options.csv << "\n";
  }
  return 0;
#endif
}
