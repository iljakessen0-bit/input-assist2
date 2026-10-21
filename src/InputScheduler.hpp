#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// Decides which physics tick each input lands on. Holds no game types, so it can be
// unit-tested on its own (see tests/scheduler_test.cpp).
//
// A "tick" is one call to GJBaseGameLayer::processCommands (1/240 s of game time).
// Every input gets a base tick (the one vanilla would have applied it on) and a due
// tick (base + delay, pushed later by the buffer and by ordering). Inputs whose due
// tick equals their base tick go through immediately; the rest wait in a queue.
class InputScheduler {
public:
    struct Input {
        bool down;
        int button;
        bool isPlayer1;
        int64_t dueTick;
    };

    // Three buttons (jump, left, right) for each of two players.
    static constexpr int kKeyCount = 6;

    // Maps (button, player) to 0..5, or -1 for a button this scheduler doesn't handle.
    // Buttons follow GD's PlayerButton enum: 1 = jump, 2 = left, 3 = right.
    static int keyIndex(int button, bool isPlayer1) {
        if (button < 1 || button > 3) return -1;
        return (button - 1) + (isPlayer1 ? 0 : 3);
    }

    // Call at the start of every tick, before the game simulates it.
    void startTick() {
        ++m_tick;
    }

    // Whether the game is currently inside processCommands for the current tick.
    void setInStep(bool inStep) {
        m_inStep = inStep;
    }
    bool inStep() const {
        return m_inStep;
    }

    int64_t tick() const {
        return m_tick;
    }

    bool hasPending() const {
        return !m_pending.empty();
    }

    // Schedules an input for a known button (keyIndex >= 0). delay moves it later;
    // buffer is the minimum number of ticks a press is held before its release lands.
    // Returns true if the caller should apply the input right now (exactly vanilla
    // timing), or false if it was queued for popDue().
    bool schedule(bool down, int button, bool isPlayer1, int64_t delay, int64_t buffer) {
        int key = keyIndex(button, isPlayer1);

        // The tick vanilla would have applied this input on: the current one when it
        // arrives from inside processCommands, otherwise the next one to run.
        int64_t base = m_inStep ? m_tick : m_tick + 1;
        int64_t due = base + delay;
        if (!down) {
            due = std::max(due, m_lastPress[key] + buffer);
        }
        // An input may never overtake an earlier one for the same button, or a
        // press/release pair could swap and leave the button stuck.
        due = std::max(due, m_lastDue[key]);

        m_lastDue[key] = due;
        if (down) m_lastPress[key] = due;

        if (due == base && m_pendingCount[key] == 0) {
            return true;
        }
        m_pending.push_back({ down, button, isPlayer1, due });
        ++m_pendingCount[key];
        return false;
    }

    // Removes and returns the oldest queued input that is due by now (or the oldest
    // queued input at all, if all is set). Per-button due ticks never decrease, so
    // taking the first ready entry keeps each button's inputs in arrival order.
    std::optional<Input> popDue(bool all) {
        auto it = std::find_if(m_pending.begin(), m_pending.end(), [&](Input const& input) {
            return all || input.dueTick <= m_tick;
        });
        if (it == m_pending.end()) return std::nullopt;

        Input input = *it;
        m_pending.erase(it);
        --m_pendingCount[keyIndex(input.button, input.isPlayer1)];
        return input;
    }

private:
    // Tick value meaning "never happened"; far enough from 0 that adding a buffer can't overflow.
    static constexpr int64_t kNever = std::numeric_limits<int64_t>::min() / 4;

    std::vector<Input> m_pending;
    std::array<int, kKeyCount> m_pendingCount{};
    // Due tick of the most recent input / press scheduled for each key.
    std::array<int64_t, kKeyCount> m_lastDue{ kNever, kNever, kNever, kNever, kNever, kNever };
    std::array<int64_t, kKeyCount> m_lastPress{ kNever, kNever, kNever, kNever, kNever, kNever };
    int64_t m_tick = 0;
    bool m_inStep = false;
};
