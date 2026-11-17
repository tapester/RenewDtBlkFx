// Headless renderer: pushes a WAV file through DtBlkFxEngine with a complete legacy parameter vector.
// It mirrors what DtBlkFxAudioProcessor does (prepare -> reset -> configureLegacyParameters per
// host block -> process) so renders are comparable with the legacy plugin driven by the same vector.
//
// usage: render_engine in.wav out.wav --params v0,v1,...,v43 [--tempo 120] [--block 512]
//
// Input WAV: IEEE float32 or PCM16, 1 or 2 channels. Output: float32, same length as input
// (the engine's latency is NOT compensated; the comparison tool estimates the lag).

#include "DtBlkFxEngine.h"
#include "wav_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
  if (argc < 4) {
    std::cerr << "usage: render_engine in.wav out.wav --params v0,...,v43 [--tempo 120] [--block 512]\n";
    return 2;
  }
  const std::string inPath = argv[1], outPath = argv[2];
  std::array<float, 44> params{};
  bool haveParams = false;
  double tempo = 120.0;
  int block = 512;
  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    if (key == "--params")
      haveParams = wavio::parseList(argv[i + 1], params.data(), params.size());
    else if (key == "--tempo")
      tempo = std::atof(argv[i + 1]);
    else if (key == "--block")
      block = std::max(1, std::atoi(argv[i + 1]));
  }
  if (!haveParams) {
    std::cerr << "need exactly 44 comma separated values in --params\n";
    return 2;
  }

  wavio::Wav wav;
  if (!wavio::read(inPath, wav)) {
    std::cerr << "cannot read input wav: " << inPath << "\n";
    return 1;
  }
  const double sampleRate = wav.sampleRate;
  const double samplesPerBeat = sampleRate * 60.0 / tempo;

  // Same plan-index formula as the engine/processor (legacy BlkFxParam::getPlan).
  const auto plan = std::clamp(static_cast<int>(params[2] * 255.0f / 4.0f - 1.5f), 0,
                               DtBlkFxEngine::fftSizeCount - 1);
  DtBlkFxEngine engine;
  if (!engine.prepare(plan, sampleRate)) {
    std::cerr << "engine.prepare failed\n";
    return 1;
  }
  engine.reset();
  if (!engine.configureLegacyParameters(params, samplesPerBeat, 0.0, false)) {
    std::cerr << "configureLegacyParameters failed\n";
    return 1;
  }

  const auto channels = wav.channels;
  const auto frames = wav.frames();
  wavio::Wav out;
  out.channels = channels;
  out.sampleRate = wav.sampleRate;
  out.interleaved.assign(wav.interleaved.size(), 0.0f);
  std::vector<std::vector<float>> planar(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(block)));
  std::vector<float*> pointers(static_cast<size_t>(channels));
  for (size_t start = 0; start < frames; start += static_cast<size_t>(block)) {
    const auto count = std::min(static_cast<size_t>(block), frames - start);
    for (int ch = 0; ch < channels; ++ch) {
      pointers[static_cast<size_t>(ch)] = planar[static_cast<size_t>(ch)].data();
      for (size_t i = 0; i < count; ++i)
        planar[static_cast<size_t>(ch)][i] = wav.interleaved[(start + i) * channels + ch];
    }
    // The plugin queues a parameter snapshot with every host block, together with the beat phase
    // derived from the host's ppq position (transport starts at ppq 0 with sample 0). Passing a
    // constant phase would restart the engine's beat grid every block and break block-sync overlap.
    const auto beatPhase = std::fmod(static_cast<double>(start), samplesPerBeat);
    if (!engine.configureLegacyParameters(params, samplesPerBeat, beatPhase, false) ||
        !engine.process(pointers.data(), channels, static_cast<int>(count))) {
      std::cerr << "engine failure at sample " << start << "\n";
      return 1;
    }
    for (int ch = 0; ch < channels; ++ch)
      for (size_t i = 0; i < count; ++i)
        out.interleaved[(start + i) * channels + ch] = planar[static_cast<size_t>(ch)][i];
  }

  if (!wavio::write(outPath, out)) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }
  std::cerr << "engine: fft=" << engine.getFftSize() << " latency=" << engine.getLatencySamples()
            << " delay=" << engine.getActiveDelaySamples() << "\n";
  return 0;
}
