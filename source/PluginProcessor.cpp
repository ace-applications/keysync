#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace keysync
{

KeySyncAudioProcessor::KeySyncAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void KeySyncAudioProcessor::prepareToPlay (double sampleRate, int)
{
    detector.prepare (sampleRate);
}

void KeySyncAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // pass-through: analyser only, audio must leave unaltered (nothing is modified)

    // mixdown to mono and feed the analysis window (RT-safe: preallocated)
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (numSamples <= 0)
        return;

    monoScratch.resize ((size_t) numSamples);
    float* mono = monoScratch.data();

    if (numChannels <= 0)
        std::fill (mono, mono + numSamples, 0.0f);
    else if (numChannels == 1)
        std::memcpy (mono, buffer.getReadPointer (0), sizeof (float) * (size_t) numSamples);
    else
        for (int i = 0; i < numSamples; ++i)
            mono[i] = 0.5f * (buffer.getReadPointer (0)[i] + buffer.getReadPointer (1)[i]);

    detector.addMono (mono, numSamples);
}

void KeySyncAudioProcessor::runAnalysis()
{
    if (state.load (std::memory_order_relaxed) == AnalyzeState::analyzing)
        return;

    state.store (AnalyzeState::analyzing, std::memory_order_relaxed);

    if (! detector.hasEnoughAudio())
    {
        state.store (AnalyzeState::noUsableAudio, std::memory_order_relaxed);
        lastResult = {};
        return;
    }

    lastResult = detector.detect();

    if (! lastResult.valid)
        state.store (AnalyzeState::noUsableAudio, std::memory_order_relaxed);
    else if (lastResult.confidence * 100.0f >= (float) KeyDetector::kConfidenceResultPct)
        state.store (AnalyzeState::result, std::memory_order_relaxed);
    else
        state.store (AnalyzeState::lowConfidence, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* KeySyncAudioProcessor::createEditor()
{
    return new KeySyncAudioProcessorEditor (*this);
}

} // namespace keysync

// unnamed: juce needs the raw global createPluginFilter entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new keysync::KeySyncAudioProcessor();
}
