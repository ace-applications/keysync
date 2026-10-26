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

    // brand (bg = ACE Audio dark navy per product brief)
    static constexpr uint32_t kBg = 0xff080C19u;
    static constexpr uint32_t kOrange = 0xffFF8A24u;
    static constexpr uint32_t kGrey = 0xffA3A3A3u;
    static constexpr uint32_t kWhite = 0xffffffffu;

    juce::Typeface::Ptr isometra;   // wordmark/logo heading only — glyphs broken for body text
    juce::Typeface::Ptr inter;
    juce::Typeface::Ptr interSemi;
    juce::Font titleFont;      // Isometra (logo wordmark)
    juce::Font keyFont;        // Inter SemiBold big

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
        // honest BPM claim, else a short grey excuse — never a blank row; shared
        // by the result and low-confidence states so the layout contract holds
        void drawBpmRow (juce::Graphics& g, juce::Rectangle<float> row,
                         const KeyResult& r) const;
    };
struct AnalyzeButton : juce::Button
    {
        KeySyncAudioProcessorEditor& owner;
        AnalyzeButton (KeySyncAudioProcessorEditor& o) : juce::Button ("Analyze"), owner (o) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
        bool hitTest (int x, int y) override;
        void mouseEnter (const juce::MouseEvent&) override { repaint(); };
        void mouseExit (const juce::MouseEvent&) override { repaint(); };
    };
    // Pro-only rolling melody contour lane (docs/research/melody-analysis.md):
    // reads the detector's melody ring, newest hop at the right edge; 1 px per
    // hop ≈ the last 16 s of the predominant line, orange = tracker confidence
    struct MelodyLane : juce::Component, juce::Timer
    {
        KeySyncAudioProcessorEditor& owner;
        explicit MelodyLane (KeySyncAudioProcessorEditor& o) : owner (o) { startTimer (45); }
        ~MelodyLane() override { stopTimer(); }
        void paint (juce::Graphics&) override;
        void timerCallback() override
        {
            const int w = owner.processor.detectorView().melodyWriteIndex();
            if (w != lastSeenWrite) { lastSeenWrite = w; repaint(); }
        }
        int lastSeenWrite = -1;
    };
    struct ProPanel : juce::Component, juce::SettableTooltipClient
    {        KeySyncAudioProcessorEditor& owner;
        explicit ProPanel (KeySyncAudioProcessorEditor& o) : owner (o) {}
        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent&) override { hoverRow(); }
        void mouseExit (const juce::MouseEvent&) override { setTooltip (""); }
    private:
        void hoverRow();
    };

    TitleBar title { *this };
    KeyDisplay keyDisplay { *this };
    AnalyzeButton analyzeButton { *this };
    ProPanel proPanel { *this };
    MelodyLane melodyLane { *this };
    juce::TooltipWindow tooltip; // explains chip vocabulary on hover
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

    // repaints only when the staleness flag flips (came from the audio thread)
    struct StalePoll : juce::Timer
    {
        KeySyncAudioProcessorEditor& owner;
        explicit StalePoll (KeySyncAudioProcessorEditor& o) : owner (o) { startTimer (400); }
        void timerCallback() override
        {
            const bool s = owner.processor.isStale();
            if (s != lastStale)
            {
                lastStale = s;
                owner.repaint();
            }
        }
        bool lastStale = false;
    };
    StalePoll stalePoll { *this };

    JUCE_DECLARE_NON_COPYABLE (KeySyncAudioProcessorEditor)
};

} // namespace keysync
