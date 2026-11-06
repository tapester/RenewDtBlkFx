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

// Diagnostic, not a pass/fail test. Measures the delay between input and output of the new engine
// and, when a path to the legacy 64-bit VST2 DLL is given, of the legacy plugin, at steady
// settings and while the FFT size changes. It always exits with 0.
//
// Usage: DtBlkFxFftSwitchDiagnostic.exe [path\to\legacy\DtBlkFx.dll]
//
// The legacy plugin is driven through a hand-written description of the VST2 interface (no SDK).

#include "DtBlkFxEngine.h"
#include "DtBlkFxLegacyMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

namespace {

constexpr double kRate = 44100.0;
constexpr double kBpm = 120.0;
constexpr double kSamplesPerBeat = kRate * 60.0 / kBpm; // 22050
constexpr int kBlock = 256;
constexpr int kClickSpacing = 2000;

// ---------------------------------------------------------------------------------------------
// Parameters (the legacy order: 4 global values, then 8 effect slots of 5 values each)
// ---------------------------------------------------------------------------------------------

using Params = std::array<float, 44>;

float fftLenParam(int planIndex)
{
  return static_cast<float>((planIndex + 2) * 4) / 255.0f;
}

// Delay parameter for a beat-based delay (below 0.5) or a millisecond delay (0.5 and above).
float delayParamBeats(double beats)
{
  return static_cast<float>(beats / (0.5 * 255.0 / 16.0) / 2.0);
}
float delayParamMs(double ms)
{
  return static_cast<float>(0.5 + ms / 6000.0 * 0.5);
}

struct DelaySetting {
  const char* label;
  float param;
  double expectedSamples; // nominal delay in samples, the 100-sample floor applied
};

DelaySetting delayMs(const char* label, double ms)
{
  return {label, delayParamMs(ms), std::max(100.0, ms * kRate / 1000.0)};
}
DelaySetting delayBeats(const char* label, double beats)
{
  return {label, delayParamBeats(beats), std::max(100.0, beats * kSamplesPerBeat)};
}

Params makeParams(int plan, float delayParam)
{
  Params p{};
  p[0] = 1.0f; // fully wet, no power match
  p[1] = delayParam;
  p[2] = fftLenParam(plan);
  p[3] = 0.0f; // no overlap
  for (size_t slot = 0; slot < 8; ++slot) {
    const auto offset = 4 + slot * 5;
    p[offset] = 0.0f;
    p[offset + 1] = 1.0f;
    p[offset + 2] = 0.6f;
    p[offset + 3] = DtBlkFxLegacy::effectTypeToLegacyValue(9); // None
    p[offset + 4] = 0.5f;
  }
  return p;
}

// Which plan is in use at a given position? The change cases switch at the first block at or after
// switchAt; with 'ramp' they step through every plan in between, one per 1500 samples.
int planAt(int position, int fromPlan, int toPlan, int switchAt, bool ramp)
{
  if (position < switchAt)
    return fromPlan;
  if (!ramp)
    return toPlan;
  const auto steps = std::abs(toPlan - fromPlan);
  const auto done = std::min(steps, (position - switchAt) / 1500 + 1);
  return fromPlan + (toPlan > fromPlan ? done : -done);
}

// ---------------------------------------------------------------------------------------------
// Backends: something that turns audio into audio while parameters change
// ---------------------------------------------------------------------------------------------

struct Backend {
  virtual ~Backend() = default;
  // Brings the backend to a clean state with the given parameters; nothing from an earlier run
  // may remain in its buffers.
  virtual bool restart(const Params& params) = 0;
  virtual bool setParams(const Params& params) = 0;
  virtual bool process(float* left, float* right, int count) = 0;
};

// --- new engine ---

struct NewEngineBackend : Backend {
  DtBlkFxEngine engine;

  bool restart(const Params& params) override
  {
    // prepare() releases everything first, so each run starts from a fresh engine.
    if (!engine.prepare(8, kRate))
      return false;
    engine.reset();
    return engine.configureLegacyParameters(params, kSamplesPerBeat, 0.0, false);
  }
  bool setParams(const Params& params) override
  {
    return engine.configureLegacyParameters(params, kSamplesPerBeat, 0.0, false);
  }
  bool process(float* left, float* right, int count) override
  {
    std::array<float*, 2> channels{left, right};
    return engine.process(channels.data(), 2, count);
  }
};

// --- legacy plugin, VST2 ---
// Hand-written description of the VST2 plugin interface. Only what is needed to load a plugin,
// set parameters and process audio. Layout for 64-bit Windows.

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
// audioMaster opcodes
constexpr std::int32_t kAudioMasterVersion = 1;
constexpr std::int32_t kAudioMasterGetTime = 7;
constexpr std::int32_t kAudioMasterGetSampleRate = 16;
constexpr std::int32_t kAudioMasterGetBlockSize = 17;
// effect opcodes
constexpr std::int32_t kEffOpen = 0;
constexpr std::int32_t kEffClose = 1;
constexpr std::int32_t kEffSetSampleRate = 10;
constexpr std::int32_t kEffSetBlockSize = 11;
constexpr std::int32_t kEffMainsChanged = 12;
// VstTimeInfo flags
constexpr std::int32_t kVstPpqPosValid = 1 << 9;
constexpr std::int32_t kVstTempoValid = 1 << 10;

// The host side: a fixed 120 BPM time info whose position advances with the audio.
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

  ~LegacyBackend() override
  {
    if (effect != nullptr) {
      if (opened) {
        effect->dispatcher(effect, kEffMainsChanged, 0, 0, nullptr, 0.0f);
        effect->dispatcher(effect, kEffClose, 0, 0, nullptr, 0.0f);
      }
    }
    if (module != nullptr)
      FreeLibrary(module);
  }

  bool load(const std::string& path)
  {
    static_assert(sizeof(void*) == 8, "this harness describes the 64-bit VST2 layout");
    // Sizes computed for 64-bit Windows with the field list above.
    static_assert(sizeof(AEffect) == 192, "AEffect layout does not match the 64-bit VST2 structure");
    static_assert(sizeof(VstTimeInfo) == 88, "VstTimeInfo layout does not match the VST2 structure");
    static_assert(offsetof(AEffect, numParams) == 44, "AEffect field offsets differ from the VST2 structure");
    static_assert(offsetof(AEffect, processReplacing) == 120, "AEffect field offsets differ from the VST2 structure");
    static_assert(offsetof(VstTimeInfo, tempo) == 32, "VstTimeInfo field offsets differ from the VST2 structure");
    static_assert(offsetof(VstTimeInfo, flags) == 84, "VstTimeInfo field offsets differ from the VST2 structure");

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
    std::cout << "legacy plugin loaded: " << effect->numParams << " parameters, " << effect->numInputs
              << " inputs, " << effect->numOutputs << " outputs, initial delay " << effect->initialDelay
              << " samples\n";
    if (effect->numParams < 44 || effect->numInputs != 2 || effect->numOutputs != 2) {
      error = "this is not the stereo build with 44+ parameters";
      return false;
    }
    if (effect->processReplacing == nullptr) {
      error = "no processReplacing";
      return false;
    }
    return true;
  }


  bool restart(const Params& params) override
  {
    // Suspend, then resume: the plugin clears its buffers when it is suspended.
    effect->dispatcher(effect, kEffMainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, kEffMainsChanged, 0, 1, nullptr, 0.0f);
    g_hostPosition = 0.0;
    return setParams(params);
  }
  bool setParams(const Params& params) override
  {
    for (int i = 0; i < 44; ++i)
      effect->setParameter(effect, i, params[static_cast<size_t>(i)]);
    return true;
  }
  bool process(float* left, float* right, int count) override
  {
    std::array<float*, 2> in{left, right};
    std::vector<float> outLeft(static_cast<size_t>(count));
    std::vector<float> outRight(static_cast<size_t>(count));
    std::array<float*, 2> out{outLeft.data(), outRight.data()};
    effect->processReplacing(effect, in.data(), out.data(), count);
    std::memcpy(left, outLeft.data(), sizeof(float) * static_cast<size_t>(count));
    std::memcpy(right, outRight.data(), sizeof(float) * static_cast<size_t>(count));
    g_hostPosition += count;
    return true;
  }
};
#endif

// ---------------------------------------------------------------------------------------------
// Runs
// ---------------------------------------------------------------------------------------------

struct Click {
  int position;
  float height;
};

// Runs a signal through a backend; the plan follows 'planFor' and the delay parameter is fixed.
// 'clickPositions' lists the input click positions.
template <typename PlanFor>
std::vector<float> runSignal(Backend& backend, int total, const std::vector<int>& clickPositions, const PlanFor& planFor,
                             float delayParam)
{
  std::vector<float> left(static_cast<size_t>(total), 0.0f);
  std::vector<float> right(static_cast<size_t>(total), 0.0f);
  for (const auto position : clickPositions) {
    left[static_cast<size_t>(position)] = 0.5f;
    right[static_cast<size_t>(position)] = 0.5f;
  }

  if (!backend.restart(makeParams(planFor(0), delayParam)))
    return {};
  for (int position = 0; position < total; position += kBlock) {
    backend.setParams(makeParams(planFor(position), delayParam));
    if (!backend.process(left.data() + position, right.data() + position, std::min(kBlock, total - position)))
      return {};
  }
  return left;
}

std::vector<Click> findClicks(const std::vector<float>& signal)
{
  std::vector<Click> clicks;
  const auto total = static_cast<int>(signal.size());
  for (int i = 1; i + 1 < total; ++i) {
    const auto value = std::abs(signal[static_cast<size_t>(i)]);
    if (value > 0.1f && value >= std::abs(signal[static_cast<size_t>(i - 1)]) &&
        value > std::abs(signal[static_cast<size_t>(i + 1)]))
      clicks.push_back({i, value});
  }
  return clicks;
}

// A single click: its output position minus its input position is the plugin's delay.
void runSteadyCases(Backend& backend, const std::string& name)
{
  const std::vector<DelaySetting> delays{
      delayMs("delay 2 ms (floor)", 2.0),
      delayMs("delay 50 ms", 50.0),
      delayBeats("delay 1 beat", 1.0),
      delayBeats("delay 2 beats", 2.0),
  };
  const std::array<std::pair<int, const char*>, 3> sizes{{{8, "FFT 1024"}, {16, "FFT 4096"}, {24, "FFT 16384"}}};

  constexpr int clickAt = 1000;
  constexpr int total = 180000;

  std::cout << "\n--- steady settings, one click at input sample " << clickAt << ": " << name << " ---\n";
  std::cout << "  (delay = output position of the click minus " << clickAt << ")\n";
  for (const auto& delay : delays) {
    for (const auto& size : sizes) {
      const auto plan = size.first;
      const auto signal = runSignal(backend, total, {clickAt}, [plan](int) { return plan; }, delay.param);
      const auto clicks = findClicks(signal);
      std::cout << "  " << delay.label << ", " << size.second << ": ";
      if (clicks.empty()) {
        std::cout << "no output click\n";
        continue;
      }
      std::cout << "delay " << clicks.front().position - clickAt << " samples (nominal "
                << static_cast<int>(std::lround(delay.expectedSamples)) << ", height " << clicks.front().height << ")";
      if (clicks.size() > 1)
        std::cout << "  EXTRA clicks: " << clicks.size() - 1;
      std::cout << "\n";
    }
  }
}

// A click train while the FFT size changes. Prints the phase of every output click (position
// modulo the click spacing minus the first click's) so that a constant delay shows as a constant.
void runChangeCase(Backend& backend, const std::string& name, const DelaySetting& delay, const std::string& label,
                   int fromPlan, int toPlan, bool ramp)
{
  constexpr int switchAt = 80000;
  constexpr int total = 200000;
  constexpr int firstClick = 1000;

  std::vector<int> clickPositions;
  for (int position = firstClick; position < total; position += kClickSpacing)
    clickPositions.push_back(position);

  const auto signal = runSignal(
      backend, total, clickPositions,
      [=](int position) { return planAt(position, fromPlan, toPlan, switchAt, ramp); }, delay.param);
  const auto clicks = findClicks(signal);

  std::cout << "\n--- " << label << " (FFT " << DtBlkFxEngine::fftSizeAt(fromPlan) << " -> "
            << DtBlkFxEngine::fftSizeAt(toPlan) << (ramp ? ", ramped" : ", direct") << "), " << delay.label << ", " << name
            << " ---\n";

  // Phase before the switch, from the last clicks before it.
  int phaseBefore = -1;
  for (const auto& click : clicks)
    if (click.position < switchAt - 4000)
      phaseBefore = ((click.position - firstClick) % kClickSpacing + kClickSpacing) % kClickSpacing;
  std::cout << "  phase before the change (output position minus input grid, modulo " << kClickSpacing
            << "): " << phaseBefore << "\n";

  int previous = -1;
  int shown = 0;
  int pairs = 0;
  int inWindow = 0;
  const int windowEnd = switchAt + 24000;
  for (const auto& click : clicks) {
    if (click.position < switchAt - 1000 || click.position > windowEnd)
      continue;
    ++inWindow;
    const auto phase = ((click.position - firstClick) % kClickSpacing + kClickSpacing) % kClickSpacing;
    if (previous >= 0 && click.position - previous < kClickSpacing / 2)
      ++pairs;
    if (shown < 26)
      std::cout << "  out " << click.position << "  height " << click.height << "  phase " << phase << "  gap "
                << (previous < 0 ? 0 : click.position - previous) << "\n";
    ++shown;
    previous = click.position;
  }
  const auto expected = (windowEnd - (switchAt - 1000)) / kClickSpacing;
  std::cout << "  summary: " << inWindow << " clicks in the window (about " << expected << " expected), " << pairs
            << " pairs closer than " << kClickSpacing / 2 << " samples\n";
}

void runAll(Backend& backend, const std::string& name)
{
  std::cout << "\n==================== " << name << " ====================\n";
  runSteadyCases(backend, name);

  const auto fiftyMs = delayMs("delay 50 ms", 50.0);
  const auto oneBeat = delayBeats("delay 1 beat", 1.0);
  for (const auto& delay : {fiftyMs, oneBeat}) {
    runChangeCase(backend, name, delay, "shrink", 16, 8, false);
    runChangeCase(backend, name, delay, "grow", 8, 16, false);
    runChangeCase(backend, name, delay, "shrink", 16, 8, true);
    runChangeCase(backend, name, delay, "grow", 8, 16, true);
  }
}

} // namespace

int main(int argc, char** argv)
{
  {
    NewEngineBackend engine;
    runAll(engine, "NEW ENGINE");
  }

#ifdef _WIN32
  if (argc >= 2) {
    LegacyBackend legacy;
    if (!legacy.load(argv[1])) {
      std::cout << "\nlegacy plugin could not be used: " << legacy.error << "\n";
      return 0;
    }
    runAll(legacy, "LEGACY PLUGIN");
  }
  else {
    std::cout << "\n(no legacy DLL given; pass the path to DtBlkFx.dll as the first argument)\n";
  }
#else
  (void)argc;
  (void)argv;
#endif
  return 0;
}
