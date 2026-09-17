# Troubleshooting

Start with the build identity in **Details** or the EXE's file properties.
Record architecture, voice flavor, affected action, and a redacted failure
category. Do not include private text, recordings, paths, or unrelated screenshots.

## Pointer or clicking stops

1. Release the controller and physical buttons, center both sticks, then move
   deliberately. Startup-held controls and stale input are intentionally ignored.
2. Check whether physical mouse/keyboard ownership, native mode, Auto game
   switching, an exclusion, or an unavailable desktop explains the state.
3. Use Menu+View or **Desktop control** for a deliberate desktop request.
   **Ctrl+Alt+F10** pauses; it is not a toggle.
4. For an unresolved accepted button/key, follow **Retry release** guidance.
   Cursor restoration failures use **Restore Windows pointer**. Do not
   automatically retry edits that may already have reached an app.

Start uses a narrow native-cursor fallback. That cannot force-show a cursor hidden
by another application. A visible/topmost overlay HWND or successful paint call
does not prove the overlay is actually visible above every system surface.

If the utility's minimize/maximize/close button stalls, identify whether the click
was physical or controller-generated. Own tagged caption-button handling differs
from native mouse tracking. Caption dragging, menus, other applications, and
message boxes are separate cases; do not apply a generic timeout workaround.

Automatic game detection can have false positives and false negatives. Fullscreen
pointer confinement is a hint, not permanent game identity. Use exact executable
exclusions or manual switching when needed. Do not bypass session/UIPI/UAC,
protected-desktop, physical-input, or cleanup guards.

## Keyboard closes or text does not appear

Focus changes, real physical input, controller changes, and unsafe routing dismiss
or pause typing. Reopen deliberately at the intended field. Details retains the
last dismissal category; it does not log the text.

Custom/browser controls may use one HWND for several fields. Native identity
validation cannot guarantee atomic targeting of every internal field. Applications
may ignore or reinterpret Unicode input even if Windows accepts it.

The default-off native-mode keyboard does not suppress the game's own controller
handling and may not appear over exclusive fullscreen surfaces.

## Voice unavailable

- Confirm a `voice-portable` or compatible `voice-avx2` build, not `voice-off`.
- Enable Local voice in Typing settings. Hold right-stick click while the
  keyboard is open and wait for **Listening**, not merely **Loading**.
- Use the exact released `ggml-tiny.en.bin` model; all other model contents are
  rejected before parsing. A blank override uses the default `models` directory
  beside the EXE. There is no runtime download fallback.
- Check Windows microphone permission for desktop apps and the default recording
  device. The application never changes those settings.
- Keep the controller held and target stable. Input/target/lease loss cancels the
  generation; an expired recording cannot be resumed.

Tiny.en is an English model. Recognition quality and latency vary by microphone,
noise, speaker, and device. A prerecorded-fixture pass does not establish live
microphone quality. Never use dictation output as an implicit command.

## Navigation filter

The experiment covers injected `VK_GAMEPAD` events only. It does not suppress all
controller APIs, physical input, ordinary arrows/Enter, or touch-keyboard behavior.
The first unknown hold may pass through. Release the controller before enabling
or disabling; identical mixed sources cannot always be identified.

If disabling or closing reports unresolved filter state, retain the diagnostic
category rather than forcing cleanup input. Windows can silently remove a
timed-out low-level hook, so absence of errors is not proof of continued filtering.

## Settings and UI tests

Save validates the full editor and detects external file changes. Reload is
explicit; no migration is written on load. Back up the file before using an older
executable that may reject a newer schema.

The Details text controls are read-only native **EDIT** controls, not static
labels. A cross-process inspection harness must use bounded `WM_GETTEXT` after
verifying the owned test PID/control identity; `GetWindowTextW` does not read
another process's edit-control contents. Wait for the explicit input-disabled
banner before treating an inspection window as ready.
