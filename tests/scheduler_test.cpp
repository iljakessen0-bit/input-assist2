// Host-side tests for InputScheduler. No Geode or GD needed:
//   g++ -std=c++20 -Wall -Wextra -Isrc tests/scheduler_test.cpp -o scheduler_test && ./scheduler_test

#include "InputScheduler.hpp"

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

    constexpr int kJump = 1;
    constexpr int kLeft = 2;

    struct Applied {
        bool down;
        int button;
        bool isPlayer1;
        int64_t tick;
    };

    // Stand-in for the game loop: drives the scheduler the same way the hooks in
    // main.cpp do, and records the tick each input actually reached the player on.
    struct Sim {
        InputScheduler scheduler;
        int64_t delay = 0;
        int64_t buffer = 0;
        std::vector<Applied> applied;

        // AssistLayer::processCommands, up to where vanilla's own step code runs.
        void beginTick() {
            scheduler.startTick();
            while (auto input = scheduler.popDue(false)) {
                applied.push_back({ input->down, input->button, input->isPlayer1, scheduler.tick() });
            }
            scheduler.setInStep(true);
        }
        void endTick() {
            scheduler.setInStep(false);
        }
        // The tick an input applied right now takes effect on: an input applied
        // between ticks first affects the player on the next one.
        int64_t effectiveTick() const {
            return scheduler.inStep() ? scheduler.tick() : scheduler.tick() + 1;
        }
        // AssistLayer::handleButton.
        void input(bool down, int button = kJump, bool isPlayer1 = true) {
            if (scheduler.schedule(down, button, isPlayer1, delay, buffer)) {
                applied.push_back({ down, button, isPlayer1, effectiveTick() });
            }
        }
        // Runs ticks until `tick` has begun (inputs sent after this land on that tick).
        void runTo(int64_t tick) {
            while (scheduler.tick() < tick) {
                if (scheduler.inStep()) endTick();
                beginTick();
            }
        }
        // AssistPlayLayer::resetLevel / pauseGame.
        void flush() {
            while (auto input = scheduler.popDue(true)) {
                applied.push_back({ input->down, input->button, input->isPlayer1, effectiveTick() });
            }
        }
    };

    void noAssistIsVanilla() {
        Sim sim;
        sim.runTo(1);
        sim.input(true);
        sim.runTo(3);
        sim.input(false);
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[0].tick == 1 && sim.applied[0].down);
        CHECK(sim.applied[1].tick == 3 && !sim.applied[1].down);
        CHECK(!sim.scheduler.hasPending());
    }

    void delayShiftsBothEdges() {
        Sim sim;
        sim.delay = 2;
        sim.runTo(1);
        sim.input(true);
        sim.runTo(4);
        sim.input(false);
        sim.runTo(10);
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[0].tick == 3);
        CHECK(sim.applied[1].tick == 6);
        // Hold length (3 ticks) is unchanged.
        CHECK(sim.applied[1].tick - sim.applied[0].tick == 3);
    }

    void oneTickDelayIsExactlyOneTick() {
        Sim sim;
        sim.delay = 1;
        sim.runTo(7);
        sim.input(true);
        CHECK(sim.applied.empty());
        sim.runTo(8);
        CHECK(sim.applied.size() == 1 && sim.applied[0].tick == 8);
    }

    void bufferExtendsShortTap() {
        Sim sim;
        sim.buffer = 3;
        sim.runTo(5);
        sim.input(true);
        sim.runTo(6);
        sim.input(false);
        sim.runTo(10);
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[0].tick == 5);
        CHECK(sim.applied[1].tick == 8);
    }

    void bufferLeavesLongHoldsAlone() {
        Sim sim;
        sim.buffer = 3;
        sim.runTo(5);
        sim.input(true);
        sim.runTo(20);
        sim.input(false);
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[1].tick == 20);
        CHECK(!sim.scheduler.hasPending());
    }

    void delayAndBufferCombine() {
        Sim sim;
        sim.delay = 1;
        sim.buffer = 3;
        sim.runTo(5);
        sim.input(true);
        sim.input(false); // a same-tick tap
        sim.runTo(12);
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[0].tick == 6 && sim.applied[0].down);
        CHECK(sim.applied[1].tick == 9 && !sim.applied[1].down);
    }

    void releaseNeverOvertakenByNextPress() {
        Sim sim;
        sim.buffer = 5;
        sim.runTo(1);
        sim.input(true);
        sim.runTo(2);
        sim.input(false); // held back to tick 6
        sim.runTo(3);
        sim.input(true); // must land after that release
        sim.runTo(10);
        CHECK(sim.applied.size() == 3);
        CHECK(sim.applied[0].down && sim.applied[0].tick == 1);
        CHECK(!sim.applied[1].down && sim.applied[1].tick == 6);
        CHECK(sim.applied[2].down && sim.applied[2].tick == 6);
    }

    void inputsBetweenTicks() {
        Sim sim;
        sim.delay = 1;
        sim.runTo(3);
        sim.endTick();
        sim.input(true); // vanilla would apply this on tick 4
        sim.runTo(4);
        CHECK(sim.applied.empty());
        sim.runTo(5);
        CHECK(sim.applied.size() == 1 && sim.applied[0].tick == 5);

        Sim vanilla;
        vanilla.runTo(3);
        vanilla.endTick();
        vanilla.input(true);
        CHECK(vanilla.applied.size() == 1 && vanilla.applied[0].tick == 4);
    }

    void buttonsAreIndependent() {
        Sim sim;
        sim.buffer = 5;
        sim.runTo(1);
        sim.input(true, kJump);
        sim.runTo(2);
        sim.input(false, kJump); // held back to tick 6
        sim.runTo(3);
        sim.input(true, kLeft); // unrelated button, not blocked
        sim.input(true, kJump, false); // player 2's jump, not blocked
        CHECK(sim.applied.size() == 3);
        CHECK(sim.applied[1].button == kLeft && sim.applied[1].tick == 3);
        CHECK(!sim.applied[2].isPlayer1 && sim.applied[2].tick == 3);
    }

    void flushDeliversEverythingInOrder() {
        Sim sim;
        sim.delay = 10;
        sim.runTo(1);
        sim.input(true);
        sim.runTo(2);
        sim.input(false);
        sim.flush();
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[0].down && !sim.applied[1].down);
        CHECK(!sim.scheduler.hasPending());
    }

    void turningAssistOffKeepsOrder() {
        Sim sim;
        sim.delay = 3;
        sim.runTo(1);
        sim.input(true); // due tick 4
        sim.delay = 0;  // setting switched off mid-level
        sim.runTo(2);
        sim.input(false); // must still wait for the press
        sim.runTo(5);
        CHECK(sim.applied.size() == 2);
        CHECK(sim.applied[0].down && sim.applied[0].tick == 4);
        CHECK(!sim.applied[1].down && sim.applied[1].tick == 4);
        sim.runTo(10);
        sim.input(true); // back to vanilla timing
        CHECK(sim.applied.size() == 3 && sim.applied[2].tick == 10);
    }

    void unknownButtonsAreRejected() {
        CHECK(InputScheduler::keyIndex(0, true) == -1);
        CHECK(InputScheduler::keyIndex(4, true) == -1);
        CHECK(InputScheduler::keyIndex(1, true) == 0);
        CHECK(InputScheduler::keyIndex(3, false) == 5);
    }

    // Random play with settings changing on the fly. Whatever happens, every input
    // must reach the player exactly once, in order per button, never earlier than
    // vanilla would apply it, and never later than delay + buffer allow.
    void randomisedInvariants() {
        std::mt19937 rng(1234);
        for (int round = 0; round < 2000; ++round) {
            Sim sim;
            struct Sent {
                bool down;
                int key;
                int64_t baseTick;
            };
            std::vector<Sent> sent;
            std::array<bool, InputScheduler::kKeyCount> held{};

            for (int64_t tick = 1; tick <= 200; ++tick) {
                sim.runTo(tick);
                if (rng() % 20 == 0) {
                    sim.delay = rng() % 4;
                    sim.buffer = rng() % 6;
                }
                if (rng() % 7 == 0) sim.endTick(); // input arriving between ticks
                int inputs = rng() % 3;
                for (int i = 0; i < inputs; ++i) {
                    int button = 1 + rng() % 3;
                    bool isPlayer1 = rng() % 4 != 0;
                    int key = InputScheduler::keyIndex(button, isPlayer1);
                    bool down = !held[key];
                    held[key] = down;
                    int64_t base = sim.scheduler.inStep() ? sim.scheduler.tick() : sim.scheduler.tick() + 1;
                    sent.push_back({ down, key, base });
                    sim.input(down, button, isPlayer1);
                }
                if (rng() % 50 == 0) sim.flush(); // a reset or pause
            }
            sim.flush();

            CHECK(sim.applied.size() == sent.size());
            if (sim.applied.size() != sent.size()) return;

            // Match inputs to deliveries per key; each key's deliveries must replay its inputs in order.
            std::array<std::vector<Sent>, InputScheduler::kKeyCount> sentPerKey;
            std::array<std::vector<Applied>, InputScheduler::kKeyCount> appliedPerKey;
            for (auto& s : sent) sentPerKey[s.key].push_back(s);
            for (auto& a : sim.applied) appliedPerKey[InputScheduler::keyIndex(a.button, a.isPlayer1)].push_back(a);

            for (int key = 0; key < InputScheduler::kKeyCount; ++key) {
                auto& s = sentPerKey[key];
                auto& a = appliedPerKey[key];
                CHECK(s.size() == a.size());
                if (s.size() != a.size()) return;
                for (size_t i = 0; i < s.size(); ++i) {
                    CHECK(s[i].down == a[i].down);
                    CHECK(a[i].tick >= s[i].baseTick);
                    if (i > 0) CHECK(a[i].tick >= a[i - 1].tick);
                }
            }
            if (g_failures) {
                std::printf("  (round %d)\n", round);
                return;
            }
        }
    }
}

int main() {
    struct Test {
        char const* name;
        void (*fn)();
    };
    Test tests[] = {
        { "no assist is vanilla", noAssistIsVanilla },
        { "delay shifts both edges", delayShiftsBothEdges },
        { "1-tick delay is exactly one tick", oneTickDelayIsExactlyOneTick },
        { "buffer extends a short tap", bufferExtendsShortTap },
        { "buffer leaves long holds alone", bufferLeavesLongHoldsAlone },
        { "delay and buffer combine", delayAndBufferCombine },
        { "release never overtaken by next press", releaseNeverOvertakenByNextPress },
        { "inputs between ticks", inputsBetweenTicks },
        { "buttons are independent", buttonsAreIndependent },
        { "flush delivers everything in order", flushDeliversEverythingInOrder },
        { "turning assist off keeps order", turningAssistOffKeepsOrder },
        { "unknown buttons are rejected", unknownButtonsAreRejected },
        { "randomised invariants (2000 rounds)", randomisedInvariants },
    };
    for (auto& test : tests) {
        int before = g_failures;
        test.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", test.name);
    }
    std::printf("%s\n", g_failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return g_failures ? 1 : 0;
}
