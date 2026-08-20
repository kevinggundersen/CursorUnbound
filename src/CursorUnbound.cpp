#include "CursorUnbound.h"

#include "Config.h"
#include "CursorImage.h"

namespace CursorUnbound
{
	namespace
	{
		// ---------------------------------------------------------------------------
		// State
		// ---------------------------------------------------------------------------

		std::atomic<bool> g_active{ false };     // a cursor-driven menu is on screen
		std::atomic<bool> g_runtimeReady{ false };

		HWND     g_window = nullptr;
		bool     g_windowIsUnicode = true;
		WNDPROC  g_originalWndProc = nullptr;
		HCURSOR  g_customCursor = nullptr;
		HCURSOR  g_fallbackCursor = nullptr;

		int(WINAPI* g_realShowCursor)(BOOL) = nullptr;

		void Activate();
		void Deactivate();

		// A gamepad drives the menu cursor through ProcessThumbstick, not ProcessMouseMove.
		// The hardware cursor cannot follow that - the OS pointer simply does not move - so
		// while a stick is driving the cursor we hand rendering back to the game entirely.
		std::atomic<bool> g_gamepadMode{ false };

		struct Diagnostics
		{
			std::uint64_t activations = 0;
			std::uint64_t entryVisible = 0;  // movie was visible again when we re-entered
			std::uint64_t entryHidden = 0;   // our previous hide was still in effect
			std::uintptr_t lastMovie = 0;
		};
		Diagnostics g_stats;

		// Calibration bookkeeping for [Debug] LogCursorRange.
		float         g_observedMinX = (std::numeric_limits<float>::max)();
		float         g_observedMaxX = std::numeric_limits<float>::lowest();
		float         g_observedMinY = (std::numeric_limits<float>::max)();
		float         g_observedMaxY = std::numeric_limits<float>::lowest();
		std::uint64_t g_lastRangeLogTick = 0;

		// ---------------------------------------------------------------------------
		// Window helpers
		// ---------------------------------------------------------------------------

		struct FindWindowCtx
		{
			DWORD pid;
			HWND  hwnd;
		};

		BOOL CALLBACK EnumWindowsProc(HWND a_hwnd, LPARAM a_lparam)
		{
			auto* ctx = reinterpret_cast<FindWindowCtx*>(a_lparam);

			DWORD pid = 0;
			::GetWindowThreadProcessId(a_hwnd, &pid);
			if (pid != ctx->pid) {
				return TRUE;
			}
			// Skip tool windows and anything owned - we want the top-level render window.
			if (!::IsWindowVisible(a_hwnd) || ::GetWindow(a_hwnd, GW_OWNER) != nullptr) {
				return TRUE;
			}

			ctx->hwnd = a_hwnd;
			return FALSE;
		}

		HWND ResolveGameWindow()
		{
			if (g_window && ::IsWindow(g_window)) {
				return g_window;
			}

			FindWindowCtx ctx{ ::GetCurrentProcessId(), nullptr };
			::EnumWindows(&EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
			g_window = ctx.hwnd;
			return g_window;
		}

		void ApplyClip(bool a_clip)
		{
			if (!a_clip) {
				::ClipCursor(nullptr);
				return;
			}

			HWND hwnd = ResolveGameWindow();
			if (!hwnd) {
				return;
			}

			RECT client{};
			if (!::GetClientRect(hwnd, &client)) {
				return;
			}

			POINT topLeft{ client.left, client.top };
			POINT bottomRight{ client.right, client.bottom };
			if (!::ClientToScreen(hwnd, &topLeft) || !::ClientToScreen(hwnd, &bottomRight)) {
				return;
			}

			RECT screenRect{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
			::ClipCursor(&screenRect);
		}

		// ---------------------------------------------------------------------------
		// OS cursor visibility
		//
		// ShowCursor maintains a display counter; the cursor is drawn only while it is
		// >= 0. The game drives it to -1 because it renders its own Scaleform pointer, so
		// we have to push it back up (and stop the game pushing it back down - see the
		// IAT patch below).
		// ---------------------------------------------------------------------------

		int RealShowCursor(BOOL a_show)
		{
			return g_realShowCursor ? g_realShowCursor(a_show) : ::ShowCursor(a_show);
		}

		// ShowCursor mutates the counter on every call, including calls that do not change
		// the visible state. Since ForceCursorShown runs from WM_SETCURSOR - i.e. on every
		// mouse move over the window - blindly calling ShowCursor(TRUE) would ratchet the
		// counter up without bound, and the bounded restore below would then never get it
		// back down. So query the state first and only touch the counter when it must move.
		bool IsCursorCurrentlyShown()
		{
			CURSORINFO info{};
			info.cbSize = sizeof(info);
			if (!::GetCursorInfo(&info)) {
				return false;
			}
			return (info.flags & CURSOR_SHOWING) != 0;
		}

		// Whether we are the ones currently holding the counter up.
		bool g_cursorShownByUs = false;

		void ForceCursorShown()
		{
			// Both signals are needed and neither is sufficient alone.
			//
			// GetCursorInfo reports the SYSTEM cursor state, not this thread's ShowCursor
			// counter. On the main menu and immediately after alt-tab the cursor reads as
			// "showing" while the game's counter is still negative, so testing it alone made
			// us skip the call that actually reveals the cursor - and with the Scaleform
			// cursor suppressed that left no cursor at all.
			//
			// Our own flag is what stops the counter ratcheting up on every mouse move.
			if (g_cursorShownByUs && IsCursorCurrentlyShown()) {
				return;
			}

			// Bounded so a runaway counter cannot spin us forever.
			for (int i = 0; i < 64; ++i) {
				if (RealShowCursor(TRUE) >= 0) {
					break;
				}
			}
			g_cursorShownByUs = true;
		}

		void ForceCursorHidden()
		{
			if (!g_cursorShownByUs && !IsCursorCurrentlyShown()) {
				return;
			}
			g_cursorShownByUs = false;
			for (int i = 0; i < 64; ++i) {
				if (RealShowCursor(FALSE) < 0) {
					return;
				}
			}
		}

		// Replaces USER32!ShowCursor in the game's import table. While we own the cursor we
		// swallow the game's hide requests.
		//
		// Returning -1 rather than 0 is deliberate: the game's own visibility helper is a
		// `do { count = ShowCursor(false); } while (count >= 0);` loop (mirrored in
		// CommonLibSSE's MenuCursor::SetCursorVisibility). Reporting "still visible" from a
		// swallowed call would hang the game in that loop.
		int WINAPI ShowCursorHook(BOOL a_show)
		{
			// Not while a gamepad owns the cursor - the game needs its own pointer back,
			// and blocking its hide calls there would leave the OS cursor stranded on screen.
			if (!a_show && g_active.load(std::memory_order_relaxed) &&
				!g_gamepadMode.load(std::memory_order_relaxed) &&
				Config::Get().blockGameCursorHide && Config::Get().useHardwareCursor) {
				return -1;
			}
			return RealShowCursor(a_show);
		}

		// ---------------------------------------------------------------------------
		// Scaleform cursor
		// ---------------------------------------------------------------------------

		// Cursor replacer mods ship differently structured cursormenu.swf files, and a
		// movie-level SetVisible does not necessarily suppress every one of them. So apply
		// the ActionScript-level hides too - _root._alpha in particular is structure
		// independent, since it dims the whole movie whatever is inside it.
		RE::GViewport g_savedViewport{};
		bool          g_viewportSaved = false;

		// ---------------------------------------------------------------------------
		// Draw-call suppression (HideMethod = render)
		//
		// Last resort for when the movie reports itself hidden but the cursor is still on
		// screen - i.e. something re-shows it every frame, or the custom render path for
		// CursorMenu ignores the visibility flag. Rather than fight over flags, skip the
		// movie's draw call.
		//
		// GFxMovieView's vtable is shared by every Scaleform movie in the game, so the
		// thunk filters by instance and only ever suppresses the one cursor movie.
		// ---------------------------------------------------------------------------

		constexpr std::size_t kDisplayVFunc = 0x26;
		constexpr std::size_t kDisplayPrePassVFunc = 0x27;

		// Two handles to the same movie, deliberately.
		//
		// The strong reference is what makes this safe: without it the movie could be freed
		// and a different movie allocated at the same address, and the raw comparison below
		// would then blank out the wrong movie. Holding a reference means the address cannot
		// be recycled while we are suppressing it.
		//
		// The raw atomic exists because Display() runs on the render thread and a GPtr
		// copy is not atomic. Publish order matters: take the reference before publishing
		// the pointer, and clear the pointer before dropping the reference, so a non-null
		// pointer always implies a live reference.
		RE::GPtr<RE::GFxMovieView>     g_suppressedMovieRef;
		std::atomic<RE::GFxMovieView*> g_suppressedMovie{ nullptr };

		void SuppressMovie(const RE::GPtr<RE::GFxMovieView>& a_movie)
		{
			g_suppressedMovieRef = a_movie;
			g_suppressedMovie.store(a_movie.get(), std::memory_order_release);
		}

		void ClearSuppressedMovie()
		{
			g_suppressedMovie.store(nullptr, std::memory_order_release);
			g_suppressedMovieRef.reset();
		}

		bool ShouldSuppress(RE::GFxMovieView* a_movie)
		{
			return a_movie && g_suppressedMovie.load(std::memory_order_acquire) == a_movie;
		}

		struct DisplayHook
		{
			static void thunk(RE::GFxMovieView* a_this)
			{
				if (ShouldSuppress(a_this)) {
					return;
				}
				func(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DisplayPrePassHook
		{
			static void thunk(RE::GFxMovieView* a_this)
			{
				if (ShouldSuppress(a_this)) {
					return;
				}
				func(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool PatchVFunc(std::uintptr_t* a_vtable, std::size_t a_index, void* a_replacement, std::uintptr_t& a_outOriginal)
		{
			DWORD previous = 0;
			if (!::VirtualProtect(&a_vtable[a_index], sizeof(std::uintptr_t), PAGE_READWRITE, &previous)) {
				return false;
			}
			a_outOriginal = a_vtable[a_index];
			a_vtable[a_index] = reinterpret_cast<std::uintptr_t>(a_replacement);
			::VirtualProtect(&a_vtable[a_index], sizeof(std::uintptr_t), previous, &previous);
			return true;
		}

		void EnsureDisplayHook(RE::GFxMovieView* a_sample)
		{
			static bool installed = false;
			if (installed || !a_sample) {
				return;
			}
			installed = true;

			auto* vtable = *reinterpret_cast<std::uintptr_t**>(a_sample);

			std::uintptr_t originalDisplay = 0;
			std::uintptr_t originalPrePass = 0;
			const bool     okDisplay = PatchVFunc(vtable, kDisplayVFunc, &DisplayHook::thunk, originalDisplay);
			const bool     okPrePass = PatchVFunc(vtable, kDisplayPrePassVFunc, &DisplayPrePassHook::thunk, originalPrePass);

			if (okDisplay) {
				DisplayHook::func = originalDisplay;
			}
			if (okPrePass) {
				DisplayPrePassHook::func = originalPrePass;
			}

			SKSE::log::info(
				"Installed Scaleform draw suppression (Display={}, DisplayPrePass={}).",
				okDisplay,
				okPrePass);
		}

		void SetScaleformCursorVisible(bool a_visible, bool a_log = true)
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				if (a_log) {
					SKSE::log::warn("Cannot reach the UI singleton to hide the game cursor.");
				}
				return;
			}

			auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME);
			if (!menu) {
				if (a_log) {
					SKSE::log::warn("Cursor Menu is not in the menu map; cannot hide the game cursor.");
				}
				return;
			}
			if (!menu->uiMovie) {
				// The menu object can exist before its movie is loaded, which is why this is
				// re-asserted periodically rather than only on menu open.
				if (a_log) {
					SKSE::log::warn("Cursor Menu has no uiMovie yet; will retry.");
				}
				return;
			}

			auto*      movie = menu->uiMovie.get();
			const auto method = Config::Get().hideMethod;

			// The state on ENTRY is the whole diagnostic. If the movie reports itself
			// visible again every time we come back, something in the game is re-showing it
			// between our calls and no amount of movie-level hiding will ever stick.
			const bool visibleOnEntry = movie->GetVisible();
			if (!a_visible) {
				++(visibleOnEntry ? g_stats.entryVisible : g_stats.entryHidden);
				g_stats.lastMovie = reinterpret_cast<std::uintptr_t>(movie);
			}

			bool setVisibleRan = false;
			int  rootVisibleResult = -1;
			int  rootAlphaResult = -1;

			if (method == HideMethod::kAll || method == HideMethod::kSetVisible) {
				movie->SetVisible(a_visible);
				setVisibleRan = true;
			}
			if (method == HideMethod::kAll || method == HideMethod::kRootVisible) {
				RE::GFxValue value(a_visible);
				rootVisibleResult = movie->SetVariable("_root._visible", value) ? 1 : 0;
			}
			if (method == HideMethod::kAll || method == HideMethod::kRootAlpha) {
				RE::GFxValue value(a_visible ? 100.0 : 0.0);
				rootAlphaResult = movie->SetVariable("_root._alpha", value) ? 1 : 0;
			}
			if (method == HideMethod::kRender) {
				EnsureDisplayHook(movie);
				if (a_visible) {
					ClearSuppressedMovie();
				} else {
					SuppressMovie(menu->uiMovie);
				}
			}
			if (method == HideMethod::kViewport) {
				if (!a_visible) {
					RE::GViewport current{};
					movie->GetViewport(&current);
					if (!g_viewportSaved) {
						g_savedViewport = current;
						g_viewportSaved = true;
					}
					// Push the movie's render target off the visible surface instead of
					// resizing it to zero, which risks a divide-by-zero inside Scaleform.
					current.left = current.bufferWidth + current.width;
					current.top = current.bufferHeight + current.height;
					movie->SetViewport(current);
				} else if (g_viewportSaved) {
					movie->SetViewport(g_savedViewport);
					g_viewportSaved = false;
				}
			}

			if (!a_log) {
				return;
			}

			// -1 means "not attempted" for the two SetVariable results.
			SKSE::log::info(
				"Game cursor visible={} | movie=0x{:X} visibleOnEntry={} | SetVisible ran={} "
				"GetVisible={} | _root._visible={} | _root._alpha={}",
				a_visible,
				reinterpret_cast<std::uintptr_t>(movie),
				visibleOnEntry,
				setVisibleRan,
				movie->GetVisible(),
				rootVisibleResult,
				rootAlphaResult);
		}

		// Dumps a compact summary instead of a line per call. The counters answer the only
		// question that matters: does our hide stay applied between calls, and if the cursor
		// is still on screen, which other menus are up that might be drawing it?
		void LogDiagnosticSummary()
		{
			if (!Config::Get().verbose) {
				return;
			}

			static std::uint64_t lastTick = 0;
			const auto           now = ::GetTickCount64();
			if (now - lastTick < 3000) {
				return;
			}
			lastTick = now;

			// Bounded so a long session does not accumulate thousands of lines. A couple of
			// minutes of activity is more than enough to diagnose anything.
			static int emitted = 0;
			if (emitted >= 40) {
				return;
			}
			++emitted;

			std::string openMenus;
			if (auto* ui = RE::UI::GetSingleton()) {
				for (const auto& entry : ui->menuMap) {
					if (entry.second.menu) {
						if (!openMenus.empty()) {
							openMenus += ", ";
						}
						openMenus += entry.first.c_str();
					}
				}
			}

			SKSE::log::info(
				"[diag] activations={} hides={} cursorMovieWasVisibleOnEntry={} wasHidden={} "
				"lastMovie=0x{:X} | menus: {}",
				g_stats.activations,
				g_stats.entryVisible + g_stats.entryHidden,
				g_stats.entryVisible,
				g_stats.entryHidden,
				g_stats.lastMovie,
				openMenus.empty() ? "<none>" : openMenus);
		}

		// The game re-shows its cursor on some menu transitions, so re-assert periodically
		// rather than only on menu open. Throttled because this runs off mouse movement.
		void ReassertScaleformHidden()
		{
			const auto& config = Config::Get();
			if (!config.useHardwareCursor || !config.hideGameCursor ||
				g_gamepadMode.load(std::memory_order_relaxed)) {
				return;
			}

			static std::uint64_t lastTick = 0;
			const auto           now = ::GetTickCount64();
			if (now - lastTick < 250) {
				return;
			}
			lastTick = now;

			// Report the first couple of attempts in full, then let the periodic summary
			// carry the story rather than flooding the log.
			static int attempts = 0;
			const bool logThis = Config::Get().verbose && attempts < 2;
			if (logThis) {
				++attempts;
			}

			SetScaleformCursorVisible(false, logThis);
			LogDiagnosticSummary();
		}

		// ---------------------------------------------------------------------------
		// Coordinate mapping
		// ---------------------------------------------------------------------------

		void ResolveSpan(const RE::MenuCursor& a_cursor, float a_clientW, float a_clientH, float& a_outX, float& a_outY)
		{
			const auto& config = Config::Get();

			if (config.spanX > 0.0f && config.spanY > 0.0f) {
				a_outX = config.spanX;
				a_outY = config.spanY;
				return;
			}

			const bool gameFieldsUsable =
				std::isfinite(a_cursor.screenWidthX) && std::isfinite(a_cursor.screenWidthY) &&
				a_cursor.screenWidthX >= 16.0f && a_cursor.screenWidthY >= 16.0f;

			switch (config.coordinateSpace) {
			case CoordinateSpace::kGame:
				a_outX = a_cursor.screenWidthX;
				a_outY = a_cursor.screenWidthY;
				break;
			case CoordinateSpace::kClient:
				a_outX = a_clientW;
				a_outY = a_clientH;
				break;
			case CoordinateSpace::kAuto:
			default:
				if (gameFieldsUsable) {
					a_outX = a_cursor.screenWidthX;
					a_outY = a_cursor.screenWidthY;
				} else {
					a_outX = a_clientW;
					a_outY = a_clientH;
				}
				break;
			}

			if (!(a_outX > 0.0f) || !(a_outY > 0.0f)) {
				a_outX = a_clientW;
				a_outY = a_clientH;
			}
		}

		void MaybeLogRange(const RE::MenuCursor& a_cursor)
		{
			if (!Config::Get().logCursorRange) {
				return;
			}

			g_observedMinX = (std::min)(g_observedMinX, a_cursor.cursorPosX);
			g_observedMaxX = (std::max)(g_observedMaxX, a_cursor.cursorPosX);
			g_observedMinY = (std::min)(g_observedMinY, a_cursor.cursorPosY);
			g_observedMaxY = (std::max)(g_observedMaxY, a_cursor.cursorPosY);

			const auto now = ::GetTickCount64();
			if (now - g_lastRangeLogTick < 5000) {
				return;
			}
			g_lastRangeLogTick = now;

			SKSE::log::info(
				"[calibration] game cursorPos X:[{:.1f}, {:.1f}] Y:[{:.1f}, {:.1f}] | "
				"screenWidth ({:.1f}, {:.1f}) safeZone ({:.1f}, {:.1f}) sensitivity {:.3f}",
				g_observedMinX, g_observedMaxX, g_observedMinY, g_observedMaxY,
				a_cursor.screenWidthX, a_cursor.screenWidthY,
				a_cursor.safeZoneX, a_cursor.safeZoneY,
				a_cursor.cursorSensitivity);
		}

		// Maps the current OS cursor position into the game's cursor coordinate space.
		// Kept separate from the write so a single sampled position can be applied more
		// than once around the game's own handler without re-reading the mouse.
		bool ComputeAbsolutePosition(float& a_outX, float& a_outY)
		{
			auto* menuCursor = RE::MenuCursor::GetSingleton();
			if (!menuCursor) {
				return false;
			}

			HWND hwnd = ResolveGameWindow();
			if (!hwnd) {
				return false;
			}

			POINT point{};
			if (!::GetCursorPos(&point) || !::ScreenToClient(hwnd, &point)) {
				return false;
			}

			RECT client{};
			if (!::GetClientRect(hwnd, &client)) {
				return false;
			}

			const auto clientW = static_cast<float>(client.right - client.left);
			const auto clientH = static_cast<float>(client.bottom - client.top);
			if (clientW <= 0.0f || clientH <= 0.0f) {
				return false;
			}

			const float normalizedX = std::clamp(static_cast<float>(point.x) / clientW, 0.0f, 1.0f);
			const float normalizedY = std::clamp(static_cast<float>(point.y) / clientH, 0.0f, 1.0f);

			float spanX = 0.0f;
			float spanY = 0.0f;
			ResolveSpan(*menuCursor, clientW, clientH, spanX, spanY);

			a_outX = normalizedX * spanX;
			a_outY = normalizedY * spanY;
			return true;
		}

		void WriteCursorPosition(float a_x, float a_y)
		{
			if (auto* menuCursor = RE::MenuCursor::GetSingleton()) {
				menuCursor->cursorPosX = a_x;
				menuCursor->cursorPosY = a_y;
			}
		}

		// Moves the OS cursor to wherever the game currently thinks the cursor is, so a menu
		// does not open with the pointer snapping across the screen.
		void SyncOsCursorToGame()
		{
			auto* menuCursor = RE::MenuCursor::GetSingleton();
			if (!menuCursor) {
				return;
			}

			HWND hwnd = ResolveGameWindow();
			if (!hwnd) {
				return;
			}

			RECT client{};
			if (!::GetClientRect(hwnd, &client)) {
				return;
			}

			const auto clientW = static_cast<float>(client.right - client.left);
			const auto clientH = static_cast<float>(client.bottom - client.top);
			if (clientW <= 0.0f || clientH <= 0.0f) {
				return;
			}

			float spanX = 0.0f;
			float spanY = 0.0f;
			ResolveSpan(*menuCursor, clientW, clientH, spanX, spanY);
			if (spanX <= 0.0f || spanY <= 0.0f) {
				return;
			}

			const float normalizedX = std::clamp(menuCursor->cursorPosX / spanX, 0.0f, 1.0f);
			const float normalizedY = std::clamp(menuCursor->cursorPosY / spanY, 0.0f, 1.0f);

			POINT target{
				static_cast<LONG>(std::lround(normalizedX * clientW)),
				static_cast<LONG>(std::lround(normalizedY * clientH))
			};
			if (::ClientToScreen(hwnd, &target)) {
				::SetCursorPos(target.x, target.y);
			}
		}

		// ---------------------------------------------------------------------------
		// Window procedure
		// ---------------------------------------------------------------------------

		// Menu open/close events are the primary activation signal, but they are not
		// sufficient on their own: the Cursor Menu can already be open before our sink is
		// registered, which is why there was no cursor on the main menu until a save was
		// loaded. Polling the actual menu state from the window procedure - which always
		// runs - closes that gap regardless of event timing.
		void SyncActiveState()
		{
			if (!g_runtimeReady.load(std::memory_order_relaxed) || !Config::Get().enabled) {
				return;
			}

			static std::uint64_t lastTick = 0;
			const auto           now = ::GetTickCount64();
			if (now - lastTick < 100) {
				return;
			}
			lastTick = now;

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			// Emitted from here as well as from the mouse hook, so the menu list is still
			// captured while inactive - which is the state to inspect if the main menu ends
			// up with no cursor again.
			LogDiagnosticSummary();

			const bool shouldBeActive = ui->IsMenuOpen(RE::CursorMenu::MENU_NAME);
			const bool isActive = g_active.load(std::memory_order_relaxed);
			if (shouldBeActive && !isActive) {
				SKSE::log::info("Activating from state poll (menu event was missed).");
				Activate();
			} else if (!shouldBeActive && isActive) {
				Deactivate();
			}
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			switch (a_msg) {
			case WM_MOUSEMOVE:
				// Re-assert the image here too. WM_SETCURSOR is not guaranteed to arrive on
				// every move if something else in the process answers it first.
				SyncActiveState();
				if (g_active.load(std::memory_order_relaxed) && Config::Get().useHardwareCursor &&
					!g_gamepadMode.load(std::memory_order_relaxed)) {
					::SetCursor(g_customCursor ? g_customCursor : g_fallbackCursor);
				}
				break;

			case WM_SETCURSOR:
				// The game does not handle WM_SETCURSOR itself, so this is where we get to
				// name the cursor image. It arrives at OS mouse-event rate, which also makes
				// it a convenient place to keep re-asserting visibility.
				SyncActiveState();
				if (g_active.load(std::memory_order_relaxed) && Config::Get().useHardwareCursor &&
					!g_gamepadMode.load(std::memory_order_relaxed) && LOWORD(a_lparam) == HTCLIENT) {
					HCURSOR cursor = g_customCursor ? g_customCursor : g_fallbackCursor;
					if (!cursor) {
						// Never pass null here - that would hide the pointer entirely.
						cursor = ::LoadCursorW(nullptr, IDC_ARROW);
					}
					::SetCursor(cursor);
					ForceCursorShown();
					return TRUE;
				}
				break;

			case WM_ACTIVATE:
			case WM_ACTIVATEAPP:
				if (g_active.load(std::memory_order_relaxed)) {
					const bool activating = (a_msg == WM_ACTIVATE)
						? (LOWORD(a_wparam) != WA_INACTIVE)
						: (a_wparam != FALSE);
					if (activating) {
						ApplyClip(Config::Get().clipToWindow);
						if (Config::Get().useHardwareCursor) {
							// Focus loss can leave the game's counter somewhere we did not
							// put it, so drop our claim and genuinely re-apply rather than
							// letting ForceCursorShown decide it has nothing to do.
							g_cursorShownByUs = false;
							ForceCursorShown();
							::SetCursor(g_customCursor ? g_customCursor : g_fallbackCursor);
						}
					} else {
						::ClipCursor(nullptr);
						g_cursorShownByUs = false;
					}
				}
				break;

			case WM_SIZE:
			case WM_MOVE:
				if (g_active.load(std::memory_order_relaxed)) {
					ApplyClip(Config::Get().clipToWindow);
				}
				break;

			case WM_DESTROY:
				// Only OS-level cleanup here. Calling Shutdown() would clear
				// g_originalWndProc, which this same function is about to call through.
				// Touching the UI singleton during teardown is not worth the risk either.
				g_active.store(false);
				::ClipCursor(nullptr);
				break;

			default:
				break;
			}

			return g_windowIsUnicode
				? ::CallWindowProcW(g_originalWndProc, a_hwnd, a_msg, a_wparam, a_lparam)
				: ::CallWindowProcA(g_originalWndProc, a_hwnd, a_msg, a_wparam, a_lparam);
		}

		void HookWindowProc()
		{
			HWND hwnd = ResolveGameWindow();
			if (!hwnd) {
				SKSE::log::error("Could not find the game window; hardware cursor art will not be applied.");
				return;
			}

			g_windowIsUnicode = ::IsWindowUnicode(hwnd) != FALSE;

			auto previous = g_windowIsUnicode
				? ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc))
				: ::SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc));

			if (previous == 0) {
				SKSE::log::error("Failed to subclass the game window (error {}).", ::GetLastError());
				return;
			}

			g_originalWndProc = reinterpret_cast<WNDPROC>(previous);
			SKSE::log::info("Window procedure hooked (hwnd={}, unicode={}).", static_cast<void*>(hwnd), g_windowIsUnicode);
		}

		// ---------------------------------------------------------------------------
		// Cursor art
		// ---------------------------------------------------------------------------

		void LoadCursorArt()
		{
			const auto& config = Config::Get();

			g_fallbackCursor = ::LoadCursorW(nullptr, IDC_ARROW);

			std::filesystem::path path;
			if (!config.cursorFile.empty()) {
				const std::filesystem::path configured(config.cursorFile);
				std::error_code             ec;

				if (configured.is_absolute()) {
					path = configured;
				} else {
					// Try the plausible bases rather than demanding one exact spelling:
					// next to the plugin, under Data, and relative to the game root.
					const std::filesystem::path bases[]{
						"Data/SKSE/Plugins",
						"Data/SKSE/Plugins/CursorUnbound",
						"Data",
						"",
					};
					for (const auto& base : bases) {
						auto candidate = base.empty() ? configured : base / configured;
						if (std::filesystem::exists(candidate, ec)) {
							path = candidate;
							break;
						}
					}
					if (path.empty()) {
						SKSE::log::error(
							"CursorFile '{}' was not found under Data\\SKSE\\Plugins, "
							"Data\\SKSE\\Plugins\\CursorUnbound, Data, or the game root.",
							config.cursorFile);
					}
				}
			} else {
				const auto found = FindCursorFile("Data/SKSE/Plugins/CursorUnbound");
				if (found) {
					path = *found;
				}
			}

			if (path.empty()) {
				SKSE::log::warn(
					"No cursor image found in Data/SKSE/Plugins/CursorUnbound - falling back to the "
					"standard Windows arrow.");
				return;
			}

			g_customCursor = CreateCursorFromFile(path, config.hotspotX, config.hotspotY, config.scale);
			if (!g_customCursor) {
				SKSE::log::warn("Falling back to the standard Windows arrow.");
			}
		}

		// ---------------------------------------------------------------------------
		// Activation
		// ---------------------------------------------------------------------------

		void Activate()
		{
			if (g_active.exchange(true)) {
				return;
			}

			const auto& config = Config::Get();

			if (config.syncOnMenuOpen) {
				SyncOsCursorToGame();
			}

			if (config.useHardwareCursor && !g_gamepadMode.load(std::memory_order_relaxed)) {
				if (config.hideGameCursor) {
					SetScaleformCursorVisible(false, false);
				}
				ForceCursorShown();
				::SetCursor(g_customCursor ? g_customCursor : g_fallbackCursor);
			}

			ApplyClip(config.clipToWindow);

			++g_stats.activations;
			SKSE::log::debug(
				"Activated (hardwareCursor={}, customArt={}).",
				config.useHardwareCursor,
				g_customCursor != nullptr);
		}

		void Deactivate()
		{
			if (!g_active.exchange(false)) {
				return;
			}

			::ClipCursor(nullptr);

			if (Config::Get().useHardwareCursor) {
				SetScaleformCursorVisible(true, false);
				ForceCursorHidden();
				// The next Cursor Menu is a fresh instance with a fresh viewport, so any
				// stashed one is stale from here on. The suppressed-movie pointer must be
				// dropped too - that instance is about to be destroyed, and a recycled
				// allocation at the same address would silently go invisible.
				g_viewportSaved = false;
				ClearSuppressedMovie();
			}

			SKSE::log::debug("Deactivated.");
		}

		// ---------------------------------------------------------------------------
		// Input source switching
		// ---------------------------------------------------------------------------

		void EnterGamepadMode()
		{
			if (g_gamepadMode.exchange(true)) {
				return;
			}

			SKSE::log::info("Gamepad cursor input detected - returning the cursor to the game.");

			// Give the game its own pointer back and get ours off the screen.
			SetScaleformCursorVisible(true, false);
			ForceCursorHidden();
		}

		void ExitGamepadMode()
		{
			if (!g_gamepadMode.exchange(false)) {
				return;
			}

			SKSE::log::info("Mouse input resumed - taking the cursor back.");

			const auto& config = Config::Get();
			if (g_active.load(std::memory_order_relaxed) && config.useHardwareCursor) {
				if (config.hideGameCursor) {
					SetScaleformCursorVisible(false, false);
				}
				ForceCursorShown();
				::SetCursor(g_customCursor ? g_customCursor : g_fallbackCursor);
			}
		}

		struct ProcessThumbstickHook
		{
			static bool thunk(RE::MenuEventHandler* a_this, RE::ThumbstickEvent* a_event)
			{
				// A small deadzone, so stick drift on a worn controller does not keep
				// yanking the cursor away from a mouse user.
				if (a_event && g_runtimeReady.load(std::memory_order_relaxed) &&
					(std::fabs(a_event->xValue) > 0.2f || std::fabs(a_event->yValue) > 0.2f)) {
					EnterGamepadMode();
				}
				return func(a_this, a_event);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------------------------------------------------------------------------
		// CursorMenu::ProcessMouseMove detour
		// ---------------------------------------------------------------------------

		struct ProcessMouseMoveHook
		{
			static bool thunk(RE::MenuEventHandler* a_this, RE::MouseMoveEvent* a_event)
			{
				const auto& config = Config::Get();

				// Any real mouse movement means the player is back on the mouse.
				if (a_event && (a_event->mouseInputX != 0 || a_event->mouseInputY != 0) &&
					g_gamepadMode.load(std::memory_order_relaxed)) {
					ExitGamepadMode();
				}

				const bool engaged =
					g_runtimeReady.load(std::memory_order_relaxed) &&
					g_active.load(std::memory_order_relaxed) &&
					!g_gamepadMode.load(std::memory_order_relaxed) &&
					config.enabled &&
					config.absolutePositioning;

				if (!engaged) {
					return func(a_this, a_event);
				}

				if (auto* menuCursor = RE::MenuCursor::GetSingleton()) {
					MaybeLogRange(*menuCursor);
				}

				float x = 0.0f;
				float y = 0.0f;
				if (!ComputeAbsolutePosition(x, y)) {
					return func(a_this, a_event);
				}

				// The position has to be in place BEFORE the original runs, not after it.
				// The original is what pushes the cursor position into Scaleform (via
				// NotifyMouseState), so a post-hoc overwrite leaves the drawn cursor and the
				// hit-test following the game's fps-scaled integration while MenuCursor holds
				// ours - the two disagree every frame, which reads as jitter.
				WriteCursorPosition(x, y);

				const auto savedX = a_event->mouseInputX;
				const auto savedY = a_event->mouseInputY;

				if (config.neutralizeGameDelta) {
					// Otherwise the original integrates its delta on top of the absolute
					// position we just wrote and overshoots by exactly one frame of movement.
					a_event->mouseInputX = 0;
					a_event->mouseInputY = 0;
				}

				const bool handled = func(a_this, a_event);

				// MenuControls dispatches this same event object to every registered handler.
				// Inventory item rotation and map dragging read these deltas, so the zeroing
				// must not outlive the call it was meant for.
				a_event->mouseInputX = savedX;
				a_event->mouseInputY = savedY;

				// Re-assert in case the original clamped or rewrote the position.
				WriteCursorPosition(x, y);

				ReassertScaleformHidden();

				return handled;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_CursorMenu[1] };
		ProcessMouseMoveHook::func = vtable.write_vfunc(0x4, ProcessMouseMoveHook::thunk);
		ProcessThumbstickHook::func = vtable.write_vfunc(0x3, ProcessThumbstickHook::thunk);
		SKSE::log::info("Hooked CursorMenu::ProcessMouseMove and ProcessThumbstick.");

		if (Config::Get().blockGameCursorHide) {
			const auto original = SKSE::PatchIAT(&ShowCursorHook, "user32.dll", "ShowCursor");
			if (original) {
				g_realShowCursor = reinterpret_cast<decltype(g_realShowCursor)>(original);
				SKSE::log::info("Patched USER32!ShowCursor in the game import table.");
			} else {
				SKSE::log::warn(
					"USER32!ShowCursor is not in the game's import table; relying on WM_SETCURSOR "
					"to keep the pointer visible. Set BlockGameCursorHide=false to silence this.");
			}
		}
	}

	void InitializeRuntime()
	{
		ResolveGameWindow();
		LoadCursorArt();
		HookWindowProc();

		g_runtimeReady.store(true);

		// A menu may already be up when we initialise (e.g. the main menu).
		auto* ui = RE::UI::GetSingleton();
		if (ui && ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
			Activate();
		}
	}

	void Shutdown()
	{
		Deactivate();

		if (g_originalWndProc && g_window && ::IsWindow(g_window)) {
			if (g_windowIsUnicode) {
				::SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
			} else {
				::SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
			}
			g_originalWndProc = nullptr;
		}

		if (g_customCursor) {
			::DestroyCursor(g_customCursor);
			g_customCursor = nullptr;
		}

		g_runtimeReady.store(false);
	}

	void OnMenuOpenClose(std::string_view a_menuName, bool a_opening)
	{
		if (!Config::Get().enabled) {
			return;
		}

		if (a_menuName == RE::CursorMenu::MENU_NAME) {
			if (a_opening) {
				Activate();
			} else {
				Deactivate();
			}
			return;
		}

		// Menus stacking on top of the cursor menu can re-show the Scaleform pointer, so
		// re-assert our state whenever anything else opens or closes.
		if (g_active.load(std::memory_order_relaxed) &&
			Config::Get().useHardwareCursor &&
			Config::Get().hideGameCursor) {
			SetScaleformCursorVisible(false, false);
		}
	}
}
