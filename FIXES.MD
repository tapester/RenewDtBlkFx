# RenewDtBlkFx – Code Review & Fix Checklist

**Repository:** `tapester/RenewDtBlkFx`
**Branch:** `main`
**Reviewed commit:** `6d3a22a`
**Review scope:** Static code review only (no build or runtime execution)

## 1. Must fix

| Done  | ID | Finding / Required fix                                                                                                                                                                                            | Risk if not addressed                                                                                                                      |
| ----- | -: | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| [ ] |  1 | **FFTW planning is not thread-safe.** Add one `static std::mutex` and lock it around the FFTW plan creation loop in `prepare()` and destruction loop in `release()` (`src/DtBlkFxEngine.cpp:L37-62`, `L105-120`). | Concurrent initialization of multiple plug-in instances can cause crashes or memory corruption in some hosts.                              |
| [ ] |  2 | **Prevent audio-thread memory allocation.** Add `effectMask.reserve(maximumBinCount);` alongside the other buffer reservations in `prepare()` (`DtBlkFxEngine.cpp:L70-84`, `L610-622`).                           | Automation that increases FFT size can trigger allocation on the audio thread, causing dropouts and violating constraint 2 in `AGENTS.md`. |

## 2. Should fix

| Done  | ID | Finding / Required fix                                                                                                                                                                                                                                                                                                        | Risk if not addressed                                                                                                 |
| ----- | -: | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| [ ] |  3 | **Measure worst-case FFT processing time.** Measure `processFrame()` execution time at maximum FFT size (80640) with all 8 effects active. Consider redesigning processing only if execution approaches the available buffer time. Use the existing `DtBlkFxEffectProbe` if suitable. (`DtBlkFxEngine.cpp:L367-389`)          | Large FFT blocks and small audio buffers may cause audio dropouts or crackling.                                       |
| [ ] |  4 | **Centralize legacy parameter conversion.** Move the FFT plan index formula (`value * 255 / 4 - 1.5`) and mix-back encoding/decoding into `DtBlkFxLegacyMapping.h`, next to `effectTypeToLegacyValue`. Use the shared functions in both `DtBlkFxProcessor.cpp` and `DtBlkFxEngine.cpp`.                                       | Duplicated conversion logic can drift, causing incompatible parameter mappings and different sound in saved projects. |
| [ ] |  5 | **Align CI builds with release builds.** Use `cmake --preset macos-arm64-tests` and `ctest --preset macos-arm64-tests` on macOS. For Windows, either switch to the corresponding Ninja preset and adjust artifact paths, or explicitly document why CI continues using Visual Studio. (`.github/workflows/build-plugins.yml`) | CI may pass while release builds fail due to different deployment targets, generators or output paths.                |
| [ ] |  6 | **Remove merge-conflict markers from the reference comparison README.** Resolve the four conflict-marker lines in `tests/reference_compare/README.md:L25-57`, retain the portable `<vcpkg>` and `<VST2-SDK>` placeholders, and translate the document into English.                                                           | The legacy comparison setup instructions are ambiguous and cannot reliably be followed.                               |
| [ ] |  7 | **Verify thread safety of latency updates.** Check whether JUCE's VST3 wrapper handles `setLatencySamples()` calls from `processBlock()` on the audio thread. If not, move latency changes to the main thread, e.g. through a parameter listener. (`DtBlkFxProcessor.cpp:L401-405`)                                           | Automating the Latency parameter may cause host inconsistencies or crashes.                                           |

## 3. Nice to have

| Done  | ID | Finding / Required fix                                                                                                                                                                                                      | Risk if not addressed                                                                             |
| ----- | -: | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| [ ] |  8 | **Clean up stale CMake comment.** Remove or update the comment above `CLAP_ID "com.tapester.dtblkfx"` stating that a permanent ID must be assigned. The ID is already set and should be considered frozen.                  | Documentation becomes misleading and may encourage unnecessary changes to the plug-in identifier. |
| [ ] |  9 | **Review diagnostic applications.** Decide whether to retain or retire `DtBlkFxFftSwitchDiagnostic`, `DtBlkFxEffectComparison` and `DtBlkFxEffectProbe` (2,182 lines combined). Verify that they still compile if retained. | Unused diagnostic code increases maintenance overhead.                                            |

## 4. Recommended implementation order

1. Fix FFTW plan synchronization (ID 1).
2. Reserve `effectMask` capacity (ID 2).
3. Centralize legacy mappings (ID 4).
4. Resolve the reference comparison README (ID 6).
5. Align CI with release presets (ID 5).
6. Investigate worst-case processing time (ID 3).
7. Verify JUCE latency update behaviour (ID 7).
8. Clean up CMake documentation (ID 8).
9. Decide on diagnostic application maintenance (ID 9).

## 5. Review verdict

**Overall assessment:** Healthy for a port at this stage, with many tests and careful handling of bad input.

The first two fixes should be prioritized because they require only a few lines of code and address important real-time and thread-safety risks.

### Estimated impact

- Approximately 10 lines removed by centralizing legacy mappings (ID 4).
- Zero additional dependencies.
- Up to 2,182 lines potentially removed if diagnostic applications are retired (ID 9).

## 6. Not checked

The following areas were not fully reviewed:

[ ] Effect mathematics in `DtBlkFxEngine.cpp` (approximately lines 700–2400; only skimmed).

[ ] Plug-in editor.

[ ] Python harness.

[ ] Test contents.

[ ] Sweep tables.

[ ] Git submodules.

[ ] `legacy` branch.
