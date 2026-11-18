# AGENTS.md

Guidance for AI coding agents working in this repository.

## Project

RenewDtBlkFx modernizes the DtBlkFx FFT-based audio effects plug-in (originally a VST2 plug-in,
forked from https://codeberg.org/skullzy/DtBlkFx). The goal is a natively buildable, multi-platform
JUCE plug-in (VST3, CLAP) that keeps the legacy plug-in's **functionality**.

- `main`: the JUCE port. All work happens here.
- `legacy`: the original VST2 code. **Read-only reference.** Never commit to it, never patch it,
  never paste its code into `main` wholesale (port deliberately into the JUCE structure). Use it to
  compare behavior.

## Layout

| Path | Purpose |
|---|---|
| `src/DtBlkFxEngine.*` | DSP engine (FFTW3f). No JUCE dependency. |
| `src/DtBlkFxLegacyMapping.h`, `src/DtBlkFxHostTiming.h` | Header-only helpers, JUCE-free so engine tests can use them |
| `src/DtBlkFxProcessor.*`, `src/DtBlkFxEditor.*` | JUCE processor (APVTS, scheduler) and editor |
| `src/DtBlkFxSweepTables.*` | Precomputed sweep envelope data; do not edit without asking |
| `tests/*.cpp` | CTest executables; diagnostics (`FftSwitchDiagnostic`, `EffectComparison`, `EffectProbe`) are standalone and not run by CTest |
| `tests/reference_compare/` | Legacy-vs-engine comparison harness (Python tooling + renderers) |
| `modules/` | Submodules: JUCE, vcpkg, clap-juce-extensions |

Keep JUCE out of the engine and the header-only helpers. New DSP logic belongs in the engine so it
stays testable without JUCE.

## Build and test

Build with CMake presets (see `readme.md` for prerequisites): `windows-x64`, `macos-arm64`,
`macos-x64`, and their `*-tests` variants. Dependencies come from vcpkg manifest mode
(`vcpkg.json`, FFTW3); do not run `vcpkg install` manually.

### What agents may do

- **Allowed without asking:** run `ctest` against an already existing test build directory
  (e.g. `ctest --test-dir build/macos-arm64-tests -C Release --output-on-failure`).
- **Ask first:** configuring or building (any preset: vcpkg/JUCE builds are slow), bootstrapping
  vcpkg, and any submodule init/update.
- **Do not run** the Python tooling under `tests/reference_compare/tools/` (including
  `selftest.py`) unless explicitly asked.
- If no test build exists, say so and ask rather than building.
- `ctest` only counts after a build of the current sources. If you changed code and were not
  approved to build, report that tests were **not run**; never report results from stale binaries.

CI (`.github/workflows/build-plugins.yml`) builds on Windows and macOS arm64, runs ctest, then
validates the VST3 with pluginval 1.0.4 at strictness level 5 (`--skip-gui-tests`). Don't make
changes that would break these steps.

## Hard constraints (do not change without asking)

1. **Legacy 44-parameter layout.** The order and IDs of the 44 legacy parameters (including the
   per-slot ID scheme built by `fxParameterID`, e.g. `freq_a`, `type`), the effect-type
   encoding (`DtBlkFxLegacy::effectTypeToLegacyValue` / `legacyValueToEffectType`), and the order of
   the FFT plan table (`fftSizeCount`, indexed by the legacy format) must stay stable. Host
   automation and saved state depend on them. Existing state/parameter tests must keep passing.
2. **Audio-thread safety.** No allocation, locks, file/console I/O, or exceptions in
   `processBlock` or anything it calls. Prepare FFT plans and buffers in `prepareToPlay`.
3. **Static FFTW on Windows.** The default `windows-x64` preset uses the `x64-windows-static-md`
   triplet so the plug-in has no FFTW DLL dependency (pluginval does not search the plug-in
   folder). CI fails if an FFTW import appears. Do not change triplets or link mode.
4. **Submodules and licensing.** Do not bump the JUCE, vcpkg or clap-juce-extensions pins or the
   vcpkg baseline. Do not edit GPLv3 file headers, `LICENSE.md`, `COPYING` or the author list.
   New source files get the same GPLv3 header as existing ones.

## Behavior parity with legacy

The target is **functional parity**: the same features, parameters and sound character as the
legacy plug-in. Small numerical deviations are acceptable (e.g. the five-bin Lagrange fractional
shift kernel, latency/delay accounting), but must be:

- intentional, not accidental,
- noted in the code comment or the relevant doc where the deviation lives.

Do not "fix" an effect by guessing from `readme.md`; the README's migration status is
descriptive and changes often. Derive behavior from the legacy code on the `legacy` branch.

## Reference-compare harness

`tests/reference_compare/` renders the same signals through the legacy VST2 plug-in and the
engine and scores the difference. Current state: tolerances are **uncalibrated starting values**
and the VST2 host (`vst2_render_host.cpp`) is **untested**.

- **Never loosen a threshold, widen a tolerance, or add an override to make a case pass.**
  Report the deviation instead.
- Threshold changes need explicit approval from the maintainer and a note naming the known
  deviation that justifies them.
- Do not treat a `PASS`/`NO-REF` result as evidence of parity.

## Code style

- C++17. Format with the repo `.clang-format` (LLVM base, 100 columns, Stroustrup braces,
  pointer alignment left). Do not reformat untouched code.
- Match the surrounding code: comments explain *why* (legacy compatibility, rounding, scheduling),
  not what.
- Add or extend a test in `tests/` for every behavior change to the engine, scheduler or
  parameter handling. New tests must be registered with CTest in `CMakeLists.txt`.

## Language

Everything is English: code, comments, docs, commit messages and PR text.
`tests/reference_compare/README.md` is currently German; translate it to English when you edit it.
It also contains unresolved merge-conflict markers (`<<<<<<<` / `>>>>>>>`) that must be resolved
when the file is touched.

## Git

- Work on `main` (or a feature branch from it), never on `legacy`.
- Small, focused commits with imperative subject lines.