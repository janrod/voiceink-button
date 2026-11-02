# PRD: VoiceInk Button Mic only when the StopWatch can talk

Status: implemented in the dongle (2026-11-01). Measured on the real devices:

- The mic leaves the Mac 0.15 s after the StopWatch says it's going to sleep.
- The mic is back 0.8–0.9 s after the StopWatch relinks.
- The macOS UID stays the same across switches.
- With the StopWatch asleep, recording started on the Mac used the Mac's mic.
- A press 1.5 s after waking needed no hold.

Not yet tested: a press within the first second after waking (R6). Section 9 tracks a related piece
of unfinished work: a VoiceInk trigger that secure input can't block.

## 1. Problem

The dongle presents **VoiceInk Button Mic** to the Mac at all times, and VoiceInk records from it.

When recording is started on the Mac (keyboard shortcut, menu bar) while the StopWatch is asleep,
the dongle has nothing to stream. The StopWatch's radio is off in sleep, so the dongle can't reach
it. VoiceInk records a flat waveform and the transcript comes out empty. The same happens when the
StopWatch is switched off, out of range, or has a flat battery.

Waking the StopWatch remotely would fix the silence, but it does the wrong thing. If you start
recording from the Mac, you are at the Mac, and the StopWatch may be across the room. The right mic
is then the Mac's own.

## 2. Principle

VoiceInk Button Mic exists on the Mac only while the StopWatch can deliver audio. When it can't, the mic
disappears, and VoiceInk falls back to the next mic in its list.

## 3. Goals and non-goals

**Goals**
- G1. VoiceInk Button Mic is listed in macOS while the StopWatch is linked, and not listed while the
  StopWatch is asleep, off or out of range.
- G2. Recording started on the Mac while the StopWatch is unavailable records from the next mic in
  VoiceInk's list (e.g. the MacBook mic), with no flat recording.
- G3. Recording started from the StopWatch keeps its first words. A press right after waking still
  records from VoiceInk Button Mic.
- G4. No extra StopWatch power. Its sleep stays radio-off.

**Non-goals**
- Waking the StopWatch remotely.
- Streaming the dongle's own mic.
- Changes to VoiceInk.

## 4. Mac setup

VoiceInk → Audio input → **Prioritized**, in this order:

1. VoiceInk Button Mic
2. The Mac's built-in mic (or any other preferred mic)

VoiceInk 2.13 has this mode: its input modes are System Default, Custom Device and Prioritized.
Custom Device (VoiceInk Button Mic) also works: when that device is missing, VoiceInk records from
another mic. Prioritized makes the fallback order explicit.

## 5. Behaviour

| StopWatch | VoiceInk Button Mic on the Mac | Dongle screen |
|---|---|---|
| Linked: READY, DIMMED, LISTENING | present | as today |
| Sent SLEEP (going to sleep) | removed right away | ASLEEP |
| Link lost, no SLEEP (off, out of range, crashed) | removed after `MIC_LOST_GRACE_MS` (proposal: 5 s) | SEARCH… |
| LINK_REQ received (waking up) | added right away | as today once linked |

- **Never during a recording.** While the Mac has the mic open (`uac_open`), the dongle doesn't remove
  it. It waits for the close, then removes it if the StopWatch is still unavailable.
- **Linked, but awake on the desk.** Recording started on the Mac uses VoiceInk Button Mic, and the
  StopWatch follows (it toasts RECORDING and streams), as today. This only happens within ~20 s of
  touching the StopWatch, so it is likely close by.
- **Wake without a press** (pickup, touch) adds the mic too. This is harmless, and it hides the
  re-add time from the press that usually follows.

## 6. Requirements

### 6.1 Dongle USB

- R1. The dongle switches between two USB device layouts:
  - full: HID keyboard + CDC serial + UAC mic
  - reduced: HID keyboard + CDC serial
- R2. A switch is a disconnect, a descriptor swap and a reconnect (TinyUSB `tud_disconnect()` /
  `tud_connect()`).
  - The keyboard and the serial port drop for about a second.
  - That is acceptable, because the dongle only types on StopWatch commands, and those are held
    during a switch (R6).
- R3. The UAC interfaces come last in the full layout. HID and CDC then keep their interface
  numbers in both layouts, and the mic gets the same number every time it returns.
  - macOS builds the audio device UID from serial and interface number. Today the UID is
    `AppleUSBAudioEngine:VoiceInk Button:VoiceInk Button Dongle:<serial>:4` (serial = the dongle's MAC).
  - The UID must stay stable, or VoiceInk loses the device from its priority list.
- R4. Same VID/PID/serial in both layouts, so macOS doesn't show the Keyboard Setup Assistant again.
- R5. Switches are rate-limited: at most one every 2 s, with the latest wanted state winning. A
  StopWatch that wakes and sleeps repeatedly must not make the device flap.

### 6.2 Commands during a switch

- R6. A StopWatch key that arrives while the dongle is switching, or before the mic is back, is held
  until the USB is up and the mic is present. Then it is tapped. The proposal is to wait for USB
  mount plus a settle delay, measured in M1.
- R7. Audio that arrives in the meantime is buffered, as it is today while the Mac isn't listening.
  - `MAX_BACKLOG` (1.5 s) and `RING_LEN` (32768 samples, ~2 s) may have to grow to cover the re-add
    time. That is ~100 KB of PSRAM per extra 3 s. (As built: 3 s and 65536 samples.)
  - The held stop (`stop_update`) is unchanged.
- R8. A held key is tapped anyway after `KEY_HOLD_MAX_MS` (4 s). This is best effort: the
  StopWatch already counts the key as delivered, so it can't show NOT SENT.

### 6.3 State sync

- R9. `rec_state` keeps coming from `uac_open`. Once the mic has been removed, the dongle reports
  `VB_REC_UNKNOWN` until a start tap shows that VoiceInk records from it again. While the mic was
  gone, VoiceInk may have been recording from another mic.
- R10. The USB drop during a switch must not show "NO USB" or "MAC ASLEEP" on either device. The
  dongle knows it caused the drop.

### 6.4 Display

- R11. The dongle shows the switch briefly ("MIC ON" / "MIC OFF") under the state word, then returns
  to the normal screen.

## 7. Acceptance criteria

1. StopWatch asleep, VoiceInk started from the Mac keyboard: VoiceInk records from the MacBook mic
   and transcribes.
2. StopWatch picked up from sleep, yellow pressed after 1 s: the recording comes from VoiceInk Button Mic
   and keeps its first word.
3. StopWatch woken by pressing yellow (the screen-wake press), then yellow at once: as in 2, possibly
   with a short delay before VoiceInk starts.
4. StopWatch switched off: within ~6 s VoiceInk Button Mic disappears from System Settings → Sound →
   Input.
5. Recording from the StopWatch, then the StopWatch goes out of range: the recording is not cut
   short by a USB switch. The mic is removed after VoiceInk closes it.
6. Ten sleep/wake cycles: VoiceInk still lists VoiceInk Button Mic first and picks it after each wake,
   without a restart. The Keyboard Setup Assistant never appears.
7. The serial port comes back after each switch (`make monitor-dongle` reconnects).

## 8. Milestones and open questions

**M1: spike (done; the answers are in the status at the top).** Measure each question below on the
real devices.
- Can the Arduino core (m5stack:esp32 3.3.10) change its USB layout at runtime?
  - `USB.begin()` builds the configuration descriptor from the interfaces registered at start.
  - The swap may need another linker wrap (like the one used for the rename) or a lower-level
    TinyUSB descriptor callback.
- How long from `tud_connect()` until macOS lists the mic, and until VoiceInk can open it?
- Does VoiceInk's Prioritized mode follow devices that come and go without a restart, and does it
  pick the higher-priority device when it returns?
- Does the UID stay identical across switches (R3)?

**M2: presence logic** (section 5, R5–R10) and display (R11).

**M3: Mac setup.** README update and the acceptance run (section 7).

**Risks**
- If the runtime layout swap isn't possible in the Arduino core, the fallback is a full USB
  re-enumeration with a reboot of the dongle. That is slower (~2–3 s) but needs no core changes.
- If VoiceInk doesn't notice a returning device, this needs a VoiceInk fix upstream. Mac-side
  helpers are out of scope here.

## 9. Related unfinished work: a trigger that secure input can't block

**Problem.** VoiceInk catches its shortcut (⌥/) with a global keyboard listener. While any app holds
macOS secure input (Chrome with a focused password field, password managers, some terminals),
listeners get no keystrokes. ⌥/ then types ÷, from the dongle and from the real keyboard alike.
Diagnose with:

```sh
ioreg -l -w 0 | grep -o '"kCGSSessionSecureInputPID"=[0-9]*'
```

**What VoiceInk offers.** VoiceInk 2.13 exposes the App Intents *Toggle VoiceInk Recorder* and
*Dismiss VoiceInk Recorder*. It has no URL scheme and no AppleScript support. A macOS Shortcut can
run the toggle intent without any keystroke.

**Done so far.**
- [mac/make_toggle_shortcut.py](../mac/make_toggle_shortcut.py) builds and signs
  `build/VoiceInk Button Toggle.shortcut` (one action: Toggle VoiceInk Recorder).
- The file was built and opened for import. The import itself hasn't been confirmed yet.

**Option 1: dedicated key.** The dongle sends F18 instead of ⌥/, and the Shortcut is bound to F18.
- To do: import the Shortcut and bind F18 by pressing the StopWatch button once the dongle sends it.
- Then test with a focused Chrome password field. It is unknown whether Shortcuts' hotkeys survive
  secure input.

**Option 2, if F18 is blocked: serial trigger.** The dongle writes `toggle` on its USB serial port. A
login agent on the Mac runs `shortcuts run "VoiceInk Button Toggle"`.
- This works regardless of secure input.
- It costs a Mac-side helper and the start-up time of `shortcuts run` (to be measured).

Either way, the start/stop decision stays on the dongle, which checks the mic state first, so a
toggle never inverts the state.
