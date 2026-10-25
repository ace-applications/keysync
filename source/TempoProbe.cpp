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
    }
    return 0;
}
