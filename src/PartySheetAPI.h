#pragma once

// Interop declarations for Skyrim Party Sheet's SKSE messaging API.
//
// Mirrors the subset of `includes/PartySheetStateAPI.h` from
// https://www.nexusmods.com/skyrimspecialedition/mods/167538 that this plugin consumes.
// Party Sheet is MIT licensed, Copyright (c) 2025 Rijosan - see THIRD-PARTY-NOTICES.md.
//
// This is an ABI contract, not ordinary code: the declaration order of the virtuals below
// IS the vtable layout, so the only safe edit is to append. Reordering or removing one
// silently calls the wrong function in another process's DLL.
//
// Nothing here makes Party Sheet a dependency. If it is not installed, no message ever
// arrives and every consumer of this header sees a null interface.

namespace PartySheetAPI
{
	inline constexpr const char* kSender = "SkyrimPartySheet";

	enum : std::uint32_t
	{
		kMsg_Interface = 0x50534831,  // dispatched at kPostPostLoad, data = IPartySheetState1*
		kMsg_State     = 0x50534832,  // dispatched on change, data = StateMsg*
	};

	enum class Panel : std::uint32_t
	{
		PartySheet     = 0,
		PartySheetV2   = 1,
		CharacterSheet = 2,
		InspectCard    = 3,
		SettingsMenu   = 4,
		Count          = 5,
	};

	enum class Reason : std::uint32_t
	{
		UserToggle = 0,
		MenuOpen   = 1,
	};

	enum class APIResult : std::uint8_t
	{
		OK = 0,
		AlreadySet,
		Refused_MenuOpen,
		Refused_BadHandle,
	};

	inline constexpr std::uint32_t PanelBit(Panel a_panel)
	{
		return 1u << static_cast<std::uint32_t>(a_panel);
	}

	struct StateMsg
	{
		std::uint32_t version;
		bool          hudHidden;
		Reason        reason;
		std::uint32_t openPanels;
	};

	class IPartySheetState1
	{
	public:
		virtual ~IPartySheetState1() = default;
		virtual std::uint32_t GetVersion() const = 0;
		virtual bool          IsHUDHidden() const = 0;
		virtual APIResult     RequestHUDHidden(SKSE::PluginHandle a_myPluginHandle,
											   bool a_hidden, Reason a_reason) = 0;
		virtual bool          IsPanelOpen(Panel a_panel) const = 0;
		virtual std::uint32_t GetOpenPanels() const = 0;
		virtual bool          IsFullscreenPanelOpen() const = 0;
	};

	// The panels that take over the screen and expect a pointer. Party Sheet's own
	// IsFullscreenPanelOpen() tests exactly this set; it is spelled out here so the mask can
	// also be applied to a cached StateMsg, which is all we have if the interface pointer
	// never arrived.
	//
	// InspectCard is deliberately absent - it is a passive HUD card, not something you click.
	inline constexpr std::uint32_t kInteractivePanelMask =
		PanelBit(Panel::PartySheet) | PanelBit(Panel::PartySheetV2) |
		PanelBit(Panel::CharacterSheet) | PanelBit(Panel::SettingsMenu);
}
