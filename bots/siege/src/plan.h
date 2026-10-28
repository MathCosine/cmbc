#pragma once
// Map plan made during the draft. The draft's first pick has about 975,000 fuel (a game turn
// 100,000) and can read the whole map, so it measures walking distances with full-map searches
// and passes them to the champions in the 1,024-byte draft note:
//   - from our spawn and from each altar on our side or in the middle to every camp group
//     (kBankCost: what banking a camp's gems costs in walking);
//   - between camp groups (kRoutePlan: the squad looks one camp ahead).
#include <algorithm>
#include <cstdint>
#include <vector>
#include "util.h"

// Out of 140 against the same bot without it (v28 settings + capture limit): look ahead always
// 57, only when calm 61, own-side next camps 64 (71 calm only), each also 7-13 worse against
// fishsim; farming with no opponent 1,076-1,099 against 1,088 rounds to 1,000 gems. Off; the
// draft measuring works (distances within 2-3 tiles of the game's) and is kept for other uses.
constexpr bool kRoutePlan = false;
// Banking walk in camp scores: a camp's gems have to be carried to an altar, so each camp's cost
// includes kBankWeight times its share of a round trip to the nearest altar on our side or in the
// middle (a bag holds kCarryGems). Camps and gem patches beside an altar rise; far gem camps fall.
constexpr bool kBankCost = false;
constexpr float kBankWeight = 1.0f;
// Out of 140 against the same bot without it (v28 settings), weight 1 / 0.5: 66/68, against Cassi &
// Issac 117/115 (126), fishsim 99/101 (106), hunter 116/120 (119); farming 1,180/1,134 rounds to
// 1,000 gems against 1,088. Charging every gem made gems look worse than gold everywhere.
// kBankRelative: charge only the walk beyond the median camp's (nearer camps gain, up to a quarter
// of their rounds), so gems keep their worth against gold. kBankMaxSide: 0 banks only on our side
// (camps by a middle altar drew the squad into contested ground), 1 also in the middle.
// Relative, all safe altars / our side only: 69/68, Cassi & Issac 113/112, fishsim 102/94, hunter
// 121/122, farming 1,128/1,125. Every version farmed slower: champions already bank from wherever
// the bag fills, so pulling the squad toward altar camps only added walking. All off.
constexpr bool kBankRelative = false;
constexpr int kBankMaxSide = 1;
constexpr bool kDraftPlan = kRoutePlan || kBankCost;

// Draft side: measure and write the note (spread over picks while fuel lasts).
void plan_in_draft();

// Game side: distances read back from the note, in World group ids.
struct RoutePlan {
    bool ok = false;
    bool pairs = false;  // camp-to-camp distances present
    int groups = 0;
    std::vector<std::uint16_t> dist;  // groups x groups, tiles; kUnreached when unknown
    std::vector<std::uint16_t> home;  // walk from our spawn, tiles; kUnreached when unknown
    std::vector<std::uint16_t> bank;  // walk to the nearest altar on our side or in the middle
    std::vector<std::vector<int>> near;  // per group, nearest planned groups (worked out on demand)

    int between(int a, int b) const {
        if (!ok || !pairs || a < 0 || b < 0 || a >= groups || b >= groups) return kUnreached;
        return dist[static_cast<std::size_t>(a) * groups + b];
    }
    const std::vector<int>& nearest(int a, int count);
    // Decode the note against the groups as the World built them; false leaves the plan off.
    bool load(int w, int h, const std::vector<vec2>& anchors);
};

inline constexpr std::uint8_t kPlanVersion = 2;
inline constexpr std::uint16_t kBankUnknown = 65534;  // World::bank_cache: not worked out yet

// 0: our side, 1: the middle, 2: the enemy side (mine/theirs: the spawn centres World uses).
inline int altar_side(vec2 altar, vec2 mine_centre, vec2 their_centre) {
    const int mine = cheb(altar, mine_centre), theirs = cheb(altar, their_centre);
    const int margin = std::max(4, (mine + theirs) / 10);
    if (theirs < mine - margin) return 2;
    if (theirs <= mine + margin) return 1;
    return 0;
}
