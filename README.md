# ACE Audio KeySync

Musical key detection VST3 for FL Studio. Free edition detects and displays the
key of the audio it receives. Pro edition adds ranked alternative keys with
relationship labels. Audio passes through unaltered.

## Building (Windows, FL Studio 21+, VST3)

Prerequisites: Visual Studio Build Tools 2022 (Desktop C++ workload),
CMake 3.24+, git.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Artifacts: `build/KeySyncFree_artefacts/Release/VST3/KeySync Free.vst3`
and `build/KeySyncPro_artefacts/Release/VST3/KeySync Pro.vst3`.

Install: copy the `.vst3` folder to
`C:\Program Files\Common Files\VST3\`, then in FL Studio: Options >
File Settings > plugin search paths > re-scan (or Find Plugins).

Third-party code: JUCE 8 (FetchContent, AGPLv3/GPLv3/commercial dual license
apply to the framework itself). Detector and recommender are original code
(Krumhansl-Schmuckler profiles, no GPL dependencies). Fonts Isometra and Inter
are SIL OFL-licensed, bundled in `assets/fonts/`.

## Detector notes

Rolling 15-second chromagram window, 4096-point STFT with 2048 hop, Krumhansl
profiles, Pearson correlation over 24 keys. Confidence combines profile match
quality with the margin between the top two keys. Thresholds live in
`source/dsp/KeyDetector.h` (`kRmsThreshold`, `kMinActiveFrames`,
`kConfidenceResultPct`).

Run the detector self-test (no host needed):

```
cmake --build build --target keysync_selftest --config Release
build\Release\keysync_selftest.exe
```

## Known limitations

- Tonality window is 55 Hz – 4 kHz; content entirely outside that range is
  invisible to the detector.
- Results are deliberately unreliable on drums-only, atonal, noisy or heavily
  distorted material; the confidence readout flags them rather than guessing.
- Bach-style polyphonic ambiguity and modal material (Dorian etc.) can sit
  between major/minor; confidence drops and "Uncertain" shows.
- Confidence percentages are not yet calibrated on a measured test set (that
  is the fixture plan in `docs/TESTING.md`, pending).
- Analysis window is a rolling last-15-seconds snapshot; if you stop playback
  and press Analyze much later, the key shown may be from stale audio.
- "Get Pro" link is a placeholder URL until the store page exists.
- Free/Pro are separate binaries; the `KEYSYNC_EDITION_PRO` flag is the seam
  for the future account-login unlock (ACE Studios API, still in development).

Planned: accuracy fixture against ~50 known-key clips, targets and results in
`docs/TESTING.md` (to be written after the accuracy run).

## Releases

CI builds both editions on every `v*` tag: GitHub Actions (`.github/workflows/
release.yml`) compiles on `windows-latest`, runs the detector self-test, and
uploads `KeySyncFree_VST3-win-x64.zip` / `KeySyncPro_VST3-win-x64.zip` to the
tag's GitHub Release. To cut a release:

```
git tag v1.0.0 && git push origin main --tags
```

Note: the binaries are unsigned; Windows SmartScreen/AV may warn on first use.

## Editions

`KeySyncFree` and `KeySyncPro` build from identical sources via
`KEYSYNC_EDITION_PRO`. Pro account-login unlock is planned against the ACE
Studios API; the edition flag is the seam (see `PluginProcessor.h`).
