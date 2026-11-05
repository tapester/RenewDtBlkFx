# DtBlkFx Audio FX Plug-in

DtBlkFx is a Fast-Fourier-Transform (FFT) based VST plug-in.

Forked from https://codeberg.org/skullzy/DtBlkFx

## Build using CMake
Windows with PowerShell, from a Developer PowerShell for VS 2022:

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
ctest --test-dir build/windows-x64 --output-on-failure
```

This is the default Windows build. FFTW is statically linked into the plugin, so no
`fftw3f.dll` has to be installed next to it. The plugin still needs the Microsoft Visual C++
2015-2022 Redistributable for the matching architecture.

A variant that links FFTW dynamically is available as a separate preset:

```
cmake --preset windows-x64-dynamic-fftw
cmake --build --preset windows-x64-dynamic-fftw --parallel
```

Its plugin is output under `build/windows-x64-dynamic-fftw`, and `fftw3f.dll` is staged
beside each plugin binary. When installing that variant manually, copy the complete
`DtBlkFx.vst3` bundle (including `Contents/x86_64-win/fftw3f.dll`) into the VST3 folder.
Some hosts, including pluginval, do not search the plugin folder for dependent DLLs and
will fail to load this variant. Prefer the default static build.


## Legacy Code

Is available in the legacy branch: https://github.com/tapester/RenewDtBlkFx/tree/legacy

## Usage examples

- Precision parametric equalizing with sharp-roll off
  - Set the frequencies so accurately that you can adjust individual harmonics
    of a sound
  - Frequency resolutions of up to 0.7 Hz
- Harmonic based (or comb) filtering
  - Set a fundamental frequency and adjust the level of it and its harmonics -
    you can even remove the pitched component of a voice
  - Active harmonic tracking - let DtBlkFx automatically track a sound and
    adjust the level of it's harmonics
- Various types of noise control
  - Change the "contrast" between loud and soft frequencies
  - Adjust only those frequencies below or above a particular threshold
  - Clip frequencies above a particular threshold
  - Sound smearing (phase randomizing)
- Frequency shifting
  - Harmonic shifting by a fixed number of notes
  - Non-harmonic shifting by a fixed frequency
  - Active harmonic repitch - the pitch of your sound is monitored and shifted
    to a destination note (or matched to another channel)
- Various methods of mixing left and right channels
  - Standard Vocoding (frequency enveloping) - make your trumpet rap, string
    section sing or synthesizer talk
  - Harmonic based vocoding - harmonics in one channel are power-matched to
    those in the other (or some predefined waveforms) for a new vocoding sound
  - Convolution-like mixing
  - 2 new mixing algorithms
- Frequency masking
  - A harmonic or threshold mask may be set for any effect (apart from vocoding)
    for example only shift frequencies that are below the threshold

You can select up to 8 of the above effects to be run in series! Combining the
effects in this way allows you to make completely new and surprising sounds.

## JUCE migration status

The current JUCE editor exposes the global controls and eight effect slots
with attached sliders, toggles, and populated effect selectors; controls are
host-automatable and save their values with the plugin state. Value readouts
show frequency, gain/mix, delay, block-size, and effect-specific units; labels
and unused controls follow the selected effect. The effect selectors show one
`None` entry at the beginning without changing the legacy host parameter order.
The Power control selects Match or Filter mode, which changes how Mix Back is
interpreted. The processor now runs FFT frames through an overlap scheduler
with legacy hop
calculation, linear block crossfades, host-beat block sync, and delay-unit
conversion (using 120 BPM when host tempo is unavailable). For short delays it
reduces the FFT plan as the legacy engine does, then keeps that FFT size while
centering a shorter active time window in it. The scheduler interpolates the
44 legacy parameter values by absolute sample position and uses the legacy
quarter-beat transition distance when parameter changes are observed. JUCE's
APVTS exposes parameter snapshots at process-block boundaries, so automation
points inside a host block cannot be timestamped by this processor. Filter,
Contrast, Smear, Threshold, Clip, Resize, Resample, Shift, ConstShift,
HarmShift, HarmRepitch, HarmFilt, AutoHarm, Sweep1–5, HarmMask,
AutoHarmMask, AutoSubharm1–3Mask, ThreshMask, Vocode, HarmMatch L>R/R>L,
CrossMix, and WarpMix are implemented. The legacy `None` choices remain
bypasses. Mask effects now select harmonic or threshold ranges for the next
effect slot. Stereo effects operate on both channel spectra and deliver a
shared processed spectrum. The frequency remappers use a normalized five-bin
Lagrange interpolation kernel for fractional FFT-bin shifts; this is not yet
verified against the legacy kernel. Fundamental tracking and stereo effect
algorithms still need comparison against legacy reference renders; in
particular, the WarpMix implementation is an approximation. The live
spectrogram has not yet been ported. A standalone single-precision FFT
workspace provides all legacy FFT sizes and a 44-value parameter snapshot.

## Suggested tests

The engine test target checks FFT round trips and sine-bin placement for every
legacy FFT size, plus scheduler hop/delay behavior, chunking invariance, and
reset/startup behavior, beat-synchronized hops, short-window FFT sizing and
sample-positioned parameter interpolation, Filter behavior, and Contrast
spectral shaping and power preservation, deterministic Smear, Threshold
selection, Clip limiting, frequency movement for Resize, Resample, Shift, and
ConstShift, harmonic filtering/tracking/shifting/repitching, all five Sweep
profiles, harmonic and threshold mask chaining, and stereo effects. Extend
these regressions with edge-frequency, zero/full-amplitude, and legacy
reference-render comparisons.

- **Parameter mapping:** verify the 44-value snapshot's order and normalized
  ranges; check Mix Back/Power Match and Overlap/Block Sync combinations, and
  confirm every effect choice maps to the legacy effect index.
- **FFT correctness:** add silence and seeded-noise cases to the existing
  impulse and sine checks at every supported block size.
- **Bypass transparency:** process mono and stereo signals with all effect
  slots disabled; check gain, channel separation, finite output, and the
  documented latency.
- **Effect behavior:** test each migrated effect with edge frequencies and
  zero/full amplitude, and compare the JUCE renders with legacy references.
- **State round trip:** save and restore plugin state, then compare every
  parameter and the resulting processed audio.
- **Real-time safety:** exercise parameter automation during processing and
  verify the audio callback does not allocate, lock the UI, or access editor
  components.
- **Host smoke tests:** load both plugin formats, automate parameters, change
  sample rate/block size, and confirm state recall and clean unload.

Run the current engine tests after building with:

```powershell
ctest --test-dir build/windows-x64 -C Release --output-on-failure
 .\tools\pluginval\pluginval.exe --validate ".\build\windows-x64-static-fftw\DtBlkFx_artefacts\Release\VST3\DtBlkFx.vst3" --strictness-level 5
```



The FFT workspace is isolated from JUCE and uses FFTW's single-precision API.
The scheduler preallocates its mono/stereo input and output buffers and reserves
workspace for every supported FFT size. The runtime keeps the selected FFT
plan independent of output delay, uses the full FFT window (the legacy
block-shoulder parameter is fixed at zero), applies plan changes at a frame
boundary, and reports its frame-collection latency; output delay is applied in
addition to it. Hop length uses the raw legacy overlap encoding, and beat-sync
intervals and the minimum hop follow the legacy tick threshold. Parameter
interpolation uses the sample positions of host callback snapshots because
JUCE's APVTS does not expose in-block automation event timestamps here.
Parameter-event-to-block alignment is available through the opt-in Parameter
Sync control and is off by default for legacy-compatible behavior. Twenty-nine
effect types are implemented with focused engine tests; the two legacy `None`
choices remain bypasses. Sweep uses the five legacy 120-by-128 harmonic-power
tables and interpolates them in scale or copy mode. Harmonic and threshold
mask effects constrain the next slot's processing, and the five stereo effects
(Vocode, HarmMatch L>R/R>L, CrossMix, and WarpMix) now run jointly across both
channel spectra. These new effects are tested for deterministic behavior,
mask selection, and stereo mixing, but their legacy parity is unverified
without reference renders. WarpMix is an approximation of the legacy curve
morphing algorithm, and mask boundary details and stereo automation still
need reference-based refinement.

At startup, the first output frame is placed at the sum of frame-collection
latency and configured delay; the 16-sample startup fade makes its first
nonzero sample one sample later. The host-reported plugin latency is the
frame-collection latency only and does not include the configurable output
delay. Delay target changes start at a frame boundary and crossfade between
the old and new delay-line read positions over at most one FFT frame. If the
new read position does not yet contain generated output history, the old
position remains active until it does; the ring's absolute-position tags
prevent wrapped stale samples from being read. The delay ring and tags are
preallocated, and the per-sample transition has bounded work. Deterministic
tests verify startup alignment, transition continuity, and block-size
invariance. These are verified JUCE-engine behaviors, not claims of legacy
output parity.

## Remaining porting activities

1. **Smoke-test the current GUI in hosts:** load VST3 and CLAP in at least one
   DAW each. Verify that each effect selector presents `None` once, first; that
   selecting effects updates labels and readouts; and that Match/Filter
   selection changes the Mix Back meaning. Check parameter automation, project
   save/restore, editor reopening, and clean unload. The parameter-bound
   controls, effect-aware readouts, and mode selector are implemented, but no
   DAW/editor smoke test has been completed.
2. **Compare and refine spectral effects:** obtain legacy reference renders,
   then compare the JUCE port using identical inputs and parameters. Verify the
   five-bin frequency-remapping kernel, fundamental tracking, Shift's
   low-frequency peak alignment, mask boundaries, Sweep interpolation, and
   stereo effect behavior. WarpMix still approximates the legacy
   curve-warping algorithm; parity remains unverified without reference renders.
3. **Validate scheduler and automation compatibility:** compare startup,
   latency, and delay transitions against legacy renders. The legacy VST can
   force partial FFT frames at host callback boundaries, while the JUCE engine
   waits for a full frame and remains host-block-size invariant. Consider a
   callback-boundary compatibility mode only if reference behavior requires
   it. VST3/CLAP currently receive APVTS snapshots at callback boundaries, not
   timestamped parameter events, so in-block automation is not sample-accurate;
   add event-offset handling only if a supported JUCE format path exposes those
   timestamps.
4. **Port the live visualization:** safely publish input/output spectra to the
   editor and implement the legacy live spectrogram. Keep audio-thread
   processing independent of editor lifetime and verify behavior when the
   editor is closed or reopened.
5. **Complete release validation:** verify preset/state migration, transport
   changes, sample-rate and channel-layout changes, callback allocation and
   locking behavior, and latency/tail reporting. Expand DSP regressions, run
   pluginval, and repeat host smoke tests for both plugin formats.
