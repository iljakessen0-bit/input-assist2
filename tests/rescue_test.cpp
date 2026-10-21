// Host-side tests for RescueTracker. No Geode or GD needed:
//   g++ -std=c++20 -Wall -Wextra -Isrc tests/rescue_test.cpp -o rescue_test && ./rescue_test

#include "RescueTracker.hpp"

#include <cstdio>
#include <random>
#include <vector>

namespace {
    int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

    using Obs = RescueTracker::Observation;
    using Kind = RescueTracker::SaveKind;
    using End = RescueTracker::End;

    struct Delivery {
        bool down;
        int64_t tick;
    };

    // Drives one tracker the way main.cpp does and records what reaches the game.
    struct Sim {
        RescueTracker tracker;
        int64_t window = 4;
        std::vector<Delivery> game;
        std::vector<RescueTracker::Save> saves;
        std::vector<End> ends;

        void step(int64_t tick, Obs obs = {}) {
            auto result = tracker.step(tick, obs);
            if (result.rePress) {
                game.push_back({ false, tick });
                game.push_back({ true, tick });
            }
            if (result.deliverRelease) game.push_back({ false, tick });
            if (result.save) saves.push_back(*result.save);
            ends.push_back(result.end);
        }
        void press(int64_t tick, bool actsInstantly = false) {
            if (tracker.takeHeldRelease()) game.push_back({ false, tick });
            game.push_back({ true, tick });
            tracker.onPress(tick, window, actsInstantly);
        }
        void release(int64_t tick) {
            if (!tracker.onRelease(tick)) game.push_back({ false, tick });
        }
    };

    void offNeverHolds() {
        Sim sim;
        sim.window = 0;
        sim.press(10);
        sim.release(11);
        CHECK(sim.game.size() == 2);
        CHECK(!sim.game[1].down && sim.game[1].tick == 11);
        CHECK(!sim.tracker.armed());
    }

    void earlyTapHeldUntilLanding() {
        Sim sim;
        sim.press(10);
        sim.release(11);
        CHECK(sim.game.size() == 1); // release held back
        sim.step(12);
        CHECK(sim.game.size() == 1);
        sim.step(13, { .pressUsed = true }); // landed and jumped during tick 12
        CHECK(sim.game.size() == 2);
        CHECK(!sim.game[1].down && sim.game[1].tick == 13);
        CHECK(sim.saves.size() == 1);
        CHECK(sim.saves[0].kind == Kind::Landing && sim.saves[0].ticks == 2);
        CHECK(sim.ends.back() == End::Used);
        CHECK(!sim.tracker.armed());
    }

    void heldReleaseExpiresWithoutSave() {
        Sim sim;
        sim.press(10);
        sim.release(11);
        for (int64_t t = 11; t < 14; ++t) sim.step(t);
        CHECK(sim.game.size() == 1);
        sim.step(14); // press tick + window
        CHECK(sim.game.size() == 2 && !sim.game[1].down && sim.game[1].tick == 14);
        CHECK(sim.saves.empty());
        CHECK(sim.ends.back() == End::Expired);
    }

    void realHoldIsLeftAlone() {
        Sim sim;
        sim.press(10);
        sim.step(11, { .pressUsed = true });
        CHECK(!sim.tracker.armed());
        sim.release(15);
        CHECK(sim.game.size() == 2 && sim.game[1].tick == 15);
        CHECK(sim.saves.empty());
    }

    void releaseAfterWindowGoesStraightThrough() {
        Sim sim;
        sim.press(10);
        sim.release(14); // window is 4: already over
        CHECK(sim.game.size() == 2 && sim.game[1].tick == 14);
    }

    void orbReachedAfterEarlyPress() {
        Sim sim;
        sim.press(10);
        sim.release(11);
        sim.step(12, { .enteredOrb = true });
        CHECK(sim.ends.back() == End::None); // still open: waiting for the orb to fire
        // Release + press so the game sees a fresh click on the orb.
        CHECK(sim.game.size() == 3);
        CHECK(!sim.game[1].down && sim.game[2].down && sim.game[2].tick == 12);
        CHECK(sim.saves.size() == 1 && sim.saves[0].kind == Kind::Orb && sim.saves[0].ticks == 2);
        sim.step(13, { .pressUsed = true }); // the orb fired
        CHECK(sim.game.size() == 4 && !sim.game[3].down); // the player's release, finally
        CHECK(sim.saves.size() == 1); // no second save for the same click
    }

    void orbOnPressTickNeedsNoHelp() {
        Sim sim;
        sim.press(10);
        sim.step(10, { .enteredOrb = true });
        CHECK(sim.game.size() == 1);
        CHECK(sim.saves.empty());
    }

    void orbRePressHappensOnce() {
        Sim sim;
        sim.press(10);
        sim.step(11, { .enteredOrb = true });
        sim.step(12, { .enteredOrb = true });
        int presses = 0;
        for (auto& d : sim.game) presses += d.down;
        CHECK(presses == 2);
    }

    void flightModesNeverArm() {
        Sim sim;
        sim.press(10, true);
        sim.release(11);
        CHECK(sim.game.size() == 2 && sim.game[1].tick == 11);
        CHECK(!sim.tracker.armed());
    }

    void newPressSendsHeldReleaseFirst() {
        Sim sim;
        sim.press(10);
        sim.release(11);
        sim.press(12);
        CHECK(sim.game.size() == 3);
        CHECK(sim.game[0].down && !sim.game[1].down && sim.game[2].down);
        CHECK(sim.tracker.armed());
    }

    void flushReturnsHeldRelease() {
        Sim sim;
        sim.press(10);
        sim.release(11);
        CHECK(sim.tracker.flush());
        CHECK(!sim.tracker.armed());
        CHECK(!sim.tracker.flush());
    }

    // Random play: whatever the timing, the game sees press and release alternate, a
    // held release never outlives the window, and the game's button ends up matching
    // the player's.
    void randomisedInvariants() {
        std::mt19937 rng(99);
        for (int round = 0; round < 5000; ++round) {
            Sim sim;
            sim.window = 1 + rng() % 16;
            bool userHeld = false;
            int64_t lastUserPress = -1000;

            for (int64_t tick = 1; tick <= 300; ++tick) {
                Obs obs;
                obs.pressUsed = rng() % 6 == 0;
                obs.enteredOrb = rng() % 9 == 0;
                sim.step(tick, obs);

                if (rng() % 3 == 0) {
                    userHeld = !userHeld;
                    if (userHeld) {
                        sim.press(tick, rng() % 5 == 0);
                        lastUserPress = tick;
                    }
                    else {
                        sim.release(tick);
                    }
                }
                if (rng() % 40 == 0 && sim.tracker.flush()) sim.game.push_back({ false, tick });

                // Replay deliveries so far: alternate, and track the game's state.
                bool gameHeld = false;
                for (auto& d : sim.game) {
                    CHECK(d.down != gameHeld);
                    gameHeld = d.down;
                }
                if (gameHeld && !userHeld) {
                    // Holding on the player's behalf only inside the window after their press.
                    CHECK(tick - lastUserPress < sim.window);
                }
                if (!gameHeld) CHECK(!userHeld);
                if (g_failures) {
                    std::printf("  (round %d, tick %lld)\n", round, static_cast<long long>(tick));
                    return;
                }
            }
            if (sim.tracker.flush()) sim.game.push_back({ false, 301 });
            bool gameHeld = !sim.game.empty() && sim.game.back().down;
            CHECK(gameHeld == userHeld);
            if (g_failures) return;
        }
    }
}

int main() {
    struct Test {
        char const* name;
        void (*fn)();
    };
    Test tests[] = {
        { "off never holds", offNeverHolds },
        { "early tap held until landing", earlyTapHeldUntilLanding },
        { "held release expires without a save", heldReleaseExpiresWithoutSave },
        { "a real hold is left alone", realHoldIsLeftAlone },
        { "release after the window goes straight through", releaseAfterWindowGoesStraightThrough },
        { "orb reached after an early press", orbReachedAfterEarlyPress },
        { "orb on the press tick needs no help", orbOnPressTickNeedsNoHelp },
        { "orb re-press happens once", orbRePressHappensOnce },
        { "flight modes never arm", flightModesNeverArm },
        { "new press sends held release first", newPressSendsHeldReleaseFirst },
        { "flush returns a held release", flushReturnsHeldRelease },
        { "randomised invariants (5000 rounds)", randomisedInvariants },
    };
    for (auto& test : tests) {
        int before = g_failures;
        test.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", test.name);
    }
    std::printf("%s\n", g_failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_failures ? 1 : 0;
}
