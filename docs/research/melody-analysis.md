# Melody extraction + display (v1.1, Pro edition) — research

Primary-source findings, Oct 26 2026. License rule: GPL/AGPL = concepts and documented
parameters only, never code. ISC (librosa) = reading code is legal, we still re-derive.

## A. Full Melodia (Salamon & Gómez) — what we are NOT building in v1

Sources:
- Salamon & Gómez, "Melody Extraction from Polyphonic Music Signals using Pitch Contour
  Characteristics", IEEE TASLP 20(6):1759–1770, 2012.
- Salamon PhD thesis, UPF 2013; Salamon & Gómez DAFx-11 salience paper.
- Essentia `PredominantPitchMelodia` / `PitchSalienceFunction` / `PitchContours` docs
  (parameter defaults = documented facts we may reuse).

Full chain: equal-loudness pre-filter → sinusoid peaks (frame 2048 @ 44.1 k, hop 128 ≈
2.9 ms, ×4 zero-pad, phase-vocoder IF refinement) → 600-bin/10-cent salience with
harmonic weight 0.8 per harmonic and cent-neighbourhood spreading → two-stage peak
filtering (0.9 × frame max; global mean−0.9σ) → contour tracking (time continuity
100 ms gap, pitch continuity 27.56 c/ms, min duration 100 ms) → batch contour-feature
selection, voicing tolerance 0.2, 3-iteration octave-error subgrouping.

MIREX 2011 numbers (author's own report): mean overall accuracy ≈ 0.75 across the six
datasets; vocal-heavy sets best (0.78–0.84), mixed instrumental worst (0.68). Known
field defect: over-voicing (confuses unvoiced as voiced).

Not v1 because: selection/voicing/octave passes are window-batch algorithms (bad for
streaming), ~2–4 ms/frame, several KB of contour state.

## B. v1 = HMM-lite on the existing spectrum (agreed)

No new FFT — runs on the per-hop magnitude spectrum the tempo/chroma paths already
compute. Budget measured target: ≤ ~0.3 ms/hop, < 1 MB state total.

1. **Peak pick** — local maxima of magnitude spectrum over 60–1300 Hz, parabolic
   interpolation on the top K=8 peaks (librosa `piptrack` + `_parabolic_interpolation`
   recipe, `librosa/core/pitch.py`, ISC).
2. **Semitone salience** — 48 rows C2–C6 (MIDI 36–83): `s[c] = Σ_{h=2..6} max(0,
   S(midi + 12·log2 h)) · 0.8^(h−1)`. Melodia's harmonic weight + 65 Hz–1.76 kHz range
   as priors.
3. **Voicing gate** — salience floor; over-voicing on quiet intros is an accepted v1
   defect (Melodia's distribution-stats voicing deferred).
4. **Viterbi, 48 states** — local ±2 semitone transitions; ±12 octave transition at
   0.3× — the octave-error mitigation that replaces Melodia's batch subgrouping
   (precedent: pYIN local transition matrix, Mauch & Dixon ICASSP 2014).
5. **Output ring** — `{midi float, conf float, voiced u8}` per hop, ~30 s span,
   seqlocked like our key/chroma rings.

## C. Display (fixed-size-friendly, message-thread-synced)

- Rolling lane: one `juce::Image`, one new column per hop (`Image::BitmapData`
  pattern, juce.com spectrogram tutorial). ~90 px/s at editor scale; repaint only when
  a column is written. `paint()` = single `drawImageAt`.
- `juce::Path` contour overlay recomputed only on hop arrival (no Timer, no 60 fps
  churn; VST peer clips dirty rects — same sync slot as key/BPM results).
- Y axis: semitone/log, C2 (MIDI 36) → C6 (84), ~7.9 px/semitone at 380-wide scale;
  grey octave labels C2–C6 (Inter 8pt; Isometra stays wordmark-only); orange = conf.
- Note boxes (quantized piano-roll): deferred — Salamon's own MIDI-conversion post
  calls note segmentation an open problem (round → median filter → group is the cheap
  recipe when we want it).

## D. Melody as a third KS chroma view (the reason this feature earns itself)

- Gómez 2006 PhD thesis (UPF): tonality ≈80% accuracy on 1400 pieces from a fusion of
  melody-salience and bass descriptors — melody histogram is a legitimate independent
  key input, not a display gimmick.
- Salamon et al., "Tonal Representations for Music Retrieval" (IJMIR 2013): melody F0
  → octave-folded semitone histogram ≈ "melody PCP" — exactly what we emit.
- Design: `melodyPCP[12]` = conf-weighted pitch-class histogram over the rolling
  window → KS correlation as a third view. Blend starts 0.7 full-chroma / 0.3
  melody-PCP; capped at 0.4 until a second labeled-corpus win proves more.
  melody-PCP alone ignores bass (parallel-confusion risk) — never ship weight > 0.5.

## Gate ladder

1. Probe-only first: extractor in `keysync_probe` over `music/` 4 + trio; melody-PCP
   key via KS consensus. Pass = no labeled regression; goal = Dark Fantasy or
   Immortal improves.
2. Plugin: `MelodySlotTight`-style ring + tracker in KeyDetector, Pro edition.
3. Editor lane panel + contour path (Pro; window may resize).
4. Selftest: synthetic melody fixture (sweep/vibrato), tracker sanity.
Kill rule (user-approved): any labeled regression → revert. Whitening, per-frame
tuning, A&S profiles all died by this rule.

## FUSION VERDICT (probed Oct 26) — FAILED, parked

HMM-lite melody-PCP fusion (flat 0.3, conf², adaptive-by-correlation weight, and
Melodia-faithful spectral-peak-only salience) was A/B'd on the labeled corpus:

- Immortal: fusion fixed it (C 5/5) — the one true win.
- Selah (choir): fused → D minor, a REGRESSION (baseline Am was right; melody windows
  scatter F/Dm/C#m/C/Dm — PCP flat, argmax noise).
- Long December: fused → A# major (baseline F 4/5). Regression.
- Dark Fantasy: still wrong, no gain.

Root cause: without Melodia's batch contour selection/voicing/octave passes the
salience histogram on dense mixes is flat and unreliable, so KS-corr on it fires
confidently onto wrong keys (a peaked hist correlates high with ANY profile —
adaptive weighting can't save this). Cheap HMM-lite is fine for DISPLAY of a
candidate predominant line (conf is honest there), not for steering the key stage.

Scope agreed with user: melody v1.1 = extract + lane display (Pro). Key-stage fusion
parked until a full Melodia-grade extractor (contour machinery, ~2–4 ms/frame) exists
or a larger labeled corpus justifies retraining the gate. The 0.4 melody-weight cap
stays as a guardrail if fusion is ever revisited.
