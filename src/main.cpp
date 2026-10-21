/*
 * Input Assist: early-click rescue for Geometry Dash 2.2081.
 *
 * How it works
 * ------------
 * GD 2.2 simulates physics in fixed ticks of 1/240 s. GJBaseGameLayer::processCommands()
 * runs once per tick, and the clicks collected since the previous tick reach the player
 * through GJBaseGameLayer::handleButton().
 *
 * Every jump input passes through two stages on its way to the game:
 *
 *  1. InputScheduler (optional fixed Click Delay): moves inputs a fixed number of ticks later.
 *  2. RescueTracker (the Rescue Stage): keeps an early jump press alive for up to N ticks.
 *     If you let go before the press did anything, your release is held back until the
 *     player jumps, flips or fires an orb, or until the window ends. If you reach an orb
 *     inside the window, the press is sent again so the orb fires.
 *
 * Whether a press "did something" is read from the player at the start of each tick,
 * by comparing it with a snapshot from the start of the previous one: it left the ground
 * moving upward without a pad launching it, a ball or spider flipped from the ground, an
 * orb or dash orb fired (PlayerObject::ringJump / startDashing), or the player is in a mode
 * where presses act instantly (ship, UFO, wave, swing). Presses in those modes are never held.
 *
 * F6 (rebindable) cycles the stage. Each save is logged to Geode's console and shown on the HUD.
 */

#include "InputScheduler.hpp"
#include "RescueTracker.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/UILayer.hpp>

#include <array>
#include <string_view>
#include <utility>

using namespace geode::prelude;

namespace {
    constexpr int64_t kMaxDelayTicks = 24;
    constexpr int kJump = static_cast<int>(PlayerButton::Jump);

    struct Stage {
        // Exactly as listed in the "stage" setting's one-of.
        std::string_view name;
        // Short form for the HUD and toasts.
        std::string_view label;
        int64_t ticks;
    };
    constexpr std::array<Stage, 5> kStages{ {
        { "Off", "Off", 0 },
        { "Stage 1 (+2 ticks)", "Stage 1 (+2t)", 2 },
        { "Stage 2 (+4 ticks)", "Stage 2 (+4t)", 4 },
        { "Stage 3 (+8 ticks)", "Stage 3 (+8t)", 8 },
        { "Stage 4 (+16 ticks)", "Stage 4 (+16t)", 16 },
    } };
    constexpr size_t kDefaultStage = 1;

    struct Config {
        bool enabled = true;
        size_t stage = kDefaultStage;
        int64_t pressDelay = 0;
        bool practiceOnly = false;
        bool showIndicator = true;
        bool logSaves = true;
        bool detailedLog = false;
    };

    // Cached copy of the settings, so the per-tick path never touches the settings store.
    Config g_config;

    size_t stageIndex(std::string_view name) {
        for (size_t i = 0; i < kStages.size(); ++i) {
            if (kStages[i].name == name) return i;
        }
        return kDefaultStage;
    }

    void loadConfig() {
        auto mod = Mod::get();
        g_config.enabled = mod->getSettingValue<bool>("enabled");
        g_config.stage = stageIndex(mod->getSettingValue<std::string>("stage"));
        g_config.pressDelay = std::clamp<int64_t>(mod->getSettingValue<int64_t>("press-delay"), 0, kMaxDelayTicks);
        g_config.practiceOnly = mod->getSettingValue<bool>("practice-only");
        g_config.showIndicator = mod->getSettingValue<bool>("show-indicator");
        g_config.logSaves = mod->getSettingValue<bool>("log-saves");
        g_config.detailedLog = mod->getSettingValue<bool>("detailed-log");
    }

    void cycleStage() {
        auto& next = kStages[(g_config.stage + 1) % kStages.size()];
        // The settings listener picks this up and refreshes the HUD.
        Mod::get()->setSettingValue<std::string>("stage", std::string(next.name));
        Notification::create(fmt::format("Input Assist: {}", next.label), NotificationIcon::Info, 1.f)->show();
    }

    bool indicatorVisible() {
        return g_config.enabled && g_config.showIndicator && (kStages[g_config.stage].ticks > 0 || g_config.pressDelay > 0);
    }

    void refreshIndicator(UILayer* ui) {
        if (!ui) return;
        if (auto label = typeinfo_cast<CCLabelBMFont*>(ui->getChildByID("indicator"_spr))) {
            auto text = fmt::format("Input Assist: {}", kStages[g_config.stage].label);
            if (g_config.pressDelay > 0) text += fmt::format(", delay {}t", g_config.pressDelay);
            if (g_config.practiceOnly) text += " (practice)";
            label->setString(text.c_str());
            label->setVisible(indicatorVisible());
        }
        if (auto label = ui->getChildByID("last-save"_spr)) {
            label->setVisible(indicatorVisible());
        }
    }

    // Shows a save under the indicator for a moment, then fades it out.
    void flashSave(UILayer* ui, std::string const& text) {
        auto label = ui ? typeinfo_cast<CCLabelBMFont*>(ui->getChildByID("last-save"_spr)) : nullptr;
        if (!label) return;
        label->setString(text.c_str());
        label->stopAllActions();
        label->setOpacity(220);
        label->runAction(CCSequence::create(CCDelayTime::create(1.5f), CCFadeTo::create(0.5f, 0), nullptr));
    }

    // Modes where a press acts the moment it lands, so holding it back would change the flight.
    bool actsInstantly(PlayerObject* player) {
        return player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;
    }

    bool movingUp(PlayerObject* player) {
        return player->m_isUpsideDown ? player->m_yVelocity < 0 : player->m_yVelocity > 0;
    }

    unsigned touchingOrbCount(PlayerObject* player) {
        return player->m_touchingRings ? player->m_touchingRings->count() : 0;
    }

    char const* modeName(PlayerObject* player) {
        if (player->m_isShip) return "ship";
        if (player->m_isBird) return "ufo";
        if (player->m_isDart) return "wave";
        if (player->m_isSwing) return "swing";
        if (player->m_isBall) return "ball";
        if (player->m_isSpider) return "spider";
        if (player->m_isRobot) return "robot";
        return "cube";
    }

    // What the last tick did with a press, plus what used it for the detailed log.
    struct Seen {
        RescueTracker::Observation observation;
        char const* usedBy = nullptr;
    };

    // A player as it was at the start of a tick.
    struct PlayerSnapshot {
        bool valid = false;
        bool onGround = false;
        bool upsideDown = false;
        int orbUses = 0;
        int padBumps = 0;
        unsigned touchingOrbs = 0;
    };
}

class $modify(AssistPlayer, PlayerObject) {
    struct Fields {
        // Orbs and dash orbs this player has fired, and pads that launched it.
        int orbUses = 0;
        int padBumps = 0;
    };

    void ringJump(RingObject* object, bool skipCheck) {
        PlayerObject::ringJump(object, skipCheck);
        ++m_fields->orbUses;
    }

    void startDashing(DashRingObject* object) {
        PlayerObject::startDashing(object);
        ++m_fields->orbUses;
    }

    void bumpPlayer(float bumpMod, int objectType, bool noEffects, GameObject* object) {
        PlayerObject::bumpPlayer(bumpMod, objectType, noEffects, object);
        ++m_fields->padBumps;
    }
};

class $modify(AssistLayer, GJBaseGameLayer) {
    struct Fields {
        InputScheduler scheduler;
        // Rescue state for each side's jump button (0: player 1, 1: player 2 in two-player mode).
        std::array<RescueTracker, 2> rescue;
        // m_player1 and m_player2 as they were at the start of the previous tick.
        std::array<PlayerSnapshot, 2> snapshots;
        // True while we (or a reset/pause) are calling handleButton and it must not be intercepted.
        bool passthrough = false;
        // True while a reset/pause delivers everything still queued, with no rescue holding.
        bool settling = false;
    };

    static void onModify(auto& self) {
        // Run before other mods' handleButton hooks, so they see each input once,
        // at the moment it is actually applied, rather than also when it was clicked.
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
    }

    // Only real levels are assisted; the editor's playtest layer is left alone.
    bool isAssistedLayer() {
        return typeinfo_cast<PlayLayer*>(this) != nullptr;
    }

    bool assistActive() {
        return g_config.enabled && (!g_config.practiceOnly || m_isPracticeMode);
    }

    int64_t rescueWindow() {
        return this->assistActive() ? kStages[g_config.stage].ticks : 0;
    }

    // The players one side's jump input moves: in single-player dual mode one click drives both.
    std::array<PlayerObject*, 2> playersFor(bool isPlayer1) {
        bool twoPlayerMode = m_levelSettings && m_levelSettings->m_twoPlayerMode;
        if (!isPlayer1) return { m_player2, nullptr };
        if (m_gameState.m_isDualMode && !twoPlayerMode) return { m_player1, m_player2 };
        return { m_player1, nullptr };
    }

    bool pressActsInstantly(bool isPlayer1) {
        bool anyPlayer = false;
        for (auto player : this->playersFor(isPlayer1)) {
            if (!player) continue;
            anyPlayer = true;
            if (!actsInstantly(player)) return false;
        }
        return anyPlayer;
    }

    // Compares the players with their snapshots: what did the last tick do with a press?
    Seen observe(bool isPlayer1) {
        Seen seen;
        auto used = [&](char const* by) {
            seen.observation.pressUsed = true;
            if (!seen.usedBy) seen.usedBy = by;
        };
        for (auto player : this->playersFor(isPlayer1)) {
            if (!player) continue;
            if (actsInstantly(player)) used("flight mode");

            auto& before = m_fields->snapshots[player == m_player2 ? 1 : 0];
            if (!before.valid) continue;
            auto counters = static_cast<AssistPlayer*>(player)->m_fields.self();

            if (counters->orbUses != before.orbUses) used("orb");
            bool launchedByPad = counters->padBumps != before.padBumps;
            if (before.onGround && !player->m_isOnGround && !launchedByPad && movingUp(player)) used("jump");
            if ((player->m_isBall || player->m_isSpider) && before.onGround && player->m_isUpsideDown != before.upsideDown) {
                used("flip");
            }
            if (touchingOrbCount(player) > before.touchingOrbs) seen.observation.enteredOrb = true;
        }
        return seen;
    }

    void takeSnapshots() {
        std::array<PlayerObject*, 2> players{ m_player1, m_player2 };
        for (size_t i = 0; i < players.size(); ++i) {
            auto player = players[i];
            auto& snapshot = m_fields->snapshots[i];
            if (!player) {
                snapshot = {};
                continue;
            }
            auto counters = static_cast<AssistPlayer*>(player)->m_fields.self();
            snapshot = {
                true, player->m_isOnGround, player->m_isUpsideDown,
                counters->orbUses, counters->padBumps, touchingOrbCount(player),
            };
        }
    }

    // After a reset the players jump to a new position; don't read that as a jump.
    void forgetSnapshots() {
        for (auto& snapshot : m_fields->snapshots) snapshot = {};
    }

    void reportSave(RescueTracker::Save save) {
        auto kind = save.kind == RescueTracker::SaveKind::Orb ? "orb" : "landing";
        // Ticks since this attempt started, like a frame counter.
        auto attemptTick = m_gameState.m_currentProgress;
        if (g_config.logSaves) {
            log::info("Saved {} tick{} on tick {} ({})", save.ticks, save.ticks == 1 ? "" : "s", attemptTick, kind);
        }
        flashSave(m_uiLayer, fmt::format("Saved {}t on tick {} ({})", save.ticks, attemptTick, kind));
    }

    // Sends an input to the game through the whole hook chain, past our own interception.
    void deliver(bool down, int button, bool isPlayer1) {
        bool wasPassthrough = std::exchange(m_fields->passthrough, true);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        m_fields->passthrough = wasPassthrough;
    }

    // Every input passes here once any fixed delay is over; jump inputs go through the rescue window.
    void applyInput(bool down, int button, bool isPlayer1) {
        auto fields = m_fields.self();
        if (button != kJump || fields->settling) {
            return this->deliver(down, button, isPlayer1);
        }
        auto& tracker = fields->rescue[isPlayer1 ? 0 : 1];
        // The tick this input takes effect on: the current one, or the next if we're between ticks.
        int64_t tick = fields->scheduler.inStep() ? fields->scheduler.tick() : fields->scheduler.tick() + 1;

        if (down) {
            if (tracker.takeHeldRelease()) this->deliver(false, button, isPlayer1);
            this->deliver(true, button, isPlayer1);
            auto window = this->rescueWindow();
            tracker.onPress(tick, window, this->pressActsInstantly(isPlayer1));
            if (g_config.detailedLog && window > 0) this->logPress(isPlayer1, window, tracker.armed());
            return;
        }
        if (tracker.onRelease(tick)) {
            if (g_config.detailedLog) log::info("[P{}] tick {}: release held", isPlayer1 ? 1 : 2, m_gameState.m_currentProgress);
            return;
        }
        this->deliver(false, button, isPlayer1);
    }

    // Detailed log: the state a press found the player in.
    void logPress(bool isPlayer1, int64_t window, bool armed) {
        auto player = this->playersFor(isPlayer1)[0];
        if (!player) return;
        if (!armed) {
            log::info(
                "[P{}] tick {}: press in {} acts instantly, not held",
                isPlayer1 ? 1 : 2, m_gameState.m_currentProgress, modeName(player)
            );
            return;
        }
        log::info(
            "[P{}] tick {}: press opened a {}-tick window ({}, {}, touching {} orb{})",
            isPlayer1 ? 1 : 2, m_gameState.m_currentProgress, window, modeName(player),
            player->m_isOnGround ? "on the ground" : "in the air",
            touchingOrbCount(player), touchingOrbCount(player) == 1 ? "" : "s"
        );
    }

    // Start of a tick: act on what the previous tick did with any press still in its window.
    void rescueStep() {
        auto fields = m_fields.self();
        int64_t tick = fields->scheduler.tick();
        for (int side = 0; side < 2; ++side) {
            auto& tracker = fields->rescue[side];
            if (!tracker.armed()) continue;
            bool isPlayer1 = side == 0;

            // Switched off (or out of practice) mid-window: let go right away.
            if (this->rescueWindow() == 0) {
                if (tracker.flush()) this->deliver(false, kJump, isPlayer1);
                continue;
            }

            auto seen = this->observe(isPlayer1);
            auto result = tracker.step(tick, seen.observation);
            if (result.rePress) {
                this->deliver(false, kJump, isPlayer1);
                this->deliver(true, kJump, isPlayer1);
            }
            if (result.deliverRelease) this->deliver(false, kJump, isPlayer1);
            if (result.save) this->reportSave(*result.save);

            if (g_config.detailedLog) {
                auto side = isPlayer1 ? 1 : 2;
                auto attemptTick = m_gameState.m_currentProgress;
                if (result.rePress) log::info("[P{}] tick {}: entered an orb, click sent again", side, attemptTick);
                if (result.end == RescueTracker::End::Used) {
                    log::info("[P{}] tick {}: press used ({})", side, attemptTick, seen.usedBy ? seen.usedBy : "?");
                }
                if (result.end == RescueTracker::End::Expired) {
                    log::info(
                        "[P{}] tick {}: window ran out, nothing used the press{}",
                        side, attemptTick, result.deliverRelease ? "; held release let go" : ""
                    );
                }
            }
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (InputScheduler::keyIndex(button, isPlayer1) < 0 || !this->isAssistedLayer()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto fields = m_fields.self();
        if (fields->passthrough) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // The fixed delay applies to jump only. Other buttons still go through the scheduler
        // (with nothing added) so they can't overtake inputs queued before a setting changed.
        int64_t delay = (button == kJump && this->assistActive()) ? g_config.pressDelay : 0;
        if (fields->scheduler.schedule(down, button, isPlayer1, delay, 0)) {
            this->applyInput(down, button, isPlayer1);
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!this->isAssistedLayer()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }
        auto& scheduler = m_fields->scheduler;

        scheduler.startTick();
        // Observe first, so a press delivered this tick isn't judged by what the last tick did.
        this->rescueStep();
        this->deliverPending(false);
        this->takeSnapshots();

        bool wasInStep = scheduler.inStep();
        scheduler.setInStep(true);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        scheduler.setInStep(wasInStep);
    }

    // Applies delayed inputs in arrival order: those due by now, or all of them.
    void deliverPending(bool all) {
        auto fields = m_fields.self();
        while (auto input = fields->scheduler.popDue(all)) {
            this->applyInput(input->down, input->button, input->isPlayer1);
        }
    }

    // Before a reset or pause: let go of anything held on the player's behalf, then deliver
    // everything still delayed, so the game's buttons match what the player is holding.
    void settleInputs() {
        auto fields = m_fields.self();
        for (int side = 0; side < 2; ++side) {
            if (fields->rescue[side].flush()) this->deliver(false, kJump, side == 0);
        }
        bool wasSettling = std::exchange(fields->settling, true);
        this->deliverPending(true);
        fields->settling = wasSettling;
    }

    // Returns the previous value, so callers can restore it.
    bool setPassthrough(bool passthrough) {
        return std::exchange(m_fields->passthrough, passthrough);
    }
};

class $modify(AssistPlayLayer, PlayLayer) {
    AssistLayer* assist() {
        return static_cast<AssistLayer*>(static_cast<GJBaseGameLayer*>(this));
    }

    // Covers deaths, restarts, start positions and practice checkpoints. Whatever the reset
    // itself does with buttons happens without interception.
    void resetLevel() {
        auto assist = this->assist();
        assist->settleInputs();
        bool wasPassthrough = assist->setPassthrough(true);
        PlayLayer::resetLevel();
        assist->setPassthrough(wasPassthrough);
        assist->forgetSnapshots();
    }

    void pauseGame(bool unfocused) {
        auto assist = this->assist();
        assist->settleInputs();
        bool wasPassthrough = assist->setPassthrough(true);
        PlayLayer::pauseGame(unfocused);
        assist->setPassthrough(wasPassthrough);
    }
};

class $modify(AssistUILayer, UILayer) {
    bool init(GJBaseGameLayer* layer) {
        if (!UILayer::init(layer)) return false;
        if (!typeinfo_cast<PlayLayer*>(layer)) return true;

        auto winSize = CCDirector::get()->getWinSize();

        auto indicator = CCLabelBMFont::create("", "chatFont.fnt");
        indicator->setID("indicator"_spr);
        indicator->setAnchorPoint({ 0.f, 1.f });
        indicator->setScale(0.45f);
        indicator->setOpacity(140);
        indicator->setPosition({ 4.f, winSize.height - 4.f });
        this->addChild(indicator, 100);

        auto lastSave = CCLabelBMFont::create("", "chatFont.fnt");
        lastSave->setID("last-save"_spr);
        lastSave->setAnchorPoint({ 0.f, 1.f });
        lastSave->setScale(0.4f);
        lastSave->setOpacity(0);
        lastSave->setColor({ 255, 214, 92 });
        lastSave->setPosition({ 4.f, winSize.height - 14.f });
        this->addChild(lastSave, 100);

        refreshIndicator(this);
        return true;
    }
};

$on_mod(Loaded) {
    loadConfig();
    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        loadConfig();
        if (auto playLayer = PlayLayer::get()) {
            refreshIndicator(playLayer->m_uiLayer);
        }
    });
    listenForKeybindSettingPresses("cycle-stage", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) cycleStage();
    });
}
