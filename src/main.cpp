#include "Config.h"
#include "CursorUnbound.h"
#include "PartySheetAPI.h"

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

	// Skyrim Party Sheet dispatches under its own sender name, so this cannot be folded into
	// OnMessage: RegisterListener with a callback alone subscribes to sender "SKSE" and
	// nothing else, and a listener registered for one sender never sees another's traffic.
	void OnPartySheetMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		CursorUnbound::OnPartySheetMessage(a_message->type, a_message->data, a_message->dataLen);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			{
				// Not in SKSEPluginLoad, which is where this would naturally go. SKSE resolves
				// a named sender to a plugin handle at registration time and fails outright if
				// that plugin is not loaded yet - and plugins load in filename order, so
				// CursorUnbound.dll is loaded before SkyrimPartySheet.dll every time. Doing it
				// here, once every plugin is loaded and before Party Sheet dispatches its
				// interface at kPostPostLoad, is the only ordering that works.
				//
				// Failure is the ordinary case when Party Sheet is not installed, and nothing
				// else is affected by it. CommonLibSSE logs its own "Failed to register
				// messaging listener" at error level from inside RegisterListener before we
				// get to answer, so the info line below exists to say that line was expected.
				if (const auto* messaging = SKSE::GetMessagingInterface();
					messaging && messaging->RegisterListener(PartySheetAPI::kSender, OnPartySheetMessage)) {
					SKSE::log::info("Listening for Skyrim Party Sheet's panel state.");
				} else {
					SKSE::log::info(
						"Skyrim Party Sheet is not installed; not listening for its panel state.");
				}
			}
			break;

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
	.Version = REL::Version{
		CURSOR_UNBOUND_VERSION_MAJOR,
		CURSOR_UNBOUND_VERSION_MINOR,
		CURSOR_UNBOUND_VERSION_PATCH,
		0 },
	.Name = "CursorUnbound"sv,
	.Author = "Datsferg"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLogging();

	SKSE::Init(a_skse);

	CursorUnbound::Config::Get().Load("Data/SKSE/Plugins/CursorUnbound.ini");
	ApplyLogLevel();

	SKSE::log::info(
		"CursorUnbound " CURSOR_UNBOUND_VERSION_STRING " loading (runtime {}).",
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
