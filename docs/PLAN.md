# Scope and roadmap

The project prioritizes reliable controller-to-desktop handoff, comfortable
pointer use, short exact text entry, and optional offline dictation.

## Current scope

- XInput sampling and automatic controller acquisition after fresh neutral input.
- Pointer movement, click/drag, scrolling, custom cursor, and manual mode control.
- Immediate controller keyboard edits and opt-in one-handed selection.
- Optional original-target-safe local Whisper tiny.en dictation on x64 and ARM64.
- Best-effort game switching, explicit exclusions, and a narrow opt-in injected
  gamepad-navigation filter.
- Native tabbed settings, system-aware rendering, bounded diagnostic metadata,
  and explicit recovery.

## Maintenance priorities

1. Reproduce remaining shell/menu/caption and app-specific handoff problems without
   widening input permissions. Keep focused regressions for every confirmed fix.
2. Improve native keyboard and screen-reader navigation, small-display layout,
   and discoverability while preserving established controller mappings.
3. Maintain reproducible, architecture-verified speech dependencies and honest
   missing-model/capability feedback.
4. Keep source snapshots, release files, and troubleshooting reports free of
   personal data, caches, recordings, and unrelated repository history.

## Not implied by the current implementation

Universal game detection, suppression of all native gamepad input, touch-keyboard
control, reliable field detection inside every app, IME composition, prediction,
spatial UI snapping, extra device APIs, cloud recognition, services, drivers, and
automatic startup are not guaranteed features.

Any expansion needs an explicit product decision, testable safety boundaries,
and clear fallback behavior. Compilation or a fake test is not a claim of broad
device or application compatibility.
