#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace keysync
{

// Probe-stage melody extractor, HMM-lite v1 (docs/research/melody-analysis.md).
// Offline offline Viterbi over collected frames; the audio-thread version
// (later, in KeyDetector) will update the trellis incrementally instead.
//
// Pipeline per 4096/2048 frame on a full-rate mono feed:
//   1. magnitude spectrum (own FFT here; will reuse KeyDetector's later)
//   2. semitone salience, 48 rows C2-C6 (MIDI 36-83),
//      s[r] = sum over harmonics h=1..6 of mag(bin(h*f_r)) * 0.8^(h-1)
//      (Melodia harmonic weighting as prior; h=1 kept so pure/hum input scores)
//   3. voicing gate from the window's own salience distribution
//   4. 49-state Viterbi (48 rows + unvoiced): local +-2 semitone free,
//      small slides 3-5 at -0.1, +-12 octave jumps at -0.35, anything else
//      unreachable (-2.0) — the octave-error mitigation
//   5. conf-weighted 12-bin pitch-class histogram = melodyPCP
class MelodyExtractor
{
public:
    static constexpr int kFftSize = 4096;
    static constexpr int kHopSize = kFftSize / 2;
    static constexpr int kRows = 48;
    static constexpr int kMidiLow = 36;
    static constexpr int kMaxHarmonic = 6;
    static constexpr float kHarmonicDecay = 0.8f;
    static constexpr float kOctavePenalty = 0.35f;
    static constexpr float kSlidePenalty = 0.1f;
    static constexpr float kSwitchPenalty = 0.6f;
    static constexpr float kHardJumpPenalty = 2.0f;
    static constexpr float kVoicedFloorPct = 0.18f; // of 90th-pct frame salience

    struct Window
    {
        std::array<double, kNumPitchClasses> pcp {};
        float voicedPct = 0.0f;
        bool valid = false; // at least one voiced frame
    };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        window.assign ((size_t) kFftSize, 0.0f);
        pending.assign ((size_t) kFftSize, 0.0f);
        pendingSize = 0;
        frameData.assign ((size_t) kFftSize * 2, 0.0f);
        mags.assign ((size_t) kFftSize / 2, 0.0);
        framePeak.assign ((size_t) kFftSize / 2, 0.0);
        fft = std::make_unique<juce::dsp::FFT> (12); // 2^12 = 4096

        for (int i = 0; i < kFftSize; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (
                juce::MathConstants<double>::twoPi * (double) i / (double) (kFftSize - 1)));

        // harmonic bin lookup per row
        for (int r = 0; r < kRows; ++r)
        {
            const double f0 = 440.0 * std::pow (2.0, (double) (kMidiLow + r - 69) / 12.0);
            for (int h = 1; h <= kMaxHarmonic; ++h)
            {
                const int bin = (int) std::lround (f0 * (double) h * (double) kFftSize / sampleRate);
                rowBin[h - 1][r] = bin < kFftSize / 2 ? bin : -1;
            }
        }
        frames.clear();
    }

    void addMono (const float* samples, int numSamples)
    {
        if (sampleRate <= 0.0 || fft == nullptr)
            return;
        const int capacity = (int) pending.size();
        int copy = juce::jmin (numSamples, capacity - pendingSize);
        if (copy <= 0)
            return;
        std::memcpy (pending.data() + pendingSize, samples, sizeof (float) * (size_t) copy);
        pendingSize += copy;

        while (pendingSize >= kFftSize)
        {
            processFrame();
            const int remaining = pendingSize - kHopSize;
            std::memmove (pending.data(), pending.data() + kHopSize,
                          sizeof (float) * (size_t) remaining);
            pendingSize = remaining;
            copy = 0;
        }
    }

    Window finalize()
    {
        Window w;
        const int n = (int) frames.size();
        if (n < 8) // < ~6 s of 46 ms frames — nothing honest to say
            return w;

        // frame max salience -> voicing floor at kVoicedFloorPct of its 90th pct
        std::vector<float> maxSal ((size_t) n);
        for (int i = 0; i < n; ++i)
            maxSal[(size_t) i] = *std::max_element (frames[(size_t) i].salience.begin(),
                                                    frames[(size_t) i].salience.end());
        std::vector<float> sorted = maxSal;
        std::sort (sorted.begin(), sorted.end());
        const float p90 = std::max (1e-9f, sorted[(int) (n * 9) / 10]);
        const float floor = kVoicedFloorPct * p90;

        // Viterbi, 49 states: 0..47 = pitch rows, 48 = unvoiced
        std::vector<float> prev ((size_t) kRows + 1, 0.0f), cur ((size_t) kRows + 1, 0.0f);
        std::vector<std::vector<uint8_t>> back ((size_t) n,
            std::vector<uint8_t> ((size_t) kRows + 1, 0));
        std::vector<uint8_t> best ((size_t) n, (uint8_t) kRows);

        auto emission = [&] (int i, int s) -> float
        {
            if (s == kRows)
                return 0.0f;
            const float v = frames[(size_t) i].salience[(size_t) s] / p90;
            return v < floor / p90 ? 0.0f : v; // hard gate at emission level
        };

        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c <= kRows; ++c)
            {
                float bestPrev = -1e18f;
                int arg = kRows;
                for (int p = 0; p <= kRows; ++p)
                {
                    float t;
                    if (p == kRows && c == kRows)             t = 0.0f;
                    else if (p == kRows || c == kRows)        t = -kSwitchPenalty;
                    else
                    {
                        const int d = std::abs (p - c);
                        if (d == 0 || d <= 2)                 t = 0.0f;
                        else if (d <= 5)                      t = -kSlidePenalty;
                        else if (d >= 11 && d <= 13)          t = -kOctavePenalty;
                        else                                  t = -kHardJumpPenalty;
                    }
                    const float score = prev[(size_t) p] + t;
                    if (score > bestPrev) { bestPrev = score; arg = p; }
                }
                cur[(size_t) c] = bestPrev + emission (i, c);
                back[(size_t) i][(size_t) c] = (uint8_t) arg;
            }
            std::swap (prev, cur);
        }

        // best end state + backtrack
        int s = kRows;
        float bestEnd = -1e18f;
        for (int c = 0; c <= kRows; ++c)
            if (prev[(size_t) c] > bestEnd) { bestEnd = prev[(size_t) c]; s = c; }
        for (int i = n - 1; i >= 0; --i)
        {
            best[(size_t) i] = (uint8_t) s;
            s = back[(size_t) i][(size_t) s];
        }

        int voiced = 0;
        for (int i = 0; i < n; ++i)
        {
            const int c = best[(size_t) i];
            if (c == kRows || frames[(size_t) i].salience[(size_t) c] < floor)
                continue;
            ++voiced;
            // conf^2: weight only frames the tracker is actually sure about —
            // weak-salience chatter stays quieter than the real line
            w.pcp[(size_t) ((kMidiLow + c) % 12)] +=
                (double) (frames[(size_t) i].salience[(size_t) c] / p90)
                    * (double) (frames[(size_t) i].salience[(size_t) c] / p90);
        }
        w.voicedPct = (float) voiced / (float) n;
        w.valid = voiced >= 8; // ~2 % of frames — ignore windows with a blink of melody
        return w;
    }

private:
    struct Frame { std::array<float, kRows> salience {}; };
    void processFrame()
    {
        for (int i = 0; i < kFftSize; ++i)
            frameData[(size_t) i] = pending[(size_t) i] * window[(size_t) i];
        for (int i = kFftSize; i < kFftSize * 2; ++i)
            frameData[(size_t) i] = 0.0f;
        fft->performRealOnlyForwardTransform (frameData.data(), true);

        const int halfBins = kFftSize / 2;
        for (int bin = 0; bin < halfBins; ++bin)
        {
            const double re = frameData[2 * bin];
            const double im = frameData[2 * bin + 1];
            mags[(size_t) bin] = std::sqrt (re * re + im * im);
        }

        Frame& f = frames.emplace_back();
        // Melodia-faithful: salience from spectral PEAKS only (local maxima in
        // magnitude, within 0.2x of the frame's strongest peak) — summing raw
        // bins folds broadband choir/drum energy into the melody line, which is
        // exactly the Selah failure mode.
        double peakMax = 0.0;
        for (int bin = 1; bin < halfBins - 1; ++bin)
            peakMax = std::max (peakMax, mags[(size_t) bin]);
        if (peakMax < 1e-9)
        {
            f.salience.fill (0.0f);
            frames.pop_back(); // silent frame: drop, gate handles it
            return;
        }
        std::fill (framePeak.begin(), framePeak.end(), 0.0);
        for (int bin = 1; bin < halfBins - 1; ++bin)
        {
            const double m = mags[(size_t) bin];
            if (m > mags[(size_t) (bin - 1)] && m >= mags[(size_t) (bin + 1)]
                && m > 0.2 * peakMax)
                framePeak[(size_t) bin] = m;
        }
        f.salience.fill (0.0f);
        for (int r = 0; r < kRows; ++r)
        {
            float s = 0.0f;
            float weight = 1.0f;
            for (int h = 1; h <= kMaxHarmonic; ++h)
            {
                const int bin = rowBin[h - 1][r];
                if (bin >= 0 && framePeak[(size_t) bin] > 0.0)
                    s += (float) framePeak[(size_t) bin] * weight;
                weight *= kHarmonicDecay;
            }
            f.salience[(size_t) r] = s;
        }
    }

    double sampleRate = 0.0;
    std::vector<float> window;
    std::vector<float> pending;
    int pendingSize = 0;
    std::vector<float> frameData;
    std::vector<double> mags;
    std::vector<double> framePeak;
    std::unique_ptr<juce::dsp::FFT> fft;
    int rowBin[kMaxHarmonic][kRows] = {};
    std::vector<Frame> frames;
};

} // namespace keysync
