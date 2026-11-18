# fo4idlefix

F4SE plugin for Fallout 4 1.10.163 (old-gen), 1.10.980/984 (next-gen) and 1.11.x (Anniversary) that tracks down, and tries to fix, NPCs that shuffle their feet into a small turn while standing idle and then turn straight back.

- **Logging**: every NPC within `fRadius` of the player is sampled each frame. When a standing NPC's heading starts changing, the turn is recorded until it settles. Each turn gets a summary line (peak and net angle, whether it returned, where the player was, which anim events fired). Turn-and-return episodes also get a full timeline: heading, position, frame time, behaviour graph variables and every animation event the NPC's graph sent.
- **Fix (experimental, off)**: `bZeroTurnVar` zeroes a chosen graph variable (default `TurnDelta`) on standing NPCs when it's below `fZeroBelow`, so small turn requests never start the turn animation. Turn it on once the log shows which variable drives the shuffle.

Settings: `Data/F4SE/Plugins/fo4idlefix.ini`. Log: `Documents/My Games/Fallout4/F4SE/fo4idlefix.log`.

Requires F4SE and Address Library for F4SE Plugins.

Build: `git -C extern/CommonLibF4 apply ../../patches/commonlibf4-1.11-runtime.patch`, then `xmake f -m releasedbg -y && xmake`. CI does both on every push to main.
