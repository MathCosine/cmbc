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

namespace {
using cambc::champion_draft;

constexpr std::array kPriority{
    champion_draft::vendetta, champion_draft::vesper_vale, champion_draft::ryth, champion_draft::tavi, champion_draft::marcella, champion_draft::willow, champion_draft::varg, champion_draft::heart_of_ruin, champion_draft::ren, champion_draft::stellar, champion_draft::prism, champion_draft::cassi_and_issac,
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
    for (auto pick : kPriority) {
        if (std::find(chosen.begin(), chosen.end(), pick) != chosen.end()) continue;
        cambc::draft::pick(pick);
        return;
    }
}
