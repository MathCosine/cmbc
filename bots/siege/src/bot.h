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
    float burst = 0;  // damage it can deal in one turn: basic attack(s) plus its biggest spell
    float now = 0;    // damage per round over the next few turns from live cooldowns (kReadyFights)
    int stunned = 0;     // remaining stun (kStunBuffs, kFocusStunned)
    float shield = 0;    // shield value (kShieldHp)
    bool disarmed = false;  // every damage and stun spell on a long cooldown (kPunish)
    float basic = 0;     // basic attack damage per round (kPunish)
    bool across = false;  // more than kReachSlack further on foot than in a straight line (kReachFights)
    int reach = 1;    // tiles from which that burst can land next turn (walks, spells, dashes)
    bc::champion_type type{};
};

// Where enemy champions were seen together, so targets near them can be avoided for a while.
struct Sighting {
    vec2 pos{-100, -100};
    int round = -1000;
    int count = 0;
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
    void fire_spells(bool retreating);
    static int enemy_id(const SeenChamp& e);
    bool harmless_on_spawn(const SeenChamp& e) const;
    bool fight_camps(bc::champion& me);
    bool flee_camps(bc::champion& me);
    bool retreat_from_camps(bc::champion& me);
    bool can_beat(const Group& g) const;
    bool hazardous(vec2 p, int except_group) const;
    bool go_home(bc::champion& me, bool urgent);
    bool tribute(bc::champion& me);
    float tribute_threat() const;
    bool abandon_tribute(bc::champion& me);
    bool capture(bc::champion& me);
    bool farm(bc::champion& me);

    // Planning.
    void choose_task(bc::champion& me);
    int choose_squad_target(float team_dps, float team_hp, int exclude, bool allow_contested = false);
    bool opening_blocks(Group& g, bool allow_contested);
    void scout_ward(bc::champion& me);
    void local_power(float& dps, float& hp) const;
    Forecast forecast(const Group& g, float dps) const;
    float group_rounds(const Group& g, float dps, int travel) const;
    int dist_to_group(Group& g);
    bool enemy_side(Group& g);
    std::uint16_t dist_from_me(vec2 p);
    bool engage_ready(const Group& g);
    void team_power(float& dps, float& hp, int& active, bool include_home = false, int party = -1) const;
    bool split_active() const;
    int party_of(int slot) const;
    int party_b_target() const;
    int my_party_target() const;

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
    void shop(bc::champion& me, bool consumables);
    void place_lock(bc::champion& me);
    void drop_ward(bc::champion& me);

    // Movement.
    bool travel(bc::champion& me, int poi);
    bool approach(bc::champion& me, bc::bitboard goal);
    bool approach_unit(bc::champion& me, vec2 target, int range);
    bool step_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats, bool grass = false);
    bool try_move(bc::champion& me, vec2 delta);
    bool try_dash_along(bc::champion& me, const bc::distance_field& field, bool walkable_roots);
    bool make_way(bc::champion& me, vec2 blocked);
    bool free_tile(vec2 p) const;

    // Helpers.
    float incoming_camp_damage(vec2 at) const;
    bool camp_threatens(const SeenCamp& c, vec2 at) const;
    float design_dps(const bc::champion& c, float strength, float focus) const;
    int best_altar(int& distance);
    bool altar_open(int index) const;
    bool altar_walls_open(int index, bool stale_ok) const;
    bool in_their_third(vec2 p) const;
    bool blocks_altar(const Group& g) const;
    bool any_altar_open() const;
    float heal_rounds(const Group& g, float dps, float hp);
    bool rally_hold(Group& g);
    int rally_since_ = -1;
    int home_dist(const Group& g) const;
    float siege_rounds(const Group& g, float team_dps, float team_hp, bool unlocks);
    void set_status(Task task, int target);

    // Reading the danger: enemy sightings, our own last death, and team deaths.
    void note_sightings();
    float danger_factor(vec2 p) const;
    float target_danger(Group& g);
    bool enemy_recent(int rounds) const { return round_ - last_enemy_round_ <= rounds; }
    bool calm() const;
    bool on_my_camp() const;
    int squad_dist(const Group& g);
    bool unsafe_alone(Group& g);
    bool stun_wasted(const SeenChamp& e) const;
    bool stun_turn() const;
    int ahead_target_ = -1, ahead_for_ = -1, ahead_round_ = -1000;  // kRouteAhead: where the squad goes next
    int next_stop(const Group& g);
    float squad_mods(Group& g, bool contested_now);
    std::vector<vec2> dash_landings();
    bool dash_toward(bc::champion& me, vec2 target, int within);
    bool dash_away(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats);
    bool held_altar_near(vec2 p) const;
    int recent_team_deaths(int window) const;
    bool pressured() const;
    bool enemy_passive() const;
    int carry_gems() const;
    float enemy_side_factor(bool solo) const;
    bool wait_for_escort();
    bool early_warning(bc::champion& me);
    bool capturing_here() const;
    bool banker_free_near(vec2 p, int range, int group) const;
    bool solo_camp_ok(Group& g, float dps);
    bool altar_guarded(vec2 altar) const;
    int altar_exposure(vec2 altar) const;
    int altar_danger(vec2 altar) const;
    bool escort_banker(bc::champion& me);
    bool guard_altar(bc::champion& me);
    bool intercept_tribute(bc::champion& me);
    bool breach_active() const;
    float kill_value(const SeenChamp& e) const;
    bool better_target(const SeenChamp& e, const SeenChamp& than) const;
    bool near_contested_altar(vec2 p) const;
    bool lock_detour(bc::champion& me);
    bool invaded() const;
    float altar_zone(const Group& g) const;
    bool peel_back(bc::champion& me, const SeenChamp* target);
    bool answer_call(bc::champion& me);
    const std::vector<int>& nearest_groups() const;
    mutable std::vector<int> nearest_;  // nearest_groups() for nearest_round_ at nearest_pos_
    mutable int nearest_round_ = -1;
    mutable vec2 nearest_pos_{-1, -1};
    const SeenCamp* breach_camp() const;
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
    // Learned enemy strength: champion damage we took against what the fight model expected.
    float dmg_seen_ = 0, dmg_expected_ = 0, last_expect_ = 0;
    int last_expect_round_ = -10;
    float enemy_scale() const;
    // Gem field we camp with the whole team (kFieldCamp): worked out once.
    bool field_ready_ = false;
    vec2 field_centre_{-100, -100};
    int field_gems_ = 0;
    const vec2* field();
    bool in_field(vec2 p);
    int last_hp_round_ = -10;
    bool can_move_ = false, can_attack_ = false, can_cast_ = false;
    bool moved_ = false, cast_done_ = false;
    std::array<int, 2> ready_cache_{-1, -1};
    std::vector<SeenCamp> camps_;
    std::vector<SeenChamp> enemies_;
    std::vector<SeenChamp> allies_seen_;
    struct CachedDps {
        float dps = 0;
        float burst = 0;
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
    bool siege_ = false;  // our squad target needs several trips
    bool has_lock_ = false;
    int ward_ready_round_ = 0;
    std::array<int, 32> enemy_lock_round_ = [] {
        std::array<int, 32> a{};
        a.fill(-1000);
        return a;
    }();
    std::array<int, 64> avoid_until_{};
    struct CachedDist {
        int dist = 0;
        int round = -1;
        vec2 pos{};
    };
    std::array<CachedDist, 64> group_dist_{};
    vec2 last_pos_{-1, -1};
    int last_hurt_round_ = -1000;  // last round we lost health
    int last_champ_hurt_round_ = -1000;  // last round we lost more than nearby camps deal (kRecallSafe 2)
    bool unseen_hit_ = false;            // hit this round by a champion none in sight could reach us from
    void reveal_ward(bc::champion& me);
    bool power_play() const;
    int living_count() const;
    bool healer_seen(const SeenChamp& e) const;
    std::array<float, 16> enemy_last_hp_{};
    std::array<int, 16> enemy_hp_round_{};
    std::array<int, 16> enemy_healed_round_{};
    int hidden_grass(vec2 p) const;
    bool safe_wait_step(bc::champion& me, const Group& g, int keep);
    bool hole_escape(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats);
    int combo_hitters(const SeenChamp& e) const;
    bool escape_recall(bc::champion& me);
    bool crystal_escape(bc::champion& me, const std::vector<std::pair<vec2, int>>& threats);
    bool can_act_crystal(bc::champion& me) const;
    std::vector<bc::champion> enemy_roster_;  // kPowerPlay
    int enemies_alive_ = 0;
    std::array<float, kMaxSlots> ally_drop_{};       // kHealFocus: health lost since last turn
    std::array<float, kMaxSlots> ally_last_hp_{};
    std::array<int, kMaxSlots> ally_seen_round_{};
    bool early_siege_ok_ = false;  // kSiegeEarly: siege_rounds ignores kSiegeFrom
    int attacked_round_ = -1000;      // last round we landed a basic attack
    int fruitless_ = 0;               // kChaseGiveUp: fight turns in a row with no hit either way
    int chase_ignore_until_ = -1000;
    int way_pick_ = -1;            // kOnTheWay: the stop the last squad choice put first, or -1
    int way_check_round_ = -1000;
    bool camp_attack_ = false;     // attack_target called on a camp (kSaveReset)
    bool scan_partial_ = false;    // kFullScan: the last squad scan stopped early for fuel
    int partial_squad_ = -1;       // squad camp chosen from such a scan, chosen again later
    int partial_round_ = -1000;
    float dps_now(const bc::champion& c, bc::champion_type t, float strength, float focus) const;
    int capture_fail_ = 0;         // turns in a row we could not step toward our capture
    int capture_since_ = -1;       // round this capture claim started
    int yield_slot_ = -1;          // ally we are making way for in a corridor
    int yield_until_ = -1;
    std::array<int, 32> altar_avoid_until_{};  // altars we could not walk to lately

    // Danger memory: recent enemy sightings and where this champion last died.
    std::array<Sighting, 8> sightings_{};
    int sighting_next_ = 0;
    int last_enemy_round_ = -1000;
    vec2 death_pos_{-100, -100};
    int death_round_ = -1000;
    bool was_alive_ = true;
    int respawn_round_ = -1000;  // first living round after our last death

    // Team deaths seen through the slot statuses.
    std::array<bool, kMaxSlots> slot_dead_{};
    std::array<int, 16> team_deaths_ = [] {
        std::array<int, 16> a{};
        a.fill(-100000);
        return a;
    }();
    int team_death_next_ = 0;
    int hold_turns_ = 0;  // turns spent waiting for an enemy to step into our range
    vec2 ward_pos_{-100, -100};  // where our ward stands
    int banker_slot_ = -1;       // our Marcella's slot, if we drafted her
    int altar_wait_until_ = -1;  // enemies held the altar: farm until this round
    int escort_wait_ = 0;        // rounds this altar trip has waited for a teammate
    bool bag_fight_ = false;     // this turn's fight is on a bag worth taking near an altar
    mutable int held_round_ = -1;           // round held_altars_ was worked out
    mutable std::vector<vec2> held_altars_;  // middle/enemy-side altars with enemies seen lately
    std::vector<bc::champion_type> ally_types_;  // champion type per slot
    int enemy_banked_ = 0;               // enemy team's turned-in gems as last seen
    int party_target_ = -1;              // second squad: the target its leader chose
    int other_party_target_ = -1;        // a camp the other squad is on (skip it when choosing)
    bool choosing_small_ = false;        // choosing for the second squad: no sieges
    int enemy_banked_round_ = -1000;     // last round that total went up
};
