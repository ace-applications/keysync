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

        const auto res = d.detect();
        std::printf ("KEY  valid=%d conf=%.3f\n", (int) res.valid, res.confidence);
        if (res.bpm > 0.0f && res.bpmConfidence >= 0.5f)
            std::printf ("BPM %.2f conf %.3f\n", res.bpm, res.bpmConfidence);
        else
            std::printf ("BPM uncertain (bpm=%.2f conf=%.3f)\n", res.bpm, res.bpmConfidence);
    }
    return 0;
}
