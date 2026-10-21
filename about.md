# Input Assist

Keeps an early jump press alive for a few physics ticks (1 tick = 1/240 s), so it still counts once it can do something.

- **Early release:** let go just before landing and you still jump.
- **Early press:** click just before an orb and it still fires.
- **Stages:** Off, +2, +4, +8 or +16 ticks. Press **F6** in game to cycle them.
- **Feedback:** every save shows on screen and in Geode's log, e.g. `Saved 3 ticks on tick 1042 (landing)`.

Ship, UFO, wave and swing presses act instantly, so they are never held. Inputs are never moved earlier than you made them.

**Note:** this changes gameplay. Completions made with it enabled count as hacked on community lists.
