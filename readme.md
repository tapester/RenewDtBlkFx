# DtBlkFx Audio FX Plug-in

DtBlkFx is a Fast-Fourier-Transform (FFT) based VST plug-in.

Forked from https://codeberg.org/skullzy/DtBlkFx

## Build using CMake
Windows with Powershell:

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
```

On Windows, the build stages `fftw3f.dll` beside each plugin binary. When
installing the VST3 manually, copy the complete `DtBlkFx.vst3` bundle (including
`Contents/x86_64-win/fftw3f.dll`) into the VST3 folder; do not copy only
`DtBlkFx.vst3`. Windows also needs the Microsoft Visual C++ 2015-2022
Redistributable for the matching architecture.

To build a Windows variant with FFTW statically linked into the plugin instead,
use the separate static-FFTW preset:

```
cmake --preset windows-x64-static-fftw
cmake --build --preset windows-x64-static-fftw --parallel
```

Its plugin is output under `build/windows-x64-static-fftw`; the existing
`windows-x64` preset continues to build the dynamic-FFTW variant. The static
variant does not require `fftw3f.dll`, but still requires the Microsoft Visual
C++ 2015-2022 Redistributable.

MacOS Silicon with Bash:

```
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --parallel
```

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
host-automatable and save their values with the plugin state. The
processor now runs FFT frames through an overlap scheduler with legacy hop
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

```
ctest --test-dir build/windows-x64 -C Release --output-on-failure
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

1. **Compare scheduler output with legacy references:** the legacy VST can force
   partial FFT frames at host callback boundaries, while the JUCE engine waits
   for a full frame and remains host-block-size invariant. No legacy reference
   renders are present, so startup and delay-transition parity is unverified.
   Obtain reference renders and host-specific behavior before considering an
   opt-in callback-boundary compatibility mode; that mode is not implemented.
   The current VST3/CLAP processor path receives APVTS snapshots at callback
   boundaries, not timestamped parameter events, so in-block automation is not
   sample-accurate. Add event-offset handling only if a supported JUCE format
   path exposes those timestamps.
2. **Compare and refine spectral effects:** render the legacy plugin and JUCE
   port with identical inputs and parameters. Verify the five-bin
   frequency-remapping kernel against legacy renders, refine fundamental
   tracking and Shift's low-frequency peak alignment, mask
   boundaries, Sweep interpolation, and stereo effect behavior. WarpMix needs
   a closer port of the legacy curve-warping algorithm. No legacy reference
   renders are currently included.
3. **Restore visualization and GUI behavior:** smoke-test the parameter-bound
   editor in VST3 and CLAP hosts, then publish input/output spectra safely to
   the editor, implement the live spectrogram, and refine effect controls to
   show the corresponding legacy value ranges and interactions.
4. **Complete compatibility and real-time validation:** verify preset/state
   migration, automation and transport changes, sample-rate/channel layouts,
   callback allocation/locking behavior, and latency/tail reporting. Expand
   DSP regressions, run pluginval, and smoke-test both plugin formats in hosts.
