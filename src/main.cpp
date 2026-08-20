#include "Config.h"
#include "CursorUnbound.h"

namespace
{
	void InitializeLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}

		*path /= "CursorUnbound.log";

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));

		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);

		spdlog::set_default_logger(std::move(logger));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void ApplyLogLevel()
	{
		const auto& level = CursorUnbound::Config::Get().logLevel;

		auto parsed = spdlog::level::info;
		if (level == "trace") {
			parsed = spdlog::level::trace;
		} else if (level == "debug") {
			parsed = spdlog::level::debug;
		} else if (level == "warn" || level == "warning") {
			parsed = spdlog::level::warn;
		} else if (level == "error") {
			parsed = spdlog::level::err;
		} else if (level == "off" || level == "none") {
			parsed = spdlog::level::off;
		}

		spdlog::default_logger()->set_level(parsed);
		spdlog::default_logger()->flush_on(parsed);
	}

	class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuWatcher* GetSingleton()
		{
			static MenuWatcher instance;
			return &instance;
		}

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent*                a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event) {
				CursorUnbound::OnMenuOpenClose(a_event->menuName.c_str(), a_event->opening);
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		MenuWatcher() = default;
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			{
				if (!CursorUnbound::Config::Get().enabled) {
					SKSE::log::info("Disabled via config; not initializing.");
					return;
				}

				CursorUnbound::InitializeRuntime();

				if (auto* ui = RE::UI::GetSingleton()) {
					ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::GetSingleton());
					SKSE::log::info("Menu event sink registered.");
				} else {
					SKSE::log::error("UI singleton unavailable; menu events will not be received.");
				}
			}
			break;

		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 0, 0 },
	.Name = "CursorUnbound"sv,
	.Author = "kevin"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLogging();

	SKSE::Init(a_skse);

	CursorUnbound::Config::Get().Load("Data/SKSE/Plugins/CursorUnbound.ini");
	ApplyLogLevel();

	SKSE::log::info(
		"CursorUnbound 1.0.0 loading (runtime {}).",
		a_skse->RuntimeVersion().string());

	if (!CursorUnbound::Config::Get().enabled) {
		SKSE::log::info("Disabled via config.");
		return true;
	}

	CursorUnbound::InstallHooks();

	const auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("Failed to register the SKSE message listener.");
		return false;
	}

	return true;
}
