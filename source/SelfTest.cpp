// Self-test for the key detection + recommendation logic. No host required.
// Build: cmake --build build --target keysync_selftest && ./build/keysync_selftest

#include "dsp/KeyDetector.h"
#include "dsp/Recommender.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace keysync;

namespace
{
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

bool closeTo (double got, double want)
{
    return got > 0.0 && std::abs (got - want) <= want * 0.08; // MIREX ±8% tier
}

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

std::vector<float> renderVoicedNote (double sampleRate, double freq, double seconds,
                                     float amp, double vibratoCents, double vibHz)
{
    std::vector<float> out ((size_t) (seconds * sampleRate));
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double t = (double) i / sampleRate;
        // vibrato pitch wobble + a detuned double (choir-spread voice)
        const double cents = vibratoCents * std::sin (juce::MathConstants<double>::twoPi * vibHz * t);
        const double f = freq * std::pow (2.0, cents / 1200.0);
        const double f2 = freq * std::pow (2.0, (-vibratoCents * 0.6
            + 18.0 * std::sin (juce::MathConstants<double>::twoPi * (vibHz * 1.13) * t)) / 1200.0);
        out[i] = amp * 0.5f * (float) (std::sin (juce::MathConstants<double>::twoPi * f * t)
            + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 2.0 * f * t)
            + 0.35 * std::sin (juce::MathConstants<double>::twoPi * f2 * t)
            + 0.10 * std::sin (juce::MathConstants<double>::twoPi * 3.0 * f * t));
    }
    return out;
}

// scale of voiced (vibrato + detune) notes, like a small sung ensemble
void renderVoicedScale (double sampleRate, int tonicMidi, bool minor, std::vector<float>& out)
{
    static const int majorSteps[] = { 0, 2, 4, 5, 7, 9, 11, 12 };
    static const int minorSteps[] = { 0, 2, 3, 5, 7, 8, 10, 12 };
    const int* steps = minor ? minorSteps : majorSteps;
    for (int s = 0; s < 8; ++s)
    {
        auto note = renderVoicedNote (sampleRate, midiToFreq (tonicMidi + steps[s]), 0.45,
                                      0.3f, 20.0, 5.4);
        out.insert (out.end(), note.begin(), note.end());
    }
    for (int s = 0; s < 8; ++s)
    {
        auto note = renderVoicedNote (sampleRate, midiToFreq (tonicMidi + steps[s]), 0.45,
                                      0.15f, 15.0, 4.7);
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
    KeyDetector d;
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
           ("expected C major, got " + keyTitle (cm)).toStdString());
    CHECK (cm.confidence > 0.2f, "C major confidence > 0.2");

    std::vector<float> aMinor;
    renderScale (sr, 57, true, aMinor); // A minor
    const KeyResult am = detectRendered (sr, aMinor);
    CHECK (am.valid, "A minor renders valid detection");
    CHECK (am.tonicPc == 9 && am.mode == 1,
           ("expected A minor, got " + keyTitle (am)).toStdString());

    // silence / insufficient audio
    KeyDetector emptyDetector;
    emptyDetector.prepare (sr);
    const KeyResult silent = emptyDetector.detect();
    CHECK (! silent.valid, "silence yields invalid result (no usable audio)");

    std::vector<float> shortAudio (cMajor.begin(), cMajor.begin() + 512); // ~12 ms
    emptyDetector.addMono (shortAudio.data(), (int) shortAudio.size());
    CHECK (! emptyDetector.detect().valid, "12 ms of audio yields invalid result");

    // recommender: C major -> relative minor first; A minor -> relative major first
    {
        KeyResult c; c.valid = true; c.tonicPc = 0; c.mode = 0;
        const auto rec = recommendCompatibleKeys (c);
        CHECK (rec.size() >= 3, "C major produces suggestions");
        CHECK (rec[0].tonicPc == 9 && rec[0].mode == 1
               && std::string (rec[0].relationship) == "Relative",
               "C major first suggestion is A minor (Relative)");
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
    // voiced (vibrato + choir-spread) A minor: sung texture must not break the key
    {
        std::vector<float> voiced;
        renderVoicedScale (sr, 45, true, voiced); // A2 minor
        const auto r = detectRendered (sr, voiced);
        CHECK (r.valid && r.tonicPc == 9 && r.mode == 1 && r.confidence > 0.2f,
               ("voiced A minor survives vibrato/detune, got "
                + std::string (r.tonicPc >= 0 ? (std::string (pitchClassName (r.tonicPc)) + " " + r.modeText()) : "invalid")
                + " conf " + std::to_string (r.confidence)).c_str());
    }

    {
        KeyResult bad; bad.valid = false;
        CHECK (recommendCompatibleKeys (bad).empty(), "no suggestions for invalid result");
    }

    // ---------------- BPM (free feature) ----------------
    // click-train fixture: 15 s of quarter-note impulses
    auto renderClicks = [sr] (double bpm, double seconds = 15.0)
    {
        std::vector<float> out ((size_t) (seconds * sr), 0.0f);
        const double periodSamples = 60.0 * sr / bpm;
        for (double beat = 0.0; beat * periodSamples + 3.0 < (double) out.size(); beat += 1.0)
        {
            auto& at = out[(size_t) (beat * periodSamples)];
            out[(size_t) (beat * periodSamples + 1)] = 0.8f;
            at = 0.8f;
            out[(size_t) (beat * periodSamples + 2)] = -0.4f;
        }
        return out;
    };

    for (const double bpmFixture : { 90.0, 120.0, 140.0 })
    {
        const auto r = detectRendered (sr, renderClicks (bpmFixture));
        const bool close = r.bpm > 0.0f && std::abs (r.bpm - bpmFixture) <= bpmFixture * 0.08;
        CHECK (close && r.bpmConfidence >= 0.5f,
               ("click train " + std::to_string ((int) bpmFixture) + " BPM found, got "
                + std::to_string (r.bpm)).c_str());
    }

    // octave family: eighth-note density at 120 still lands on the 120 family
    {
        const auto r = detectRendered (sr, renderClicks (240.0)); // 8ths of 120
        const bool family = closeTo (r.bpm, 120.0) || closeTo (r.bpm, 240.0)
                         || closeTo (r.bpm, 60.0);
        CHECK (r.bpm > 0.0f && family,
               ("eighth-note density lands in 120 family, got " + std::to_string (r.bpm)).c_str());
    }

    // silence: no tempo claim at all
    {
        KeyDetector d; d.prepare (sr);
        CHECK (d.detect().bpm == 0.0f, "silence yields no BPM claim");
    }

    // sustained sine (pad): flux ~ flat, tempo must be flagged uncertain
    {
        const auto r = detectRendered (sr, renderNote (sr, midiToFreq (69.0), 15.0));
        CHECK (r.bpmConfidence < 0.5f,
               ("sustained sine yields uncertain BPM (conf "
                + std::to_string (r.bpmConfidence) + ")").c_str());
    }

    // mixed content: kick pulse under a sustained bassline (real-music-ish level)
    // window is 30 s now, so fixtures should cover the whole window
    {
        std::vector<float> mix = renderClicks (128.0, 30.0);
        const std::vector<float> bass = renderNote (sr, midiToFreq (36), 30.0, 0.12f);
        for (size_t i = 0; i < mix.size() && i < bass.size(); ++i)
            mix[i] += bass[i] * 0.8f;
        const auto r = detectRendered (sr, mix);
        CHECK (r.bpm > 0.0f && closeTo (r.bpm, 128.0) && r.bpmConfidence >= 0.5f,
               ("mixed kick+bass lands 128 BPM with real confidence, got "
                + std::to_string (r.bpm) + " conf " + std::to_string (r.bpmConfidence)).c_str());
    }

    // dense pop realism: kick with decay every beat, snare on 2/4, hats on 8ths,
    // bass line, pad chord — the kind of mix the user reports failing
    {
        const double bpmFixture = 124.0;
        const int numSamples = (int) (30.0 * sr);
        std::vector<float> mix ((size_t) numSamples, 0.0f);
        const double beatLen = 60.0 / bpmFixture * sr;

        // kick: pitch-dropping thump each beat
        for (double beat = 0.0; beat * beatLen + 4000.0 < (double) numSamples; beat += 1.0)
            for (int i = 0; i < 4000; ++i)
            {
                const double t = (double) i / sr;
                mix[(size_t) (beat * beatLen) + (size_t) i] +=
                    0.9f * (float) std::exp (-t * 18.0)
                        * (float) std::sin (juce::MathConstants<double>::twoPi * 55.0 * t);
            }
        // snare: noise burst on beats 2 and 4
        for (double beat = 1.0; beat * beatLen + 3000.0 < (double) numSamples; beat += 2.0)
        {
            unsigned seed = (unsigned) (beat * 7.0);
            for (int i = 0; i < 3000; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                const float n = (float) (seed >> 16) / 32768.0f - 1.0f;
                const float decay = (float) std::exp (-((double) i / sr) * 22.0);
                mix[(size_t) (beat * beatLen) + (size_t) i] += 0.5f * n * decay;
            }
        }
        // hats: short noise ticks on 8ths
        for (double eighth = 0.0; eighth * beatLen * 0.5 + 2000.0 < (double) numSamples; eighth += 1.0)
        {
            unsigned seed = 12345u + (unsigned) (eighth * 13.0);
            for (int i = 0; i < 1500; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                const float n = (float) (seed >> 16) / 65535.0f - 1.0f;
                const float decay = (float) std::exp (-((double) i / sr) * 60.0);
                mix[(size_t) (eighth * beatLen * 0.5) + (size_t) i] += 0.18f * n * decay;
            }
        }
        // bass + pad: sustained content under the drums
        const double chunk = beatLen; // one note per beat
        for (double beat = 0.0; beat * chunk + chunk < (double) numSamples; beat += 4.0)
        {
            const int midi = 36 + (int) beat % 4 * 3; // slow movement
            const int from = std::min (numSamples - 1, (int) (beat * chunk));
            const int len = std::min (numSamples - from, (int) chunk);
            for (int i = 0; i < len; ++i)
                mix[(size_t) (from + i)] += 0.14f * (float) std::sin (
                    juce::MathConstants<double>::twoPi * midiToFreq (midi) * ((double) i / sr)
                    + (double) from / sr * juce::MathConstants<double>::twoPi * midiToFreq (midi));
        }

        const auto r = detectRendered (sr, mix);
        CHECK (r.bpm > 0.0f && closeTo (r.bpm, bpmFixture) && r.bpmConfidence >= 0.5f,
               ("dense pop mix lands ~124 BPM confident, got "
                + std::to_string (r.bpm) + " conf " + std::to_string (r.bpmConfidence)).c_str());
    }

    std::printf (failures == 0 ? "\nall checks passed\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
