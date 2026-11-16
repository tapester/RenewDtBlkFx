"""Alignment and perceptually motivated comparison metrics between a reference (legacy) render and a
test (JUCE engine) render.

Why not just a null test: the port deliberately differs in details (fractional-shift kernel,
latency bookkeeping), and many effects are phase-sensitive. A bit-exact null is reported, but the
verdict rests on spectral and envelope similarity. All thresholds are starting points and meant to
be calibrated once the first real reference renders exist (see README).
"""
import numpy as np

EPS = 1e-12

# (warn, fail) thresholds. Starting values, NOT validated against real legacy output.
DEFAULT_TOLERANCES = {
    "lsd_db": (3.0, 6.0),            # log-spectral distance, lower is better
    "level_diff_db": (1.0, 3.0),     # |RMS(test) - RMS(ref)|
    "env_corr": (0.90, 0.70),        # 10 ms envelope correlation, higher is better
    "band_diff_db": (3.0, 8.0),      # max |octave-band energy difference|
}


def _db(x):
    return 20.0 * np.log10(np.maximum(x, EPS))


def _mono(x):
    return x.mean(axis=1)


def _xcorr_peak(a, b, max_lag):
    """Lag k (test delayed by k samples vs ref), normalised peak, peak/mean confidence."""
    n = 1
    while n < len(a) + len(b):
        n <<= 1
    fa, fb = np.fft.rfft(a, n), np.fft.rfft(b, n)
    corr = np.fft.irfft(np.conj(fa) * fb, n)  # corr[k] = sum_n a[n] * b[n+k]
    window = np.concatenate([corr[-max_lag:], corr[:max_lag + 1]]) if max_lag > 0 else corr[:1]
    lags = np.arange(-max_lag, max_lag + 1)
    idx = int(np.argmax(window))
    norm = np.sqrt(np.sum(a ** 2) * np.sum(b ** 2)) + EPS
    return int(lags[idx]), float(window[idx] / norm), float(window[idx] / (np.mean(np.abs(window)) + EPS))


def _envelope(x, sr):
    win = max(1, int(0.005 * sr))
    env = np.convolve(np.abs(x), np.ones(win) / win, mode="same")
    return env - env.mean()


def estimate_lag(ref, test, sr, max_lag_s=2.0):
    """Waveform correlation if it is convincing (normalised peak >= 0.3), else envelope correlation."""
    max_lag = int(max_lag_s * sr)
    r, t = _mono(ref), _mono(test)
    lag, norm_peak, conf = _xcorr_peak(r, t, max_lag)
    method = "waveform"
    if norm_peak < 0.3:
        lag, norm_peak, conf = _xcorr_peak(_envelope(r, sr), _envelope(t, sr), max_lag)
        method = "envelope"
    return {"lag_samples": lag, "lag_method": method, "lag_peak": round(norm_peak, 4),
            "lag_confidence": round(conf, 2)}


def apply_lag(ref, test, lag):
    """lag > 0: test is later than ref by lag samples -> drop the first lag samples of test."""
    if lag >= 0:
        test = test[lag:]
    else:
        ref = ref[-lag:]
    n = min(len(ref), len(test))
    return ref[:n], test[:n]


def _stft_mag(x, n_fft=2048, hop=512):
    """Amplitude-normalised magnitude (a full-scale sine peaks near 0 dB). Returns [frames, bins]."""
    if len(x) < n_fft:
        x = np.pad(x, (0, n_fft - len(x)))
    window = np.hanning(n_fft)
    count = 1 + (len(x) - n_fft) // hop
    idx = np.arange(n_fft)[None, :] + hop * np.arange(count)[:, None]
    return np.abs(np.fft.rfft(x[idx] * window, axis=1)) / (n_fft / 4.0)


def compare_signals(ref, test, sr, max_lag_s=2.0, forced_lag=None):
    """ref/test: float arrays [n, channels]. Returns a dict of metrics (JSON serialisable)."""
    channels = min(ref.shape[1], test.shape[1])
    ref, test = ref[:, :channels], test[:, :channels]
    info = {"length_ref": int(len(ref)), "length_test": int(len(test))}

    if forced_lag is None:
        info.update(estimate_lag(ref, test, sr, max_lag_s))
    else:
        info.update({"lag_samples": int(forced_lag), "lag_method": "forced"})
    ref, test = apply_lag(ref, test, info["lag_samples"])

    rms_ref = float(np.sqrt(np.mean(ref ** 2)))
    rms_test = float(np.sqrt(np.mean(test ** 2)))
    info["rms_ref_db"] = round(float(_db(rms_ref)), 2)
    info["rms_test_db"] = round(float(_db(rms_test)), 2)
    info["level_diff_db"] = round(float(_db(rms_test) - _db(rms_ref)), 2)
    info["peak_diff_db"] = round(float(_db(np.abs(test).max()) - _db(np.abs(ref).max())), 2)
    info["null_residual_db"] = round(float(_db(np.sqrt(np.mean((ref - test) ** 2)) / (rms_ref + EPS))), 2)

    # envelope correlation (10 ms RMS)
    win = max(1, int(0.010 * sr))
    frames = len(ref) // win
    if frames >= 2:
        er = np.sqrt(np.mean(_mono(ref)[:frames * win].reshape(frames, win) ** 2, axis=1))
        et = np.sqrt(np.mean(_mono(test)[:frames * win].reshape(frames, win) ** 2, axis=1))
        if er.std() > 0 and et.std() > 0:
            env_corr = float(np.corrcoef(er, et)[0, 1])
        else:
            env_corr = 1.0 if np.allclose(er, et) else 0.0
        info["env_corr"] = round(env_corr, 4)
    else:
        info["env_corr"] = 0.0

    # STFT based metrics, averaged over channels
    floor_db = -90.0
    lsd, centroid, band_rows = [], [], []
    for ch in range(channels):
        mr, mt = _stft_mag(ref[:, ch]), _stft_mag(test[:, ch])
        rdb, tdb = np.maximum(_db(mr), floor_db), np.maximum(_db(mt), floor_db)
        frame_level = np.maximum(mr.max(axis=1), mt.max(axis=1))
        active = _db(frame_level) > -70.0
        if active.any():
            bins = np.maximum(rdb, tdb) > floor_db + 1.0
            diff2 = np.where(bins, (rdb - tdb) ** 2, 0.0)
            per_frame = np.sqrt(diff2.sum(axis=1) / np.maximum(bins.sum(axis=1), 1))
            lsd.append(float(per_frame[active].mean()))
            freqs = np.fft.rfftfreq(2048, 1.0 / sr)
            cr = (mr * freqs).sum(axis=1) / (mr.sum(axis=1) + EPS)
            ct = (mt * freqs).sum(axis=1) / (mt.sum(axis=1) + EPS)
            centroid.append(float(np.abs(cr - ct)[active].mean()))
        pr, pt = (mr ** 2).mean(axis=0), (mt ** 2).mean(axis=0)
        freqs = np.fft.rfftfreq(2048, 1.0 / sr)
        edges = 31.25 * 2.0 ** np.arange(0, 10)
        for low, high in zip(edges[:-1], edges[1:]):
            sel = (freqs >= low) & (freqs < high)
            if sel.any() and 10 * np.log10(pr[sel].sum() + EPS) > -80.0:
                band_rows.append(abs(10 * np.log10(pr[sel].sum() + EPS) - 10 * np.log10(pt[sel].sum() + EPS)))
    info["lsd_db"] = round(float(np.mean(lsd)), 3) if lsd else 0.0
    info["centroid_diff_hz"] = round(float(np.mean(centroid)), 1) if centroid else 0.0
    info["band_diff_db"] = round(float(max(band_rows)), 2) if band_rows else 0.0

    if channels == 2 and rms_ref > 0 and rms_test > 0:
        def lr(x):
            return float(np.corrcoef(x[:, 0], x[:, 1])[0, 1]) if x[:, 0].std() > 0 and x[:, 1].std() > 0 else 0.0
        info["lr_corr_diff"] = round(lr(test) - lr(ref), 3)
    return info


def verdict(metrics, tolerances=None):
    """PASS / WARN / FAIL plus the list of violated metrics."""
    tol = dict(DEFAULT_TOLERANCES)
    tol.update(tolerances or {})
    status, reasons = "PASS", []
    for name, (warn, fail) in tol.items():
        value = metrics.get(name)
        if value is None:
            continue
        if name == "level_diff_db":
            value = abs(value)
        higher_is_better = name == "env_corr"
        bad = value < fail if higher_is_better else value > fail
        warned = value < warn if higher_is_better else value > warn
        if bad:
            status = "FAIL"
            reasons.append(f"{name}={value}")
        elif warned:
            if status != "FAIL":
                status = "WARN"
            reasons.append(f"{name}={value}")
    if metrics.get("rms_ref_db", 0) < -90 and metrics.get("rms_test_db", 0) < -90:
        status, reasons = "WARN", ["both outputs silent"]
    return status, reasons
