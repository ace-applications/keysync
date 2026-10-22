#include "KeyDetector.h"

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

const char* pitchClassName (int pc) { return kPitchNames[((pc % 12) + 12) % 12]; }

const char* KeyResult::keyName() const
{
    if (! valid || tonicPc < 0 || (mode != 0 && mode != 1))
        return "?";
    return mode == 0 ? "major" : "minor";
}

void KeyDetector::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    window.resize ((size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) (kFftSize - 1)));

    // input FIFO big enough for any host block plus one frame
    pending.assign ((size_t) kFftSize + (1 << 17), 0.0f);
    pendingSize = 0;
    frameData.assign ((size_t) kFftSize * 2, 0.0f);

    const int windowFrames = (int) std::ceil ((15.0 * sampleRate) / (double) kHopSize) + 4;
    frameRing.assign ((size_t) windowFrames, {});
    chromaSum.fill (0.0);
    ringHead = 0;
    activeFrames = 0;
    totalActiveFrames = 0;
}

void KeyDetector::reset()
{
    pendingSize = 0;
    ringHead = 0;
    activeFrames = 0;
    totalActiveFrames = 0;
    chromaSum.fill (0.0);
}

void KeyDetector::addMono (const float* samples, int numSamples)
{
    if (sampleRate <= 0.0)
        return;

    const int capacity = (int) pending.size();
    const int copy = juce::jmin (numSamples, capacity - pendingSize); // drop overflow; window is plenty larger than any sane block
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

        if (rms >= kRmsThreshold)
        {
            for (int i = 0; i < kFftSize; ++i)
                frameData[(size_t) i] = pending[(size_t) i] * window[(size_t) i];
            for (int i = kFftSize; i < kFftSize * 2; ++i)
                frameData[(size_t) i] = 0.0f;

            fft.performRealOnlyForwardTransform (frameData.data(), true);

            auto frame = chromaFromFrame (frameData.data(), kFftSize, sampleRate);

            const int capacity = (int) frameRing.size();
            if (activeFrames == capacity) // window full: drop oldest
            {
                for (int pc = 0; pc < kNumPitchClasses; ++pc)
                    chromaSum[(size_t) pc] -= (double) frameRing[(size_t) ringHead][(size_t) pc];
                ringHead = (ringHead + 1) % capacity;
                --activeFrames;
            }

            const int slot = (ringHead + activeFrames) % capacity;
            frameRing[(size_t) slot] = frame;
            for (int pc = 0; pc < kNumPitchClasses; ++pc)
                chromaSum[(size_t) pc] += frame[(size_t) pc];
            ++activeFrames;
            ++totalActiveFrames;
        }

        // shift the next hop of samples to the front
        const int remaining = pendingSize - kHopSize;
        std::memmove (pending.data(), pending.data() + kHopSize, sizeof (float) * (size_t) remaining);
        pendingSize = remaining;
    }
}

std::array<float, kNumPitchClasses>
KeyDetector::chromaFromFrame (const float* fftData, int fftSize, double sr)
{
    std::array<float, kNumPitchClasses> chroma {};
    double sum = 0.0;
    for (int bin = 0; bin < fftSize / 2; ++bin)
    {
        const double re = (double) fftData[2 * bin];
        const double im = (double) fftData[2 * bin + 1];
        const double mag = std::sqrt (re * re + im * im);
        if (mag <= 0.0)
            continue;

        const double freq = (double) bin * sr / (double) fftSize;
        if (freq < 55.0 || freq > 4000.0) // tonal range for pop music, skip rumble/hiss
            continue;

        // gentle octave weighting: favour the middle octaves
        const double midi = 69.0 + 12.0 * std::log2 (freq / 440.0);
        const double octaveWeight = (midi < 48.0 || midi > 84.0) ? 0.5 : 1.0;

        const int pc = (((int) std::lround (midi)) % 12 + 12) % 12;
        chroma[(size_t) pc] += (float) (mag * octaveWeight);
        sum += mag;
    }
    if (sum > 0.0)
        for (float& c : chroma)
            c /= (float) sum; // per-frame normalisation, loudness independent
    return chroma;
}

KeyResult KeyDetector::detect() const
{
    KeyResult out;
    if (activeFrames < kMinActiveFrames)
        return out;

    double avg[kNumPitchClasses] = {};
    double energy = 0.0;
    for (int pc = 0; pc < kNumPitchClasses; ++pc)
    {
        avg[pc] = chromaSum[(size_t) pc] / (double) activeFrames;
        energy += avg[pc];
    }
    if (energy < 0.05) // window was (almost) silence or pure noise
        return out;

    out.valid = true;

    double bestR = -2.0, secondR = -2.0;
    for (int rotate = 0; rotate < kNumPitchClasses; ++rotate)
    {
        for (int mode = 0; mode < 2; ++mode)
        {
            double profile[kNumPitchClasses];
            for (int i = 0; i < kNumPitchClasses; ++i)
                profile[(i + rotate) % 12] = (mode == 0 ? kMajorProfile : kMinorProfile)[i];

            const double r = correlation (avg, profile);
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
    return out;
}

bool KeyDetector::hasEnoughAudio() const
{
    return activeFrames >= kMinActiveFrames;
}

} // namespace keysync
