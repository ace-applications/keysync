#include "Recommender.h"

namespace keysync
{
namespace
{
int wrapPc (int pc) { return ((pc % 12) + 12) % 12; }
} // namespace

std::vector<KeySuggestion> recommendCompatibleKeys (const KeyResult& detected)
{
    std::vector<KeySuggestion> out;
    if (! detected.valid || detected.tonicPc < 0)
        return out;

    const int t = detected.tonicPc;

    if (detected.mode == 0) // detected major
    {
        // relative minor (+9), dominant (+7), subdominant (+5), parallel minor
        out.push_back ({ wrapPc (t + 9), 1, "Relative minor" });
        out.push_back ({ wrapPc (t + 7), 0, "Dominant" });
        out.push_back ({ wrapPc (t + 5), 0, "Subdominant" });
        out.push_back ({ wrapPc (t + 0), 1, "Parallel key" });
    }
    else // detected minor
    {
        // relative major (+3), dominant minor (+7), subdominant minor (+5), parallel major
        out.push_back ({ wrapPc (t + 3), 0, "Relative major" });
        out.push_back ({ wrapPc (t + 7), 1, "Dominant" });
        out.push_back ({ wrapPc (t + 5), 1, "Subdominant" });
        out.push_back ({ wrapPc (t + 0), 0, "Parallel key" });
    }

    return out;
}

} // namespace keysync
