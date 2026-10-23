#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "dsp/KeyDetector.h"

#if ! (defined(KEYSYNC_EDITION_PRO) && (KEYSYNC_EDITION_PRO == 0 || KEYSYNC_EDITION_PRO == 1))
 #error "KEYSYNC_EDITION_PRO must be 0 (Free) or 1 (Pro)"
#endif

namespace keysync
{

enum class AnalyzeState
{
    idle,
    analyzing,
    result,
    lowConfidence,
    noUsableAudio
};

class KeySyncAudioProcessor : public juce::AudioProcessor
{
public:
    KeySyncAudioProcessor();

    void prepareToPlay (double sampleRate, int) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "KeySync"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    // Message thread only. requestAnalysis shows the Analyzing state; the
    // editor then calls performAnalysis on a short delay so the state is
    // actually painted (analysis itself is ~tens of ms on the message thread —
    // the audio thread is never blocked).
    AnalyzeState getState() const { return state.load (std::memory_order_relaxed); }
    KeyResult getLastResult() const { return lastResult; }

    void requestAnalysis();
    void performAnalysis();

private:
    KeyDetector detector;
    std::atomic<AnalyzeState> state { AnalyzeState::idle };
    KeyResult lastResult;
    std::vector<float> monoScratch;
};

} // namespace keysync
