// Self-test for the key detection + recommendation logic. No host required.
// Build: cmake --build build --target keysync_selftest && ./build/keysync_selftest

#include "dsp/KeyDetector.h"
#include "dsp/Recommender.h"

#include <cmath>
#include <cstdio>

using namespace keysync;

namespace
{
struct TestingDetector : KeyDetector
{
    // nothing extra; exposed via public API only
};

std::vector<float> renderNote (double sampleRate, double freq, double seconds, float amp = 0.3f)
{
    std::vector<float> out ((size_t) (seconds * sampleRate));
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double t = (double) i / sampleRate;
        out[i] = amp * (float) (std::sin (juce::MathConstants<double>::twoPi * freq * t)
                                + 0.35 * std::sin (juce::MathConstants<double>::twoPi * 2.0 * freq * t)
                                + 0.15 * std::sin (juce::MathConstants<double>::twoPi * 3.0 * freq * t));
    }
    return out;
}

double midiToFreq (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }

// renders a scale: sequential notes over ~4 seconds
void renderScale (double sampleRate, int tonicMidi, bool minor, std::vector<float>& out)
{
    static const int majorSteps[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
    static const int minorSteps[] = { 0, 2, 3, 5, 7, 8, 10, 12 };
    const int* steps = minor ? minorSteps : majorSteps;

    for (int s = 0; s < 8; ++s)
    {
        auto note = renderNote (sampleRate, midiToFreq (tonicMidi + steps[s]), 0.45);
        out.insert (out.end(), note.begin(), note.end());
    }
    // second pass, softer, so the detector has a solid sustained window
    for (int s = 0; s < 8; ++s)
    {
        auto note = renderNote (sampleRate, midiToFreq (tonicMidi + steps[s]), 0.45, 0.15f);
        out.insert (out.end(), note.begin(), note.end());
    }
}

int failures = 0;

#define CHECK(cond, msg)                                              \
    do                                                                \
    {                                                                 \
        if (! (cond))                                                                                   \
        {                                                                                             \
            std::printf ("FAIL: %s (line %d)\n", msg, __LINE__);                                        \
            ++failures;                                                                               \
        }                                                                                             \
        else                                                                                          \
            std::printf ("pass: %s\n", msg);                                                          \
    } while (false)

KeyResult detectRendered (double sampleRate, const std::vector<float>& audio)
{
    TestingDetector d;
    d.prepare (sampleRate);
    for (size_t start = 0; start < audio.size(); start += 512)
        d.addMono (audio.data() + start,
                   (int) std::min<size_t> (512, audio.size() - start));
    return d.detect();
}
} // namespace

int main()
{
    const double sr = 44100.0;

    std::vector<float> cMajor;
    renderScale (sr, 60, false, cMajor); // C major
    const KeyResult cm = detectRendered (sr, cMajor);
    CHECK (cm.valid, "C major renders valid detection");
    CHECK (cm.tonicPc == 0 && cm.mode == 0,
           ("expected C major, got " + juce::String (pitchClassName (cm.tonicPc)) + " " + cm.keyName()).toStdString());
    CHECK (cm.confidence > 0.2f, "C major confidence > 0.2");

    std::vector<float> aMinor;
    renderScale (sr, 57, true, aMinor); // A minor
    const KeyResult am = detectRendered (sr, aMinor);
    CHECK (am.valid, "A minor renders valid detection");
    CHECK (am.tonicPc == 9 && am.mode == 1,
           ("expected A minor, got " + juce::String (pitchClassName (am.tonicPc)) + " " + am.keyName()).toStdString());

    // silence / insufficient audio
    TestingDetector empty;
    empty.prepare (sr);
    const KeyResult silent = empty.detect();
    CHECK (! silent.valid, "silence yields invalid result (no usable audio)");

    std::vector<float> shortAudio (cMajor.begin(), cMajor.begin() + 512); // ~12 ms
    empty.addMono (shortAudio.data(), (int) shortAudio.size());
    CHECK (! empty.detect().valid, "12 ms of audio yields invalid result");

    // recommender: C major -> relative minor first; A minor -> relative major first
    {
        KeyResult c; c.valid = true; c.tonicPc = 0; c.mode = 0;
        const auto rec = recommendCompatibleKeys (c);
        CHECK (rec.size() >= 3, "C major produces suggestions");
        CHECK (rec[0].tonicPc == 9 && rec[0].mode == 1
               && std::string (rec[0].relationship) == "Relative minor",
               "C major first suggestion is A minor (Relative minor)");
        CHECK (std::string (rec[1].relationship) == "Dominant"
               && rec[1].tonicPc == 7 && rec[1].mode == 0,
               "C major second suggestion is G major (Dominant)");
    }
    {
        KeyResult a; a.valid = true; a.tonicPc = 9; a.mode = 1;
        const auto rec = recommendCompatibleKeys (a);
        CHECK (rec.size() >= 3 && rec[0].tonicPc == 0 && rec[0].mode == 0,
               "A minor first suggestion is C major (Relative major)");
    }
    {
        KeyResult bad; bad.valid = false;
        CHECK (recommendCompatibleKeys (bad).empty(), "no suggestions for invalid result");
    }

    std::printf (failures == 0 ? "\nall checks passed\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
