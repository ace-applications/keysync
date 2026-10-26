#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace keysync
{

constexpr int kNumPitchClasses = 12;
constexpr int kTempoBands = 32; // log-spaced magnitude bands 35 Hz-8 kHz

inline int wrapPc (int pc) { return ((pc % 12) + 12) % 12; }
inline const char* modeName (int mode) { return mode == 0 ? "major" : "minor"; }

const char* pitchClassName (int pc);

// Krumhansl-Schmuckler key profiles (shared by the detector and the dev probe
// for melody-fusion experiments).
inline constexpr double kMajorProfile[kNumPitchClasses] =
    { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
inline constexpr double kMinorProfile[kNumPitchClasses] =
    { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

// Pearson correlation of two 12-bin vectors (0 when either is degenerate).
inline double ksCorrelation (const double* a, const double* b)
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

// tonicPc: 0=C ... 11=B. mode: 0 = major, 1 = minor.
struct KeyResult
{
    bool valid = false;         // a key detection was actually computed
    int tonicPc = -1;
    int mode = -1;
    float confidence = 0.0f;    // 0..1

    // bpmConfidence < 0.5 -> no honest tempo claim, UI shows "BPM uncertain"
    float bpm = 0.0f;           // 0 = none           // 0 = none
    float bpmConfidence = 0.0f; // 0..1

    const char* modeText() const { return modeName (mode); }
};

juce::String keyTitle (const KeyResult& r); // "A minor"

// Melody tracker output, one slot per full-rate hop (uniform time grid like the
// tempo ring; silent frames publish voiced==0).
struct MelodySlot
{
    std::atomic<uint32_t> seq { 0 };
    float row = 0.0f;    // fractional row 0..47 = C2..B5 (MIDI 36 + row)
    float conf = 0.0f;   // 0..1 salience-normalised strength
    uint8_t voiced = 0;
};

// Value copy of one released MelodySlot (frame-local, non-atomic).
struct MelodySample
{
    float row = 0.0f;
    float conf = 0.0f;
    uint8_t voiced = 0;
};

// Rolling chromagram + Krumhansl-Schmuckler key detection.
//
// Threading: addMono() is RT-audio-thread only. detect()/hasEnoughAudio() are
// message-thread only (they race is prevented by per-frame seqlock
// publication — see ChromaSlot — readers skip frames that are mid-write).
class KeyDetector
{
public:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr float kRmsThreshold = 0.0015f; // ~ -56 dB, below counts as silence
    static constexpr int kMinActiveFrames = 24;     // ~1.2 s of real audio
    static constexpr int kConfidenceResultPct = 45; // >= this: an honest "Result"
    // incoming-block cap: ~3 s at 44.1/48 kHz sample rate — far above any real
    // host block; anything larger is truncated on purpose to bound the buffer.
    static constexpr int kMaxBlockSamples = 1 << 17;

    // rolling analysis window: longer than the original 15 s so slow / soft
    // pulse material still yields an honest tempo claim (30 s, user-approved)
    static constexpr double kAnalysisWindowSeconds = 30.0;

    // chroma branch runs on a decimated copy of the stream at sampleRate/4:
    // the full-rate FFT (12-point order = 4096 @ 44.1 kHz) has ~10.8 Hz bins,
    // COARSER than a semitone below ~110 Hz; at 11025 Hz the same 4096-point
    // frame gives ~2.7 Hz bins so every low-octave semitone spans >1 bin.
    static constexpr int kChromaDecim = 4;
    // key gating uses decimated frames (185.8 ms apart at 44.1/4), so the
    // 24-frame full-rate minimum (~1.2 s) maps to ~12 chroma frames.
    static constexpr int kMinKeyFrames = kMinActiveFrames / 2;

    // ---- melody tracker (v1.1, display-only, Pro lane) — docs/research/
    // melody-analysis.md: HMM-lite salience grid; fusion into the key stage is
    // PARKED (probe-verified: cheap tracker can't steer KS on dense mixes).
    static constexpr int kMelodyRows = 48;      // C2..B5, MIDI 36..83
    static constexpr int kMidiMelLow = 36;
    static constexpr int kMelodyHarmonics = 6;
    static constexpr float kMelodyHarmonicDecay = 0.8f;
    static constexpr float kMelodyPeakFloorRatio = 0.20f; // of frame peak
    static constexpr float kMelodyVoicingRatio = 0.12f;   // of running max salience
    static constexpr float kMelodyMaxEma = 0.005f;        // EMA toward frame max (~9 s)
    static constexpr float kMelodyRowEma = 0.50f;
    static constexpr float kMelodyContinuityBias = 0.25f; // +-2 semitone stay bonus

    void prepare (double sampleRate);
    void reset();

    void addMono (const float* samples, int numSamples);

    // Analyzes the accumulated audio. Returns an invalid KeyResult when there
    // is not enough material.
    KeyResult detect() const;

    bool hasEnoughAudio() const;

private:
    // Seqlock-published tempo-band frame: even seq = stable, odd = mid-write.
    struct ChromaSlot
    {
        std::atomic<uint32_t> seq { 0 };
        std::array<float, kNumPitchClasses> chroma {};
        std::array<float, kTempoBands> bands {};
    };

    // Seqlock-published CHROMA-only frame from the decimated branch.
    struct ChromaSlotTight
    {
        std::atomic<uint32_t> seq { 0 };
        std::array<float, kNumPitchClasses> chroma {};
    };

    int stableFrameCount() const;
    bool tryReadFrame (const ChromaSlot& slot, ChromaSlot& out) const;

    void processPendingFrames();
    void processChromaPendingFrames();
    void chromaFromFrame (const float* fftData, int fftSize, double freqSr,
                          std::array<float, kNumPitchClasses>& chroma,
                          std::array<float, kTempoBands>& bands);
    bool tryReadChromaFrame (const ChromaSlotTight& slot,
                             std::array<float, kNumPitchClasses>& out) const;
    void processMelodyFrame (const std::vector<double>& mags, MelodySample& out);
    void estimateTempo (const std::vector<std::array<float, kTempoBands>>& bandFrames,
                        KeyResult& out) const;

    static constexpr int kHopSize = kFftSize / 2;

    double sampleRate = 0.0;
    double chromaSampleRate = 0.0;        // decimated rate for the chroma branch

    // mean chroma view from the last detect() call (filled only when valid);
    // probe seam for melody-fusion experiments
    mutable std::array<double, kNumPitchClasses> lastChroma {};
public:
    std::array<double, kNumPitchClasses> chromaView() const { return lastChroma; }
private:

    std::vector<float> window;            // Hann, kFftSize
    std::vector<float> pending;           // sample FIFO (full rate, tempo path)
    std::vector<float> frameData;         // FFT workspace (kFftSize * 2)
    int pendingSize = 0;

    std::vector<float> chromaPending;     // decimated sample FIFO (chroma path)
    int chromaPendingSize = 0;
    std::vector<float> chromaFrameData;   // FFT workspace for the chroma branch
    // decimation state: two cascaded 4-sample boxcars (alias-suppressing CIC2)
    float decimD1[3] = { 0.f, 0.f, 0.f };
    float decimP2[3] = { 0.f, 0.f, 0.f };
    int decimPhase = 0;

    std::vector<double> frameMags;        // per-frame magnitudes (audio-thread scratch)

    std::unique_ptr<ChromaSlot[]> frameRing;  // tempo path ring (bands grid)
    int ringCapacity = 0;                     // slots in frameRing
    int ringWrite = 0;                        // next slot to (over)write

    std::unique_ptr<ChromaSlotTight[]> chromaRing; // chroma path ring (12-bin)
    int chromaRingCapacity = 0;
    int chromaRingWrite = 0;

    // melody tracker state (same hop cadence as the tempo ring)
    std::unique_ptr<MelodySlot[]> melodyRing;
    int melodyRingWrite = 0;                  // shares ringCapacity with frameRing
    std::vector<double> melodyPeakMag;        // peak-masked magnitudes (audio thread)
    std::array<float, kMelodyRows> melodySal {};
    std::array<int, kMelodyHarmonics * kMelodyRows> melodyRowBin {}; // [h][row]
    float melodyMaxEma = 0.0f;
    int melodyPrevRow = -1;
    float melodyRowF = 0.0f;

public:
    // melody lane polling (message thread): read a seqlocked tracker sample.
    // out.row is the fractional row 0..47 (C2..B5); voiced==0 means silence.
    bool tryReadMelody (const MelodySlot& slot, MelodySample& out) const
    {
        const uint32_t s1 = slot.seq.load (std::memory_order_acquire);
        if (s1 == 0 || (s1 & 1u) != 0)
            return false;
        out.row = slot.row;
        out.conf = slot.conf;
        out.voiced = slot.voiced;
        std::atomic_thread_fence (std::memory_order_acquire);
        return slot.seq.load (std::memory_order_relaxed) == s1;
    }
    int melodyWriteIndex() const { return melodyRingWrite; }
    const MelodySlot* melodyRingData() const { return melodyRing.get(); }
    int melodyRingSize() const { return ringCapacity; }

    juce::dsp::FFT fft { kFftOrder };
};

} // namespace keysync
