// Tiny WAV helpers shared by render_engine.cpp and legacy_render/vst2_render_host.cpp.
// Reads IEEE float32 / PCM16 (1-2 channels), writes IEEE float32.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace wavio {

struct Wav {
  int channels = 0;
  int sampleRate = 0;
  std::vector<float> interleaved;
  size_t frames() const { return channels > 0 ? interleaved.size() / static_cast<size_t>(channels) : 0; }
};

inline uint32_t rd32(const unsigned char* p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t rd16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

inline bool read(const std::string& path, Wav& wav)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  std::vector<unsigned char> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (blob.size() < 12 || std::memcmp(blob.data(), "RIFF", 4) != 0 || std::memcmp(blob.data() + 8, "WAVE", 4) != 0)
    return false;
  int tag = 0, bits = 0;
  size_t dataPos = 0, dataSize = 0;
  for (size_t pos = 12; pos + 8 <= blob.size();) {
    const auto size = static_cast<size_t>(rd32(&blob[pos + 4]));
    if (std::memcmp(&blob[pos], "fmt ", 4) == 0 && pos + 8 + 16 <= blob.size()) {
      tag = rd16(&blob[pos + 8]);
      wav.channels = rd16(&blob[pos + 10]);
      wav.sampleRate = static_cast<int>(rd32(&blob[pos + 12]));
      bits = rd16(&blob[pos + 22]);
    }
    else if (std::memcmp(&blob[pos], "data", 4) == 0) {
      dataPos = pos + 8;
      dataSize = std::min(size, blob.size() - dataPos);
    }
    pos += 8 + size + (size & 1);
  }
  if (wav.channels < 1 || wav.channels > 2 || dataPos == 0)
    return false;
  if (tag == 3 && bits == 32) {
    wav.interleaved.resize(dataSize / 4);
    std::memcpy(wav.interleaved.data(), &blob[dataPos], wav.interleaved.size() * 4);
  }
  else if (tag == 1 && bits == 16) {
    wav.interleaved.resize(dataSize / 2);
    for (size_t i = 0; i < wav.interleaved.size(); ++i)
      wav.interleaved[i] = static_cast<int16_t>(rd16(&blob[dataPos + i * 2])) / 32768.0f;
  }
  else
    return false;
  wav.interleaved.resize(wav.frames() * static_cast<size_t>(wav.channels));
  return true;
}

inline bool write(const std::string& path, const Wav& wav)
{
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  const uint32_t dataBytes = static_cast<uint32_t>(wav.interleaved.size() * 4);
  auto w32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
  auto w16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
  file.write("RIFF", 4);
  w32(36 + dataBytes);
  file.write("WAVEfmt ", 8);
  w32(16);
  w16(3);
  w16(static_cast<uint16_t>(wav.channels));
  w32(static_cast<uint32_t>(wav.sampleRate));
  w32(static_cast<uint32_t>(wav.sampleRate * wav.channels * 4));
  w16(static_cast<uint16_t>(wav.channels * 4));
  w16(32);
  file.write("data", 4);
  w32(dataBytes);
  file.write(reinterpret_cast<const char*>(wav.interleaved.data()), dataBytes);
  return static_cast<bool>(file);
}

// Parse "v0,v1,..." into out[0..n). Returns true when exactly n values were read.
inline bool parseList(const char* text, float* out, size_t n)
{
  size_t count = 0;
  const char* p = text;
  while (*p && count < n) {
    char* end = nullptr;
    out[count++] = std::strtof(p, &end);
    if (end == p)
      return false;
    p = (*end == ',') ? end + 1 : end;
  }
  return count == n && *p == '\0';
}

}  // namespace wavio
