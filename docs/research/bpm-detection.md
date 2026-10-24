# BPM (Tempo) Detection for KeySync — Research Notes

Studied 2026-10-24 for v1.1 (BPM = FREE feature). Scope: tempo estimation inside the
existing offline key detector (15 s ring on the audio thread, analysis on message
thread, zero external deps, GPL/AGPL banned as dependencies). All claims cite primary
sources (§Sources at bottom); GPL/AGPL may be studied for algorithm *concepts*, never
copied (§Licensing).

## 0. What we already store (from our own source/dsp/KeyDetector.h/.cpp)

- FFT size 4096 (`kFftOrder=12`), hop 2048 (`kHopSize = kFftSize/2`), Hann window,
  real-only forward transform on the audio thread in `processPendingFrames()`.
- Frame cadence: **21.53 fps @ 44.1 kHz / 23.44 fps @ 48 kHz** (2048-sample hop).
  The 15 s ring holds `ceil(15·sr/2048)+4` slots = **327 @ 44.1 kHz / 356 @ 48 kHz**.
- Per frame we store **only the 12-bin chroma** (`ChromaSlot.chroma`, seqlock). Frame
  RMS is computed as a silence gate then **discarded**; FFT magnitudes are consumed by
  `chromaFromFrame()` then discarded. No spectrum/RMS/flux history exists.
- `performAnalysis()` seqlock-reads this ring on the message thread; a BPM stage can
  read the same ring the same way.

Extra per-frame storage each BPM option must consider:

| Addition | Size @44.1 kHz (327 slots) | Where computed |
|---|---|---|
| frame RMS (1 float) | 1.3 KB | audio thread — already computed, just keep it |
| band-flux scalar (1 float) | 1.3 KB | audio thread (needs prev-frame magnitudes) |
| 32 log-band magnitudes (32 × float) | 41.9 KB | audio thread — from existing FFT output |
| full magnitude spectrum (2048 × float) | 2.68 MB | not worth it |

## 1. Algorithm options

### 1.1 librosa (BSD): onset strength → tempogram → prior-weighted argmax

Actual algorithm from librosa source:

- **Onset strength (ODF).** `ODF[t] = mean_f max(0, S[f,t] − ref[f,t−lag])` — half-wave
  rectified difference of a log-power **mel spectrogram**; defaults `n_fft=2048`,
  `hop=512`, `lag=1`, `max_size=1` (so `ref=S`), aggregate = mean over mel bins [1][2].
  Spectral flux on log-mel bands, not RMS energy. Note: onset peak-picking defaults
  "were chosen by large-scale hyper-parameter optimization over the dataset"
  (CPJKU/onset_db) [2].
- **Tempogram.** A Hann window (default `win_length=384` frames ≈ 8.9 s) slides over
  the ODF with `hop_length=1` — every frame is an autocorrelation window origin (this
  overlap-over-origins design is what earlier librosa docs described as "interleaved
  autocorrelation with origin offsets"); each window is autocorrelated and
  ℓ∞-normalized [1].
- **Tempo.** `tempo()` aggregates the tempogram over time (default `np.mean`), converts
  integer lag L to BPM via `bpm = 60·sr/(L·hop)` (`tempo_frequencies`) [3], weights the
  lag axis with a **log-normal prior centered `start_bpm=120`, `std_bpm=1.0`**
  (log2-octave units), clamps above `max_tempo=320`, then
  `argmax(log1p(1e6·tempogram) + logprior)` (`log1p` for stability); autocorrelation
  window defaults `ac_size=8.0` s [1]. The sr=22050/hop=512 (~43 fps) defaults are
  librosa's library-wide speed/quality convention.

librosa also ships Ellis' dynamic-programming beat tracker (`beat_track`; Ellis 2007)
[4] — beat *phase*, more depth than we need for a BPM number.

### 1.2 Essentia (AGPL-3): two pipelined reference designs

Documented (pages) vs. needs-source-reading, marked:

- **RhythmExtractor2013** wraps a beat tracker (`method=multifeature` →
  `BeatTrackerMultiFeature`), returns bpm/ticks/confidence; defaults minTempo=40,
  maxTempo=208 [5].
- **BeatTrackerMultiFeature** (documented) computes **five onset detection functions**
  (complex spectral difference, RMS/energy flux, mel-band flux, beat emphasis,
  information-gain flux), gets beat candidates from each via `TempoTapDegara`, fuses
  them with `TempoTapMaxAgreement`, outputs a **confidence in [0, 5.32]** with
  interpretation bands (§3); requires 44.1 kHz; internal fusion needs source reading [6].
- **BpmHistogramDescriptors** (documented) turns beat intervals into a **BPM histogram +
  first/second peak weight and "spread"** [7]; internals need source reading.
- **PercivalBpmEstimator** (page documented; pipeline confirmed in source [8][9]):
  hamming 1024-pt frames @ hop 128 → spectrum → log-magnitude → **half-rectified L1
  Flux** → 15-tap low-pass FIR (~7 Hz) = ODF → 2048-lag autocorrelation →
  `PercivalEnhanceHarmonics` (sharpens multiples of a candidate lag) → peak detection in
  lag range (minBPM=50/maxBPM=210, ≤10 peaks) → score candidates by cross-correlating
  the ODF with a **pulse train** → Gaussian accumulation = smooth BPM histogram → argmax
  lag → `bpm = 60·sr_OSS/lag` → **octave decider**: SVM over `energyUnder` (normalized
  histogram energy below the winning lag), `str05` (near half the lag), bpm →
  multiplier 1, 2, or 0.5. Reference: Percival & Tzanetakis 2014, TASLP 22(12) [10].

### 1.3 aubio (GPL)

`aubio tempo` docs: defaults `bufsize 1024`, `hop 512`; onset methods include
`energy`, `hfc`, `complex`, `specflux`, KL/mKL; the tracker implements **Davies &
Plumbley 2004, "Causal tempo tracking of audio"** [11]. Library primary reference:
Brossier PhD thesis, QMUL 2006 [12]. Repo license metadata: GPL-3.0; `COPYING` =
GPL-3 text [16]. Study only; never copy/link.

### 1.4 Simple alternatives for non-percussive content

- **Energy/RMS flux + autocorrelation + comb filter.** The minimal pipeline. The comb
  (harmonic-weighted ACF / pulse-train cross-correlation) handles the case where only
  every 2nd/4th flux event fires: Percival's enhance-harmonics + pulse-train scoring
  is exactly this [9][10]; Böck et al.'s resonating comb filter is a stronger
  variant [13].
- **Fourier tempogram** (librosa `fourier_tempogram`; Grosche/Müller/Kurth ICASSP 2010
  as cited in librosa source): cosine-projection view of the ODF that keeps phase and
  works where flat autocorrelation blurs; `plp` (Grosche & Müller 2011) extracts a
  "predominant local pulse" from it [1][4].
- **Goto 2001** (JNMR 30(2)): beat tracking "with or without drum-sounds" — canonical
  no-drum citation, but its chord-change hypothesis machinery is far over budget [14].
- **Do chroma frames suffice as flux source?** Partly. Chroma flux captures chord/note
  onsets, but our chroma is per-frame sum-normalized [our source], which removes
  loudness and flattens real onsets — unreliable primary signal. Primary-source
  consensus (librosa defaults [1][2], Essentia's five ODFs [6], aubio
  specflux/complex/KL [11], Percival log-magnitude flux [9]): **log-band spectral flux
  from raw magnitudes is the standard ODF**; RMS/energy flux is the cheap plan B.
  Store band energies from our existing FFT rather than reusing chroma alone.

## 2. Half/double (octave) errors and default ranges

Remedies (primary-sourced):

1. **Hard clamp** to `minBPM..maxBPM` (Essentia RhythmExtractor2013 default 40–208 [5];
   PercivalBpmEstimator 50–210 [8]).
2. **Log-normal prior / tempo bias** — librosa weights the lag axis by
   `exp(−0.5·((log2(bpm)−log2(120))/1)²)` with `max_tempo=320` [1][3].
3. **Harmonic weighting** of the ACF at multiples of a candidate lag, so a pulse at 2×
   the beat still supports the slower fundamental [9][13].
4. **Explicit 2×/0.5× decision** from ratio features — Percival's SVM compares
   histogram energy below the winner vs at half the lag and may output ×2 or ÷0.5 [9].
   Non-learned surrogate: comb score at L, L/2, 2L; prefer the octave family (L, 2L,
   L/2 within range) with total support.

**Recommended default for us: 40–208 BPM** (Essentia's parameterization [5]) with the
120-BPM lognormal prior as tie-breaker [3]; a narrower "producer" variant 50–190 still
contains all octave-legal values of the 120-BPM center and rejects most flips. Task
brief's "60–180" is folklore; real documented defaults are in [1][5][8].

## 3. Confidence / quality metric

- **BpmHistogramDescriptors** emits `firstPeakWeight`/`firstPeakSpread` /
  `secondPeakWeight` — BPM-histogram peak prominence + spread, natural cheap confidence
  proxies [7]; Percival's octave-decider features (`energyUnder`, `str05`) double as
  tempo-stability scores [9]; with the tempogram ℓ∞-normalized [1], the ACF
  winning-peak height vs neighbors is a peak-state ratio.
- **Real software scoring**: Essentia's calibrated confidence in [0, 5.32]: [0,1) "very
  low — input is hard for the employed candidate beat trackers", [1,1.5] low,
  (1.5,3.5] "good — accuracy around 80% in AMLt measure", (3.5,5.32] excellent,
  traceable to Zapata et al. ISMIR 2012 [6].
- Cheap ML-free combo: `score = (ACF peak/mean inside clamp) × margin over best rival
  octave`; buckets <0.50 "uncertain", 0.50–0.75 "likely", ≥0.75 "good". Honest failures:
  flat ACF (silence, sustained pad, noise) → "uncertain"; strong ACF but unresolved
  octave rivals → report tempo at low confidence.

## 4. Offline cost sized to OUR frame rate

Analysis milliseconds, audio thread unchanged except per-frame magnitude bookkeeping:

- Onset representation kept from the existing FFT: a few thousand mult-adds per frame
  at 21.5/23.4 fps — **no new FFT**.
- **Upsampled ODF**: ×4 linear interpolation → ~1300 points ≈ 86 fps (librosa uses 43).
  Needed because integer-lag resolution at native 21.5 fps is coarse: `dBPM/dL =
  −60·fps/L²` (from `bpm=60·fps/L` [3]) — at 161 BPM one lag step ≈ 20 BPM, at 81 BPM
  ≈ 5 BPM; parabolic interpolation recovers most loss but ×4 is free.
- **Global autocorrelation**: lags ≈ 29–129 for 40–208 BPM over ~1300 points ≈ **168 k
  MACs** — microseconds; direct non-FFT ACF fine at this size.
- Fourier tempogram variant: one 512-pt DFT (optional); comb/pulse-train scoring of
  5–10 candidate lags, harmonics ≤4×: < 5 k MACs.

Total added message-thread work **< 1 ms** (existing key correlation is slower).

## 5. Recommendation matrix for OUR case

Ranking constraint recap: reuse the ring (no rate change), C++20, `{bpm, confidence,
valid}`, honest "uncertain" for silence/atonal/no-percussion, ≤300 lines.

**#1 — log-band flux ODF + clamped, prior-weighted, octave-checked autocorrelation
(librosa + Percival hybrid), ~220 lines.** Add `float band[32]` per ChromaSlot
(log-spaced 35 Hz–8 kHz energies from the existing FFT; audio thread; active frames
only). In `performAnalysis()`: equalized flux `ODF[t] = Σ_b max(0, log(b_b[t]+ε) −
log(b_b[t−1]+ε))` [2][9]; ×4 upsample; direct ACF, lags 29→129 (40–208 BPM [5]);
lognormal 120-BPM prior + `log1p` argmax [3]; parabolic refine; Percival-derived (not
copied) octave check on comb scores at L, 0.5L, 2L [9]. Memory +41.9 KB @44.1 kHz.
Confidence per §3. **#2** — chroma-flux fallback (~80 lines): ODF from positive chroma
flux, needs the *unnormalized* chroma sum stored too (+1.3 KB). Fragile on clean drum
content and octave-unreliable; fallback only. **#3** — Fourier tempogram cross-check
(~60 lines, add-on to #1): single 512-pt DFT of the ODF, bins via
`bpm_bin = 60·fps·k/win` [3]; agreement raises confidence, disagreement lowers it and
flags "unstable tempo" — targets the weak-beat/no-drum case per Grosche et al. [1].

Not now: Ellis-DP beat tracing [4], Zapata multi-ODF fusion [6], Goto-style
hypothesis tracking [14], neural ODFs (Böck RNN/comb) [13].

| Param | Value | Source of value |
|---|---|---|
| ODF source | 32 log bands, 35 Hz–8 kHz, log-magnitude, half-rectified | [2][9] |
| Band storage | 32 × float/frame in ring (41.9 KB @44.1 kHz) | §0 |
| ODF rate | 21.53 fps native → ×4 upsample → 86.1 fps | §4 |
| Tempo range | 40–208 BPM + 120-BPM lognormal prior | [5][3] |
| ACF lag window | up to L_max = 60·fps/40 ≈ 129 interpolated lags | §4 |
| Octave check | comb scores at L, 2L, L/2 (keep in-range) | [9] |
| Confidence | ω_peak/mean × octave margin → 0.50/0.75 buckets | §3, [7] |
| Uncertain states | silence, flat ACF (peak/mean ≲ 1.3), all-octave tie | §3 |

## 6. Testing

Domain practice: papers score against annotated datasets (Ballroom / Extended Ballroom,
GTZAN tempo, LMD-tempo) under a tolerance — MIREX's standing criterion, restated in
Böck et al. 2015, is "identified correctly within a maximum allowed deviation of ±8%
from the ground truth" [13]. librosa's tests for `tempo` are synthetic click fixtures
asserting the estimate lands on one tempo-family candidate, not the exact BPM [15].

Three selftest ideas runnable in `keysync_selftest`:

1. **Synthetic impulse grid**: click trains (quarter notes, ~15 s) at 60/90/120/128/140/
   180 BPM → assert |bpm−x| ≤ 1.5; same at 16th-note density → assert octave-family
   recovery, with the "within 8%" tier as the loose pass and ±4% as target [13].
2. **Octave honesty**: eighth-note train at 220 BPM and 55 BPM half-time → estimate
   must stay in range with penalty (confidence < 0.75) rather than a silent lie.
3. **Uncertain-state fixtures**: silence → invalid/uncertain; sustained sine or pad
   (no periodicity) → confidence < 0.5; white noise → no tempo reported.

## Licensing notes (concept port vs code copy)

- **aubio = GPL-3** [16]. GPL §5c: "You must license the entire work, as a whole, under
  this License to anyone who comes into possession of a copy." GPL §2 includes a
  translation into another language in the definition of a work based on the Program,
  so even a cleaned-up line-by-line translation of aubio's tracker must be GPL.
  **Porting the concept is safe**: 17 U.S.C. §102(b): "In no case does copyright
  protection for an original work of authorship extend to any idea, procedure, process,
  system, method of operation, concept, principle, or discovery, regardless of the form
  in which it is described, explained, illustrated, or embodied in such work."
  Implement from the mathematical description only; never transcribe structure,
  identifiers, or comment flow.
- **Essentia = AGPL-3** (COPYING.txt is the AGPL text) [17]. §5 coverage same as GPL;
  §13 adds an interactive-server source offer — irrelevant to a desktop plugin but the
  distribution clause matched GPL's. Study the algorithms, reimplement from
  descriptions, cite inspiration if desired, copy nothing. Quoting factual parameter
  defaults (BPM ranges, hop sizes) from the docs is not code copying.

## Sources

1. librosa source, `feature/rhythm.py` (`tempogram`, `fourier_tempogram`, `tempo`, `tempogram_ratio`) — github.com/librosa/librosa/blob/main/librosa/feature/rhythm.py
2. librosa source, `onset.py` (`onset_strength`, `onset_strength_multi`; peak-picking defaults "chosen by large-scale hyper-parameter optimization") — github.com/librosa/librosa/blob/main/librosa/onset.py
3. librosa source, `core/convert.py` (`tempo_frequencies`, `time_to_frames`) — github.com/librosa/librosa/blob/main/librosa/core/convert.py
4. librosa 0.8.1, `beat.py` (Ellis 2007 DP tracker; `plp`; Grosche refs) — github.com/librosa/librosa/blob/0.8.1/librosa/beat.py
5. Essentia RhythmExtractor2013 reference — https://essentia.upf.edu/reference/std_RhythmExtractor2013.html
6. Essentia BeatTrackerMultiFeature reference (five ODFs, confidence bands, cites Zapata et al. ISMIR 2012, "Assigning a confidence threshold on automatic beat annotation") — https://essentia.upf.edu/reference/std_BeatTrackerMultiFeature.html
7. Essentia BpmHistogramDescriptors reference — https://essentia.upf.edu/reference/std_BpmHistogramDescriptors.html
8. Essentia PercivalBpmEstimator reference — https://essentia.upf.edu/reference/std_PercivalBpmEstimator.html
9. Essentia `percivalbpmestimator.cpp` (flux → ACF → pulse trains → Gaussian histogram → hard-coded SVM octave decider) — github.com/MTG/essentia/blob/master/src/algorithms/rhythm/percivalbpmestimator.cpp
10. Percival & Tzanetakis 2014, "Streamlined Tempo Estimation Based on Autocorrelation and Cross-Correlation With Pulses," IEEE/ACM TASLP 22(12):1765–1776 (author-deposited full text) — https://www.researchgate.net/publication/265130658
11. aubio CLI docs (tempo/beat defaults, ODF method list; cites Davies & Plumbley 2004, "Causal tempo tracking of audio," ISMIR) — https://aubio.readthedocs.io/en/latest/cli.html
12. P. Brossier, "Automatic Annotation of Musical Audio for Interactive Applications," PhD thesis, QMUL 2006 (aubio primary reference) — https://aubio.org/phd/thesis/brossier06thesis.pdf
13. Böck, Krebs & Widmer 2015, "Accurate Tempo Estimation based on Recurrent Neural Networks and Resonating Comb Filters," ISMIR 2015 (§4.4 states the MIREX ±8% criterion) — https://www.cp.jku.at/research/papers/Boeck_etal_ISMIR_2015.pdf
14. Goto 2001, "An audio-based real-time beat tracking system for music with or without drum-sounds," JNMR 30(2) — https://staff.aist.go.jp/m.goto/PAPER/JNMR2001goto.pdf
15. librosa test suite (synthetic tempo fixtures pattern) — github.com/librosa/librosa/tree/main/tests
16. GNU GPL v3 text (aubio's `COPYING`); repo license metadata — https://www.gnu.org/licenses/gpl-3.0.txt ; github.com/aubio/aubio
17. Essentia license (AGPL-3.0) and licensing notes — github.com/MTG/essentia/blob/master/COPYING.txt ; github.com/MTG/essentia/blob/master/README.md
18. 17 U.S. Code §102(b) — https://www.law.cornell.edu/uscode/text/17/102

**Recommended approach.** Implement #1: in the existing audio-thread
`processPendingFrames()` loop, when the frame FFT is already computed, also store 32
log-band magnitudes (+RMS) in the seqlock slot (+41.9 KB @44.1 kHz, no new FFT, no new
ring rate); in `performAnalysis()` — where `detect()` already seqlock-reads the ring —
build a half-rectified log-band flux ODF [2][9], upsample ×4, autocorrelate over the
40–208 BPM clamp [5] with the 120-BPM lognormal prior and `log1p + logprior` argmax
[3], refine parabolically, run the Percival-derived non-copied octave check (comb
scores at L, 2L, L/2 [9]), and emit `{bpm (float), confidence, "uncertain"}` buckets
<0.5 uncertain / 0.5–0.75 likely / ≥0.75 good, extended honestly for silence, atonal
pads, and octave ties. ~220 lines + ~40 selftest fixtures; zero deps, zero GPL
exposure.

Unverified (risks): the MIREX wiki page was unreachable for fetch, so the ±8%
tolerance is cited via Böck 2015's statement of the MIREX criterion [13]; the Percival
2014 PDF was not fully fetched — the pipeline description leans on the Essentia
implementation, itself that paper's reference implementation [9][10].
