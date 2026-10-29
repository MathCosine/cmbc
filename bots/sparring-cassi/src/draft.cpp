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
    champion_draft::vesper_vale, champion_draft::vendetta, champion_draft::heart_of_ruin, champion_draft::cassi_and_issac, champion_draft::marcella, champion_draft::ryth, champion_draft::tavi, champion_draft::willow, champion_draft::varg, champion_draft::heart_of_ruin, champion_draft::ren, champion_draft::stellar, champion_draft::prism, champion_draft::cassi_and_issac,
};
} // namespace

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
    constexpr std::array kSmall{champion_draft::vesper_vale, champion_draft::vendetta, champion_draft::heart_of_ruin, champion_draft::cassi_and_issac, champion_draft::marcella, champion_draft::ryth, champion_draft::tavi, champion_draft::vendetta, champion_draft::willow, champion_draft::varg, champion_draft::heart_of_ruin};
    const bool small_team = cambc::draft::team_size() <= 4;
    for (auto pick : small_team ? std::span<const champion_draft>(kSmall) : std::span<const champion_draft>(kPriority)) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
}
