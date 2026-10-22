#include "world.h"
#include <array>
#include <cstdlib>

World& world() {
    static World instance;
    return instance;
}

int World::add_poi(std::vector<vec2> tiles, const bc::bitboard* board, bool walkable) {
    Poi poi;
    poi.tiles = std::move(tiles);
    poi.board = board;
    poi.roots_walkable = walkable;
    pois.push_back(std::move(poi));
    return static_cast<int>(pois.size()) - 1;
}

void World::init() {
    w = bc::map::width();
    h = bc::map::height();
    const auto& enemy_region = bc::map::spawn_region(team::enemy);
    blocked_board = bc::map::walls() | bc::map::holes() | bc::map::altars() | enemy_region;
    blocked.load(blocked_board, w, h);
    ally_spawn.load(bc::map::spawn_region(team::ally), w, h);
    enemy_spawn.load(enemy_region, w, h);
    altars_grid.load(bc::map::altars(), w, h);
    altars = bc::map::altars().positions();
    spawn_tiles = bc::map::spawn_region(team::ally).positions();
    group_at.reset(new std::int16_t[static_cast<std::size_t>(w) * h]);
    camp_at.reset(new std::int16_t[static_cast<std::size_t>(w) * h]);
    camp_board.clear();

    // Camp design stats are per kind; query each kind once.
    std::array<std::optional<bc::camp_stats>, 6> kind_stats;
    const auto raw_groups = bc::map::camp_groups();
    for (const auto& raw : raw_groups) {
        Group g;
        g.id = static_cast<int>(groups.size());
        g.handle = raw;
        g.gems = static_cast<int>(raw.gem_value());
        g.gold = static_cast<int>(raw.gold_value());
        g.respawn = raw.max_respawn_timer();
        for (const auto& c : raw.camps()) {
            const auto kind = c.kind();
            auto& stats = kind_stats[static_cast<int>(kind)];
            if (!stats) stats = c.base_stats();
            CampInfo info{c, c.spawn_position(), kind, stats->base_health, stats->base_strength, stats->base_attack_range,
                          stats->base_health};
            g.max_hp += info.max_hp;
            g.strength += info.strength;
            g.max_range = std::max(g.max_range, info.strength > 0 ? info.range : 0);
            g.tiles.push_back(info.pos);
            group_at[info.pos.y * w + info.pos.x] = static_cast<std::int16_t>(g.id);
            camp_at[info.pos.y * w + info.pos.x] = static_cast<std::int16_t>(g.camps.size());
            camp_board.set(info.pos);
            g.camps.push_back(std::move(info));
        }
        if (g.camps.empty()) continue;
        g.harmless = g.strength <= 0;
        for (int i = 0; i < static_cast<int>(g.camps.size()); ++i) g.by_strength.push_back(i);
        std::stable_sort(g.by_strength.begin(), g.by_strength.end(),
                         [&](int a, int b) { return g.camps[a].strength > g.camps[b].strength; });
        g.hp_left = g.max_hp;
        // Anchor on the tile closest to the group's centre so distances are representative.
        vec2 sum{0, 0};
        for (auto t : g.tiles) sum += t;
        const vec2 centre = sum / static_cast<int>(g.tiles.size());
        g.anchor = *std::min_element(g.tiles.begin(), g.tiles.end(), [&](vec2 a, vec2 b) { return cheb(a, centre) < cheb(b, centre); });
        for (auto t : g.tiles) g.extent = std::max(g.extent, cheb(t, g.anchor));
        g.poi = add_poi(g.tiles, nullptr, true);
        groups.push_back(std::move(g));
    }

    camp_grid.load(camp_board, w, h);
    spawn_poi = add_poi({}, &bc::map::spawn_region(team::ally), true);
    enemy_spawn_poi = add_poi({}, &enemy_region, false);
    for (auto altar : altars) altar_poi.push_back(add_poi({altar}, nullptr, false));
    ready = true;
}

const bc::distance_field* World::field(int poi_id, std::uint64_t fuel_floor) {
    if (poi_id < 0 || poi_id >= static_cast<int>(pois.size())) return nullptr;
    auto& poi = pois[poi_id];
    if (poi.field) return poi.field.get();
    // A full-map breadth-first search costs roughly three fuel per tile.
    const std::uint64_t estimate = static_cast<std::uint64_t>(w) * h * 3 + 2000;
    if (bc::fuel_remaining() < fuel_floor + estimate) return nullptr;
    // Roots that are themselves blocked (altars, enemy spawn) seed the search; walkers stop next to them.
    // Live camps block movement, so routes go around every camp but the target's own.
    auto field = std::make_unique<bc::distance_field>();
    bc::bitboard visited;
    const auto roots = poi.roots();
    field->compute((blocked_board | camp_board) & ~roots, roots, visited);
    poi.field = std::move(field);
    return poi.field.get();
}

std::uint16_t World::dist(int poi, vec2 p, std::uint64_t fuel_floor) {
    if (!in_bounds(p)) return kUnreached;
    const auto* f = field(poi, fuel_floor);
    if (!f) return kUnreached;
    return dist_on(*f, p);
}

std::uint16_t World::dist_on(const bc::distance_field& f, vec2 p) const {
    const std::uint16_t here = f.get(p);
    if (here != kUnreached || group_id_at(p) < 0) return here;
    // Standing on a dead camp's tile, which the fields treat as blocked: measure from the
    // nearest open ring around us (a capture can leave us inside a block of camp tiles).
    for (int r = 1; r <= 3; ++r) {
        std::uint16_t best = kUnreached;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const vec2 n{p.x + dx, p.y + dy};
                if (!in_bounds(n) || blocked.get(n)) continue;
                best = std::min(best, f.get(n));
            }
        if (best != kUnreached) return static_cast<std::uint16_t>(best + r);
    }
    return kUnreached;
}

void World::precompute(std::uint64_t fuel_floor) {
    // Spawns and altars first (every trip home and every tribute needs them), then groups.
    const std::size_t specials = 2 + altar_poi.size();
    while (precompute_next < pois.size()) {
        const std::size_t index = precompute_next;
        int poi;
        if (index == 0) poi = spawn_poi;
        else if (index == 1) poi = enemy_spawn_poi;
        else if (index < specials) poi = altar_poi[index - 2];
        else poi = groups[index - specials].poi;
        if (!has_field(poi) && !field(poi, fuel_floor)) return;
        ++precompute_next;
    }
}
