#include "CursorUnbound.h"

#include "Config.h"
#include "CursorImage.h"
#include "PartySheetAPI.h"

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
		WNDPROC  g_ourWndProc = nullptr;
		HCURSOR  g_customCursor = nullptr;
		HCURSOR  g_fallbackCursor = nullptr;

		int(WINAPI* g_realShowCursor)(BOOL) = nullptr;

		void Activate();
		void Deactivate();

		// A gamepad drives the menu cursor through ProcessThumbstick, not ProcessMouseMove.
		// The hardware cursor cannot follow that - the OS pointer simply does not move - so
		// while a stick is driving the cursor we hand rendering back to the game entirely.
		std::atomic<bool> g_gamepadMode{ false };

		// A mod that draws its own pointer is on screen and we have chosen to let it keep
		// drawing rather than take its pointer away. See ShouldYieldPointer, which is the only
		// thing that writes this.
		//
		// Distinct from gamepad mode, which hands the cursor back to the game completely. Here
		// we still drive the position absolutely and still keep the Scaleform cursor down,
		// because the mod reads the same MenuCursor fields we write and so inherits the
		// frame-rate independent sensitivity from us. The only thing given up is drawing a
		// pointer of our own on top of theirs.
		std::atomic<bool> g_yieldPointer{ false };

		struct Diagnostics
		{
			std::uint64_t activations = 0;
			std::uint64_t entryVisible = 0;  // movie was visible again when we re-entered
			std::uint64_t entryHidden = 0;   // our previous hide was still in effect
			std::uintptr_t lastMovie = 0;

			// Input path, reset on every activation so each counter reads "since this menu
			// opened". The question they answer is whether CursorMenu::ProcessMouseMove is
			// reached at all: an ImGui overlay or an HTML framework that owns mouse input
			// leaves `mouseMoveCalls` at zero, and every downstream symptom - no absolute
			// positioning, no re-suppression - follows from that single fact.
			std::uint64_t mouseMoveCalls = 0;    // hook entered
			std::uint64_t mouseMoveEngaged = 0;  // hook actually wrote an absolute position
			std::uint64_t thumbstickCalls = 0;

			// Window path, also reset on activation. Zero WM_SETCURSOR while we are active
			// means something subclassed the window after us and is answering it first, which
			// is what a pointer that is plainly not our art looks like from in here.
			std::uint64_t wmMouseMove = 0;
			std::uint64_t wmSetCursor = 0;
			std::uint64_t wmSetCursorAnswered = 0;
			// The timer is the control. It is ours, it fires on a schedule rather than on
			// input, and nothing else in the process has a reason to filter it - so a live
			// timer alongside zero mouse messages means the messages are being eaten, while
			// zero of both means we are off the message path altogether.
			std::uint64_t wmTimer = 0;

			void ResetPerActivation()
			{
				mouseMoveCalls = 0;
				mouseMoveEngaged = 0;
				thumbstickCalls = 0;
				wmMouseMove = 0;
				wmSetCursor = 0;
				wmSetCursorAnswered = 0;
				wmTimer = 0;
			}
		};
		Diagnostics g_stats;

		// Diagnostic line budget, spent per activation rather than once per session. It used
		// to be a function-local counter that ran down while the game sat at the main menu, so
		// a two minute load screen consumed the whole allowance before the player opened
		// anything - and the menu list, the one field worth having, was missing from exactly
		// the window that mattered.
		int g_diagEmitted = 0;

		// Calibration bookkeeping for [Debug] LogCursorRange.
		float         g_observedMinX = (std::numeric_limits<float>::max)();
		float         g_observedMaxX = std::numeric_limits<float>::lowest();
		float         g_observedMinY = (std::numeric_limits<float>::max)();
		float         g_observedMaxY = std::numeric_limits<float>::lowest();
		std::uint64_t g_lastRangeLogTick = 0;

		// Throttle for ReassertScaleformHidden. At namespace scope rather than a
		// function-local static so Activate can zero it: otherwise a mouse move shortly
		// before a menu opened would hold the first re-assert off for the rest of that
		// 250ms, which is the whole window the re-assert exists to cover.
		std::uint64_t g_lastReassertTick = 0;

		// Throttle for ReassertScaleformShown, the gamepad-mode mirror of the above.
		std::uint64_t g_lastReshowTick = 0;

		// True while a hide of ours has actually reached the cursor movie and not been undone.
		// Written only by SetScaleformCursorVisible when it gets as far as the movie, and by
		// Deactivate, which drops every piece of per-instance state. This is what lets the
		// gamepad-mode re-show undo exactly our own hides and nothing the game did itself.
		std::atomic<bool> g_scaleformHiddenByUs{ false };

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

		// ShowCursor returns the NEW value of the display counter, and that return value is
		// the only trustworthy read of it. GetCursorInfo reports the SYSTEM cursor state, and
		// on the main menu and immediately after alt-tab that reads as "showing" while this
		// queue's counter is still negative. A cached "we already showed it" flag guarded by
		// GetCursorInfo could therefore latch on while the pointer was in fact invisible and
		// then skip the one call that would have revealed it - which is what left alt-tabbing
		// out and back as the only way to get a cursor.
		//
		// So probe with a real call and hand the increment straight back when it was not
		// needed. The counter lands on exactly 0 (visible) or -1 (hidden) whatever moved it
		// behind our back, nothing ratchets, and both are cheap enough to call at timer rate.
		int g_lastCursorCount = 0;

		// Both of these normalize the counter to exactly 0 (shown) or -1 (hidden), whatever
		// it was on entry and whoever moved it. Overshooting in either direction has bitten
		// us before: a counter left at -3 needs three increments before the pointer appears,
		// and one left high cannot be undone by a single decrement.
		void ForceCursorShown()
		{
			int count = RealShowCursor(TRUE);
			for (int i = 0; i < 64 && count > 0; ++i) {
				count = RealShowCursor(FALSE);
			}
			for (int i = 0; i < 64 && count < 0; ++i) {
				count = RealShowCursor(TRUE);
			}
			g_lastCursorCount = count;
		}

		void ForceCursorHidden()
		{
			int count = RealShowCursor(FALSE);
			for (int i = 0; i < 64 && count < -1; ++i) {
				count = RealShowCursor(TRUE);
			}
			for (int i = 0; i < 64 && count >= 0; ++i) {
				count = RealShowCursor(FALSE);
			}
			g_lastCursorCount = count;
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
			// Yielding to a mod that draws its own pointer is the same situation for the same
			// reason: we want the OS cursor gone, so hide calls have to get through.
			if (!a_show && g_active.load(std::memory_order_relaxed) &&
				!g_gamepadMode.load(std::memory_order_relaxed) &&
				!g_yieldPointer.load(std::memory_order_relaxed) &&
				Config::Get().blockGameCursorHide && Config::Get().useHardwareCursor) {
				return -1;
			}
			return RealShowCursor(a_show);
		}

		// A bounds-checked view over a module that is currently mapped into the process.
		//
		// Everything below reads PE structures belonging to somebody else's module, and every
		// offset in them is data we do not control: modules get packed, import directories get
		// rewritten by other hooking libraries, and a hand-written proxy DLL only has to be
		// valid enough for the loader, not shaped the way the documentation draws it. 1.0.2
		// walked those structures unchecked and dereferenced whatever came out, which is what
		// crashed at kDataLoaded on load orders containing such a module. Nothing here is
		// allowed to read outside the image.
		struct ModuleImage
		{
			std::uint8_t* base = nullptr;
			std::uint32_t size = 0;

			bool Contains(std::uint64_t a_rva, std::uint64_t a_bytes) const
			{
				return a_rva + a_bytes >= a_rva && a_rva + a_bytes <= size;
			}

			template <class T>
			const T* At(std::uint64_t a_rva, std::uint64_t a_count = 1) const
			{
				return Contains(a_rva, sizeof(T) * a_count) ? reinterpret_cast<const T*>(base + a_rva) : nullptr;
			}

			// A string only counts as readable if its terminator is inside the image too -
			// otherwise the compare that follows runs off the end looking for one.
			const char* String(std::uint64_t a_rva) const
			{
				if (a_rva >= size) {
					return nullptr;
				}
				const auto* first = reinterpret_cast<const char*>(base + a_rva);
				const auto  span = static_cast<std::size_t>(size - a_rva);
				return std::memchr(first, '\0', span) ? first : nullptr;
			}
		};

		// Validates a loaded module's PE headers and returns a view of it. The first page of a
		// mapped image is always present, so reading the headers themselves is safe; every
		// offset taken out of them is treated as hostile from here on.
		bool OpenModuleImage(HMODULE a_module, ModuleImage& a_out)
		{
			auto* const base = reinterpret_cast<std::uint8_t*>(a_module);
			if (!base) {
				return false;
			}

			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
				dos->e_lfanew < 0 ||
				static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > 0x1000) {
				return false;
			}

			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE ||
				nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
				nt->OptionalHeader.SizeOfImage < 0x1000) {
				return false;
			}

			a_out.base = base;
			a_out.size = nt->OptionalHeader.SizeOfImage;
			return true;
		}

		const IMAGE_NT_HEADERS64* ModuleHeaders(const ModuleImage& a_image)
		{
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_image.base);
			return reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_image.base + dos->e_lfanew);
		}

		// DataDirectory is not the fixed 16-entry array it is declared as: only the first
		// NumberOfRvaAndSizes entries exist. Indexing past that reads the section headers that
		// follow the optional header and interprets them as an RVA and a size - which is one
		// way to end up walking "imports" that are really somebody's section table.
		const IMAGE_DATA_DIRECTORY* ModuleDirectory(const ModuleImage& a_image, std::uint32_t a_index)
		{
			const auto* nt = ModuleHeaders(a_image);
			if (nt->OptionalHeader.NumberOfRvaAndSizes <= a_index) {
				return nullptr;
			}

			const auto& dir = nt->OptionalHeader.DataDirectory[a_index];
			if (!dir.VirtualAddress || !dir.Size || !a_image.Contains(dir.VirtualAddress, dir.Size)) {
				return nullptr;
			}
			return &dir;
		}

		// Locates the import table slot a module uses to call a_dll!a_function.
		//
		// SKSE::GetIATPtr exists, but it only ever walks the game executable and logs a
		// warning for every miss - across a few hundred loaded modules that is a few hundred
		// warnings per launch. This also handles a null OriginalFirstThunk, which bound
		// imports leave behind and which SKSE's version dereferences unconditionally.
		void** FindImportSlotIn(const ModuleImage& a_image, const char* a_dll, const char* a_function)
		{
			const auto* dir = ModuleDirectory(a_image, IMAGE_DIRECTORY_ENTRY_IMPORT);
			if (!dir) {
				return nullptr;
			}

			// The descriptor array ends with an all-zero entry, but the directory size is the
			// authority on how far it may extend. Trusting only the terminator is what lets a
			// module without one walk into whatever data follows, where the next "Name" is
			// really the low half of a pointer and base+Name lands outside the address space.
			const std::uint64_t remaining =
				(a_image.size - dir->VirtualAddress) / sizeof(IMAGE_IMPORT_DESCRIPTOR);

			std::uint64_t count = dir->Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
			if (count == 0 || count > remaining) {
				// A directory size too small to hold a descriptor, or too large to fit the
				// image, is not a reason to give up on the module - the terminator can still
				// end the walk. It is a reason to let the image bound be the one that binds.
				count = remaining;
			}

			const auto* desc = a_image.At<IMAGE_IMPORT_DESCRIPTOR>(dir->VirtualAddress, count);
			if (!desc) {
				return nullptr;
			}

			for (std::uint64_t d = 0; d < count && desc[d].Name != 0; ++d) {
				const char* dll = a_image.String(desc[d].Name);
				if (!dll || _stricmp(dll, a_dll) != 0) {
					continue;
				}

				// Bound imports zero OriginalFirstThunk, leaving FirstThunk as the only copy
				// of the name table. It is still readable, it just doubles as the live slot.
				const std::uint64_t namesRva =
					desc[d].OriginalFirstThunk ? desc[d].OriginalFirstThunk : desc[d].FirstThunk;
				if (!namesRva || !desc[d].FirstThunk || namesRva >= a_image.size) {
					continue;
				}

				// Same reasoning as above: the thunk array is null-terminated, but the image
				// bound decides how long it is allowed to be.
				const std::uint64_t maxEntries = (a_image.size - namesRva) / sizeof(IMAGE_THUNK_DATA64);
				for (std::uint64_t i = 0; i < maxEntries; ++i) {
					const auto* thunk = a_image.At<IMAGE_THUNK_DATA64>(namesRva + (i * sizeof(IMAGE_THUNK_DATA64)));
					if (!thunk || thunk->u1.AddressOfData == 0) {
						break;
					}
					if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) {
						continue;  // Imported by ordinal, so it carries no name to match.
					}

					const char* name =
						a_image.String(thunk->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name));
					if (!name || std::strcmp(name, a_function) != 0) {
						continue;
					}

					const auto* slot = a_image.At<void*>(desc[d].FirstThunk + (i * sizeof(void*)));
					return const_cast<void**>(slot);
				}
			}

			return nullptr;
		}

		// Even a fully validated walk can fault, because the validation and the read are not
		// one atomic act: a module can be unloaded, or have its import directory point into
		// memory another mod owns and is busy rewriting. A module we cannot read is a module
		// we skip - none of this is worth taking the game down for.
		//
		// The SEH frame lives here, in a function with no unwindable locals, so the C++ work
		// stays in FindImportSlotIn where destructors are allowed.
		void** FindImportSlot(HMODULE a_module, const char* a_dll, const char* a_function)
		{
			__try {
				ModuleImage image{};
				if (!OpenModuleImage(a_module, image)) {
					return nullptr;
				}
				return FindImportSlotIn(image, a_dll, a_function);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// Redirects one module's USER32!ShowCursor import to our hook. Returns true if the
		// module imported it at all.
		bool PatchShowCursorIn(HMODULE a_module)
		{
			auto* slot = FindImportSlot(a_module, "user32.dll", "ShowCursor");
			if (!slot || *slot == reinterpret_cast<void*>(&ShowCursorHook)) {
				return slot != nullptr;
			}

			// The first original we see is our route to the real function. Whoever owns the
			// slot now may itself be another mod's hook, so preferring the game executable
			// (patched first) keeps us at the bottom of any chain rather than cutting other
			// mods out of it.
			if (!g_realShowCursor) {
				g_realShowCursor = reinterpret_cast<decltype(g_realShowCursor)>(*slot);
			}

			const auto hook = reinterpret_cast<std::uintptr_t>(&ShowCursorHook);
			REL::safe_write(reinterpret_cast<std::uintptr_t>(slot), hook);
			return true;
		}

		std::string Narrow(const wchar_t* a_wide)
		{
			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_wide, -1, nullptr, 0, nullptr, nullptr);
			if (needed <= 1) {
				return {};
			}
			std::string out(static_cast<std::size_t>(needed) - 1, '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_wide, -1, out.data(), needed, nullptr, nullptr);
			return out;
		}

		// True for anything living under C:\Windows. Those are excluded from the sweep: the
		// modules we are actually after are mods, and redirecting the imports of system DLLs
		// (or of the Steam overlay, which does its own cursor management) buys nothing and
		// puts us in the middle of conversations we have no business in.
		bool IsSystemModule(const wchar_t* a_path)
		{
			static const std::wstring windows = [] {
				wchar_t    buffer[MAX_PATH]{};
				const auto len = ::GetWindowsDirectoryW(buffer, MAX_PATH);
				return (len > 0 && len < MAX_PATH) ? std::wstring(buffer, len) : std::wstring{};
			}();

			if (windows.empty()) {
				return false;
			}

			const auto len = static_cast<int>(windows.size());
			if (static_cast<int>(std::wcslen(a_path)) < len) {
				return false;
			}
			return ::CompareStringOrdinal(a_path, len, windows.c_str(), len, TRUE) == CSTR_EQUAL;
		}

		// The game executable is not the only thing calling ShowCursor. SSEDisplayTweaks in
		// particular drives cursor visibility for its borderless window, and its calls come
		// from its own import table - which is why a session could end up with the display
		// counter at -3 while we believed we had forced the pointer visible.
		//
		// Run once every SKSE plugin is loaded, so the sweep sees them.
		void PatchShowCursorEverywhere()
		{
			HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE) {
				SKSE::log::warn("Could not enumerate loaded modules; only the game import table is hooked.");
				return;
			}

			HMODULE self = nullptr;
			::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ShowCursorHook),
				&self);

			const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
			const HMODULE gameExe = ::GetModuleHandleW(nullptr);

			int         patched = 0;
			std::string names;

			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (::Module32FirstW(snapshot, &entry)) {
				do {
					// Skipping ourselves keeps RealShowCursor's ::ShowCursor fallback from
					// re-entering the hook if the slot capture above ever came up empty.
					if (!entry.hModule || entry.hModule == self || entry.hModule == user32 ||
						IsSystemModule(entry.szExePath)) {
						continue;
					}

					// Named before it is walked, not after, so that if a module ever does
					// take the process down here the log says which one it was.
					SKSE::log::debug("Sweeping {} for USER32!ShowCursor.", Narrow(entry.szModule));

					// The snapshot lists modules that were loaded a moment ago. Take a real
					// reference before reading one, so it cannot be unmapped mid-walk. The
					// executable is exempt: the loader pins it, and it is never going away.
					HMODULE target = entry.hModule;
					bool    referenced = false;
					if (target != gameExe) {
						HMODULE pinned = nullptr;
						if (!::GetModuleHandleExW(
								GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
								reinterpret_cast<LPCWSTR>(entry.modBaseAddr),
								&pinned) ||
							!pinned) {
							continue;  // Unloaded since the snapshot was taken.
						}
						target = pinned;
						referenced = true;
					}

					const bool imported = PatchShowCursorIn(target);

					if (referenced) {
						::FreeLibrary(target);
					}

					if (imported) {
						++patched;
						if (!names.empty()) {
							names += ", ";
						}
						names += Narrow(entry.szModule);
					}
				} while (::Module32NextW(snapshot, &entry));
			}

			::CloseHandle(snapshot);

			SKSE::log::info("Patched USER32!ShowCursor in {} module(s): {}", patched, names);
		}

		// ---------------------------------------------------------------------------
		// Third-party cursor suppression
		//
		// Some UI frameworks draw their own pointer instead of using the Scaleform cursor
		// menu, which puts them out of reach of everything else in this file. Because they
		// draw inside the frame, theirs trails the hardware cursor by a frame or more, and
		// both on screen at once is what "double cursor" reports look like.
		//
		// Each such mod gets a CursorStub below: a module, a signature for the one function
		// that draws the pointer, and a byte to write over its first instruction. The
		// mechanism is shared; only the policy for when to apply it differs per mod.
		//
		// PrismaUI draws its own cursor: a DirectX sprite, blitted in the render loop from
		// MenuCursor::cursorPosX/Y by PrismaUI::ViewRenderer::DrawCursor. It is neither a
		// Scaleform movie nor an HTML element, so nothing else in this file can reach it, and
		// because it is drawn inside the frame it trails the hardware cursor by a frame or
		// more. Both on screen at once is the "double cursor" report from Prisma menus.
		//
		// Their cursor cannot simply be displaced instead: their input handler reads the same
		// MenuCursor fields to place mouse events, so anything that moves the sprite out of
		// the way also breaks hovering and clicking.
		//
		// DrawCursor takes no arguments, returns void, and already opens with its own
		// early-outs (null sprite batch or texture, no active input capture). Writing a single
		// RET over its first byte is therefore a complete and correct suppression - it returns
		// before establishing a frame, so the /GS cookie and the unwind funclet are never
		// reached - and writing the original byte back restores it exactly.
		//
		// None of this makes PrismaUI a dependency. If the module is not loaded, or the
		// signature does not match exactly one location, this does nothing at all.
		// ---------------------------------------------------------------------------

		constexpr std::uint8_t kRetOpcode = 0xC3;

		// What gets written, and therefore what the signature has to point at.
		//
		// kFunctionRet is the shape the first two stubs take: the mod keeps its cursor draw in
		// a function of its own, the signature matches that function's prologue, and a RET over
		// the first byte is a complete suppression.
		//
		// kFloatConstant exists because that shape is not guaranteed. When the compiler inlines
		// the draw into its caller there is no prologue to return from and no function to stub -
		// the enclosing function draws the entire UI, so a RET there would take the whole menu
		// with it. What remains reachable is the guard the mod's own author wrote at the top of
		// the draw: if it opens by comparing the cursor position against a float constant and
		// returning when it fails, raising that constant makes the guard always fire, and the
		// pointer is skipped by the mod's own early-out. The signature then matches the guard,
		// and the constant patched is whichever one the guard's own instruction points at, read
		// out of its RIP displacement rather than hardcoded.
		//
		// The constant is also the safer thing to write. It is four bytes, naturally aligned,
		// and read fresh every frame by the render thread - so a single aligned 32-bit store is
		// atomic against that reader and can never be observed half-applied. The equivalent
		// code patch would be a six-byte write over a live instruction, which has no such
		// guarantee.
		enum class StubPatch
		{
			kFunctionRet,
			kFloatConstant,
		};

		// One suppressible cursor draw in one foreign module.
		//
		// `label` is what the log calls this mod, and `noun` names what the player loses if
		// the signature goes stale - both exist so a warning is actionable without knowing
		// which stub emitted it.
		struct CursorStub
		{
			const wchar_t* module;
			const char*    label;
			const char*    noun;
			const int*     signature;
			std::size_t    signatureLength;
			StubPatch      patch = StubPatch::kFunctionRet;

			// kFloatConstant only. `expected` is the value the guard must currently hold and is
			// verified before anything is written, so a signature that has drifted onto an
			// unrelated constant refuses rather than corrupting it. `replacement` is the value
			// that makes the guard always fire.
			float expected = 0.0f;
			float replacement = 0.0f;

			std::uint8_t* site = nullptr;
			std::size_t   length = 1;  // bytes at `site` that this stub owns
			std::uint8_t  originalBytes[4]{};
			std::uint8_t  patchBytes[4]{};
			bool          patched = false;
			bool          resolved = false;  // resolution was attempted, successfully or not
		};

		// PrismaUI::ViewRenderer::DrawCursor prologue:
		//
		//   40 56                    push rsi
		//   57                       push rdi
		//   48 81 EC 28 02 00 00     sub  rsp, 0x228
		//   48 8B 05 ?? ?? ?? ??     mov  rax, [rip+__security_cookie]
		//   48 33 C4                 xor  rax, rsp
		//   48 89 84 24 10 02 00 00  mov  [rsp+0x210], rax
		//
		// The RIP displacement is wildcarded because it moves with every build; the 0x228
		// frame and the cookie slot at 0x210 are what make this specific. Verified unique
		// across the whole .text section of the PrismaUI.dll this was developed against, where
		// the function sits at +0x922C0. Re-check against every new PrismaUI release - a stale
		// signature is meant to match nothing rather than match the wrong thing.
		constexpr int kPrismaDrawCursorSig[] = {
			0x40, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x28, 0x02, 0x00, 0x00,
			0x48, 0x8B, 0x05,   -1,   -1,   -1,   -1,
			0x48, 0x33, 0xC4,
			0x48, 0x89, 0x84, 0x24, 0x10, 0x02, 0x00, 0x00,
		};

		// ImGuiRenderer::DrawSkyrimCursor prologue, from SkyrimPartySheet.dll:
		//
		//   48 89 7C 24 08           mov  [rsp+8], rdi
		//   55                       push rbp
		//   48 8D 6C 24 A9           lea  rbp, [rsp-0x57]
		//   48 81 EC D0 00 00 00     sub  rsp, 0xD0
		//   0F 29 B4 24 C0 00 00 00  movaps [rsp+0xC0], xmm6
		//   48 8B 05 ?? ?? ?? ??     mov  rax, [rip+__security_cookie]
		//   48 33 C4                 xor  rax, rsp
		//   48 89 45 37              mov  [rbp+0x37], rax
		//
		// Same shape of signature and the same reasoning as Prisma's: the RIP displacement to
		// the cookie moves with every build and is wildcarded, while the 0xD0 frame, the saved
		// xmm6 and the cookie slot at [rbp+0x37] are what make it specific. Verified unique
		// across the whole .text section of Party Sheet 3.1 (link stamp 0x6A68DAC1,
		// 2026-07-28), where the function sits at +0x64E40.
		//
		// The function was identified by its only string reference,
		// "Data\Interface\PartySheet\Icons\cursor.png", which the compiler inlines as SSE
		// stores rather than a lea - so the string is not usable as a signature itself.
		// Re-check against every new Party Sheet release.
		//
		// A RET on the first byte is a complete suppression here for the same reason it is for
		// Prisma: it returns before the frame is established, so neither the /GS cookie nor the
		// saved xmm6 is ever written, and the function returns void. Every other code path in
		// Party Sheet writes io.MouseDrawCursor = false at the top of its frame, so stubbing
		// the one place that would have set it true leaves no ImGui software cursor either.
		constexpr int kPartySheetDrawCursorSig[] = {
			0x48, 0x89, 0x7C, 0x24, 0x08,
			0x55,
			0x48, 0x8D, 0x6C, 0x24, 0xA9,
			0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,
			0x0F, 0x29, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00,
			0x48, 0x8B, 0x05,   -1,   -1,   -1,   -1,
			0x48, 0x33, 0xC4,
			0x48, 0x89, 0x45, 0x37,
		};

		// Grid Inventory's pointer guard, from GridInventory.dll.
		//
		// Grid Inventory replaces the inventory with a Dear ImGui grid. It hides the Scaleform
		// cursor itself, every frame, and draws its own arrow onto the ImGui foreground list at
		// the end of the frame - so alongside the hardware cursor it reads as a second pointer
		// trailing the first, exactly like Prisma's and Party Sheet's.
		//
		// Unlike those two it cannot be stubbed with a RET, and that is the whole reason this
		// patch kind exists. Its DrawPointer is a small static function called from one place,
		// and the compiler inlines it into the function that draws every window in the mod. The
		// first byte of that function belongs to the entire UI, not to the pointer.
		//
		// What it does still have is the guard DrawPointer opens with - ImGui parks an unknown
		// mouse position far off screen, and the pointer is not drawn there:
		//
		//   F3 0F 10 05 ?? ?? ?? ??     movss  xmm0, [rip+kOffscreen]   ; -1000.0f
		//   F3 44 0F 10 80 D8 00 00 00  movss  xmm8, [rax+0xD8]         ; io.MousePos.x
		//   41 0F 2F C0                 comiss xmm0, xmm8
		//   0F 87 ?? ?? ?? ??           ja     past the pointer
		//   0F 29 BC 24 F0 00 00 00     movaps [rsp+0xF0], xmm7
		//   F3 0F 10 B8 DC 00 00 00     movss  xmm7, [rax+0xDC]         ; io.MousePos.y
		//   0F 2F C7                    comiss xmm0, xmm7
		//   0F 87 ?? ?? ?? ??           ja     past the pointer
		//
		// Raising that -1000.0f to FLT_MAX makes the first comparison true for every finite
		// cursor position, so the branch the author already wrote is taken every frame and the
		// pointer is never drawn. Nothing else in the frame is touched: the branch target is the
		// compiler's own, not one we picked, and it lands on the ImGui::Render call immediately
		// after the pointer.
		//
		// Both RIP displacements are wildcarded because they move with every build. What makes
		// the signature specific is the pair of guarded comparisons against MousePos.x and .y at
		// +0xD8/+0xDC and the xmm7 spill at [rsp+0xF0]. Verified unique across the whole .text
		// section of Grid Inventory 1.4.3 (link stamp image 0x2F6000, Nexus 188733), where the
		// guard sits at +0x7376B and the constant it reads at +0x23D5C8 - and that constant is
		// referenced from exactly one instruction in the entire module, which is what makes it
		// safe to write. Re-check both against every new Grid Inventory release.
		constexpr int kGridInventoryPointerSig[] = {
			0xF3, 0x0F, 0x10, 0x05,   -1,   -1,   -1,   -1,
			0xF3, 0x44, 0x0F, 0x10, 0x80, 0xD8, 0x00, 0x00, 0x00,
			0x41, 0x0F, 0x2F, 0xC0,
			0x0F, 0x87,   -1,   -1,   -1,   -1,
			0x0F, 0x29, 0xBC, 0x24, 0xF0, 0x00, 0x00, 0x00,
			0xF3, 0x0F, 0x10, 0xB8, 0xDC, 0x00, 0x00, 0x00,
			0x0F, 0x2F, 0xC7,
			0x0F, 0x87,   -1,   -1,   -1,   -1,
		};

		// Meridian UI's CursorRenderer::Draw prologue, from MeridianUI.dll:
		//
		//   48 89 5C 24 18           mov  [rsp+0x18], rbx
		//   55                       push rbp
		//   56                       push rsi
		//   57                       push rdi
		//   41 54                    push r12
		//   41 55                    push r13
		//   41 56                    push r14
		//   41 57                    push r15
		//   48 83 EC 70              sub  rsp, 0x70
		//   48 8B 05 ?? ?? ?? ??     mov  rax, [rip+__security_cookie]
		//   48 33 C4                 xor  rax, rsp
		//   48 89 44 24 60           mov  [rsp+0x60], rax
		//   4C 8B EA                 mov  r13, rdx      ; RenderData&
		//   4C 8B F9                 mov  r15, rcx      ; this
		//   48 8B 29                 mov  rbp, [rcx]    ; m_current, the HCURSOR to draw
		//
		// Meridian UI is a Chromium (CEF) framework, and its pointer has the same shape as
		// Prisma's. Chromium reports a Windows cursor handle, Meridian rasterises it into a
		// texture, and Draw blits that texture with a sprite batch at MenuCursor::cursorPosX/Y
		// after its browser layers, inside the game's present hook - so it inherits our absolute
		// position and is still frame-locked. It only draws while one of its browsers holds
		// focus, which is also when its focus menu (kUsesCursor) has brought us up.
		//
		// Same signature shape and the same reasoning as Prisma's: the cookie displacement is
		// wildcarded, and the 0x70 frame, the cookie slot at 0x60 and the three argument moves
		// are what make it specific - the eight-register prologue with a 0x70 frame on its own
		// also matches a second, unrelated function in the same module. Verified unique across
		// the whole .text section of Meridian UI 1.2.0 (link stamp 0x6A9AF1EF, 2026-09-04, image
		// 0x22B000, Nexus 190723), where the function sits at +0x85410. It is the only caller of
		// the rasteriser, which is in turn the only function in the module that calls
		// USER32!DrawIconEx - that chain is how to find it again after an update. The source is
		// public (github.com/heathbrownkeyworks/MeridianUI, src/UIPlatform/Render/CursorRenderer.cpp).
		//
		// Re-check against every new Meridian release. It builds with /Ob3, so a future build
		// could inline Draw into the render host's frame function, which would turn this into
		// the Grid Inventory situation rather than a merely stale signature.
		//
		// A RET on the first byte is a complete suppression for the same reason it is for
		// Prisma: Draw returns void, and the RET lands before the frame, the cookie or any
		// register save. Meridian's own guard around the call - draw only while a browser holds
		// focus - is untouched, as is the vanilla-cursor hiding it does alongside.
		constexpr int kMeridianDrawCursorSig[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18,
			0x55, 0x56, 0x57,
			0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x83, 0xEC, 0x70,
			0x48, 0x8B, 0x05,   -1,   -1,   -1,   -1,
			0x48, 0x33, 0xC4,
			0x48, 0x89, 0x44, 0x24, 0x60,
			0x4C, 0x8B, 0xEA,
			0x4C, 0x8B, 0xF9,
			0x48, 0x8B, 0x29,
		};

		CursorStub g_prismaStub{
			.module = L"PrismaUI.dll",
			.label = "PrismaUI",
			.noun = "Prisma menus",
			.signature = kPrismaDrawCursorSig,
			.signatureLength = std::size(kPrismaDrawCursorSig),
		};

		CursorStub g_partySheetStub{
			.module = L"SkyrimPartySheet.dll",
			.label = "Skyrim Party Sheet",
			.noun = "Party Sheet panels",
			.signature = kPartySheetDrawCursorSig,
			.signatureLength = std::size(kPartySheetDrawCursorSig),
		};

		CursorStub g_gridInventoryStub{
			.module = L"GridInventory.dll",
			.label = "Grid Inventory",
			.noun = "the grid inventory",
			.signature = kGridInventoryPointerSig,
			.signatureLength = std::size(kGridInventoryPointerSig),
			.patch = StubPatch::kFloatConstant,
			.expected = -1000.0f,
			// Any finite MousePos compares below this, so the guard fires on every frame the
			// pointer would have been drawn on. Not infinity: an unordered compare leaves the
			// branch untaken, which is the one outcome that would draw the pointer anyway.
			.replacement = (std::numeric_limits<float>::max)(),
		};

		CursorStub g_meridianStub{
			.module = L"MeridianUI.dll",
			.label = "Meridian UI",
			.noun = "Meridian pages",
			.signature = kMeridianDrawCursorSig,
			.signatureLength = std::size(kMeridianDrawCursorSig),
		};

		bool GetTextSection(HMODULE a_module, std::uint8_t*& a_outBegin, std::size_t& a_outSize)
		{
			ModuleImage image{};
			if (!OpenModuleImage(a_module, image)) {
				return false;
			}

			const auto* nt = ModuleHeaders(image);
			const auto* section = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
				if (std::memcmp(section->Name, ".text", 5) != 0) {
					continue;
				}

				// The scan that follows reads every byte of this range, so a section header
				// claiming more than the image holds has to fail here rather than there.
				if (!image.Contains(section->VirtualAddress, section->Misc.VirtualSize)) {
					return false;
				}

				a_outBegin = image.base + section->VirtualAddress;
				a_outSize = section->Misc.VirtualSize;
				return true;
			}
			return false;
		}

		// Deliberately scans the whole range and counts every hit rather than stopping at the
		// first. Two matches means the signature has stopped being specific enough, and that
		// has to stay distinguishable from a clean hit - patching the first of several would
		// be a guess.
		std::uint8_t* FindPattern(std::uint8_t* a_begin, std::size_t a_size, const int* a_pattern,
			std::size_t a_length, std::size_t& a_outMatches)
		{
			a_outMatches = 0;
			if (!a_begin || a_size < a_length) {
				return nullptr;
			}

			std::uint8_t* first = nullptr;
			for (std::size_t i = 0; i <= a_size - a_length; ++i) {
				bool matched = true;
				for (std::size_t j = 0; j < a_length; ++j) {
					if (a_pattern[j] >= 0 && a_begin[i + j] != static_cast<std::uint8_t>(a_pattern[j])) {
						matched = false;
						break;
					}
				}
				if (matched && ++a_outMatches == 1) {
					first = a_begin + i;
				}
			}
			return first;
		}

		// The file version resource, when the module actually carries one. PrismaUI does not -
		// it reports 0.0.0.0 - which is exactly why this is not the only identifier logged.
		std::string ModuleFileVersion(HMODULE a_module)
		{
			wchar_t path[MAX_PATH]{};
			if (!::GetModuleFileNameW(a_module, path, static_cast<DWORD>(std::size(path)))) {
				return {};
			}

			DWORD       ignored = 0;
			const DWORD size = ::GetFileVersionInfoSizeW(path, &ignored);
			if (!size) {
				return {};
			}

			std::vector<std::uint8_t> buffer(size);
			if (!::GetFileVersionInfoW(path, 0, size, buffer.data())) {
				return {};
			}

			VS_FIXEDFILEINFO* info = nullptr;
			UINT              length = 0;
			if (!::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) ||
				!info || length < sizeof(VS_FIXEDFILEINFO)) {
				return {};
			}

			if (!info->dwFileVersionMS && !info->dwFileVersionLS) {
				return {};  // Present but unstamped, which is no more use than absent.
			}

			return std::format("{}.{}.{}.{}",
				HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
				HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
		}

		// A signature is only ever valid for the builds it was checked against, so which build
		// we are looking at belongs in the log right next to the resolved address. Without it a
		// silent mismatch after a PrismaUI update is indistinguishable from the feature never
		// having worked at all.
		//
		// The link timestamp and image size come from the PE headers of the already-mapped
		// module, cost nothing, and change with every rebuild - so they identify a build even
		// for a DLL like PrismaUI that ships no version resource.
		std::string DescribeModuleBuild(HMODULE a_module)
		{
			ModuleImage image{};
			if (!OpenModuleImage(a_module, image)) {
				return "<unreadable PE header>";
			}

			const auto* nt = ModuleHeaders(image);
			const auto  stamp = nt->FileHeader.TimeDateStamp;

			char when[32] = "?";
			auto asTime = static_cast<std::time_t>(stamp);
			std::tm utc{};
			if (::gmtime_s(&utc, &asTime) == 0) {
				std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%SZ", &utc);
			}

			const auto version = ModuleFileVersion(a_module);
			return std::format("{}build 0x{:08X} ({}), image 0x{:X}",
				version.empty() ? "" : std::format("v{}, ", version),
				stamp, when, image.size);
		}

		// Locating the draw function is done once per stub. A failure is remembered as an
		// attempt so a missing module does not re-log on every policy application.
		void ResolveCursorStub(CursorStub& a_stub)
		{
			if (a_stub.resolved) {
				return;
			}
			a_stub.resolved = true;

			HMODULE module = ::GetModuleHandleW(a_stub.module);
			if (!module) {
				SKSE::log::info("{} is not loaded; nothing to do about its cursor.", a_stub.label);
				return;
			}

			std::uint8_t* text = nullptr;
			std::size_t   size = 0;
			if (!GetTextSection(module, text, size)) {
				SKSE::log::warn(
					"{} has no readable .text section; leaving its cursor alone.", a_stub.label);
				return;
			}

			const auto  build = DescribeModuleBuild(module);
			std::size_t matches = 0;
			auto* found = FindPattern(text, size, a_stub.signature, a_stub.signatureLength, matches);

			if (matches != 1) {
				SKSE::log::warn(
					"{} [{}] - cursor-draw signature matched {} time(s), expected exactly 1. "
					"Leaving its cursor alone, so expect two pointers in {}. This normally means "
					"the mod has been updated and the signature needs revisiting.",
					a_stub.label, build, matches, a_stub.noun);
				return;
			}

			if (a_stub.patch == StubPatch::kFunctionRet) {
				a_stub.site = found;
				a_stub.length = 1;
				a_stub.originalBytes[0] = *found;
				a_stub.patchBytes[0] = kRetOpcode;
				SKSE::log::info(
					"{} [{}] - resolved its cursor-draw function at +0x{:X} (0x{:X}).",
					a_stub.label,
					build,
					static_cast<std::uintptr_t>(found - reinterpret_cast<std::uint8_t*>(module)),
					reinterpret_cast<std::uintptr_t>(found));
				return;
			}

			// kFloatConstant. The signature begins on the guard's own load:
			//
			//   F3 0F 10 05 <disp32>   movss xmm0, [rip+disp32]
			//
			// so the constant is 8 bytes on (the length of that instruction, which is what RIP
			// holds by the time the displacement is applied) plus the displacement. Taking it
			// from the instruction rather than from a stored offset is the point: the constant
			// pool moves with every build, and this way we always patch whatever this specific
			// guard reads, or nothing at all.
			std::int32_t displacement = 0;
			std::memcpy(&displacement, found + 4, sizeof(displacement));
			auto* constant = found + 8 + displacement;

			// Everything from here is derived from bytes in somebody else's module, so none of
			// it is trusted: the address has to land inside the image, and on a 4-byte boundary,
			// or the store below would be neither safe nor atomic.
			ModuleImage image{};
			if (!OpenModuleImage(module, image)) {
				SKSE::log::warn("{} [{}] - unreadable PE header; leaving its cursor alone.",
					a_stub.label, build);
				return;
			}

			// Ordered so the range check happens before the subtraction that assumes it: a
			// displacement pointing below the image would make `constant - image.base` a
			// meaningless difference rather than an offset.
			const auto rva = constant >= image.base
				? static_cast<std::uint64_t>(constant - image.base)
				: 0;
			if (constant < image.base || !image.Contains(rva, sizeof(float)) ||
				(reinterpret_cast<std::uintptr_t>(constant) % alignof(float)) != 0) {
				SKSE::log::warn(
					"{} [{}] - its pointer guard reads 0x{:X}, which is not an aligned address "
					"inside the module. Leaving its cursor alone, so expect two pointers in {}.",
					a_stub.label, build, reinterpret_cast<std::uintptr_t>(constant), a_stub.noun);
				return;
			}

			// The last check, and the one that makes a drifted signature harmless rather than
			// destructive: a match that points at a constant holding something other than the
			// value the guard is documented to compare against is not this guard, whatever the
			// surrounding bytes looked like.
			float current = 0.0f;
			std::memcpy(&current, constant, sizeof(current));
			if (current != a_stub.expected) {
				SKSE::log::warn(
					"{} [{}] - its pointer guard reads {} where {} was expected, so this is not "
					"the constant the signature was written for. Leaving its cursor alone, so "
					"expect two pointers in {}. This normally means the mod has been updated and "
					"the signature needs revisiting.",
					a_stub.label, build, current, a_stub.expected, a_stub.noun);
				return;
			}

			a_stub.site = constant;
			a_stub.length = sizeof(float);
			std::memcpy(a_stub.originalBytes, &a_stub.expected, sizeof(float));
			std::memcpy(a_stub.patchBytes, &a_stub.replacement, sizeof(float));
			SKSE::log::info(
				"{} [{}] - resolved its pointer guard at +0x{:X}, reading the constant at "
				"+0x{:X} (0x{:X}).",
				a_stub.label,
				build,
				static_cast<std::uintptr_t>(found - image.base),
				rva,
				reinterpret_cast<std::uintptr_t>(constant));
		}

		void SetCursorStubSuppressed(CursorStub& a_stub, bool a_suppress)
		{
			if (!a_stub.site || a_stub.patched == a_suppress) {
				return;
			}

			const std::uint8_t* bytes = a_suppress ? a_stub.patchBytes : a_stub.originalBytes;
			const bool          isCode = a_stub.patch == StubPatch::kFunctionRet;

			// Code gets execute permission back, a constant does not - there is no reason to
			// leave a page of somebody else's .rdata executable behind us.
			DWORD previous = 0;
			if (!::VirtualProtect(a_stub.site, a_stub.length,
					isCode ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &previous)) {
				SKSE::log::warn("Could not unprotect {}'s cursor draw (error {}).",
					a_stub.label, ::GetLastError());
				return;
			}

			// Both widths are written in one naturally aligned store, and that is deliberate
			// rather than incidental: the render thread reads this every frame while we write
			// it, and on x86-64 an aligned store of one or four bytes is the width at which it
			// cannot be observed half-applied. Alignment of the four-byte case was checked when
			// the stub resolved.
			if (a_stub.length == sizeof(std::uint32_t)) {
				std::uint32_t value = 0;
				std::memcpy(&value, bytes, sizeof(value));
				*reinterpret_cast<volatile std::uint32_t*>(a_stub.site) = value;
			} else {
				*reinterpret_cast<volatile std::uint8_t*>(a_stub.site) = bytes[0];
			}

			::VirtualProtect(a_stub.site, a_stub.length, previous, &previous);
			if (isCode) {
				::FlushInstructionCache(::GetCurrentProcess(), a_stub.site, a_stub.length);
			}

			a_stub.patched = a_suppress;
			SKSE::log::info(
				"{} cursor {}.", a_stub.label, a_suppress ? "suppressed" : "restored");
		}

		bool WantPrismaCursorSuppressed()
		{
			const auto& config = Config::Get();
			switch (config.suppressPrismaCursor) {
			case CursorSuppression::kOff:
				return false;
			case CursorSuppression::kOn:
				return true;
			case CursorSuppression::kAuto:
			default:
				// Only while we are the ones drawing a pointer. In gamepad mode the cursor goes
				// back to the game, and taking Prisma's away as well would leave none at all.
				//
				// Not gated on g_active, unlike Party Sheet below: a Prisma view drives the
				// Cursor Menu, so any moment Prisma wants a pointer is a moment we are already
				// active. Nothing is left uncovered by suppressing for the whole session.
				return config.enabled && config.useHardwareCursor &&
					   !g_gamepadMode.load(std::memory_order_relaxed);
			}
		}

		bool WantPartySheetCursorSuppressed()
		{
			const auto& config = Config::Get();
			switch (config.suppressPartySheetCursor) {
			case CursorSuppression::kOff:
				return false;
			case CursorSuppression::kOn:
				return true;
			case CursorSuppression::kAuto:
			default:
				// Gated on being active, which Prisma's policy is not, and the difference
				// matters. Party Sheet paints its pointer for widgets its API does not report -
				// the horse picker is one - and those never bring us up. Suppressing for the
				// whole session would leave them with no pointer at all, so this only takes
				// their cursor away in the moments we are demonstrably drawing one instead.
				return config.enabled && config.useHardwareCursor &&
					   !g_gamepadMode.load(std::memory_order_relaxed) &&
					   g_active.load(std::memory_order_relaxed);
			}
		}

		bool WantGridInventoryCursorSuppressed()
		{
			const auto& config = Config::Get();
			switch (config.suppressGridInventoryCursor) {
			case CursorSuppression::kOff:
				return false;
			case CursorSuppression::kOn:
				return true;
			case CursorSuppression::kAuto:
			default:
				// Gated on being active, like Party Sheet rather than like Prisma, but for a
				// different reason: the grid menu carries kUsesCursor and opens the Cursor Menu
				// itself, so we come up a frame or two after it does. Suppressing for the whole
				// session would leave those frames with no pointer at all, and the ones after
				// them are covered anyway.
				//
				// The gamepad gate is doing more work here than in the two policies above. Grid
				// Inventory has a whole pad-cursor mode whose pointer is this same draw - its
				// author added it because pad players had no cursor otherwise - so suppressing
				// it on a gamepad would take away the only pointer on screen.
				return config.enabled && config.useHardwareCursor &&
					   !g_gamepadMode.load(std::memory_order_relaxed) &&
					   g_active.load(std::memory_order_relaxed);
			}
		}

		bool WantMeridianCursorSuppressed()
		{
			const auto& config = Config::Get();
			switch (config.suppressMeridianCursor) {
			case CursorSuppression::kOff:
				return false;
			case CursorSuppression::kOn:
				return true;
			case CursorSuppression::kAuto:
			default:
				// Prisma's rule, not Party Sheet's. Meridian draws its pointer only while one of
				// its browsers holds focus, and taking focus is what opens its focus menu - a
				// kUsesCursor menu that brings us up. So every moment Meridian wants a pointer is
				// a moment we are drawing one, and nothing is left uncovered by suppressing for
				// the whole session. The gamepad gate is the same as everywhere else: Meridian
				// hides the vanilla cursor while focused, so on a pad its sprite is the only one.
				return config.enabled && config.useHardwareCursor &&
					   !g_gamepadMode.load(std::memory_order_relaxed);
			}
		}

		bool GridInventoryMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			// Constructed once. IsMenuOpen takes a BSFixedString, and building one per call
			// would hash this literal on every sync tick.
			static const RE::BSFixedString name{ "GridInventoryMenu" };
			return ui->IsMenuOpen(name);
		}

		// The fallback for a Grid Inventory we could not patch, and the reason that mod needs
		// something the other two do not.
		//
		// Grid Inventory is updated often, and its signature is a guard inside an inlined
		// function - the kind of thing that moves on any build. When it fails to resolve we
		// cannot take their pointer away, so we give up ours instead for as long as their menu
		// is on screen: one frame-locked pointer drawn by them beats two.
		//
		// Deliberately not gated on g_active, unlike the suppression policy it backs up. This
		// is read by Activate() to decide whether to draw at all, so it has to be settled
		// before the activation it applies to rather than derived from it.
		bool ShouldYieldPointer()
		{
			const auto& config = Config::Get();
			if (!config.enabled || !config.useHardwareCursor ||
				config.suppressGridInventoryCursor == CursorSuppression::kOff ||
				g_gamepadMode.load(std::memory_order_relaxed)) {
				return false;
			}

			// Only when the module is actually loaded and the signature actually failed. A
			// resolved stub does the real thing, and an absent Grid Inventory must not cost
			// anybody their hardware cursor.
			if (!g_gridInventoryStub.resolved || g_gridInventoryStub.site ||
				!::GetModuleHandleW(g_gridInventoryStub.module)) {
				return false;
			}

			return GridInventoryMenuOpen();
		}

		void UpdatePointerYield()
		{
			const bool yield = ShouldYieldPointer();
			if (g_yieldPointer.exchange(yield, std::memory_order_relaxed) == yield) {
				return;
			}

			if (yield) {
				SKSE::log::info(
					"Grid Inventory could not be patched, so the hardware cursor is standing "
					"down while its menu is open - expect one pointer, drawn by Grid Inventory "
					"at frame rate.");
			} else {
				SKSE::log::info("Grid Inventory's menu closed; taking the hardware cursor back.");
			}
		}

		void ApplyPrismaCursorPolicy()
		{
			SetCursorStubSuppressed(g_prismaStub, WantPrismaCursorSuppressed());
		}

		void ApplyPartySheetCursorPolicy()
		{
			SetCursorStubSuppressed(g_partySheetStub, WantPartySheetCursorSuppressed());
		}

		void ApplyGridInventoryCursorPolicy()
		{
			SetCursorStubSuppressed(g_gridInventoryStub, WantGridInventoryCursorSuppressed());
		}

		void ApplyMeridianCursorPolicy()
		{
			SetCursorStubSuppressed(g_meridianStub, WantMeridianCursorSuppressed());
		}

		// Called wherever any policy's inputs can have changed. All are cheap and idempotent -
		// SetCursorStubSuppressed early-outs unless the desired state actually differs from the
		// applied one - so this is safe on the sync timer.
		void ApplyCursorSuppressionPolicies()
		{
			ApplyPrismaCursorPolicy();
			ApplyPartySheetCursorPolicy();
			ApplyGridInventoryCursorPolicy();
			ApplyMeridianCursorPolicy();
		}

		// ---------------------------------------------------------------------------
		// Skyrim Party Sheet panel tracking
		//
		// Party Sheet's panels are an ImGui overlay drawn inside the game's present hook, not
		// Scaleform menus. Nothing about them reaches the Cursor Menu or the menu stack, so
		// MenusWantCursor cannot see them by any of its usual means and we would stay inactive
		// for the entire time one is open - which is exactly when a smooth pointer is wanted.
		//
		// Party Sheet publishes its panel state over SKSE messaging, so no signature is
		// involved in the detection half of this: only the suppression half above is
		// build-sensitive. If Party Sheet is absent the interface never arrives, the mask
		// stays zero, and every path here answers false.
		// ---------------------------------------------------------------------------

		// Published once at kPostPostLoad and valid for the life of the process - Party Sheet
		// hands out a pointer to a function-local static.
		std::atomic<PartySheetAPI::IPartySheetState1*> g_partySheetState{ nullptr };

		// Fallback for the window between our listener registering and the interface arriving,
		// and for a future Party Sheet that broadcasts state without dispatching an interface.
		std::atomic<std::uint32_t> g_partySheetPanels{ 0 };

		bool PartySheetWantsCursor()
		{
			if (!Config::Get().trackPartySheetPanels) {
				return false;
			}

			// Prefer the live query. The broadcast is edge-triggered, so a cached mask is only
			// ever as fresh as the last message we happened to receive; asking the interface
			// cannot be stale by construction.
			if (auto* state = g_partySheetState.load(std::memory_order_acquire)) {
				return state->IsFullscreenPanelOpen();
			}

			return (g_partySheetPanels.load(std::memory_order_relaxed) &
					   PartySheetAPI::kInteractivePanelMask) != 0;
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

		// Returns whether the call reached the cursor movie at all. A false return means
		// nothing was applied, and callers that need the state to land - the periodic
		// re-asserts in both directions - retry on it.
		bool SetScaleformCursorVisible(bool a_visible, bool a_log = true)
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				if (a_log) {
					SKSE::log::warn("Cannot reach the UI singleton to hide the game cursor.");
				}
				return false;
			}

			auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME);
			if (!menu) {
				if (a_log) {
					SKSE::log::warn("Cursor Menu is not in the menu map; cannot hide the game cursor.");
				}
				return false;
			}
			if (!menu->uiMovie) {
				// The menu object can exist before its movie is loaded, which is why this is
				// re-asserted periodically rather than only on menu open.
				if (a_log) {
					SKSE::log::warn("Cursor Menu has no uiMovie yet; will retry.");
				}
				return false;
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

			g_scaleformHiddenByUs.store(!a_visible, std::memory_order_relaxed);

			if (!a_log) {
				return true;
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
			return true;
		}

		// Whoever currently sits at the head of the window's procedure chain.
		//
		// Both the A and W variants are queried deliberately. GetWindowLongPtr returns an
		// internal translation thunk rather than the real address whenever the caller's
		// ANSI/Unicode-ness disagrees with the window's, and the window's can change under us
		// the moment another module subclasses with the other variant. Asking with one variant
		// only is how this reports "not ours" for a window we are still perfectly well
		// attached to.
		LONG_PTR CurrentWndProc()
		{
			HWND hwnd = ResolveGameWindow();
			if (!hwnd) {
				return 0;
			}

			const auto ours = reinterpret_cast<LONG_PTR>(g_ourWndProc);
			const auto ansi = ::GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
			if (ours && ansi == ours) {
				return ansi;
			}

			const auto wide = ::GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
			if (ours && wide == ours) {
				return wide;
			}

			// Neither matched, so report the one that is not a thunk of the other. The wide
			// value is the more useful of the two to resolve to a module.
			return wide ? wide : ansi;
		}

		// True while our subclass is still the head of the chain. Another module subclassing
		// after us puts its procedure in front of ours, and it can then answer WM_SETCURSOR -
		// or swallow mouse messages outright - before we ever see them.
		bool WndProcIsOurs()
		{
			return g_ourWndProc != nullptr &&
				   CurrentWndProc() == reinterpret_cast<LONG_PTR>(g_ourWndProc);
		}

		// Names the module that owns an address, which is the whole point of the exercise:
		// "something subclassed after us" is not actionable, "SomePlugin.dll subclassed after
		// us" is. GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS resolves any address inside a loaded
		// image, and UNCHANGED_REFCOUNT keeps this from pinning the module in memory.
		std::string ModuleNameForAddress(LONG_PTR a_address)
		{
			if (!a_address) {
				return "<none>";
			}

			HMODULE module = nullptr;
			if (!::GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address),
					&module) ||
				!module) {
				return "<unowned>";
			}

			wchar_t path[MAX_PATH]{};
			if (!::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)))) {
				return "<unnamed>";
			}

			const std::string full = Narrow(path);
			const auto        slash = full.find_last_of("\\/");
			return slash == std::string::npos ? full : full.substr(slash + 1);
		}

		// True while the shape Windows is actually drawing is the one we asked for. False with
		// WndProcIsOurs also false is the signature of another framework owning the pointer.
		bool CursorIsOurs()
		{
			HCURSOR ours = g_customCursor ? g_customCursor : g_fallbackCursor;
			return ours != nullptr && ::GetCursor() == ours;
		}

		// The SWF a movie was loaded from. GFxMovie::GetMovieDef is vfunc 01 and
		// GFxMovieDef::GetFileURL is vfunc 0C, both stable across runtimes.
		//
		// This is what separates a genuine second cursor - another instance of cursormenu.swf
		// living under a different menu name - from an ordinary menu that merely carries
		// kUsesCursor. Journal Menu and MessageBoxMenu both set that flag and are not cursors.
		std::string MovieSourceFile(RE::GFxMovieView* a_movie)
		{
			if (!a_movie) {
				return "<null>";
			}

			auto* def = a_movie->GetMovieDef();
			if (!def) {
				return "<no def>";
			}

			const char* url = def->GetFileURL();
			if (!url || !*url) {
				return "<no url>";
			}

			// Only the leaf name; the full path is a long Data\Interface\... prefix that is
			// the same for every movie and would swamp the line.
			const std::string full = url;
			const auto        slash = full.find_last_of("\\/");
			return slash == std::string::npos ? full : full.substr(slash + 1);
		}

		// Every on-stack menu OTHER than the vanilla Cursor Menu that owns a Scaleform movie
		// and claims the cursor.
		//
		// This is the scan that finds a second cursor movie. A UI framework can load its own
		// instance of cursormenu.swf under its own menu name and set kUsesCursor on it -
		// PrismaUI's focus menu does exactly that - and suppressing the vanilla Cursor Menu
		// then leaves that copy still drawing, on top of our hardware cursor.
		std::string DescribeForeignCursorMovies()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return "<no ui>";
			}

			const auto* suppressed = g_suppressedMovie.load(std::memory_order_acquire);

			std::string out;
			for (const auto& entry : ui->menuMap) {
				const auto& menu = entry.second.menu;
				if (!menu || !menu->OnStack() || !menu->uiMovie) {
					continue;
				}

				const char* name = entry.first.c_str();
				if (name && RE::CursorMenu::MENU_NAME == name) {
					continue;
				}
				if (!menu->menuFlags.all(RE::UI_MENU_FLAGS::kUsesCursor)) {
					continue;
				}

				auto* movie = menu->uiMovie.get();
				if (!out.empty()) {
					out += ", ";
				}
				out += std::format(
					"{}[{}](movie=0x{:X} visible={}{})",
					name ? name : "<unnamed>",
					MovieSourceFile(movie),
					reinterpret_cast<std::uintptr_t>(movie),
					movie->GetVisible(),
					movie == suppressed ? " SUPPRESSED" : "");
			}

			return out.empty() ? "<none>" : out;
		}

		// Dumps a compact summary instead of a line per call. The counters answer the only
		// question that matters: does our hide stay applied between calls, and if the cursor
		// is still on screen, which other menus are up that might be drawing it?
		void LogDiagnosticSummary()
		{
			if (!Config::Get().verbose) {
				return;
			}

			// One second while a menu is up, three while idle. A Prisma menu can come and go
			// inside a single three second window, which left exactly one sample of the state
			// worth watching.
			const std::uint64_t interval =
				g_active.load(std::memory_order_relaxed) ? 1000 : 3000;

			static std::uint64_t lastTick = 0;
			const auto           now = ::GetTickCount64();
			if (now - lastTick < interval) {
				return;
			}
			lastTick = now;

			// Bounded so a long session does not accumulate thousands of lines. The budget is
			// refilled by Activate(), so it is spent on the menu the player just opened rather
			// than on whatever the game was doing beforehand.
			if (g_diagEmitted >= 40) {
				return;
			}
			++g_diagEmitted;

			// Every live menu object, tagged with the two things the activation decision reads:
			// '*' for on the stack (i.e. genuinely open), '+' for carrying kUsesCursor. An
			// untagged or '+'-only entry is a menu that exists but is closed - which is exactly
			// the case that used to keep the pointer on screen for a whole session.
			std::string menus;
			if (auto* ui = RE::UI::GetSingleton()) {
				for (const auto& entry : ui->menuMap) {
					const auto& menu = entry.second.menu;
					if (!menu) {
						continue;
					}
					if (!menus.empty()) {
						menus += ", ";
					}
					menus += entry.first.c_str();
					if (menu->OnStack()) {
						menus += '*';
					}
					if (menu->menuFlags.all(RE::UI_MENU_FLAGS::kUsesCursor)) {
						menus += '+';
					}
				}
			}

			SKSE::log::info(
				"[diag] activations={} hides={} cursorMovieWasVisibleOnEntry={} wasHidden={} "
				"lastMovie=0x{:X} showCursorCount={} | menus (*=on stack, +=uses cursor): {}",
				g_stats.activations,
				g_stats.entryVisible + g_stats.entryHidden,
				g_stats.entryVisible,
				g_stats.entryHidden,
				g_stats.lastMovie,
				g_lastCursorCount,
				menus.empty() ? "<none>" : menus);

			// Who is actually receiving the input, and who owns the pointer. `entered=0` while
			// a menu is up means our absolute positioning never runs for that menu, whatever
			// else the rest of the log says.
			SKSE::log::info(
				"[input] since activation: ProcessMouseMove entered={} engaged={} thumbstick={} "
				"| WM_TIMER={} WM_MOUSEMOVE={} WM_SETCURSOR={} answered={} | gamepad={} "
				"scaleformHiddenByUs={} cursorIsOurs={}",
				g_stats.mouseMoveCalls,
				g_stats.mouseMoveEngaged,
				g_stats.thumbstickCalls,
				g_stats.wmTimer,
				g_stats.wmMouseMove,
				g_stats.wmSetCursor,
				g_stats.wmSetCursorAnswered,
				g_gamepadMode.load(std::memory_order_relaxed),
				g_scaleformHiddenByUs.load(std::memory_order_relaxed),
				CursorIsOurs());

			// Which window we are attached to, which one the input is actually going to, and
			// who owns the head of the procedure chain. `ourWindow` differing from `underCursor`
			// means we subclassed the wrong window; them agreeing while `chainOwner` is another
			// module means that module is not chaining to us.
			POINT probe{};
			HWND  underCursor = ::GetCursorPos(&probe) ? ::WindowFromPoint(probe) : nullptr;
			const auto chainHead = CurrentWndProc();
			SKSE::log::info(
				"[window] ourWindow=0x{:X} foreground=0x{:X} underCursor=0x{:X} | "
				"chainHead=0x{:X} owner={} isOurs={}",
				reinterpret_cast<std::uintptr_t>(g_window),
				reinterpret_cast<std::uintptr_t>(::GetForegroundWindow()),
				reinterpret_cast<std::uintptr_t>(underCursor),
				static_cast<std::uintptr_t>(chainHead),
				ModuleNameForAddress(chainHead),
				WndProcIsOurs());

			// Movie identity. `match=false` means our suppression is pointed at a movie the
			// game has since replaced, so the live cursor movie is drawing unsuppressed.
			std::uintptr_t vanillaMovie = 0;
			std::string    vanillaSource = "<none>";
			if (auto* ui = RE::UI::GetSingleton()) {
				if (auto menu = ui->GetMenu(RE::CursorMenu::MENU_NAME); menu && menu->uiMovie) {
					vanillaMovie = reinterpret_cast<std::uintptr_t>(menu->uiMovie.get());
					vanillaSource = MovieSourceFile(menu->uiMovie.get());
				}
			}
			const auto suppressedMovie =
				reinterpret_cast<std::uintptr_t>(g_suppressedMovie.load(std::memory_order_acquire));
			SKSE::log::info(
				"[movies] vanillaCursorMovie=0x{:X}[{}] suppressed=0x{:X} match={} | "
				"other cursor-flagged menus: {}",
				vanillaMovie,
				vanillaSource,
				suppressedMovie,
				vanillaMovie != 0 && vanillaMovie == suppressedMovie,
				DescribeForeignCursorMovies());

			// The direct measurement of the sensitivity complaint: if gameCursor does not track
			// osClient scaled by span, the game's pointer is still integrating its own
			// fps-scaled delta and our absolute write is not landing.
			POINT osPoint{};
			const bool osOk = ::GetCursorPos(&osPoint) != FALSE;
			POINT      clientPoint = osPoint;
			if (osOk && g_window) {
				::ScreenToClient(g_window, &clientPoint);
			}
			auto* menuCursor = RE::MenuCursor::GetSingleton();
			SKSE::log::info(
				"[track] osScreen=({}, {}) osClient=({}, {}) gameCursor=({:.1f}, {:.1f}) "
				"screenWidth=({:.1f}, {:.1f}) sensitivity={:.3f}",
				osPoint.x, osPoint.y,
				clientPoint.x, clientPoint.y,
				menuCursor ? menuCursor->cursorPosX : -1.0f,
				menuCursor ? menuCursor->cursorPosY : -1.0f,
				menuCursor ? menuCursor->screenWidthX : -1.0f,
				menuCursor ? menuCursor->screenWidthY : -1.0f,
				menuCursor ? menuCursor->cursorSensitivity : -1.0f);
		}

		// Re-applies the game-cursor hide, periodically rather than only on menu open.
		//
		// Two things make the one-shot hide in Activate unreliable. The game re-shows its
		// cursor on some menu transitions; and Activate runs off the Cursor Menu's open hint,
		// which by design fires a frame or two before the menu reaches the stack - so the menu
		// may not be in the map yet, may still be the outgoing instance, or may not have
		// loaded its uiMovie. All three miss the movie that actually draws, and
		// SetScaleformCursorVisible returns quietly when they do.
		//
		// Guards on g_active itself rather than trusting the caller. Yielding is deliberately
		// not a guard: the mod we yield to hides this cursor too, so keeping it down is right.
		void ReassertScaleformHidden()
		{
			const auto& config = Config::Get();
			if (!g_active.load(std::memory_order_relaxed) ||
				!config.useHardwareCursor || !config.hideGameCursor ||
				g_gamepadMode.load(std::memory_order_relaxed)) {
				return;
			}

			const auto now = ::GetTickCount64();
			if (now - g_lastReassertTick < 250) {
				return;
			}
			g_lastReassertTick = now;

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

		// The mirror of ReassertScaleformHidden, for as long as a gamepad owns the cursor.
		//
		// In gamepad mode the game's own pointer is the only one on screen - the OS cursor is
		// already down - so a hide of ours that is still in effect leaves the player with no
		// cursor at all. That used to be exactly what happened on the world map: gamepad mode
		// is sticky until the mouse moves, and a menu opening or closing over the map (the
		// fast-travel prompt, the map opening under an already-open Cursor Menu) re-hid the
		// movie with nothing to ever show it again. EnterGamepadMode undoes the hide once, on
		// the transition, but that is a single attempt against a menu that may not have its
		// movie yet, and it cannot see a hide that lands afterwards.
		//
		// Runs from the sync timer. Only ever undoes what this plugin applied: the tracked
		// hide, the suppressed-movie reference and the saved viewport are all ours.
		void ReassertScaleformShown()
		{
			if (!g_gamepadMode.load(std::memory_order_relaxed)) {
				return;
			}

			const bool hidden = g_scaleformHiddenByUs.load(std::memory_order_relaxed);
			const bool suppressed = g_suppressedMovie.load(std::memory_order_acquire) != nullptr;
			if (!hidden && !suppressed && !g_viewportSaved) {
				return;
			}

			const auto now = ::GetTickCount64();
			if (now - g_lastReshowTick < 250) {
				return;
			}
			g_lastReshowTick = now;

			if (SetScaleformCursorVisible(true, false)) {
				SKSE::log::info(
					"Game cursor was hidden while a gamepad owns it; shown again.");
				return;
			}

			// The call found no Cursor Menu, or one without a movie yet. Either way the
			// movie we are still holding suppressed belongs to an instance that is gone or
			// going, and so does the saved viewport - the same reasoning Deactivate applies.
			// The tracked hide is left set so the next tick tries the new movie again.
			if (suppressed || g_viewportSaved) {
				ClearSuppressedMovie();
				g_viewportSaved = false;
				SKSE::log::info(
					"Dropped a stale cursor-movie suppression while a gamepad owns the cursor.");
			}
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
			if (clientW <= 1.0f || clientH <= 1.0f) {
				return false;
			}

			// Divide by the last addressable pixel, not the pixel count. ScreenToClient tops
			// out at clientH - 1, so dividing by clientH makes the bottom row land just short
			// of spanY and the far edge unreachable - 1599 of 1600, never 1600. The game's own
			// integration saturates against its clamp and sits at exactly the maximum, so
			// anything testing for the cursor being *at* the edge sees true from vanilla and
			// false from us. That is why the world map would pan up but never down: an error
			// of this shape is zero at the origin and maximal at the opposite edge, so top and
			// left were always fine while bottom and right silently lost the last unit.
			const float normalizedX = std::clamp(static_cast<float>(point.x) / (clientW - 1.0f), 0.0f, 1.0f);
			const float normalizedY = std::clamp(static_cast<float>(point.y) / (clientH - 1.0f), 0.0f, 1.0f);

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
			if (clientW <= 1.0f || clientH <= 1.0f) {
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

			// The inverse of ComputeAbsolutePosition, and it has to scale by the same extent:
			// against clientW a cursor sitting at spanX would target clientW, one past the last
			// valid pixel, and get pushed back a pixel by the clip rect on arrival.
			POINT target{
				static_cast<LONG>(std::lround(normalizedX * (clientW - 1.0f))),
				static_cast<LONG>(std::lround(normalizedY * (clientH - 1.0f)))
			};
			if (::ClientToScreen(hwnd, &target)) {
				::SetCursorPos(target.x, target.y);
			}
		}

		// ---------------------------------------------------------------------------
		// Window procedure
		// ---------------------------------------------------------------------------

		// Drives SyncActiveState/AssertCursorState from the window's own message queue, so
		// neither depends on the player moving the mouse first.
		constexpr UINT_PTR kSyncTimerId = 0xC0DE;

		// The live menu state is what decides this, not the event stream: the Cursor Menu can
		// already be open before our sink is registered, and the events only ever describe
		// the Cursor Menu itself - some menus take the pointer without it appearing at all.
		//
		// The one thing the live state cannot tell us is that a menu is *about* to open. The
		// Cursor Menu's open event arrives a frame or two before the menu reaches the stack,
		// so IsMenuOpen still answers false at that point. Acting on the event alone and
		// letting the poll correct it a moment later is what produced a hide and re-show
		// within 3ms of every menu opening; this bridges the gap instead. It expires on its
		// own, so a missed close event cannot latch the plugin on.
		std::atomic<std::uint64_t> g_cursorMenuOpenHint{ 0 };  // tick, 0 = no hint

		constexpr std::uint64_t kOpenHintLifetimeMs = 1000;

		bool MenusWantCursor()
		{
			// Ahead of the UI singleton deliberately: a Party Sheet panel is not a menu, and
			// whether the game's menu system is reachable has no bearing on it.
			if (PartySheetWantsCursor()) {
				return true;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}

			if (ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
				// The real state has caught up, so stop trusting the hint from here on.
				g_cursorMenuOpenHint.store(0, std::memory_order_relaxed);
				return true;
			}

			const auto hint = g_cursorMenuOpenHint.load(std::memory_order_relaxed);
			if (hint && ::GetTickCount64() - hint < kOpenHintLifetimeMs) {
				return true;
			}

			// Fallback for menus that drive the pointer without the Cursor Menu being open -
			// the main menu at startup being the one everybody hits. Always-open menus (the
			// HUD) are skipped so this cannot latch on during normal gameplay.
			//
			// OnStack is the part that makes this safe: a non-null entry in menuMap only means
			// the menu object exists, not that it is open (UI::IsMenuOpen is
			// `menu && menu->OnStack()`). Without it, one instantiated-but-closed menu
			// carrying kUsesCursor keeps the pointer on screen for the rest of the session.
			//
			// Read kUsesCursor off menuFlags directly: IMenu::UsesCursor() in CommonLibSSE-NG
			// tests kUsesMenuContext, one of a run of five accessors bound to the wrong flag.
			for (const auto& entry : ui->menuMap) {
				const auto& menu = entry.second.menu;
				if (menu && menu->OnStack() && !menu->AlwaysOpen() &&
					menu->menuFlags.all(RE::UI_MENU_FLAGS::kUsesCursor)) {
					return true;
				}
			}

			return false;
		}

		// Re-applies the two things that do not stay applied on their own: the OS display
		// counter, which the game keeps pushing negative, and the cursor image, which
		// DefWindowProc resets to the window class cursor - null, for Skyrim - on every
		// WM_SETCURSOR we do not answer ourselves.
		//
		// Idempotent and cheap, so it can run from the sync timer as well as from mouse
		// messages. Running it off the timer is the point: mouse messages only arrive once
		// the player moves the mouse, and at the main menu the mouse is usually still.
		void AssertCursorState()
		{
			if (!g_active.load(std::memory_order_relaxed) ||
				!Config::Get().useHardwareCursor ||
				g_gamepadMode.load(std::memory_order_relaxed) ||
				g_yieldPointer.load(std::memory_order_relaxed)) {
				return;
			}

			ForceCursorShown();
			if (HCURSOR cursor = g_customCursor ? g_customCursor : g_fallbackCursor) {
				::SetCursor(cursor);
			}
		}

		// The exact mirror of AssertCursorState, and the fix for the pointer that stayed on
		// screen after a menu closed.
		//
		// The display counter is process-wide and everything writes to it: the game, other
		// SKSE plugins, SSEDisplayTweaks for its borderless window. Hiding once in Deactivate
		// left any later ShowCursor(TRUE) permanently unanswered, because every re-assertion
		// we had was gated on being active. Alt-tabbing out and back was the only cure, and
		// only because the game re-runs its own hide path on focus regain.
		//
		// ForceCursorHidden normalizes to exactly -1 and is idempotent from there - it probes
		// to -2 and restores - so this is safe to run forever at a low rate.
		void AssertCursorHidden()
		{
			const auto& config = Config::Get();
			if (!config.enforceHiddenWhenInactive || !config.useHardwareCursor) {
				return;
			}

			// Only while the cursor is not ours to show. Gamepad mode counts as not ours: the
			// game draws its own pointer there and wants the OS one gone. So does yielding to a
			// mod drawing its own, which is the same bargain struck with a different party.
			if (g_active.load(std::memory_order_relaxed) &&
				!g_gamepadMode.load(std::memory_order_relaxed) &&
				!g_yieldPointer.load(std::memory_order_relaxed)) {
				return;
			}

			// Nothing to undo until we have actually shown the cursor at least once. Before
			// that, leaving the counter alone keeps us out of the way of mods that legitimately
			// want a pointer during gameplay.
			if (g_stats.activations == 0) {
				return;
			}

			static std::uint64_t lastTick = 0;
			const auto           now = ::GetTickCount64();
			if (now - lastTick < 250) {
				return;
			}
			lastTick = now;

			ForceCursorHidden();
		}

		// a_force bypasses the throttle, for the callers that are reacting to a menu event
		// rather than polling.
		void SyncActiveState(bool a_force = false)
		{
			if (!g_runtimeReady.load(std::memory_order_relaxed) || !Config::Get().enabled) {
				return;
			}

			static std::uint64_t lastTick = 0;
			const auto           now = ::GetTickCount64();
			if (!a_force && now - lastTick < 32) {
				return;
			}
			lastTick = now;

			// Emitted from here as well as from the mouse hook, so the menu list is still
			// captured while inactive - which is the state to inspect if a menu ever ends up
			// with no cursor again.
			LogDiagnosticSummary();

			// Before the activation decision, not after it: Activate() reads this to decide
			// whether to draw a pointer at all, so a stale value would show one for a tick on
			// the frame the grid inventory opens.
			UpdatePointerYield();

			const bool shouldBeActive = MenusWantCursor();
			const bool isActive = g_active.load(std::memory_order_relaxed);
			if (shouldBeActive && !isActive) {
				Activate();
			} else if (!shouldBeActive && isActive) {
				Deactivate();
			}

			// After the decision, never before it: the Party Sheet policy reads g_active, so
			// applying it first would suppress or restore against the previous frame's state
			// and leave a frame showing either two pointers or none.
			ApplyCursorSuppressionPolicies();
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wparam, LPARAM a_lparam)
		{
			switch (a_msg) {
			case WM_TIMER:
				// The only assertion point here that does not depend on mouse input. Without it
				// a menu that opens while the mouse is still - the main menu at startup, most
				// obviously - has nothing to bring the pointer up.
				if (a_wparam == kSyncTimerId) {
					++g_stats.wmTimer;
					SyncActiveState();
					if (!g_window || ::GetForegroundWindow() == g_window) {
						AssertCursorState();
						AssertCursorHidden();
						// The game cursor needs the same treatment as the OS one, and for the
						// same reason: the hide on menu open can miss the movie, and until this
						// ran here the only thing that ever retried was the mouse-move hook. A
						// menu opened with the mouse held still therefore kept the game's
						// pointer on screen until the player moved it.
						ReassertScaleformHidden();
						ReassertScaleformShown();
					}
					return 0;
				}
				break;

			case WM_MOUSEMOVE:
				++g_stats.wmMouseMove;
				// Re-assert the image here too. WM_SETCURSOR is not guaranteed to arrive on
				// every move if something else in the process answers it first.
				SyncActiveState();
				AssertCursorState();
				AssertCursorHidden();
				break;

			case WM_SETCURSOR:
				++g_stats.wmSetCursor;
				// The game does not handle WM_SETCURSOR itself, so this is where we get to name
				// the cursor image. Answering it rather than falling through to DefWindowProc
				// is what stops the window class cursor being applied over ours a moment later.
				SyncActiveState();
				if (g_active.load(std::memory_order_relaxed) && Config::Get().useHardwareCursor &&
					!g_gamepadMode.load(std::memory_order_relaxed) &&
					!g_yieldPointer.load(std::memory_order_relaxed) && LOWORD(a_lparam) == HTCLIENT) {
					HCURSOR cursor = g_customCursor ? g_customCursor : g_fallbackCursor;
					if (!cursor) {
						// Never pass null here - that would hide the pointer entirely.
						cursor = ::LoadCursorW(nullptr, IDC_ARROW);
					}
					::SetCursor(cursor);
					ForceCursorShown();
					++g_stats.wmSetCursorAnswered;
					return TRUE;
				}
				break;

			case WM_ACTIVATE:
			case WM_ACTIVATEAPP:
				{
					const bool activating = (a_msg == WM_ACTIVATE)
						? (LOWORD(a_wparam) != WA_INACTIVE)
						: (a_wparam != FALSE);
					if (activating) {
						// Synced unconditionally rather than only when already active: coming back
						// to the window is exactly when a menu we never saw open has to be picked
						// up.
						SyncActiveState();
						if (g_active.load(std::memory_order_relaxed)) {
							ApplyClip(Config::Get().clipToWindow);
							AssertCursorState();
						}
					} else if (g_active.load(std::memory_order_relaxed)) {
						::ClipCursor(nullptr);
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
			g_ourWndProc = &WndProc;

			SKSE::log::info(
				"Window procedure chain at hook time: previous owner={} (0x{:X}), we are now the head.",
				ModuleNameForAddress(reinterpret_cast<LONG_PTR>(g_originalWndProc)),
				reinterpret_cast<std::uintptr_t>(g_originalWndProc));

			// ~60 Hz. WM_TIMER is the lowest-priority message there is, so this never competes
			// with input or painting - it only guarantees us a look in every frame or so even
			// when no mouse messages are being generated at all.
			const bool timerOk = ::SetTimer(hwnd, kSyncTimerId, 15, nullptr) != 0;

			SKSE::log::info(
				"Window procedure hooked (hwnd={}, unicode={}, syncTimer={}).",
				static_cast<void*>(hwnd),
				g_windowIsUnicode,
				timerOk);
			if (!timerOk) {
				SKSE::log::warn(
					"Could not start the sync timer (error {}); menus will only pick the cursor up "
					"once the mouse moves.",
					::GetLastError());
			}
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

			// Not when a Party Sheet panel is what brought us up. The sync exists to stop the
			// pointer jumping when a menu opens, by moving the OS cursor to wherever the
			// game's menu cursor already sits - but a Party Sheet panel is not a game menu and
			// never touched MenuCursor, so those coordinates are left over from the last real
			// menu. Syncing to them would cause the jump rather than prevent it, and Party
			// Sheet places its own hit-testing from the OS position, so there is nothing to
			// reconcile with in the first place.
			if (config.syncOnMenuOpen && !PartySheetWantsCursor()) {
				SyncOsCursorToGame();
			}

			const bool yielded = g_yieldPointer.load(std::memory_order_relaxed);

			if (config.useHardwareCursor && !g_gamepadMode.load(std::memory_order_relaxed)) {
				if (config.hideGameCursor) {
					SetScaleformCursorVisible(false, false);
				}
				// The Scaleform cursor above stays down even when yielding - the mod we are
				// yielding to hides it itself, and putting it back here would be a third pointer
				// on screen. Only the showing of ours is skipped; AssertCursorHidden takes the
				// OS cursor back down from here.
				if (!yielded) {
					ForceCursorShown();
					::SetCursor(g_customCursor ? g_customCursor : g_fallbackCursor);
				}
			} else if (config.useHardwareCursor) {
				// Gamepad mode carried over from an earlier menu. The game's pointer is the
				// one that has to be visible here, so make sure no hide of ours is still in
				// effect; the timer retries if the movie is not loaded yet.
				SetScaleformCursorVisible(true, false);
			}

			ApplyClip(config.clipToWindow);

			// The hide above may have found no menu, the outgoing one, or no uiMovie yet.
			// Zeroing the throttle lets the next sync tick retry at once instead of waiting
			// out a window that a mouse move just before the menu opened may have half spent.
			g_lastReassertTick = 0;

			++g_stats.activations;

			// Refill the diagnostic budget and zero the per-activation counters, so the log
			// describes the menu that just opened rather than everything before it.
			g_diagEmitted = 0;
			g_stats.ResetPerActivation();

			SKSE::log::info(
				"Activated (hardwareCursor={}, customArt={}, showCursorCount={}, yielded={}).",
				config.useHardwareCursor,
				g_customCursor != nullptr,
				g_lastCursorCount,
				yielded);
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
				g_scaleformHiddenByUs.store(false, std::memory_order_relaxed);
			}

			SKSE::log::info(
				"Deactivated (showCursorCount={}) | this session: ProcessMouseMove entered={} "
				"engaged={} thumbstick={} | WM_TIMER={} WM_MOUSEMOVE={} WM_SETCURSOR={} "
				"answered={} | wndProcIsOurs={} chainOwner={} cursorIsOurs={}",
				g_lastCursorCount,
				g_stats.mouseMoveCalls,
				g_stats.mouseMoveEngaged,
				g_stats.thumbstickCalls,
				g_stats.wmTimer,
				g_stats.wmMouseMove,
				g_stats.wmSetCursor,
				g_stats.wmSetCursorAnswered,
				WndProcIsOurs(),
				ModuleNameForAddress(CurrentWndProc()),
				CursorIsOurs());
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

			// Give the game its own pointer back and get ours off the screen. The suppressed
			// sprites come back too - with no hardware cursor there is nothing to double up.
			SetScaleformCursorVisible(true, false);
			ForceCursorHidden();
			ApplyCursorSuppressionPolicies();
		}

		void ExitGamepadMode()
		{
			if (!g_gamepadMode.exchange(false)) {
				return;
			}

			SKSE::log::info("Mouse input resumed - taking the cursor back.");

			ApplyCursorSuppressionPolicies();

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
				++g_stats.thumbstickCalls;

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
				++g_stats.mouseMoveCalls;

				const auto& config = Config::Get();

				// Any real mouse movement means the player is back on the mouse.
				if (a_event && (a_event->mouseInputX != 0 || a_event->mouseInputY != 0) &&
					g_gamepadMode.load(std::memory_order_relaxed)) {
					ExitGamepadMode();
				}

				// Whether a menu currently has the pointer. This is what decides both halves
				// below - AbsolutePositioning gates only the position write, because the game's
				// own cursor has to be kept hidden either way.
				const bool active =
					g_runtimeReady.load(std::memory_order_relaxed) &&
					g_active.load(std::memory_order_relaxed) &&
					!g_gamepadMode.load(std::memory_order_relaxed) &&
					config.enabled;

				bool handled = false;
				if (active && config.absolutePositioning) {
					handled = DriveAbsolutePosition(a_this, a_event);
				} else {
					handled = func(a_this, a_event);
				}

				// Deliberately outside the positioning branch. Some menus re-show the game
				// cursor every frame, and the one-shot hides on menu open lose that race, so
				// the hide has to be re-asserted rather than trusted. Hanging it off
				// AbsolutePositioning meant the documented AbsolutePositioning = false fallback
				// silently gave up the suppression too, and drew both cursors.
				//
				// The sync timer re-asserts as well, which is what covers a menu opened while
				// the mouse is still. This call is still worth keeping: it catches the re-show
				// on the frame the player moves, without waiting for the next tick.
				if (active) {
					ReassertScaleformHidden();
				}

				return handled;
			}

			// The absolute-positioning half, split out so the caller has a single exit and
			// the cursor re-assert above cannot be skipped by one of its early returns.
			static bool DriveAbsolutePosition(RE::MenuEventHandler* a_this, RE::MouseMoveEvent* a_event)
			{
				const auto& config = Config::Get();

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
				++g_stats.mouseMoveEngaged;

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

				return handled;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_CursorMenu[1] };

		// 1.7.99 inserted ProcessMotionGesture and ProcessSixaxis into MenuEventHandler
		// directly after CanProcess, pushing everything below them down by two slots.
		// CommonLibSSE applies the same shift to its own MenuEventHandler wrappers (see
		// AE1799_SLOT_SHIFT in RE/M/MenuEventHandler.h); we patch the vtable by raw index,
		// so the shift has to be repeated here. Getting this wrong is silent rather than
		// loud - the pre-shift indices land on the two new virtuals, so the mouse hook
		// simply never fires and a sixaxis event arrives typed as a MouseMoveEvent.
		const std::size_t shift = REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? 2 : 0;
		const std::size_t mouseMoveIndex = 0x4 + shift;
		const std::size_t thumbstickIndex = 0x3 + shift;

		ProcessMouseMoveHook::func = vtable.write_vfunc(mouseMoveIndex, ProcessMouseMoveHook::thunk);
		ProcessThumbstickHook::func = vtable.write_vfunc(thumbstickIndex, ProcessThumbstickHook::thunk);

		// The resolved indices are logged because they are the first thing worth checking
		// when the cursor does nothing on a runtime this build has not been tested against.
		SKSE::log::info(
			"Hooked CursorMenu::ProcessMouseMove (vfunc {:#x}) and ProcessThumbstick (vfunc {:#x}).",
			mouseMoveIndex,
			thumbstickIndex);

		if (Config::Get().blockGameCursorHide) {
			// The executable first, so its original is the one we keep as the real function.
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
		// Deferred to kDataLoaded rather than done alongside the executable patch above, so
		// the sweep sees every SKSE plugin that will ever be loaded.
		if (Config::Get().blockGameCursorHide && Config::Get().hookAllModules) {
			PatchShowCursorEverywhere();
		}

		ResolveGameWindow();
		LoadCursorArt();
		HookWindowProc();

		// Independent of the window and of everything above it - kept out of HookWindowProc
		// so a failure to find the game window does not also cost us this.
		ResolveCursorStub(g_prismaStub);
		ResolveCursorStub(g_partySheetStub);
		ResolveCursorStub(g_gridInventoryStub);
		ResolveCursorStub(g_meridianStub);
		ApplyCursorSuppressionPolicies();

		g_runtimeReady.store(true);

		// A menu may already be up when we initialise. Usually one is not: kDataLoaded fires
		// before the main menu appears, which is why the sync timer has to exist.
		SyncActiveState(true);
	}

	void Shutdown()
	{
		Deactivate();

		// Hand every patched mod its own code back before we go, so a reloaded or unloaded
		// plugin does not leave another mod permanently stubbed.
		SetCursorStubSuppressed(g_prismaStub, false);
		SetCursorStubSuppressed(g_partySheetStub, false);
		SetCursorStubSuppressed(g_gridInventoryStub, false);
		SetCursorStubSuppressed(g_meridianStub, false);

		if (g_window && ::IsWindow(g_window)) {
			::KillTimer(g_window, kSyncTimerId);
		}

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

	void OnPartySheetMessage(std::uint32_t a_type, void* a_data, std::uint32_t a_dataLen)
	{
		if (!Config::Get().enabled) {
			return;
		}

		switch (a_type) {
		case PartySheetAPI::kMsg_Interface:
			{
				// The payload IS the interface pointer - Party Sheet passes it as the message
				// data with dataLen set to sizeof(void*), not as a struct containing one.
				auto* state = static_cast<PartySheetAPI::IPartySheetState1*>(a_data);
				if (!state || a_dataLen != sizeof(void*)) {
					SKSE::log::warn(
						"Party Sheet sent an interface message with an unexpected payload "
						"({} bytes, expected {}); ignoring it.", a_dataLen, sizeof(void*));
					return;
				}

				// Ask before calling anything else through it. GetVersion is the second vtable
				// slot in every version by construction, so it is the one virtual that is safe
				// to call on an interface we have not yet agreed a layout with; the rest are
				// only safe once it answers 1.
				const auto version = state->GetVersion();
				if (version != 1) {
					SKSE::log::warn(
						"Party Sheet's state interface reports version {}, and this build only "
						"understands version 1. Not tracking its panels - update Cursor Unbound.",
						version);
					return;
				}

				g_partySheetState.store(state, std::memory_order_release);
				SKSE::log::info(
					"Skyrim Party Sheet detected; tracking its panels for cursor activation "
					"(state interface v{}).", version);

				// A panel can already be open by the time this arrives, so decide again rather
				// than waiting for the next timer tick.
				SyncActiveState(true);
			}
			break;

		case PartySheetAPI::kMsg_State:
			{
				if (!a_data || a_dataLen < sizeof(PartySheetAPI::StateMsg)) {
					return;
				}
				const auto* msg = static_cast<const PartySheetAPI::StateMsg*>(a_data);
				if (msg->version != 1) {
					return;
				}
				g_partySheetPanels.store(msg->openPanels, std::memory_order_relaxed);

				// Edge-triggered, and therefore the earliest this can be known. Reacting here
				// rather than on the next 32ms tick is the difference between the pointer
				// appearing with the panel and appearing shortly after it.
				SyncActiveState(true);
			}
			break;

		default:
			break;
		}
	}

	void OnMenuOpenClose(std::string_view a_menuName, bool a_opening)
	{
		if (!Config::Get().enabled) {
			return;
		}

		if (a_menuName == RE::CursorMenu::MENU_NAME) {
			// The event is a hint, not the decision. SyncActiveState is the single decider so
			// that the event and the sync timer cannot disagree and flip the cursor twice in
			// the same frame - which is what every Activated/Deactivated/Activated triple in a
			// 1.0.1 log was.
			g_cursorMenuOpenHint.store(a_opening ? ::GetTickCount64() : 0, std::memory_order_relaxed);
			SyncActiveState(true);
			return;
		}

		// Menus stacking on top of the cursor menu can re-show the Scaleform pointer, so
		// re-assert our state whenever anything else opens or closes.
		//
		// Not while a gamepad owns the cursor. This was the one hide not gated on gamepad
		// mode, and with the OS cursor already hidden there it left the world map with no
		// pointer at all after any menu event - the fast-travel prompt being the reliable
		// one. ReassertScaleformShown catches anything that still slips through.
		if (g_active.load(std::memory_order_relaxed) &&
			!g_gamepadMode.load(std::memory_order_relaxed) &&
			Config::Get().useHardwareCursor &&
			Config::Get().hideGameCursor) {
			SetScaleformCursorVisible(false, false);
		}
	}
}
