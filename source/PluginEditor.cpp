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

#if KEYSYNC_EDITION_PRO == 1
    addAndMakeVisible (melodyLane);
#endif

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
#if KEYSYNC_EDITION_PRO == 1
    setSize (380, 780); // melody lane added below the alt-keys panel
#else
    setSize (380, 620);
#endif
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

#if KEYSYNC_EDITION_PRO == 1
    bound.removeFromTop (16);
    melodyLane.setBounds (bound.removeFromTop (140));
#endif
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

void KeySyncAudioProcessorEditor::KeyDisplay::drawBpmRow (juce::Graphics& g,
                                                          juce::Rectangle<float> row,
                                                          const KeyResult& r) const
{
    if (r.bpm > 0.0f && r.bpmConfidence >= 0.5f)
    {
        g.setFont (fontOf (owner.interSemi, 15.0f));
        g.setColour (uiColour (kWhite));
        g.drawText (juce::String (juce::roundToInt (r.bpm)) + " BPM",
                    row, juce::Justification::centred);
    }
    else
    {
        g.setFont (fontOf (owner.inter, 12.0f));
        g.setColour (uiColour (kGrey));
        g.drawText ("BPM uncertain - not enough pulse in this audio",
                    row, juce::Justification::centred);
    }
}

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

            drawBpmRow (g, bpmRow.toFloat(), r);

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
            // same layout contract as the result state, so the BPM row never
            // vanishes going in or out of the uncertain state
            auto bb = b;
            auto strip = bb.removeFromBottom (44.0f);
            auto bpmRow = bb.removeFromBottom (26.0f);

            g.setFont (owner.keyFont);
            g.setColour (uiColour (kGrey));
            g.drawText ("Uncertain", bb, text);

            drawBpmRow (g, bpmRow.toFloat(), r);

            g.setFont (fontOf (owner.inter, 12.0f));
            g.setColour (uiColour (kOrange));
            g.drawText ("Low confidence - best guess: " + keyTitle (r)
                            + " (" + confidencePercent (r) + ")",
                        strip, text);
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

    // core bloom: a warm light pooled at the crown, inside the button shape;
    // hovers brighter — the "live" signal without moving anything
    if (! busy)
    {
        g.saveState();
        g.reduceClipRegion (p);
        const float bloomA = down ? 0.34f : over ? 0.30f : 0.16f;
        juce::ColourGradient bloom (uiColour (kOrange).withAlpha (bloomA),
                                    b.getCentreX(), b.getY() + b.getHeight() * 0.10f,
                                    juce::Colour (0x00FF8A24u),
                                    b.getCentreX(), b.getBottom() + b.getHeight() * 0.25f, false);
        g.setGradientFill (bloom);
        g.fillRect (b);
        g.restoreState();
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
        // rows show whenever there is anything to read — including the
        // low-confidence best guess; the placeholder is only for "no result yet"
        const bool ready = r.valid
            && (owner.processor.getState() == AnalyzeState::result
                || owner.processor.getState() == AnalyzeState::lowConfidence);
        if (! ready)
        {
            g.setFont (fontOf (owner.inter, 13.0f));
            g.setColour (uiColour (kGrey));
            g.drawText ("Analyze to see compatible keys", rows, juce::Justification::centred);
        }
        else
        {
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

// Pro-only rolling melody contour: oldest on the left, latest hop at the
// right edge (1 px per full-rate hop ≈ 46.4 ms → ~16 s visible). Painted from
// the detector's seqlocked melody ring, one Path per hop-batch — no Timer
// repaint storm, no flicker. Conf → alpha, unvoiced frames = line breaks.
void KeySyncAudioProcessorEditor::MelodyLane::paint (juce::Graphics& g)
{
    auto bb = getLocalBounds();
    const auto b = bb.toFloat();
    drawGlass (g, b);

    // header label
    g.setFont (fontOf (owner.interSemi, 12.0f));
    g.setColour (uiColour (kGrey));
    g.drawText ("MELODY", bb.removeFromTop (24).reduced (14, 0).toFloat(),
                juce::Justification::centredLeft);

    const KeyDetector& det = owner.processor.detectorView();
    const int cap = det.melodyRingSize();
    const float rowH = (float) getHeight() / (float) KeyDetector::kMelodyRows;

    // octave guide lines + labels at every C row
    for (int octave = 0; octave < 4; ++octave)
    {
        const float y = (float) (KeyDetector::kMelodyRows - 1 - octave * 12) * rowH;
        auto track = juce::Rectangle<float> (b.getX(), b.getY() + y, b.getWidth(), 1.0f);
        g.setColour (uiColour (kGrey).withAlpha (0.12f));
        g.fillRect (track);
        g.setFont (fontOf (owner.inter, 9.0f));
        g.setColour (uiColour (kGrey).withAlpha (0.45f));
        g.drawText (juce::String (("C2C3C4C5") + octave * 2, 2), 8.0f,
                    b.getY() + y - 7.0f, 40.0f, 14.0f, juce::Justification::centredLeft);
    }

    const int write = det.melodyWriteIndex();
    float yPrev = 0.0f;
    bool lineOpen = false;
    float confSum = 0.0f;
    int confCount = 0;

    juce::Path line, glow;
    const float x0 = b.getX(), y0 = b.getY(), h = b.getHeight() - 24.0f;

    for (int col = 0; col < (int) b.getWidth(); ++col)
    {
        const int idx = ((write - 1 - col) % cap + cap) % cap;
        MelodySample s;
        if (! det.tryReadMelody (det.melodyRingData()[idx], s) || s.voiced == 0)
        {
            lineOpen = false;
            continue;
        }
        const float x = x0 + (float) (b.getWidth() - 1 - col);
        const float y = y0 + 24.0f + (float) (KeyDetector::kMelodyRows / 2 - s.row)
                          * rowH;
        if (lineOpen)
        {
            confSum += s.conf;
            ++confCount;
            line.lineTo (x, y);
        }
        else
        {
            line.startNewSubPath (x, y);
            confSum = s.conf;
            confCount = 1;
            lineOpen = true;
        }
        yPrev = y;
    }
    (void) yPrev; (void) h;

    if (confCount > 0)
    {
        const float conf = confSum / (float) confCount;
        glow.setUsingNonZeroWinding (false);
        glow.addPath (line);
        g.setColour (uiColour (kOrange).withAlpha (0.20f + 0.30f * conf));
        g.strokePath (glow, juce::PathStrokeType (6.0f + 3.0f * conf,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        g.setColour (uiColour (kOrange).withAlpha (0.55f + 0.45f * conf));
        g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }
}

} // namespace keysync
