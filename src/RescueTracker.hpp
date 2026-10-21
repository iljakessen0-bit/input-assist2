#pragma once

#include <cstdint>
#include <optional>

// Keeps an early jump press alive for a short window, so a click that came a few
// ticks too soon still counts once it can do something. Holds no game types, so it
// can be unit-tested on its own (see tests/rescue_test.cpp).
//
// One tracker covers one player's jump button. A press "arms" the tracker for
// `window` ticks. While armed:
//  - a release is held back until the press has been used (the player jumped, flipped
//    or fired an orb) or the window runs out;
//  - if the player enters an orb, the press is sent again so the orb fires.
// Presses in modes that act on a press instantly (ship, UFO, wave, swing) never arm it.
class RescueTracker {
public:
    enum class SaveKind {
        // A release came early and was held until the jump happened.
        Landing,
        // A press came before the orb and was sent again on reaching it.
        Orb,
    };

    struct Save {
        int64_t ticks;
        SaveKind kind;
    };

    // What the game saw during the last tick, gathered before the next one starts.
    struct Observation {
        // The press did something: a jump, a flip, an orb, or a switch to a mode where presses act instantly.
        bool pressUsed = false;
        // The player started touching an orb.
        bool enteredOrb = false;
    };

    // Why the window closed on this step, if it did.
    enum class End {
        None,
        Used,
        Expired,
    };

    struct StepResult {
        bool deliverRelease = false;
        // Send a release then a press, so the game sees a fresh click.
        bool rePress = false;
        std::optional<Save> save;
        End end = End::None;
    };

    // A press reached the game on `tick`. A window of 0 (assist off) doesn't arm the tracker.
    void onPress(int64_t tick, int64_t window, bool actsInstantly) {
        *this = RescueTracker();
        if (window <= 0 || actsInstantly) return;
        m_armed = true;
        m_pressTick = tick;
        m_window = window;
    }

    // A release is about to reach the game on `tick`. Returns true if it's held back instead.
    bool onRelease(int64_t tick) {
        if (!m_armed || tick >= m_pressTick + m_window) return false;
        m_holdingRelease = true;
        m_releaseTick = tick;
        return true;
    }

    // Before a new press reaches the game: returns true if a held-back release must go first.
    bool takeHeldRelease() {
        bool holding = m_holdingRelease;
        m_holdingRelease = false;
        return holding;
    }

    // Call at the start of every tick with what happened during the previous one.
    StepResult step(int64_t tick, Observation observation) {
        StepResult result;
        if (!m_armed) return result;

        if (observation.pressUsed) {
            result.deliverRelease = m_holdingRelease;
            // Only a release we held mattered; if the player was still holding, vanilla would have done the same.
            if (m_holdingRelease && !m_rePressed) {
                result.save = Save{ tick - m_releaseTick, SaveKind::Landing };
            }
            result.end = End::Used;
            *this = RescueTracker();
            return result;
        }

        if (tick >= m_pressTick + m_window) {
            result.deliverRelease = m_holdingRelease;
            result.end = End::Expired;
            *this = RescueTracker();
            return result;
        }

        // Entering an orb on the press's own tick needs no help: that press is already fresh.
        if (observation.enteredOrb && !m_rePressed && tick > m_pressTick) {
            m_rePressed = true;
            result.rePress = true;
            result.save = Save{ tick - m_pressTick, SaveKind::Orb };
        }
        return result;
    }

    // Drops all state (level reset, pause). Returns true if a held-back release must be delivered.
    bool flush() {
        bool holding = m_holdingRelease;
        *this = RescueTracker();
        return holding;
    }

    bool armed() const {
        return m_armed;
    }

private:
    bool m_armed = false;
    bool m_holdingRelease = false;
    bool m_rePressed = false;
    int64_t m_pressTick = 0;
    int64_t m_releaseTick = 0;
    int64_t m_window = 0;
};
