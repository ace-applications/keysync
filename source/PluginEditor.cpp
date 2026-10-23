#include "PluginEditor.h"

#include "dsp/Recommender.h"

#include <BinaryData.h>

namespace keysync
{

#if KEYSYNC_EDITION_PRO == 1
static constexpr bool kIsProEdition = true;
#else
static constexpr bool kIsProEdition = false;
#endif

namespace
{
juce::Colour uiColour (uint32_t v) { return juce::Colour ((uint32_t) v); }

// FontOptions builder — avoids the removed (typeface, height) ctor
inline juce::Font fontOf (const juce::Typeface::Ptr& tf, float height)
{
    return juce::Font (juce::FontOptions().withTypeface (tf).withHeight (height));
}

juce::String confidencePercent (const KeyResult& r)
{
    return juce::String (int (r.confidence * 100.0f)) + "%";
}

// simulated glass panel (brief wants glassmorphism; a VST3 cannot blur what is
// behind it, so this is layered translucency + a hairline border)
void drawGlass (juce::Graphics& g, juce::Rectangle<float> r)
{
    juce::Path p;
    p.addRoundedRectangle (r.expanded (0.0f), 14.0f);
    g.setColour (juce::Colour (0x0bffffffu)); // ~4% white fill
    g.fillPath (p);
    g.setColour (juce::Colour (0x1fffffffu)); // hairline border
    g.strokePath (p, juce::PathStrokeType (1.0f));

    // top highlight
    juce::Rectangle<float> shine = r.removeFromTop (1.5f);
    g.setColour (juce::Colour (0x28ffffffu));
    g.fillRect (shine.reduced (10.0f, 0.0f));
}
} // namespace

// ---------------------------------------------------------------- editor

KeySyncAudioProcessorEditor::KeySyncAudioProcessorEditor (KeySyncAudioProcessor& processorIn)
    : AudioProcessorEditor (processorIn), processor (processorIn)
{
    isometra = juce::Typeface::createSystemTypefaceFor (BinaryData::IsometraRegular_ttf,
                                                        BinaryData::IsometraRegular_ttfSize);
    inter = juce::Typeface::createSystemTypefaceFor (BinaryData::InterRegular_ttf,
                                                     BinaryData::InterRegular_ttfSize);
    interSemi = juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf,
                                                         BinaryData::InterSemiBold_ttfSize);

    auto makeFont = [] (const juce::Typeface::Ptr& tf, float height)
    { return juce::Font (juce::FontOptions().withTypeface (tf).withHeight (height)); };

    titleFont = makeFont (isometra, 26.0f);
    keyFont = makeFont (isometra, 58.0f);

    addAndMakeVisible (title);
    addAndMakeVisible (keyDisplay);
    addAndMakeVisible (analyzeButton);
    addAndMakeVisible (proPanel);

    if constexpr (! kIsProEdition)
    {
        getPro.setColour (juce::HyperlinkButton::textColourId, uiColour (kOrange));
        getPro.setJustificationType (juce::Justification::centred);
        proPanel.addAndMakeVisible (getPro);
    }

    analyzeButton.onClick = [this]
    {
        if (processor.getState() == AnalyzeState::analyzing)
            return;

        processor.requestAnalysis();
        repaint();

        // keep the Analyzing state visible for one paint before the compute;
        // Timer is lifetime-safe (auto-stopped if the editor dies first)
        analyzePulse.startTimer (60);
    };

    // shortcut: "Get Pro" URL is a placeholder until ACE Audio ships a real store page
    setSize (380, 620);
}

void KeySyncAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (uiColour (kBg));

    // brand glow, palette-only: warm orange top-right, quiet grey bottom-left
    {
        juce::ColourGradient orange (juce::Colour (0x2EFF8A24u), 340.0f, 40.0f,
                                     juce::Colour (0x00FF8A24u), 340.0f, 40.0f + 300.0f, true);
        g.setGradientFill (orange);
        g.fillAll();
    }
    {
        juce::ColourGradient grey (juce::Colour (0x16A3A3A3u), 40.0f, 590.0f,
                                   juce::Colour (0x00A3A3A3u), 40.0f + 300.0f, 590.0f - 280.0f, true);
        g.setGradientFill (grey);
        g.fillAll();
    }
}

void KeySyncAudioProcessorEditor::resized()
{
    auto bound = getLocalBounds().reduced (16);

    title.setBounds (bound.removeFromTop (56));
    bound.removeFromTop (10);
    keyDisplay.setBounds (bound.removeFromTop (190));
    bound.removeFromTop (16);
    analyzeButton.setBounds (bound.removeFromTop (52).withSizeKeepingCentre (220, 52));
    bound.removeFromTop (18);
    proPanel.setBounds (bound.removeFromTop (236));
    getPro.setBounds (proPanel.getLocalBounds().removeFromBottom (30).reduced (8));
}

// ---------------------------------------------------------------- TitleBar

void KeySyncAudioProcessorEditor::TitleBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();

    // waveform logo mark (from the brand sheet)
    juce::Path wave;
    static constexpr float pts[] = { 0.0f, 0.55f, 0.08f, 0.40f, 0.16f, 0.60f, 0.24f, 0.25f,
        0.32f, 0.75f, 0.40f, 0.35f, 0.48f, 0.55f, 0.56f, 0.45f, 0.64f, 0.60f, 0.72f, 0.42f,
        0.80f, 0.55f, 0.88f, 0.45f, 1.0f, 0.50f };
    wave.startNewSubPath (b.getX(), b.getY() + pts[1] * b.getHeight());
    for (int i = 2; i < (int) (sizeof (pts) / sizeof (float)); i += 2)
        wave.lineTo (b.getX() + pts[i] * 48.0f, b.getY() + pts[i + 1] * b.getHeight());
    g.setColour (uiColour (kWhite));
    g.strokePath (wave, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved));

    // wordmark: "ACE AUDIO" small, "KEY" white + "SYNC" orange in Isometra
    const float x = b.getX() + 60.0f;
    g.setFont (fontOf (owner.inter, 10.0f));
    g.setColour (uiColour (kGrey));
    g.drawText ("ACE AUDIO", x, b.getY() + 12.0f, 200.0f, 14.0f, juce::Justification::bottomLeft);

    g.setFont (owner.titleFont);
    // fixed offsets: Isometra "KEY" ≈ 58 px at 26 pt (no width re-measure needed)
    g.setColour (uiColour (kWhite));
    g.drawText ("KEY", x, b.getY() + 24.0f, 58.0f, 32.0f, juce::Justification::bottomLeft);
    g.setColour (uiColour (kOrange));
    g.drawText ("SYNC", x + 58.0f, b.getY() + 24.0f, 110.0f, 32.0f, juce::Justification::bottomLeft);
}

// ---------------------------------------------------------------- KeyDisplay

void KeySyncAudioProcessorEditor::KeyDisplay::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds();
    drawGlass (g, b.toFloat());

    const AnalyzeState state = owner.processor.getState();
    const auto text = juce::Justification::centred;

    switch (state)
    {
        case AnalyzeState::idle:
        {
            g.setFont (fontOf (owner.inter, 15.0f));
            g.setColour (uiColour (kGrey));
            g.drawText ("Play audio, then press Analyze", b, text);
            break;
        }
        case AnalyzeState::analyzing:
        {
            g.setFont (owner.keyFont);
            g.setColour (uiColour (kGrey));
            g.drawText ("Analyzing", b, text);
            break;
        }
        case AnalyzeState::result:
        {
            const auto r = owner.processor.getLastResult();
            auto bb = b;
            g.setFont (owner.keyFont);
            g.setColour (uiColour (kWhite));
            g.drawText (keyTitle (r), bb, text);

            g.setFont (fontOf (owner.inter, 12.0f));
            g.setColour (uiColour (kGrey));
            g.drawText ("Confidence " + confidencePercent (r),
                        bb.removeFromBottom (44.0f), text);
            break;
        }
        case AnalyzeState::lowConfidence:
        {
            const auto r = owner.processor.getLastResult();
            auto bb = b;
            g.setFont (owner.keyFont);
            g.setColour (uiColour (kGrey));
            g.drawText ("Uncertain", bb, text);

            g.setFont (fontOf (owner.inter, 12.0f));
            g.setColour (uiColour (kOrange));
            g.drawText ("Low confidence - best guess: " + keyTitle (r)
                            + " (" + confidencePercent (r) + ")",
                        bb.removeFromBottom (44.0f), text);
            break;
        }
        case AnalyzeState::noUsableAudio:
        {
            g.setFont (fontOf (owner.inter, 15.0f));
            g.setColour (uiColour (kGrey));
            g.drawText ("Play audio to analyze", b, text);
            break;
        }
    }
}

// ---------------------------------------------------------------- AnalyzeButton

void KeySyncAudioProcessorEditor::AnalyzeButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.0f);

    juce::Colour fill = down ? uiColour (kOrange).brighter (0.12f)
                             : over ? uiColour (kOrange).brighter (0.06f)
                                    : uiColour (kOrange);

    juce::Path p;
    p.addRoundedRectangle (b, 14.0f);
    g.setColour (fill);
    g.fillPath (p);
    g.setColour (juce::Colour (0x66ffffffu).withAlpha (down ? 0.0f : 0.25f));
    g.strokePath (p, juce::PathStrokeType (1.0f));

    g.setFont (fontOf (owner.isometra, 20.0f));
    g.setColour (juce::Colour (0xff140a03u)); // near-black on orange
    g.drawText ("ANALYZE", b, juce::Justification::centred);
}

// ---------------------------------------------------------------- ProPanel

void KeySyncAudioProcessorEditor::ProPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    drawGlass (g, b);

    g.setFont (fontOf (owner.interSemi, 12.0f));
    g.setColour (uiColour (kGrey));
    g.drawText ("ALTERNATIVE KEYS", b.removeFromTop (30.0f).reduced (14.0f, 0.0f),
                juce::Justification::centredLeft);

    const auto r = owner.processor.getLastResult();
    const auto suggestions = recommendCompatibleKeys (r);
    const bool showSuggestions = kIsProEdition && owner.processor.getState() == AnalyzeState::result && r.valid;

    auto rows = b.removeFromTop (b.getHeight() - (kIsProEdition ? 0.0f : 34.0f)).reduced (0.0f, 6.0f);

    if (kIsProEdition || showSuggestions) // pro editions always render their list
    {
        if (owner.processor.getState() == AnalyzeState::lowConfidence)
        {
            g.setFont (fontOf (owner.inter, 12.0f));
            g.setColour (uiColour (kGrey));
            g.drawText ("Suggestions shown despite low confidence", rows, juce::Justification::centredTop);
        }

        const int rowsH = 36;
        for (size_t i = 0; i < suggestions.size(); ++i)
        {
            auto row = rows.removeFromTop ((float) rowsH);
            if (i >= 4)
                break; // brief: up to 5 entries total incl. spacing allowed, keep 4 core rows

            g.setFont (fontOf (owner.isometra, 20.0f));
            g.setColour (uiColour (kWhite));
            g.drawText (juce::String (pitchClassName (suggestions[i].tonicPc)) + " "
                            + modeName (suggestions[i].mode),
                        row, juce::Justification::centredLeft);

            // relationship chip, right aligned
            const float chipW = 118.0f;
            auto chip = row.removeFromRight (chipW).reduced (0.0f, 6.0f);
            juce::Path cp;
            cp.addRoundedRectangle (chip, chip.getHeight() * 0.5f);
            g.setColour (uiColour (kOrange).withAlpha (0.14f));
            g.fillPath (cp);
            g.setFont (fontOf (owner.inter, 11.0f));
            g.setColour (uiColour (kOrange));
            g.drawText (suggestions[i].relationship, chip, juce::Justification::centred);
        }
    }
    else if (! kIsProEdition)
    {
        // free edition: locked teaser
        g.setFont (fontOf (owner.inter, 13.0f));
        g.setColour (uiColour (kGrey));
        g.drawText ("Compatible keys, relative major/minor and harmonic options",
                    rows, juce::Justification::centred);
        g.drawText ("are available in KeySync Pro.",
                    rows.translated (0.0f, 18.0f), juce::Justification::centred);

        // three dimmed placeholder stacks
        g.setColour (juce::Colour (0x14ffffffu));
        for (int i = 0; i < 3; ++i)
        {
            auto row = rows.removeFromBottom (26.0f);
            juce::Path rp;
            rp.addRoundedRectangle (row.reduced (14.0f, 4.0f), 6.0f);
            g.fillPath (rp);
        }
    }
}

} // namespace keysync
