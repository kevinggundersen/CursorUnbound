#pragma once

namespace CursorUnbound
{
	// How cursorPosX/Y should be interpreted when we write an absolute position.
	enum class CoordinateSpace
	{
		// Trust MenuCursor::screenWidthX/Y if they look sane, otherwise fall back to the
		// window client size. This is what you want unless something is visibly off.
		kAuto,
		// Always use MenuCursor::screenWidthX/Y.
		kGame,
		// Always use the window client size in pixels.
		kClient,
	};

	// Which mechanism is used to suppress the game's own Scaleform cursor sprite.
	// Different cursor replacer mods ship differently structured cursormenu.swf files, so
	// no single method is guaranteed. kAll applies every one of them.
	enum class HideMethod
	{
		kAll,          // SetVisible + _root._visible + _root._alpha
		kSetVisible,   // GFxMovie::SetVisible(false) only
		kRootVisible,  // _root._visible = false only
		kRootAlpha,    // _root._alpha = 0 only
		kViewport,     // push the movie's viewport off the render surface (no ActionScript)
		kRender,       // skip the cursor movie's draw call outright (ignores every flag)
	};

	// Whether to neutralize PrismaUI's own cursor sprite.
	enum class PrismaSuppression
	{
		// Suppress only while we are actually drawing a hardware cursor. This is the setting
		// that avoids two pointers without taking Prisma's away from anyone who is not using
		// ours.
		kAuto,
		kOn,
		kOff,
	};

	struct Config
	{
		// [General]
		bool        enabled = true;
		std::string logLevel = "info";

		// [Cursor]
		bool        useHardwareCursor = true;
		bool        hideGameCursor = true;
		// kRender is the default because it is the only method that reliably suppresses the
		// cursor across UI replacers - the others can report success while the cursor is
		// still drawn, because the game re-shows it every frame.
		HideMethod hideMethod = HideMethod::kRender;
		std::string cursorFile;  // empty => auto-detect inside SKSE/Plugins/CursorUnbound
		int         hotspotX = 0;
		int         hotspotY = 0;
		float       scale = 1.0f;

		// [Behavior]
		bool            absolutePositioning = true;
		// Zero the mouse delta for the duration of the game's own cursor handler, so it
		// cannot integrate on top of the absolute position we just wrote. The delta is
		// restored immediately afterwards so other menu handlers still see it.
		bool            neutralizeGameDelta = true;
		bool            clipToWindow = true;
		bool            blockGameCursorHide = true;
		// Keep re-hiding the OS cursor while no menu wants it. The display counter is a
		// process-wide resource that the game, SSEDisplayTweaks and other SKSE plugins all
		// write to, so hiding once on menu close is not enough - a single stray
		// ShowCursor(TRUE) afterwards strands the pointer on screen for the rest of the
		// session.
		bool            enforceHiddenWhenInactive = true;
		// Patch USER32!ShowCursor in every loaded module's import table, not just the game
		// executable's. Without this, hide calls coming from another DLL bypass the hook.
		bool            hookAllModules = true;
		bool            syncOnMenuOpen = true;
		// PrismaUI draws its own cursor as a DirectX sprite in the render loop, positioned
		// from MenuCursor::cursorPosX/Y. It therefore inherits our absolute position but is
		// still drawn at frame rate, so alongside the hardware cursor it reads as a second
		// pointer lagging the first.
		PrismaSuppression suppressPrismaCursor = PrismaSuppression::kAuto;
		CoordinateSpace coordinateSpace = CoordinateSpace::kAuto;
		// Manual escape hatch. When > 0 these override whatever the coordinate space
		// resolution would have picked.
		float spanX = 0.0f;
		float spanY = 0.0f;

		// [Debug]
		// Logs the range of cursor positions the game itself produces, which is how you
		// verify the coordinate space without guessing.
		bool logCursorRange = false;
		// Periodic state summaries. Off by default so a normal session logs a handful of
		// lines rather than hundreds.
		bool verbose = false;

		static Config& Get();

		void Load(const std::filesystem::path& a_path);
	};
}
