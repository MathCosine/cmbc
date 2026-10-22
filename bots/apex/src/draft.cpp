// Draft: picks are simultaneous and the opponent cannot take ours, so a fixed
// priority list is enough. Basic attacks dominate camp damage, so the list
// favours Strength and sustain:
//   Vendetta     70 Strength melee, kills (including camps) reset her cooldowns
//   Vesper Vale  55 Strength ranged, +33% gold
//   Varg         splash hits the adjacent camps of a group, self-shield
//   Willow       55 Strength ranged and a 3x Focus heal for the tank
//   Marcella     1200 health, stun immune, gold for every gem she tributes
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
    champion_draft::vendetta,      champion_draft::vesper_vale, champion_draft::varg,
    champion_draft::willow,        champion_draft::marcella,    champion_draft::tavi,
    champion_draft::ryth,          champion_draft::heart_of_ruin, champion_draft::ren,
    champion_draft::stellar,       champion_draft::prism,       champion_draft::cassi_and_issac,
};
} // namespace

void play_draft() {
    static bool wrote_memory = false;
    if (!wrote_memory) {
        cambc::draft_memory = std::string_view("apex1");
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
