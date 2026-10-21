# Input Assist (Geode mod for Geometry Dash 2.2081)

Tick-based **click delay** and **jump buffering** for Geometry Dash, built on Geode SDK v5.

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| Enable Input Assist | on/off | on | Master switch; applies immediately, even mid-level. |
| Click Delay (ticks) | 0–24 | 1 | Every press **and** release lands N physics ticks later (1 tick = 1/240 s ≈ 4.2 ms). Hold lengths are unchanged. |
| Buffer Ticks | 0–24 | 2 | Every press is held for at least N ticks, so a tap released just before you land or reach an orb still counts. Longer holds are untouched. |
| Affect Platformer Movement | on/off | off | Also adjust left/right in platformer levels (otherwise jump only). |
| Practice Mode Only | on/off | off | Only adjust inputs while practice mode is on. |
| Show Indicator | on/off | on | Small "Input Assist: delay Nt, buffer Nt" label in the top-left of a level. |

All settings are in **Geode → Input Assist → Settings**, including from the pause menu.

## How it works

GD 2.2 runs physics in fixed steps of 1/240 s. Once per step, `GJBaseGameLayer::processCommands` applies the clicks collected since the last step, each through `GJBaseGameLayer::handleButton`, which is where a click reaches the player.

- **`processCommands` hook**: counts ticks. At the start of each tick it delivers any held-back input that has come due, before the game simulates that tick.
- **`handleButton` hook**: asks `InputScheduler` which tick the input should land on. That is the tick vanilla would use (the *base*), plus the click delay; a release may also be pushed later by the buffer. If nothing was added the input goes straight through, so timing is exactly vanilla. Otherwise it is queued.
- **Ordering**: inputs for the same button can never overtake each other, so a press/release pair can't swap and leave the button stuck.
- **`PlayLayer::resetLevel` / `pauseGame` hooks**: anything still queued is delivered at once, so after a death, restart, checkpoint load or pause the game's button state matches what you're physically holding.
- **`UILayer::init` hook**: adds the indicator label.

Inputs only ever move later, and only onto tick boundaries, so physics stays deterministic in normal and practice mode. The queue lives in the layer's `m_fields` and holds plain values only: no cocos objects are retained, and it is freed with the level.

The editor's playtest is not affected (only `PlayLayer` is assisted).

### Tuning

- **Click Delay 1** (≈4 ms) is the subtlest setting. Raise it one tick at a time if you still tend to click early. Note that it delays *every* click, so late clicks get later too. It helps when your error is consistently early.
- **Buffer 2–3** catches taps that end a few milliseconds before you land or hit an orb. In ship and wave it lengthens only taps shorter than the buffer, so keep it low there.
- **0 / 0** is identical to vanilla.

## Building

You need:

- [Geode](https://geode-sdk.org) installed into Geometry Dash 2.2081
- [Geode CLI](https://docs.geode-sdk.org/getting-started/geode-cli) and the SDK v5.11.2 or newer: `geode sdk install`, then `geode sdk install-binaries`
- CMake 3.25+ and a compiler Geode v5 accepts: **Clang 19+**, or **MSVC 19.44+** (Visual Studio 2022 17.14 / 2026) on Windows

Optionally, to auto-install builds into GD, run `geode profile add` once and point it at your GD folder.

### Option A: build locally

```sh
cd input-assist
geode build            # configures + builds with CMake, outputs chief.input-assist.geode
```

or with plain CMake:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

If a Geode CLI profile is set, the `.geode` file is copied into your mods folder automatically. Otherwise it's in `build/`.

### Option B: build in GitHub Actions (all platforms, no local toolchain)

Push this folder to a GitHub repository. `.github/workflows/multi-platform.yml` builds Windows and macOS, combines them into one `chief.input-assist.geode`, and publishes it on the repo's **Releases** page. If one platform fails, the others are still combined and released.

## Installing

Copy `chief.input-assist.geode` into the Geode mods folder and restart GD:

- **Windows**: `<Geometry Dash folder>/geode/mods/`
- **macOS**: `~/Library/Application Support/GeometryDash/geode/mods/`

Or open the Geode menu in-game, click the folder icon, and drop the file there.

Before publishing, change `"developer"` and the `id` prefix in `mod.json` to your own name.

## Tests

The scheduling logic in `src/InputScheduler.hpp` has no game dependencies, so it can be tested on any machine:

```sh
g++ -std=c++20 -Wall -Wextra -Isrc tests/scheduler_test.cpp -o scheduler_test && ./scheduler_test
```

Cases covered: vanilla passthrough, exact 1-tick delay, delay preserving hold length, buffering short taps, long holds untouched, per-button ordering, inputs arriving between ticks, independent buttons and players, flushes, settings changed mid-level. A 2,000-round randomized test also checks that every input lands exactly once, in order, never earlier than vanilla.

## Compatibility notes

- **Click Between Frames / vanilla click-between-steps**: inputs that pass through undelayed keep their sub-tick timing. Inputs that are delayed land at the start of their tick, so they lose sub-tick precision (under 4.2 ms).
- `handleButton` is hooked at `Priority::VeryEarly`, so other mods hooking it see each input once, when it actually lands.
- Mods that reskin or replace `UILayer` may hide the indicator; the assist itself still works.

## Fair play

This changes gameplay timing, so it's tagged `cheat` for the Geode index. Completions made with it enabled count as hacked on community lists (Demonlist, Pointercrate, etc.). Turn it off, or use **Practice Mode Only**, for runs you plan to submit.
