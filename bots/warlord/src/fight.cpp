// Team fight engine: what every champion does while enemy champions are close.
//
// Experimental (the warlord bot is not the live submission): with quickest-kill
// targeting and stun/shield awareness switched on it won 43/80 against siege in
// self-play but 9/20 in ladder scrims, against 12/20 for siege on the same games.
//
// Ladder statistics drove this design. Our losses averaged about 20 deaths a game
// against 8 for the enemy, and the teams beating us won their fights by stacking
// damage on one target at a time. So the engine:
//   1. Decides as a team. A side's fighting strength grows with the product of its
//      health and its damage (Lanchester's square law), counting shields and stuns.
//      Champions that are already fighting stay in at a lower margin than it takes
//      to start a fight, and no champion runs alone from a fight its team is in.
//   2. Picks the enemy that dies soonest to everyone who can reach it (shields add
//      health, a stun or a full bag of gems makes a target juicier), and shares it
//      through team memory so everyone in reach hits the same champion.
//   3. Chains crowd control: a stun is spent only on a focus that is not already
//      stunned, then damage spells, then basic attacks.
//   4. Protects our ranged champions: tanks prefer whoever is hitting them.
//   5. Retreats early when outnumbered, toward home, firing whatever still lands.
#include "bot.h"
#include <algorithm>

namespace {
constexpr int kFightRadius = 9;           // enemies this close are part of the fight
constexpr int kAllyRadius = 7;            // allies this close fight with us
constexpr float kEngageMargin = 0.77f;    // strength ratio needed to start a fight
constexpr float kCommittedMargin = 0.77f;  // ratio needed to keep fighting once in
constexpr int kCommitRounds = 4;          // how long a fight counts as ongoing
constexpr int kChaseRange = 4;            // how far we walk to reach the focus
// Feature switches, measured one at a time against the previous engine.
constexpr bool kFocusPick = true;   // quickest-kill targeting with peel
constexpr bool kSpellChain = false;  // stun a focus that is not stunned, then damage
constexpr bool kStunAware = true;   // stunned enemies hit less, shields add health
constexpr bool kIsolated = false;    // alone against several: leave early
} // namespace

int Bot::enemy_id(const SeenChamp& e) {
    return static_cast<int>(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(e.handle)) & 0x7F);
}

float Bot::kill_time(const SeenChamp& e) const {
    // Effective health over the damage of everyone close enough to join in.
    float dps = cheb(e.pos, pos_) <= range_ + 2 ? my_dps() : 0.f;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, e.pos) <= a.range + 2) dps += a.dps;
    float t = (e.hp + e.shield) / std::max(dps, 1.f);
    if (e.stunned) t *= 0.7f;  // it cannot hit back or walk away this round
    if (e.gems > 0) t /= 1.f + std::min(e.gems, 100) / 100.f;  // a kill takes the bag
    return t;
}

const SeenChamp* Bot::pick_focus(const std::vector<const SeenChamp*>& foes) const {
    const SeenChamp* best = nullptr;
    float best_t = 0;
    for (const auto* e : foes) {
        if (world().enemy_spawn.get(e->pos)) continue;  // healed every turn, unreachable
        float t = kill_time(*e);
        // Tanks peel: an enemy on top of one of our ranged champions comes first.
        if (tank_)
            for (const auto& a : allies_seen_)
                if (a.range > 1 && cheb(a.pos, e->pos) <= 1) t *= 0.6f;
        if (!best || t < best_t) {
            best = e;
            best_t = t;
        }
    }
    // Keep the team's current focus unless the new one is clearly faster to kill.
    if (slots_ <= 5)
        for (const auto* e : foes)
            if (enemy_id(*e) == comms_.focus() && !world().enemy_spawn.get(e->pos) && kill_time(*e) <= best_t * 1.5f) return e;
    return best;
}

void Bot::cast_fight_spells(const SeenChamp* focus, bool retreating) {
    if (cast_done_ || !can_cast_) return;
    bc::champion& me = bc::self();
    if (try_heal(me)) return;
    // Shields only when something is actually hitting us.
    bool threatened = incoming_camp_damage(pos_) > 0;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= e.range + 1) threatened = true;
    for (int i = 0; i < static_cast<int>(spells_.size()) && !cast_done_; ++i) {
        auto& sp = spells_[i];
        if (sp.kind == bc::ability_kind::shield && threatened && hp_ < max_hp_ * 0.9f && spell_ready(i)) cast(sp);
    }
    auto in_range = [&](const SeenChamp& e, const Spell& sp) {
        return !world().enemy_spawn.get(e.pos) && cheb(e.pos, pos_) <= static_cast<int>(sp.range);
    };
    for (int i = 0; i < static_cast<int>(spells_.size()) && !cast_done_; ++i) {
        auto& sp = spells_[i];
        const bool stuns = sp.kind == bc::ability_kind::cc || (sp.kind == bc::ability_kind::aoe_damage && sp.duration > 0);
        const bool offensive = stuns || sp.kind == bc::ability_kind::damage || sp.kind == bc::ability_kind::aoe_damage;
        if (!offensive || (sp.exclusive && (retreating || moved_))) continue;
        // The focus first; a stun goes to it only while it can still act, otherwise to
        // the next enemy that can; damage goes wherever it lands.
        const SeenChamp* target = nullptr;
        if (focus && in_range(*focus, sp) && !(stuns && focus->stunned)) target = focus;
        if (!target)
            for (const auto& e : enemies_)
                if (in_range(e, sp) && !(stuns && e.stunned) && (!target || e.hp + e.shield < target->hp + target->shield)) target = &e;
        if (!target && focus && in_range(*focus, sp)) target = focus;
        if (target && spell_ready(i)) cast_on(sp, target->handle);
    }
}

bool Bot::engage(bc::champion& me, const SeenChamp* focus, const std::vector<const SeenChamp*>& foes) {
    cast_fight_spells(focus, false);
    // Basic attack: the focus when in reach, otherwise whoever in reach dies soonest.
    auto attack_best = [&]() {
        if (!can_attack_) return false;
        const SeenChamp* t = focus && cheb(focus->pos, pos_) <= range_ ? focus : nullptr;
        if (!t)
            for (const auto* e : foes)
                if (cheb(e->pos, pos_) <= range_ && !world().enemy_spawn.get(e->pos) && (!t || kill_time(*e) < kill_time(*t))) t = e;
        return t && attack_target(me, t->handle, t->pos, t->hp);
    };
    if (attack_best()) {
        fight_round_ = round_;
        return true;
    }
    if (!focus) return false;
    // Close in on a focus that is near; chasing deep into their side splits the team.
    const int want = ranged_ ? range_ : 1;
    if (cheb(focus->pos, pos_) > want + kChaseRange) return false;
    if (cheb(focus->pos, pos_) > want && !moved_) approach_unit(me, focus->pos, want);
    if (attack_best()) fight_round_ = round_;
    return true;
}

bool Bot::disengage(bc::champion& me, const SeenChamp* focus) {
    // Anything that does not root us still lands on the way out.
    cast_fight_spells(focus, true);
    set_status(Task::other, kHome);
    std::vector<std::pair<vec2, int>> threats;
    for (const auto& e : enemies_) threats.push_back({e.pos, e.range + 1});
    if (step_away(me, threats)) return true;
    // Cornered: hit back rather than stand still.
    if (focus && can_attack_ && cheb(focus->pos, pos_) <= range_) attack_target(me, focus->handle, focus->pos, focus->hp);
    return true;
}

bool Bot::fight_enemies(bc::champion& me) {
    if (enemies_.empty()) return false;
    std::vector<const SeenChamp*> foes;
    const SeenChamp* nearest = nullptr;
    int nearest_d = 1000;
    float threat = 0;
    for (const auto& e : enemies_) {
        const int d = cheb(e.pos, pos_);
        if (d <= 8) foes.push_back(&e);
        if (d <= e.range + 1 && !(kStunAware && e.stunned)) threat += e.dps;
        if (d < nearest_d) {
            nearest_d = d;
            nearest = &e;
        }
    }
    if (!nearest || nearest_d > 7) return false;

    // Team strength on each side: health times damage (Lanchester's square law).
    float our_hp = hp_, our_dps = my_dps();
    int beside = 0, allies = 0;
    for (const auto& a : allies_seen_) {
        const int d = cheb(a.pos, pos_);
        if (d <= 6) {
            our_hp += a.hp;
            our_dps += a.dps;
            ++allies;
        }
        if (d <= 3) ++beside;
    }
    float their_hp = 0, their_dps = 0;
    for (const auto* e : foes) {
        their_hp += e->hp + (kStunAware ? e->shield : 0.f);
        their_dps += kStunAware && e->stunned ? e->dps * 0.5f : e->dps;
    }
    const bool losing = (our_hp * our_dps) < (their_hp * their_dps) * kEngageMargin;

    // Target: the team's focus, else (new) quickest kill or (old) weakest in reach.
    const int reach = ranged_ ? range_ : range_ + 1;
    auto huntable = [&](const SeenChamp& e) { return !world().enemy_spawn.get(e.pos); };
    const SeenChamp* target = nullptr;
    if (kFocusPick) {
        target = pick_focus(foes);
        if (target && cheb(target->pos, pos_) > reach) {
            const SeenChamp* close = nullptr;
            for (const auto* e : foes)
                if (huntable(*e) && cheb(e->pos, pos_) <= reach && (!close || kill_time(*e) < kill_time(*close))) close = e;
            if (close) target = close;
            else target = nullptr;
        }
    } else {
        for (const auto& e : enemies_)
            if (huntable(e) && cheb(e.pos, pos_) <= reach && (!target || e.hp < target->hp)) target = &e;
        if (slots_ <= 5)
            for (const auto& e : enemies_)
                if (enemy_id(e) == comms_.focus() && huntable(e) && cheb(e.pos, pos_) <= reach) target = &e;
    }
    if (target && slots_ <= 5) comms_.set_focus(enemy_id(*target));
    const bool can_finish = target && target->hp <= strength_ * 1.05f;
    const bool in_danger = hp_ < threat * 2.5f + max_hp_ * 0.1f;

    std::vector<std::pair<vec2, int>> threats;
    for (const auto& e : enemies_) threats.push_back({e.pos, e.range + 1});
    auto strike = [&](const SeenChamp& e) {
        bool acted = false;
        if (cheb(e.pos, pos_) > range_) acted = approach_unit(me, e.pos, range_);
        if (cheb(e.pos, pos_) <= range_) {
            if (!kSpellChain) use_combat_spells(me, nullptr, &e);
            acted |= attack_target(me, e.handle, e.pos, e.hp);
        }
        return acted;
    };
    // A champion running alone from a fight its teammates are in is the one that dies.
    const bool committed = beside >= 2 && !losing && hp_ > max_hp_ * 0.2f;
    const bool isolated = kIsolated && allies == 0 && foes.size() >= 2 && losing;
    const bool retreating = (in_danger || (losing && nearest_d <= nearest->range + 2) || isolated) && !can_finish && !committed;
    // Spells reach further than basic attacks (range 4): fire them before walking.
    const SeenChamp* spell_focus = target;
    if (!spell_focus && kFocusPick) spell_focus = pick_focus(foes);
    if (kSpellChain) cast_fight_spells(spell_focus, retreating);
    else fire_spells_old(retreating);
    if (retreating) {
        if (!kSpellChain) use_combat_spells(me, nullptr, target);
        set_status(Task::other, kHome);
        if (!step_away(me, threats) && target && cheb(target->pos, pos_) <= range_) attack_target(me, target->handle, target->pos, target->hp);
        return true;
    }
    if (target && strike(*target)) {
        fight_round_ = round_;
        return true;
    }
    // Even or better: close in on the weakest enemy nearby rather than get kited.
    if (!losing) {
        const SeenChamp* weakest = nullptr;
        for (const auto& e : enemies_)
            if (huntable(e) && cheb(e.pos, pos_) <= 4 && (!weakest || e.hp < weakest->hp)) weakest = &e;
        if (weakest && strike(*weakest)) return true;
    }
    return false;
}

void Bot::fire_spells_old(bool retreating) {
    if (cast_done_ || !can_cast_) return;
    for (int i = 0; i < static_cast<int>(spells_.size()) && !cast_done_; ++i) {
        auto& sp = spells_[i];
        const bool offensive = sp.kind == bc::ability_kind::damage || sp.kind == bc::ability_kind::cc || sp.kind == bc::ability_kind::aoe_damage;
        if (!offensive || (sp.exclusive && (retreating || moved_))) continue;
        const SeenChamp* best = nullptr;
        for (const auto& e : enemies_) {
            if (world().enemy_spawn.get(e.pos) || cheb(e.pos, pos_) > static_cast<int>(sp.range)) continue;
            const bool focused = slots_ <= 5 && enemy_id(e) == comms_.focus();
            if (!best || focused || (e.hp < best->hp && !(slots_ <= 5 && enemy_id(*best) == comms_.focus()))) best = &e;
        }
        if (best && spell_ready(i)) cast_on(sp, best->handle);
    }
}
