
# DtBlkFx Audio FX Plug-in

DtBlkFx is an FFT-based audio effects plug-in.

Forked from https://codeberg.org/skullzy/DtBlkFx

## Build

The project uses CMake, Ninja, JUCE, clap-juce-extensions and vcpkg.

Supported build configurations:

| Platform | Architecture | Preset |
|---|---|---|
| Windows | x64 | `windows-x64` |
| Windows | x64, dynamic FFTW | `windows-x64-dynamic-fftw` |
| macOS | Apple Silicon (arm64) | `macos-arm64` |
| macOS | Intel (x86_64) | `macos-x64` |

The normal release presets build the plug-in without the automated test
executables. Dedicated `*-tests` presets build the plug-in and the test
executables.

### Prerequisites

#### All platforms

- Git, including submodule support
- CMake 3.22 or newer
- Ninja
- A supported C++17 compiler

#### Windows

- Visual Studio 2022 with the Desktop development with C++ workload
- Windows SDK

Run the commands below from a Developer PowerShell for VS 2022.

#### macOS

- Xcode Command Line Tools
- CMake
- Ninja
- Git

Install the command-line tools:

```bash
xcode-select --install
```

Install the remaining tools with Homebrew:

```bash
brew install cmake ninja git
```

### Clone and initialize dependencies

Clone the repository including its submodules:

```bash
git clone --recurse-submodules \
    https://github.com/tapester/RenewDtBlkFx.git

cd RenewDtBlkFx
```

If you already cloned the repository without submodules, initialize them:

```bash
git submodule update --init --recursive
```

Bootstrap the included vcpkg checkout:

On Windows:

```powershell
.\modules\vcpkg\bootstrap-vcpkg.bat
```

On macOS:

```bash
./modules/vcpkg/bootstrap-vcpkg.sh
```

The project uses vcpkg manifest mode. The root `vcpkg.json` declares
FFTW3 and pins the vcpkg baseline. CMake installs the required dependency
during configuration through the vcpkg toolchain.

You normally do not need to run `vcpkg install` manually.

### Windows x64: release build

Run from a Developer PowerShell for VS 2022:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
```

This is the default Windows build. FFTW is statically linked into the
plug-in, so `fftw3f.dll` does not need to be installed beside it.

The plug-in still requires the Microsoft Visual C++ 2015-2022
Redistributable for the matching architecture.

### Windows x64: dynamic FFTW build

A variant that links FFTW dynamically is available:

```powershell
cmake --preset windows-x64-dynamic-fftw
cmake --build --preset windows-x64-dynamic-fftw --parallel
```

Its plug-in is output under:

```text
build/windows-x64-dynamic-fftw
```

`fftw3f.dll` is staged beside each plug-in binary. When installing this
variant manually, copy the complete `DtBlkFx.vst3` bundle, including
`Contents/x86_64-win/fftw3f.dll`, into the VST3 folder.

Some hosts, including pluginval, do not search the plug-in folder for
dependent DLLs and may fail to load this variant. Prefer the default
static build.

### macOS Apple Silicon: release build

For M1, M2, M3 and later Apple Silicon Macs:

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --parallel
```

The output is generated under:

```text
build/macos-arm64
```

### macOS Intel: release build

For Intel Macs:

```bash
cmake --preset macos-x64
cmake --build --preset macos-x64 --parallel
```

The output is generated under:

```text
build/macos-x64
```

### Build with automated tests

Use the dedicated test presets to compile the plug-in and all registered
CTest executables.

Windows:

```powershell
cmake --preset windows-x64-tests
cmake --build --preset windows-x64-tests --parallel
ctest --preset windows-x64-tests
```

macOS Apple Silicon:

```bash
cmake --preset macos-arm64-tests
cmake --build --preset macos-arm64-tests --parallel
ctest --preset macos-arm64-tests
```

macOS Intel:

```bash
cmake --preset macos-x64-tests
cmake --build --preset macos-x64-tests --parallel
ctest --preset macos-x64-tests
```

CTest prints details for failing tests. The test presets run up to four
tests concurrently.

To run the tests manually without a test preset, use:

```bash
ctest --test-dir build/macos-arm64-tests \
    -C Release \
    --output-on-failure
```

### Build DSP diagnostic applications

The DSP diagnostic applications are separate from the automated tests.
They are not built by default.

To build them, enable `DTBLKFX_BUILD_DIAGNOSTICS`:

```bash
cmake --preset macos-arm64-tests \
    -DDTBLKFX_BUILD_DIAGNOSTICS=ON

cmake --build --preset macos-arm64-tests --parallel
```

This builds:

- `DtBlkFxFftSwitchDiagnostic`
- `DtBlkFxEffectComparison`
- `DtBlkFxEffectProbe`

These are standalone diagnostic tools and are not executed by CTest.

### Locate the plug-in artifacts

The plug-in artifacts are generated below the selected build directory.

To locate VST3 and CLAP outputs on macOS:

```bash
find build/macos-arm64 \
    \( -name "*.vst3" -o -name "*.clap" \)
```

For Intel macOS, replace `build/macos-arm64` with `build/macos-x64`.

On Windows:

```powershell
Get-ChildItem -Path build\windows-x64 -Recurse `
    -Include *.vst3,*.clap
```

### Install plug-ins manually on macOS

The standard user-level plug-in directories are:

| Format | Installation directory |
|---|---|
| VST3 | `~/Library/Audio/Plug-Ins/VST3` |
| CLAP | `~/Library/Audio/Plug-Ins/CLAP` |
| AU, if enabled in a future build | `~/Library/Audio/Plug-Ins/Components` |

Create the directories if necessary:

```bash
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
```

Copy the complete plug-in bundle or file found in the build directory.
For example, for a VST3 bundle:

```bash
ditto \
    "path/to/DtBlkFx.vst3" \
    "$HOME/Library/Audio/Plug-Ins/VST3/DtBlkFx.vst3"
```

For CLAP, use the actual `.clap` artifact path returned by `find`:

```bash
ditto \
    "path/to/DtBlkFx.clap" \
    "$HOME/Library/Audio/Plug-Ins/CLAP/DtBlkFx.clap"
```

Do not copy only the executable from inside a plug-in bundle. Preserve
the complete bundle structure.

Restart the DAW or rescan its plug-in directories after installation.

### Verify macOS architecture and dependencies

For a VST3 bundle, inspect the executable inside the bundle:

```bash
file \
    "$HOME/Library/Audio/Plug-Ins/VST3/DtBlkFx.vst3/Contents/MacOS/DtBlkFx"
```

Expected architecture:

- Apple Silicon: `arm64`
- Intel: `x86_64`

Inspect linked dynamic libraries with:

```bash
otool -L \
    "$HOME/Library/Audio/Plug-Ins/VST3/DtBlkFx.vst3/Contents/MacOS/DtBlkFx"
```

The same checks can be performed on the CLAP executable.

### Validate with pluginval

If pluginval is available in `tools/pluginval`, run it against the
generated VST3 bundle.

Windows:

```powershell
.\tools\pluginval\pluginval.exe `
    --validate ".\build\windows-x64\DtBlkFx_artefacts\Release\VST3\DtBlkFx.vst3" `
    --strictness-level 5
```

Adjust the path if the actual JUCE output location differs.

Pluginval validation should be complemented by manual DAW testing.
A successful pluginval run does not prove that every effect or host
interaction behaves correctly.

## Legacy Code

The legacy implementation is available in the legacy branch:

https://github.com/tapester/RenewDtBlkFx/tree/legacy

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

## JUCE migration status

The current JUCE editor exposes the global controls and eight effect slots
with attached sliders, toggles, and populated effect selectors; controls
are host-automatable and save their values with the plugin state. Value
readouts show frequency, gain/mix, delay, block-size, and effect-specific
units; labels and unused controls follow the selected effect. The effect
selectors show one `None` entry at the beginning without changing the
legacy host parameter order. The Power control selects Match or Filter
mode, which changes how Mix Back is interpreted.

The processor now runs FFT frames through an overlap scheduler with legacy
hop calculation, linear block crossfades, host-beat block sync, and
delay-unit conversion (using 120 BPM when host tempo is unavailable).
For short delays it reduces the FFT plan as the legacy engine does, then
keeps that FFT size while centering a shorter active time window in it.

The scheduler interpolates the 44 legacy parameter values by absolute
sample position and uses the legacy quarter-beat transition distance when
parameter changes are observed. JUCE's APVTS exposes parameter snapshots
at process-block boundaries, so automation points inside a host block
cannot be timestamped by this processor.

Filter, Contrast, Smear, Threshold, Clip, Resize, Resample, Shift,
ConstShift, HarmShift, HarmRepitch, HarmFilt, AutoHarm, Sweep1–5,
HarmMask, AutoHarmMask, AutoSubharm1–3Mask, ThreshMask, Vocode,
HarmMatch L>R/R>L, CrossMix, and WarpMix are implemented. The legacy
`None` choices remain bypasses.

Mask effects now select harmonic or threshold ranges for the next effect
slot. Stereo effects operate on both channel spectra and deliver a shared
processed spectrum.

The frequency remappers use a normalized five-bin Lagrange interpolation
kernel for fractional FFT-bin shifts; this is not yet verified against
the legacy kernel. Fundamental tracking and stereo effect algorithms
still need comparison against legacy reference renders. The live
spectrogram has not yet been ported.

Porting status of the stereo effects: Vocode, HarmMatch L>R/R>L, CrossMix,
and WarpMix are ported from the legacy algorithms. CrossMix derives the
phase from the weighted sum of the two bins instead of the legacy
averaged angles, because averaging wrapped angles biases the phase towards
0 and creates an impulse at the frame start that cost about 0.54 dB at
mid mix; this is a deliberate sound deviation. WarpMix follows the legacy
`WeirdMixProcess` (mode 0 curve morphing, mode 1 power-segment stretching
with bin-shift phase correction); the amplitude control is a distortion
amount (linear below 0 dB) and the legacy power-target adjustment is
applied. DC is not mixed (as in CrossMix). Two details are not verified
against legacy renders: the bin-shift phase correction uses an exact
sine/cosine instead of the legacy 4096-entry table, and WarpMix level
relies on Match mode (Power control) as in the legacy engine.

A standalone single-precision FFT workspace provides all legacy FFT sizes
and a 44-value parameter snapshot.

## Suggested tests

The engine test target checks FFT round trips and sine-bin placement for
every legacy FFT size, plus scheduler hop/delay behavior, chunking
invariance, reset/startup behavior, beat-synchronized hops, short-window
FFT sizing and sample-positioned parameter interpolation, Filter behavior,
Contrast spectral shaping and power preservation, deterministic Smear,
Threshold selection, Clip limiting, frequency movement for Resize,
Resample, Shift, and ConstShift, harmonic filtering/tracking/shifting/
repitching, all five Sweep profiles, harmonic and threshold mask chaining,
and stereo effects.

Extend these regressions with edge-frequency, zero/full-amplitude, and
legacy reference-render comparisons.

Additional test areas:

- **Parameter mapping:** verify the 44-value snapshot order and normalized
  ranges; check Mix Back/Power Match and Overlap/Block Sync combinations.
- **FFT correctness:** add silence and seeded-noise cases to the existing
  impulse and sine checks at every supported block size.
- **Bypass transparency:** process mono and stereo signals with all effect
  slots disabled; check gain, channel separation, finite output, and latency.
- **Effect behavior:** test each migrated effect with edge frequencies and
  zero/full amplitude, and compare JUCE renders with legacy references.
- **State round trip:** save and restore plugin state, then compare every
  parameter and the resulting processed audio.
- **Real-time safety:** exercise parameter automation during processing and
  verify the audio callback does not allocate, lock the UI, or access editor
  components.
- **Host smoke tests:** load both plugin formats, automate parameters,
  change sample rate/block size, and confirm state recall and clean unload.

Run the registered automated tests with the platform-specific test preset
described above. CTest runs all registered tests, not just the engine tests.

The FFT workspace is isolated from JUCE and uses FFTW's single-precision
API. The scheduler preallocates its mono/stereo input and output buffers
and reserves workspace for every supported FFT size.

The runtime keeps the selected FFT plan independent of output delay, uses
the full FFT window (the legacy block-shoulder parameter is fixed at zero),
applies plan changes at a frame boundary, and reports its frame-collection
latency; output delay is applied in addition to it.

Hop length uses the raw legacy overlap encoding, and beat-sync intervals
and the minimum hop follow the legacy tick threshold. Parameter
interpolation uses the sample positions of host callback snapshots because
JUCE's APVTS does not expose in-block automation event timestamps here.
Parameter-event-to-block alignment is available through the opt-in
Parameter Sync control and is off by default for legacy-compatible behavior.

Twenty-nine effect types are implemented with focused engine tests; the
two legacy `None` choices remain bypasses. Sweep uses the five legacy
120-by-128 harmonic-power tables and interpolates them in scale or copy
mode. Harmonic and threshold mask effects constrain the next slot's
processing, and the five stereo effects (Vocode, HarmMatch L>R/R>L,
CrossMix, and WarpMix) now run jointly across both channel spectra.

These new effects are tested for deterministic behavior, mask selection,
and stereo mixing, but their legacy parity is unverified without reference
renders. Mask boundary details and stereo automation still need
reference-based refinement. The harness in `tests/reference_compare`
renders both implementations from the same 44 parameters for this purpose.

At startup, the first output frame is placed at the sum of frame-collection
latency and configured delay; the 16-sample startup fade makes its first
nonzero sample one sample later. The host-reported plugin latency is the
frame-collection latency only and does not include the configurable
output delay.

Delay target changes start at a frame boundary and crossfade between the
old and new delay-line read positions over at most one FFT frame. If the
new read position does not yet contain generated output history, the old
position remains active until it does; the ring's absolute-position tags
prevent wrapped stale samples from being read.

The delay ring and tags are preallocated, and the per-sample transition
has bounded work. Deterministic tests verify startup alignment, transition
continuity, and block-size invariance. These are verified JUCE-engine
behaviors, not claims of legacy output parity.

## Remaining porting activities

1. **Smoke-test the current GUI in hosts:** load VST3 and CLAP in at least
   one DAW each. Verify that each effect selector presents `None` once,
   first; that selecting effects updates labels and readouts; and that
   Match/Filter selection changes the Mix Back meaning. Check parameter
   automation, project save/restore, editor reopening, and clean unload.
   The parameter-bound controls, effect-aware readouts, and mode selector
   are implemented, but no DAW/editor smoke test has been completed.

2. **Compare and refine spectral effects:** obtain legacy reference renders,
   then compare the JUCE port using identical inputs and parameters.
   Verify the five-bin frequency-remapping kernel, fundamental tracking,
   Shift's low-frequency peak alignment, mask boundaries, Sweep
   interpolation, and stereo effect behavior (including WarpMix and the
   CrossMix phase change). Parity remains unverified without reference
   renders; use `tests/reference_compare` once legacy renders exist.

3. **Validate scheduler and automation compatibility:** compare startup,
   latency, and delay transitions against legacy renders. The legacy VST
   can force partial FFT frames at host callback boundaries, while the
   JUCE engine waits for a full frame and remains host-block-size invariant.
   Consider a callback-boundary compatibility mode only if reference
   behavior requires it. VST3/CLAP currently receive APVTS snapshots at
   callback boundaries, not timestamped parameter events, so in-block
   automation is not sample-accurate; add event-offset handling only if a
   supported JUCE format path exposes those timestamps.

4. **Port the live visualization:** safely publish input/output spectra to
   the editor and implement the legacy live spectrogram. Keep audio-thread
   processing independent of editor lifetime and verify behavior when the
   editor is closed or reopened.

5. **Complete release validation:** verify preset/state migration, transport
   changes, sample-rate and channel-layout changes, callback allocation and
   locking behavior, and latency/tail reporting. Expand DSP regressions,
   run pluginval, and repeat host smoke tests for both plugin formats.
