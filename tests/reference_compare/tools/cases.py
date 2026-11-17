"""Test-case generation. A case = input signal + the complete 44-value legacy parameter vector.

Parameter layout (legacy BlkFxParam.h, identical in the JUCE engine's configureLegacyParameters):
  [0] mixback/power-match  [1] delay  [2] fft length  [3] overlap
  then 8 effect slots x 5 values: freqA, freqB, amplitude, type, value
"""
import json
import re

TOTAL_PARAMS = 44
NUM_SLOTS = 8

EFFECT_NAMES = [
    "Filter", "Contrast", "Smear", "Threshold", "Clip", "Resize", "Resample", "Shift", "ConstShift",
    "None", "None", "HarmShift", "HarmRepitch", "HarmFilt", "AutoHarm", "Triangles", "Squares",
    "Saws", "Pointy", "Sweep", "HarmMask", "AutoHarmMask", "AutoSubharm1Mask", "AutoSubharm2Mask",
    "AutoSubharm3Mask", "ThreshMask", "Vocode", "HarmMatch L>R", "HarmMatch R>L", "CrossMix",
    "WarpMix"]
NONE_TYPE = 9
SHIFT_TYPE = 7


# ---- legacy parameter encodings (see BlkFxParam.h) -------------------------------------------
def enc_type(index):
    return (index * 8 + 4) / 255.0


def enc_fft_len(plan):
    return min(1.0, (plan + 2.0) * (4.0 / 255.0))


def enc_delay_beats(beats):
    return min(0.4999, beats * 16.0 / 255.0)


def enc_delay_msec(msec):
    return min(1.0, 0.5 + 0.5 * min(1.0, msec / 6000.0))


def enc_overlap(part, sync):
    """part 0..1 (0 ~ no overlap, 1 ~ 85 %), sync = block-sync to beat."""
    return min(1.0, max(0.501, (2.0 - part) * 0.5)) if sync else min(0.499, max(0.0, part * 0.5))


def make_params(mixback=0.0, delay=enc_delay_beats(1.0), fft_plan=16, overlap=0.35, slots=None):
    """slots: list of up to 8 dicts {type, a, b, amp, val}; unused slots are 'None'."""
    params = [mixback, delay, enc_fft_len(fft_plan), overlap]
    slots = list(slots or [])
    for index in range(NUM_SLOTS):
        slot = slots[index] if index < len(slots) else {"type": NONE_TYPE}
        params += [slot.get("a", 0.0), slot.get("b", 1.0), slot.get("amp", 0.6),
                   enc_type(slot["type"]), slot.get("val", 0.5)]
    assert len(params) == TOTAL_PARAMS
    return [round(float(p), 7) for p in params]


# ---- preset import ---------------------------------------------------------------------------
def parse_presets(path):
    """Parse '<name>:<p0> <p1> ...' lines. Missing trailing values are 0, as in legacy VstProgram."""
    presets = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("//") or ":" not in line:
                continue
            name, _, rest = line.partition(":")
            if name.startswith(">"):
                continue
            try:
                values = [float(tok) for tok in re.split(r"[\s,]+", rest) if tok]
            except ValueError:
                continue
            values = (values + [0.0] * TOTAL_PARAMS)[:TOTAL_PARAMS]
            presets.append((name.strip(), values))
    return presets


def slug(text):
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_").lower()


# ---- case construction -----------------------------------------------------------------------
def _case(case_id, group, signal, params, sample_rate=44100, tempo=120.0, note=""):
    return {"id": case_id, "group": group, "input": signal, "sample_rate": sample_rate,
            "tempo": tempo, "params": params, "note": note}


def _signal_for(effect_type):
    if effect_type <= 4 or 15 <= effect_type <= 19:
        return "noise"
    if 11 <= effect_type <= 14 or effect_type >= 20:
        return "voice"
    return "chord"


def build_cases(preset_file=None, preset_signals=("chord",)):
    cases = []

    # 1) every legacy factory preset (real-world parameter combinations)
    if preset_file:
        for number, (name, values) in enumerate(parse_presets(preset_file)):
            for signal in preset_signals:
                cases.append(_case(f"preset_{number:02d}_{slug(name)}_{signal}", "preset", signal,
                                   values, note=name))

    # 2) every effect type alone, three values; mask types get a Filter behind them
    for effect_type, name in enumerate(EFFECT_NAMES):
        for value in (0.25, 0.5, 0.75):
            slots = [{"type": effect_type, "a": 0.0, "b": 1.0, "amp": 0.6, "val": value}]
            if 20 <= effect_type <= 25:
                slots.append({"type": 0, "a": 0.0, "b": 1.0, "amp": 0.3, "val": 0.0})
            cases.append(_case(f"fx_{effect_type:02d}_{slug(name)}_v{int(value * 100):02d}",
                               "fx_single", _signal_for(effect_type), make_params(slots=slots),
                               note=f"{name} value={value}"))

    # 3) timing / latency: identity effect on clicks; delay units and block-vs-delay interplay
    timing = [("beats1", enc_delay_beats(1.0)), ("beats0_25", enc_delay_beats(0.25)),
              ("msec50", enc_delay_msec(50.0)), ("msec500", enc_delay_msec(500.0)),
              ("min_delay", enc_delay_msec(0.0))]
    for plan in (8, 16, 24):
        for label, delay in timing:
            cases.append(_case(f"timing_plan{plan:02d}_{label}", "timing", "impulses",
                               make_params(delay=delay, fft_plan=plan),
                               note="delay shorter than block: legacy shrinks the block, JUCE port does not"
                               if label in ("beats0_25", "msec50", "min_delay") and plan >= 16 else ""))

    # 4) block length / overlap / mixback sweeps with a frequency shift
    shift = [{"type": SHIFT_TYPE, "a": 0.0, "b": 1.0, "amp": 0.6, "val": 0.6}]
    for plan in (4, 8, 12, 16, 20, 24, 28):
        cases.append(_case(f"fft_plan{plan:02d}_shift", "fft_len", "chord",
                           make_params(fft_plan=plan, slots=shift)))
    for label, overlap in (("none", enc_overlap(0.0, False)), ("raw035", 0.35),
                           ("max", enc_overlap(0.99, False)), ("sync_half", enc_overlap(0.5, True)),
                           ("sync_max", 1.0)):
        cases.append(_case(f"overlap_{label}_shift", "overlap", "chord",
                           make_params(overlap=overlap, slots=shift)))
    for mixback in (0.0, 0.25, 0.5, 0.75, 1.0):
        cases.append(_case(f"mixback_{int(mixback * 100):03d}_shift", "mixback", "chord",
                           make_params(mixback=mixback, slots=shift)))
    return cases


def save_cases(cases, path):
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"version": 1, "cases": cases}, f, indent=1)


def load_cases(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)["cases"]
