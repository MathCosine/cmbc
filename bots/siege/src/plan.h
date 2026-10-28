#pragma once
// Map plan made during the draft. The draft's first pick has about 975,000 fuel (a game turn
// 100,000) and can read the whole map, so it measures walking distances with full-map searches and
// passes them to the champions in the 1,024-byte draft note:
//   - from our spawn and from the enemy's to every camp group (kDraftHome, two searches);
//   - between camp groups (kRoutePlan, one search per group): the squad looks one camp ahead.
#include <cstdint>
#include <vector>
#include "util.h"

// Out of 140 against the same bot without it (v28 settings + capture limit): look ahead always
// 57, only when calm 61, own-side next camps 64 (71 calm only), each also 7-13 worse against
// fishsim; farming with no opponent 1,076-1,099 against 1,088 rounds to 1,000 gems. Off; the
// draft measuring works (distances within 2-3 tiles of the game's) and is kept for other uses.
constexpr bool kRoutePlan = false;
// Round 1 runs short of fuel (setup takes 60-90k of the 100k), so the squad chose its first camp
// with most distances unknown and kept it: on 01concentrated-wealth only a 4-gem wolf 139 tiles
// away had a distance, and the squad walked to it for 170 rounds past the 71-gem middle camp.
// kDraftHome: spawn distances from the draft fill in each group's home walk and side at setup, and
// stand in for the walk to a group while we are on our spawn and its own field is not built yet.
// Out of 140 against v28 / Cassi & Issac / fishsim / hunter (v28 70/126/106/119; a build with
// every new switch off 68/123/109/120): 66/120/99/121, farming 1,114 against 1,088. The distances
// match the game's within a tile; with all of them known the first choice often goes to a big far
// camp (a contested 600-gold strip on 01yellow-brick-road) instead of the nearest ones. Off.
constexpr bool kDraftHome = false;
constexpr bool kDraftPlan = kRoutePlan || kDraftHome;

// Draft side: measure and write the note (spread over picks while fuel lasts).
void plan_in_draft();

// Game side: distances read back from the note, in World group ids.
struct RoutePlan {
    bool ok = false;
    bool pairs = false;  // camp-to-camp distances present
    int groups = 0;
    std::vector<std::uint16_t> dist;  // groups x groups, tiles; kUnreached when unknown
    std::vector<std::uint16_t> home;   // walk from our spawn, tiles; kUnreached when unknown
    std::vector<std::uint16_t> enemy;  // walk from the enemy spawn
    std::vector<std::vector<int>> near;  // per group, nearest planned groups (worked out on demand)

    int between(int a, int b) const {
        if (!ok || !pairs || a < 0 || b < 0 || a >= groups || b >= groups) return kUnreached;
        return dist[static_cast<std::size_t>(a) * groups + b];
    }
    const std::vector<int>& nearest(int a, int count);
    // Decode the note against the groups as the World built them; false leaves the plan off.
    bool load(int w, int h, const std::vector<vec2>& anchors);
};

// Encoding shared by both sides.
inline constexpr std::uint8_t kPlanVersion = 3;
