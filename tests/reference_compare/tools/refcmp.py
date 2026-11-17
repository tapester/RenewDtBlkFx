#!/usr/bin/env python3
"""Reference comparison driver: legacy DtBlkFx (VST2) vs. JUCE engine.

  refcmp.py gen            --workdir W [--presets legacy/resources/stereo_presets.txt]
  refcmp.py render-engine  --workdir W --engine-bin build/render_engine[.exe]
  refcmp.py render-legacy  --workdir W --host vst2_render_host.exe --dll DtBlkFx.dll      (Windows)
  refcmp.py compare        --workdir W [--tolerances tol.json] [--strict]
  refcmp.py consistency    --workdir W --engine-bin build/render_engine [--block 37]
                           (engine only: host block size independence, NaN/Inf, level sanity)

Work directory layout:  signals/*.wav  cases.json  legacy/<case>.wav  engine/<case>.wav  report.{md,json}
Legacy renders can also be produced in any DAW/host (same input wav + the case's 44 parameters,
no latency compensation, same length as the input) and just dropped into legacy/.
"""
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cases as casemod  # noqa: E402
import metrics  # noqa: E402
import signals  # noqa: E402
import wavio  # noqa: E402

SEVERITY = {"FAIL": 0, "WARN": 1, "NO-REF": 2, "NO-TEST": 2, "ERROR": 0, "PASS": 3}


def _select(items, needle):
    return [c for c in items if not needle or needle in c["id"] or needle == c["group"]]


def _params_arg(case):
    return ",".join(f"{p:.7g}" for p in case["params"])


# ---- gen -------------------------------------------------------------------------------------
def cmd_gen(args):
    os.makedirs(os.path.join(args.workdir, "signals"), exist_ok=True)
    for name, fn in signals.SIGNALS.items():
        wavio.write_wav(os.path.join(args.workdir, "signals", f"{name}.wav"), fn(args.sample_rate), args.sample_rate)
    cases = casemod.build_cases(args.presets, tuple(args.preset_signals.split(",")))
    for case in cases:
        case["sample_rate"] = args.sample_rate
        case["tempo"] = args.tempo
    casemod.save_cases(cases, os.path.join(args.workdir, "cases.json"))
    groups = {}
    for case in cases:
        groups[case["group"]] = groups.get(case["group"], 0) + 1
    print(f"{len(signals.SIGNALS)} signals, {len(cases)} cases: {groups}")


# ---- rendering -------------------------------------------------------------------------------
def _run(commands, jobs, label):
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(subprocess.run, cmd, capture_output=True, text=True): case_id
                   for case_id, cmd in commands}
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            result = future.result()
            if result.returncode != 0:
                failures += 1
                print(f"[{label}] {futures[future]} FAILED: {(result.stderr or result.stdout).strip()[:300]}")
            elif done % 20 == 0 or done == len(futures):
                print(f"[{label}] {done}/{len(futures)}")
    return failures


def cmd_render_engine(args):
    out_dir = os.path.join(args.workdir, "engine")
    os.makedirs(out_dir, exist_ok=True)
    commands = []
    for case in _select(casemod.load_cases(os.path.join(args.workdir, "cases.json")), args.filter):
        commands.append((case["id"], [args.engine_bin,
                                      os.path.join(args.workdir, "signals", case["input"] + ".wav"),
                                      os.path.join(out_dir, case["id"] + ".wav"),
                                      "--params", _params_arg(case), "--tempo", str(case["tempo"]),
                                      "--block", str(args.block)]))
    return 1 if _run(commands, args.jobs, "engine") else 0


def cmd_render_legacy(args):
    out_dir = os.path.join(args.workdir, "legacy")
    os.makedirs(out_dir, exist_ok=True)
    commands = []
    for case in _select(casemod.load_cases(os.path.join(args.workdir, "cases.json")), args.filter):
        commands.append((case["id"], [args.host, args.dll,
                                      os.path.join(args.workdir, "signals", case["input"] + ".wav"),
                                      os.path.join(out_dir, case["id"] + ".wav"),
                                      "--params", _params_arg(case), "--tempo", str(case["tempo"]),
                                      "--block", str(args.block)]))
    # one plugin instance per process; DLL state is not shared, so parallel runs are fine
    return 1 if _run(commands, args.jobs, "legacy") else 0


# ---- compare ---------------------------------------------------------------------------------
def _tolerances(config, case):
    tol = {}
    if config:
        tol.update(config.get("default", {}))
        tol.update(config.get("groups", {}).get(case["group"], {}))
        tol.update(config.get("cases", {}).get(case["id"], {}))
    return {k: tuple(v) for k, v in tol.items()}


def compare_case(case, workdir, config, max_lag_s, forced_lags):
    ref_path = os.path.join(workdir, "legacy", case["id"] + ".wav")
    test_path = os.path.join(workdir, "engine", case["id"] + ".wav")
    row = {"id": case["id"], "group": case["group"], "input": case["input"], "note": case.get("note", "")}
    if not os.path.exists(ref_path):
        return {**row, "status": "NO-REF", "reasons": []}
    if not os.path.exists(test_path):
        return {**row, "status": "NO-TEST", "reasons": []}
    try:
        ref, ref_rate = wavio.read_wav(ref_path)
        test, test_rate = wavio.read_wav(test_path)
        if ref_rate != test_rate:
            return {**row, "status": "ERROR", "reasons": [f"sample rate {ref_rate} != {test_rate}"]}
        m = metrics.compare_signals(ref, test, ref_rate, max_lag_s, forced_lags.get(case["id"]))
    except Exception as exc:  # report and continue with the other cases
        return {**row, "status": "ERROR", "reasons": [str(exc)]}
    status, reasons = metrics.verdict(m, _tolerances(config, case))
    return {**row, "status": status, "reasons": reasons, **m}


def cmd_compare(args):
    all_cases = casemod.load_cases(os.path.join(args.workdir, "cases.json"))
    config = json.load(open(args.tolerances)) if args.tolerances else None
    forced = json.load(open(args.lags)) if args.lags else {}
    rows = [compare_case(c, args.workdir, config, args.max_lag, forced) for c in _select(all_cases, args.filter)]

    counts = {}
    for row in rows:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    with open(os.path.join(args.workdir, "report.json"), "w", encoding="utf-8") as f:
        json.dump({"counts": counts, "rows": rows}, f, indent=1)

    lines = ["# DtBlkFx reference comparison (legacy vs. JUCE engine)", "",
             "Status counts: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())), "",
             "`lag` = samples by which the engine output is later than the legacy output. "
             "Thresholds are uncalibrated starting values.", ""]
    for group in sorted({r["group"] for r in rows}):
        group_rows = sorted((r for r in rows if r["group"] == group),
                            key=lambda r: (SEVERITY.get(r["status"], 9), -r.get("lsd_db", 0)))
        lines += [f"## {group}", "",
                  "| case | status | lag | lsd dB | level dB | env corr | band dB | null dB | notes |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for r in group_rows:
            if "lsd_db" in r:
                lines.append(f"| {r['id']} | {r['status']} | {r['lag_samples']} ({r['lag_method'][0]}) | {r['lsd_db']} | "
                             f"{r['level_diff_db']} | {r['env_corr']} | {r['band_diff_db']} | {r['null_residual_db']} | "
                             f"{'; '.join(r['reasons']) or r['note']} |")
            else:
                lines.append(f"| {r['id']} | {r['status']} | | | | | | | {'; '.join(r['reasons'])} |")
        lines.append("")
    with open(os.path.join(args.workdir, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("status counts:", counts)
    print("report:", os.path.join(args.workdir, "report.md"))
    if args.strict and (counts.get("FAIL", 0) or counts.get("ERROR", 0)):
        return 1
    return 0


def cmd_consistency(args):
    """Render every case with a second host block size and compare with the existing engine/ renders."""
    import numpy as np
    alt_dir = os.path.join(args.workdir, f"engine_block{args.block}")
    os.makedirs(alt_dir, exist_ok=True)
    selected = _select(casemod.load_cases(os.path.join(args.workdir, "cases.json")), args.filter)
    commands = [(c["id"], [args.engine_bin, os.path.join(args.workdir, "signals", c["input"] + ".wav"),
                           os.path.join(alt_dir, c["id"] + ".wav"), "--params", _params_arg(c),
                           "--tempo", str(c["tempo"]), "--block", str(args.block)]) for c in selected]
    _run(commands, args.jobs, "engine-alt")
    problems = 0
    for case in selected:
        base_path = os.path.join(args.workdir, "engine", case["id"] + ".wav")
        alt_path = os.path.join(alt_dir, case["id"] + ".wav")
        if not (os.path.exists(base_path) and os.path.exists(alt_path)):
            print(f"{case['id']}: missing render"); problems += 1; continue
        base, _ = wavio.read_wav(base_path)
        alt, _ = wavio.read_wav(alt_path)
        issues = []
        if not (np.isfinite(base).all() and np.isfinite(alt).all()):
            issues.append("non-finite samples")
        diff = float(np.abs(base - alt).max()) if base.shape == alt.shape else float("inf")
        if diff > 1e-4:
            issues.append(f"block-size dependent (max diff {diff:.3g})")
        if float(np.abs(base).max()) > 8.0:
            issues.append(f"peak {np.abs(base).max():.1f}")
        if issues:
            problems += 1
            print(f"{case['id']}: " + "; ".join(issues))
    print(f"consistency: {len(selected)} cases, {problems} with findings")
    return 1 if problems and args.strict else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    gen = sub.add_parser("gen")
    gen.add_argument("--workdir", required=True)
    gen.add_argument("--presets", help="legacy resources/stereo_presets.txt (adds one case per preset)")
    gen.add_argument("--preset-signals", default="chord", help="comma separated, e.g. chord,voice")
    gen.add_argument("--sample-rate", type=int, default=44100)
    gen.add_argument("--tempo", type=float, default=120.0)
    gen.set_defaults(fn=cmd_gen)

    eng = sub.add_parser("render-engine")
    eng.add_argument("--workdir", required=True)
    eng.add_argument("--engine-bin", required=True)
    eng.add_argument("--block", type=int, default=512)
    eng.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    eng.add_argument("--filter", default="", help="substring of case id or group name")
    eng.set_defaults(fn=cmd_render_engine)

    leg = sub.add_parser("render-legacy")
    leg.add_argument("--workdir", required=True)
    leg.add_argument("--host", required=True)
    leg.add_argument("--dll", required=True)
    leg.add_argument("--block", type=int, default=512)
    leg.add_argument("--jobs", type=int, default=2)
    leg.add_argument("--filter", default="")
    leg.set_defaults(fn=cmd_render_legacy)

    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("--workdir", required=True)
    cmp_.add_argument("--tolerances", help="JSON: {default|groups|cases: {metric: [warn, fail]}}")
    cmp_.add_argument("--lags", help="JSON {case_id: lag_samples} to force the alignment of single cases")
    cmp_.add_argument("--max-lag", type=float, default=2.0, help="seconds searched for the alignment lag")
    cmp_.add_argument("--filter", default="")
    cmp_.add_argument("--strict", action="store_true", help="exit 1 if any case FAILs")
    cmp_.set_defaults(fn=cmd_compare)

    con = sub.add_parser("consistency")
    con.add_argument("--workdir", required=True)
    con.add_argument("--engine-bin", required=True)
    con.add_argument("--block", type=int, default=37)
    con.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    con.add_argument("--filter", default="")
    con.add_argument("--strict", action="store_true")
    con.set_defaults(fn=cmd_consistency)

    args = parser.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
