# Troubleshooting

Start with the build identity in **Details** or the EXE's file properties.
Record architecture, voice flavor, affected action, and a redacted failure
category. Do not include private text, recordings, paths, or unrelated screenshots.

## Controller is paired but not recognized

XInput and mapped DirectInput/HID controllers are supported. Windows recognizing
a HID device does not establish its button layout. Keep the bundled `controllers`
folder beside the EXE and use Automatic preference. Details shows a mapping GUID
and control counts for DirectInput devices. Unrecognized layouts stay disabled
until configured; see [Controller mappings](CONTROLLERS.md).

Release buttons and center the sticks after connecting or changing controllers.
XInput has priority in its original slots. With all four XInput slots occupied,
no DirectInput slot is available. A reader error discards old input; it does not
replay buttons after a recovery. Virtual-controller remappers can expose duplicate
physical and virtual controllers; do not run both paths together.

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

**Controller interrupted** means the controller snapshot is stale, has a gap
over 100 ms, or violates timestamp/sequence continuity. It is distinct from
**Controller disconnected**, which indicates connection or identity loss.
The reader keeps periodic device discovery off the sampling thread; do not
increase the freshness deadline or replay missed strokes to hide a real gap.

Focus changes, real physical input, controller changes, and unsafe routing dismiss
or pause typing. Reopen deliberately at the intended field. Details retains the
last dismissal category; it does not log the text.

Custom/browser controls may use one HWND for several fields. Native identity
validation cannot guarantee atomic targeting of every internal field. Applications
may ignore or reinterpret Unicode input even if Windows accepts it.

The default-off native-mode keyboard does not suppress the game's own controller
handling and may not appear over exclusive fullscreen surfaces.

The floating keyboard requests a nonactivating topmost window. Documented
[`HWND_TOPMOST`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)
ordering places it above non-topmost windows; this is not proof of visibility
above Windows Start or every system surface. For identified foreground Start and
Search windows, the app instead finds a rectangle outside the shell's reported
window region on the same monitor. This matters when the shell root is fullscreen
but has a smaller region. Without a region, the DWM frame remains the boundary.
Full size is preferred; if it cannot fit, a shell-only compact fallback reduces
the keyboard by at most 20%, never below the baseline pixel size. Ordinary-app
size and system DPI settings are unchanged. Suggestion expansion and shell
movement recheck the largest fitting size and placement.

Start may route keyboard focus to Search while that Search window has an empty
region. The app treats this as a non-drawing window and avoids the visible Start
peer instead; it does not click or focus the search bar for you. A real metadata
failure or the absence of any verifiable visible shell extent still pauses.

If there is no room within that compact limit, the app pauses with **No keyboard
space beside Start or Search within the 20% compact limit**. Unknown or invalid
shell-region geometry also pauses; View closes and deliberate reopening starts
a new attempt. There is no automatic input replay.
This cannot account for unrelated overlays or transparent areas inside a large
shell root rectangle. The app does not use privileged window bands, dismiss
Start, or move focus to force visibility. If still obscured, close it with View
before continuing; do not type blindly into the still-focused target.

Windows may independently display its touch keyboard in a search field. The
optional navigation filter does not own that behavior and cannot guarantee that
it will prevent it. The app does not close the Windows keyboard, change its
settings, or claim exclusive controller input. First-launch Start/companion
activation and late native-navigation bridge events remain unverified limits;
`--release-tap` changes click timing, not these guarantees.

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

### Optional shell-routing metadata

Start, Search, taskbar and companion surfaces can have different foreground,
focus and pointer-hit recipients. Do not infer their input source or rendering
order from a window being visible/topmost. The filter remains conservative when
the application's existing route cannot be validated.

For a deliberate diagnostic session, build the separate `overworld_shell_probe`
target. It is excluded from normal builds and CTest. Run it only when ready to
reproduce the issue yourself:

```powershell
$probe = Start-Process .\overworld_shell_probe.exe -ArgumentList '--metadata-only' `
  -NoNewWindow -PassThru
if (-not $probe.WaitForExit(20000)) {
    Stop-Process -Id $probe.Id
    throw 'The owned metadata probe exceeded its external time limit.'
}
```

The probe waits three seconds, then samples up to twenty times over ten seconds.
The external timeout above also bounds a stalled OS metadata query. It writes
only to the console: window/owner identities, process basenames, classes, cloak
state, session/integrity and query errors. It does not read titles, text, key data,
coordinates, audio or screenshots, and does not change input, focus or visibility.
Unstable or protected-desktop samples are withheld.

This output still reveals application metadata; review it before sharing.
Pair the relevant sample with the utility's build, eligibility, last missed
GAMEPAD down and gate/age, and keyboard-dismissal category. The probe does not
prove the origin of native controller input or permission to inject.

## Settings and UI tests

Save validates the full editor and detects external file changes. Reload is
explicit; no migration is written on load. Back up the file before using an older
executable that may reject a newer schema.

The Details text controls are read-only native **EDIT** controls, not static
labels. A cross-process inspection harness must use bounded `WM_GETTEXT` after
verifying the owned test PID/control identity; `GetWindowTextW` does not read
another process's edit-control contents. Wait for the explicit input-disabled
banner before treating an inspection window as ready.
