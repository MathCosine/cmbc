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
    // A turn that runs out of fuel in here runs it again next turn: start from empty lists.
    groups.clear();
    pois.clear();
    altar_poi.clear();
    precompute_next = 0;
    breach.clear();
    breach_board.clear();
    breach_group = -1;
    breach_planned = false;
    w = bc::map::width();
    h = bc::map::height();
    const auto& enemy_region = bc::map::spawn_region(team::enemy);
    blocked_board = bc::map::walls() | bc::map::holes() | bc::map::altars() | enemy_region;
    blocked.load(blocked_board, w, h);
    ally_spawn.load(bc::map::spawn_region(team::ally), w, h);
    enemy_spawn.load(enemy_region, w, h);
    altars_grid.load(bc::map::altars(), w, h);
    bushes.load(bc::map::bushes(), w, h);
    altars = bc::map::altars().positions();
    spawn_tiles = bc::map::spawn_region(team::ally).positions();
    ally_centre = spawn_tiles.empty() ? vec2{} : spawn_tiles[spawn_tiles.size() / 2];
    const auto enemy_tiles = enemy_region.positions();
    enemy_centre = enemy_tiles.empty() ? vec2{} : enemy_tiles[enemy_tiles.size() / 2];
    group_at.reset(new std::int16_t[static_cast<std::size_t>(w) * h]);
    camp_at.reset(new std::int16_t[static_cast<std::size_t>(w) * h]);
    camp_board.clear();

    // Camp design stats are per kind; query each kind once.
    std::array<std::optional<bc::camp_stats>, 6> kind_stats;
    const auto raw_groups = bc::map::camp_groups();
    // Growing these lists one push at a time cost most of the setup's fuel on big maps.
    groups.reserve(raw_groups.size());
    pois.reserve(raw_groups.size() + 2 + altars.size());
    for (const auto& raw : raw_groups) {
        Group g;
        g.id = static_cast<int>(groups.size());
        g.handle = raw;
        g.gems = static_cast<int>(raw.gem_value());
        g.gold = static_cast<int>(raw.gold_value());
        g.respawn = raw.max_respawn_timer();
        g.camps.reserve(raw.camps().size());
        g.tiles.reserve(raw.camps().size());
        g.by_strength.reserve(raw.camps().size());
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
        // Strongest first, ties in map order (insertion sort: a group has a handful of camps).
        for (int i = 0; i < static_cast<int>(g.camps.size()); ++i) {
            int j = static_cast<int>(g.by_strength.size());
            g.by_strength.push_back(i);
            while (j > 0 && g.camps[g.by_strength[j - 1]].strength < g.camps[i].strength) {
                g.by_strength[j] = g.by_strength[j - 1];
                --j;
            }
            g.by_strength[j] = i;
        }
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
    // Precompute fields nearest first. In group order, the first targets on a big map (100x100)
    // were whichever low-numbered groups had fields: the squad walked 40-60 tiles to them and
    // left the camps beside our spawn for 200 rounds.
    group_order.clear();
    for (std::size_t i = 0; i < groups.size(); ++i) group_order.push_back(static_cast<int>(i));
    std::sort(group_order.begin(), group_order.end(), [&](int a, int b) {
        return cheb(groups[a].anchor, ally_centre) < cheb(groups[b].anchor, ally_centre);
    });
    spawn_poi = add_poi({}, &bc::map::spawn_region(team::ally), true);
    enemy_spawn_poi = add_poi({}, &enemy_region, false);
    for (auto altar : altars) altar_poi.push_back(add_poi({altar}, nullptr, false));
    bank_altars.clear();
    for (std::size_t i = 0; i < (kBankCost ? altars.size() : 0); ++i)
        if (altar_side(altars[i], ally_centre, enemy_centre) <= kBankMaxSide) bank_altars.push_back(static_cast<int>(i));
    if (kBankCost && bank_altars.empty())
        for (std::size_t i = 0; i < altars.size(); ++i)
            if (altar_side(altars[i], ally_centre, enemy_centre) <= 1) bank_altars.push_back(static_cast<int>(i));
    if (kBankCost && bank_altars.empty())
        for (std::size_t i = 0; i < altars.size(); ++i) bank_altars.push_back(static_cast<int>(i));
    if (kBankCost) bank_cache.assign(groups.size(), kBankUnknown);
    bank_median = -1;
    if (kDraftPlan) {
        std::vector<vec2> anchors;
        anchors.reserve(groups.size());
        for (const auto& g : groups) anchors.push_back(g.anchor);
        plan.load(w, h, anchors);
    }
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
    auto camps = camp_board;
    if (!poi.roots_walkable && !poi.board) {
        // Altars: a camp beside one may be the only way in once it dies.
        bc::bitboard beside;
        beside.clear();
        for (auto t : poi.tiles) beside.draw_chebyshev(t, 1);
        camps &= ~beside;
    }
    if (!breach.empty()) camps &= ~breach_board;
    field->compute((blocked_board | camps) & ~roots, roots, visited);
    poi.field = std::move(field);
    return poi.field.get();
}

std::uint16_t World::bank_dist(int g) {
    if (g < 0 || g >= static_cast<int>(groups.size())) return kUnreached;
    if (plan.ok && plan.bank[g] != kUnreached) return plan.bank[g];
    if (bank_cache[g] != kBankUnknown) return bank_cache[g];
    // Only from fields already made (altar fields are precomputed first): never start one here.
    std::uint16_t best = kUnreached;
    for (int i : bank_altars) {
        if (!has_field(altar_poi[i])) return kUnreached;
        best = std::min(best, dist_on(*pois[altar_poi[i]].field, groups[g].anchor));
    }
    bank_cache[g] = best;
    return best;
}

int World::bank_ref() {
    if (bank_median >= 0) return bank_median;
    // Known once the note loaded or every bank altar has its field.
    bool ready = plan.ok;
    if (!ready) {
        ready = true;
        for (int i : bank_altars)
            if (!has_field(altar_poi[i])) ready = false;
    }
    if (!ready) return -1;
    std::vector<int> all;
    for (std::size_t g = 0; g < groups.size(); ++g)
        if (groups[g].gems > 0) {
            const int d = bank_dist(static_cast<int>(g));
            if (d != kUnreached) all.push_back(d);
        }
    if (all.empty()) return bank_median = 0;
    std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
    return bank_median = all[all.size() / 2];
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

void World::plan_breach(std::uint64_t fuel_floor) {
    if (breach_planned) return;
    const std::uint64_t estimate = static_cast<std::uint64_t>(w) * h * 3 + 2000;
    if (bc::fuel_remaining() < fuel_floor + estimate * (altars.size() + 2)) return;  // a later turn
    breach_planned = true;
    if (altars.empty() || spawn_tiles.empty()) return;
    const vec2 home = spawn_tiles[spawn_tiles.size() / 2];
    // An altar we can walk to past the live camps needs no breach.
    for (std::size_t i = 0; i < altar_poi.size(); ++i)
        if (dist(altar_poi[i], home, 0) != kUnreached) return;
    // Walk in on terrain alone and note the camps on the way; keep the altar needing fewest kills.
    for (auto altar : altars) {
        bc::bitboard roots;
        roots.clear();
        roots.set(altar);
        bc::distance_field f;
        bc::bitboard visited;
        f.compute(blocked_board & ~roots, roots, visited);
        if (f.get(home) == kUnreached) continue;
        std::vector<vec2> on_path;
        vec2 p = home;
        for (int step = 0; step < w * h && p != altar; ++step) {
            const auto move = f.next_move_from(p);
            if (!move) break;
            p += *move;
            if (camp_grid.get(p)) on_path.push_back(p);
        }
        if (!on_path.empty() && (breach.empty() || on_path.size() < breach.size())) breach = std::move(on_path);
    }
    if (breach.empty()) return;
    breach_group = group_id_at(breach.front());
    for (auto t : breach) breach_board.set(t);
    // Altar routes so far went around the breach camps: compute them again through it.
    for (int poi : altar_poi) pois[poi].field.reset();
}

void World::precompute(std::uint64_t fuel_floor) {
    plan_breach(fuel_floor);
    // Spawns and altars first (every trip home and every tribute needs them), then groups.
    const std::size_t specials = 2 + altar_poi.size();
    while (precompute_next < pois.size()) {
        const std::size_t index = precompute_next;
        int poi;
        if (index == 0) poi = spawn_poi;
        else if (index == 1) poi = enemy_spawn_poi;
        else if (index < specials) poi = altar_poi[index - 2];
        else poi = groups[group_order[index - specials]].poi;
        if (!has_field(poi) && !field(poi, fuel_floor)) return;
        ++precompute_next;
    }
}
