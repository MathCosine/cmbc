// Draft: picks are simultaneous and the opponent cannot take ours, so a fixed
// priority list is enough. Basic attacks dominate camp damage, while stuns cut
// what camps and enemies deal back:
//   Vendetta   70 Strength melee, kills (including camps) reset her cooldowns
//   Vesper     55 Strength ranged with an attack reset, and a third more gold
//   Ryth       1200 health tank with an area stun that catches paired camps
//   Tavi       1.4x Focus damage + stun, and shops anywhere on the map
//   Marcella   1200 health, stun immune, gold for every gem she tributes
// Vesper Vale in place of Willow won 50-30 over 80 games; Prism (strong on
// gem-heavy maps for teams built around her) lost badly in this squad.
// Cassi & Issac look strong on paper (two bodies for one pick) but share one
// life: enemies kill 600-health Cassi and take both, so they come last.
#include "draft.h"
#include <cambc.h>
#include <algorithm>
#include <array>
#include <string_view>
#include <span>

namespace {
using cambc::champion_draft;

constexpr std::array kPriority{
    champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::ryth, champion_draft::tavi, champion_draft::marcella, champion_draft::willow, champion_draft::varg, champion_draft::heart_of_ruin, champion_draft::ren, champion_draft::stellar, champion_draft::prism, champion_draft::cassi_and_issac,
};
} // namespace

// Heart of Ruin in place of Tavi with five (4 of the 5 strongest ladder teams in the v20/v21 games
// pick him: 30 Strength but the fastest Focus growth, and up to 50% more damage as he bleeds).
constexpr bool kHeartDraft = false;
// Out of 140: 51 against v21, 107 against a Cassi & Issac team, 72 against fishsim (v21 70/115/86):
// off. Varg in place of Tavi instead: his basic attacks splash, and most camps come in packs.
// Out of 140 (as above): 63/112/83, farming 1% faster: off.
constexpr bool kVargDraft = false;

void play_draft() {
    static bool wrote_memory = false;
    if (!wrote_memory) {
        cambc::draft_memory = std::string_view("siege2");
        wrote_memory = true;
    }
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    if (chosen.size() >= cambc::draft::team_size()) return;
    // Maps set 3, 4 or 5 champions a side. With 3 or 4 we won 20-25% of ladder games against 39%
    // with five: the list above gave us Vendetta, Vesper Vale and Ryth, who died 6-7 times each a
    // game. Without Vendetta and with both stunners (Marcella, Tavi), the 14 small-team maps went
    // 173/196 against 147/196 locally.
    constexpr std::array kSmall{champion_draft::vesper_vale, champion_draft::marcella, champion_draft::ryth, champion_draft::tavi, champion_draft::vendetta, champion_draft::willow, champion_draft::varg, champion_draft::heart_of_ruin};
    const bool small_team = cambc::draft::team_size() <= 4;
    constexpr std::array kHeart{champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::ryth, champion_draft::heart_of_ruin, champion_draft::marcella, champion_draft::tavi, champion_draft::willow, champion_draft::varg};
    constexpr std::array kVarg{champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::ryth, champion_draft::varg, champion_draft::marcella, champion_draft::tavi, champion_draft::willow, champion_draft::heart_of_ruin};
    const auto big = kHeartDraft ? std::span<const champion_draft>(kHeart) : kVargDraft ? std::span<const champion_draft>(kVarg) : std::span<const champion_draft>(kPriority);
    for (auto pick : small_team ? std::span<const champion_draft>(kSmall) : big) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
}
