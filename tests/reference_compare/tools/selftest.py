#!/usr/bin/env python3
"""Self-test of the comparison pipeline (no legacy plugin required).

Renders a few cases with the JUCE engine and fabricates 'legacy' references from them:
  A) identical copy                 -> must be PASS, lag 0, null residual very low
  B) delayed by 1234 samples + 3 dB -> lag must be recovered exactly, level diff must be -3 dB
  C) different effect parameter     -> must NOT be PASS (the comparator can see sound differences)
  D) missing reference              -> NO-REF
usage: selftest.py --engine-bin build/render_engine [--workdir DIR]
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cases as casemod  # noqa: E402
import metrics  # noqa: E402
import refcmp  # noqa: E402
import signals  # noqa: E402
import wavio  # noqa: E402


def render(engine_bin, workdir, signal, params, out_path):
    subprocess.run([engine_bin, os.path.join(workdir, "signals", signal + ".wav"), out_path,
                    "--params", ",".join(f"{p:.7g}" for p in params), "--tempo", "120"],
                   check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-bin", required=True)
    parser.add_argument("--workdir", default=None)
    args = parser.parse_args()
    workdir = args.workdir or tempfile.mkdtemp(prefix="refcmp_selftest_")
    for sub in ("signals", "legacy", "engine"):
        os.makedirs(os.path.join(workdir, sub), exist_ok=True)
    for name in ("chord", "impulses"):
        wavio.write_wav(os.path.join(workdir, "signals", name + ".wav"), signals.SIGNALS[name](44100), 44100)

    shift = lambda value: [{"type": casemod.SHIFT_TYPE, "a": 0.0, "b": 1.0, "amp": 0.6, "val": value}]
    base = casemod.make_params(fft_plan=12, slots=shift(0.6))
    other = casemod.make_params(fft_plan=12, slots=shift(0.8))
    cases = [casemod._case(f"selftest_{tag}", "selftest", "chord", base) for tag in "ABCD"]
    casemod.save_cases(cases, os.path.join(workdir, "cases.json"))

    engine_wav = {tag: os.path.join(workdir, "engine", f"selftest_{tag}.wav") for tag in "ABCD"}
    for tag in "ABCD":
        render(args.engine_bin, workdir, "chord", base, engine_wav[tag])
    audio, rate = wavio.read_wav(engine_wav["A"])

    legacy = lambda tag: os.path.join(workdir, "legacy", f"selftest_{tag}.wav")
    wavio.write_wav(legacy("A"), audio, rate)
    delayed = np.concatenate([np.zeros((1234, audio.shape[1]), np.float32), audio])[:len(audio)]
    wavio.write_wav(legacy("B"), delayed * 10 ** (3 / 20), rate)
    render(args.engine_bin, workdir, "chord", other, legacy("C"))
    # D: no legacy file on purpose

    results = {c["id"]: refcmp.compare_case(c, workdir, None, 2.0, {}) for c in cases}
    failures = []

    def check(condition, text):
        print(("ok   " if condition else "FAIL ") + text)
        if not condition:
            failures.append(text)

    a, b, c, d = (results[f"selftest_{t}"] for t in "ABCD")
    check(a["status"] == "PASS" and a["lag_samples"] == 0, f"A identical -> PASS, lag 0 (got {a['status']}, lag {a.get('lag_samples')})")
    check(a["null_residual_db"] < -100, f"A null residual < -100 dB (got {a['null_residual_db']})")
    check(b.get("lag_samples") == -1234, f"B lag recovered exactly (got {b.get('lag_samples')}, method {b.get('lag_method')})")
    check(abs(b.get("level_diff_db", 99) + 3.0) < 0.1, f"B level difference -3 dB (got {b.get('level_diff_db')})")
    check(c["status"] != "PASS", f"C different parameter is detected (status {c['status']}, lsd {c.get('lsd_db')}, "
                                  f"band {c.get('band_diff_db')}, env {c.get('env_corr')})")
    check(d["status"] == "NO-REF", f"D missing reference -> NO-REF (got {d['status']})")

    # alignment on phase-scrambling output: envelope fallback on clicks, delayed by 700 samples
    clicks, rate = wavio.read_wav(os.path.join(workdir, "signals", "impulses.wav"))
    delayed_clicks = np.concatenate([np.zeros((700, 2), np.float32), clicks])[:len(clicks)]
    info = metrics.estimate_lag(clicks, delayed_clicks, rate)
    check(info["lag_samples"] == 700, f"impulse train lag 700 (got {info})")

    print("workdir:", workdir)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
