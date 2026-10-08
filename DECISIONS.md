# Decisions

## ShowCursorInBackground (1.0.12)

- **On by default.** The problem it fixes (a hidden pointer left over the unfocused game
  window, so clicks pull the game back into focus) hits anyone playing borderless, and the
  change only affects the pointer while the game is out of focus. Alternative: off by
  default, opt-in like `ClipDuringGameplay`.
- **Standard Windows arrow, not the configured cursor art.** Out of focus, the game window is
  part of the desktop, and the arrow matches every other window there. Alternative: reuse
  `CursorFile` art for consistency with in-game menus.
- **Re-shown from mouse messages and the sync timer rather than by blocking hide calls.** The
  counter only matters while the pointer is over the game window, which is exactly when
  `WM_SETCURSOR`/`WM_MOUSEMOVE` arrive, and the timer covers a pointer resting there. Blocking
  `ShowCursor(FALSE)` in the import hook while unfocused would also have worked, but would mean
  the plugin overriding the game's own hide calls outside menus for the first time.
- **Hidden again on refocus only if the plugin lifted it, and only when no menu wants the
  hardware cursor.** A pointer another mod had visible before alt-tab is left alone.
