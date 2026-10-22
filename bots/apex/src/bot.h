#pragma once
// Per-champion decision making. Every champion runs one Bot; teammates
// coordinate through Comms (shared memory) and what they can see.
#include "comms.h"
#include "util.h"
#include "world.h"
#include <array>
#include <optional>
#include <vector>

struct SeenCamp {
    bc::camp handle;
    vec2 pos;
    float hp;
    float strength;
    int range;
    bool awake;
    int group;
};

struct SeenChamp {
    bc::champion handle;
    vec2 pos;
    float hp;
    float max_hp;
    float dps;
    int range;
    int gems;
};

struct Spell {
    bc::ability handle;
    bc::ability_kind kind;
    float range = 0, modifier = 0, radius = 0;
    int duration = 0;
    bool exclusive = false;
};

// Rough fight forecast: rounds to clear and damage soaked doing it.
struct Forecast {
    float rounds;
    float damage;
};

class Bot {
public:
    void play_turn();

private:
    // Setup and observation.
    void init(bc::champion& me);
    void observe(bc::champion& me);
    void update_groups();
    void load_occupancy();

    // Top-level behaviours; each returns true once it has used the turn.
    void act(bc::champion& me);
    bool fight_enemies(bc::champion& me);
    bool fight_camps(bc::champion& me);
    bool flee_camps(bc::champion& me);
    bool retreat_from_camps(bc::champion& me);
    bool can_beat(const Group& g) const;
    bool hazardous(vec2 p, int except_group) const;
    bool go_home(bc::champion& me, bool urgent);
    bool tribute(bc::champion& me);
    bool capture(bc::champion& me);
    bool farm(bc::champion& me);

    // Planning.
    void choose_task(bc::champion& me);
    int choose_squad_target(float team_dps, float team_hp, int exclude);
    void local_power(float& dps, float& hp) const;
    Forecast forecast(const Group& g, float dps) const;
    float group_rounds(const Group& g, float dps, int travel) const;
    int dist_to_group(const Group& g);
    bool enemy_side(Group& g);
    std::uint16_t dist_from_me(vec2 p);
    bool engage_ready(const Group& g);
    void team_power(float& dps, float& hp, int& active) const;

    // Actions.
    bool attack_target(bc::champion& me, const bc::unit& target, vec2 target_pos, float target_hp);
    bool attack_camp(bc::champion& me, const SeenCamp& camp);
    void use_combat_spells(bc::champion& me, const SeenCamp* camp, const SeenChamp* enemy);
    bool cast(Spell& spell);
    bool cast_on(Spell& spell, const bc::camp& target);
    bool cast_on(Spell& spell, const bc::champion& target);
    bool cast_at(Spell& spell, vec2 tile);
    bool spell_ready(int index);
    bool try_heal(bc::champion& me);
    void shop(bc::champion& me);

    // Movement.
    bool travel(bc::champion& me, int poi);
    bool approach(bc::champion& me, bc::bitboard goal);
    bool approach_unit(bc::champion& me, vec2 target, int range);
    bool step_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats);
    bool try_move(bc::champion& me, vec2 delta);
    bool try_dash_along(bc::champion& me, const bc::distance_field& field, bool walkable_roots);
    bool free_tile(vec2 p) const;

    // Helpers.
    float incoming_camp_damage(vec2 at) const;
    bool camp_threatens(const SeenCamp& c, vec2 at) const;
    float design_dps(const bc::champion& c, float strength, float focus) const;
    int best_altar(int& distance);
    bool altar_open(int index) const;
    void set_status(Task task, int target);
    bool on_spawn() const { return world().ally_spawn.get(pos_); }

    // Match constants.
    bool ready_ = false;
    int slot_ = -1;
    int slots_ = 0;
    bc::champion_type type_{};
    bool ranged_ = false;
    int range_ = 1;
    std::vector<bc::champion> allies_;
    std::vector<float> ally_dps_;
    std::vector<float> ally_hp_;
    std::vector<Spell> spells_;
    float my_design_dps_ = 0;
    bool tank_ = false;

    // Shopping progress: index into the build and shards bought for that step.
    std::size_t build_step_ = 0;
    int build_bought_ = 0;

    // Per-turn state.
    Comms comms_;
    int round_ = 0;
    vec2 pos_{};
    float hp_ = 0, max_hp_ = 0, strength_ = 0, focus_ = 0;
    int gems_ = 0, gold_ = 0;
    float last_hp_ = 0, damage_taken_ = 0;
    int last_hp_round_ = -10;
    bool can_move_ = false, can_attack_ = false, can_cast_ = false;
    bool moved_ = false, cast_done_ = false;
    std::array<int, 2> ready_cache_{-1, -1};
    std::vector<SeenCamp> camps_;
    std::vector<SeenChamp> enemies_;
    std::vector<SeenChamp> allies_seen_;
    struct CachedDps {
        float dps = 0;
        int round = -1;
    };
    std::array<CachedDps, 32> dps_cache_{};
    BitGrid visible_;
    BitGrid alive_vis_;
    BitGrid awake_vis_;
    BitGrid occupied_;
    bc::bitboard occupied_board_;
    bool occupancy_loaded_ = false;
    std::optional<bc::distance_field> me_field_;
    int me_field_round_ = -1;
    vec2 me_field_pos_{};
    bc::distance_field scratch_field_;

    // Current plan.
    Task task_ = Task::farm;
    int target_ = -1;
    int plan_round_ = -1000;
    int wait_rounds_ = 0;
    int stuck_rounds_ = 0;
    int altar_choice_ = -1;
    struct CachedDist {
        int dist = 0;
        int round = -1;
        vec2 pos{};
    };
    std::array<CachedDist, 64> group_dist_{};
    vec2 last_pos_{-1, -1};
};
