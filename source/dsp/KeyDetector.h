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

inline int wrapPc (int pc) { return ((pc % 12) + 12) % 12; }
inline const char* modeName (int mode) { return mode == 0 ? "major" : "minor"; }

const char* pitchClassName (int pc);

// tonicPc: 0=C ... 11=B. mode: 0 = major, 1 = minor.
struct KeyResult
{
    bool valid = false;         // a detection was actually computed
    int tonicPc = -1;
    int mode = -1;
    float confidence = 0.0f;    // 0..1

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

    void prepare (double sampleRate);
    void reset();

    void addMono (const float* samples, int numSamples);

    // Analyzes the accumulated audio. Returns an invalid KeyResult when there
    // is not enough material.
    KeyResult detect() const;

    bool hasEnoughAudio() const;

private:
    // Seqlock-published chroma frame: even seq = stable, odd = mid-write.
    struct ChromaSlot
    {
        std::atomic<uint32_t> seq { 0 };
        std::array<float, kNumPitchClasses> chroma {};
    };

    int stableFrameCount() const;
    bool tryReadFrame (const ChromaSlot& slot, std::array<float, kNumPitchClasses>& out) const;

    void processPendingFrames();
    static std::array<float, kNumPitchClasses> chromaFromFrame (const float* fftData,
                                                               int fftSize,
                                                               double sampleRate);

    static constexpr int kHopSize = kFftSize / 2;

    double sampleRate = 0.0;
    std::vector<float> window;            // Hann, kFftSize
    std::vector<float> pending;           // sample FIFO
    std::vector<float> frameData;         // FFT workspace (kFftSize * 2)
    int pendingSize = 0;

    std::unique_ptr<ChromaSlot[]> frameRing;  // one published frame per active hop
    int ringCapacity = 0;                     // slots in frameRing
    int ringWrite = 0;                        // next slot to (over)write

    juce::dsp::FFT fft { kFftOrder };
};

} // namespace keysync
