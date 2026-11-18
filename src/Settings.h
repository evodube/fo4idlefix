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
	inline float fStatsInterval = 30.0f;  // seconds between summary lines (0 = off)
	inline int   iMaxDumps = 40;          // stop dumping timelines after this many (summaries continue)
	inline std::vector<std::string> sGraphVars{
		"Speed", "Direction", "TurnDelta", "TurnDeltaDamped", "SpeedSmoothed",
		"IsTurning", "bIsTurning", "fTurnAngle", "TurnAngle", "iSyncTurnState",
		"bAllowRotation", "bAnimationDriven", "bHeadTrackingOn", "IsMoving"
	};

	// [Fix]
	// Standing NPCs get their turn request (TurnDelta) cleared before their animation updates, so the
	// turn-in-place shuffle never starts: always for NPCs held in place (restrained: they can't turn, the
	// feet step round and snap back), and for other standing NPCs when the turn asked for is small.
	inline bool  bPreventIdleTurns = true;
	inline bool  bRestrainedNeverTurn = true;  // restrained NPCs: clear every turn request, any size
	inline float fBlockBelowDeg = 35.0f;       // other standing NPCs: clear turn requests smaller than this (0 = off)
	inline float fStandingSpeed = 5.0f;        // graph Speed below this counts as standing
	inline std::vector<std::string> sTurnVars{ "TurnDelta", "TurnDeltaDamped", "TurnDeltaSmoothed" };

	void Load();
}
