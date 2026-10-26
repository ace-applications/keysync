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
    void estimateTempo (const std::vector<std::array<float, kTempoBands>>& bandFrames,
                        KeyResult& out) const;

    static constexpr int kHopSize = kFftSize / 2;

    double sampleRate = 0.0;
    double chromaSampleRate = 0.0;        // decimated rate for the chroma branch

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

    juce::dsp::FFT fft { kFftOrder };
};

} // namespace keysync
