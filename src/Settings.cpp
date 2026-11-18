#include "Settings.h"

namespace Settings
{
	namespace
	{
		std::string Trim(std::string_view a_str)
		{
			const auto first = a_str.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_str.find_last_not_of(" \t\r\n");
			return std::string(a_str.substr(first, last - first + 1));
		}

		std::string Lower(std::string a_str)
		{
			std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_str;
		}

		bool ParseBool(const std::string& a_value, bool a_default)
		{
			const auto v = Lower(a_value);
			if (v == "1" || v == "true" || v == "on" || v == "yes") {
				return true;
			}
			if (v == "0" || v == "false" || v == "off" || v == "no") {
				return false;
			}
			return a_default;
		}

		float ParseFloat(const std::string& a_value, float a_default)
		{
			try {
				return std::stof(a_value);
			} catch (...) {
				return a_default;
			}
		}

		std::vector<std::string> ParseList(const std::string& a_value)
		{
			std::vector<std::string> out;
			std::size_t              start = 0;
			while (start <= a_value.size()) {
				const auto comma = a_value.find(',', start);
				const auto item = Trim(std::string_view(a_value).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
				if (!item.empty()) {
					out.push_back(item);
				}
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
			return out;
		}
	}

	void Load()
	{
		const std::filesystem::path path{ "Data/F4SE/Plugins/fo4idlefix.ini" };

		std::ifstream file(path);
		if (!file.is_open()) {
			logger::info("no ini at {}, using defaults", path.string());
		} else {
			std::string line;
			while (std::getline(file, line)) {
				const auto comment = line.find_first_of(";#");
				if (comment != std::string::npos) {
					line.erase(comment);
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}

				const auto key = Lower(Trim(std::string_view(line).substr(0, eq)));
				const auto value = Trim(std::string_view(line).substr(eq + 1));

				if (key == "benabled") {
					bEnabled = ParseBool(value, bEnabled);
				} else if (key == "blogepisodes") {
					bLogEpisodes = ParseBool(value, bLogEpisodes);
				} else if (key == "bdumpepisodes") {
					bDumpEpisodes = ParseBool(value, bDumpEpisodes);
				} else if (key == "blogallevents") {
					bLogAllEvents = ParseBool(value, bLogAllEvents);
				} else if (key == "fradius") {
					fRadius = std::clamp(ParseFloat(value, fRadius), 100.0f, 20000.0f);
				} else if (key == "fminturndeg") {
					fMinTurnDeg = std::clamp(ParseFloat(value, fMinTurnDeg), 0.5f, 90.0f);
				} else if (key == "fstatsinterval") {
					fStatsInterval = std::clamp(ParseFloat(value, fStatsInterval), 0.0f, 3600.0f);
				} else if (key == "imaxdumps") {
					iMaxDumps = std::clamp(static_cast<int>(ParseFloat(value, static_cast<float>(iMaxDumps))), 0, 100000);
				} else if (key == "sgraphvars") {
					if (auto list = ParseList(value); !list.empty()) {
						sGraphVars = std::move(list);
					}
				} else if (key == "bpreventidleturns") {
					bPreventIdleTurns = ParseBool(value, bPreventIdleTurns);
				} else if (key == "brestrainedneverturn") {
					bRestrainedNeverTurn = ParseBool(value, bRestrainedNeverTurn);
				} else if (key == "fblockbelowdeg") {
					fBlockBelowDeg = std::clamp(ParseFloat(value, fBlockBelowDeg), 0.0f, 180.0f);
				} else if (key == "fstandingspeed") {
					fStandingSpeed = std::clamp(ParseFloat(value, fStandingSpeed), 0.0f, 100.0f);
				} else if (key == "sturnvars") {
					if (auto list = ParseList(value); !list.empty()) {
						sTurnVars = std::move(list);
					}
				}
			}
		}

		std::string vars;
		for (const auto& v : sGraphVars) {
			vars += vars.empty() ? v : "," + v;
		}
		std::string turnVars;
		for (const auto& v : sTurnVars) {
			turnVars += turnVars.empty() ? v : "," + v;
		}
		logger::info(
			"settings: enabled={} episodes={} dumps={} allEvents={} radius={:.0f} minTurn={:.1f}deg stats={:.0f}s maxDumps={} vars=[{}] prevent={} restrainedNeverTurn={} blockBelow={:.0f}deg standingSpeed={:.1f} turnVars=[{}]",
			bEnabled, bLogEpisodes, bDumpEpisodes, bLogAllEvents, fRadius, fMinTurnDeg, fStatsInterval, iMaxDumps, vars,
			bPreventIdleTurns, bRestrainedNeverTurn, fBlockBelowDeg, fStandingSpeed, turnVars);
	}
}
