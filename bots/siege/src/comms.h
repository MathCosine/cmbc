#pragma once
// Team memory layout (8 bytes, shared by every teammate, invisible to the enemy):
//   byte 0      squad target group + 1 (0 = none)
//   byte 1      team item multiplier: live damage over base damage, in 1/32 steps
//   bytes 2..7  one byte per champion slot: task << 6 | (target + 1); with five
//               champions byte 7 instead holds the enemy everyone focuses (id + 1)
// A champion's slot is its index in champions(team::ally), which is the same list for everyone.
#include "util.h"
#include <array>
#include <span>

enum class Task : std::uint8_t { farm = 0, capture = 1, solo = 2, other = 3 };

// Targets used with Task::other.
inline constexpr int kTribute = 0;
inline constexpr int kHome = 1;
inline constexpr int kDead = 2;
inline constexpr int kFight = 3;  // in a fight with enemy champions: teammates may come and help
inline constexpr int kBagFight = 4;  // going after a bag carrier near an altar: teammates come from further

inline constexpr int kMaxSlots = 6;
inline constexpr int kMaxTarget = 62;

struct Slot {
    Task task;
    int target;  // -1 when unset
};

class Comms {
public:
    void read() { bc::shared_memory.read(0, std::span{bytes_}); }

    int squad_target() const { return static_cast<int>(bytes_[0]) - 1; }
    void set_squad_target(int group) {
        if (group > kMaxTarget) group = -1;
        const std::uint8_t value = static_cast<std::uint8_t>(group + 1);
        if (bytes_[0] == value) return;
        bytes_[0] = value;
        bc::shared_memory.write(0, std::span{bytes_.data(), 1});
    }

    // Every champion nudges the shared multiplier toward its own, so it tracks the team average.
    float power() const { return bytes_[1] == 0 ? 1.f : bytes_[1] / 32.f; }
    void blend_power(float mine) {
        const float blended = (power() * 5 + std::clamp(mine, 1.f, 7.9f)) / 6;
        const std::uint8_t value = static_cast<std::uint8_t>(std::clamp(blended * 32 + 0.5f, 32.f, 255.f));
        if (bytes_[1] == value) return;
        bytes_[1] = value;
        bc::shared_memory.write(1, std::span{bytes_.data() + 1, 1});
    }

    // Byte 7 is free with five champions: the enemy everyone should hit (id + 1, 0 = none).
    int focus() const { return static_cast<int>(bytes_[7]) - 1; }
    void set_focus(int id) {
        const std::uint8_t value = static_cast<std::uint8_t>(std::clamp(id + 1, 0, 255));
        if (bytes_[7] == value) return;
        bytes_[7] = value;
        bc::shared_memory.write(7, std::span{bytes_.data() + 7, 1});
    }

    Slot slot(int index) const {
        const std::uint8_t b = bytes_[2 + index];
        return {static_cast<Task>(b >> 6), static_cast<int>(b & 63) - 1};
    }
    void set_slot(int index, Task task, int target) {
        if (index < 0 || index >= kMaxSlots) return;
        if (target > kMaxTarget) target = -1;
        const std::uint8_t value = static_cast<std::uint8_t>(static_cast<int>(task) << 6 | (target + 1));
        if (bytes_[2 + index] == value) return;
        bytes_[2 + index] = value;
        bc::shared_memory.write(2 + index, std::span{bytes_.data() + 2 + index, 1});
    }

    // Whether another slot claims a group with the given task.
    bool claimed_by_other(int self, Task task, int group, int slots) const { return claims_by_others(self, task, group, slots) > 0; }
    // Number of other slots claiming a group with the given task.
    int claims_by_others(int self, Task task, int group, int slots) const {
        int n = 0;
        for (int i = 0; i < slots && i < kMaxSlots; ++i)
            if (i != self && slot(i).task == task && slot(i).target == group) ++n;
        return n;
    }

private:
    std::array<std::uint8_t, 8> bytes_{};
};
