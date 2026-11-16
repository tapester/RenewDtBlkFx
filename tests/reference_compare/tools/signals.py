"""Deterministic stereo test signals. Every signal = 6 s content + 4 s silence (tail for delay/latency)."""
import numpy as np

CONTENT_SECONDS = 6.0
TAIL_SECONDS = 4.0


def _finish(left, right, sr):
    tail = np.zeros(int(TAIL_SECONDS * sr))
    out = np.stack([np.concatenate([left, tail]), np.concatenate([right, tail])], axis=1)
    return out.astype(np.float32)


def _time(sr):
    return np.arange(int(CONTENT_SECONDS * sr)) / sr


def chord(sr):
    """A-minor chord with 4 harmonics each, slow attack/release, decorrelated L/R phases."""
    t = _time(sr)
    left = np.zeros_like(t)
    right = np.zeros_like(t)
    for index, freq in enumerate([110.0, 164.81, 220.0, 261.63, 329.63]):
        for harmonic in range(1, 5):
            amp = 1.0 / harmonic
            left += amp * np.sin(2 * np.pi * freq * harmonic * t + 0.3 * index)
            right += amp * np.sin(2 * np.pi * freq * harmonic * t + 1.1 * index + 0.5 * harmonic)
    env = np.minimum(1.0, np.minimum(t / 0.5, (CONTENT_SECONDS - t) / 0.5))
    peak = max(np.abs(left).max(), np.abs(right).max())
    return _finish(0.5 * env * left / peak, 0.5 * env * right / peak, sr)


def sweep(sr):
    """Logarithmic sine sweep 40 Hz .. 12 kHz."""
    t = _time(sr)
    f0, f1 = 40.0, 12000.0
    k = np.log(f1 / f0) / CONTENT_SECONDS
    phase = 2 * np.pi * f0 * (np.exp(k * t) - 1.0) / k
    env = np.minimum(1.0, np.minimum(t / 0.05, (CONTENT_SECONDS - t) / 0.05))
    sig = 0.4 * env * np.sin(phase)
    return _finish(sig, 0.8 * sig, sr)


def noise(sr):
    """Pink-ish noise, fixed seeds, partially correlated channels, RMS ~0.1."""
    n = int(CONTENT_SECONDS * sr)

    def pink(seed):
        spec = np.fft.rfft(np.random.RandomState(seed).randn(n))
        freqs = np.fft.rfftfreq(n, 1.0 / sr)
        spec[1:] /= np.sqrt(freqs[1:])
        spec[0] = 0
        out = np.fft.irfft(spec, n)
        return out / np.sqrt(np.mean(out ** 2)) * 0.1

    a, b = pink(12345), pink(54321)
    return _finish(a, 0.5 * a + 0.5 * b, sr)


def impulses(sr):
    """Single-sample clicks every 250 ms (L 0.8, R 0.4) for timing/latency checks."""
    n = int(CONTENT_SECONDS * sr)
    left = np.zeros(n)
    right = np.zeros(n)
    step = int(0.25 * sr)
    left[step // 2::step] = 0.8
    right[step // 2::step] = 0.4
    return _finish(left, right, sr)


def voice(sr):
    """Harmonic tone with gliding f0 (110->220 Hz) and vibrato, formant-like tilt, light noise.
    R is a slightly delayed, differently coloured copy so that stereo effects have something to do."""
    t = _time(sr)
    f0 = 110.0 * (2.0 ** (t / CONTENT_SECONDS)) * (1.0 + 0.03 * np.sin(2 * np.pi * 5.5 * t))
    phase = 2 * np.pi * np.cumsum(f0) / sr
    rng = np.random.RandomState(777)

    def render(shift, tilt, noise_seed):
        sig = np.zeros_like(t)
        for h in range(1, 21):
            formant = np.exp(-((h * f0 - 800.0) / 600.0) ** 2) + 0.4 * np.exp(-((h * f0 - 2400.0) / 900.0) ** 2)
            sig += (formant + 0.05) / h ** tilt * np.sin(h * phase + shift * h)
        sig += 0.01 * np.random.RandomState(noise_seed).randn(len(t))
        return sig

    left = render(0.0, 0.5, 1)
    right = render(0.4, 1.0, 2)
    del rng
    env = np.minimum(1.0, np.minimum(t / 0.1, (CONTENT_SECONDS - t) / 0.1))
    peak = max(np.abs(left).max(), np.abs(right).max())
    return _finish(0.5 * env * left / peak, 0.5 * env * right / peak, sr)


SIGNALS = {"chord": chord, "sweep": sweep, "noise": noise, "impulses": impulses, "voice": voice}
