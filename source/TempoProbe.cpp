// Dev-only CLI: run the plugin's exact detection path on real audio files.
// Usage: keysync_probe <file1> [<file2> ...]
// Prints [DBG-tempo] lines for every tempo decision + the final KeyResult.
// Not part of the shipped plugin.

#include "dsp/KeyDetector.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <algorithm>

using namespace keysync;

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf ("usage: keysync_probe <audio file> [...]\n");
        return 2;
    }

    for (int a = 1; a < argc; ++a)
    {
        juce::File f (juce::String::fromUTF8 (argv[a]));
        std::printf ("\n=== %s ===\n", argv[a]);

        juce::AudioFormatManager mgr;
        mgr.registerBasicFormats(); // WAV/AIFF/MP3 (where supported)
        std::unique_ptr<juce::AudioFormatReader> r (mgr.createReaderFor (f));
        if (r == nullptr)
        {
            std::printf ("UNREADABLE (unsupported format?)\n");
            continue;
        }

        const double sr = r->sampleRate;
        const auto totalSamples = r->lengthInSamples;
        std::printf ("sr=%.0f ch=%d samples=%lld (%.1f s)\n",
                     sr, r->numChannels, (long long) totalSamples, totalSamples / sr);

        KeyDetector d;
        d.prepare (sr);

        // feed exactly like the plugin audio thread, interleaved->mono mix
        const int chunk = 512;
        juce::AudioBuffer<float> buf ((int) r->numChannels, chunk);
        for (long long pos = 0; pos < totalSamples; pos += chunk)
        {
            const int n = (int) std::min<long long> (chunk, totalSamples - pos);
            r->read (&buf, 0, n, pos, true, true);
            d.addMono (buf.getReadPointer (0), n);
        }

        // whole-file mono mix, analyzed window-by-window: the live plugin only
        // ever sees a rolling 30 s slice, so report several slices as DJs hear
        // them at different points in the track
        const long long win = (long long) (30.0 * sr);
        juce::AudioBuffer<float> all ((int) r->numChannels, (int) totalSamples);
        r->read (&all, 0, (int) totalSamples, 0, true, true);
        juce::AudioBuffer<float> mono (1, (int) totalSamples);
        for (int c = 0; c < r->numChannels; ++c)
            mono.addFrom (0, 0, all, c, 0, (int) totalSamples,
                          1.0f / r->numChannels);

        const int nWins = 5;
        long long starts[nWins] = {
            0,
            juce::jmax (0LL, totalSamples - win),
            0, 0, 0,
        };
        starts[2] = juce::jmin (totalSamples - win, (long long) (0.33 * (double) totalSamples));
        starts[3] = juce::jmin (totalSamples - win, (long long) (0.66 * (double) totalSamples));
        starts[4] = juce::jmin (totalSamples - win, (long long) (0.50 * (double) totalSamples));
        const char* names[nWins] = { "head", "tail", "33%", "66%", "mid" };

        int tonicVotes[24] = {};
        float headBpm = 0.f; // representative BPM: head window (calibrated vs 71/102 truths)
        for (int w = 0; w < nWins; ++w)
        {
            if (totalSamples < win && w > 0)
                break;
            KeyDetector d;
            d.prepare (sr);
            const int slice = (int) juce::jmin (win, totalSamples - starts[w]);
            for (int p = 0; p < slice; p += chunk)
            {
                const int n = juce::jmin (chunk, slice - p);
                d.addMono (mono.getReadPointer (0) + starts[w] + p, n);
            }
            const auto res = d.detect();
            if (w == 0)
                headBpm = res.bpm;
            std::printf ("%s: KEY %s conf %.3f | BPM %.2f conf %.3f\n",
                         names[w], res.valid ? keyTitle (res).toRawUTF8()
                                             : "invalid",
                         res.confidence, res.bpm, res.bpmConfidence);
            if (res.valid)
                ++tonicVotes[res.tonicPc + 12 * res.mode];
        }
        int best = -1, bestV = 0;
        for (int i = 0; i < 24; ++i)
            if (tonicVotes[i] > bestV)
                { bestV = tonicVotes[i]; best = i; }
        if (bestV > 0)
        {
            std::printf ("consensus (%d votes, ties kept):", bestV);
            for (int i = 0; i < 24; ++i)
                if (tonicVotes[i] == bestV)
                    std::printf (" %s %s", pitchClassName (i % 12),
                                 i >= 12 ? "minor" : "major");
            std::printf (" | full vote: [");
            for (int i = 0; i < 24; ++i)
                if (tonicVotes[i] > 0)
                    std::printf (" %s%s=%d", pitchClassName (i % 12),
                                 i >= 12 ? "m" : "", tonicVotes[i]);
            std::printf (" ]\n");
        }

        // MIREX score against a "Name - F Major - 71 BPM.mp3"-style label when
        // the filename carries one (exact 1.0, fifth 0.5, relative 0.3,
        // parallel 0.2; BPM counted at the project's +-8% tier)
        {
            const juce::String name (argv[a]);
            // note letter -> pitch class: C C# D D# E F F# G G# A A# B
            const juce::String noteLetters ("CCDDEFFGGAAB");
            int p1 = name.indexOf (" - ");
            int p2 = p1 >= 0 ? name.indexOf (p1 + 3, " - ") : -1;
            const int labelTonic = p1 >= 0
                ? noteLetters.indexOf (name.substring (p1 + 3, p1 + 3 + 1))
                : -1;
            if (p1 >= 0 && p2 > p1 && labelTonic >= 0 && bestV > 0)
            {
                const juce::String keyToken = name.substring (p1 + 3, p2);
                const juce::String tail = name.substring (p2 + 3);
                const bool labelMajor = keyToken.containsIgnoreCase ("Major");
                const bool consMajor = best < 12;
                const int consTonic = best % 12;
                auto sameModality = [] (bool a, bool b) { return a == b; };
                double kScore = 0.0;
                if (consTonic == labelTonic && sameModality (consMajor, labelMajor))
                    kScore = 1.0;
                else if (sameModality (consMajor, labelMajor)
                         && ((consTonic - labelTonic + 12) % 12 == 7
                             || (labelTonic - consTonic + 12) % 12 == 7))
                    kScore = 0.5;
                else if (! sameModality (consMajor, labelMajor)
                         && (labelMajor ? consTonic == (labelTonic + 9) % 12
                                        : consTonic == (labelTonic + 3) % 12))
                    kScore = 0.3;
                else if (consTonic == labelTonic)
                    kScore = 0.2;

                double bScore = 0.0;
                const int bPos = tail.indexOf (" BPM");
                if (bPos > 0)
                {
                    const double labelBpm = tail.substring (0, bPos).getDoubleValue();
                    if (labelBpm > 0.0 && headBpm > 0.0f
                        && std::abs (headBpm - labelBpm) <= labelBpm * 0.08)
                        bScore = 1.0;
                    std::printf ("MIREX: key=%.2f (got %s %s vs %s) bpm=%.2f (got %.1f vs %.0f)\n",
                                 kScore,
                                 pitchClassName (consTonic), consMajor ? "major" : "minor",
                                 keyToken.toRawUTF8(),
                                 bScore, headBpm, labelBpm);
                }
                else
                {
                    std::printf ("MIREX: key=%.2f (got %s %s vs %s)\n", kScore,
                                 pitchClassName (consTonic), consMajor ? "major" : "minor",
                                 keyToken.toRawUTF8());
                }
            }
        }
    }
    return 0;
}
