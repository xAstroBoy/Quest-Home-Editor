#pragma once

namespace navigationfloor {

struct Rank {
    bool coversStart = false;
    bool compactFoundation = false;
    double score = 0.0;
    int stableId = -1;
};

inline bool better(const Rank& a, const Rank& b) {
    if (a.coversStart != b.coversStart)
        return a.coversStart > b.coversStart;
    if (a.coversStart && a.compactFoundation != b.compactFoundation)
        return a.compactFoundation > b.compactFoundation;
    if (a.score != b.score)
        return a.score > b.score;
    return a.stableId < b.stableId;
}

}  // namespace navigationfloor
