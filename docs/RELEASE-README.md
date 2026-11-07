# Overworld

A native Windows controller-to-desktop utility with a floating keyboard and
optional offline push-to-talk voice. This is an early-development build, not
a replacement shell or a guarantee of compatibility with every application.

## Set up the release files

Choose a build matching your device's x64 or ARM64 architecture. Keep the
individual release files in this layout; voice-off builds omit the model and
voice licenses:

```text
Overworld.exe
LICENSE
models\
  ggml-tiny.en.bin
controllers\
  gamecontrollerdb.txt
licenses\
  SDL_GameControllerDB-LICENSE.txt
  prediction-notices.txt
  prediction-CC-BY-SA-4.0.txt
  whisper.cpp-LICENSE.txt
  voice-notices.txt
```

No installer, driver, startup entry, runtime download, or ZIP extraction is
required. Close any running Overworld or legacy Gamepad Desktop instance first.
Launch deliberately: the application can map controller input to your desktop.

Release all controls, then move a stick deliberately. Left stick moves,
A clicks, X right-clicks, right stick scrolls, and Y opens the keyboard at
the focused field. **Ctrl+Alt+F10** or tray **Pause** stops desktop control.
Menu+View switches desktop/native mode. Recovery faults require explicit recovery,
not just a mode switch.

Word suggestions are on by default; opt out in Settings > Typing and Save for
sensitive use. Native metadata cannot recognize every password/custom field.
Verify the target before typing or accepting a suggestion.

For voice-enabled builds, enable Local voice in Settings > Typing and Save.
While the keyboard is open, hold right-stick click, wait for **Listening**,
speak, and release. **View cancels**. The supplied model is verified tiny.en;
other models are rejected. There is no cloud fallback or automatic Enter.
Windows microphone permissions and the default input device are unchanged.

See the online [usage guide](https://github.com/Kevin-Taha/overworld/blob/main/docs/USAGE.md),
[troubleshooting](https://github.com/Kevin-Taha/overworld/blob/main/docs/TROUBLESHOOTING.md),
and [privacy information](https://github.com/Kevin-Taha/overworld/blob/main/docs/PRIVACY.md).
`CONTROLLERS.md` describes custom layouts.
Build identity in Details/file properties is provenance, not a code signature or
proof of real-device testing.

## License

Original application code and documentation use the supplied MIT `LICENSE`.
Third-party code, model weights, and mapping data retain their own terms.
The embedded vocabulary is CC BY-SA 4.0, including the filtered selection/order;
retain its attribution and full license. It can contain offensive language and
is not child-safe. Keep all supplied notices/licenses when redistributing.
