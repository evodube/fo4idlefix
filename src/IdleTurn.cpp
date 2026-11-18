#include "IdleTurn.h"

#include "Settings.h"

// Diagnostics for NPCs that, while standing idle, step/turn a little with their
// feet and then turn straight back to where they were facing.
//
// Every NPC near the player is watched from Actor::Update. When a standing NPC's
// heading starts to change we open an "episode" and close it once the heading has
// been still for a moment. Each episode gets one summary line (how far it turned,
// whether it came back, which way the player was, which anim events fired). For
// turn-and-return episodes the whole timeline is dumped: per-frame heading,
// position, frame time and the behaviour graph variables named in the ini,
// interleaved with every animation event the NPC's graph sent.
//
// Only engine pieces that are the same on 1.10.163, 1.10.980/984 and 1.11.x are
// used: the Actor vtable, the REFR's angle/location, and the animation graph
// holder interface.

namespace IdleTurn
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		constexpr std::size_t kUpdateVfunc = 0xCF;     // Actor::Update(float)
		constexpr std::size_t kProcessEventVfunc = 1;  // BSTEventSink<T>::ProcessEvent
		constexpr std::size_t kMaxVars = 24;

		constexpr double kHistory = 4.0;          // seconds of samples/events kept per NPC
		constexpr double kStillAfter = 0.6;       // episode ends after this long without turning
		constexpr double kMaxEpisode = 15.0;      // force-close very long episodes
		constexpr double kStationaryWindow = 0.5;
		constexpr float  kStationaryDist = 12.0f;  // game units moved inside the window
		constexpr float  kTurnRateDeg = 12.0f;     // deg/s that counts as "turning"
		constexpr double kForgetAfter = 10.0;      // drop NPCs not updated for this long
		constexpr double kDumpStep = 0.033;        // dump at most ~30 samples per second

		const clock::time_point g_start = clock::now();

		double Now()
		{
			return std::chrono::duration<double>(clock::now() - g_start).count();
		}

		float WrapPi(float a)
		{
			constexpr float pi = std::numbers::pi_v<float>;
			constexpr float tau = 2.0f * pi;
			a = std::fmod(a + pi, tau);
			if (a < 0.0f) {
				a += tau;
			}
			return a - pi;
		}

		float Deg(float a_rad) { return a_rad * (180.0f / std::numbers::pi_v<float>); }

		enum class VarKind : std::uint8_t
		{
			kNone,
			kFloat,
			kInt,
			kBool
		};

		struct Sample
		{
			double t;
			float  heading;
			float  x, y, z;
			float  dist;       // to player
			float  relPlayer;  // player bearing relative to NPC facing (rad, + = to the right)
			float  dt;         // Actor::Update delta
			bool   restrained; // held in place (SetRestrained): can't turn at all
			std::array<float, kMaxVars> vars;
		};

		struct Event
		{
			double      t;
			float       heading;
			std::string tag;
			std::string payload;
		};

		struct State
		{
			std::string        name;
			std::deque<Sample> samples;
			std::deque<Event>  events;
			std::vector<VarKind> kinds;
			double             probedAt{ -100.0 };
			bool               loggedVars{ false };
			double             lastSeen{ 0.0 };

			// episode
			bool   inEpisode{ false };
			double epStart{ 0.0 };
			double lastTurnT{ 0.0 };
			float  startHeading{ 0.0f };
			float  peakDev{ 0.0f };      // signed deviation at the peak
			float  startRel{ 0.0f };
			float  startDist{ 0.0f };
			bool   moved{ false };

			std::uint32_t episodes{ 0 };
			std::uint32_t returns{ 0 };

			// graph turn: the animation graph asked to turn (TurnDelta) while standing, whether or not the body did
			bool   gtActive{ false };
			double gtStart{ 0.0 };
			double gtQuietSince{ 0.0 };
			float  gtPeak{ 0.0f };
			float  gtHeading{ 0.0f };
			float  gtRel{ 0.0f };
			float  gtDist{ 0.0f };
			bool   gtRestrained{ false };
			std::uint32_t blocked{ 0 };    // frames the fix cleared this NPC's turn request (since the graph turn began)
			std::uint32_t graphTurns{ 0 };
		};

		std::mutex                                   g_lock;
		std::unordered_map<std::uint32_t, State>     g_actors;
		std::vector<RE::BSFixedString>               g_varNames;
		std::vector<RE::BSFixedString>               g_turnVarNames;  // [0] decides, all get cleared
		std::optional<RE::BSFixedString>             g_speedVar;
		int                                          g_tdIndex{ -1 };  // TurnDelta's slot in g_varNames

		std::atomic<std::uint64_t> g_blockedRestrained{ 0 };
		std::atomic<std::uint64_t> g_blockedSmall{ 0 };
		std::atomic<std::uint64_t> g_preUpdateCalls{ 0 };
		std::atomic<std::uint64_t> g_blockedInPreUpdate{ 0 };
		std::once_flag                               g_initOnce;

		// global stats (under g_lock)
		double        g_lastStats{ 0.0 };
		double        g_dtSum{ 0.0 };
		float         g_dtMin{ 1e9f };
		float         g_dtMax{ 0.0f };
		std::uint64_t g_dtCount{ 0 };
		std::uint32_t g_episodes{ 0 };
		std::uint32_t g_returns{ 0 };
		std::uint32_t g_towardPlayer{ 0 };
		std::uint32_t g_graphTurns{ 0 };
		int           g_dumps{ 0 };

		// ------------------------------------------------------------------
		// Animation graph access
		// ------------------------------------------------------------------

		VarKind Probe(RE::Actor* a_actor, const RE::BSFixedString& a_name)
		{
			float f = 0.0f;
			if (a_actor->GetGraphVariableImplFloat(a_name, f)) {
				return VarKind::kFloat;
			}
			std::int32_t i = 0;
			if (a_actor->GetGraphVariableImplInt(a_name, i)) {
				return VarKind::kInt;
			}
			bool b = false;
			if (a_actor->GetGraphVariableImplBool(a_name, b)) {
				return VarKind::kBool;
			}
			return VarKind::kNone;
		}

		float ReadVar(RE::Actor* a_actor, const RE::BSFixedString& a_name, VarKind a_kind)
		{
			switch (a_kind) {
			case VarKind::kFloat:
				{
					float f = 0.0f;
					return a_actor->GetGraphVariableImplFloat(a_name, f) ? f : std::nanf("");
				}
			case VarKind::kInt:
				{
					std::int32_t i = 0;
					return a_actor->GetGraphVariableImplInt(a_name, i) ? static_cast<float>(i) : std::nanf("");
				}
			case VarKind::kBool:
				{
					bool b = false;
					return a_actor->GetGraphVariableImplBool(a_name, b) ? (b ? 1.0f : 0.0f) : std::nanf("");
				}
			default:
				return std::nanf("");
			}
		}

		std::string VarList(const State& a_state)
		{
			std::string out;
			for (std::size_t i = 0; i < g_varNames.size(); ++i) {
				if (i < a_state.kinds.size() && a_state.kinds[i] != VarKind::kNone) {
					const char* kind = a_state.kinds[i] == VarKind::kFloat ? "f" : a_state.kinds[i] == VarKind::kInt ? "i" :
					                                                                                                   "b";
					out += std::format("{}{}({})", out.empty() ? "" : " ", g_varNames[i].c_str(), kind);
				}
			}
			return out.empty() ? "none" : out;
		}

		std::string VarValues(const State& a_state, const Sample& a_sample)
		{
			std::string out;
			for (std::size_t i = 0; i < g_varNames.size() && i < a_state.kinds.size(); ++i) {
				if (a_state.kinds[i] == VarKind::kNone) {
					continue;
				}
				out += std::format(" {}={:.3f}", g_varNames[i].c_str(), a_sample.vars[i]);
			}
			return out;
		}

		// ------------------------------------------------------------------
		// Episode bookkeeping
		// ------------------------------------------------------------------

		const Sample* SampleAtOrBefore(const State& a_state, double a_t)
		{
			const Sample* found = nullptr;
			for (const auto& s : a_state.samples) {
				if (s.t > a_t) {
					break;
				}
				found = &s;
			}
			return found ? found : (a_state.samples.empty() ? nullptr : &a_state.samples.front());
		}

		std::string EventSummary(const State& a_state, double a_from, double a_to)
		{
			std::vector<std::pair<std::string, int>> counts;
			for (const auto& e : a_state.events) {
				if (e.t < a_from || e.t > a_to) {
					continue;
				}
				auto it = std::ranges::find_if(counts, [&](auto& p) { return p.first == e.tag; });
				if (it == counts.end()) {
					counts.emplace_back(e.tag, 1);
				} else {
					++it->second;
				}
			}
			std::string out;
			for (const auto& [tag, n] : counts) {
				out += std::format("{}{}{}", out.empty() ? "" : ", ", tag, n > 1 ? std::format("x{}", n) : "");
			}
			return out.empty() ? "none" : out;
		}

		// Builds the log lines for a finished episode (logged after the lock is released).
		void CloseEpisode(std::uint32_t a_formID, State& a_state, double a_now, std::vector<std::string>& a_out)
		{
			a_state.inEpisode = false;

			const auto& last = a_state.samples.back();
			const float net = WrapPi(last.heading - a_state.startHeading);
			const float peak = std::abs(a_state.peakDev);
			if (Deg(peak) < Settings::fMinTurnDeg) {
				return;
			}
			if (a_state.moved) {
				return;  // NPC walked off, that's ordinary locomotion
			}

			const bool returned = std::abs(net) < 0.4f * peak;
			const bool toward = (a_state.peakDev > 0.0f) == (a_state.startRel > 0.0f) &&
			                    std::abs(a_state.startRel) > 0.05f;
			const double end = a_state.lastTurnT;

			++a_state.episodes;
			++g_episodes;
			if (returned) {
				++a_state.returns;
				++g_returns;
			}
			if (toward) {
				++g_towardPlayer;
			}

			if (Settings::bLogEpisodes) {
				a_out.push_back(std::format(
					"[{:08X} {}] {} : peak {:+.1f}deg, net {:+.1f}deg over {:.2f}s | player {:.0f}u at {:+.0f}deg ({} the player) | events: {} | npc totals: {} turns, {} returns",
					a_formID, a_state.name, returned ? "TURN-AND-RETURN" : "turn", Deg(a_state.peakDev), Deg(net),
					end - a_state.epStart, a_state.startDist, Deg(a_state.startRel), toward ? "toward" : "away from",
					EventSummary(a_state, a_state.epStart - 0.3, end + 0.1), a_state.episodes, a_state.returns));
			}

			if (returned && Settings::bDumpEpisodes && g_dumps < Settings::iMaxDumps) {
				++g_dumps;
				const double from = a_state.epStart - 0.5;
				a_out.push_back(std::format("  --- timeline {} for {:08X} (t=0 is episode start, start heading {:.1f}deg, graph vars: {}) ---",
					g_dumps, a_formID, Deg(a_state.startHeading), VarList(a_state)));

				auto   ev = a_state.events.begin();
				double lastDumped = -1e9;
				for (const auto& s : a_state.samples) {
					if (s.t < from) {
						continue;
					}
					for (; ev != a_state.events.end() && ev->t <= s.t; ++ev) {
						if (ev->t >= from) {
							a_out.push_back(std::format("  {:+.3f}s  EVENT {}{}{} (hdg {:.1f})", ev->t - a_state.epStart, ev->tag,
								ev->payload.empty() ? "" : " payload=", ev->payload, Deg(ev->heading)));
						}
					}
					if (s.t - lastDumped < kDumpStep && s.t < end) {
						continue;
					}
					lastDumped = s.t;
					a_out.push_back(std::format("  {:+.3f}s  hdg {:7.2f} dev {:+6.2f} pos ({:.1f}, {:.1f}, {:.1f}) player {:.0f}u@{:+.0f}deg dt {:.1f}ms{}{}",
						s.t - a_state.epStart, Deg(s.heading), Deg(WrapPi(s.heading - a_state.startHeading)), s.x, s.y, s.z,
						s.dist, Deg(s.relPlayer), s.dt * 1000.0f, s.restrained ? " [restrained]" : "", VarValues(a_state, s)));
				}
				for (; ev != a_state.events.end(); ++ev) {
					a_out.push_back(std::format("  {:+.3f}s  EVENT {}{}{} (hdg {:.1f})", ev->t - a_state.epStart, ev->tag,
						ev->payload.empty() ? "" : " payload=", ev->payload, Deg(ev->heading)));
				}
				a_out.push_back("  --- end timeline ---");
				if (g_dumps == Settings::iMaxDumps) {
					a_out.push_back(std::format("reached iMaxDumps={}, only summary lines from now on", Settings::iMaxDumps));
				}
			}
			(void)a_now;
		}

		void Trim(State& a_state, double a_now)
		{
			// keep enough history for the whole open episode
			const double keepFrom = a_state.inEpisode ? std::min(a_now - kHistory, a_state.epStart - 1.0) : a_now - kHistory;
			while (!a_state.samples.empty() && a_state.samples.front().t < keepFrom) {
				a_state.samples.pop_front();
			}
			while (!a_state.events.empty() && a_state.events.front().t < keepFrom) {
				a_state.events.pop_front();
			}
		}

		void Stats(double a_now, std::vector<std::string>& a_out)
		{
			if (Settings::fStatsInterval <= 0.0f || a_now - g_lastStats < Settings::fStatsInterval) {
				return;
			}
			if (g_dtCount > 0) {
				const double avg = g_dtSum / static_cast<double>(g_dtCount);
				a_out.push_back(std::format(
					"stats: {} NPCs tracked | NPC update dt avg {:.2f}ms (~{:.0f} fps) min {:.2f}ms max {:.2f}ms | body turns {} ({} turn-and-return, {} toward player) | graph turn requests {} | fix cleared {} restrained + {} small requests ({} of them before the anim update, {} anim updates seen)",
					g_actors.size(), avg * 1000.0, avg > 0.0 ? 1.0 / avg : 0.0, g_dtMin * 1000.0f, g_dtMax * 1000.0f,
					g_episodes, g_returns, g_towardPlayer, g_graphTurns, g_blockedRestrained.load(), g_blockedSmall.load(),
					g_blockedInPreUpdate.load(), g_preUpdateCalls.load()));
			}
			g_lastStats = a_now;
			g_dtSum = 0.0;
			g_dtCount = 0;
			g_dtMin = 1e9f;
			g_dtMax = 0.0f;

			std::erase_if(g_actors, [&](const auto& kv) { return a_now - kv.second.lastSeen > kForgetAfter; });
		}

		// ------------------------------------------------------------------
		// The fix
		// ------------------------------------------------------------------

		const RE::ActorState* StateOf(const RE::Actor* a_actor)
		{
			return static_cast<const RE::ActorState*>(a_actor);
		}

		bool IsRestrained(const RE::Actor* a_actor)
		{
			return StateOf(a_actor)->lifeState == static_cast<std::uint32_t>(RE::LIFE_STATE::kRestrained);
		}

		enum class Blocked
		{
			kNo,
			kRestrained,
			kSmall
		};

		// An NPC standing still (or held in place) whose animation graph asks for a turn: the request is cleared
		// before the graph acts on it, so the turn-in-place step never starts. Held NPCs can't turn at all (their
		// feet step round and snap back), so theirs is always cleared; anyone else only for small turns, so they
		// still turn round to face you. Never with a weapon out (combat, aiming).
		// Never called with g_lock held: it talks to the animation graph.
		Blocked Prevent(RE::Actor* a_actor)
		{
			if (!Settings::bPreventIdleTurns || g_turnVarNames.empty() || !a_actor || a_actor->IsPlayerRef()) {
				return Blocked::kNo;
			}
			const auto* st = StateOf(a_actor);
			const auto  life = static_cast<RE::LIFE_STATE>(st->lifeState);
			if (life != RE::LIFE_STATE::kAlive && life != RE::LIFE_STATE::kRestrained) {
				return Blocked::kNo;
			}
			if (st->GetWeaponMagicDrawn()) {
				return Blocked::kNo;
			}

			const bool held = life == RE::LIFE_STATE::kRestrained && Settings::bRestrainedNeverTurn;
			if (!held) {
				if (Settings::fBlockBelowDeg <= 0.0f || !g_speedVar) {
					return Blocked::kNo;
				}
				float speed = 0.0f;
				if (!a_actor->GetGraphVariableImplFloat(*g_speedVar, speed) || speed > Settings::fStandingSpeed) {
					return Blocked::kNo;
				}
			}

			float turn = 0.0f;
			if (!a_actor->GetGraphVariableImplFloat(g_turnVarNames[0], turn) || turn == 0.0f || std::isnan(turn)) {
				return Blocked::kNo;
			}
			if (!held && Deg(std::abs(turn)) >= Settings::fBlockBelowDeg) {
				return Blocked::kNo;  // a real turn (to face you, say): let it happen
			}
			for (const auto& var : g_turnVarNames) {
				float v = 0.0f;
				if (a_actor->GetGraphVariableImplFloat(var, v) && v != 0.0f) {
					a_actor->SetGraphVariableFloat(var, 0.0f);
				}
			}
			if (held) {
				++g_blockedRestrained;
				return Blocked::kRestrained;
			}
			++g_blockedSmall;
			return Blocked::kSmall;
		}

		void NoteBlocked(RE::TESFormID a_id)
		{
			std::scoped_lock lock(g_lock);
			if (const auto it = g_actors.find(a_id); it != g_actors.end()) {
				++it->second.blocked;
			}
		}

		// Turn requests from the graph, logged whether or not the body turned (a held NPC's never does: the
		// shuffle is all in the feet). The values are read before the fix clears them, so this shows what the
		// game asked for.
		void TrackGraphTurn(std::uint32_t a_formID, State& a_state, const Sample& a_s, bool a_stationary, float a_bearing, std::vector<std::string>& a_out)
		{
			if (g_tdIndex < 0 || static_cast<std::size_t>(g_tdIndex) >= a_state.kinds.size() || a_state.kinds[g_tdIndex] != VarKind::kFloat) {
				return;
			}
			const float td = a_s.vars[g_tdIndex];
			if (std::isnan(td)) {
				return;
			}
			const double now = a_s.t;

			auto close = [&](const char* a_how) {
				a_state.gtActive = false;
				const float bodyTurned = WrapPi(a_s.heading - a_state.gtHeading);
				if (Deg(std::abs(a_state.gtPeak)) < Settings::fMinTurnDeg) {
					return;
				}
				++a_state.graphTurns;
				++g_graphTurns;
				if (!Settings::bLogEpisodes) {
					return;
				}
				a_out.push_back(std::format(
					"[{:08X} {}] GRAPH TURN {}: asked {:+.1f}deg for {:.2f}s, body turned {:+.1f}deg | {} | fix cleared it {} frames | player {:.0f}u at {:+.0f}deg ({} the player) | events: {} | npc graph turns {}",
					a_formID, a_state.name, a_how, Deg(a_state.gtPeak), now - a_state.gtStart, Deg(bodyTurned),
					a_state.gtRestrained ? "RESTRAINED (can't turn)" : "standing", a_state.blocked, a_state.gtDist, Deg(a_state.gtRel),
					(a_state.gtPeak > 0.0f) == (a_state.gtRel > 0.0f) ? "toward" : "away from",
					EventSummary(a_state, a_state.gtStart - 0.3, now + 0.05), a_state.graphTurns));
			};

			if (!a_state.gtActive) {
				if (std::abs(td) > 0.05f && (a_stationary || a_s.restrained)) {
					a_state.gtActive = true;
					a_state.gtStart = now;
					a_state.gtQuietSince = 0.0;
					a_state.gtPeak = td;
					a_state.gtHeading = a_s.heading;
					a_state.gtRel = WrapPi(a_bearing - a_s.heading);
					a_state.gtDist = a_s.dist;
					a_state.gtRestrained = a_s.restrained;
					a_state.blocked = 0;
				}
				return;
			}
			if (std::abs(td) > std::abs(a_state.gtPeak)) {
				a_state.gtPeak = td;
			}
			a_state.gtRestrained = a_state.gtRestrained || a_s.restrained;
			if (std::abs(td) < 0.02f) {
				if (a_state.gtQuietSince == 0.0) {
					a_state.gtQuietSince = now;
				} else if (now - a_state.gtQuietSince > 0.3) {
					close("done");
				}
			} else {
				a_state.gtQuietSince = 0.0;
			}
			if (a_state.gtActive && now - a_state.gtStart > 10.0) {
				close("STILL ASKING after 10s");  // (logged every 10s while it lasts)
			}
		}

		void Flush(std::vector<std::string>& a_lines)
		{
			for (const auto& line : a_lines) {
				logger::info("{}", line);
			}
		}

		// ------------------------------------------------------------------
		// Per-frame sampling
		// ------------------------------------------------------------------

		void OnUpdate(RE::Actor* a_actor, float a_delta)
		{
			if (a_actor->IsPlayerRef()) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}

			const auto& pos = a_actor->data.location;
			const auto& ppos = player->data.location;
			const float dx = ppos.x - pos.x;
			const float dy = ppos.y - pos.y;
			const float dz = ppos.z - pos.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist > Settings::fRadius) {
				return;
			}

			const double now = Now();
			const float  heading = a_actor->data.angle.z;
			const float  bearing = std::atan2(dx, dy);  // 0 = north, clockwise, same as heading
			const auto   formID = a_actor->GetFormID();

			// Phase 1 (locked): what does this NPC still need?
			bool                 needName = false;
			bool                 needProbe = false;
			bool                 stationary = false;
			std::vector<VarKind> kinds;
			{
				std::scoped_lock lock(g_lock);
				auto& state = g_actors[formID];
				needName = state.name.empty();
				const bool anyVar = std::ranges::any_of(state.kinds, [](VarKind k) { return k != VarKind::kNone; });
				if (!anyVar && now - state.probedAt > 2.0) {
					needProbe = true;
					state.probedAt = now;
				}
				kinds = state.kinds;

				if (const auto* old = SampleAtOrBefore(state, now - kStationaryWindow); old && now - old->t >= kStationaryWindow * 0.8) {
					const float mx = pos.x - old->x;
					const float my = pos.y - old->y;
					stationary = std::sqrt(mx * mx + my * my) < kStationaryDist;
				}
			}

			// Phase 2 (unlocked): talk to the engine. The graph can send anim events
			// (which take g_lock) while it holds its own locks, so never call into it
			// with g_lock held.
			std::string name;
			if (needName) {
				const char* full = a_actor->GetDisplayFullName();
				name = full && *full ? full : "<unnamed>";
			}
			if (needProbe) {
				kinds.assign(g_varNames.size(), VarKind::kNone);
				for (std::size_t i = 0; i < g_varNames.size(); ++i) {
					kinds[i] = Probe(a_actor, g_varNames[i]);
				}
			}

			Sample s{};
			s.t = now;
			s.heading = heading;
			s.x = pos.x;
			s.y = pos.y;
			s.z = pos.z;
			s.dist = dist;
			s.relPlayer = WrapPi(bearing - heading);
			s.dt = a_delta;
			for (std::size_t i = 0; i < kMaxVars; ++i) {
				s.vars[i] = i < g_varNames.size() && i < kinds.size() ? ReadVar(a_actor, g_varNames[i], kinds[i]) : std::nanf("");
			}

			s.restrained = IsRestrained(a_actor);

			// Phase 3 (locked): bookkeeping
			std::vector<std::string> lines;
			{
				std::scoped_lock lock(g_lock);
				auto& state = g_actors[formID];
				state.lastSeen = now;
				if (needName) {
					state.name = std::move(name);
				}
				if (needProbe) {
					state.kinds = kinds;
					if (!state.loggedVars && std::ranges::any_of(kinds, [](VarKind k) { return k != VarKind::kNone; })) {
						state.loggedVars = true;
						lines.push_back(std::format("tracking [{:08X} {}] graph vars: {}", formID, state.name, VarList(state)));
					}
				}

				const Sample* prev = state.samples.empty() ? nullptr : &state.samples.back();
				bool          turning = false;
				if (prev && now > prev->t) {
					const float dh = Deg(std::abs(WrapPi(heading - prev->heading)));
					turning = dh / static_cast<float>(now - prev->t) > kTurnRateDeg && dh > 0.02f;
				}

				if (a_delta > 0.0f && a_delta < 1.0f) {
					g_dtSum += a_delta;
					g_dtMin = std::min(g_dtMin, a_delta);
					g_dtMax = std::max(g_dtMax, a_delta);
					++g_dtCount;
				}

				if (!state.inEpisode && turning && stationary && prev) {
					state.inEpisode = true;
					state.epStart = prev->t;
					state.lastTurnT = now;
					state.startHeading = prev->heading;
					state.peakDev = 0.0f;
					state.startRel = WrapPi(bearing - prev->heading);
					state.startDist = dist;
					state.moved = false;
					state.samples.push_back(s);
				} else if (state.inEpisode) {
					state.samples.push_back(s);
					const float dev = WrapPi(heading - state.startHeading);
					if (std::abs(dev) > std::abs(state.peakDev)) {
						state.peakDev = dev;
					}
					if (turning) {
						state.lastTurnT = now;
					}
					if (!stationary) {
						state.moved = true;
					}
					if (now - state.lastTurnT > kStillAfter || now - state.epStart > kMaxEpisode) {
						CloseEpisode(formID, state, now, lines);
					}
				} else {
					state.samples.push_back(s);
				}

				TrackGraphTurn(formID, state, s, stationary, bearing, lines);

				Trim(state, now);
				Stats(now, lines);
			}
			Flush(lines);
		}

		void OnAnimEvent(RE::TESObjectREFR* a_ref, const RE::BSAnimationGraphEvent& a_event)
		{
			if (!a_ref) {
				return;
			}
			const char* tag = a_event.tag.c_str();
			const char* payload = a_event.payload.c_str();

			std::scoped_lock lock(g_lock);
			const auto it = g_actors.find(a_ref->GetFormID());
			if (it == g_actors.end()) {
				return;
			}
			const double now = Now();
			if (now - it->second.lastSeen > 1.0) {
				return;  // out of range now
			}
			const float heading = a_ref->data.angle.z;
			it->second.events.push_back({ now, heading, tag ? tag : "", payload ? payload : "" });
			if (Settings::bLogAllEvents) {
				logger::info("[{:08X} {}] event {}{}{} hdg {:.1f}", a_ref->GetFormID(), it->second.name, tag ? tag : "",
					payload && *payload ? " payload=" : "", payload ? payload : "", Deg(heading));
			}
		}

		// ------------------------------------------------------------------
		// Hooks
		// ------------------------------------------------------------------

		using AnimSink = RE::BSTEventSink<RE::BSAnimationGraphEvent>;
		using ProcessEvent_t = RE::BSEventNotifyControl (*)(AnimSink*, const RE::BSAnimationGraphEvent&, RE::BSTEventSource<RE::BSAnimationGraphEvent>*);
		std::atomic<ProcessEvent_t> g_origProcessEvent{ nullptr };

		RE::BSEventNotifyControl ProcessEventThunk(AnimSink* a_this, const RE::BSAnimationGraphEvent& a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			OnAnimEvent(static_cast<RE::TESObjectREFR*>(a_this), a_event);
			return g_origProcessEvent.load()(a_this, a_event, a_source);
		}

		// The Actor anim-event sink vtable is found from a live NPC (the sub-object
		// offset comes from CommonLib's class layout, so no vtable index is guessed).
		void HookAnimEvents(RE::Actor* a_actor)
		{
			auto*       sink = static_cast<AnimSink*>(a_actor);
			auto* const vtbl = *reinterpret_cast<std::uintptr_t**>(sink);

			int match = -1;
			for (std::size_t i = 0; i < RE::VTABLE::Actor.size(); ++i) {
				REL::Relocation<std::uintptr_t> candidate{ RE::VTABLE::Actor[i] };
				if (candidate.address() == reinterpret_cast<std::uintptr_t>(vtbl)) {
					match = static_cast<int>(i);
					break;
				}
			}
			if (match < 0) {
				logger::warn("anim event sink vtable {:X} isn't one of Actor's; anim events won't be logged",
					reinterpret_cast<std::uintptr_t>(vtbl) - REL::Module::get().base());
				return;
			}

			g_origProcessEvent = reinterpret_cast<ProcessEvent_t>(vtbl[kProcessEventVfunc]);
			REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtbl[kProcessEventVfunc]), reinterpret_cast<std::uintptr_t>(&ProcessEventThunk));
			logger::info("hooked Actor anim event sink (vtable #{} of Actor)", match);
		}

		// Which of Actor's vtables a sub-object of a live NPC points at (-1: none, leave it alone)
		int ActorVtableIndex(const void* a_subobject)
		{
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(a_subobject);
			for (std::size_t i = 0; i < RE::VTABLE::Actor.size(); ++i) {
				REL::Relocation<std::uintptr_t> candidate{ RE::VTABLE::Actor[i] };
				if (candidate.address() == vtbl) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		// IAnimationGraphManagerHolder::PreUpdateAnimationGraphManager: right before the NPC's graph updates, after
		// the game has set this frame's variables. Clearing the turn request here beats the game rewriting it.
		constexpr std::size_t kPreUpdateVfunc = 0x16;
		using PreUpdate_t = void (*)(const RE::IAnimationGraphManagerHolder*, const RE::BSTSmartPointer<RE::BSAnimationGraphManager>&);
		std::atomic<PreUpdate_t> g_origPreUpdate{ nullptr };

		void PreUpdateThunk(const RE::IAnimationGraphManagerHolder* a_this, const RE::BSTSmartPointer<RE::BSAnimationGraphManager>& a_mgr)
		{
			g_origPreUpdate.load()(a_this, a_mgr);
			++g_preUpdateCalls;
			if (!a_this) {
				return;
			}
			auto* actor = const_cast<RE::Actor*>(static_cast<const RE::Actor*>(static_cast<const RE::TESObjectREFR*>(a_this)));
			if (Prevent(actor) != Blocked::kNo) {
				++g_blockedInPreUpdate;
			}
		}

		void HookPreUpdate(RE::Actor* a_actor)
		{
			auto*      holder = static_cast<RE::IAnimationGraphManagerHolder*>(a_actor);
			const auto match = ActorVtableIndex(holder);
			if (match < 0) {
				logger::warn("anim graph holder vtable isn't one of Actor's; turn requests only cleared from Actor::Update");
				return;
			}
			auto* const vtbl = *reinterpret_cast<std::uintptr_t**>(holder);
			g_origPreUpdate = reinterpret_cast<PreUpdate_t>(vtbl[kPreUpdateVfunc]);
			REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtbl[kPreUpdateVfunc]), reinterpret_cast<std::uintptr_t>(&PreUpdateThunk));
			logger::info("hooked Actor anim graph pre-update (vtable #{} of Actor)", match);
		}

		void LateInit(RE::Actor* a_actor)
		{
			for (const auto& name : Settings::sGraphVars) {
				if (g_varNames.size() >= kMaxVars) {
					logger::warn("only the first {} sGraphVars are sampled", kMaxVars);
					break;
				}
				if (std::ranges::equal(name, "TurnDelta"sv, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
					g_tdIndex = static_cast<int>(g_varNames.size());
				}
				g_varNames.emplace_back(name);
			}
			for (const auto& name : Settings::sTurnVars) {
				g_turnVarNames.emplace_back(name);
			}
			g_speedVar.emplace("Speed");

			for (const auto* dll : { "HighFPSPhysicsFix.dll", "HighFPSFix.dll", "Buffout4.dll", "fo4lipsync.dll", "fo4settlementperformance.dll", "WastelandWagers.dll" }) {
				logger::info("module {}: {}", dll, REX::W32::GetModuleHandleA(dll) ? "loaded" : "not loaded");
			}

			HookAnimEvents(a_actor);
			if (Settings::bPreventIdleTurns) {
				HookPreUpdate(a_actor);
				logger::info("fix ON: standing NPCs' turn requests cleared ({}; others below {:.0f}deg while Speed <= {:.1f}; never with a weapon out)",
					Settings::bRestrainedNeverTurn ? "always for restrained NPCs" : "restrained NPCs like anyone else", Settings::fBlockBelowDeg,
					Settings::fStandingSpeed);
			} else {
				logger::info("fix OFF: logging only");
			}
			g_lastStats = Now();
			logger::info("watching NPCs within {:.0f} units of the player", Settings::fRadius);
		}

		struct UpdateHook
		{
			static void thunk(RE::Actor* a_this, float a_delta)
			{
				func(a_this, a_delta);
				if (!a_this) {
					return;
				}
				std::call_once(g_initOnce, LateInit, a_this);
				OnUpdate(a_this, a_delta);  // (reads what the game asked for first)
				if (Prevent(a_this) != Blocked::kNo) {
					NoteBlocked(a_this->GetFormID());
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::Actor[0] };
		UpdateHook::func = vtbl.write_vfunc(kUpdateVfunc, UpdateHook::thunk);
		logger::info("hooked Actor::Update");
	}
}
