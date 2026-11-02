// Handheld (M5Stack StopWatch) configuration.
#pragma once
#include "vb_protocol.h"

// The dictation shortcut and the send key are in firmware/common/vb_keys.h (both devices use them).
#include "vb_keys.h"

// Blue while dictating = stop dictation, wait for VoiceInk to transcribe and paste, then Enter.
// Blue again during the wait sends Enter immediately; yellow cancels the pending Enter.
// (The dongle holds the stop key until the buffered audio reached the Mac, up to ~0.5 s.)
#define PASTE_WAIT_MS      2300

// ---- Audio -------------------------------------------------------------------------
// The mic streams to the dongle from the yellow press (VoiceInk input: "VoiceInk Button Mic").
// After the stop press it keeps streaming this long so the last syllable is not cut.
#define AUDIO_TAIL_MS      150

// ---- Buttons -----------------------------------------------------------------------
// M5Unified: BtnA = GPIO2, BtnB = GPIO1. Swap if yellow/blue come out reversed.
#define BTN_YELLOW  M5.BtnA
#define BTN_BLUE    M5.BtnB

// Where the blue button sits around the bezel, in degrees clockwise from 12 o'clock.
// Used to point an arrow at it ("press blue to restart the dongle").
#define BLUE_BUTTON_ANGLE  45   // about 1:30

// Long-press yellow re-syncs the local dictation state to "idle" without sending
// anything, for when VoiceInk was stopped from the Mac side.
#define RESYNC_HOLD_MS     900

// ---- Display / power ---------------------------------------------------------------
// Screen brightness (0-255): the levels chosen in SETTINGS, and the dimmed level.
#define BRIGHTNESS_LEVELS  {60, 110, 160, 230}
#define BRIGHTNESS_NAMES   {"Low", "Medium", "High", "Max"}
#define BRIGHTNESS_DIM     20
#define DIM_AFTER_MS       10000  // idle (not dictating) before dimming
#define SCREEN_OFF_AFTER_MS 20000 // idle before the panel turns off; touch or any button wakes it
                                  // (a press that wakes the screen does nothing else)

// Sleep: after this much idle time on battery, the radio turns off and the chip light-sleeps
// until a button, a touch or a pickup wakes it. Never sleeps on USB power.
#define SLEEP_AFTER_MS      20000  // with the screen off; on battery only. Wake + relink takes ~0.5 s
#define WAKE_LINK_WAIT_MS   2500   // a press right after waking waits this long for the link

// Wake on pickup: in sleep, the accelerometer is read every PICKUP_POLL_MS (about +0.4 mA).
// Movement of more than PICKUP_THRESHOLD_G between two readings wakes the device.
#define PICKUP_POLL_MS      200
#define PICKUP_THRESHOLD_G  0.10f

// Green power LED. Awake: a blink of AWAKE_BLINK_ON_MS every AWAKE_BLINK_MS. Asleep: a short blink
// every SLEEP_BLINK_MS (0 = stay dark); the shorter the blink, the dimmer it looks.
#define AWAKE_BLINK_MS      2000
#define AWAKE_BLINK_ON_MS   60
#define SLEEP_BLINK_MS      5000
#define SLEEP_BLINK_ON_MS   6

// Battery level, from the battery voltage (the PM1 measures nothing else, once a second). Charging
// lifts the voltage by 30-60 mV, so on USB it is read with charging paused for BATT_PAUSE_MS every
// BATT_PAUSE_EVERY_MS. The shown level only falls on battery and only rises on USB, at most this
// many percent per minute, so plugging in or out doesn't make it jump. On battery, the StopWatch's
// own load (screen, radio) pulls the voltage 25-45 mV below the battery's resting voltage; readings
// get BATT_LOAD_MV added back. Reading high costs nothing, as the level can't rise on battery.
#define BATT_SAMPLE_MS        5000
#define BATT_LOAD_MV            30
#define BATT_PAUSE_EVERY_MS  60000
#define BATT_PAUSE_MS         3000
#define BATT_DOWN_PER_MIN      1.0f
#define BATT_UP_PER_MIN        2.0f

// Defaults for SETTINGS (menu); the choices are kept in flash.
#define DEFAULT_WAKE_ON_PICKUP 1
#define DEFAULT_WAKE_ON_TOUCH  1
#define DEFAULT_BRIGHTNESS     2   // index into BRIGHTNESS_LEVELS
