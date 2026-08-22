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
			if (!a_show && g_active.load(std::memory_order_relaxed) &&
				!g_gamepadMode.load(std::memory_order_relaxed) &&
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
		// PrismaUI cursor suppression
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

		std::uint8_t* g_prismaDrawCursor = nullptr;
		std::uint8_t  g_prismaOriginalByte = 0;
		bool          g_prismaPatched = false;

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
		constexpr int kDrawCursorSig[] = {
			0x40, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x28, 0x02, 0x00, 0x00,
			0x48, 0x8B, 0x05,   -1,   -1,   -1,   -1,
			0x48, 0x33, 0xC4,
			0x48, 0x89, 0x84, 0x24, 0x10, 0x02, 0x00, 0x00,
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

		void ResolvePrismaDrawCursor()
		{
			if (g_prismaDrawCursor) {
				return;
			}

			HMODULE module = ::GetModuleHandleW(L"PrismaUI.dll");
			if (!module) {
				SKSE::log::info("PrismaUI is not loaded; nothing to do about its cursor.");
				return;
			}

			std::uint8_t* text = nullptr;
			std::size_t   size = 0;
			if (!GetTextSection(module, text, size)) {
				SKSE::log::warn("PrismaUI.dll has no readable .text section; leaving its cursor alone.");
				return;
			}

			const auto  build = DescribeModuleBuild(module);
			std::size_t matches = 0;
			auto*       found = FindPattern(text, size, kDrawCursorSig, std::size(kDrawCursorSig), matches);

			if (matches != 1) {
				SKSE::log::warn(
					"PrismaUI.dll [{}] - DrawCursor signature matched {} time(s), expected exactly 1. "
					"Leaving Prisma's cursor alone, so expect two pointers in Prisma menus. This "
					"normally means PrismaUI has been updated and the signature needs revisiting.",
					build, matches);
				return;
			}

			g_prismaDrawCursor = found;
			g_prismaOriginalByte = *found;
			SKSE::log::info(
				"PrismaUI.dll [{}] - resolved ViewRenderer::DrawCursor at +0x{:X} (0x{:X}).",
				build,
				static_cast<std::uintptr_t>(found - reinterpret_cast<std::uint8_t*>(module)),
				reinterpret_cast<std::uintptr_t>(found));
		}

		void SetPrismaCursorSuppressed(bool a_suppress)
		{
			if (!g_prismaDrawCursor || g_prismaPatched == a_suppress) {
				return;
			}

			const std::uint8_t byte = a_suppress ? kRetOpcode : g_prismaOriginalByte;

			DWORD previous = 0;
			if (!::VirtualProtect(g_prismaDrawCursor, 1, PAGE_EXECUTE_READWRITE, &previous)) {
				SKSE::log::warn(
					"Could not unprotect PrismaUI's DrawCursor (error {}).", ::GetLastError());
				return;
			}
			*g_prismaDrawCursor = byte;
			::VirtualProtect(g_prismaDrawCursor, 1, previous, &previous);
			::FlushInstructionCache(::GetCurrentProcess(), g_prismaDrawCursor, 1);

			g_prismaPatched = a_suppress;
			SKSE::log::info("PrismaUI cursor sprite {}.", a_suppress ? "suppressed" : "restored");
		}

		bool WantPrismaCursorSuppressed()
		{
			const auto& config = Config::Get();
			switch (config.suppressPrismaCursor) {
			case PrismaSuppression::kOff:
				return false;
			case PrismaSuppression::kOn:
				return true;
			case PrismaSuppression::kAuto:
			default:
				// Only while we are the ones drawing a pointer. In gamepad mode the cursor goes
				// back to the game, and taking Prisma's away as well would leave none at all.
				return config.enabled && config.useHardwareCursor &&
					   !g_gamepadMode.load(std::memory_order_relaxed);
			}
		}

		void ApplyPrismaCursorPolicy()
		{
			SetPrismaCursorSuppressed(WantPrismaCursorSuppressed());
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
				"| WM_TIMER={} WM_MOUSEMOVE={} WM_SETCURSOR={} answered={} | cursorIsOurs={}",
				g_stats.mouseMoveCalls,
				g_stats.mouseMoveEngaged,
				g_stats.thumbstickCalls,
				g_stats.wmTimer,
				g_stats.wmMouseMove,
				g_stats.wmSetCursor,
				g_stats.wmSetCursorAnswered,
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
				g_gamepadMode.load(std::memory_order_relaxed)) {
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
			// game draws its own pointer there and wants the OS one gone.
			if (g_active.load(std::memory_order_relaxed) &&
				!g_gamepadMode.load(std::memory_order_relaxed)) {
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

			const bool shouldBeActive = MenusWantCursor();
			const bool isActive = g_active.load(std::memory_order_relaxed);
			if (shouldBeActive && !isActive) {
				Activate();
			} else if (!shouldBeActive && isActive) {
				Deactivate();
			}
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
					!g_gamepadMode.load(std::memory_order_relaxed) && LOWORD(a_lparam) == HTCLIENT) {
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

			// Refill the diagnostic budget and zero the per-activation counters, so the log
			// describes the menu that just opened rather than everything before it.
			g_diagEmitted = 0;
			g_stats.ResetPerActivation();

			SKSE::log::info(
				"Activated (hardwareCursor={}, customArt={}, showCursorCount={}).",
				config.useHardwareCursor,
				g_customCursor != nullptr,
				g_lastCursorCount);
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

			// Give the game its own pointer back and get ours off the screen. Prisma's sprite
			// comes back too - with no hardware cursor there is nothing for it to double up.
			SetScaleformCursorVisible(true, false);
			ForceCursorHidden();
			ApplyPrismaCursorPolicy();
		}

		void ExitGamepadMode()
		{
			if (!g_gamepadMode.exchange(false)) {
				return;
			}

			SKSE::log::info("Mouse input resumed - taking the cursor back.");

			ApplyPrismaCursorPolicy();

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
				// this per-move re-assert is the only thing holding it down. Hanging it off
				// AbsolutePositioning meant the documented AbsolutePositioning = false fallback
				// silently gave up the suppression too, and drew both cursors.
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
		ProcessMouseMoveHook::func = vtable.write_vfunc(0x4, ProcessMouseMoveHook::thunk);
		ProcessThumbstickHook::func = vtable.write_vfunc(0x3, ProcessThumbstickHook::thunk);
		SKSE::log::info("Hooked CursorMenu::ProcessMouseMove and ProcessThumbstick.");

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
		ResolvePrismaDrawCursor();
		ApplyPrismaCursorPolicy();

		g_runtimeReady.store(true);

		// A menu may already be up when we initialise. Usually one is not: kDataLoaded fires
		// before the main menu appears, which is why the sync timer has to exist.
		SyncActiveState(true);
	}

	void Shutdown()
	{
		Deactivate();

		// Hand PrismaUI its own code back before we go, so a reloaded or unloaded plugin does
		// not leave another mod permanently patched.
		SetPrismaCursorSuppressed(false);

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
		if (g_active.load(std::memory_order_relaxed) &&
			Config::Get().useHardwareCursor &&
			Config::Get().hideGameCursor) {
			SetScaleformCursorVisible(false, false);
		}
	}
}
