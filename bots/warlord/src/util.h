#pragma once
// Small helpers shared by every module. Host calls cost fuel, so hot lookups
// go through local copies of engine bitboards instead of bitboard::is_set.
#include <cambc.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bc = cambc;
using bc::team;
using bc::vec2;

inline int cheb(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }

inline constexpr std::uint16_t kUnreached = 65535;

inline constexpr vec2 kDirs[8] = {
    {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
};

// Row-major, least-significant-bit-first copy of a bitboard.
struct BitGrid {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bits;

    void load(const bc::bitboard& board, int width, int height) {
        w = width;
        h = height;
        bits.resize(bc::bitboard::linear_buffer_size());
        board.export_linear_buffer(bits);
    }
    void clear(int width, int height) {
        w = width;
        h = height;
        bits.assign(bc::bitboard::linear_buffer_size(), 0);
    }
    bool in(vec2 p) const { return p.x >= 0 && p.y >= 0 && p.x < w && p.y < h; }
    bool get(vec2 p) const {
        if (!in(p)) return false;
        const int i = p.y * w + p.x;
        return (bits[i >> 3] >> (i & 7)) & 1;
    }
    void set(vec2 p, bool v = true) {
        if (!in(p)) return;
        const int i = p.y * w + p.x;
        if (v) bits[i >> 3] |= std::uint8_t(1u << (i & 7));
        else bits[i >> 3] &= std::uint8_t(~(1u << (i & 7)));
    }
};

// Where the current turn is, for error reports.
inline const char* g_phase = "";

// Log only in debug builds; release builds (and the platform) skip it for free.
#define LOG(...) debug_block({ std::cout << __VA_ARGS__ << '\n'; })
