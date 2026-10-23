#pragma once

#include "PluginProcessor.h"

#include <vector>

namespace keysync
{

class KeySyncAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit KeySyncAudioProcessorEditor (KeySyncAudioProcessor& processorIn);
    ~KeySyncAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    KeySyncAudioProcessor& processor;

    // brand
    static constexpr uint32_t kBg = 0xff080808u;
    static constexpr uint32_t kOrange = 0xffFF8A24u;
    static constexpr uint32_t kGrey = 0xffA3A3A3u;
    static constexpr uint32_t kWhite = 0xffffffffu;

    juce::Typeface::Ptr isometra;
    juce::Typeface::Ptr inter;
    juce::Typeface::Ptr interSemi;
    juce::Font titleFont;      // Isometra
    juce::Font keyFont;        // Isometra big

    // child components (custom painted)
    struct TitleBar : juce::Component
    {
        KeySyncAudioProcessorEditor& owner;
        explicit TitleBar (KeySyncAudioProcessorEditor& o) : owner (o) {}
        void paint (juce::Graphics&) override;
    };
    struct KeyDisplay : juce::Component
    {
        KeySyncAudioProcessorEditor& owner;
        explicit KeyDisplay (KeySyncAudioProcessorEditor& o) : owner (o) {}
        void paint (juce::Graphics&) override;
    };
    struct AnalyzeButton : juce::Button
    {
        KeySyncAudioProcessorEditor& owner;
        AnalyzeButton (KeySyncAudioProcessorEditor& o) : juce::Button ("Analyze"), owner (o) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
    };
    struct ProPanel : juce::Component
    {
        KeySyncAudioProcessorEditor& owner;
        explicit ProPanel (KeySyncAudioProcessorEditor& o) : owner (o) {}
        void paint (juce::Graphics&) override;
    };

    TitleBar title { *this };
    KeyDisplay keyDisplay { *this };
    AnalyzeButton analyzeButton { *this };
    ProPanel proPanel { *this };
    juce::HyperlinkButton getPro { "Get ACE Audio KeySync Pro", juce::URL ("https://aceaudio.example") };

    struct AnalyzePulse : juce::Timer // one-shot delayed continuation of Analyze
    {
        KeySyncAudioProcessorEditor& owner;
        explicit AnalyzePulse (KeySyncAudioProcessorEditor& o) : owner (o) {}
        void timerCallback() override
        {
            stopTimer();
            owner.processor.performAnalysis();
            owner.repaint();
        }
    };
    AnalyzePulse analyzePulse { *this };

    JUCE_DECLARE_NON_COPYABLE (KeySyncAudioProcessorEditor)
};

} // namespace keysync
