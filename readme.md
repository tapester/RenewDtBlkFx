# DtBlkFx Audio FX Plug-in

DtBlkFx is an FFT-based audio effects plug-in (VST3 and CLAP).

Forked from https://codeberg.org/skullzy/DtBlkFx. The original VST2 code is in the
[`legacy` branch](https://github.com/tapester/RenewDtBlkFx/tree/legacy).

## Usage examples

- Precision parametric equalizing with sharp roll-off
  - Set frequencies accurately enough to adjust individual harmonics.
  - Frequency resolutions of up to 0.7 Hz.
- Harmonic-based (or comb) filtering
  - Set a fundamental frequency and adjust the level of it and its harmonics.
  - Remove the pitched component of a voice.
  - Active harmonic tracking automatically tracks a sound and adjusts
    the level of its harmonics.
- Various types of noise control
  - Change the contrast between loud and soft frequencies.
  - Adjust only frequencies below or above a particular threshold.
  - Clip frequencies above a particular threshold.
  - Sound smearing (phase randomizing).
- Frequency shifting
  - Harmonic shifting by a fixed number of notes.
  - Non-harmonic shifting by a fixed frequency.
  - Active harmonic repitch monitors the pitch and shifts it to a
    destination note or matches it to another channel.
- Various methods of mixing left and right channels
  - Standard vocoding (frequency enveloping).
  - Harmonic-based vocoding.
  - Convolution-like mixing.
  - Two additional mixing algorithms.
- Frequency masking
  - A harmonic or threshold mask may be set for any effect apart from
    vocoding. For example, only shift frequencies below a threshold.

You can select up to eight effects to run in series. Combining effects
allows you to create new and surprising sounds.

## Build

The project uses CMake, Ninja, JUCE, clap-juce-extensions and vcpkg.

| Platform | Architecture | Preset |
|---|---|---|
| Windows | x64 (static FFTW) | `windows-x64` |
| Windows | x64, dynamic FFTW | `windows-x64-dynamic-fftw` |
| macOS | Apple Silicon (arm64) | `macos-arm64` |
| macOS | Intel (x86_64) | `macos-x64` |

The release presets build the plug-in only. The `*-tests` presets
(`windows-x64-tests`, `macos-arm64-tests`, `macos-x64-tests`) also build the
test executables.

### Prerequisites

- Git with submodule support, CMake 3.22 or newer, Ninja, a C++17 compiler
- Windows: Visual Studio 2022 with the Desktop development with C++
  workload and the Windows SDK. Run the commands from a Developer
  PowerShell for VS 2022.
- macOS: Xcode Command Line Tools (`xcode-select --install`), then
  `brew install cmake ninja git`

### Get the sources and bootstrap vcpkg

```bash
git clone --recurse-submodules https://github.com/tapester/RenewDtBlkFx.git
cd RenewDtBlkFx
# If you cloned without submodules:
git submodule update --init --recursive
```

Bootstrap the included vcpkg checkout (once):

```bash
./modules/vcpkg/bootstrap-vcpkg.sh        # macOS
.\modules\vcpkg\bootstrap-vcpkg.bat       # Windows (PowerShell)
```

vcpkg runs in manifest mode: `vcpkg.json` declares FFTW3 and pins the
baseline, and CMake installs it during configuration. Do not run
`vcpkg install` manually.

### Build and test

Replace `<preset>` with a preset from the table:

```bash
cmake --preset <preset>
cmake --build --preset <preset> --parallel
```

Output goes to `build/<preset>`. To build and run the tests, use the
matching test preset:

```bash
cmake --preset macos-arm64-tests
cmake --build --preset macos-arm64-tests --parallel
ctest --preset macos-arm64-tests
```

CTest runs all registered tests, not just the engine tests.

The DSP diagnostic applications (`DtBlkFxFftSwitchDiagnostic`,
`DtBlkFxEffectComparison`, `DtBlkFxEffectProbe`) are standalone tools, not
run by CTest. Build them with `-DDTBLKFX_BUILD_DIAGNOSTICS=ON` on a test
preset.

### Windows: static and dynamic FFTW

`windows-x64` links FFTW statically, so the plug-in needs no `fftw3f.dll`.
It still needs the Microsoft Visual C++ 2015-2022 Redistributable.

`windows-x64-dynamic-fftw` stages `fftw3f.dll` beside each plug-in binary.
Install the complete `RenewDtBlkFx.vst3` bundle, including
`Contents/x86_64-win/fftw3f.dll`. Some hosts, including pluginval, do not
search the plug-in folder for DLLs and fail to load this variant, so prefer
the static build.

### Install and validate

Find the built plug-ins:

```bash
find build/macos-arm64 \( -name "*.vst3" -o -name "*.clap" \)   # macOS
```

```powershell
Get-ChildItem -Path build\windows-x64 -Recurse -Include *.vst3,*.clap   # Windows
```

Copy the complete bundle, not just the executable inside it. On macOS the
user-level directories are `~/Library/Audio/Plug-Ins/VST3` and
`~/Library/Audio/Plug-Ins/CLAP`:

```bash
ditto "path/to/RenewDtBlkFx.vst3" "$HOME/Library/Audio/Plug-Ins/VST3/RenewDtBlkFx.vst3"
```

Restart the DAW or rescan its plug-in folders. To check the architecture of
a macOS build, run `file` on the executable in `Contents/MacOS`.

Validate with [pluginval](https://github.com/Tracktion/pluginval) (CI uses
1.0.4):

```bash
pluginval --strictness-level 5 --skip-gui-tests path/to/RenewDtBlkFx.vst3
```

A successful pluginval run does not prove that every effect or host
interaction works; also test manually in a DAW.

## Behavior notes

- **Parameters:** The 44 legacy parameter values drive the engine. They are
  interpolated by sample position, using the legacy quarter-beat transition
  distance. The host delivers parameter values per block, so automation
  inside a block is not sample-accurate. The opt-in Parameter Sync control
  aligns parameter events to blocks; it is off by default for
  legacy-compatible behavior.
- **Latency:** The Latency parameter sets the fixed delay the host
  compensates for, and Block Size is limited to what fits into it. The
  configurable output Delay is added on top and is not reported to the host.
- **Block-size independent:** The engine waits for a full FFT frame, so its
  output does not depend on the host's block size. The legacy VST2 plug-in
  could force partial frames at callback boundaries.
- **Effects:** All 29 effect types are implemented; the two legacy `None`
  choices stay bypasses. Mask effects constrain the next slot. Stereo
  effects (Vocode, HarmMatch L>R/R>L, CrossMix, WarpMix) work jointly on
  both channels.

### Known deviations from legacy

Functional parity is the goal; these differences are known and intended or
not yet verified:

- Frequency remappers (Resize, Resample, Shift, ConstShift, harmonic
  shifts) use a normalized five-bin Lagrange kernel for fractional bin
  shifts. Not yet verified against the legacy kernel.
- CrossMix derives the phase from the weighted sum of the two bins instead
  of the legacy averaged angles. Averaging wrapped angles biases the phase
  towards 0 and creates an impulse at the frame start (about 0.54 dB at mid
  mix). This is a deliberate sound deviation.
- WarpMix follows the legacy `WeirdMixProcess`, but its bin-shift phase
  correction uses exact sine/cosine instead of the legacy 4096-entry table,
  and its level relies on Match mode (Power control) as in legacy.
- Fundamental tracking and the stereo effects still need comparison against
  legacy reference renders. `tests/reference_compare` renders both
  implementations from the same 44 parameters for this purpose.
- The live spectrogram has not been ported.

### CPU load with large block sizes

The engine computes a whole FFT frame in one burst when the frame is
complete, so CPU load arrives in spikes, not evenly. A spike grows with the
Block Size and the effects used. Indicative longest times for one audio
call with 8 effects, stereo, 48 kHz, measured on a 2-core cloud VM with a
512-sample host buffer (10.7 ms available; your hardware will differ):

| Block Size | Longest call |
|---|---|
| 4096 | 3.5 ms (99% of calls under 1 ms) |
| 16384 | 4 ms |
| 80640 | 18 ms, over the 10.7 ms available |

With a 64-sample buffer (1.3 ms available), 80640 took up to 24 ms. At 80640 an empty slot costs about 3.4 ms. Resample and ConstShift add about
11 ms each, Shift about 6 ms and WarpMix about 4 ms; the other effects add
0-3 ms. If you hear dropouts, use a smaller Block Size, fewer heavy effects,
or a larger host buffer.

## Remaining porting activities

1. **Validate scheduler and automation compatibility:** compare startup,
   latency and delay transitions against legacy renders. Add a
   callback-boundary compatibility mode only if reference behavior requires
   it, and sample-accurate automation only if a supported JUCE format path
   exposes event timestamps.
2. **Reduce frame-time spikes:** see "CPU load with large block sizes".
   Options are a worker thread or time slicing within spare latency.
3. **Port the live visualization:** publish input/output spectra to the
   editor safely, independent of the editor's lifetime.
4. **Complete release validation:** verify preset/state migration, transport
   and sample-rate changes, channel layouts, and latency/tail reporting;
   expand DSP regressions with edge frequencies, zero/full amplitude and
   legacy reference renders; run pluginval and host smoke tests for both
   plug-in formats.