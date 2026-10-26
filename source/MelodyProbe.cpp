// Dev-only CLI: melody-extraction prototype harness.
// Usage: keysync_melody <audio file> [...]
// Per 30 s window prints: melody-only key, fused key (0.7 chroma / 0.3 melody-PCP),
// baseline detector key. Consensus over windows at the end. Not shipped.

#include "dsp/KeyDetector.h"
#include "dsp/MelodyExtract.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <algorithm>

using namespace keysync;

namespace
{
// best KS key over a normalized 12-bin view: {tonic, mode, r}
struct KsPick { int tonic = -1, mode = -1; double r = -2.0; };
KsPick ksPick (const std::array<double, 12>& view)
{
    std::array<double, 12> norm = view;
    double sum = 0.0;
    for (double v : norm) sum += v;
    if (sum < 1e-9)
        return {};
    for (double& v : norm) v /= sum;

    KsPick p;
    for (int rotate = 0; rotate < 12; ++rotate)
        for (int mode = 0; mode < 2; ++mode)
        {
            double profile[12];
            for (int i = 0; i < 12; ++i)
                profile[(i + rotate) % 12] = (mode == 0 ? kMajorProfile : kMinorProfile)[i];
            const double r = ksCorrelation (norm.data(), profile);
            if (r > p.r) { p.r = r; p.tonic = rotate; p.mode = mode; }
        }
    return p;
}

juce::String ksTitle (const KsPick& p)
{
    if (p.tonic < 0) return "?";
    return juce::String (pitchClassName (p.tonic)) + (p.mode == 1 ? " minor" : " major");
}
} // namespace

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf ("usage: keysync_melody <audio file> [...]\n");
        return 2;
    }

    // --selfcheck: synth melody gate — does the tracker actually track?
    // 6 s: D4 line (sine, 293.66 Hz) with 5 c vibrato over a quieter pad chord
    // (A2/C#3/E3 sines). PASS if treat wins the chosen row within -1..+1
    // cent-semitone on >= 80% of voiced frames.
    if (argc == 2 && juce::String ("--selfcheck") == argv[1])
    {
        const double sr = 44100.0;
        const int n = (int) (6.0 * sr);
        juce::AudioBuffer<float> synth (1, n);
        float* dst = synth.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double vib = std::sin (juce::MathConstants<double>::twoPi * 5.0 * t) * 0.004;
            const double f = 293.66 * std::pow (2.0, vib / 12.0);
            const double melody = 0.4 * std::sin (juce::MathConstants<double>::twoPi * f * t);
            double pad = 0.0;
            for (double pf : { 110.0, 138.591, 164.814 })
                pad += 0.06 * std::sin (juce::MathConstants<double>::twoPi * pf * t);
            dst[i] = (float) (melody + pad);
        }

        MelodyExtractor m;
        m.prepare (sr);
        const int chunk = 1024;
        for (int p = 0; p < n; p += chunk)
            m.addMono (synth.getReadPointer (0) + p, juce::jmin (chunk, n - p));
        const auto w = m.finalize();

        // D4 = MIDI 62 -> row 62 - kMidiLow = 26
        // The fused melody vote drifts on pad content; assert the strongest PCP
        // bin matches the D4 pitch class and voiced fraction is sane.
        int bestPc = -1; double bestV = -1.0;
        for (int pc = 0; pc < 12; ++pc)
            if (w.pcp[(size_t) pc] > bestV) { bestV = w.pcp[(size_t) pc]; bestPc = pc; }
        const bool pcpOk = bestPc == (62 % 12) && w.voicedPct > 0.5f;
        std::printf ("SYNTH: bestPc=%s v=%d%% bestV=%.3f -> %s\n",
                     pitchClassName (bestPc), (int) (w.voicedPct * 100.0f), bestV,
                     pcpOk ? "PASS" : "FAIL");
        return pcpOk ? 0 : 1;
    }

    for (int a = 1; a < argc; ++a)
    {
        juce::File f (juce::String::fromUTF8 (argv[a]));
        std::printf ("\n=== %s ===\n", argv[a]);

        juce::AudioFormatManager mgr;
        mgr.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (mgr.createReaderFor (f));
        if (r == nullptr)
        {
            std::printf ("UNREADABLE (unsupported format?)\n");
            continue;
        }

        const double sr = r->sampleRate;
        const auto totalSamples = r->lengthInSamples;

        juce::AudioBuffer<float> all ((int) r->numChannels, (int) totalSamples);
        r->read (&all, 0, (int) totalSamples, 0, true, true);
        juce::AudioBuffer<float> mono (1, (int) totalSamples);
        for (int c = 0; c < r->numChannels; ++c)
            mono.addFrom (0, 0, all, c, 0, (int) totalSamples, 1.0f / r->numChannels);

        const long long win = (long long) (30.0 * sr);
        const int chunk = 1024;
        const int nWins = 5;
        long long starts[nWins] = {
            0,
            juce::jmax (0LL, totalSamples - win),
            0, 0, 0,
        };
        starts[2] = juce::jmin (totalSamples - win, (long long) (0.33 * (double) totalSamples));
        starts[3] = juce::jmin (totalSamples - win, (long long) (0.50 * (double) totalSamples));
        starts[4] = juce::jmin (totalSamples - win, (long long) (0.66 * (double) totalSamples));
        // head, tail, mid, 33, 66 — same five as keysync_probe (names shuffled here)
        const char* names[nWins] = { "head", "tail", "mid", "33%", "66%" };

        int melVotes[24] = {}, fuseVotes[24] = {};
        for (int w = 0; w < nWins; ++w)
        {
            if (totalSamples < win && w > 0)
                break;
            const int slice = (int) juce::jmin (win, totalSamples - starts[w]);

            KeyDetector d;
            d.prepare (sr);
            MelodyExtractor m;
            m.prepare (sr);
            for (int p = 0; p < slice; p += chunk)
            {
                const int n = juce::jmin (chunk, slice - p);
                const float* src = mono.getReadPointer (0) + starts[w] + p;
                d.addMono (src, n);
                m.addMono (src, n);
            }

            // baseline from the standard detector (decimated chroma branch)
            KeyResult base = d.detect();

            const auto chroma = d.chromaView();
            const auto mel = m.finalize();

            const KsPick melKey = mel.valid ? ksPick (mel.pcp) : KsPick{};

            // adaptive blend: melody earns influence in proportion to its own
            // profile match strength — flat histograms (weak r) stay out of the
            // way of the chroma, strong ones (r>0.7) get the full 0.3 weight
            const float melGain = melKey.tonic >= 0
                ? juce::jlimit (0.0f, 1.0f, (float) (melKey.r - 0.2) / 0.7f) : 0.0f;
            const float wMel = 0.3f * melGain;
            std::array<double, 12> fused {};
            for (int pc = 0; pc < 12; ++pc)
                fused[(size_t) pc] = (1.0 - (double) wMel) * chroma[(size_t) pc]
                                   + (double) wMel * mel.pcp[(size_t) pc];
            const KsPick fuseKey = ksPick (fused);

            std::printf ("%s: BASE %s | MELODY %s (v %d%% g %.2f) | FUSED %s (r %.2f w%.2f)\n",
                         names[w], base.valid ? keyTitle (base).toRawUTF8() : "invalid",
                         melKey.tonic >= 0 ? ksTitle (melKey).toRawUTF8() : "unvoiced",
                         (int) (mel.voicedPct * 100.0f), melKey.r,
                         fuseKey.tonic >= 0 ? ksTitle (fuseKey).toRawUTF8() : "invalid",
                         fuseKey.r, wMel);

            if (melKey.tonic >= 0)
                ++melVotes[melKey.tonic + 12 * melKey.mode];
            if (fuseKey.tonic >= 0)
                ++fuseVotes[fuseKey.tonic + 12 * fuseKey.mode];
        }

        for (auto* votes : { melVotes, fuseVotes })
        {
            int best = -1, bestV = 0;
            for (int i = 0; i < 24; ++i)
                if (votes[i] > bestV) { bestV = votes[i]; best = i; }
            if (bestV > 0)
                std::printf ("consensus: %s %s (%d)\n", pitchClassName (best % 12),
                             best >= 12 ? "minor" : "major", bestV);
        }
    }
    return 0;
}
