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

// U+00B7 as explicit UTF-8 bytes — raw non-ASCII literals in .cpp turn into "Â·" under MSVC
const juce::String sepDot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));

// bucket word must use the same threshold the state machine uses
// (kConfidenceResultPct = what makes an honest Result at all)
juce::String confidenceWord (float confidence)
{
    if (confidence < 0.60f)
        return "weak";
    if (confidence < 0.80f)
        return "medium";
    return "high";
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

    auto makeFont = [] (const juce::Typeface::Ptr& tf, float height, float kerning = 0.0f)
    {
        return juce::Font (juce::FontOptions().withTypeface (tf).withHeight (height)
                                                  .withKerningFactor (kerning));
    };

    titleFont = makeFont (isometra, 26.0f);
    keyFont = makeFont (interSemi, 52.0f, -0.02f);

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
        juce::ColourGradient orange (juce::Colour (0x1EFF8A24u), 340.0f, 40.0f,
                                     juce::Colour (0x00FF8A24u), 340.0f, 40.0f + 240.0f, true);
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

    // logo mark: assets/logo.png shipped in the binary data (replaces the old
    // hand-drawn waveform path)
    auto logo = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);
    if (logo.isValid())
        g.drawImage (logo, juce::Rectangle<float> (b.getX() + 2.0f, b.getY() + 10.0f, 84.0f, 36.0f),
                     juce::RectanglePlacement (juce::RectanglePlacement::centred), false);

    // wordmark: "ACE AUDIO" small, "KEY" white + "SYNC" orange in Isometra
    const float x = b.getX() + 96.0f;
    g.setFont (fontOf (owner.inter, 10.0f));
    g.setColour (uiColour (kGrey));
    g.drawText ("ACE AUDIO", x, b.getY() + 12.0f, 200.0f, 14.0f, juce::Justification::bottomLeft);

    // width measured, not hardcoded — Isometra metrics are irregular
    const float keyW = juce::GlyphArrangement::getStringWidth (owner.titleFont, "KEY");

    g.setFont (owner.titleFont);
    g.setColour (uiColour (kWhite));
    g.drawText ("KEY", x, b.getY() + 24.0f, keyW + 2.0f, 32.0f, juce::Justification::bottomLeft);
    g.setColour (uiColour (kOrange));
    g.drawText ("SYNC", x + keyW + 2.0f, b.getY() + 24.0f, 110.0f, 32.0f,
                juce::Justification::bottomLeft);
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
            auto strip = bb.removeFromBottom (44.0f); // confidence strip
            auto bpmRow = bb.removeFromBottom (26.0f); // BPM row (BPM is a Free feature)

            g.setFont (owner.keyFont);
            g.setColour (uiColour (kWhite));
            g.drawText (keyTitle (r), bb, text);

            // BPM: shown when the detector has an honest claim, else "BPM uncertain"
            if (r.bpm > 0.0f && r.bpmConfidence >= 0.5f)
            {
                g.setFont (fontOf (owner.interSemi, 15.0f));
                g.setColour (uiColour (kWhite));
                g.drawText (juce::String (juce::roundToInt (r.bpm)) + " BPM",
                            bpmRow, text);
            }
            else
            {
                g.setFont (fontOf (owner.inter, 12.0f));
                g.setColour (uiColour (kGrey));
                g.drawText ("BPM uncertain - not enough pulse in this audio", bpmRow, text);
            }

            g.setFont (fontOf (owner.inter, 12.0f));
            g.setColour (uiColour (kGrey));
            juce::String staleNote = owner.processor.isStale()
                ? sepDot + juce::String ("new audio since analysis") : juce::String();
            g.drawText (juce::String ("Confidence ") + confidencePercent (r)
                            + sepDot + confidenceWord (r.confidence) + staleNote,
                        strip, text);
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
    const auto b = getLocalBounds().toFloat();
    const bool busy = owner.processor.getState() == AnalyzeState::analyzing;

    juce::Path p;
    p.addRoundedRectangle (b, 14.0f);

    // soft orange under-shadow lifts the button off the navy bg
    if (! busy)
    {
        auto shadow = p;
        shadow.applyTransform (juce::AffineTransform::translation (0.0f, down ? 1.0f : 3.0f));
        g.setColour (uiColour (kOrange).withAlpha (down ? 0.18f : 0.30f));
        g.fillPath (shadow);
    }

    // vertical sheen: brighter crown into flat brand orange; fades when busy
    juce::Colour top = busy ? uiColour (kOrange).withAlpha (0.35f)
                      : down ? uiColour (kOrange).darker (0.12f)
                      : over ? uiColour (kOrange).brighter (0.12f)
                             : uiColour (kOrange).brighter (0.05f);
    juce::Colour bottom = busy ? uiColour (kOrange).withAlpha (0.25f)
                           : down ? uiColour (kOrange).darker (0.18f)
                                  : uiColour (kOrange).darker (0.06f);
    g.setGradientFill (juce::ColourGradient (top, 0.0f, b.getY(),
                                             bottom, 0.0f, b.getBottom(), false));
    g.fillPath (p);

    if (! busy)
    {
        // 1px inner highlight on the crown + hairline border
        juce::Rectangle<float> lip = b;
        lip = lip.removeFromTop (1.2f);
        g.setColour (juce::Colour (0x40ffffffu));
        g.fillRect (lip.reduced (12.0f, 0.0f));
        g.setColour (juce::Colour (0x26ffffffu).withAlpha (down ? 0.05f : 0.15f));
        g.strokePath (p, juce::PathStrokeType (1.0f));
    }

    g.setColour (busy ? juce::Colour (0x80140a03u) : juce::Colour (0xff140a03u));
    g.drawText (busy ? "ANALYZING" : "ANALYZE", b, juce::Justification::centred);
}

bool KeySyncAudioProcessorEditor::AnalyzeButton::hitTest (int, int)
{
    return owner.processor.getState() != AnalyzeState::analyzing;
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

    auto rows = b.reduced (14.0f, 4.0f);

    if (kIsProEdition)
    {
        if (owner.processor.getState() != AnalyzeState::result || ! r.valid)
        {
            g.setFont (fontOf (owner.inter, 13.0f));
            g.setColour (uiColour (kGrey));
            g.drawText ("Analyze to see compatible keys", rows, juce::Justification::centred);
        }

        const int rowsH = (int) rows.getHeight() / 4; // rows fill the panel, no dead space
        for (size_t i = 0; i < suggestions.size() && i < 4; ++i)
        {
            auto row = rows.removeFromTop ((float) rowsH);

            // relationship chip, right aligned
            const float chipW = 118.0f;
            auto chip = row.removeFromRight (chipW + 8.0f)
                             .withSizeKeepingCentre (chipW, 28.0f);
            juce::Path cp;
            cp.addRoundedRectangle (chip, chip.getHeight() * 0.5f);
            g.setColour (uiColour (kOrange).withAlpha (0.14f));
            g.fillPath (cp);
            g.setFont (fontOf (owner.inter, 11.0f));
            g.setColour (uiColour (kOrange));
            g.drawText (suggestions[i].relationship, chip, juce::Justification::centred);

            g.setFont (fontOf (owner.interSemi, 18.0f));
            g.setColour (uiColour (kWhite));
            g.drawText (juce::String (pitchClassName (suggestions[i].tonicPc)) + " "
                            + modeName (suggestions[i].mode),
                        row, juce::Justification::centredLeft);
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

// row index under the mouse -> plain-language gloss on the relationship chip
void KeySyncAudioProcessorEditor::ProPanel::hoverRow()
{
    // must stay in sync with paint(): header 30px, then rows padded 4px
    const int rowsH = juce::jmax (1, (getHeight() - 30 - 8) / 4);
    const int row = (int) (getMouseXYRelative().getY() - (30 + 4)) / rowsH;
    static constexpr const char* gloss[4] = {
        "Relative: same notes, opposite feel - always compatible",
        "Dominant: cadence tension, pulls home to this key",
        "Subdominant: colour change, shares most notes",
        "Parallel: same root, major/minor swapped",
    };
    setTooltip (row >= 0 && row < 4 ? juce::String (gloss[row]) : juce::String());
}

} // namespace keysync
