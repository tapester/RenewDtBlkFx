// Minimal offline VST2 host that renders a WAV file through the LEGACY DtBlkFx DLL with a given
// 44-value parameter vector. Windows only. NOT TESTED in the environment this file was written in
// (no Windows, no legacy DLL, no VST SDK) - treat the first run as part of the setup.
//
// Build (bitness MUST match the legacy DLL; open the matching "x64/x86 Native Tools" prompt):
//   cl /EHsc /O2 /I <VST2 SDK>\pluginterfaces\vst2.x vst2_render_host.cpp /Fe:vst2_render_host.exe
//
// usage:
//   vst2_render_host plugin.dll in.wav out.wav --params v0,...,v43 [--tempo 120] [--block 512]
//
// The host answers audioMasterGetTime with tempo + ppq position so the legacy beat-synchronous
// delay and block sync behave as in a DAW. Output length equals input length (no latency
// compensation; compare.py estimates the lag). Legacy reports initialDelay = 0.

#include <windows.h>

#include "aeffectx.h"
#include "../engine_render/wav_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static VstTimeInfo g_timeInfo;
static double g_sampleRate = 44100.0;
static int g_blockSize = 512;

static VstIntPtr VSTCALLBACK hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void* ptr, float)
{
  switch (opcode) {
    case audioMasterVersion: return 2400;
    case audioMasterGetTime: return reinterpret_cast<VstIntPtr>(&g_timeInfo);
    case audioMasterGetSampleRate: return static_cast<VstIntPtr>(g_sampleRate);
    case audioMasterGetBlockSize: return g_blockSize;
    case audioMasterGetCurrentProcessLevel: return kVstProcessLevelUser;
    case audioMasterIOChanged: return 1;
    case audioMasterGetVendorString: std::strcpy(static_cast<char*>(ptr), "reference_host"); return 1;
    case audioMasterGetProductString: std::strcpy(static_cast<char*>(ptr), "vst2_render_host"); return 1;
    case audioMasterGetVendorVersion: return 1;
    case audioMasterCanDo: return std::strcmp(static_cast<const char*>(ptr), "sendVstTimeInfo") == 0 ? 1 : 0;
    default: return 0;
  }
}

typedef AEffect*(VSTCALLBACK* PluginEntry)(audioMasterCallback);

int main(int argc, char** argv)
{
  if (argc < 5) {
    std::cerr << "usage: vst2_render_host plugin.dll in.wav out.wav --params v0,...,v43 [--tempo 120] [--block 512]\n";
    return 2;
  }
  const std::string dllPath = argv[1], inPath = argv[2], outPath = argv[3];
  float params[44] = {};
  bool haveParams = false;
  double tempo = 120.0;
  for (int i = 4; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    if (key == "--params")
      haveParams = wavio::parseList(argv[i + 1], params, 44);
    else if (key == "--tempo")
      tempo = std::atof(argv[i + 1]);
    else if (key == "--block")
      g_blockSize = std::max(1, std::atoi(argv[i + 1]));
  }
  if (!haveParams) {
    std::cerr << "need exactly 44 comma separated values in --params\n";
    return 2;
  }

  wavio::Wav wav;
  if (!wavio::read(inPath, wav)) {
    std::cerr << "cannot read input wav\n";
    return 1;
  }
  g_sampleRate = wav.sampleRate;

  HMODULE module = LoadLibraryA(dllPath.c_str());
  if (!module) {
    std::cerr << "LoadLibrary failed (wrong bitness? missing fftw/libpng DLLs next to the plugin?)\n";
    return 1;
  }
  auto entry = reinterpret_cast<PluginEntry>(GetProcAddress(module, "VSTPluginMain"));
  if (!entry)
    entry = reinterpret_cast<PluginEntry>(GetProcAddress(module, "main"));
  if (!entry) {
    std::cerr << "no VSTPluginMain/main export\n";
    return 1;
  }
  AEffect* effect = entry(hostCallback);
  if (!effect || effect->magic != kEffectMagic) {
    std::cerr << "not a VST2 plugin\n";
    return 1;
  }
  if (effect->numParams != 44)
    std::cerr << "warning: plugin reports " << effect->numParams << " parameters (expected 44)\n";
  if (effect->numInputs != wav.channels || effect->numOutputs != wav.channels)
    std::cerr << "warning: plugin has " << effect->numInputs << " in / " << effect->numOutputs
              << " out, wav has " << wav.channels << " channels (use the matching mono/stereo build)\n";

  effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
  effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, static_cast<float>(g_sampleRate));
  effect->dispatcher(effect, effSetBlockSize, 0, g_blockSize, nullptr, 0.0f);
  effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
  effect->dispatcher(effect, effStartProcess, 0, 0, nullptr, 0.0f);
  for (int i = 0; i < 44 && i < effect->numParams; ++i)
    effect->setParameter(effect, i, params[i]);

  const int channels = wav.channels;
  const size_t frames = wav.frames();
  std::vector<std::vector<float>> in(channels, std::vector<float>(g_blockSize));
  std::vector<std::vector<float>> out(channels, std::vector<float>(g_blockSize));
  std::vector<float*> inPtr(channels), outPtr(channels);
  for (int ch = 0; ch < channels; ++ch) {
    inPtr[ch] = in[ch].data();
    outPtr[ch] = out[ch].data();
  }

  wavio::Wav result;
  result.channels = channels;
  result.sampleRate = wav.sampleRate;
  result.interleaved.assign(wav.interleaved.size(), 0.0f);
  const double samplesPerBeat = g_sampleRate * 60.0 / tempo;

  std::memset(&g_timeInfo, 0, sizeof(g_timeInfo));
  g_timeInfo.sampleRate = g_sampleRate;
  g_timeInfo.tempo = tempo;
  g_timeInfo.timeSigNumerator = 4;
  g_timeInfo.timeSigDenominator = 4;
  g_timeInfo.flags = kVstTempoValid | kVstPpqPosValid | kVstTransportPlaying | kVstTimeSigValid;

  for (size_t start = 0; start < frames; start += static_cast<size_t>(g_blockSize)) {
    const size_t count = std::min(static_cast<size_t>(g_blockSize), frames - start);
    for (int ch = 0; ch < channels; ++ch) {
      std::fill(in[ch].begin(), in[ch].end(), 0.0f);
      for (size_t i = 0; i < count; ++i)
        in[ch][i] = wav.interleaved[(start + i) * channels + ch];
    }
    g_timeInfo.samplePos = static_cast<double>(start);
    g_timeInfo.ppqPos = static_cast<double>(start) / samplesPerBeat;
    effect->processReplacing(effect, inPtr.data(), outPtr.data(), static_cast<VstInt32>(count));
    for (int ch = 0; ch < channels; ++ch)
      for (size_t i = 0; i < count; ++i)
        result.interleaved[(start + i) * channels + ch] = out[ch][i];
  }

  effect->dispatcher(effect, effStopProcess, 0, 0, nullptr, 0.0f);
  effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);
  effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
  FreeLibrary(module);

  if (!wavio::write(outPath, result)) {
    std::cerr << "cannot write output\n";
    return 1;
  }
  return 0;
}
