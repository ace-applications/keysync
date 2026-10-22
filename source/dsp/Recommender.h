#pragma once

#include "KeyDetector.h"

#include <vector>

namespace keysync
{

struct KeySuggestion
{
    int tonicPc = 0;
    int mode = 0;
    const char* relationship = ""; // short label, e.g. "Relative major"
};

// Ranked list of compatible keys for a detected key. Kept separate from the
// detector on purpose (brief section 4.2). Ordered by harmonic relevance.
std::vector<KeySuggestion> recommendCompatibleKeys (const KeyResult& detected);

} // namespace keysync
