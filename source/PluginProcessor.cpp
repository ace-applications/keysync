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

    // staleness: any audio above the silence floor invalidates the shown result
    float energy = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        energy = std::max (energy, std::abs (mono[i]));

    if (energy >= KeyDetector::kRmsThreshold)
        stale.store (true, std::memory_order_relaxed);
}

void KeySyncAudioProcessor::requestAnalysis()
{
    if (state.load (std::memory_order_relaxed) == AnalyzeState::analyzing)
        return;

    stale.store (false, std::memory_order_relaxed);
    state.store (AnalyzeState::analyzing, std::memory_order_relaxed);
}

void KeySyncAudioProcessor::performAnalysis()
{
    if (! detector.hasEnoughAudio())
    {
        state.store (AnalyzeState::noUsableAudio, std::memory_order_relaxed);
        lastResult = {};
        return;
    }

    lastResult = detector.detect();
    stale.store (false, std::memory_order_relaxed); // result now reflects current audio

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
