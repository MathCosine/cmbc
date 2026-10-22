#pragma once
// Static map knowledge plus what this champion has observed about camps.
// Each champion runs its own copy; teammates share only the 8-byte memory.
#include "util.h"
#include <memory>
#include <optional>

enum class GroupState : std::uint8_t { alive, reward, respawning };

struct CampInfo {
    bc::camp handle;
    vec2 pos;
    bc::camp_type kind;
    float max_hp;
    float strength;
    int range;
    // Last observation; reset when the group respawns.
    float hp_known;
    bool alive_known = true;
};

struct Group {
    int id = 0;
    std::optional<bc::map::camp_group> handle;
    std::vector<CampInfo> camps;
    std::vector<vec2> tiles;
    vec2 anchor;
    int gems = 0, gold = 0, respawn = 0;
    float max_hp = 0, strength = 0;
    int max_range = 0;
    bool harmless = false;  // every member has zero strength (gem/gold clusters)
    int poi = -1;

    // Knowledge, refreshed whenever the group is in this champion's vision.
    GroupState state = GroupState::alive;
    int respawn_round = 0;  // estimated round the group is alive again
    int seen_round = -1;
    float hp_left = 0;
    bool awake = false;
    bool unreachable = false;

    bool alive_by(int round) const { return state == GroupState::alive || (state == GroupState::respawning && respawn_round <= round); }
    float value(float gold_weight) const { return gems + gold_weight * gold; }
    void respawned() {
        state = GroupState::alive;
        awake = false;
        hp_left = max_hp;
        for (auto& c : camps) {
            c.hp_known = c.max_hp;
            c.alive_known = true;
        }
    }
};

// Point of interest with a lazily computed terrain-only distance field.
struct Poi {
    bc::bitboard roots;
    bool roots_walkable = true;  // false for altars: the roots stay blocked, approach tiles get distance 1
    std::unique_ptr<bc::distance_field> field;
};

class World {
public:
    int w = 0, h = 0;
    BitGrid blocked;      // walls, holes, altars and enemy spawn
    BitGrid ally_spawn;
    BitGrid enemy_spawn;
    BitGrid altars_grid;
    bc::bitboard blocked_board;
    bc::bitboard camp_board;  // every camp spawn tile
    std::vector<Group> groups;
    std::vector<int> group_at;  // tile -> group id or -1
    std::vector<vec2> altars;
    std::vector<int> altar_poi;
    std::vector<vec2> spawn_tiles;
    std::vector<Poi> pois;
    int spawn_poi = -1;
    int enemy_spawn_poi = -1;
    bool ready = false;

    void init();
    // Computes a field if it is missing and enough fuel remains; returns null otherwise.
    const bc::distance_field* field(int poi, std::uint64_t fuel_floor = 30000);
    // Terrain distance from a point of interest, or kUnreached when unknown.
    std::uint16_t dist(int poi, vec2 p, std::uint64_t fuel_floor = 30000);
    std::uint16_t dist_on(const bc::distance_field& f, vec2 p) const;
    bool has_field(int poi) const { return pois[poi].field != nullptr; }
    // Spend spare fuel precomputing fields, nearest groups first.
    void precompute(std::uint64_t fuel_floor);

    int group_id_at(vec2 p) const { return blocked.in(p) ? group_at[p.y * w + p.x] : -1; }
    bool in_bounds(vec2 p) const { return blocked.in(p); }

private:
    int add_poi(const bc::bitboard& roots, bool walkable);
    std::size_t precompute_next = 0;
};

World& world();
