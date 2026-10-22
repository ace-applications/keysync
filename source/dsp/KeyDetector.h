#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <vector>

namespace keysync
{

constexpr int kNumPitchClasses = 12;

// tonicPc: 0=C ... 11=B. mode: 0 = major, 1 = minor.
struct KeyResult
{
    bool valid = false;         // a detection was actually computed
    int tonicPc = -1;
    int mode = -1;
    float confidence = 0.0f;    // 0..1
    const char* keyName() const;
};

const char* pitchClassName (int pc);

// Rolling chromagram + Krumhansl-Schmuckler key detection.
// Fed from the audio thread. detect() must be called from the message thread
// only; tearing against a concurrent processBlock is possible but harmless
// (worst case: one partially-written frame).
class KeyDetector
{
public:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr float kRmsThreshold = 0.0015f; // ~ -56 dB, below counts as silence
    static constexpr int kMinActiveFrames = 24;     // ~1.2 s of real audio
    static constexpr int kConfidenceResultPct = 45; // >= this: an honest "Result"

    void prepare (double sampleRate);
    void reset();

    void addMono (const float* samples, int numSamples);

    // Analyzes the accumulated audio. Returns an invalid KeyResult when there
    // is not enough material.
    KeyResult detect() const;

    bool hasEnoughAudio() const;

private:
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

    // ring of chroma frames, one per active frame, oldest first
    std::vector<std::array<float, kNumPitchClasses>> frameRing;
    int ringHead = 0;      // oldest index in frameRing
    int activeFrames = 0;  // frames currently in window
    long long totalActiveFrames = 0;
    std::array<double, kNumPitchClasses> chromaSum {};

    juce::dsp::FFT fft { kFftOrder };
};

} // namespace keysync
