#pragma once

namespace CursorUnbound
{
	// Installs the CursorMenu::ProcessMouseMove vtable detour and the USER32!ShowCursor
	// IAT patch. Safe to call once, from SKSE's kPostLoad.
	void InstallHooks();

	// Resolves the game window, loads the cursor art and hooks the window procedure.
	// Needs the window to exist, so it runs at kDataLoaded.
	void InitializeRuntime();

	// Restores the window procedure and OS cursor state.
	void Shutdown();

	// Called from the MenuOpenCloseEvent sink.
	void OnMenuOpenClose(std::string_view a_menuName, bool a_opening);

	// Called from the SKSE message listener registered against Skyrim Party Sheet. Handles
	// both of its message types; anything else is ignored. Safe to call when Party Sheet is
	// not installed, which is simply the case where it is never called at all.
	void OnPartySheetMessage(std::uint32_t a_type, void* a_data, std::uint32_t a_dataLen);
}
