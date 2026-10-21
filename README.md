# Input Assist (Geode mod for Geometry Dash 2.2081)

Early-click rescue for Geometry Dash, built on Geode SDK v5. A jump press that comes a few physics ticks too soon stays alive until it can do something.

| Setting | Default | What it does |
| --- | --- | --- |
| Enable Input Assist | on | Master switch; applies immediately, even mid-level. |
| Rescue Stage | Stage 1 (+2 ticks) | Off, +2, +4, +8 or +16 ticks. How long an early jump press stays alive (1 tick = 1/240 s ≈ 4.2 ms). |
| Cycle Stage | **F6** | Steps through the stages in game; a toast shows the new one. Rebindable. |
| Practice Mode Only | off | Only assist while practice mode is on. |
| Show Indicator | on | Stage in the top-left corner of a level, with each save shown under it for a moment. |
| Log Saves | on | Writes every save to Geode's log: `Saved 3 ticks on tick 1042 (landing)`. |
| Detailed Log | off | Also logs every press that opens a window and what ended it. For testing and tuning. |
| Fixed Click Delay | 0 | Advanced: moves every jump press and release a fixed number of ticks later. |

Settings live in **Geode → Input Assist → Settings**, including from the pause menu. To watch saves live on Windows, turn on **Show Platform Console** in Geode's own settings; a console window opens next to the game.

## How it works

GD 2.2 runs physics in fixed ticks of 1/240 s. Once per tick, `GJBaseGameLayer::processCommands` applies the clicks collected since the last tick through `GJBaseGameLayer::handleButton`, which is where a click reaches the player.

A jump press **arms** the rescue window for N ticks (the stage). While it's armed:

- **Early release (landing):** if you let go before the press did anything, your release is held back until the player jumps or flips, or the window ends. A tap that ended just before you landed still jumps. Logged as `(landing)`, with the number of ticks your release came early.
- **Early press (orbs):** if you reach an orb inside the window, the press is sent again so the orb fires. Logged as `(orb)`, with how many ticks early you clicked.
- **Flight modes** (ship, UFO, wave, swing) act on a press instantly, so they're never armed and hold lengths there are never changed.

"The press did something" is read from the player at the start of every tick, by comparing it with a snapshot from the tick before:

- it left the ground moving upward, and no pad launched it (`PlayerObject::bumpPlayer`);
- a ball or spider flipped gravity from the ground;
- an orb or dash orb fired (`PlayerObject::ringJump`, `PlayerObject::startDashing`);
- it switched to a mode where presses act instantly.

Inputs are only ever sent at tick boundaries, so physics stays deterministic. Before a death, restart, checkpoint load or pause, anything held on your behalf is let go first, so the game's buttons always match what you're holding.

The logic lives in two game-independent classes:

- **`src/RescueTracker.hpp`**: the rescue window.
- **`src/InputScheduler.hpp`**: the fixed delay and input ordering.

`src/main.cpp` hooks them into the game.

### What it doesn't do

It never moves an input **earlier** than you made it, and it doesn't react to hazards. Saving a late click or a late release would mean predicting or rewinding the game's physics, which this mod doesn't do.

## Building

Push this folder to a GitHub repository. `.github/workflows/multi-platform.yml` builds the Windows version, packages it as `chief.input-assist.geode`, and publishes it on the repo's **Releases** page.

To build locally instead, you need [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli), SDK v5.11.2 (`geode sdk install`, `geode sdk install-binaries`), CMake 3.25+ and Clang 19+ or MSVC 19.44+:

```sh
geode build
```

## Installing

Copy `chief.input-assist.geode` into `<Geometry Dash folder>/geode/mods/` and restart the game. In Steam: right-click Geometry Dash → Manage → Browse local files.

Before publishing, change `"developer"` and the `id` prefix in `mod.json` to your own name.

## Tests

Both logic classes are tested on the host, with no Geode or GD needed:

```sh
g++ -std=c++20 -Wall -Wextra -Isrc tests/rescue_test.cpp -o rescue_test && ./rescue_test
g++ -std=c++20 -Wall -Wextra -Isrc tests/scheduler_test.cpp -o scheduler_test && ./scheduler_test
```

Each has targeted cases plus randomized runs (5,000 and 2,000 rounds). They check four things:

- The game always sees press and release alternate.
- A held release never outlives the window.
- The game's button ends up matching yours.
- Delayed inputs never land early or out of order.

## Compatibility notes

- **Click Between Frames / vanilla click-between-steps:** inputs the mod sends itself (held releases, orb re-presses, delayed clicks) land at the start of a tick and lose sub-tick precision (under 4.2 ms). Everything else keeps its exact timing.
- `handleButton` is hooked at `Priority::VeryEarly`, so other mods hooking it see each input once, when it actually reaches the game.
- Mods that replace `UILayer` may hide the indicator; the assist itself still works.

## Fair play

This changes gameplay, so it's tagged `cheat` for the Geode index. Completions made with it enabled count as hacked on community lists (Demonlist, Pointercrate, etc.). Use **Practice Mode Only** or set the stage to Off for runs you plan to submit.
