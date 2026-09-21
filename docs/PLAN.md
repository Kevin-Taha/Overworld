# Scope and roadmap

The product is a controller-first bridge to ordinary Windows desktop applications
on handheld or couch PCs, not a replacement shell. The project prioritizes reliable
controller-to-desktop handoff, comfortable pointer use, short exact text entry,
and optional offline dictation.

## Current scope

- XInput and mapped DirectInput/HID sampling, with automatic controller
  acquisition after fresh neutral input.
- SDL-format community mappings and file-based overrides, generic repeated-axis
  handling, and metadata discovery isolated from controller sampling.
- Pointer movement, click/drag, scrolling, custom cursor, and manual mode control.
- Default-off `--controller-view` visualization for recordings, isolated from
  input delivery with lazy native resources and no additional controller reader.
- Immediate controller keyboard edits and opt-in one-handed selection.
- Start/Search region-aware popup placement, including focused non-drawing Search
  windows and a shell-only compact fallback bounded to 20%.
- Broad local word suggestions enabled by default, with Settings opt-out and
  explicit append-only suffix acceptance. Hidden/empty suggestions collapse.
- Optional original-target-safe local Whisper tiny.en dictation on x64 and ARM64.
- Optional `--release-tap` click timing with the custom cursor; normal launch
  retains immediate A-down clicking.
- Best-effort game switching, explicit exclusions, and a narrow opt-in injected
  gamepad-navigation filter.
- Native tabbed settings, system-aware rendering, bounded diagnostic metadata,
  and explicit recovery.

## Maintenance priorities

1. Reproduce remaining Start/Phone Link first-interaction, late native-navigation
   bridge, and app-specific keyboard problems without widening input permissions.
   Keep focused regressions for every confirmed fix.
2. Preserve native Settings keyboard/screen-reader accessibility, small-display
   layout, and discoverability. This does not authorize the declined cross-app
   focus-navigation experiment.
3. Maintain reproducible, architecture-verified speech dependencies and honest
   missing-model/capability feedback.
4. Keep source snapshots, release files, and troubleshooting reports free of
   personal data, caches, recordings, and unrelated repository history.

## Not implied by the current implementation

Universal game detection, suppression of all native gamepad input, touch-keyboard
control, reliable field detection inside every app, IME composition,
spatial UI snapping, extra device APIs, cloud recognition, services, drivers, and
automatic startup are not guaranteed features.
Word suggestions are implemented, but are not universal password/caret detection,
autocorrect, a personal language model, or proof of application receipt.

Any expansion needs an explicit product decision, testable safety boundaries,
and clear fallback behavior. Compilation or a fake test is not a claim of broad
device or application compatibility.

## Experiment decisions and remaining evidence gaps

The entries below are dispositions, not an automatically scheduled work queue.

| Area | Disposition | Evidence and next boundary |
| --- | --- | --- |
| Cross-app focus navigation | **Declined.** Preserve `ccb5ff5147bf68e08c1af2ca7512cb0cbb32beda` on `kevin-taha-controller-quick-actions`; not in main and not active future work. | Do not restart, merge, or reinterpret ordinary accessibility maintenance as approval for this experiment. |
| Spoken punctuation | **Backlog only.** Preserve `69265423430a3a87e366eeaf8e9a1769f795c96b` on `kevin-taha-offline-voice-punctuation`; not in main. | Interaction with Whisper's punctuation remains unresolved. Do not fix or merge without a new product decision. |
| Aim/flick assistance and UIA helper experiments | **Parked.** Published history ends at `cfa17469bfe4f74bfe32c71375e49d0f2f6a111b` on `kevin-taha-aim-assist-and-flicks`. | Do not add to the product or resume automatically. Local-only diagnostic work is not a portable dependency or a newer published checkpoint. |
| Start/Phone Link initial interaction and late native bridge events | **Unresolved.** | Release-aware taps are opt-in, not a verified universal fix. Preserve conservative mixed-source filtering and original-target cancellation. |
| Windows touch keyboard appearing during search | **Unresolved OS interaction.** | The app does not own or suppress the Windows keyboard and must not change its settings. |
| Floating keyboard underneath Start | **Region-aware placement and bounded compact mitigation accepted for the current device checkpoint.** | User trials cover explicit Search and Start's initial search focus. Avoid verified visible Start/Search peer regions; at most 20% smaller without going below baseline pixel size. Broader shell versions, DPI configurations and compositor behavior remain unverified. No force-focus or privileged window-band workaround. |
| Teams/Explorer keyboard opening or dismissal | **App-specific validation outstanding.** | The null-hint renderer failure and visible-session mouse-probe bug have concrete fixes, but neither establishes the cause or resolution of every reported app interaction. |

Select one small investigation only after a deterministic baseline and an explicit
next product decision. Useful experiments need a real user scenario and an honest
path to owned/device validation, not only success in a fake model.
