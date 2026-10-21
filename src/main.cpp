/*
 * Input Assist: tick-based click delay and jump buffering for Geometry Dash 2.2081.
 *
 * How the input timing works
 * --------------------------
 * GD 2.2 simulates physics in fixed steps ("ticks") of 1/240 s of game time.
 * GJBaseGameLayer::processCommands() runs exactly once per tick, and during it the
 * game drains the clicks collected since the previous tick and applies each one
 * through GJBaseGameLayer::handleButton(). handleButton is the point where a click
 * actually reaches the PlayerObject.
 *
 * This mod hooks both functions:
 *
 *  - processCommands is the tick counter. At the start of each tick it advances the
 *    counter and delivers every held-back input whose due tick has arrived, before
 *    the game simulates that tick.
 *  - handleButton asks the InputScheduler when the input should land. If nothing was
 *    added it is applied straight away (exactly vanilla); otherwise it waits in a
 *    small queue until its due tick.
 *
 *  Click Delay   (press-delay)  : every press and release is moved N ticks later.
 *                                 Hold lengths are unchanged; the whole input shifts.
 *  Buffer Ticks  (buffer-ticks) : a release is never applied sooner than N ticks
 *                                 after its press, so a short tap that ends just
 *                                 before landing or reaching an orb still counts.
 *
 * Inputs only ever move later, and only onto tick boundaries, so the simulation stays
 * deterministic. Before a level reset or pause, everything still queued is delivered
 * at once, so the game's button state always ends up matching the player's.
 */

#include "InputScheduler.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/UILayer.hpp>

#include <utility>

using namespace geode::prelude;

namespace {
    constexpr int64_t kMaxTicks = 24;

    struct Config {
        bool enabled = true;
        int64_t pressDelay = 1;
        int64_t bufferTicks = 2;
        bool affectMovement = false;
        bool practiceOnly = false;
        bool showIndicator = true;
    };

    // Cached copy of the settings, so the per-tick path never touches the settings store.
    Config g_config;

    void loadConfig() {
        auto mod = Mod::get();
        g_config.enabled = mod->getSettingValue<bool>("enabled");
        g_config.pressDelay = std::clamp<int64_t>(mod->getSettingValue<int64_t>("press-delay"), 0, kMaxTicks);
        g_config.bufferTicks = std::clamp<int64_t>(mod->getSettingValue<int64_t>("buffer-ticks"), 0, kMaxTicks);
        g_config.affectMovement = mod->getSettingValue<bool>("affect-movement");
        g_config.practiceOnly = mod->getSettingValue<bool>("practice-only");
        g_config.showIndicator = mod->getSettingValue<bool>("show-indicator");
    }

    bool indicatorVisible() {
        return g_config.enabled && g_config.showIndicator && (g_config.pressDelay > 0 || g_config.bufferTicks > 0);
    }

    void refreshIndicator(UILayer* ui) {
        if (!ui) return;
        auto label = typeinfo_cast<CCLabelBMFont*>(ui->getChildByID("indicator"_spr));
        if (!label) return;
        auto text = fmt::format("Input Assist: delay {}t, buffer {}t", g_config.pressDelay, g_config.bufferTicks);
        if (g_config.practiceOnly) text += " (practice)";
        label->setString(text.c_str());
        label->setVisible(indicatorVisible());
    }
}

class $modify(AssistLayer, GJBaseGameLayer) {
    struct Fields {
        InputScheduler scheduler;
        // True while we (or a reset/pause) are calling handleButton and it must not be intercepted.
        bool passthrough = false;
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

    bool assistAppliesTo(int button) {
        if (!g_config.enabled) return false;
        if (button != static_cast<int>(PlayerButton::Jump) && !g_config.affectMovement) return false;
        if (g_config.practiceOnly && !m_isPracticeMode) return false;
        return true;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (InputScheduler::keyIndex(button, isPlayer1) < 0 || !this->isAssistedLayer()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        auto fields = m_fields.self();
        if (fields->passthrough) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // With the assist off for this button it still goes through the scheduler (with
        // nothing added), so it can't overtake inputs queued before a setting changed.
        bool assisted = this->assistAppliesTo(button);
        int64_t delay = assisted ? g_config.pressDelay : 0;
        int64_t buffer = assisted ? g_config.bufferTicks : 0;

        if (fields->scheduler.schedule(down, button, isPlayer1, delay, buffer)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!this->isAssistedLayer()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }
        auto& scheduler = m_fields->scheduler;

        scheduler.startTick();
        // Inputs due this tick go in before the game simulates it.
        this->deliverPending(false);

        bool wasInStep = scheduler.inStep();
        scheduler.setInStep(true);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        scheduler.setInStep(wasInStep);
    }

    // Applies queued inputs in arrival order: those due by now, or all of them.
    void deliverPending(bool all) {
        auto fields = m_fields.self();
        while (auto input = fields->scheduler.popDue(all)) {
            // Goes through the whole hook chain, so other mods see the input when it lands.
            bool wasPassthrough = this->setPassthrough(true);
            GJBaseGameLayer::handleButton(input->down, input->button, input->isPlayer1);
            this->setPassthrough(wasPassthrough);
        }
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

    // Covers deaths, restarts, start positions and practice checkpoints. Anything still
    // queued is delivered first (vanilla would already have applied it), and whatever
    // the reset itself does with buttons happens without interception.
    void resetLevel() {
        auto assist = this->assist();
        assist->deliverPending(true);
        bool wasPassthrough = assist->setPassthrough(true);
        PlayLayer::resetLevel();
        assist->setPassthrough(wasPassthrough);
    }

    void pauseGame(bool unfocused) {
        auto assist = this->assist();
        assist->deliverPending(true);
        bool wasPassthrough = assist->setPassthrough(true);
        PlayLayer::pauseGame(unfocused);
        assist->setPassthrough(wasPassthrough);
    }
};

class $modify(AssistUILayer, UILayer) {
    bool init(GJBaseGameLayer* layer) {
        if (!UILayer::init(layer)) return false;
        if (!typeinfo_cast<PlayLayer*>(layer)) return true;

        auto label = CCLabelBMFont::create("", "chatFont.fnt");
        label->setID("indicator"_spr);
        label->setAnchorPoint({ 0.f, 1.f });
        label->setScale(0.45f);
        label->setOpacity(140);
        auto winSize = CCDirector::get()->getWinSize();
        label->setPosition({ 4.f, winSize.height - 4.f });
        this->addChild(label, 100);

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
}
