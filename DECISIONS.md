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

## Stall watchdog (1.0.13)

- **Both releases on by default, as separate settings.** `ReleaseCursorOnStall` only undoes a
  clip, which is what focus loss does anyway. `ReleaseMouseOnStall` calls into DirectInput from
  outside the game's thread, which is the riskier half; it is on so the first test shows whether
  it helps, and it can be turned off on its own. Alternative: mouse release off until proven.
- **"Stuck" means the window procedure has gone quiet for 500 ms.** The sync timer delivers a
  message every ~15 ms while the thread pumps, so silence is unambiguous. The threshold is low
  because acting early costs nothing: the watchdog only releases what switching away would
  have released anyway. Alternative: a longer threshold (1-5 s) closer to Windows' 5 s hang
  definition.
- **Triggered by another window coming to the front, or Alt+Tab being held.** The second
  covers the case where alt-tab itself does not get anywhere while the game is stuck.
- **Unacquire runs on a throwaway thread; Acquire runs on the main thread on refocus.**
  DirectInput may need the stuck thread to finish an Unacquire, and the watchdog must not hang
  with it. Acquire on refocus guards against the game not reacquiring a device it lost.
- **The pointer stays hidden over the stuck game window.** Its display counter belongs to the
  stuck thread. Attaching the watchdog to that thread's input queue (AttachThreadInput) could
  change it, but attaching to a hung thread risks hanging the attacher, so it was left out.

## Reacquire guard (1.0.14)

- **Bundled under `ReleaseMouseOnStall` rather than a new setting.** Without it the release is
  undone within a second, so the two only make sense together. Alternative: a separate
  `BlockFocusTheftOnStall`.
- **Refused calls: the game's mouse Acquire while another window is in front, and
  SetForegroundWindow / BringWindowToTop on the game window from inside the process.** Only
  while the player is away from a stall, ending when the game window is genuinely in front
  with its thread running. Calls the player makes through the shell are never seen.
- **SetCursorPos is traced, not refused.** Many overlays import it; the probe could not say
  which module moved the pointer, so the trace names it first. dinput8.dll is the only system
  DLL whose imports are touched, because it is the prime suspect.
