# Controller compatibility and mappings

Gamepad Desktop reads XInput and Windows DirectInput game controllers directly.
Bluetooth and USB devices supported by Windows can use this reader; no virtual
Xbox device, emulation driver, service, or external remapper is needed.
This does not mean every HID device has a known gamepad layout: keyboards, mice,
vendor-only interfaces, or a device requiring a special protocol are not generic
DirectInput game controllers.

Known layouts come from the bundled community SDL-format database, not
model-specific application code. The app uses native DirectInput, not SDL's video,
audio, HIDAPI, or UI runtime. Windows mappings for other SDL backends or platforms
are not interchangeable with DirectInput mappings. Firmware/driver differences
can still require a custom mapping. Hardware behavior must be checked separately.

## Normal use

Keep `controllers\gamecontrollerdb.txt` next to the EXE. Select Automatic,
release all buttons, center the controls, and then move deliberately.
The four existing app slots are shared: XInput keeps its original slot numbers
and priority; other devices occupy unused slots without merging gestures.
Standard XInput/DirectInput duplicate interfaces are filtered by the Windows
`IG_` interface marker. Third-party virtual-controller drivers can expose both a
physical device and a virtual one without that relationship; do not run both.

Mappings use physical positions: `a` bottom, `b` right, `x` left, `y` top.
Printed letters on non-Xbox controllers may differ. Guide and extra
vendor controls are not desktop actions. Trigger axes and digital triggers
are both supported. No rumble, LEDs, motion sensors, output reports, or
device calibration writes are issued.

## Add or override a mapping

1. Open Details and inspect **Recent state changes** for the HID mapping GUID
   and axis/button/hat counts. Unknown layouts remain disabled, with a mapping
   error; they never guess which button clicks or types.
2. Create `controllers\user-mappings.txt` beside the EXE using UTF-8 text.
   Put one SDL-format mapping per line, using the exact 32-digit lowercase GUID.
   Existing Windows DirectInput mappings from a mapping tool/database can be
   pasted here. Blank lines and lines beginning with `#` are allowed.
3. Exit and restart the app. Loading never rewrites either mapping file.
   Your matching entry overrides the bundled entry. Incorrect, unsupported, or
   out-of-range mappings report a reader error rather than silently falling back.
4. Verify the layout deliberately in an owned, disposable target before relying
   on it. If a control is wrong, pause/exit, correct the file, and restart.

Example for an **invented controller**, not a claim about any hardware:

```text
03000000341200007856000000000000,My controller,a:b0,b:b1,x:b2,y:b3,back:b4,start:b5,leftshoulder:b6,rightshoulder:b7,leftstick:b8,rightstick:b9,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b10,righttrigger:b11,platform:Windows,
```

Indices start at zero. DirectInput axes are ordered X, Y, Z, Rx, Ry, Rz,
Slider0, Slider1 with absent axes removed. Repeated axis types retain their logical
indices and alias the same `DIJOYSTATE2` field, matching SDL's Windows reader.
For example, X/Y/Y/Z/Rz/Rz places Z at `a3`, not `a2`. Native object offsets are
not offsets into the read buffer. Buttons and hats use their enumeration
order, not object instance numbers. The GUID identifies a layout class, not a
unique physical controller; matching devices share an override. Optional
`crc:hhhh`, or CRC bytes embedded in an exported SDL GUID, restricts a record to
SDL's product-name CRC16. Contradictory embedded/explicit CRCs are rejected.

| Binding | Meaning |
| --- | --- |
| `b0` | Button 0 |
| `a0` | Full axis 0 |
| `a0~` | Inverted full axis |
| `+a0`, `-a0` | Positive or negative half-axis |
| `h0.1`, `h0.2`, `h0.4`, `h0.8` | Hat 0 up, right, down, left |
| `+leftx:b0`, `-leftx:b1` | Separate positive/negative stick-output bindings |

A full analog trigger rests at -32768; a half-axis trigger rests at zero.
A digital trigger uses `bN` or `hN.mask` and produces 0/255. Do not assume an
axis's center or purpose from its number. A controller without sticks can use
explicit digital pointer mappings, for example
`+leftx:h0.2,-leftx:h0.8,+lefty:h0.4,-lefty:h0.1`. This is an explicit mapping
choice, not an automatic conversion of the keyboard's D-pad shortcuts.
Simultaneously active opposing output halves are an invalid sample, not neutral;
release both directions before reacquiring control.

The app needs `a` plus left X/Y controls for pointer operation. Missing other
actions stay inactive. Not all database entries are suitable for desktop use
without edits (for example, an arcade stick with only D-pad bindings).
Conditional SDL `hint:` mappings are rejected; there is no SDL runtime hint
interpreter. Mapping lines are bounded to 4 KB and each file to 2 MB. A malformed
custom file disables the DirectInput mapping path; XInput is unaffected.

## Errors and boundaries

Error 50 means missing/unsupported mapping or control layout. Error 13 means
invalid mapping data or an invalid native report. File-read errors mean the
`controllers` files are unavailable. Other HRESULTs remain visible in Details.
Discovery and reacquisition are throttled; failed reads discard all controls.
Discovery runs independently of sampling, so a slow inventory scan does not
interrupt typing from an already connected pad. At startup, error 997 means the
first inventory is still pending. Error 1460 during shutdown means an in-flight
metadata query did not stop within its bound; it cannot publish or restart.
Reconnection, source replacement, and reacquisition advance identity epochs
and require fresh neutral input. A mapping never overrides physical takeover,
original-target checks, exclusions, or pending accepted-button release recovery.

The development-only Practice harness links the same reader and normalization.
Its per-slot focus/lease checks and bounded shutdown also cover native teardown.
Deterministic mapping/lifecycle tests use fake devices only; they do not establish
Bluetooth connectivity, firmware layouts, native acquisition, or live delivery.

## Mapping-data provenance

The unmodified bundled file is from
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB/tree/5a12daa568d19344f9b6e9286ef5929833b25c7c),
revision `5a12daa568d19344f9b6e9286ef5929833b25c7c`.
Its SHA-256 is
`07ec5b753e685c4829987919b27905cc3c45c8e18c11b3046e275fcf38b0f3cb`.
Retain `licenses\SDL_GameControllerDB-LICENSE.txt` (zlib license). This grants
no application-code license. Build-time preparation verifies these bytes;
the application never downloads mappings at runtime.
