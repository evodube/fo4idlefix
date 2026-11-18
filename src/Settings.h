#pragma once

namespace Settings
{
	// [General]
	inline bool bEnabled = true;

	// [Log]
	inline bool  bLogEpisodes = true;     // one line per idle turn (start/peak/end heading, cause hints)
	inline bool  bDumpEpisodes = true;    // full sample + anim event timeline for each turn-and-return
	inline bool  bLogAllEvents = false;   // every anim event of tracked NPCs, live (very spammy)
	inline float fRadius = 1500.0f;       // only NPCs this close to the player are tracked (game units)
	inline float fMinTurnDeg = 4.0f;      // smaller heading changes are ignored
	inline float fStatsInterval = 60.0f;  // seconds between summary lines (0 = off)
	inline int   iMaxDumps = 40;          // stop dumping timelines after this many (summaries continue)
	inline std::vector<std::string> sGraphVars{
		"Speed", "Direction", "TurnDelta", "TurnDeltaDamped", "SpeedSmoothed",
		"IsTurning", "bIsTurning", "fTurnAngle", "TurnAngle", "iSyncTurnState",
		"bAllowRotation", "bAnimationDriven", "bHeadTrackingOn", "IsMoving"
	};

	// [Fix] (experimental, off until the log shows what drives the turn)
	inline bool        bZeroTurnVar = false;   // zero sTurnVar on standing NPCs when it is below fZeroBelow
	inline std::string sTurnVar = "TurnDelta";
	inline float       fZeroBelow = 0.5f;      // in the variable's own units (TurnDelta is radians: 0.5 ~ 29 deg)

	void Load();
}
