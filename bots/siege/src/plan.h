#pragma once
// Route plan made during the draft. The draft's first pick has about 975,000 fuel (a game turn
// 100,000) and can read the whole map, so it measures walking distances between camp groups and
// from our spawn with one full-map search per group, and passes them to the champions in the
// 1,024-byte draft note. In the game, the squad looks one camp ahead with them.
#include <cstdint>
#include <vector>
#include "util.h"

// Out of 140 against the same bot without it (v28 settings + capture limit): look ahead always
// 57, only when calm 61, own-side next camps 64 (71 calm only), each also 7-13 worse against
// fishsim; farming with no opponent 1,076-1,099 against 1,088 rounds to 1,000 gems. Off; the
// draft measuring works (distances within 2-3 tiles of the game's) and is kept for other uses.
constexpr bool kRoutePlan = false;

// Draft side: measure and write the note (spread over picks while fuel lasts).
void plan_in_draft();

// Game side: distances read back from the note, in World group ids.
struct RoutePlan {
    bool ok = false;
    int groups = 0;
    std::vector<std::uint16_t> dist;  // groups x groups, tiles; kUnreached when unknown
    std::vector<std::uint16_t> home;  // walk from our spawn, tiles; kUnreached when unknown
    std::vector<std::vector<int>> near;  // per group, nearest planned groups (worked out on demand)

    int between(int a, int b) const {
        if (!ok || a < 0 || b < 0 || a >= groups || b >= groups) return kUnreached;
        return dist[static_cast<std::size_t>(a) * groups + b];
    }
    const std::vector<int>& nearest(int a, int count);
    // Decode the note against the groups as the World built them; false leaves the plan off.
    bool load(int w, int h, const std::vector<vec2>& anchors);
};

// Encoding shared by both sides.
inline constexpr int kPlanMaxGroups = 43;
inline constexpr std::uint8_t kPlanVersion = 1;
