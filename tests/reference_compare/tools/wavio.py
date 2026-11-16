"""Minimal WAV reader/writer (PCM16/24/32 and IEEE float32) without external dependencies."""
import struct

import numpy as np


def read_wav(path):
    """Return (data[n, channels] as float32, sample_rate)."""
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos = 12
    fmt = None
    data = None
    while pos + 8 <= len(blob):
        chunk_id = blob[pos:pos + 4]
        size = struct.unpack("<I", blob[pos + 4:pos + 8])[0]
        body = blob[pos + 8:pos + 8 + size]
        if chunk_id == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and size >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif chunk_id == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError(f"{path}: missing fmt or data chunk")
    tag, channels, rate, bits = fmt
    if tag == 3 and bits == 32:
        samples = np.frombuffer(data, dtype="<f4").astype(np.float32)
    elif tag == 1 and bits == 16:
        samples = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
    elif tag == 1 and bits == 24:
        raw = np.frombuffer(data, dtype=np.uint8)
        raw = raw[: (len(raw) // 3) * 3].reshape(-1, 3).astype(np.int32)
        value = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        value = np.where(value & 0x800000, value - 0x1000000, value)
        samples = value.astype(np.float32) / 8388608.0
    elif tag == 1 and bits == 32:
        samples = np.frombuffer(data, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"{path}: unsupported format tag={tag} bits={bits}")
    frames = len(samples) // channels
    return samples[: frames * channels].reshape(frames, channels), rate


def write_wav(path, data, sample_rate):
    """Write float32 WAV. data has shape (n,) or (n, channels)."""
    data = np.asarray(data, dtype="<f4")
    if data.ndim == 1:
        data = data[:, None]
    channels = data.shape[1]
    payload = data.tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 3, channels, sample_rate,
                                    sample_rate * channels * 4, channels * 4, 32)
    header += b"data" + struct.pack("<I", len(payload))
    with open(path, "wb") as f:
        f.write(header + payload)
