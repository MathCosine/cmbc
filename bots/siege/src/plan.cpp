#include "plan.h"
#include <cambc.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>

// Note layout (at most 1,024 bytes):
//   0-1 "SP", 2 version, 3 n (groups encoded), 4 map width, 5 map height, 6-7 anchor checksum,
//   8 contents (kHasPairs), then n group ids (World order), n distances from our spawn, n from the
//   enemy spawn and, with pairs, n(n-1)/2 group-to-group distances. Each distance is a byte holding
//   half the walk in tiles (rounded up), 255 when unknown.

namespace {

constexpr std::uint64_t kDraftReserve = 60000;  // left for picking and writing the note
constexpr std::uint8_t kHasPairs = 1;
// With pairs, 9 + 3n + n(n-1)/2 bytes fit 41 groups; without, 9 + 3n fit any map.
constexpr int kMaxEncoded = kRoutePlan ? 41 : 250;

struct DraftPlan {
    bool started = false;
    int w = 0, h = 0;
    std::vector<vec2> anchors;               // every group, World order
    std::vector<std::vector<vec2>> tiles;
    std::vector<int> enc;                    // groups encoded, nearest our spawn first
    std::vector<vec2> access;                // per encoded group: a free tile beside it
    std::vector<std::uint16_t> d;            // enc x enc, measured from the row's group
    std::vector<std::uint16_t> home;         // per encoded group, from our spawn
    std::vector<std::uint16_t> enemy;        // per encoded group, from the enemy spawn
    int spawns_done = 0;                     // 0, 1 (ours) or 2 (both)
    int next = 0;                            // next encoded group to search from
    bc::bitboard walls;                      // walls, holes, altars
    bc::bitboard camps;                      // every camp tile
    bc::bitboard blocked;                    // walls, enemy spawn and every camp
    BitGrid grid;                            // local copy of blocked
    BitGrid terrain;                         // walls, holes, altars, enemy spawn (World::blocked)
};

DraftPlan& state() {
    static DraftPlan p;
    return p;
}

vec2 centre_of(const std::vector<vec2>& pts) {
    vec2 sum{0, 0};
    for (auto t : pts) sum += t;
    return sum / static_cast<int>(pts.size());
}

void setup(DraftPlan& p) {
    p.started = true;
    p.w = bc::map::width();
    p.h = bc::map::height();
    const auto& enemy = bc::map::spawn_region(team::enemy);
    p.camps.clear();
    // Same rules as World::init: groups without camps are skipped, ids count the rest in order,
    // the anchor is the camp tile nearest the group's centre.
    for (const auto& raw : bc::map::camp_groups()) {
        std::vector<vec2> tiles;
        for (const auto& c : raw.camps()) tiles.push_back(c.spawn_position());
        if (tiles.empty()) continue;
        const vec2 c = centre_of(tiles);
        p.anchors.push_back(*std::min_element(tiles.begin(), tiles.end(), [&](vec2 a, vec2 b) { return cheb(a, c) < cheb(b, c); }));
        for (auto t : tiles) p.camps.set(t);
        p.tiles.push_back(std::move(tiles));
    }
    p.walls = bc::map::walls() | bc::map::holes() | bc::map::altars();
    p.blocked = p.walls | enemy | p.camps;
    p.grid.load(p.blocked, p.w, p.h);
    p.terrain.load(p.walls | enemy, p.w, p.h);
    const auto spawn = bc::map::spawn_region(team::ally).positions();
    vec2 ally_centre{p.w / 2, p.h / 2};
    if (!spawn.empty()) {
        std::vector<vec2> s(spawn.begin(), spawn.end());
        ally_centre = centre_of(s);
    }
    // Groups nearest our spawn first: when there are too many, the far ones go.
    std::vector<int> order(p.anchors.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return cheb(p.anchors[a], ally_centre) < cheb(p.anchors[b], ally_centre); });
    if (static_cast<int>(order.size()) > kMaxEncoded) order.resize(kMaxEncoded);
    p.enc = order;
    // Pair distances are measured to a free tile right beside one of the group's camps (camps
    // count as blocked), nearest the anchor. A free tile merely near the anchor could lie across a
    // hole or a wall: on 01yellow-brick-road and 01islands that made unreachable camps look close.
    for (int g : p.enc) {
        vec2 best{-1, -1};
        int best_d = 1000;
        for (auto t : p.tiles[g])
            for (auto d : kDirs) {
                const vec2 n = t + d;
                if (!p.grid.in(n) || p.grid.get(n)) continue;
                const int dn = cheb(n, p.anchors[g]);
                if (dn < best_d) {
                    best_d = dn;
                    best = n;
                }
            }
        p.access.push_back(best);
    }
    const std::size_t n = p.enc.size();
    if (kRoutePlan) p.d.assign(n * n, kUnreached);
    p.home.assign(n, kUnreached);
    p.enemy.assign(n, kUnreached);
}

// As World::dist_on: a camp tile is blocked in the field, so measure from the nearest open ring.
std::uint16_t at_anchor(const DraftPlan& p, const bc::distance_field& f, vec2 a) {
    for (int r = 1; r <= 3; ++r) {
        std::uint16_t best = kUnreached;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const vec2 n{a.x + dx, a.y + dy};
                if (!p.terrain.in(n) || p.terrain.get(n)) continue;
                best = std::min(best, f.get(n));
            }
        if (best != kUnreached) return static_cast<std::uint16_t>(best + r);
    }
    return kUnreached;
}

void search_spawn(DraftPlan& p, bool ours) {
    // As World::field for a spawn: the region seeds the search, every camp blocks.
    const auto& region = bc::map::spawn_region(ours ? team::ally : team::enemy);
    bc::bitboard visited;
    bc::distance_field f;
    f.compute((p.walls | bc::map::spawn_region(team::enemy) | p.camps) & ~region, region, visited);
    auto& out = ours ? p.home : p.enemy;
    for (std::size_t k = 0; k < p.enc.size(); ++k) out[k] = at_anchor(p, f, p.anchors[p.enc[k]]);
}

void search_from(DraftPlan& p, int k) {
    const int g = p.enc[k];
    bc::bitboard roots;
    roots.clear();
    for (auto t : p.tiles[g]) roots.set(t);
    bc::bitboard visited;
    bc::distance_field f;
    f.compute(p.blocked & ~roots, roots, visited);
    const std::size_t n = p.enc.size();
    for (std::size_t j = 0; j < n; ++j) {
        if (static_cast<int>(j) == k) {
            p.d[k * n + j] = 0;
            continue;
        }
        if (p.access[j].x >= 0) p.d[k * n + j] = f.get(p.access[j]);
    }
}

std::uint8_t half(std::uint16_t tiles) {
    if (tiles == kUnreached) return 255;
    return static_cast<std::uint8_t>(std::min<int>(254, (tiles + 1) / 2));
}

void write_note(const DraftPlan& p) {
    const std::size_t n = p.enc.size();
    const bool pairs = kRoutePlan && p.next >= static_cast<int>(n);
    std::vector<std::uint8_t> out;
    out.reserve(9 + 3 * n + (pairs ? n * (n - 1) / 2 : 0));
    unsigned sum = 0;
    for (int g : p.enc) sum += static_cast<unsigned>(p.anchors[g].x * 31 + p.anchors[g].y);
    out.push_back('S');
    out.push_back('P');
    out.push_back(kPlanVersion);
    out.push_back(static_cast<std::uint8_t>(n));
    out.push_back(static_cast<std::uint8_t>(p.w));
    out.push_back(static_cast<std::uint8_t>(p.h));
    out.push_back(static_cast<std::uint8_t>(sum & 0xFF));
    out.push_back(static_cast<std::uint8_t>((sum >> 8) & 0xFF));
    out.push_back(pairs ? kHasPairs : 0);
    for (int g : p.enc) out.push_back(static_cast<std::uint8_t>(g));
    for (std::size_t k = 0; k < n; ++k) out.push_back(half(p.home[k]));
    for (std::size_t k = 0; k < n; ++k) out.push_back(half(p.enemy[k]));
    if (pairs)
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = i + 1; j < n; ++j) {
                // Either direction measured; the shorter wins.
                out.push_back(half(std::min(p.d[i * n + j], p.d[j * n + i])));
            }
    bc::draft_memory = out;
}

}  // namespace

void plan_in_draft() {
    if (!kDraftPlan) return;
    auto& p = state();
    if (!p.started) setup(p);
    const std::size_t n = p.enc.size();
    if (n == 0) return;
    const std::uint64_t estimate = static_cast<std::uint64_t>(p.w) * p.h * 3 + 60 * n + 5000;
    bool changed = false;
    while (p.spawns_done < 2 && bc::fuel_remaining() > kDraftReserve + estimate) {
        search_spawn(p, p.spawns_done == 0);
        ++p.spawns_done;
        changed = true;
    }
    while (kRoutePlan && p.spawns_done == 2 && p.next < static_cast<int>(n) && bc::fuel_remaining() > kDraftReserve + estimate) {
        search_from(p, p.next);
        ++p.next;
        changed = true;
    }
    // Written once both spawns are measured (and again when the pairs are complete).
    if (changed && p.spawns_done == 2 && (!kRoutePlan || p.next == 0 || p.next >= static_cast<int>(n))) write_note(p);
}

const std::vector<int>& RoutePlan::nearest(int a, int count) {
    auto& list = near[a];
    if (!list.empty() || !ok) return list;
    std::vector<std::pair<int, int>> all;
    for (int b = 0; b < groups; ++b)
        if (b != a && between(a, b) != kUnreached) all.push_back({between(a, b), b});
    const int k = std::min<int>(count, static_cast<int>(all.size()));
    std::partial_sort(all.begin(), all.begin() + k, all.end());
    for (int i = 0; i < k; ++i) list.push_back(all[i].second);
    return list;
}

bool RoutePlan::load(int w, int h, const std::vector<vec2>& anchors) {
    ok = pairs = false;
    groups = static_cast<int>(anchors.size());
    const auto& note = bc::draft_memory;
    const auto* b = reinterpret_cast<const std::uint8_t*>(note.data());
    const std::size_t size = note.size();
    if (size < 9 || b[0] != 'S' || b[1] != 'P' || b[2] != kPlanVersion) return false;
    const int n = b[3];
    const bool has_pairs = (b[8] & kHasPairs) != 0;
    if (b[4] != static_cast<std::uint8_t>(w) || b[5] != static_cast<std::uint8_t>(h) || n > groups) return false;
    if (size < 9 + 3 * static_cast<std::size_t>(n) + (has_pairs ? static_cast<std::size_t>(n) * (n - 1) / 2 : 0)) return false;
    std::vector<int> ids(n);
    unsigned sum = 0;
    for (int k = 0; k < n; ++k) {
        ids[k] = b[9 + k];
        if (ids[k] >= groups) return false;
        sum += static_cast<unsigned>(anchors[ids[k]].x * 31 + anchors[ids[k]].y);
    }
    if (b[6] != static_cast<std::uint8_t>(sum & 0xFF) || b[7] != static_cast<std::uint8_t>((sum >> 8) & 0xFF)) return false;
    auto tiles = [](std::uint8_t v) { return v == 255 ? kUnreached : static_cast<std::uint16_t>(v * 2); };
    home.assign(groups, kUnreached);
    enemy.assign(groups, kUnreached);
    near.assign(groups, {});
    std::size_t at = 9 + static_cast<std::size_t>(n);
    for (int k = 0; k < n; ++k) home[ids[k]] = tiles(b[at + k]);
    at += n;
    for (int k = 0; k < n; ++k) enemy[ids[k]] = tiles(b[at + k]);
    at += n;
    dist.clear();
    if (has_pairs) {
        dist.assign(static_cast<std::size_t>(groups) * groups, kUnreached);
        for (int i = 0; i < n; ++i) {
            dist[static_cast<std::size_t>(ids[i]) * groups + ids[i]] = 0;
            for (int j = i + 1; j < n; ++j, ++at) {
                const auto v = tiles(b[at]);
                dist[static_cast<std::size_t>(ids[i]) * groups + ids[j]] = v;
                dist[static_cast<std::size_t>(ids[j]) * groups + ids[i]] = v;
            }
        }
        pairs = true;
    }
    ok = true;
    return true;
}
