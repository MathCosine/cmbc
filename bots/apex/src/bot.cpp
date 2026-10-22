// Champion decisions. main.cpp dispatches here after the draft.
//
// Turn priorities: enemy champions, then awake camps (rotating out before a
// hit could kill), then going home, tributing, capturing and farming.
// Farming is a squad plan shared through team memory: everyone converges on
// one dangerous group and only engages once the party nearby can clear it,
// while gem and gold clusters (no damage) are claimed by single champions.
#include "bot.h"
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace {
// Debug builds record fuel at each phase change to find expensive turns.
struct FuelMark {
    const char* phase;
    std::uint64_t fuel;
};
std::vector<FuelMark> g_marks;
void mark(const char* phase) {
    g_phase = phase;
    debug_block({ g_marks.push_back({phase, bc::fuel_remaining()}); });
}
} // namespace

namespace {
constexpr float kGoldWeight = 0.15f;
constexpr int kCaptureRounds = 5;
constexpr int kReplanRounds = 8;
constexpr std::uint64_t kPlanFuel = 45000;  // fuel needed before optional planning work

bool tank_type(bc::champion_type t) { return t == bc::champion_type::ryth || t == bc::champion_type::marcella; }

struct BuildStep {
    bc::item_type item;
    int count;
};

// Five shards of one kind combine into its emblem; the last step repeats forever.
const std::vector<BuildStep>& build_for(bc::champion_type t) {
    using it = bc::item_type;
    static const std::vector<BuildStep> tank{{it::jungle_shard_of_health, 5},
                                             {it::monster_shard_of_strength, 5},
                                             {it::durability_shard_of_health, 5},
                                             {it::power_shard_of_strength, 1 << 30}};
    static const std::vector<BuildStep> dps{
        {it::monster_shard_of_strength, 5}, {it::durability_shard_of_health, 5}, {it::power_shard_of_strength, 1 << 30}};
    return tank_type(t) ? tank : dps;
}

struct DpsModel {
    float strength = 1;
    float focus = 0;
    int range = 1;
};

// Damage per round from Strength and Focus, with spells averaged over their cooldown.
const DpsModel& model_for(bc::champion_type type) {
    static std::array<std::optional<DpsModel>, 13> cache;
    auto& slot = cache[static_cast<int>(type)];
    if (slot) return *slot;
    bc::champion design{type};
    DpsModel m;
    m.range = design.base_stats().base_attack_range;
    for (const auto& ab : design.abilities()) {
        const auto& info = ab.info();
        if (!info) continue;
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            const float cd = std::max(1, p.max_cooldown);
            if constexpr (std::is_same_v<T, bc::ability_attack_reset>) m.strength += 1.f / cd;
            else if constexpr (std::is_same_v<T, bc::ability_damage>) m.focus += p.modifier / cd;
            else if constexpr (std::is_same_v<T, bc::ability_cc>) m.focus += p.modifier / cd;
            else if constexpr (std::is_same_v<T, bc::ability_aoe_damage>) m.focus += p.modifier * 1.5f / cd;
            else if constexpr (std::is_same_v<T, bc::ability_dash>) m.focus += p.modifier * 1.5f / cd;
        }, *info);
    }
    if (design.passive() == bc::passive_type::splash_attack) m.strength += 0.5f;
    slot = m;
    return *slot;
}

float dps_of(bc::champion_type type, float strength, float focus) {
    const auto& m = model_for(type);
    return strength * m.strength + focus * m.focus;
}
} // namespace

float Bot::design_dps(const bc::champion& c, float strength, float focus) const {
    return dps_of(c.champion_name(), strength, focus);
}

// ---------------------------------------------------------------------------
// Setup and observation

void Bot::init(bc::champion& me) {
    auto& W = world();
    mark("init-world");
    if (!W.ready) W.init();
    mark("init-allies");
    type_ = me.champion_name();
    const auto stats = me.base_stats();
    range_ = stats.base_attack_range;
    ranged_ = range_ > 1;
    tank_ = tank_type(type_);
    allies_ = bc::champions(team::ally);
    slots_ = std::min<int>(static_cast<int>(allies_.size()), kMaxSlots);
    for (int i = 0; i < static_cast<int>(allies_.size()); ++i) {
        if (allies_[i] == me) slot_ = i;
        const auto t = allies_[i].champion_name();
        bc::champion design{t};
        const auto s = design.base_stats();
        ally_dps_.push_back(dps_of(t, s.base_strength, s.base_focus));
        ally_hp_.push_back(s.base_health);
    }
    if (slot_ >= kMaxSlots) slot_ = -1;
    for (const auto& ab : me.abilities()) {
        const auto& info = ab.info();
        if (!info) continue;
        Spell spell{ab, bc::ability_kind::damage};
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            spell.kind = T::kind;
            spell.exclusive = p.exclusive_with_movement;
            if constexpr (requires { p.range; }) spell.range = p.range;
            if constexpr (requires { p.modifier; }) spell.modifier = p.modifier;
            if constexpr (requires { p.radius; }) spell.radius = p.radius;
            if constexpr (requires { p.duration; }) spell.duration = p.duration;
        }, *info);
        spells_.push_back(spell);
    }
    my_design_dps_ = dps_of(type_, stats.base_strength, stats.base_focus);
    // The first turn has double fuel: precompute the trip home and the nearest targets.
    mark("init-precompute");
    W.precompute(110000);
    ready_ = true;
}

void Bot::observe(bc::champion& me) {
    auto& W = world();
    visible_.load(bc::vision::visible_tiles(), W.w, W.h);
    occupancy_loaded_ = false;

    // Camp knowledge comes from native bitboards; only nearby camps get per-camp queries,
    // since every host call and every loop iteration costs fuel.
    mark("obs-camps");
    const auto alive_board = bc::vision::camp_tiles();
    alive_vis_.load(alive_board, W.w, W.h);
    awake_vis_.load(bc::vision::awake_camp_tiles(), W.w, W.h);
    camps_.clear();
    bc::bitboard near;
    near.clear();
    near.draw_chebyshev(pos_, 6);
    near &= alive_board;
    for (const vec2 p : near.positions()) {
        auto* info = W.camp_info_at(p);
        if (!info) continue;
        const auto c = bc::vision::camp_at(p);
        const float hp = info->strength > 0 || cheb(p, pos_) <= range_ + 1 ? c.health() : info->hp_known;
        if (hp <= 0) continue;
        info->hp_known = hp;
        camps_.push_back({c, p, hp, info->strength, info->range, info->strength > 0 && awake_vis_.get(p), W.group_id_at(p)});
    }

    mark("obs-champs");
    // Strength and Focus only change when items are bought: refresh them now and then.
    auto seen = [&](const bc::champion& c) {
        const auto t = c.champion_name();
        SeenChamp s{c, c.position(), c.health(), c.max_health(), 0, model_for(t).range, 0};
        auto& cached = dps_cache_[static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)) & 31];
        if (cached.round < 0 || round_ - cached.round > 25) cached = {dps_of(t, c.strength(), c.focus()), round_};
        s.dps = cached.dps;
        return s;
    };
    enemies_.clear();
    for (const auto& c : bc::vision::champions(team::enemy)) enemies_.push_back(seen(c));
    allies_seen_.clear();
    for (const auto& c : bc::vision::champions(team::ally))
        if (!(c == me)) allies_seen_.push_back(seen(c));
    mark("obs-groups");
    update_groups();
}

void Bot::update_groups() {
    auto& W = world();
    for (auto& g : W.groups) {
        if (g.state == GroupState::respawning && round_ >= g.respawn_round) g.respawned();
        // Vision reaches 8 tiles; skip groups that cannot be in view.
        if (cheb(g.anchor, pos_) > g.extent + 9) continue;
        // One pass over the group's tiles: which are in view, which still hold a camp.
        bool any_visible = false, awake = false;
        int alive = 0;
        float hp_left = 0;
        for (auto& info : g.camps) {
            if (visible_.get(info.pos)) {
                any_visible = true;
                info.alive_known = alive_vis_.get(info.pos);
                if (info.alive_known && info.strength > 0) awake |= awake_vis_.get(info.pos);
            }
            if (info.alive_known) {
                ++alive;
                hp_left += info.hp_known;
            }
        }
        if (!any_visible) continue;
        g.seen_round = round_;
        bool visible_alive = false;
        for (const auto& info : g.camps)
            if (info.alive_known && visible_.get(info.pos)) {
                visible_alive = true;
                break;
            }
        if (visible_alive) {
            g.state = GroupState::alive;
            g.awake = awake;
            g.hp_left = hp_left;
            continue;
        }
        g.awake = false;
        // Asking the engine is costly for big groups; only do it when the answer can have changed.
        const bool ask = g.state == GroupState::alive || (g.state == GroupState::reward && round_ % 2 == 0);
        if (!ask) continue;
        try {
            const auto state = g.handle->state();
            if (std::holds_alternative<bc::map::camp_group_reward_available>(state)) {
                g.state = GroupState::reward;
            } else if (const auto* r = std::get_if<bc::map::camp_group_respawning>(&state)) {
                g.state = GroupState::respawning;
                g.respawn_round = round_ + r->timer;
            }
        } catch (const std::exception&) {
        }
    }
}

void Bot::load_occupancy() {
    if (occupancy_loaded_) return;
    occupied_board_ = bc::vision::champion_tiles() | bc::vision::camp_tiles() | bc::vision::teleport_tiles();
    occupied_.load(occupied_board_, world().w, world().h);
    occupancy_loaded_ = true;
}

bool Bot::free_tile(vec2 p) const {
    const auto& W = world();
    return W.in_bounds(p) && !W.blocked.get(p) && !occupied_.get(p);
}

void Bot::set_status(Task task, int target) { comms_.set_slot(slot_, task, target); }

// ---------------------------------------------------------------------------
// Turn entry


void Bot::play_turn() {
    auto& me = bc::self();
    debug_block({ g_marks.clear(); g_marks.push_back({"start", bc::fuel_remaining()}); });
    if (!ready_) init(me);
    round_ = static_cast<int>(bc::round_number());
    comms_.read();
    moved_ = false;
    cast_done_ = false;
    ready_cache_ = {-1, -1};

    const auto state = me.state();
    if (!state.contains(bc::champion_state::alive)) {
        mark("dead");
        // Dead champions still get a turn to shop.
        gold_ = static_cast<int>(me.gold_count());
        shop(me, true);
        set_status(Task::other, kDead);
        task_ = Task::farm;
        target_ = -1;
        return;
    }

    // A respawned champion still shows as dead to the team until it says otherwise.
    if (slot_ >= 0) {
        const auto mine = comms_.slot(slot_);
        if (mine.task == Task::other && mine.target == kDead) set_status(Task::farm, -1);
    }
    pos_ = me.position();
    hp_ = me.health();
    max_hp_ = me.max_health();
    strength_ = me.strength();
    focus_ = me.focus();
    gems_ = static_cast<int>(me.gem_count());
    gold_ = static_cast<int>(me.gold_count());
    can_move_ = state.contains(bc::champion_state::can_move);
    can_attack_ = state.contains(bc::champion_state::can_attack);
    can_cast_ = state.contains(bc::champion_state::can_cast);
    // Teammates out of sight are estimated from their base stats times the team's item multiplier.
    comms_.blend_power(dps_of(type_, strength_, focus_) / std::max(my_design_dps_, 1.f));
    mark("observe");
    observe(me);
    damage_taken_ = last_hp_round_ == round_ - 1 ? std::max(0.f, last_hp_ - hp_) : 0.f;
    last_hp_ = hp_;
    last_hp_round_ = round_;

    if (state.contains(bc::champion_state::recalling)) {
        set_status(Task::other, kHome);
        mark("recalling");
    } else if (state.contains(bc::champion_state::submitting_gems)) {
        set_status(Task::other, kTribute);
        mark("submitting");
    } else if (!state.contains(bc::champion_state::teleporting)) {
        act(me);
    }
    debug_block({
        const auto start = g_marks.empty() ? 0 : g_marks.front().fuel;
        const auto now = bc::fuel_remaining();
        if (start > now && start - now > 60000) {
            std::cout << "FUEL r" << round_ << " " << bc::to_string(type_) << " used" << (start - now);
            for (std::size_t i = 1; i < g_marks.size(); ++i) std::cout << " " << g_marks[i - 1].phase << "=" << (g_marks[i - 1].fuel - g_marks[i].fuel);
            if (!g_marks.empty()) std::cout << " " << g_marks.back().phase << "=" << (g_marks.back().fuel - now);
            std::cout << "\n";
        }
    });
    // Debug builds tally where each champion spends its rounds.
    debug_block({
        static std::map<std::string, int> activity;
        ++activity[g_phase];
        if (std::string(g_phase) == "farm-travel" && !moved_) ++activity["travel-still"];
        if (round_ % 500 == 0) {
            std::cout << "P r" << round_ << " " << bc::to_string(type_);
            for (const auto& [k, v] : activity) std::cout << " " << k << "=" << v;
            std::cout << "\n";
        }
    });

    if (round_ % 20 == 0)
        LOG("S r" << round_ << " " << bc::to_string(type_) << " slot" << slot_ << " task" << static_cast<int>(task_) << " t" << target_
                  << " squad" << comms_.squad_target() << " pos" << bc::to_string(pos_) << " hp" << hp_ << "/" << max_hp_
                  << " gems" << gems_ << " gold" << gold_ << " str" << strength_ << " fuel" << bc::fuel_remaining()
                  << " camps" << camps_.size() << " en" << enemies_.size());
    if (stuck_rounds_ == 15) debug_block({
        std::cout << "STUCK r" << round_ << " " << bc::to_string(type_) << bc::to_string(pos_) << " phase " << g_phase << " task" << int(task_) << " t" << target_ << " gems" << gems_;
        if (target_ >= 0 && task_ != Task::other) {
            const auto& g = world().groups[target_];
            int vis = 0;
            for (const auto& c : camps_)
                if (c.group == target_) ++vis;
            std::cout << " anchor" << bc::to_string(g.anchor) << " state" << int(g.state) << " vis" << vis << " wait" << wait_rounds_;
        }
        std::cout << "\n";
    });
    stuck_rounds_ = pos_ == last_pos_ ? stuck_rounds_ + 1 : 0;
    last_pos_ = pos_;
    // Spend leftover fuel on distance fields we will need later.
    mark("precompute");
    if (bc::fuel_remaining() > kPlanFuel) world().precompute(kPlanFuel - 10000);
}

void Bot::act(bc::champion& me) {
    const bool at_spawn = on_spawn();
    mark("shop");
    if (at_spawn || type_ == bc::champion_type::tavi) shop(me, at_spawn);
    mark("locks");
    place_lock(me);

    mark("enemies");
    if (fight_enemies(me)) return;
    mark("flee");
    if (flee_camps(me)) return;
    mark("retreat");
    if (retreat_from_camps(me)) return;
    mark("camps");
    if (fight_camps(me)) return;
    // Spawn heals fully at turn end; stay a turn when hurt.
    if (at_spawn && hp_ < max_hp_ * 0.95f && enemies_.empty()) {
        set_status(Task::other, kHome);
        mark("heal");
        return;
    }
    mark("home");
    if (go_home(me, false)) return;
    mark("tribute");
    if (tribute(me)) return;
    mark("capture");
    if (capture(me)) return;
    mark("farm");
    if (farm(me)) return;
    mark("idle");
    // Nothing to farm: bank whatever we carry.
    if (gems_ > 0) {
        int d = 0;
        const int a = best_altar(d);
        if (a >= 0 && d != kUnreached) {
            set_status(Task::other, kTribute);
            if (cheb(pos_, world().altars[a]) <= bc::constants::submit_gems_range) {
                try {
                    me.submit_gems(world().altars[a]);
                } catch (const std::exception&) {
                }
            } else {
                travel(me, world().altar_poi[a]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Enemy champions

bool Bot::fight_enemies(bc::champion& me) {
    if (enemies_.empty()) return false;
    const float my_dps = dps_of(type_, strength_, focus_);
    float ally_dps = my_dps, ally_hp = hp_;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 6) {
            ally_dps += a.dps;
            ally_hp += a.hp;
        }
    float enemy_dps = 0, enemy_hp = 0, threat = 0;
    const SeenChamp* nearest = nullptr;
    int nearest_d = 1000;
    for (const auto& e : enemies_) {
        const int d = cheb(e.pos, pos_);
        if (d <= 8) {
            enemy_dps += e.dps;
            enemy_hp += e.hp;
        }
        if (d <= e.range + 1) threat += e.dps;
        if (d < nearest_d) {
            nearest_d = d;
            nearest = &e;
        }
    }
    if (!nearest || nearest_d > 7) return false;

    // Rounds for each side to wipe the other if everyone nearby commits.
    const float we_need = enemy_hp / std::max(ally_dps, 1.f);
    const float they_need = ally_hp / std::max(enemy_dps, 1.f);
    const bool losing = we_need > they_need * 1.3f;

    // Target: the enemy we can reach this turn that dies soonest; melee may step first.
    const int reach = ranged_ ? range_ : range_ + 1;
    // Enemies on their own spawn heal to full every round and cannot be reached.
    auto huntable = [&](const SeenChamp& e) { return !world().enemy_spawn.get(e.pos); };
    const SeenChamp* target = nullptr;
    for (const auto& e : enemies_)
        if (huntable(e) && cheb(e.pos, pos_) <= reach && (!target || e.hp < target->hp)) target = &e;
    const bool can_finish = target && target->hp <= strength_ * 1.05f;
    const bool in_danger = hp_ < threat * 2.5f + max_hp_ * 0.1f;

    std::vector<std::pair<vec2, int>> threats;
    for (const auto& e : enemies_) threats.push_back({e.pos, e.range + 1});
    auto strike = [&](const SeenChamp& e) {
        bool acted = false;
        if (cheb(e.pos, pos_) > range_) acted = approach_unit(me, e.pos, range_);
        if (cheb(e.pos, pos_) <= range_) {
            use_combat_spells(me, nullptr, &e);
            acted |= attack_target(me, e.handle, e.pos, e.hp);
        }
        return acted;
    };

    if ((in_danger || (losing && nearest_d <= nearest->range + 2)) && !can_finish) {
        // Shoot first if we cannot get away anyway, then run toward home.
        use_combat_spells(me, nullptr, target);
        set_status(Task::other, kHome);
        if (!step_away(me, threats) && target && cheb(target->pos, pos_) <= range_) attack_target(me, target->handle, target->pos, target->hp);
        return true;
    }
    if (target && strike(*target)) return true;
    // Even or better: close in on the weakest enemy nearby rather than get kited.
    if (!losing) {
        const SeenChamp* weakest = nullptr;
        for (const auto& e : enemies_)
            if (huntable(e) && cheb(e.pos, pos_) <= 4 && (!weakest || e.hp < weakest->hp)) weakest = &e;
        if (weakest && strike(*weakest)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Camps

bool Bot::camp_threatens(const SeenCamp& c, vec2 at) const {
    if (!c.awake || c.strength <= 0 || cheb(c.pos, at) > c.range) return false;
    // Camps hit the nearest champion; ties count against us.
    const int mine = cheb(c.pos, at);
    for (const auto& a : allies_seen_)
        if (cheb(c.pos, a.pos) < mine) return false;
    for (const auto& e : enemies_)
        if (cheb(c.pos, e.pos) < mine) return false;
    return true;
}

float Bot::incoming_camp_damage(vec2 at) const {
    float total = 0;
    for (const auto& c : camps_)
        if (camp_threatens(c, at)) total += c.strength;
    return total;
}

bool Bot::can_beat(const Group& g) const {
    if (g.harmless) return true;
    float dps = 0, hp = 0;
    local_power(dps, hp);
    return forecast(g, dps).damage <= hp * 0.8f;
}

bool Bot::hazardous(vec2 p, int except_group) const {
    const auto& W = world();
    for (const auto& g : W.groups) {
        if (g.id == except_group || g.harmless || !g.alive_by(round_) || cheb(g.anchor, p) > 12) continue;
        for (const auto& c : g.camps)
            if (c.alive_known && c.strength > 0 && cheb(c.pos, p) <= c.range) return true;
    }
    return false;
}

bool Bot::flee_camps(bc::champion& me) {
    std::vector<std::pair<vec2, int>> threats;
    // Awake camps we cannot beat with the champions here.
    for (const auto& c : camps_)
        if (c.awake && c.strength > 0 && c.group >= 0 && cheb(c.pos, pos_) <= c.range + 1 && !can_beat(world().groups[c.group]))
            threats.push_back({c.pos, c.range + 1});
    // Damage nothing visible explains: a camp behind a wall is hitting us.
    float explained = 0;
    for (const auto& c : camps_)
        if (camp_threatens(c, pos_)) explained += c.strength;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= e.range + 1) explained += e.dps * 2;
    if (damage_taken_ > explained + 25) {
        for (const auto& g : world().groups) {
            if (g.harmless || !g.alive_by(round_) || cheb(g.anchor, pos_) > 12) continue;
            for (const auto& c : g.camps)
                if (c.alive_known && c.strength > 0 && cheb(c.pos, pos_) <= c.range && !visible_.get(c.pos))
                    threats.push_back({c.pos, c.range + 1});
        }
    }
    if (threats.empty()) return false;
    use_combat_spells(me, nullptr, nullptr);
    return step_away(me, threats);
}

bool Bot::retreat_from_camps(bc::champion& me) {
    const float incoming = incoming_camp_damage(pos_);
    if (incoming <= 0) return false;
    // Melee need more steps to get out of range than ranged champions.
    const float buffer = incoming * (ranged_ ? 2.5f : 3.5f) + max_hp_ * 0.08f;
    if (hp_ > buffer) return false;
    std::vector<std::pair<vec2, int>> threats;
    for (const auto& c : camps_)
        if (c.awake && c.strength > 0) threats.push_back({c.pos, c.range});
    // A shield or heal might buy the hit instead.
    use_combat_spells(me, nullptr, nullptr);
    if (step_away(me, threats)) return true;
    return false;
}

bool Bot::engage_ready(const Group& g) {
    if (g.awake || g.harmless) return true;
    float dps = dps_of(type_, strength_, focus_), hp = hp_;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, g.anchor) <= 7) {
            dps += a.dps;
            hp += a.hp;
        }
    const auto f = forecast(g, dps);
    const float margin = wait_rounds_ > 15 ? 0.8f : 0.6f;
    return f.damage <= hp * margin;
}

bool Bot::fight_camps(bc::champion& me) {
    auto& W = world();
    // Join any awake group within reach; otherwise engage our target once the party is ready.
    int gid = -1;
    for (const auto& c : camps_)
        if (c.awake && c.group >= 0 && cheb(c.pos, pos_) <= std::max(c.range, range_) + 2 && can_beat(W.groups[c.group])) {
            gid = c.group;
            break;
        }
    const bool weak = hp_ < max_hp_ * 0.4f;
    if (gid >= 0 && weak && !W.groups[gid].harmless) return false;
    if (gid < 0 && (task_ == Task::farm || task_ == Task::solo) && target_ >= 0) {
        const auto& g = W.groups[target_];
        bool near = false;
        for (const auto& c : camps_)
            if (c.group == target_ && cheb(c.pos, pos_) <= range_ + 3) near = true;
        if (near && !(weak && !g.harmless) && engage_ready(g)) gid = target_;
    }
    if (gid < 0) return false;
    const auto& g = W.groups[gid];

    // Focus fire: strongest camp first (cuts incoming damage fastest), then the most wounded.
    auto better = [](const SeenCamp* a, const SeenCamp* b) {
        if (!b) return true;
        if (a->strength != b->strength) return a->strength > b->strength;
        return a->hp < b->hp;
    };
    const SeenCamp* focus = nullptr;
    const SeenCamp* in_range = nullptr;
    for (const auto& c : camps_) {
        if (c.group != gid) continue;
        if (better(&c, focus)) focus = &c;
        if (cheb(c.pos, pos_) <= range_ && better(&c, in_range)) in_range = &c;
    }
    if (!focus) return false;
    if (task_ != Task::capture) set_status(task_ == Task::solo ? Task::solo : Task::farm, gid);

    if (ranged_) {
        // Ranged cannot walk and attack in one turn: shoot whatever is in range.
        if (in_range) {
            use_combat_spells(me, in_range, nullptr);
            attack_camp(me, *in_range);
            return true;
        }
        // Out of range: a spell may still reach (range 4) before walking in.
        use_combat_spells(me, focus, nullptr);
        if (!moved_) approach_unit(me, focus->pos, range_);
        return true;
    }

    // Melee: step next to the focus, then hit it (or whatever is adjacent).
    if (cheb(focus->pos, pos_) > 1) approach_unit(me, focus->pos, 1);
    const SeenCamp* adjacent = nullptr;
    for (const auto& c : camps_)
        if (c.group == gid && cheb(c.pos, pos_) <= 1 && better(&c, adjacent)) adjacent = &c;
    if (focus && cheb(focus->pos, pos_) <= 1) adjacent = focus;
    if (adjacent) {
        use_combat_spells(me, adjacent, nullptr);
        attack_camp(me, *adjacent);
    } else {
        use_combat_spells(me, focus, nullptr);
    }
    (void)g;
    return true;
}

bool Bot::attack_camp(bc::champion& me, const SeenCamp& camp) { return attack_target(me, camp.handle, camp.pos, camp.hp); }

bool Bot::attack_target(bc::champion& me, const bc::unit& target, vec2 target_pos, float target_hp) {
    if (!can_attack_ || cheb(pos_, target_pos) > range_) return false;
    try {
        me.attack(target);
    } catch (const std::exception&) {
        return false;
    }
    can_attack_ = false;
    if (ranged_) can_move_ = false;
    // An attack reset buys a second hit this turn.
    if (cast_done_ || !can_cast_) return true;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::attack_reset || !spell_ready(i)) continue;
        // A dead target leaves vision; querying it would throw.
        if (target_hp <= strength_) break;
        if (cast(s)) {
            can_attack_ = true;
            try {
                me.attack(target);
                can_attack_ = false;
            } catch (const std::exception&) {
            }
        }
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Spells

bool Bot::spell_ready(int index) {
    if (index < 0 || index >= static_cast<int>(spells_.size())) return false;
    if (ready_cache_[index] < 0) ready_cache_[index] = spells_[index].handle.is_ready() ? 1 : 0;
    return ready_cache_[index] == 1;
}

bool Bot::cast(Spell& spell) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use();
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::cast_on(Spell& spell, const bc::camp& target) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use(target);
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::cast_on(Spell& spell, const bc::champion& target) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use(target);
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::cast_at(Spell& spell, vec2 tile) {
    if (cast_done_ || !can_cast_ || (spell.exclusive && moved_)) return false;
    bool ok = false;
    try {
        ok = spell.handle.use(tile);
    } catch (const std::exception&) {
    }
    if (ok) {
        cast_done_ = true;
        ready_cache_[spell.handle.slot()] = 0;
        if (spell.exclusive) can_move_ = false;
    }
    return ok;
}

bool Bot::try_heal(bc::champion& me) {
    (void)me;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::heal) continue;
        const SeenChamp* best = nullptr;
        float best_missing = focus_ * s.modifier * 0.6f;
        for (const auto& a : allies_seen_) {
            const float missing = a.max_hp - a.hp;
            if (cheb(a.pos, pos_) <= static_cast<int>(s.range) && missing > best_missing) {
                best = &a;
                best_missing = missing;
            }
        }
        if (best && spell_ready(i) && cast_on(s, best->handle)) return true;
    }
    return false;
}

void Bot::use_combat_spells(bc::champion& me, const SeenCamp* camp, const SeenChamp* enemy) {
    if (cast_done_ || !can_cast_) return;
    if (try_heal(me)) return;
    const bool threatened = incoming_camp_damage(pos_) > 0 || (enemy && cheb(enemy->pos, pos_) <= enemy->range + 1);
    for (int i = 0; i < static_cast<int>(spells_.size()) && !cast_done_; ++i) {
        auto& s = spells_[i];
        switch (s.kind) {
        case bc::ability_kind::shield:
            if (threatened && hp_ < max_hp_ * 0.9f && spell_ready(i)) cast(s);
            break;
        case bc::ability_kind::damage:
        case bc::ability_kind::cc: {
            // Stuns are wasted on harmless or sleeping camps; damage spells are not.
            const bool stun_only = s.kind == bc::ability_kind::cc && s.modifier <= 0;
            if (enemy && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) {
                if (spell_ready(i)) cast_on(s, enemy->handle);
            } else if (camp && cheb(camp->pos, pos_) <= static_cast<int>(s.range)) {
                if (s.kind == bc::ability_kind::cc && (!camp->awake || camp->strength <= 0) && stun_only) break;
                if (camp->hp < strength_ && s.kind == bc::ability_kind::damage) break;  // the basic attack finishes it
                if (spell_ready(i)) cast_on(s, camp->handle);
            }
            break;
        }
        case bc::ability_kind::aoe_damage: {
            if (enemy && cheb(enemy->pos, pos_) <= static_cast<int>(s.range)) {
                if (spell_ready(i)) cast_on(s, enemy->handle);
            } else if (camp && cheb(camp->pos, pos_) <= static_cast<int>(s.range)) {
                // Prefer a centre that also catches neighbours of the target.
                int hits = 0;
                for (const auto& c : camps_)
                    if (c.group == camp->group && cheb(c.pos, camp->pos) <= static_cast<int>(s.radius)) ++hits;
                if ((hits >= 2 || camp->hp > strength_ * 2) && spell_ready(i)) cast_on(s, camp->handle);
            }
            break;
        }
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Movement

bool Bot::try_move(bc::champion& me, vec2 delta) {
    if (!can_move_ || moved_) return false;
    const vec2 dest = pos_ + delta;
    bool ok = false;
    try {
        ok = me.move(delta);
    } catch (const std::exception&) {
    }
    if (ok) {
        pos_ = dest;
        moved_ = true;
        can_move_ = false;
        if (ranged_) can_attack_ = false;
        return true;
    }
    occupied_.set(dest);
    return false;
}

bool Bot::step_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats) {
    if (!can_move_ || moved_) return false;
    load_occupancy();
    auto& W = world();
    auto margin = [&](vec2 p) {
        int worst = 1000;
        for (const auto& [t, r] : threats) worst = std::min(worst, cheb(p, t) - r);
        return worst;
    };
    const auto* home = W.field(W.spawn_poi, 20000);
    int best = margin(pos_);
    std::uint16_t best_home = home ? W.dist_on(*home, pos_) : 0;
    vec2 best_tile = pos_;
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!free_tile(n)) continue;
        const int m = margin(n);
        const std::uint16_t h = home ? home->get(n) : 0;
        if (m > best || (m == best && h < best_home)) {
            best = m;
            best_home = h;
            best_tile = n;
        }
    }
    if (best_tile == pos_) return false;
    return try_move(me, best_tile - pos_);
}

bool Bot::approach(bc::champion& me, bc::bitboard goal) {
    if (!can_move_ || moved_) return false;
    load_occupancy();
    auto& W = world();
    bc::bitboard walls = W.blocked_board | occupied_board_;
    walls.set(pos_, false);
    goal &= ~walls;
    if (goal.is_empty() || goal.is_set(pos_)) return false;
    bc::bitboard dest;
    dest.clear();
    dest.set(pos_);
    bc::bitboard visited;
    scratch_field_.compute_to(walls, goal, dest, visited);
    if (scratch_field_.get(pos_) == kUnreached) return false;
    for (auto step : scratch_field_.next_moves_from(pos_))
        if (try_move(me, step)) return true;
    return false;
}

bool Bot::approach_unit(bc::champion& me, vec2 target, int range) {
    if (cheb(pos_, target) <= range) return false;
    bc::bitboard goal;
    goal.clear();
    goal.draw_chebyshev(target, static_cast<float>(range));
    return approach(me, goal);
}

bool Bot::try_dash_along(bc::champion& me, const bc::distance_field& field, bool walkable_roots) {
    if (cast_done_ || !can_cast_) return false;
    for (int i = 0; i < static_cast<int>(spells_.size()); ++i) {
        auto& s = spells_[i];
        if (s.kind != bc::ability_kind::dash) continue;
        const std::uint16_t here = world().dist_on(field, pos_);
        if (here == kUnreached || here < 5) return false;
        // Two greedy steps down the field.
        vec2 a = pos_, b = pos_;
        std::uint16_t da = here, db = here;
        for (auto d : kDirs) {
            const vec2 n = pos_ + d;
            if (!free_tile(n)) continue;
            const auto v = field.get(n);
            if (v < da) {
                da = v;
                a = n;
            }
        }
        if (a == pos_) return false;
        for (auto d : kDirs) {
            const vec2 n = a + d;
            if (n == pos_ || !free_tile(n)) continue;
            const auto v = field.get(n);
            if (v < db && v < da) {
                db = v;
                b = n;
            }
        }
        if (b == pos_ || (!walkable_roots && db < 1)) return false;
        // A landing that damages would wake sleeping camps.
        if (s.modifier > 0)
            for (const auto& c : camps_)
                if (cheb(c.pos, b) <= static_cast<int>(s.radius) + 1) return false;
        if (!spell_ready(i)) return false;
        if (cast_at(s, b)) {
            pos_ = b;
            return true;
        }
        return false;
    }
    (void)me;
    return false;
}

bool Bot::travel(bc::champion& me, int poi) {
    if (!can_move_ || moved_) return false;
    auto& W = world();
    const auto* f = W.field(poi, 15000);
    load_occupancy();
    const bool walkable = W.pois[poi].roots_walkable;
    if (!f) {
        // No fuel for a field this turn: search locally toward the goal instead.
        bc::bitboard goal = W.pois[poi].roots();
        if (!walkable) {
            bc::bitboard ring;
            ring.clear();
            for (auto p : goal.positions()) ring.draw_chebyshev(p, 1);
            goal = ring;
        }
        return approach(me, goal);
    }
    std::uint16_t here = W.dist_on(*f, pos_);
    if (here == kUnreached && W.group_id_at(pos_) >= 0) {
        // Boxed in by camp tiles the static field treats as walls: search the live map.
        bc::bitboard goal = W.pois[poi].roots();
        if (!walkable) {
            bc::bitboard ring;
            ring.clear();
            for (auto p : goal.positions()) ring.draw_chebyshev(p, 1);
            goal = ring;
        }
        return approach(me, goal);
    }
    if (here == kUnreached || here == 0 || (!walkable && here <= 1)) return false;
    if (try_dash_along(me, *f, walkable)) here = W.dist_on(*f, pos_);
    if (here == 0 || (!walkable && here <= 1)) return true;

    std::array<std::pair<std::uint16_t, vec2>, 8> options;
    int count = 0;
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!W.in_bounds(n) || W.blocked.get(n)) continue;
        const auto v = f->get(n);
        if (v < here) options[count++] = {v, n};
    }
    std::sort(options.begin(), options.begin() + count, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (int i = 0; i < count; ++i)
        if (!occupied_.get(options[i].second) && try_move(me, options[i].second - pos_)) return true;
    // Every downhill tile is taken: route around the blockers.
    if (stuck_rounds_ >= 2 || count > 0) {
        bc::bitboard goal = W.pois[poi].roots();
        if (!walkable) {
            bc::bitboard ring;
            ring.clear();
            for (auto p : goal.positions()) ring.draw_chebyshev(p, 1);
            goal = ring;
        }
        if (bc::fuel_remaining() > 30000) return approach(me, goal);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Home, tribute and capture

void Bot::place_lock(bc::champion& me) {
    auto& W = world();
    for (int i = 0; i < static_cast<int>(W.altars.size()) && i < 32; ++i) {
        const vec2 a = W.altars[i];
        if (cheb(a, pos_) > 8 || !visible_.get(a)) continue;
        // Remember enemy locks so tributes go elsewhere.
        bool locked = bc::vision::has_altar_lock(a);
        bool ours = false;
        if (locked) {
            ours = bc::vision::altar_lock_at(a).is_ally();
            if (!ours) enemy_lock_round_[i] = round_;
        } else {
            enemy_lock_round_[i] = -1000;
        }
        if (!has_lock_ || locked || cheb(a, pos_) > 2) continue;
        // Lock altars the enemy will use: anything when there are only one or two,
        // otherwise those on their side, and always one an enemy is tributing at.
        bool channeling = false;
        for (const auto& e : enemies_)
            if (cheb(e.pos, a) <= 1 && e.handle.is_submitting_gems()) channeling = true;
        const bool their_side = cheb(a, W.enemy_centre) <= cheb(a, W.ally_centre);
        if (!channeling && !their_side && W.altars.size() > 2) continue;
        bool placed = false;
        try {
            placed = me.consumables()[bc::consumable_type::altar_lock].use(a);
        } catch (const std::exception&) {
        }
        if (placed) {
            has_lock_ = false;
            enemy_lock_round_[i] = -1000;
        }
    }
}

void Bot::drop_ward(bc::champion& me) {
    // One free ward that recharges over 10 turn ends; a new one replaces the old.
    if (round_ < ward_ready_round_) return;
    load_occupancy();
    for (auto d : kDirs) {
        const vec2 n = pos_ + d;
        if (!free_tile(n)) continue;
        bool placed = false;
        try {
            placed = me.place_ward(n);
        } catch (const std::exception&) {
        }
        if (placed) ward_ready_round_ = round_ + bc::constants::ward_cooldown + 1;
        else ward_ready_round_ = round_ + 3;
        return;
    }
}

bool Bot::altar_open(int index) const {
    // Some altars are walled in by camps; they open only once those camps are dead.
    const auto& W = world();
    const vec2 a = W.altars[index];
    for (auto d : kDirs) {
        const vec2 n = a + d;
        if (!W.in_bounds(n) || W.blocked.get(n)) continue;
        const int gid = W.group_id_at(n);
        if (gid < 0) return true;
        const auto& g = W.groups[gid];
        bool camp_alive = false;
        for (const auto& c : g.camps)
            if (c.pos == n) camp_alive = g.alive_by(round_) && c.alive_known;
        if (!camp_alive) return true;
    }
    return false;
}

int Bot::best_altar(int& distance) {
    auto& W = world();
    // Fields for the two closest altars as the crow flies keep the cost bounded.
    std::vector<std::pair<int, int>> order;
    for (int i = 0; i < static_cast<int>(W.altars.size()); ++i)
        if (altar_open(i)) order.push_back({cheb(W.altars[i], pos_), i});
    std::sort(order.begin(), order.end());
    int best = -1;
    distance = kUnreached;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const int i = order[k].second;
        if (k >= 3 && !W.has_field(W.altar_poi[i])) continue;
        if (order[k].first >= distance) break;
        int d = W.dist(W.altar_poi[i], pos_, 15000);
        // Stick with the altar we were walking to unless another is clearly closer.
        if (i == altar_choice_ && d != kUnreached) d -= 3;
        // An enemy lock costs five attacks to clear: prefer another altar unless far.
        if (i < 32 && round_ - enemy_lock_round_[i] < 150 && d != kUnreached) d += 12;
        if (d < distance) {
            distance = d;
            best = i;
        }
    }
    if (best == altar_choice_ && distance != kUnreached) distance += 3;
    altar_choice_ = best;
    return best;
}

bool Bot::go_home(bc::champion& me, bool urgent) {
    if (on_spawn()) return false;
    const bool hurt = hp_ < max_hp_ * 0.4f;
    const bool rich = gold_ >= 250 || (gold_ >= 150 && hp_ < max_hp_ * 0.6f);
    if (!urgent && !hurt && !rich) return false;
    // Bank gems first when an altar is close.
    if (gems_ > 0 && hp_ > max_hp_ * 0.2f) {
        int d = 0;
        if (best_altar(d) >= 0 && d <= 8) return false;
    }
    set_status(Task::other, kHome);
    auto& W = world();
    const int home = W.dist(W.spawn_poi, pos_, 10000);
    if (home <= 6) return travel(me, W.spawn_poi);
    bool threatened = false;
    for (const auto& c : camps_)
        if (c.awake && c.strength > 0 && cheb(c.pos, pos_) <= c.range + 1) threatened = true;
    for (const auto& e : enemies_)
        if (cheb(e.pos, pos_) <= 7) threatened = true;
    if (!threatened) {
        try {
            me.recall();
            return true;
        } catch (const std::exception&) {
        }
    }
    return travel(me, W.spawn_poi);
}

bool Bot::tribute(bc::champion& me) {
    if (gems_ <= 0) return false;
    int d = 0;
    const int a = best_altar(d);
    if (a < 0 || d == kUnreached) return false;
    const int remaining = static_cast<int>(bc::constants::max_rounds) - round_;
    const int channel = (gems_ + 9) / 10;
    const bool endgame = remaining <= d + channel + 20;
    const bool wins = bc::global_submitted_gems(team::ally) + gems_ >= bc::constants::winning_gems;
    const bool threatened = !enemies_.empty();
    const bool want = gems_ >= 20 || endgame || wins || d <= 6 || (threatened && gems_ >= 10) ||
                      task_ == Task::other;
    if (!want) return false;
    task_ = Task::other;
    target_ = kTribute;
    set_status(Task::other, kTribute);
    const vec2 altar = world().altars[a];
    if (cheb(pos_, altar) <= bc::constants::submit_gems_range) {
        try {
            me.submit_gems(altar);
            drop_ward(me);
            task_ = Task::farm;
            target_ = -1;
            return true;
        } catch (const std::exception&) {
            // Locked or blocked: attack the lock if one is visible.
            if (bc::vision::has_altar_lock(altar)) {
                const auto lock = bc::vision::altar_lock_at(altar);
                if (lock.is_enemy() && can_attack_) {
                    try {
                        me.attack(lock);
                    } catch (const std::exception&) {
                    }
                }
            }
            return true;
        }
    }
    travel(me, world().altar_poi[a]);
    return true;
}

bool Bot::capture(bc::champion& me) {
    auto& W = world();
    if (task_ == Task::capture && target_ >= 0) {
        const auto& g = W.groups[target_];
        if (g.state != GroupState::reward) {
            task_ = Task::farm;
            target_ = -1;
        }
    }
    if (task_ != Task::capture) {
        // Claim a waiting reward nearby that nobody else is collecting.
        int best = -1, best_d = 1000;
        for (const auto& g : W.groups) {
            if (g.state != GroupState::reward || g.seen_round != round_) continue;
            const int d = cheb(g.anchor, pos_);
            if (d > 10 || d >= best_d || g.id > kMaxTarget) continue;
            if (comms_.claimed_by_other(slot_, Task::capture, g.id, slots_)) continue;
            best = g.id;
            best_d = d;
        }
        if (best < 0) return false;
        task_ = Task::capture;
        target_ = best;
        wait_rounds_ = 0;
    }
    set_status(Task::capture, target_);
    const auto& g = W.groups[target_];
    for (auto t : g.tiles)
        if (t == pos_) {
            drop_ward(me);  // keep an eye on the group after we leave
            return true;    // stand still until the reward lands
        }
    load_occupancy();
    bc::bitboard goal;
    goal.clear();
    for (auto t : g.tiles)
        if (!occupied_.get(t)) goal.set(t);
    if (goal.is_empty()) return true;
    approach(me, goal);
    return true;
}

// ---------------------------------------------------------------------------
// Farming plan

Forecast Bot::forecast(const Group& g, float dps) const {
    dps = std::max(dps, 1.f);
    Forecast f{0, 0};
    if (g.harmless) {
        for (const auto& c : g.camps)
            if (c.alive_known) f.rounds += c.hp_known / dps;
        return f;
    }
    // Camps are pre-sorted strongest first: focus fire kills them in that order.
    float threat = 0;
    for (const auto& c : g.camps)
        if (c.alive_known) threat += c.strength;
    for (const int i : g.by_strength) {
        const auto& c = g.camps[i];
        if (!c.alive_known) continue;
        const float t = c.hp_known / dps;
        f.rounds += t;
        f.damage += threat * t;
        threat -= c.strength;
    }
    return f;
}

std::uint16_t Bot::dist_from_me(vec2 p) {
    auto& W = world();
    if (!W.in_bounds(p)) return kUnreached;
    if (me_field_round_ != round_ || me_field_pos_ != pos_ || !me_field_) {
        if (bc::fuel_remaining() < static_cast<std::uint64_t>(W.w) * W.h * 3 + 25000) return kUnreached;
        if (!me_field_) me_field_.emplace();
        bc::bitboard roots;
        roots.clear();
        roots.set(pos_);
        bc::bitboard visited;
        me_field_->compute(W.blocked_board, roots, visited);
        me_field_round_ = round_;
        me_field_pos_ = pos_;
    }
    return me_field_->get(p);
}

int Bot::dist_to_group(const Group& g) {
    auto& W = world();
    auto& cached = group_dist_[g.id & 63];
    if (cached.round == round_ && cached.pos == pos_) return cached.dist;
    const int d = W.has_field(g.poi) ? W.dist(g.poi, pos_) : dist_from_me(g.anchor);
    cached = {d, round_, pos_};
    return d;
}

bool Bot::enemy_side(Group& g) {
    // Terrain distances never change: compute once per group.
    if (g.side < 0) {
        auto& W = world();
        const int mine = W.dist(W.spawn_poi, g.anchor, 30000);
        const int theirs = W.dist(W.enemy_spawn_poi, g.anchor, 30000);
        if (mine == kUnreached || theirs == kUnreached) return false;  // try again later
        g.side = theirs < mine ? 1 : 0;
    }
    return g.side == 1;
}

void Bot::team_power(float& dps, float& hp, int& active, bool include_home) const {
    dps = 0;
    hp = 0;
    active = 0;
    for (int i = 0; i < slots_; ++i) {
        const auto s = comms_.slot(i);
        // Dead, recalling or shopping teammates will not be at the fight soon.
        if (i != slot_ && s.task == Task::other && (s.target == kDead || (s.target == kHome && !include_home))) continue;
        if (i == slot_) {
            dps += dps_of(type_, strength_, focus_);
            hp += max_hp_;
        } else {
            dps += ally_dps_[i] * comms_.power();
            hp += ally_hp_[i] * 0.85f;
        }
        ++active;
    }
    if (slot_ < 0) {
        dps += dps_of(type_, strength_, focus_);
        hp += max_hp_;
        ++active;
    }
}

float Bot::group_rounds(const Group& g, float dps, int travel) const {
    int wait = 0;
    if (g.state == GroupState::respawning) wait = std::max(0, g.respawn_round - (round_ + travel));
    return travel + wait + forecast(g, dps).rounds + kCaptureRounds;
}

int Bot::choose_squad_target(float team_dps, float team_hp, int exclude) {
    auto& W = world();
    int best = -1;
    float best_score = 0;
    for (auto& g : W.groups) {
        if (bc::fuel_remaining() < 20000) break;
        if (g.unreachable || g.harmless || g.id > kMaxTarget || g.id == exclude || g.state == GroupState::reward) continue;
        if (round_ < avoid_until_[g.id & 63]) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_)) continue;
        const int d = dist_to_group(g);
        if (forecast(g, team_dps).damage > team_hp * 0.55f) continue;
        if (d == kUnreached) continue;
        if (!g.alive_by(round_ + d + 15)) continue;
        float score = g.value(kGoldWeight) / group_rounds(g, team_dps, d);
        // Contested ground near the enemy spawn is riskier.
        if (enemy_side(g)) score *= 0.7f;
        if (score > best_score) {
            best_score = score;
            best = g.id;
        }
    }
    return best;
}

void Bot::local_power(float& dps, float& hp) const {
    dps = dps_of(type_, strength_, focus_);
    hp = hp_;
    for (const auto& a : allies_seen_)
        if (cheb(a.pos, pos_) <= 8) {
            dps += a.dps;
            hp += a.hp;
        }
}

void Bot::choose_task(bc::champion& me) {
    (void)me;
    auto& W = world();
    float team_dps = 0, team_hp = 0;
    int active = 0;
    team_power(team_dps, team_hp, active);
    const int previous = target_;

    // Validate or replace the shared squad target. A squad that has waited too long
    // without enough people picks something the champions actually present can clear.
    int squad = comms_.squad_target();
    if (squad >= static_cast<int>(W.groups.size())) squad = -1;
    const bool stalled = squad >= 0 && target_ == squad && task_ == Task::farm && wait_rounds_ > 20 && !W.groups[squad].awake;
    // A squad that could not assemble enough strength here should not come straight back.
    if (stalled) avoid_until_[squad & 63] = round_ + 80;
    if (squad >= 0) {
        const auto& g = W.groups[squad];
        // Only drop the target when it is gone or too strong for the whole living team:
        // teammates popping home to shop must not send the squad across the map.
        float living_dps = 0, living_hp = 0;
        int living = 0;
        team_power(living_dps, living_hp, living, true);
        const bool stale = g.state == GroupState::reward ||
                           (g.state == GroupState::respawning && g.respawn_round > round_ + 25) ||
                           forecast(g, living_dps).damage > living_hp * 0.7f;
        if (stale || stalled) squad = -1;
    }
    if (squad < 0 && bc::fuel_remaining() > 30000) {
        float dps = team_dps, hp = team_hp;
        if (stalled) local_power(dps, hp);
        squad = choose_squad_target(dps, hp, stalled ? comms_.squad_target() : -1);
        if (squad >= 0 || !stalled) comms_.set_squad_target(squad);
        else squad = comms_.squad_target();
    }

    // Solo work is limited to harmless clusters nearby, and to a third of the team.
    int solos = 0;
    for (int i = 0; i < slots_; ++i)
        if (i != slot_ && comms_.slot(i).task == Task::solo) ++solos;
    const bool may_solo = solos < std::max(1, active / 3) || squad < 0;
    const float my_dps = dps_of(type_, strength_, focus_);
    int solo = -1;
    float solo_score = 0;
    for (auto& g : W.groups) {
        if (!may_solo) break;
        if (!g.harmless || g.unreachable || g.id > kMaxTarget || g.state == GroupState::reward) continue;
        if (comms_.claimed_by_other(slot_, Task::solo, g.id, slots_) ||
            comms_.claimed_by_other(slot_, Task::capture, g.id, slots_))
            continue;
        if (bc::fuel_remaining() < 12000) break;
        if (cheb(g.anchor, pos_) > 24) continue;
        const int d = dist_to_group(g);
        if (d == kUnreached || d > (squad < 0 ? 60 : 20)) continue;
        if (!g.alive_by(round_ + d + 5)) continue;
        float score = g.value(kGoldWeight) / group_rounds(g, my_dps, d);
        if (enemy_side(g)) score *= 0.6f;
        if (score > solo_score) {
            solo_score = score;
            solo = g.id;
        }
    }

    float squad_share = 0;
    if (squad >= 0) {
        const auto& g = W.groups[squad];
        const int d = dist_to_group(g);
        if (d != kUnreached) squad_share = g.value(kGoldWeight) / std::max(active, 1) / group_rounds(g, team_dps, d);
    }

    if (solo >= 0 && (squad < 0 || solo_score > squad_share * 1.25f)) {
        task_ = Task::solo;
        target_ = solo;
    } else if (squad >= 0) {
        task_ = Task::farm;
        target_ = squad;
    } else {
        task_ = Task::farm;
        target_ = -1;
    }
    plan_round_ = round_;
    if (target_ != previous) wait_rounds_ = 0;
}

bool Bot::farm(bc::champion& me) {
    auto& W = world();
    bool replan = target_ < 0 || task_ == Task::other || task_ == Task::capture || round_ - plan_round_ >= kReplanRounds;
    if (target_ >= 0 && task_ != Task::other && task_ != Task::capture) {
        const auto& g = W.groups[target_];
        if (!g.alive_by(round_ + 25) || g.state == GroupState::reward) replan = true;
        // Follow the squad when someone moved it.
        if (task_ == Task::farm && target_ != comms_.squad_target()) replan = true;
        if (wait_rounds_ > 20 && !g.awake) replan = true;
    }
    if (replan && bc::fuel_remaining() > 25000) choose_task(me);
    if (target_ < 0 || task_ == Task::other || task_ == Task::capture) return false;

    auto& g = W.groups[target_];
    set_status(task_, target_);
    // Close enough to the camps: wait for the party (fight_camps engages when ready).
    int nearest = 1000;
    for (auto t : g.tiles) nearest = std::min(nearest, cheb(t, pos_));
    const int standoff = ranged_ ? range_ : 1;
    // Near means near on foot: a wall of camps can make a close tile a long walk.
    const bool near = nearest <= standoff + 1 && W.dist(g.poi, pos_) <= standoff + 3;
    if (near && g.state != GroupState::respawning) {
        ++wait_rounds_;
        mark("farm-wait");
        bool any_camp = false;
        for (const auto& c : camps_)
            if (c.group == target_) any_camp = true;
        bool tiles_seen = false;
        for (auto t : g.tiles) tiles_seen |= visible_.get(t);
        if (!any_camp && g.state == GroupState::alive && tiles_seen) {
            // Everything here is dead but the reward is not visible yet; replan next turn.
            plan_round_ = -1000;
        }
        if (!any_camp && !tiles_seen) {
            // Bushes hide the camps: walk right up to them so we can see (and hit) them.
            bc::bitboard ring;
            ring.clear();
            for (auto t : g.tiles) ring.draw_chebyshev(t, 1);
            approach(me, ring);
        } else if (hazardous(pos_, target_) && can_move_) {
            // Wait out of reach of the neighbouring groups; enemies may wake them.
            load_occupancy();
            vec2 best = pos_;
            int best_d = 1000;
            for (auto d : kDirs) {
                const vec2 n = pos_ + d;
                if (!free_tile(n) || hazardous(n, target_)) continue;
                int dn = 1000;
                for (auto t : g.tiles) dn = std::min(dn, cheb(t, n));
                if (dn < best_d) {
                    best_d = dn;
                    best = n;
                }
            }
            if (best != pos_) try_move(me, best - pos_);
        } else if (nearest > standoff) {
            for (const auto& c : camps_)
                if (c.group == target_) {
                    approach_unit(me, c.pos, standoff);
                    break;
                }
        }
        return true;
    }
    if (g.state == GroupState::respawning && nearest <= 3) {
        ++wait_rounds_;
        mark("farm-respawn-wait");
        // An occupied spawn tile holds back the whole group's respawn: wait beside it.
        if (W.group_id_at(pos_) == target_) {
            std::vector<std::pair<vec2, int>> tiles;
            for (auto t : g.tiles) tiles.push_back({t, 0});
            step_away(me, tiles);
        }
        return true;
    }
    mark("farm-travel");
    travel(me, g.poi);
    return true;
}

// ---------------------------------------------------------------------------
// Shopping

void Bot::shop(bc::champion& me, bool consumables) {
    // An altar lock (10 gold) blocks enemy tributes, or cancels one in progress.
    if (consumables && !has_lock_ && gold_ >= 10) {
        try {
            me.buy(bc::consumable_type::altar_lock);
            gold_ -= 10;
        } catch (const std::exception&) {
        }
        has_lock_ = me.consumables()[bc::consumable_type::altar_lock].count() > 0;
    }
    const auto& build = build_for(type_);
    while (gold_ >= 30 && build_step_ < build.size()) {
        const auto& step = build[build_step_];
        const int cost = static_cast<int>(step.item.cost());
        if (cost <= 0 || gold_ < cost) break;
        const int n = std::min(step.count - build_bought_, gold_ / cost);
        if (n <= 0) break;
        try {
            me.buy(step.item, static_cast<std::uint32_t>(n));
        } catch (const std::exception&) {
            // Stat overflow or a shop rule: move on to the next step.
            ++build_step_;
            build_bought_ = 0;
            continue;
        }
        gold_ -= n * cost;
        build_bought_ += n;
        if (build_bought_ >= step.count) {
            ++build_step_;
            build_bought_ = 0;
        }
    }
}
