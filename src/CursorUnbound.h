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
}
