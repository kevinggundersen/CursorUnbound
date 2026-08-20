#include "Config.h"

#include <charconv>
#include <fstream>

namespace CursorUnbound
{
	namespace
	{
		std::string Trim(std::string_view a_in)
		{
			const auto first = a_in.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_in.find_last_not_of(" \t\r\n");
			return std::string(a_in.substr(first, last - first + 1));
		}

		// Paths get pasted in with surrounding quotes constantly. Without this they end up
		// as part of the filename, and an absolute path stops looking absolute.
		std::string StripQuotes(std::string a_in)
		{
			if (a_in.size() >= 2) {
				const auto front = a_in.front();
				const auto back = a_in.back();
				if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
					return a_in.substr(1, a_in.size() - 2);
				}
			}
			return a_in;
		}

		std::string ToLower(std::string a_in)
		{
			std::transform(a_in.begin(), a_in.end(), a_in.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_in;
		}

		bool ParseBool(const std::string& a_value, bool a_fallback)
		{
			const auto v = ToLower(a_value);
			if (v == "true" || v == "1" || v == "yes" || v == "on") {
				return true;
			}
			if (v == "false" || v == "0" || v == "no" || v == "off") {
				return false;
			}
			return a_fallback;
		}

		int ParseInt(const std::string& a_value, int a_fallback)
		{
			try {
				return std::stoi(a_value);
			} catch (...) {
				return a_fallback;
			}
		}

		float ParseFloat(const std::string& a_value, float a_fallback)
		{
			try {
				return std::stof(a_value);
			} catch (...) {
				return a_fallback;
			}
		}
	}

	Config& Config::Get()
	{
		static Config instance;
		return instance;
	}

	void Config::Load(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			SKSE::log::info("No config at {}, using defaults.", a_path.string());
			return;
		}

		std::ifstream file(a_path);
		if (!file) {
			SKSE::log::warn("Could not open config at {}, using defaults.", a_path.string());
			return;
		}

		std::string section;
		std::string line;
		while (std::getline(file, line)) {
			// Strip comments. Both ';' and '#' are accepted since modders use either.
			const auto comment = line.find_first_of(";#");
			if (comment != std::string::npos) {
				line.erase(comment);
			}

			const auto trimmed = Trim(line);
			if (trimmed.empty()) {
				continue;
			}

			if (trimmed.front() == '[' && trimmed.back() == ']') {
				section = ToLower(trimmed.substr(1, trimmed.size() - 2));
				continue;
			}

			const auto eq = trimmed.find('=');
			if (eq == std::string::npos) {
				continue;
			}

			const auto key = ToLower(Trim(std::string_view(trimmed).substr(0, eq)));
			const auto value = StripQuotes(Trim(std::string_view(trimmed).substr(eq + 1)));

			if (section == "general") {
				if (key == "enabled") {
					enabled = ParseBool(value, enabled);
				} else if (key == "loglevel") {
					logLevel = ToLower(value);
				}
			} else if (section == "cursor") {
				if (key == "usehardwarecursor") {
					useHardwareCursor = ParseBool(value, useHardwareCursor);
				} else if (key == "hidegamecursor") {
					hideGameCursor = ParseBool(value, hideGameCursor);
				} else if (key == "hidemethod") {
					const auto v = ToLower(value);
					if (v == "setvisible") {
						hideMethod = HideMethod::kSetVisible;
					} else if (v == "rootvisible") {
						hideMethod = HideMethod::kRootVisible;
					} else if (v == "rootalpha") {
						hideMethod = HideMethod::kRootAlpha;
					} else if (v == "viewport") {
						hideMethod = HideMethod::kViewport;
					} else if (v == "render") {
						hideMethod = HideMethod::kRender;
					} else {
						hideMethod = HideMethod::kAll;
					}
				} else if (key == "cursorfile") {
					cursorFile = value;
				} else if (key == "hotspotx") {
					hotspotX = ParseInt(value, hotspotX);
				} else if (key == "hotspoty") {
					hotspotY = ParseInt(value, hotspotY);
				} else if (key == "scale") {
					scale = ParseFloat(value, scale);
				}
			} else if (section == "behavior") {
				if (key == "absolutepositioning") {
					absolutePositioning = ParseBool(value, absolutePositioning);
				} else if (key == "neutralizegamedelta") {
					neutralizeGameDelta = ParseBool(value, neutralizeGameDelta);
				} else if (key == "cliptowindow") {
					clipToWindow = ParseBool(value, clipToWindow);
				} else if (key == "blockgamecursorhide") {
					blockGameCursorHide = ParseBool(value, blockGameCursorHide);
				} else if (key == "enforcehiddenwheninactive") {
					enforceHiddenWhenInactive = ParseBool(value, enforceHiddenWhenInactive);
				} else if (key == "hookallmodules") {
					hookAllModules = ParseBool(value, hookAllModules);
				} else if (key == "synconmenuopen") {
					syncOnMenuOpen = ParseBool(value, syncOnMenuOpen);
				} else if (key == "coordinatespace") {
					const auto v = ToLower(value);
					if (v == "game") {
						coordinateSpace = CoordinateSpace::kGame;
					} else if (v == "client") {
						coordinateSpace = CoordinateSpace::kClient;
					} else {
						coordinateSpace = CoordinateSpace::kAuto;
					}
				} else if (key == "spanx") {
					spanX = ParseFloat(value, spanX);
				} else if (key == "spany") {
					spanY = ParseFloat(value, spanY);
				}
			} else if (section == "debug") {
				if (key == "logcursorrange") {
					logCursorRange = ParseBool(value, logCursorRange);
				} else if (key == "verbose") {
					verbose = ParseBool(value, verbose);
				}
			}
		}

		// Clamp to something sane so a typo cannot produce a 4000x cursor or a zero-size one.
		scale = std::clamp(scale, 0.1f, 8.0f);

		SKSE::log::info("Config loaded from {}", a_path.string());
	}
}
