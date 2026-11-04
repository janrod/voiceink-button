# VoiceInk Button

A wireless push-to-talk remote for [VoiceInk](https://github.com/Beingpax/VoiceInk), the macOS
dictation app. Press a button, speak, press again: VoiceInk transcribes and pastes the text. The
remote's mic travels with you, so you can dictate from the sofa, standing at a whiteboard, or pacing
the room.

<p>
  <img src="assets/remote-dongle-ready.jpg" width="49%" alt="The round remote and the USB dongle side by side, both showing READY in green">
  <img src="assets/remote-dongle-listening.jpg" width="49%" alt="The round remote and the USB dongle side by side, both showing LISTENING in red">
</p>

*The remote (left) and the dongle (right): ready, then listening.*

It is two M5Stack devices running this firmware:

- **Remote:** an M5Stack [**StopWatch**](https://docs.m5stack.com/en/core/StopWatch) (ESP32-S3,
  round touch screen, two buttons, mic, battery).
- **Dongle:** an M5Stack [**StickS3**](https://docs.m5stack.com/en/core/StickS3), plugged into the
  Mac. It shows up as a USB keyboard, which presses VoiceInk's shortcut, and a USB microphone,
  **VoiceInk Button Mic**, which carries the remote's audio.

The two talk over ESP-NOW, encrypted, after a one-time pairing with a 6-digit code. No Mac app or
driver is needed.

> Unofficial. VoiceInk Button is a community project and is not affiliated with or endorsed by
> VoiceInk.

## Using it

**Buttons**

| | Press | Hold |
|---|---|---|
| **Yellow** | Start / stop dictation | Reset the remote to idle without sending anything, if it ever gets out of step with VoiceInk |
| **Blue** | Enter. While dictating: stop, wait for VoiceInk to paste, then Enter. Press blue again to send at once, or yellow to cancel. | Back, in menus |

- **Wake:** a button, a touch or picking it up. A press on a dark screen only wakes it. Touch and
  pickup can be turned off under Settings; with both off, only the buttons wake it.

**The menu**

Swipe down on the main screen to open the menu. Swipe up or tap outside its buttons to close it. It
also closes when the screen turns off, or when you press a button; the press still does its usual
job, so yellow starts dictation.

| Item | What it does |
|---|---|
| **Add dongle** | Searches for nearby dongles and lists them with their signal strength, marked *new*, *paired, will replace* (pairing takes it from its current remote) or *already yours*. Tap one to pair it; see [Pair](#pair). |
| **Dongles** | Lists the dongles the remote remembers, up to 4. Tap one to switch to it. The one in use says *active, linked* or *active, not in range*. To forget a dongle, hold its row until it buzzes, then tap it within 3 s. |
| **Settings** | **Wake on pickup** and **Wake on touch** turn on or off; both start on. **Brightness** steps through Low, Medium, High and Max; it starts on High. Changes apply at once and are remembered. |
| **Reboot** | Restarts the remote. Pairings and settings are kept. |
| **Power off** | Turns the remote off; press its power button to turn it back on. On USB it can't turn off, and shows **ON USB POWER**. |

In **Add dongle**, **Dongles** and **Settings**, the highlighted row sits in the middle of the
screen:

- **By touch:** tap the highlighted row to choose it. Tap the row above or below, or drag, to move.
  Tap **Back** (**Cancel** in Add dongle) to return to the menu.
- **By button:** yellow moves up, blue moves down, holding yellow chooses, and holding blue goes
  back.

**What the screens say**

| Word | Meaning |
|---|---|
| **READY** | Linked, idle |
| **LISTENING** | Dictating: the remote's mic is streaming to the Mac |
| **DIMMED** | Idle for 10 s; any touch brings it back |
| **ASLEEP** | Remote asleep (dongle screen); radio off to save battery |
| **SEARCH…** | The dongle can't hear the remote |
| **NO USB** | The dongle lost the Mac. Press A on the dongle, or blue on the remote, to restart it |

An awake remote follows VoiceInk. If you start, stop or cancel a recording on the Mac, the remote
notices, because the dongle sees VoiceInk open or close the mic.

**Power.** On battery, the remote dims after 10 s, then turns off its screen and radio after 20 s.
The green LED blinks every 2 s while awake and every 5 s while asleep. Waking and relinking takes
about half a second. On USB the remote never sleeps; only its screen turns off. The dongle turns on when plugged in and off when
unplugged.

**When the remote is asleep,** the dongle removes VoiceInk Button Mic from the Mac. Recordings you
start on the Mac then use the Mac's own mic instead of recording silence. The mic comes back as soon
as the remote wakes.

## Set up VoiceInk

### 1. Choose how the button starts VoiceInk

The yellow button can start and stop VoiceInk in two ways. Option 1 is the tested one. Option 2 is
an experiment for people who run into the secure input problem described under
[Known caveats](#known-caveats).

#### Option 1: Press your VoiceInk shortcut

The dongle presses the same shortcut you would press on your keyboard, so it has to know yours.

1. In VoiceInk's settings, find the shortcut you use to start recording and set its mode to
   **Toggle**. The dongle taps the shortcut; it doesn't hold it down.
2. Put the same shortcut in [firmware/common/vb_keys.h](firmware/common/vb_keys.h) before you flash
   both devices. The values in the file are only the author's own, so replace them. For example,
   for Control + Option + Space:

   ```c
   #define DICTATE_MODIFIERS  (VB_MOD_LCTRL | VB_MOD_LALT)
   #define DICTATE_KEY        VB_KEY_SPACE
   #define DICTATE_LABEL      "HOTKEY"    // shown on the remote; keep it short
   ```

   Modifiers and key codes are listed in
   [firmware/common/vb_protocol.h](firmware/common/vb_protocol.h). Key codes are physical key
   positions on a US keyboard, so a key may be labelled differently on your layout. Use a shortcut
   that includes a regular key: modifier-only shortcuts and Fn haven't been tested.

#### Option 2: Run VoiceInk's action through a macOS Shortcut (experimental)

VoiceInk has a *Toggle VoiceInk Recorder* action (VoiceInk 2.13 has it), and a macOS Shortcut can
run it, so VoiceInk doesn't have to catch a hotkey. The dongle still presses a key, but only to
launch the Shortcut. **This setup hasn't been tested end to end,** and it isn't known whether it
gets past secure input.

1. Set the key in [firmware/common/vb_keys.h](firmware/common/vb_keys.h) to one nothing else uses,
   then flash both devices:

   ```c
   #define DICTATE_MODIFIERS  0
   #define DICTATE_KEY        VB_KEY_F18
   #define DICTATE_LABEL      "F18"
   ```

2. Build the Shortcut and import it:

   ```sh
   python3 mac/make_toggle_shortcut.py && open "build/VoiceInk Button Toggle.shortcut"
   ```

3. In the Shortcuts app, add a keyboard shortcut to **VoiceInk Button Toggle**. When it asks for the
   keys, press the yellow button.

A version with no keystroke at all is designed but not built: the dongle would signal a helper on
the Mac over USB serial, and the helper would run the Shortcut. See
[docs/PRD-mic-presence.md](docs/PRD-mic-presence.md), section 9.

### 2. Choose the microphone

In VoiceInk's audio input settings, choose **Custom Device** → **VoiceInk Button Mic**. Or choose
**Prioritized** with VoiceInk Button Mic first and your Mac's mic second. Either way, VoiceInk falls
back to another mic while the remote is asleep. The remote has to be awake for VoiceInk Button Mic
to appear in the list.

### 3. Dismiss the Keyboard Setup Assistant

The first time the dongle is plugged in, macOS may ask you to identify the keyboard. Close the
window; nothing needs to be set up.

### Known caveats

**Secure input blocks shortcuts.** While an app holds macOS *secure input*, no app can catch global
shortcuts, so VoiceInk doesn't start. The same happens with your real keyboard. The usual causes are
a focused password field in a browser, a password manager, or some terminals. A shortcut that uses
Option may type a special character into the focused app instead. To find which app holds secure
input:

```sh
ioreg -l -w 0 | grep -o '"kCGSSessionSecureInputPID"=[0-9]*'   # then: ps -p <pid>
```

Click out of the password field, or quit the app, and the button works again.

**A sleeping remote can't follow the Mac.** Its radio is off, so a recording you start on the Mac
doesn't wake it. VoiceInk records from another mic instead, because VoiceInk Button Mic is only
there while the remote is awake.

**Other apps can trigger the remote.** The dongle can't tell who opened VoiceInk Button Mic. If
another app opens it, such as a video call, the remote shows LISTENING and sends its audio, and its
buttons can get out of step with VoiceInk until that app lets go. Hold yellow to reset the remote.

## Build and flash

You need macOS, Python 3, and these tools:

```sh
brew install arduino-cli esptool
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index && arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified M5GFX
```

Built and tested with the m5stack:esp32 core 3.3.10.

**Name your boards.** Flashing only touches the two boards you list, so another ESP32 on your desk
can't be overwritten by accident. Plug both in, then:

```sh
make boards                  # lists connected ESP32-S3 boards: MAC, name, port
cp boards.example.mk boards.mk
# edit boards.mk: HANDHELD_MAC = the StopWatch, DONGLE_MAC = the StickS3
```

**Flash:**

```sh
make flash-handheld          # StopWatch
make flash-dongle            # StickS3 that already runs this firmware
make monitor-handheld        # serial log
make monitor-dongle
```

**First flash of a StickS3,** or after a crash: hold its side button for about 2 s until the green
LED blinks (download mode), then run `make first-flash-dongle`. A StopWatch goes into download mode
the same way, with its power button.

Flashing keeps pairing and settings.

## Pair

On the remote, swipe down → **Add dongle** and pick the dongle (`VB-xxxx`, from the last digits of
its MAC). Check that both screens show the same 6-digit code, then accept with yellow on the remote
and **A** on the dongle.

- The remote remembers up to 4 dongles; switch between them under **Dongles**.
- To pair a dongle that is already paired with another remote, hold **A** on it for 3 s to put it
  back into pairing mode. Holding **B** for 6 s makes it forget its remote.

## Tuning and diagnostics

Timings and the battery settings are in [firmware/handheld/config.h](firmware/handheld/config.h);
the shortcut and the send key are in [firmware/common/vb_keys.h](firmware/common/vb_keys.h).

The serial port takes single-letter commands:

| Key | Board | Prints |
|---|---|---|
| `d` | both | boot history |
| `p` | remote | pickup (wake) log |
| `a` | remote | ten raw accelerometer readings |
| `b` | remote | battery log, the last ~2 h |
| `v` | dongle | power source, mic and link state |

**Battery level.** The remote can only measure its battery's voltage, and charging pushes that up.
So on USB it pauses charging for 3 s once a minute to take its reading. The shown level only falls
on battery and only rises on USB, so plugging in or unplugging doesn't make it jump.

## Security

- Pairing uses X25519 with a commitment and a 6-digit code you compare on both screens. After that,
  every packet is encrypted by ESP-NOW and carries an HMAC tag with a per-session key. The fixed
  ESP-NOW primary key in the source only wraps the per-pair key on the chip; it isn't a secret.
- The dongle types only its own two keys, the dictation shortcut and Enter
  ([vb_keys.h](firmware/common/vb_keys.h)). The remote says which one to press, never what to
  type, so even a lost remote can't type anything else.
- A replayed link packet can't start or reset a session. The dongle switches to a new session only
  once the remote uses it.
- The pairing keys are stored on the remote without flash encryption. If you lose the remote, hold
  **B** on the dongle for 6 s to make it forget it.

## License

Copyright (C) 2026 Jan Rod. VoiceInk Button is free software under the
[GNU General Public License v3.0](LICENSE), the same license as VoiceInk.
