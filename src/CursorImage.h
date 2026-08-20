#pragma once

namespace CursorUnbound
{
	// Builds a Win32 cursor from an image on disk.
	//
	// .cur / .ani are handed to LoadImage directly (so animated cursors keep animating,
	// driven by the OS rather than by the game). Everything else goes through WIC, which
	// covers .png / .bmp / .tif / .jpg / .gif with a real alpha channel.
	//
	// Returns nullptr on failure; the caller is expected to fall back to IDC_ARROW.
	// The returned HCURSOR is owned by the caller and must be freed with DestroyCursor
	// (LoadImage results included, since we always pass LR_SHARED-free flags).
	[[nodiscard]] HCURSOR CreateCursorFromFile(
		const std::filesystem::path& a_path,
		int                          a_hotspotX,
		int                          a_hotspotY,
		float                        a_scale);

	// Picks the first usable cursor file in a_directory, preferring animated formats.
	// Search order: cursor.ani, cursor.cur, cursor.png, then any single file with a
	// supported extension.
	[[nodiscard]] std::optional<std::filesystem::path> FindCursorFile(
		const std::filesystem::path& a_directory);
}
