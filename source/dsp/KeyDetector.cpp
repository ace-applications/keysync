#include "KeyDetector.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace keysync
{

namespace
{
constexpr const char* kPitchNames[kNumPitchClasses] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// Krumhansl-Schmuckler key profiles.
constexpr double kMajorProfile[kNumPitchClasses] =
    { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
constexpr double kMinorProfile[kNumPitchClasses] =
    { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

// Pearson correlation of two 12-bin vectors.
double correlation (const double* a, const double* b)
{
    double meanA = 0.0, meanB = 0.0;
    for (int i = 0; i < kNumPitchClasses; ++i)
    {
        meanA += a[i];
        meanB += b[i];
    }
    meanA /= (double) kNumPitchClasses;
    meanB /= (double) kNumPitchClasses;

    double num = 0.0, denA = 0.0, denB = 0.0;
    for (int i = 0; i < kNumPitchClasses; ++i)
    {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        num += da * db;
        denA += da * da;
        denB += db * db;
    }
    const double den = std::sqrt (denA) * std::sqrt (denB);
    return den > 0.0 ? num / den : 0.0;
}
} // namespace

const char* pitchClassName (int pc) { return kPitchNames[wrapPc (pc)]; }

juce::String keyTitle (const KeyResult& r)
{
    if (! r.valid || r.tonicPc < 0)
        return "?";
    return juce::String (pitchClassName (r.tonicPc)) + " " + r.modeText();
}

void KeyDetector::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    window.resize ((size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) (kFftSize - 1)));

    pending.assign ((size_t) kFftSize + (size_t) kMaxBlockSamples, 0.0f);
    pendingSize = 0;
    frameData.assign ((size_t) kFftSize * 2, 0.0f);
    frameMags.assign ((size_t) kFftSize / 2 + 1, 0.0);

    const int windowFrames = (int) std::ceil ((kAnalysisWindowSeconds * sampleRate) / (double) kHopSize) + 4;
    ringCapacity = windowFrames;
    frameRing.reset (new ChromaSlot[(size_t) windowFrames]); // default-constructed, no copies
    ringWrite = 0;
}

void KeyDetector::reset()
{
    pendingSize = 0;
    ringWrite = 0;
    for (int i = 0; i < ringCapacity; ++i)
        frameRing[(size_t) i].seq.store (0, std::memory_order_release);
}

void KeyDetector::addMono (const float* samples, int numSamples)
{
    if (sampleRate <= 0.0)
        return;

    const int capacity = (int) pending.size();
    const int copy = juce::jmin (numSamples, capacity - pendingSize);
    if (copy <= 0)
        return;

    std::memcpy (pending.data() + pendingSize, samples, sizeof (float) * (size_t) copy);
    pendingSize += copy;

    processPendingFrames();
}

void KeyDetector::processPendingFrames()
{
    while (pendingSize >= kFftSize)
    {
        // frame RMS on the raw (unwindowed) samples
        double sqrSum = 0.0;
        for (int i = 0; i < kFftSize; ++i)
            sqrSum += (double) pending[(size_t) i] * (double) pending[(size_t) i];
        const double rms = std::sqrt (sqrSum / (double) kFftSize);

        // every hop enters the ring on a uniform time grid (the tempo stage needs
        // adjacent-evenly-spaced frames); silent frames publish an empty payload
        std::array<float, kNumPitchClasses> chroma {};
        std::array<float, kTempoBands> bands {};
        if (rms >= kRmsThreshold)
        {
            for (int i = 0; i < kFftSize; ++i)
                frameData[(size_t) i] = pending[(size_t) i] * window[(size_t) i];
            for (int i = kFftSize; i < kFftSize * 2; ++i)
                frameData[(size_t) i] = 0.0f;

            fft.performRealOnlyForwardTransform (frameData.data(), true);

            chromaFromFrame (frameData.data(), kFftSize, sampleRate, chroma, bands);
        }

        // seqlock publish: book a slot, mark in-progress, fill, mark stable
        auto& slot = frameRing[(size_t) ringWrite];
        ringWrite = (ringWrite + 1) % ringCapacity; // next frame overwrites oldest
        slot.seq.fetch_add (1, std::memory_order_release);    // odd: mid-write
        slot.chroma = chroma;
        slot.bands = bands;
        slot.seq.fetch_add (1, std::memory_order_release);    // even: stable

        // shift the next hop of samples to the front
        const int remaining = pendingSize - kHopSize;
        std::memmove (pending.data(), pending.data() + kHopSize, sizeof (float) * (size_t) remaining);
        pendingSize = remaining;
    }
}

// chroma from one FFT frame. Vocal-robustness journey (see docs/research/
// vocal-key-detection.md and the A/B notes there): Stowell whitening, librosa
// tuning estimation, fractional bin splitting and HPCP magnitude compression
// all REGRESSED real music and were removed; the survivor is the max-per-class
// chroma view consumed by detect() alongside the mean view.
void KeyDetector::chromaFromFrame (const float* fftData, int fftSize, double sr,
                                   std::array<float, kNumPitchClasses>& chroma,
                                   std::array<float, kTempoBands>& bands)
{
    chroma = {};
    bands = {};
    const int halfBins = fftSize / 2;

    // magnitudes, in one pass
    std::vector<double>& mags = frameMags;
    for (int bin = 0; bin < halfBins; ++bin)
    {
        const double re = fftData[2 * bin];
        const double im = fftData[2 * bin + 1];
        mags[bin] = std::sqrt (re * re + im * im);
    }
    double sum = 0.0;
    for (int bin = 0; bin < halfBins; ++bin)
    {
        double mag = mags[bin];
        if (mag <= 0.0)
            continue;
        const double freq = (double) bin * sr / (double) fftSize;

        // tempo bands consume raw magnitudes
        if (freq >= 35.0 && freq <= 8000.0)
        {
            const int band = juce::jlimit (0, kTempoBands - 1,
                (int) (kTempoBands * (std::log (freq / 35.0) / std::log (8000.0 / 35.0))));
            bands[(size_t) band] += (float) mag;
        }

        // chroma: middle-octave weighting, snapped to nearest semitone class
        if (freq < 55.0 || freq > 4000.0)
            continue;

        const double midi = 69.0 + 12.0 * std::log2 (freq / 440.0);

        const double octaveWeight = (midi < 48.0 || midi > 84.0) ? 0.5 : 1.0;

        const int cls = ((int) std::lround (midi) % 12 + 12) % 12;
        chroma[(size_t) cls] += (float) (mag * octaveWeight);
        sum += mag * octaveWeight;
    }
    if (sum > 0.0)
        std::for_each (chroma.begin(), chroma.end(),
                       [sum] (float& c) { c /= (float) sum; });
}
// seqlock read pattern — false when the frame is unpublished or mid-write
bool KeyDetector::tryReadFrame (const ChromaSlot& slot, ChromaSlot& out) const
{
    const uint32_t s1 = slot.seq.load (std::memory_order_acquire);
    if (s1 == 0 || (s1 & 1u) != 0)
        return false;
    out.chroma = slot.chroma;
    out.bands = slot.bands;
    std::atomic_thread_fence (std::memory_order_acquire);
    return slot.seq.load (std::memory_order_relaxed) == s1;
}

// normalized chroma sums to ~1 for any frame with tonal content, 0 when silent
static bool frameHasContent (const std::array<float, kNumPitchClasses>& chroma)
{
    float sum = 0.0f;
    for (float c : chroma)
        sum += c;
    return sum > 0.25f;
}

int KeyDetector::stableFrameCount() const
{
    ChromaSlot tmp;
    int count = 0;
    for (int i = 0; i < ringCapacity; ++i)
        if (tryReadFrame (frameRing[(size_t) i], tmp) && frameHasContent (tmp.chroma))
            ++count;
    return count;
}

KeyResult KeyDetector::detect() const
{
    KeyResult out;

    std::array<double, kNumPitchClasses> avg {};
    std::array<double, kNumPitchClasses> maxc {}; // max per class across frames
    int count = 0;
    ChromaSlot tmp;
    std::vector<std::array<float, kNumPitchClasses>> chromaFrames;
    std::vector<std::array<float, kTempoBands>> bandFrames;
    chromaFrames.reserve ((size_t) ringCapacity);
    bandFrames.reserve ((size_t) ringCapacity);
    for (int i = 0; i < ringCapacity; ++i)
        if (tryReadFrame (frameRing[(size_t) i], tmp))
        {
            if (frameHasContent (tmp.chroma))
            {
                for (int pc = 0; pc < kNumPitchClasses; ++pc)
                {
                    avg[(size_t) pc] += tmp.chroma[(size_t) pc];
                    maxc[(size_t) pc] = std::max (maxc[(size_t) pc],
                                                  (double) tmp.chroma[(size_t) pc]);
                }
                ++count;
            }
            chromaFrames.push_back (tmp.chroma); // full time grid for the tempo stage
            bandFrames.push_back (tmp.bands);
        }

    if (count < kMinActiveFrames)
        return out;

    double energy = 0.0;
    for (int pc = 0; pc < kNumPitchClasses; ++pc)
    {
        avg[(size_t) pc] /= (double) count;
        energy += avg[(size_t) pc];
    }
    if (energy < 0.05) // window was (almost) silence or pure noise
        return out;

    out.valid = true;

    // two chroma view selection: mean for steady instrument material, max-per-
    // class for vibrato/choir smear (the pitch-crossing frames give sharp class
    // peaks; the mean washes them flat). The profile correlation decides.
    {
        double maxEnergy = 0.0;
        for (int pc = 0; pc < kNumPitchClasses; ++pc)
            maxEnergy += maxc[(size_t) pc];
        if (maxEnergy > 1e-9)
            for (double& v : maxc)
                v /= maxEnergy;
    }
    const std::array<double, kNumPitchClasses> meanView = avg;

    double bestR = -2.0, secondR = -2.0;
    for (int rotate = 0; rotate < kNumPitchClasses; ++rotate)
    {
        for (int mode = 0; mode < 2; ++mode)
        {
            double profile[kNumPitchClasses];
            for (int i = 0; i < kNumPitchClasses; ++i)
                profile[(i + rotate) % 12] = (mode == 0 ? kMajorProfile : kMinorProfile)[i];

            const double rMean = correlation (meanView.data(), profile);
            const double rMax = correlation (maxc.data(), profile);
            const double r = std::max (rMean, rMax);
            if (r > bestR)
            {
                secondR = bestR;
                bestR = r;
                out.tonicPc = rotate;
                out.mode = mode;
            }
            else if (r > secondR)
            {
                secondR = r;
            }
        }
    }

    // confidence: profile match quality, scaled down when top two keys are nearly tied
    const double margin = std::max (0.0, bestR - secondR);
    const double certainty = std::min (1.0, margin / 0.10);
    out.confidence = (float) juce::jlimit (0.0, 1.0, bestR * (0.55 + 0.45 * certainty));

#ifdef KEY_SYNC_DEBUG_KEY
    {
        // full score table + the chroma fingerprint the decision came from
        std::fprintf (stderr, "[DBG-key] count=%d energy=%.3f best=%.3f second=%.3f -> %s %s (conf %.3f)\n",
                      count, energy, bestR, secondR,
                      pitchClassName (out.tonicPc), out.modeText(), out.confidence);
        std::fprintf (stderr, "[DBG-key] chroma:");
        for (int pc = 0; pc < kNumPitchClasses; ++pc)
            std::fprintf (stderr, " %s=%.3f", pitchClassName (pc), avg[(size_t) pc]);
        std::fprintf (stderr, "\n");
        struct Score { double r; int tonic; int mode; };
        Score top[3] = { { -2.0, -1, -1 }, { -2.0, -1, -1 }, { -2.0, -1, -1 } };
        for (int rotate = 0; rotate < kNumPitchClasses; ++rotate)
            for (int mode = 0; mode < 2; ++mode)
            {
                double profile[kNumPitchClasses];
                for (int i = 0; i < kNumPitchClasses; ++i)
                    profile[(i + rotate) % 12] = (mode == 0 ? kMajorProfile : kMinorProfile)[i];
                const double r = std::max (correlation (avg.data(), profile),
                                           correlation (maxc.data(), profile));
                if (r > top[0].r)
                    { top[2] = top[1]; top[1] = top[0]; top[0] = { r, rotate, mode }; }
                else if (r > top[1].r)
                    { top[2] = top[1]; top[1] = { r, rotate, mode }; }
                else if (r > top[2].r)
                    top[2] = { r, rotate, mode };
            }
        for (const auto& t : top)
            if (t.tonic >= 0)
                std::fprintf (stderr, "[DBG-key]   r=%.4f %s %s\n",
                              t.r, pitchClassName (t.tonic), modeName (t.mode));
    }
#endif

    // tempo stage — independent of the key fields above; stays 0/0 when unsure
    estimateTempo (bandFrames, out);
    return out;
}

// Tempo from the published band magnitudes (librosa/Percival concepts, own code —
// see docs/research/bpm-detection.md §5 approach #1):
// half-rectified log-band flux ODF -> x4 upsample -> clamped + 120 BPM
// lognormal-prior weighted ACF -> parabolic refine -> octave check.
// Fills out.bpm / out.bpmConfidence; leaves 0/0 for silence, noise, pads.
void KeyDetector::estimateTempo (const std::vector<std::array<float, kTempoBands>>& bandFrames,
                                 KeyResult& out) const
{
    const int n = (int) bandFrames.size();
#ifdef KEY_SYNC_DEBUG_TEMPO
    std::fprintf (stderr, "[DBG-tempo] n=%d\n", n);
#endif
    if (n < kMinActiveFrames)
        return;

    const double fps = sampleRate / (double) kHopSize;
    const double fpsUp = fps * 4.0; // research §4: native frame rate is too coarse

    // ODF: log-band flux whitened by each band's window AVERAGE (transients get
    // relative weight, sustained content ≈ 1). Bands with negligible window-mean
    // presence (dead noise floor) are gated out — their whitened log flicker
    // otherwise drowns the ODF ("+log floor noise"). The demean before the ACF
    // keeps the always-positive flux baseline from dominating peak/mean.
    std::array<double, kTempoBands> bandMean {};
    for (int t = 0; t < n; ++t)
        for (int b = 0; b < kTempoBands; ++b)
            bandMean[(size_t) b] += (double) bandFrames[(size_t) t][(size_t) b];
    for (int b = 0; b < kTempoBands; ++b)
        bandMean[(size_t) b] = std::max (bandMean[(size_t) b] / (double) n, 1e-12);
    double bandMaxMean = 0.0;
    for (int b = 0; b < kTempoBands; ++b)
        bandMaxMean = std::max (bandMaxMean, bandMean[(size_t) b]);

    int liveBands = 0;
    std::vector<double> odf ((size_t) (n - 1), 0.0);
    for (int t = 1; t < n; ++t)
    {
        double s = 0.0;
        for (int b = 0; b < kTempoBands; ++b)
        {
            if (bandMean[(size_t) b] < bandMaxMean * 0.05)
                continue;
            if (t == 1)
                ++liveBands;
            const double cur = std::log (std::max (bandFrames[(size_t) t][(size_t) b] / bandMean[(size_t) b], 1e-3));
            const double prev = std::log (std::max (bandFrames[(size_t) (t - 1)][(size_t) b] / bandMean[(size_t) b], 1e-3));
            const double d = cur - prev;
            // attacks are magnitude rises of 10%+ per hop (~46 ms); below that
            // level it is FFT reanalysis jitter of a sustained tone, not rhythm
            if (d > 0.095)
                s += d - 0.095;
        }
        odf[(size_t) (t - 1)] = s;
    }
    if (odf.size() < 2)
        return;

    // pure-tone content (sustained single note/pad): too little spectral
    // diversity for a pulse claim — the log flicker above is not rhythm
    if (liveBands < 4)
    {
#ifdef KEY_SYNC_DEBUG_TEMPO
        std::fprintf (stderr, "[DBG-tempo] REJECT narrowband liveBands=%d\n", liveBands);
#endif
        return;
    }

    // featureless flux (constant tone/pad: tiny jitter-derived deltas only)
    double flatRatio = 0.0;
    {
        double mean = 0.0;
        for (double v : odf)
            mean += v;
        mean /= (double) odf.size();
        double var = 0.0;
        for (double v : odf)
            var += (v - mean) * (v - mean);
        flatRatio = mean > 1e-9 ? std::sqrt (var / (double) odf.size()) / mean : 0.0;
        if (flatRatio < 0.30) // effectively flat flux → "uncertain"
        {
#ifdef KEY_SYNC_DEBUG_TEMPO
            std::fprintf (stderr, "[DBG-tempo] REJECT flat %.3f < 0.30\n", flatRatio);
#endif
            return;
        }
    }

    // half-rectified flux is >= 0 everywhere; on a full mix every frame carries
    // some flux, so the ODF baseline (mean^2 term) dominates the raw ACF.
    // Keep the biased ACF for lag selection (it favors steady-period lags),
    // but compute pulse confidence on the demeaned ACF where peak/mean is a
    // true strength measure.
    double odfBase = 0.0;
    {
        double odMean = 0.0;
        for (double v : odf)
            odMean += v;
        odfBase = odMean / (double) odf.size();
    }

    // x4 linear upsample
    const int m = int (odf.size() - 1) * 4 + 1;
    std::vector<double> up ((size_t) m, 0.0);
    for (int i = 0; i < m; ++i)
    {
        const double pos = (double) i / 4.0;
        const int k = std::min ((int) odf.size() - 1, (int) std::floor (pos));
        const double frac = pos - (double) k;
        const double a = odf[(size_t) k];
        const double b = k + 1 < (int) odf.size() ? odf[(size_t) (k + 1)] : a;
        up[(size_t) i] = a + (b - a) * frac;
    }

    // ACF with the tempo clamp; needs at least one lag of headroom
    const int lagMin = (int) std::ceil (60.0 * fpsUp / 208.0);
    const int lagMaxAll = (int) std::floor (60.0 * fpsUp / 40.0);
    const int lagMax = std::min (lagMaxAll, m - (m / 8));
    if (lagMax <= lagMin)
    {
#ifdef KEY_SYNC_DEBUG_TEMPO
        std::fprintf (stderr, "[DBG-tempo] REJECT window-short lagMax %d <= lagMin %d\n", lagMax, lagMin);
#endif
        return;
    }

    std::vector<double> acf ((size_t) (lagMax - lagMin + 1), 0.0);
    double acfMean = 0.0;
    for (int L = lagMin; L <= lagMax; ++L)
    {
        double s = 0.0;
        for (int y = 0; y + L < m; ++y)
            s += up[(size_t) y] * up[(size_t) (y + L)];
        acf[(size_t) (L - lagMin)] = s;
        acfMean += s;
    }
    acfMean /= (double) acf.size();

    // lognormal prior around 120 BPM (1 std = 1 octave), applied linearly
    auto priorAt = [fpsUp] (double lag)
    {
        const double bpm = 60.0 * fpsUp / lag;
        const double o = std::log2 (bpm / 120.0);
        return std::exp (-0.5 * o * o);
    };

    int bestI = -1;
    double bestW = -1.0;
    for (int i = 0; i < (int) acf.size(); ++i)
    {
        const double L = (double) (lagMin + i);
        const double w = acf[(size_t) i] * priorAt (L);
        if (w > bestW)
        {
            bestW = w;
            bestI = i;
        }
    }
    if (bestI < 0)
        return;
    const double bestAcf = acf[(size_t) bestI];
    if (bestAcf <= 0.0 || bestW <= 0.0)
    {
#ifdef KEY_SYNC_DEBUG_TEMPO
        std::fprintf (stderr, "[DBG-tempo] REJECT no-positive-periodicity bestAcf=%.4g\n", bestAcf);
#endif
        return; // no positive periodicity at all: silence / noise / tape hiss
    }

    // parabolic refine around the peak
    double delta = 0.0;
    if (bestI > 0 && bestI + 1 < (int) acf.size())
    {
        const double y1 = acf[(size_t) (bestI - 1)];
        const double y2 = bestAcf;
        const double y3 = acf[(size_t) (bestI + 1)];
        const double den = y1 - 2.0 * y2 + y3;
        if (std::abs (den) > 1e-12)
            delta = juce::jlimit (-0.5, 0.5, 0.5 * (y1 - y3) / den);
    }
    const double lagRef = (double) (lagMin + bestI) + delta;
    const double bpm = 60.0 * fpsUp / lagRef;

    // octave check: half/double the lag (same pulse family, per Percival's fix)
    double rivalW = -1.0;
    const int rivalLags[] = { (int) std::lround (lagRef / 2.0), (int) std::lround (lagRef * 2.0) };
    for (int rl : rivalLags)
    {
        if (rl < lagMin || rl > lagMax || rl < 1)
            continue;
        rivalW = std::max (rivalW, acf[(size_t) (rl - lagMin)] * priorAt ((double) rl));
    }

    // demean-agnostic pulse confidence: peak/σ over the full lag ACF is
    // shift-invariant, so the raw (selection) ACF scale serves both jobs.
    // Recalibrated Oct 2026 on real-music probe runs — see [DBG-tempo].
    double pkStd = 0.0;
    double acfVar = 0.0;
    for (double v : acf)
        acfVar += (v - acfMean) * (v - acfMean);
    acfVar = std::sqrt (acfVar / (double) acf.size());
    pkStd = acfVar > 1e-12 ? (bestAcf - acfMean) / acfVar : 0.0;
    double conf = juce::jlimit (0.0, 1.0, (pkStd - 0.8) / 1.5);
    if (rivalW > 0.85 * bestW) // unresolved octave family → flag, don't lie
        conf *= 0.5;

#ifdef KEY_SYNC_DEBUG_TEMPO
    {
        double pk = 0.0;
        for (double v : acf)
            pk = std::max (pk, v);
        std::fprintf (stderr, "[DBG-tempo] flat=%.3f pkStd=%.3f rival=%.3f lag=%.1f bpm=%.2f conf=%.3f\n",
                      flatRatio, pkStd, rivalW / std::max (bestW, 1e-12), lagRef, bpm, conf);
    }
#endif

    out.bpm = (float) bpm;
    out.bpmConfidence = (float) conf;
}

bool KeyDetector::hasEnoughAudio() const
{
    return stableFrameCount() >= kMinActiveFrames;
}

} // namespace keysync
