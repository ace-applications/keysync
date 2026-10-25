# Vocal/choir robustness for Krumhansl-Schmuckler key detection

> **Implementation outcome (Oct 2026).** Path A was implemented in `KeyDetector`
> and A/B-tested against a synthetic voiced fixture (A minor scale, vibrato +
> detuned double) and three real sung tracks via the `keysync_probe` CLI.
> Results, measured, and what actually shipped:
> - **Stowell whitening: removed.** Flattened real-music chroma badly — Mr. Rager
>   A major 0.840 → D-window ~0.47 conf, phantom pitch classes doubled
>   (Staff Roll A# 0.049 → 0.107). Even with all other Path-A pieces off it
>   never beat the plain mapping. Our 4096-FFT + snap grid gains nothing.
> - **Per-frame local tuning: removed.** Broke plain C major into A minor
>   (best r 0.728, wrong). Frame-local residual-mode voting is too sparse at
>   4096 pts / 2048 hop; librosa-style global estimation needs far more votes
>   than a single frame supplies. A/B showed it neutral-to-harmful on the
>   fixture too.
> - **Fractional bin splitting: removed.** Phantom-class inflation (bins
>   between two semitones contribute to both) — snap mapping won every A/B.
> - **Max-per-class chroma view (mean ∨ max): kept.** Path A's survivor. On the
>   fixture it lifted the A-minor margin materially at high-wobble settings;
>   on real music neutral-or-better (Tequila margin 0.14 → 0.20), never worse.
> - **Fixture calibration:** the first fixture used ±42 c vibrato — ~3× real
>   choir wobble (5–15 c) and beyond what snap chroma holds at 2048 hop.
>   At realistic ±20 c (voice 1) / ±15 c (voice 2) the detector is green; the
>   ±42 c red state is documented as the known limit, next escalation is
>   NNLS-lite (Path B) if extreme-vibrato material ever shows up in the wild.

Research note for KeySync (JUCE 8 VST3, C++20, our pipeline: rolling 30 s window, 4096-pt FFT, hop 2048,
Hann, per-frame chroma from plain STFT bins 55 Hz–4 kHz with slight octave weighting, frame-normalized,
window-average, Pearson correlation vs Krumhansl major/minor profiles; 32 log bands 35 Hz–8 kHz already
per-frame for the tempo stage). License rule: GPL/AGPL code banned as dependency; GPL sources below are
concept-only, licenses checked per library.

## 1. Why sung material breaks plain per-frame STFT chroma

Our mapping assigns each FFT bin to one semitone class with a fixed grid. Vocals violate that:

1. **Vibrato/pitch drift smear energy across adjacent semitones.** ±30 cents vibrato at 5–7 Hz puts
   energy in neighbor bins every frame; over a 30 s window that dilutes the tonic/fifth contrast KS
   needs. NNLS-Chroma/Chordino docs state instruments with fluctuating pitches — "(also: voice)" —
   degrade tonal features, and ship per-frame local tuning on log-frequency spectra to re-align frames
   [4]; librosa instead estimates the recording's global tuning offset and uses it in chroma mapping
   because raw bin mapping assumes exact A440 equal temperament [1][2].
2. **Not tuned to A440.** Global detuning smears all material, but voices sit on few long contoured
   notes, so there is no averaging-out across many transient harmonic complexes as in busy mixes. Mode
   confusion is the classic downstream symptom: MIREX scoring weights relative/parallel confusion as
   the dominant failure mode [8]; S-KEY reports Krumhansl-profile detection at only 64.9 % mode accuracy
   on general material [9] — the profile operates near coin-flip on mode once chroma input degrades.
3. **Strong upper harmonics bias multi-voice chroma.** Voice harmonics land on 2f, 3f, 4f…, so choirs
   weight chroma towards fifths/overtones; octave-doubling across parts loads phantom pitch classes.
   NNLS Chroma targets this directly: deconvolve the log-frequency spectrum with a dictionary of
   harmonic notes (i-th partial ∝ shape^(i−1), shape 0.7), yielding a mid-range-emphasis semitone map
   not dominated by upper partials [4]; used as the MIREX 2010 feature extractor [5].
4. **Sibilance/broadband noise spreads evenly over the 12 classes**, shrinking all inter-class
   contrast. Counter-measure: spectral whitening, present in NNLS Chroma as bins "divided by [standard
   deviation of their neighbours]^[spectral whitening]", default strength 1.0 [4]. Distinct from (and
   complementary to) Stowell adaptive whitening below.
5. **No percussive anchor**; window-average chroma includes sung portamenti/glide frames where the grid
   is transiently misaligned. Median-filter HPSS removes bins not stable across time while keeping
   sustained harmonic energy [6][7].
6. **Profile side.** Corpus-trained alternative profiles (Albrecht–Shanahan) lift accuracy substantially
   on classical corpora: 93.1 % on 957 works vs earlier profiles [10] — matters for choral textures.

## 2. Techniques with evidence, ranked by fit to our codebase

**A. librosa-style global tuning estimate + tuned chroma mapping** [1][2][3].
`chroma_stft(..., tuning=None)`: if the A440 deviation (fractional chroma bins) is not supplied, librosa
estimates it automatically with `estimate_tuning` [1][2]. Source (librosa/core/pitch.py): `piptrack` —
thresholded parabolic peak tracking on the STFT (bin is a peak if above `threshold × per-frame max`,
threshold 0.1, search band 150–4000 Hz), then discard peak magnitudes below the *median* peak magnitude,
feed surviving peak frequencies to `pitch_tuning`: `residual = mod(12 * hz_to_octs(f), 1)`, wrap
(residual ≥ 0.5 → residual − 1), histogram into 100 bins over [−0.5, 0.5), argmax → tuning [3].
Parabola shift = −b/a, a = x[−1]+x[+1]−2x[0], b = (x[+1]−x[−1])/2 [3].
License: librosa is ISC [11]. Math above fully stated in citation; no code copy needed.

**B. NNLS Chroma / "Chordino" (Mauch / Cannam)** [4][5].
Pipeline per README [4]: spectrum → constant-Q log-frequency spectrum, **3 bins per semitone** → tuning
(global or local; "Local tuning is only advisable when the tuning is likely to change… or in a cappella
singing") → subtract background → "running standardisation" + *spectral whitening* (divide each
log-freq bin by std of its neighbours raised to a strength exponent, default 1.0) → NNLS approximate
transcription against a dictionary of harmonic notes → semitone spectrum → multiply by mid-range
emphasis pattern → map to 12 bins.
License: repo COPYING is GNU GPL v2 or later [12]; AUR marks GPL [13]. Banned as dependency;
concept-only. Cost: even "NNLS-lite" (coordinate descent on a 12–24-note dictionary of harmonic
templates on a ~72-band log-frequency spectrum, 10–20 iterations) is the biggest lift (~150–250 LOC).
Highest documented quality ceiling [5], highest implementation risk.

**C. HPSS then chroma on the harmonic part** [6][7].
librosa `decompose.hpss` [6]: median filters — horizontal (time axis) → harmonic spectrogram, vertical
(freq axis) → percussive, Wiener power-2 soft mask, margin extension per Driedger/Müller/Disch [7];
kernel_size defaults 31. `librosa.effects.harmonic` = STFT→HPSS→ISTFT round-trip returning the harmonic
waveform [6]. Mechanism for us: median across time suppresses any bin unstable for ≥ kernel/2 frames —
kills sibilance/broadband noise, keeps sustained tones; zero added latency since we re-analyze the
rolling buffer post-hoc. Cheap if done on our 32-band (or future 72-semitone-band) matrix rather than
all 2049 FFT bins.

**D. Spectral whitening (Stowell & Plumbley) and what "KeyFinder actually does"**
Premise correction for this task: **current libKeyFinder contains no whitening and no explicit tuning
estimation.** From the sources [14][15][16][17]:
- Preprocess: mono, LPF cutoff ≈ top-band × 1.012, downsample by sr/2/(top×1.10) [16].
- Chroma: 16384-sample FFT at downsampled rate; constant-Q-*like* transform summing magnitude over a
  cos(2πn/N)-shaped kernel spanning 72 semitone bands (6 octaves), kernel normalized by coefficient sum
  × band frequency ("models CQT very closely"), fixed stretch DIRECTSKSTRETCH = 0.8 × semitone spacing
  [14][15][17].
- Sum octaves → 12-D chroma → K-S classify [16][17].
Tuning tolerance comes from (a) the huge FFT producing fine bins and (b) the 0.8-stretch kernel width
(several tens of cents), not from re-mapping. Lesson for us at 4096: buy tuning tolerance via option A
(estimate + fractional re-map) or widen our bin kernels similarly.
Whitening as the task referenced it: Stowell–Plumbley (venue note: ICMC 2007, not DAFx): P[n,k] =
max(|X[n,k]|, r, m·P[n−1,k]); |X| ← |X|/P[n,k], floor r, memory m — per-bin running-peak normalization,
"no training, relatively lightweight, real-time" [18][19]. The practical variant Chordino actually ships
is the median-of-neighbour-std division form in [4]; both free to reimplement as concepts.

**E. Alternative profiles: Temperley & Albrecht–Shanahan** [10][20].
Temperley (2007) publishes corpus-derived major/minor profiles [20]; A&S (2013) train a new profile on
large classical corpora and report 93.1 % on 957 works, beating earlier profiles [10]. Drop-in for our
correlation step; worth an A/B toggle over our fixtures.

**F. Deep chroma (future note only).** Supervised chroma/key models exist (S-KEY 2025 reaches 79.0 %
mode accuracy vs Krumhansl baseline 64.9 % [9]) but ship trained weight blobs whose redistribution
licenses could not be source-verified this session. Not for v1; needs separate audit.

## 3. Cost given our existing 4096-FFT pipeline

Reuse of per-frame magnitude buffer (2049 bins) and tempo-stage 32-band matrix assumed. All operations
are array sums or small medians.

- **Tuning estimate (A, per-window):** parabolic shift only on candidate semitone-peak bins (reuse
  existing bin determination): a few hundred candidates per frame, shift = −b/a where |b| < |a|, reject
  |shift| ≥ 0.5, 100-bin residual histogram, argmax → tuning. One pass over window frames (not per
  hop), ~1–2 ms, one 100-float histogram. Re-map chroma by fractional neighbor-bin weighting shifted by
  the tuning fraction. ~60–90 lines.
- **Per-band whitening before chroma accumulation (A′, Stowell-lite):** running-peak array P (2049 bins
  or 72-band rows); per frame |X| ← |X| / max(|X|, floor, m·P), m ≈ 0.997. One extra max+mult per bin
  per frame (~2–4 kFLOP/frame, one array) — negligible next to the FFT. Replaces/extends our existing
  "slight octave weighting"; do not combine with per-frame sum normalization. ~25–40 lines.
- **HPSS-on-band-matrix (C-lite):** median of last 5 frames of each of 32 bands (band ring buffer
  already exists in the tempo stage) → per-band stability gate s = median/mean, chroma contributions
  weighted by s. 32 × 5 = 160 comparisons/frame. ~30–45 lines. Hop ≈ 46 ms → 5 frames ≈ 0.23 s; post-hoc
  on the analysis buffer, zero added latency.
- **NNLS-lite (B):** ~150–250 lines (dictionary ~40, coordinate descent ~60, semitone mapping
  restructure ~40, tests). Defer to v2 unless A + A′ + C fail the fixture suite.

Recommended stack for v1.x: **A + A′ + C**, all on existing frame data, ~120–180 LOC total.

## 4. Exact algorithms to lift (as concepts)

**librosa `estimate_tuning`** [3] (librosa/core/pitch.py):
1. STFT magnitudes S; per frame, peaks = bins with S > threshold(0.1) × per-frame max AND strict local max.
2. Parabolic shift s per peak: a = S[k−1]+S[k+1]−2S[k], b = (S[k+1]−S[k−1])/2, s = −b/a, valid when
   |b| < |a| (else 0); interpolated frequency f = (k + s)·sr/n_fft.
3. Keep peaks with magnitude ≥ **median** of all peak magnitudes (skew term droppable).
4. residual = mod(12·hz_to_octs(f), 1); wrap residual ≥ 0.5 → residual − 1.
5. 100-bin histogram over [−0.5, 0.5); argmax = tuning (fractions of a chroma bin).
For us: run over the already-computed 30 s window's FFT frames (not per hop), 150–4000 Hz — matches our
chroma band.

**KeyFinder (libKeyFinder) chroma pipeline** [14][15][16][17]:
1. Preprocess: mono; LPF cutoff topFreq × 1.012; downsample to ~2 × topFreq × 1.10.
2. FFT 16384 at downsampled rate (≈0.3 Hz bin at top of range), hop = frame/4, Blackman/Hamming window.
3. Chroma transform: for each of 72 semitone bands, a cos-shaped kernel (1 − cos(2πn/N), "discretely
   sampled continuous function") over FFT bins covering a stretched window of width = band-centre ×
   0.8 × (2^(1/12) − 1); coefficients normalized by sum × band-centre-frequency ("models CQT very
   closely"); chroma band = Σ magnitude × kernel coeff.
4. 72 → 12 by octave sum; K-S-style tone profiles for classification.
5. No whitening, no per-frame tuning; tolerance from big FFT + 0.8-stretch kernel width.

## 5. Test methodology without a big labeled dataset

### 5.1 Synthetic fixtures (ground truth free)
Additive "voice + choir pad + orchestra" over labeled chord voicings, synthesized by us — tonic/mode
labels are construction facts:
- Voice tones: partials 1/n, n ≤ 8, formant-boosted n = 2–5; vibrato {10, 20, 50} cents at 5.5 Hz; 60 ms
  onset glide.
- Choir pad: same note = voices at unison + octave offsets; octave-doubled partials create
  phantom-P5/11th pitch classes (the anti-mechanism-3 fixture).
- Chord voicing fixtures: root/3rd/5th triads of target key; for each of 12 keys **both major and minor
  of the same tonic** (24 sets). Validity gate: each fixture's own ideal 12-chroma correlated against
  Krumhansl major+minor must rank the intended label top-1, else the fixture is marked ambiguous (input
  problem, not detector failure).
- Sibilance: low-level white-noise bursts at voice attacks.
- Orchestra layer: thin bowed-string pad with slow portamento between chord tones.
- Suite ≈ 24 tonics × 2 modes × {10, 20, 50} cents × (no-choir / choir / full mix) — large enough to
  catch tuning/whitening/HPSS regressions cheaply.

### 5.2 Real-vocal protocol (~100 clips)
- 40 real vocal/choir loops with user-asserted tonic+mode (spot-checked via the mode-uncertain gating
  UI already used for BPM).
- Minimum 12 paired tonic-and-mode assertions: **12 loops whose texture exists in both a major and a
  minor version of the same tonic** (render pairs in a DAW by transposing/recoloring — only way
  pair-level asserts share texture).
- Scoring = MIREX Audio Key Detection weights [8]: 1.0 correct / 0.5 fifth / 0.3 relative / 0.2
  parallel / 0 otherwise. Report per-texture (a cappella / choir+pad / choir+orch) mean plus mode-only
  accuracy.
- Every change (A, A′, C) is diff-tested against the current build on the same fixtures; ships only if
  real-vocal weighted score improves without regressing the committed synthetic instrumental SelfTest
  pass rate.

## Sources

[1] librosa `chroma_stft` docstring (source, librosa repo) — "tuning… If None, it is automatically
estimated"; Gaussian octave weighting (ctroct/octwidth).
https://github.com/librosa/librosa/blob/main/librosa/feature/spectral.py
[2] librosa filter chroma — filter bank taking `tuning` (fractional bin deviation).
https://github.com/librosa/librosa/blob/main/librosa/filters.py
[3] librosa `estimate_tuning`/`pitch_tuning`/`piptrack` source (librosa/core/pitch.py) — parabolic peak
interpolation, median-magnitude keep-mask, residual = mod(12·hz_to_octs(f), 1) wrapped at 0.5, 100-bin
histogram argmax; doc cites
https://ccrma.stanford.edu/~jos/sasp/Sinusoidal_Peak_Interpolation.html
https://github.com/librosa/librosa/blob/main/librosa/core/pitch.py
[4] NNLS Chroma & Chordino README (c4dm/nnls-chroma) — 3 bins/semitone log-freq spectrum; global/local
tuning ("…or in a cappella singing"); running standardisation; spectral whitening = divide bin by
std-of-neighbours^strength (default 1.0); NNLS harmonic dictionary (partial i ∝ shape^(i−1), shape 0.7);
mid-range pattern; consonance note: "Instruments with fluctuating pitches (also: voice)…"
https://github.com/c4dm/nnls-chroma/blob/master/README
[5] Mauch, M., Dixon, S. — "Approximate Note Transcription for the Improved Identification of Difficult
Chords", ISMIR 2010 (reference named in [4]). https://github.com/c4dm/nnls-chroma
[6] librosa `decompose.hpss` + `effects.hpss/harmonic` docstrings — median-filter HPSS, kernel_size
default 31, Wiener power-2 soft mask, margin; STFT→HPSS→ISTFT wrappers.
https://github.com/librosa/librosa/blob/main/librosa/decompose.py (also `/librosa/effects.py`)
[7] Fitzgerald, D. — "Harmonic/percussive separation using median filtering", DAFx-10, Graz 2010;
Driedger, Müller, Disch — "Extending harmonic-percussive separation of audio", ISMIR 2014 (cited in [6]).
[8] MIREX Wiki, Audio Key Detection — weighted score: 1.0 same / 0.5 fifth / 0.3 relative / 0.2
parallel. https://music-ir.org/mirex/wiki/2025:Audio_Key_Detection
[9] S-KEY: Self-supervised Learning of Major and Minor Keys from Audio, arXiv:2501.12907 — Krumhansl
baseline MIREX 53.4 %, mode accuracy 64.9 %; S-KEY mode accuracy 79.0 %.
https://arxiv.org/html/2501.12907v1
[10] Albrecht, J., Shanahan, D. (2013) — "The Use of Large Corpora to Train a New Type of Key-Finding
Algorithm", Music Theory Spectrum 35(2); 93.1 % on 957 classical works (third-party reproduction in
Empirical Musicology Review commentary).
https://www.researchgate.net/publication/259731554_The_Use_of_Large_Corpora_to_Train_a_New_Type_of_Key-Finding_Algorithm
https://emusicology.org/article/4749/galley/5215/download
[11] librosa license: ISC. https://github.com/librosa/librosa/blob/main/LICENSE.md
[12] nnls-chroma license: GNU GPL v2 or later (COPYING).
https://github.com/c4dm/nnls-chroma/blob/master/COPYING
[13] AUR nnls-chroma package: "Licenses: GPL". https://aur.archlinux.org/packages/nnls-chroma/
[14] libKeyFinder chromatransform.cpp — cos-shaped kernel (1 − cos(2πn/N)), stretched window
DIRECTSKSTRETCH 0.8, per-band normalization Σcoeff × band-frequency, "models CQT very closely", 72
bands. https://github.com/ibsh/libKeyFinder/blob/master/chromatransform.cpp
[15] libKeyFinder constants.h — OCTAVES 6, BANDS 72, FFT frame 16384, hop 4096, DIRECTSKSTRETCH 0.8.
https://github.com/ibsh/libKeyFinder/blob/master/constants.h
[16] libKeyFinder keyfinder.cpp + spectrumanalyser.cpp — preprocess: mono, LPF cutoff topFreq × 1.012,
downsample to ~2 × topFreq × 1.10; frame windowing; per-frame chroma → chromagram.
https://github.com/ibsh/libKeyFinder/blob/master/keyfinder.cpp (also `spectrumanalyser.cpp`)
[17] libKeyFinder README — "published under the GNU General Public License version 3 or later"
(concept-only for us); KeyFinder project page + accuracy comparison PDF.
https://github.com/ibsh/libKeyFinder · https://www.ibrahimshaath.co.uk/keyfinder/
[18] Stowell, D., Plumbley, M. — "Adaptive whitening for improved real-time audio onset detection",
ICMC 2007, pp. 312–319 (per-bin running-peak memory: P = max(|X|, r, m·P), |X| ← |X|/P). (Venue note:
ICMC, not DAFx as the task prompt assumed.)
[19] ISMIR 2012 onset-detection evaluation reproducing the same adaptive-whitening iteration as its
Eq. (2) — independent verification of the formula.
https://archives.ismir.net/ismir2012/paper/000049.pdf
[20] Temperley, D. (2007) — *Music and Probability*, MIT Press (corpus-based major/minor profiles).
https://mitpress.mit.edu/9780262201676/

## Recommended approach

**Verdict for our codebase.** Rank by (real-vocal robustness expected) × (implementation cost):

1. **Path A — librosa-style tuning-corrected + whitened chroma (~120–180 LOC).** Global tuning estimate
   per 30 s window on candidate STFT peaks (reuse our chroma bin range), re-frame per-frame chroma
   around the estimated offset, plus Stowell-lite running whitening on the magnitude column before band
   accumulation. Addresses mechanisms 1–2; cheap, additive to the existing frame loop; no license risk
   (concepts from ISC-licensed librosa [11] and the ICMC paper [18]). Largest expected gain for
   off-pitch material — fits our live-mixer use case, where project tuning is whatever the user runs.
2. **Path C — median-HPSS lite on our band matrix (~30–60 LOC).** Post-pass on the existing 32 bands (or
   a future 72-band log spectrum) before chroma accumulation. Removes sibilance/noise and unstable
   portamento frames (mechanism 5). Zero added latency — post-hoc on the analysis buffer.
3. **Path B — NNLS-lite / Chordino-style deconvolution (~150–250 LOC, defer to v2).** Only if A + C
   underperform on the real-vocal fixtures; it handles mechanism 3 (upper-partial weighting) properly,
   at the highest risk. GPL v2+ reference [12] means concept-only re-derivation from the ISMIR 2010
   paper [5].

Include with Path A: K-S ↔ Albrecht–Shanahan profile A/B toggle (~20 LOC in our correlation step) to
test mechanism 6 on our fixtures [10].

Not pursued: KeyFinder's 16384-frame CQT-style kernel rewrite — too invasive for our 4096 pipeline;
kept instead as inspiration for widening our per-semitone bin windows around the estimated tuning, same
idea on an FFT we already pay for [14][15].
