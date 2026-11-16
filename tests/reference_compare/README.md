# Referenzvergleich: Legacy-DtBlkFx (VST2) gegen JUCE-Engine

Dieses Gerüst rendert dieselben Testsignale mit **denselben 44 Legacy-Parametern** durch das alte
VST2-Plugin und durch `DtBlkFxEngine` (main-Branch) und bewertet die Unterschiede. Es misst
Klangähnlichkeit, keine Bit-Gleichheit: Die Portierung weicht bewusst bzw. bekanntermaßen ab
(z. B. Fractional-Shift-Kern, Latenz-/Delay-Bilanz), und viele Effekte sind phasenempfindlich.

## Stand

| Baustein | Status |
|---|---|
| Signale, Testfälle (168), Metriken, Vergleich, Bericht | getestet (Selbsttest + Durchlauf aller Fälle) |
| Engine-Renderer (`render_engine`) | getestet; alle 168 Fälle rendern, Ergebnis ist unabhängig von der Host-Blockgröße |
| VST2-Host für die Legacy-DLL (`vst2_render_host.cpp`) | **ungetestet** (kein Windows, keine DLL, kein VST-SDK in der Entwicklungsumgebung) |
| Schwellwerte | **unkalibriert**, Startwerte |

Einen echten Vergleich gibt es erst, wenn Legacy-Renderings vorliegen. Bis dahin ergibt `compare`
für jeden Fall `NO-REF`.

## Ablauf

```
python tests/reference_compare/tools/refcmp.py gen --workdir work --presets <legacy>/resources/stereo_presets.txt
```

erzeugt `work/signals/*.wav` (5 Signale, 44,1 kHz, 6 s Inhalt + 4 s Stille) und `work/cases.json`.

Oder via uv mit Pfaden:

```powershell
uv run tests\reference_compare\tools\refcmp.py gen --workdir work --presets legacy_plugin\DtBlkFx-x64\dtblkfx\stereo_presets.txt
```

**1. Engine rendern** (ohne JUCE, nur FFTW3f):
```
cmake -S tests/reference_compare/engine_render -B build-refrender -DCMAKE_TOOLCHAIN_FILE=modules/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-refrender --config Release
python tests/reference_compare/tools/refcmp.py render-engine --workdir work --engine-bin build-refrender/render_engine
```

**2. Legacy rendern (Windows)**: Legacy-DLL bauen (Visual-Studio-Projekt im `legacy`-Branch, VST2-SDK,
FFTW- und libpng-DLLs neben die DLL legen). Bitness von Host und DLL muss übereinstimmen; für
Stereo-Signale die Stereo-Variante verwenden.
```
cl /EHsc /O2 /I legacy_src\dtblkfx\vstsdk\source\vst2.x tests\reference_compare\legacy_render\vst2_render_host.cpp /Fe:vst2_render_host.exe
python tests\reference_compare\tools\refcmp.py render-legacy --workdir work --host vst2_render_host.exe --dll DtBlkFx.dll
```
Alternativ in einem beliebigen Host rendern: Eingabedatei aus `work/signals/`, die 44 Werte des
Falls aus `cases.json` setzen, Tempo 120 BPM, Transport ab Position 0, **ohne**
Latenzkompensation, gleiche Länge wie die Eingabe, Ergebnis als `work/legacy/<case-id>.wav`.

**3. Vergleichen**:
```
python tests/reference_compare/tools/refcmp.py compare --workdir work [--tolerances tolerances.example.json] [--strict]
```
Ergebnis: `work/report.md` (nach Gruppe, schlechteste Fälle zuerst) und `work/report.json`.

Zusatz ohne Legacy-Referenz: `refcmp.py consistency` rendert die Engine mit einer zweiten
Host-Blockgröße (Standard 37) und meldet Blockgrößen-Abhängigkeit, NaN/Inf und extreme Pegel.
`tools/selftest.py` prüft die Vergleichskette selbst (identisch, verzögert + abgesenkt,
andere Parameter, fehlende Referenz).

## Testfälle

| Gruppe | Inhalt |
|---|---|
| `preset` | je ein Fall pro Legacy-Preset (43 Stereo-Presets), Signal `chord` (über `--preset-signals` erweiterbar) |
| `fx_single` | jeder der 31 Effekt-Typen allein mit Wert 0,25 / 0,5 / 0,75; Maskentypen mit nachgeschaltetem Filter |
| `timing` | Impulszug; Delay in Beats und ms, FFT-Plan 8/16/24, auch Delay kürzer als Block (bekannte Abweichung) |
| `fft_len` | Frequenzverschiebung über FFT-Pläne 4 bis 28 |
| `overlap` | Overlap 0 bis max., mit und ohne Block-Sync |
| `mixback` | Mixback 0 bis 1 |

Die Parameter der Gruppen außer `preset` sind aus meinem Lesen von `BlkFxParam.h` kodiert
(`tools/cases.py`, Funktionen `enc_*`). Wenn ein Fall unerwartet wirkt, zuerst prüfen, ob die
Kodierung zum Legacy-Plugin passt; die Preset-Fälle kommen direkt aus den Legacy-Dateien.

## Metriken (`tools/metrics.py`)

Zuerst wird die Verzögerung des Engine-Outputs gegenüber dem Legacy-Output geschätzt
(Wellenform-Kreuzkorrelation, sonst Hüllkurve; Suchbereich 2 s) und herausgerechnet. Dann:

- `lsd_db`: mittlere log-spektrale Distanz (STFT 2048/512), aktive Frames, Boden −90 dB
- `level_diff_db`, `peak_diff_db`: Effektiv- und Spitzenpegel
- `env_corr`: Korrelation der 10-ms-Hüllkurven
- `band_diff_db`: größte Energiedifferenz in Oktavbändern
- `centroid_diff_hz`, `lr_corr_diff`: Spektralschwerpunkt, Stereo-Korrelation
- `null_residual_db`: Nullresiduum, nur informativ

Urteil `PASS` / `WARN` / `FAIL` aus `lsd_db`, `level_diff_db`, `env_corr`, `band_diff_db`
(Schwellen in `DEFAULT_TOLERANCES`, pro Gruppe oder Fall überschreibbar, siehe
`tolerances.example.json`).

## Einordnung und Grenzen

- **Schwellen kalibrieren**: Nach den ersten echten Legacy-Renderings die Verteilung ansehen und
  die Grenzen so setzen, dass bekannt gute Fälle (z. B. Identitäts- und Filterfälle) `PASS` sind.
- **Bekannte Abweichungen** sollten sich im Bericht zeigen: Gruppe `timing` bei Delay kürzer als
  Block (Legacy verkürzt die FFT-Länge) und Shift-Effekte (anderer Fractional-Shift-Kern).
  Falls nicht, sind die Schwellen zu locker.
- **Lag-Schätzung** kann bei stark verfremdeten Signalen fehlschlagen; dann in `--lags` einen
  festen Wert pro Fall vorgeben.
- Latenz der Legacy-Seite: das Plugin meldet 0; die tatsächliche Verzögerung wird per Korrelation
  bestimmt und im Bericht als `lag` ausgewiesen.
- Mono wird nicht abgedeckt (Signale sind stereo); die Stereo-Effekte 26 bis 30 brauchen Stereo.
- Der Renderer übergibt die Beat-Phase aus der Sample-Position (wie der Prozessor aus der
  Host-Position). Ein konstanter Wert würde das Beat-Raster der Engine pro Block neu starten.
