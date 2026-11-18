# fo4idlefix

F4SE plugin for Fallout 4 1.10.163 (old-gen), 1.10.980/984 (next-gen) and 1.11.x (Anniversary) that stops NPCs shuffling their feet into a small turn while standing idle and then turning straight back, and logs where it happens.

- **Fix**: before a standing NPC's animation updates, its turn request (`TurnDelta`, `TurnDeltaDamped`) is cleared, so the turn-in-place step never starts. NPCs held in place (restrained) can't turn at all, so every request they get is cleared; other standing NPCs only lose small ones (below `fBlockBelowDeg`), so they still turn to face you. NPCs with a weapon out are left alone.
- **Logging**: every NPC within `fRadius` of the player is watched. `GRAPH TURN` lines show each turn the animation graph asked for while the NPC stood (before the fix cleared it), whether the NPC was restrained, and how often the fix cleared it. Body turns that came back get `TURN-AND-RETURN` lines and a full timeline (heading, position, frame time, graph variables, every animation event). A stats line every minute counts it all.

Settings: `Data/F4SE/Plugins/fo4idlefix.ini`. Log: `Documents/My Games/Fallout4/F4SE/fo4idlefix.log`.

Requires F4SE and Address Library for F4SE Plugins.

Build: `git -C extern/CommonLibF4 apply ../../patches/commonlibf4-1.11-runtime.patch`, then `xmake f -m releasedbg -y && xmake`. CI does both on every push to main.
