// VoiceInk Button handheld — M5Stack StopWatch (ESP32-S3, 466x466 round AMOLED)
//
// Yellow: toggle dictation (taps the VoiceInk shortcut, see config.h). Long-press: resync to idle.
// Blue:   Enter (while dictating: stop, wait for the paste, then Enter).
// Swipe down: menu (add dongle, dongles, reboot, power off).
// Talks to a paired StickS3 dongle over ESP-NOW (link.ino); the dongle types the keys over USB HID.
//
// Files: app.h (shared state), handheld.ino (main screen + input), link.ino (radio, pairing),
// screens.ino (add-dongle and dongles screens).

#include "app.h"

// ---------------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------------
static uint32_t dictate_start_ms = 0;
static bool dimmed = false;
static bool screen_off = false;

// "stop, wait for paste, then Enter"
static bool enter_after_stop = false;   // set when blue stops dictation; consumed on delivery
static uint32_t enter_due_ms = 0;       // 0 = no Enter pending

// a button press on a dark screen only wakes it; the rest of that press is ignored
static bool swallow_yellow = false, swallow_blue = false;

// sleep
static char deferred_btn = 0;           // 'y' / 'b': pressed while the link was still coming back
static uint32_t link_wait_until = 0;
static bool imu_ok = false;

static bool menu_dragging = false;
static bool touch_ignored = false;      // touch that only woke the screen
static int off_warning_s = 0;           // counting down to power off: seconds left; 0 = not counting

static int battery = -1;
static bool charging = false;
static bool usb_power = false;          // VBUS present: never sleep (and keeps the serial port up)

static float mic_level = 0.f;           // 0..1, smoothed
static std::atomic<int> mic_level_target{0};   // 0..1000, set by the audio task
static bool mic_on = false;
static uint32_t mic_stop_at = 0;        // after a stop press: mic off at this time (audio tail)

static uint32_t enter_flash_until = 0;
static uint32_t last_key_ms = 0;        // when the last key was delivered (or gave up)

// ---------------------------------------------------------------------------------
// Haptics: non-blocking pattern of alternating on/off durations (ms)
// ---------------------------------------------------------------------------------
static const uint16_t* vib_pat = nullptr;
static uint8_t vib_step = 0, vib_len = 0;
static uint32_t vib_next_ms = 0;
static volatile uint32_t vib_quiet_at = 0;   // the mic hears the motor: audio is muted until then

static const uint16_t VIB_START[] = {70};
static const uint16_t VIB_STOP[]  = {35, 70, 35};
static const uint16_t VIB_ENTER[] = {25};
static const uint16_t VIB_ERROR[] = {250};
static const uint16_t VIB_RESYNC[] = {20, 60, 20, 60, 20};

static void vibrate(const uint16_t* pat, uint8_t len) {
  vib_pat = pat; vib_len = len; vib_step = 0;
  M5.Power.setVibration(220);
  vib_next_ms = millis() + pat[0];
}
#define VIBRATE(p) vibrate(p, sizeof(p) / sizeof(p[0]))

static void vib_update() {
  if (!vib_pat || (int32_t)(millis() - vib_next_ms) < 0) return;
  if (++vib_step >= vib_len) {
    M5.Power.setVibration(0);
    vib_pat = nullptr;
    vib_quiet_at = millis() + 60;       // frames are processed 20-40 ms after capture
    return;
  }
  M5.Power.setVibration((vib_step & 1) ? 0 : 220);
  vib_next_ms = millis() + vib_pat[vib_step];
}

static void toast(const char* text, uint16_t color, uint32_t ms = 1200) {
  toast_text = text; toast_color = color; toast_until = millis() + ms;
}

// ---------------------------------------------------------------------------------
// Mic: streams to the dongle while dictating (the dongle is the Mac's USB mic) and drives the
// level animation. Two 20 ms buffers alternate in the capture task; each finished one is
// copied to a queue and sent from audio_task, so rendering never delays the audio.
// ---------------------------------------------------------------------------------
static int16_t mic_buf[2][VB_AUDIO_FRAME];
static QueueHandle_t frame_queue;
static std::atomic<bool> stream_first{false};

static void rec_released(void*, void* data, size_t samples) {
  xQueueSend(frame_queue, data, 0);
  M5.Mic.record((int16_t*)data, samples);
}

static void audio_task(void*) {
  static vb_audio_t a;
  uint32_t seq = 0;
  uint8_t stream = 0;
  for (;;) {
    if (xQueueReceive(frame_queue, a.pcm, portMAX_DELAY) != pdTRUE) continue;
    // level: remove DC, RMS -> dBFS -> 0..1 over a -60..-10 dB window
    int32_t mean = 0;
    for (int i = 0; i < VB_AUDIO_FRAME; i++) mean += a.pcm[i];
    mean /= VB_AUDIO_FRAME;
    float acc = 0;
    for (int i = 0; i < VB_AUDIO_FRAME; i++) { float s = a.pcm[i] - mean; acc += s * s; }
    float db = 20.f * log10f(sqrtf(acc / VB_AUDIO_FRAME) / 32768.f + 1e-6f);
    mic_level_target = (int)(constrain((db + 60.f) / 50.f, 0.f, 1.f) * 1000);
    if (vib_pat || (int32_t)(vib_quiet_at - millis()) > 0) memset(a.pcm, 0, sizeof(a.pcm));

    a.flags = stream_first.exchange(false) ? VB_AUDIO_START : 0;
    if (a.flags & VB_AUDIO_START) { seq = 0; stream++; }
    a.seq = ++seq;
    a.stream = stream;
    send_audio(&a);
  }
}

static void mic_start() {
  mic_stop_at = 0;
  if (mic_on) return;
  M5.Speaker.end();
  M5.Mic.begin();
  xQueueReset(frame_queue);
  stream_first = true;
  M5.Mic.record(mic_buf[0], VB_AUDIO_FRAME, VB_AUDIO_RATE);
  M5.Mic.record(mic_buf[1], VB_AUDIO_FRAME, VB_AUDIO_RATE);
  audio_sent = audio_busy = audio_failed = 0;
  mic_on = true;
}

static void mic_stop() {
  mic_stop_at = 0;
  if (!mic_on) return;
  M5.Mic.end();
  mic_on = false;
  mic_level_target = 0;
  Serial.printf("audio: %lu frames sent, %lu dropped (radio busy), %lu not acked\n",
                (unsigned long)audio_sent, (unsigned long)audio_busy, (unsigned long)audio_failed);
}

static void mic_update() {
  if (mic_stop_at && (int32_t)(millis() - mic_stop_at) >= 0) mic_stop();
  float target = mic_on ? mic_level_target / 1000.f : 0.f;
  mic_level += (target - mic_level) * (target > mic_level ? 0.6f : 0.12f);
}

// ---------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------
// Returns true if the screen was off or dimmed (the input only woke it).
static bool wake() {
  last_activity_ms = millis();
  bool was_asleep = screen_off || dimmed;
  if (screen_off) { M5.Display.wakeup(); screen_off = false; }
  if (dimmed || was_asleep) { M5.Display.setBrightness(brightness_active()); dimmed = false; }
  return was_asleep;
}

// called by send_update() (link.ino) once a key was delivered or gave up
static void on_key_result(bool ok, uint8_t key_id) {
  last_key_ms = millis();
  if (!ok) {
    if (key_id == VB_KEYID_DICTATE) mic_stop();
    enter_after_stop = false;
    VIBRATE(VIB_ERROR);
    toast("NOT SENT", C_RED);
    Serial.println("key: delivery failed");
    return;
  }
  if (key_id == VB_KEYID_DICTATE) {
    dictating = true;
    dictate_start_ms = millis();
    VIBRATE(VIB_START);
    Serial.println("dictation ON");
  } else if (key_id == VB_KEYID_DICTATE_STOP) {
    dictating = false;
    mic_stop_at = millis() + AUDIO_TAIL_MS;
    VIBRATE(VIB_STOP);
    if (enter_after_stop) enter_due_ms = millis() + PASTE_WAIT_MS;
    enter_after_stop = false;
    Serial.println("dictation OFF");
  } else {
    enter_flash_until = millis() + 650;
    VIBRATE(VIB_ENTER);
    Serial.println("enter sent");
  }
}

// Linked over radio, but the dongle has no USB connection to the Mac.
static bool dongle_no_usb() { return linked() && dongle_usb_state == VB_USB_NONE; }

// Paired, but the active dongle can't be seen. Not while unpaired, or while adding a dongle: there,
// no link is expected.
static bool dongle_missing() { return store.count && screen != SCR_DISCOVER && !linked(); }

static void not_linked_feedback() {
  VIBRATE(VIB_ERROR);
  toast(store.count ? "NO LINK" : "NO DONGLE", C_AMBER);
}

static void send_enter_now() {
  enter_due_ms = 0;
  if (!send_key(VB_KEYID_ENTER, SEND_MODIFIERS, SEND_KEY)) not_linked_feedback();
}

static void press_yellow() {
  if (enter_due_ms) {
    enter_due_ms = 0;
    VIBRATE(VIB_ENTER);
    toast("CANCELLED", C_MUTED);
  } else if (dictating) {
    if (!send_key(VB_KEYID_DICTATE_STOP, DICTATE_MODIFIERS, DICTATE_KEY)) not_linked_feedback();
  } else if (send_key(VB_KEYID_DICTATE, DICTATE_MODIFIERS, DICTATE_KEY)) {
    mic_start();                      // stream from the press; the dongle buffers until the Mac listens
  } else {
    not_linked_feedback();
  }
}

static void press_blue() {
  if (dongle_no_usb()) {
    send_restart();                   // nothing can be typed anyway; blue = restart dongle
    VIBRATE(VIB_ENTER);
    toast("RESTARTING", C_BLUE, 4000);
  } else if (enter_due_ms) {
    send_enter_now();                 // skip the rest of the wait
  } else if (dictating) {
    if (send_key(VB_KEYID_DICTATE_STOP, DICTATE_MODIFIERS, DICTATE_KEY)) enter_after_stop = true;
    else not_linked_feedback();
  } else {
    send_enter_now();
  }
}

// Right after waking from sleep the link needs a moment; a press in that window waits for it.
static bool link_coming_up() { return !linked() && (int32_t)(link_wait_until - millis()) > 0; }

static void deferred_update() {
  if (!deferred_btn) return;
  if (linked()) {
    char b = deferred_btn;
    deferred_btn = 0;
    if (b == 'y') press_yellow(); else press_blue();
  } else if (!link_coming_up()) {
    deferred_btn = 0;
    not_linked_feedback();
  }
}

static void buttons_update() {
  if ((screen_off || off_warning_s) && (BTN_YELLOW.wasPressed() || BTN_BLUE.wasPressed())) {
    swallow_yellow |= BTN_YELLOW.wasPressed();
    swallow_blue |= BTN_BLUE.wasPressed();
    wake();
  }
  bool yellow_hold = BTN_YELLOW.wasHold(), yellow = BTN_YELLOW.wasClicked();
  bool blue_hold = BTN_BLUE.wasHold(), blue_click = BTN_BLUE.wasClicked(), blue = BTN_BLUE.wasPressed();
  if (swallow_yellow) { swallow_yellow = !BTN_YELLOW.wasReleased(); yellow_hold = yellow = false; }
  if (swallow_blue)   { swallow_blue = !BTN_BLUE.wasReleased(); blue_hold = blue_click = blue = false; }
  if (yellow_hold || yellow || blue || blue_hold) { wake(); menu_target = 0.f; }

  if (screen != SCR_MAIN) { screen_buttons(yellow, yellow_hold, blue_click, blue_hold); return; }

  if (yellow_hold) {
    if (dictating) { dictating = false; mic_stop(); }
    enter_due_ms = 0;
    deferred_btn = 0;
    VIBRATE(VIB_RESYNC);
    toast("RESYNCED", C_MUTED);
    Serial.println("resync -> idle");
  } else if (yellow) {
    Serial.println("yellow");
    if (link_coming_up()) deferred_btn = 'y'; else press_yellow();
  }
  if (blue) {
    Serial.println("blue");
    if (link_coming_up()) deferred_btn = 'b'; else press_blue();
  }
}

// The dongle reports whether VoiceInk is recording (it has the USB mic open). Follow it once a
// mismatch outlasts the key that just went out: VoiceInk cancelled, ignored a tap, or was
// started/stopped on the Mac.
static void voiceink_sync() {
  static uint32_t mismatch_since = 0;
  uint32_t now = millis();
  bool known = linked() && voiceink_state != VB_REC_UNKNOWN;
  bool rec = voiceink_state == VB_REC_ON;
  if (!known || rec == dictating || send_state != SEND_IDLE || deferred_btn || now - last_key_ms < 2500) {
    mismatch_since = 0;
    return;
  }
  if (!mismatch_since) { mismatch_since = now; return; }
  if (now - mismatch_since < 600) return;
  mismatch_since = 0;
  dictating = rec;
  if (rec) { dictate_start_ms = now; mic_start(); VIBRATE(VIB_START); toast("RECORDING", C_MUTED); }
  else     { mic_stop(); enter_after_stop = false; VIBRATE(VIB_STOP); toast("STOPPED", C_MUTED); }
  Serial.printf("voiceink: %s, following it\n", rec ? "recording" : "stopped");
}

static void enter_update() {
  if (enter_due_ms && (int32_t)(millis() - enter_due_ms) >= 0 && send_state == SEND_IDLE) {
    send_enter_now();
  }
}

// ---------------------------------------------------------------------------------
// Touch: wake the screen, swipe down for the menu
// ---------------------------------------------------------------------------------
static constexpr int MENU_ITEMS = 5;
static constexpr int MENU_BTN_W = 250, MENU_BTN_H = 60, MENU_PITCH = 74;
static constexpr int TAP_SLOP_PX = 20;  // finger roll allowed before a touch becomes a drag (M5Unified default: 8)
static int menu_btn_y(int i) { return CY - 148 + i * MENU_PITCH; }   // button centers when open

// Button under a tap, or -1. The gaps between buttons go to the nearest one.
static int menu_hit(int x, int y) {
  if (abs(x - CX) > MENU_BTN_W / 2 + 20) return -1;
  int i = (int)lroundf((y - menu_btn_y(0)) / (float)MENU_PITCH);
  return i >= 0 && i < MENU_ITEMS && abs(y - menu_btn_y(i)) <= MENU_PITCH / 2 ? i : -1;
}

static void do_reboot();
static void do_power_off();

static void menu_select(int i) {
  switch (i) {
    case 0: open_discover(); break;
    case 1: open_dongles(); break;
    case 2: open_settings(); break;
    case 3: do_reboot(); break;
    case 4: do_power_off(); break;
  }
}

static void touch_update() {
  press_x = press_y = -1;
  if (!M5.Touch.getCount()) {
    if (menu_dragging) { menu_dragging = false; menu_target = menu_pos > 0.5f ? 1.f : 0.f; }
    if (list_dragging) { list_dragging = false; list_drag = 0; }
    return;
  }
  const auto& t = M5.Touch.getDetail(0);

  if (t.wasPressed() && off_warning_s) { wake(); touch_ignored = true; }   // stay on; it does nothing else
  else if (t.wasPressed()) touch_ignored = (dimmed || screen_off) && !settings.wake_on_touch ? true : wake();
  if (touch_ignored) { if (!t.isPressed()) touch_ignored = false; return; }
  last_activity_ms = millis();
  if (t.isPressed() && !t.isDragging() && !t.isFlicking() && !menu_dragging && !list_dragging) {
    press_x = t.x;
    press_y = t.y;
  }

  if (screen != SCR_MAIN) { screen_touch(t); return; }

  bool menu_open = menu_target > 0.5f;
  if (t.isDragging() || t.isFlicking()) {
    int dy = t.distanceY();
    if (!menu_open && dy > 0)       { menu_dragging = true; menu_pos = constrain(dy / (float)CY, 0.f, 1.f); }
    else if (menu_open && dy < 0)   { menu_dragging = true; menu_pos = constrain(1.f + dy / (float)CY, 0.f, 1.f); }
    return;
  }
  if (menu_dragging && !t.isPressed()) {
    menu_dragging = false;
    float threshold = menu_open ? 0.7f : 0.3f;
    menu_target = menu_pos > threshold ? 1.f : 0.f;
    return;
  }
  // A slow tap ends as hold_end, not touch_end; both count. Hit-test where the finger landed.
  bool tap = t.state == m5::touch_state_t::touch_end || t.state == m5::touch_state_t::hold_end;
  if (tap && menu_open) {
    int i = menu_hit(t.base_x, t.base_y);
    if (i >= 0) menu_select(i);
    else menu_target = 0.f;             // tap outside the buttons closes the menu
  }
}

static void menu_update() {
  if (menu_dragging) return;
  menu_pos += (menu_target - menu_pos) * 0.3f;
  if (fabsf(menu_target - menu_pos) < 0.01f) menu_pos = menu_target;
}

// ---------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------
static uint16_t scale(uint16_t c, float k) {
  int r = ((c >> 11) & 0x1F) * k, g = ((c >> 5) & 0x3F) * k, b = (c & 0x1F) * k;
  return (r << 11) | (g << 5) | b;
}

// Pressed state of a touch target: its fill, a quarter of the way to white.
static uint16_t lighten(uint16_t c, float k = 0.25f) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r += (0x1F - r) * k; g += (0x3F - g) * k; b += (0x1F - b) * k;
  return (r << 11) | (g << 5) | b;
}
static bool pressed_in_rect(int cx, int cy, int w, int h) {
  return press_x >= 0 && abs(press_x - cx) < w / 2 && abs(press_y - cy) < h / 2;
}
static bool pressed_in_circle(int cx, int cy, int r) {
  return press_x >= 0 && (press_x - cx) * (press_x - cx) + (press_y - cy) * (press_y - cy) <= r * r;
}

static void draw_mic(int x, int y, uint16_t col) {
  canvas.fillSmoothRoundRect(x - 21, y - 52, 42, 70, 21, col);       // capsule
  canvas.fillArc(x, y - 4, 39, 33, 0, 180, col);                     // cradle
  canvas.fillRect(x - 39, y - 14, 6, 10, col);
  canvas.fillRect(x + 33, y - 14, 6, 10, col);
  canvas.fillRect(x - 3, y + 34, 6, 16, col);                        // stem
  canvas.fillSmoothRoundRect(x - 20, y + 48, 40, 6, 3, col);         // base
}

static void draw_return_arrow(int x, int y, uint16_t col) {
  canvas.fillRect(x + 22, y - 34, 8, 42, col);                       // down
  canvas.fillRect(x - 24, y + 1, 54, 8, col);                        // left
  canvas.fillTriangle(x - 42, y + 5, x - 18, y - 17, x - 18, y + 27, col);
}

// Pulsing arrow at the bezel, pointing at the blue button.
static void draw_blue_arrow(uint32_t now) {
  float a = (BLUE_BUTTON_ANGLE - 90) * DEG_TO_RAD;
  float pulse = 0.5f + 0.5f * sinf(now / 180.f);
  int r_tip = 212 + (int)(6 * pulse), r_base = r_tip - 26;
  float ux = cosf(a), uy = sinf(a), px = -uy, py = ux;
  int tx = CX + ux * r_tip, ty = CY + uy * r_tip;
  int bx = CX + ux * r_base, by = CY + uy * r_base;
  canvas.fillTriangle(tx, ty, bx + px * 16, by + py * 16, bx - px * 16, by - py * 16,
                      scale(C_BLUE, 0.55f + 0.45f * pulse));
}

static void draw_battery(int x, int y) {
  // x,y = center of the whole widget (icon + text)
  const int bw = 40, bh = 20;
  int bx = x - 46, by = y - bh / 2;
  uint16_t col = battery < 0 ? C_DIM : battery <= 15 ? C_RED : battery <= 30 ? C_AMBER : C_GREEN;
  if (charging) col = C_GREEN;
  canvas.drawRoundRect(bx, by, bw, bh, 5, C_MUTED);
  canvas.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 4, C_MUTED);
  canvas.fillRoundRect(bx + bw + 1, y - 4, 3, 8, 1, C_MUTED);
  if (battery > 0) canvas.fillRoundRect(bx + 4, by + 4, max(2, (bw - 8) * battery / 100), bh - 8, 2, col);
  if (charging) {
    int cx = bx + bw / 2;
    canvas.fillTriangle(cx + 3, by + 1, cx - 6, y + 2, cx + 1, y + 2, C_BG);
    canvas.fillTriangle(cx - 3, by + bh - 1, cx + 6, y - 2, cx - 1, y - 2, C_BG);
    canvas.fillTriangle(cx + 2, by + 3, cx - 4, y + 1, cx + 1, y + 1, C_TEXT);
    canvas.fillTriangle(cx - 2, by + bh - 3, cx + 4, y - 1, cx - 1, y - 1, C_TEXT);
  }
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT);
  char buf[8];
  if (battery < 0) strcpy(buf, "--"); else snprintf(buf, sizeof(buf), "%d%%", battery);
  canvas.drawString(buf, bx + bw + 10, y + 1);
}

static void draw_ring(uint32_t now, bool is_linked) {
  const int r_out = 229, r_in = 221;
  if (dictating) {
    float pulse = 0.35f + 0.65f * mic_level;
    canvas.fillArc(CX, CY, r_out, r_in, 0, 360, scale(C_RED, pulse));
  } else if (is_linked) {
    canvas.fillArc(CX, CY, r_out, r_in + 4, 0, 360, scale(C_GREEN, 0.28f));
  } else {
    // searching: rotating comet
    canvas.fillArc(CX, CY, r_out, r_in, 0, 360, RGB(24, 18, 0));
    float head = fmodf(now * 0.30f, 360.f);
    for (int k = 0; k < 14; k++) {
      float a1 = head - k * 6.f, a0 = a1 - 6.5f;
      canvas.fillArc(CX, CY, r_out, r_in, a0, a1, scale(C_AMBER, 1.f - k / 14.f));
    }
  }
}

static void draw_center(uint32_t now, bool is_linked) {
  bool enter_flash = (int32_t)(enter_flash_until - now) > 0;
  if (enter_flash) {
    float k = (enter_flash_until - now) / 650.f;
    canvas.fillSmoothCircle(CX, MIC_Y, DISC_R + (int)(10 * k), C_BLUE);
    draw_return_arrow(CX + 4, MIC_Y, C_TEXT);
    return;
  }
  if (enter_due_ms) {
    // waiting for VoiceInk to paste: blue arc fills up until Enter fires
    float p = 1.f - constrain((int32_t)(enter_due_ms - now) / (float)PASTE_WAIT_MS, 0.f, 1.f);
    canvas.fillSmoothCircle(CX, MIC_Y, DISC_R, C_DISC);
    canvas.fillArc(CX, MIC_Y, DISC_R + 12, DISC_R + 5, 0, 360, scale(C_BLUE, 0.2f));
    if (p > 0.f) canvas.fillArc(CX, MIC_Y, DISC_R + 12, DISC_R + 5, -90, -90 + 360 * p, C_BLUE);
    draw_return_arrow(CX + 4, MIC_Y, C_BLUE);
    return;
  }
  if (dictating) {
    // expanding ripples, amplitude driven by mic level
    float amp = 0.35f + mic_level;
    for (int i = 0; i < 3; i++) {
      float p = fmodf(now / 1400.f + i / 3.f, 1.f);
      int r = DISC_R + 6 + (int)(p * 62 * amp);
      uint16_t c = scale(C_RED, (1.f - p) * (0.25f + 0.6f * mic_level));
      canvas.fillArc(CX, MIC_Y, r + 3, r, 0, 360, c);
    }
    int r = DISC_R + (int)(mic_level * 8);
    canvas.fillSmoothCircle(CX, MIC_Y, r, C_RED);
    draw_mic(CX, MIC_Y, C_TEXT);
  } else {
    canvas.fillSmoothCircle(CX, MIC_Y, DISC_R, C_DISC);
    draw_mic(CX, MIC_Y, is_linked ? C_TEXT : C_DIM);
  }
}

static void draw_labels(uint32_t now, bool is_linked) {
  canvas.setTextDatum(middle_center);

  // primary state line
  const char* state; uint16_t col;
  if (enter_due_ms)      { state = "SENDING";    col = C_BLUE; }
  else if (dictating)    { state = "LISTENING";  col = C_RED; }
  else if (dongle_no_usb()) { state = "NO USB";  col = C_AMBER; }
  else if (is_linked && dimmed) { state = "DIMMED"; col = C_MUTED; }   // same words as the dongle
  else if (is_linked)    { state = "READY";      col = C_TEXT; }
  else if (!store.count) { state = "NOT PAIRED"; col = C_AMBER; }
  else                   { state = "SEARCHING";  col = C_AMBER; }
  bool toast_on = toast_text && (int32_t)(toast_until - now) > 0;
  if (toast_on) { state = toast_text; col = toast_color; }
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(col);
  canvas.drawString(state, CX, 330);

  // secondary line: timer while dictating, otherwise button hints
  canvas.setFont(&fonts::FreeSans12pt7b);
  if (enter_due_ms) {
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.setTextDatum(middle_left);
    canvas.fillSmoothCircle(CX - 70, 372, 6, C_YELLOW);
    canvas.drawString("CANCEL", CX - 58, 373);
    canvas.setTextDatum(middle_center);
  } else if (dongle_no_usb()) {
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.setTextDatum(middle_left);
    canvas.fillSmoothCircle(CX - 88, 372, 6, C_BLUE);
    canvas.drawString("RESTART DONGLE", CX - 76, 373);
    canvas.setTextDatum(middle_center);
    draw_blue_arrow(now);
  } else if (dictating) {
    uint32_t s = (now - dictate_start_ms) / 1000;
    char buf[12]; snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    canvas.setTextColor(C_TEXT);
    canvas.drawString(buf, CX, 372);
  } else {
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.setTextDatum(middle_left);
    canvas.fillSmoothCircle(CX - 112, 372, 6, C_YELLOW);
    canvas.drawString(DICTATE_LABEL, CX - 100, 373);
    canvas.fillSmoothCircle(CX + 22, 372, 6, C_BLUE);
    canvas.drawString("ENTER", CX + 34, 373);
    canvas.setTextDatum(middle_center);
  }

  // link line
  char id[8]; active_id(id);
  char link[32]; uint16_t lc;
  if (!store.count)                  { strcpy(link, "SWIPE DOWN TO ADD DONGLE"); lc = C_AMBER; }
  else if (is_linked && dongle_usb_state == VB_USB_READY)     { snprintf(link, sizeof(link), "LINKED  %s", id); lc = C_GREEN; }
  else if (is_linked && dongle_usb_state == VB_USB_SUSPENDED) { snprintf(link, sizeof(link), "%s  MAC ASLEEP", id); lc = C_MUTED; }
  else if (is_linked)                { snprintf(link, sizeof(link), "%s  NO USB", id); lc = C_AMBER; }
  else                               { snprintf(link, sizeof(link), "LOOKING FOR %s", id); lc = C_AMBER; }
  canvas.setFont(&fonts::FreeSans9pt7b);
  int tw = canvas.textWidth(link);
  bool blink = !is_linked && ((now / 500) & 1);
  canvas.fillSmoothCircle(CX - tw / 2 - 12, 412, 5, blink ? C_BG : lc);
  canvas.setTextColor(lc);
  canvas.setTextDatum(middle_left);
  canvas.drawString(link, CX - tw / 2, 413);
}

static void draw_menu() {
  if (menu_pos <= 0.f) return;
  int oy = (int)((menu_pos - 1.f) * canvas.height());   // panel slides down from the top
  canvas.fillRect(0, oy, canvas.width(), canvas.height(), RGB(10, 10, 12));

  static const char* labels[MENU_ITEMS] = {"ADD DONGLE", "DONGLES", "SETTINGS", "REBOOT", "POWER OFF"};
  const uint16_t fills[MENU_ITEMS] = {RGB(10, 70, 140), RGB(44, 44, 46), RGB(44, 44, 46), RGB(44, 44, 46),
                                      RGB(120, 24, 18)};
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextColor(C_TEXT);
  int pressed = menu_pos >= 1.f && press_x >= 0 ? menu_hit(press_x, press_y) : -1;
  for (int i = 0; i < MENU_ITEMS; i++) {
    int y = oy + menu_btn_y(i);
    canvas.fillSmoothRoundRect(CX - MENU_BTN_W / 2, y - MENU_BTN_H / 2, MENU_BTN_W, MENU_BTN_H,
                               MENU_BTN_H / 2, i == pressed ? lighten(fills[i]) : fills[i]);
    canvas.drawString(labels[i], CX, y + 1);
  }
  canvas.fillSmoothRoundRect(CX - 30, oy + canvas.height() - 30, 60, 6, 3, C_DIM);  // handle
}

static void draw_off_warning() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", off_warning_s);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(C_AMBER);
  canvas.drawString("NO DONGLE", CX, CY - 92);
  canvas.setFont(&fonts::FreeSans12pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("powering off in", CX, CY - 42);
  canvas.setFont(&fonts::FreeSansBold24pt7b);
  canvas.drawString(buf, CX, CY + 12);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(C_MUTED);
  canvas.drawString("touch to stay on", CX, CY + 70);
}

static void render() {
  uint32_t now = millis();
  canvas.fillScreen(C_BG);
  if (off_warning_s) {
    draw_off_warning();
  } else if (screen != SCR_MAIN) {
    render_screen(now);
  } else {
    bool is_linked = linked();
    draw_ring(now, is_linked);
    draw_battery(CX, 66);
    draw_center(now, is_linked);
    draw_labels(now, is_linked);
    if (now < 15000) {                  // why did we last reset? (first 15 s after boot)
      char diag[40];
      vb_diag_summary(diag, sizeof(diag));
      canvas.setFont(&fonts::Font0);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(C_DIM);
      canvas.drawString(diag, CX, 2 * CY - 30);
    }
    draw_menu();
  }
  canvas.pushSprite(0, 0);
}

static void show_message(const char* title, const char* sub) {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(title, CX, CY - 14);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(sub, CX, CY + 26);
  canvas.pushSprite(0, 0);
}

static void do_reboot() {
  Serial.println("menu: reboot");
  show_message("REBOOTING", "");
  M5.Power.setVibration(0);
  delay(400);
  ESP.restart();
}

static void do_power_off() {
  Serial.println("menu: power off");
  show_message("POWER OFF", "press the power button to turn on");
  M5.Power.setVibration(200); delay(80); M5.Power.setVibration(0);
  delay(1500);
  M5.Display.sleep();
  M5.Power.powerOff();
  // Still running = USB power is keeping the board alive.
  M5.Display.wakeup();
  menu_target = 0.f;
  toast("ON USB POWER", C_AMBER, 2000);
}

// Power off without being asked: the dongle was unplugged, or the remote went idle without a dongle.
// A dark screen stays dark. On USB it can't turn off, so it stays on.
static void auto_power_off(const char* why) {
  if (M5.Power.getVBUSVoltage() > 4000) return;
  Serial.printf("power off: %s\n", why);
  Serial.flush();
  dictating = false;
  mic_stop();
  if (!screen_off) {
    show_message("POWER OFF", "press the power button to turn on");
    delay(1500);
    M5.Display.sleep();
  }
  M5.Power.powerOff();
  if (!screen_off) M5.Display.wakeup();  // still running: something else powers the board
}

// ---------------------------------------------------------------------------------
// Sleep: radio off + light sleep until a button or the touch panel pulls its line low, or the
// accelerometer (polled on a timer) sees the device move. RAM, the canvas and the pairing
// survive; only the radio has to reconnect.
// ---------------------------------------------------------------------------------
static const gpio_num_t BTN_PINS[] = {GPIO_NUM_2, GPIO_NUM_1};   // yellow, blue (active low)
static const gpio_num_t TOUCH_INT_PIN = GPIO_NUM_13;

// The gyro is never used and the accelerometer only feeds wake on pickup: run it in low power.
static uint8_t imu_addr = 0;
static float imu_lsb_per_g = 4096.f;
static uint32_t accel_fails = 0;

// Pickup diagnostics: the last lines stay in RAM (sleep keeps it); 'p' on USB serial prints them.
static char pk_log[16][72];
static uint8_t pk_head = 0, pk_count = 0;

static void pk_note(const char* fmt, ...) {
  char* line = pk_log[pk_head];
  int n = snprintf(line, sizeof(pk_log[0]), "[%lus] ", (unsigned long)(millis() / 1000));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(pk_log[0]) - n, fmt, ap);
  va_end(ap);
  Serial.println(line);
  pk_head = (pk_head + 1) % 16;
  if (pk_count < 16) pk_count++;
}

static void pk_print() {
  Serial.printf("pickup log (oldest first), threshold %.2f g, accel read failures %lu:\n",
                PICKUP_THRESHOLD_G, (unsigned long)accel_fails);
  for (int i = 0; i < pk_count; i++) Serial.printf("  %s\n", pk_log[(pk_head + 16 - pk_count + i) % 16]);
}

static bool read_accel(float a[3]);

// Battery level (see BATT_* in config.h). The shown level moves one way at a limited rate: down on
// battery, up on USB. On USB it is fed only with readings taken while charging is paused; a reading
// with the charger on reads several percent high.
struct batt_log_t { uint32_t s; uint16_t mv; uint8_t level, shown, flags; };   // flags: 1 usb, 2 charging, 4 paused reading
static constexpr int BATT_LOG_LEN = 128;    // one entry a minute: the last ~2 h awake (sleep isn't logged)
static batt_log_t batt_log[BATT_LOG_LEN];   // a ring: the oldest entry is overwritten
static uint8_t batt_log_n = 0, batt_log_head = 0;

static void batt_note(uint16_t mv, int level, uint8_t flags) {
  batt_log[batt_log_head] = { millis() / 1000, mv, (uint8_t)level, (uint8_t)battery, flags };
  batt_log_head = (batt_log_head + 1) % BATT_LOG_LEN;
  if (batt_log_n < BATT_LOG_LEN) batt_log_n++;
}

static void batt_print() {
  Serial.printf("battery log (oldest first), now %d%% %s%s:\n", battery, usb_power ? "usb" : "battery",
                charging ? " charging" : "");
  for (int i = 0; i < batt_log_n; i++) {
    const batt_log_t& e = batt_log[(batt_log_head + BATT_LOG_LEN - batt_log_n + i) % BATT_LOG_LEN];
    Serial.printf("  %6lus  %4u mV  level %3u  shown %3u  %s%s%s\n", (unsigned long)e.s, e.mv, e.level, e.shown,
                  e.flags & 1 ? "usb" : "bat", e.flags & 2 ? " chg" : "", e.flags & 4 ? " paused" : "");
  }
}

static int batt_level(float mv) { return constrain((int)((mv - 3300.f) * 100.f / 800.f), 0, 100); }

static void batt_show(int level, uint32_t now) {
  static float shown = -1.f;
  static uint32_t last_ms = 0;
  float budget = (now - last_ms) / 60000.f;
  last_ms = now;
  if (shown < 0.f) shown = level;
  else if (usb_power && level > shown) shown = min((float)level, shown + BATT_UP_PER_MIN * budget);
  else if (!usb_power && level < shown) shown = max((float)level, shown - BATT_DOWN_PER_MIN * budget);
  battery = (int)(shown + 0.5f);
}

static void battery_update(uint32_t now) {
  static uint32_t last_ms = 0, paused_at = 0, last_pause_ms = 0, resumed_ms = 0, last_note_ms = 0;
  static float mv_avg = 0.f;
  static bool was_usb = false;
  if (paused_at) {
    bool usb = M5.Power.getVBUSVoltage() > 4000;
    if (usb && now - paused_at < BATT_PAUSE_MS) return;
    int16_t mv = M5.Power.getBatteryVoltage();
    M5.Power.setBatteryCharge(true);
    paused_at = 0;
    last_pause_ms = resumed_ms = now;
    if (usb && mv > 0) {
      int level = charging ? batt_level(mv) : 100;   // the charger stopped: full
      batt_show(level, now);
      batt_note(mv, level, 1 | (charging ? 2 : 0) | 4);
    }
    return;
  }
  if (last_ms && now - last_ms < BATT_SAMPLE_MS) return;
  last_ms = now;
  usb_power = M5.Power.getVBUSVoltage() > 4000;
  if (!usb_power || !resumed_ms || now - resumed_ms > 1500)   // just after a pause, CHG_STAT still says "not charging"
    charging = usb_power && M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
  if (usb_power) {
    was_usb = true;
    if (!last_pause_ms || now - last_pause_ms >= BATT_PAUSE_EVERY_MS) {
      M5.Power.setBatteryCharge(false);
      paused_at = now ? now : 1;
    }
    return;
  }
  int16_t mv = M5.Power.getBatteryVoltage();
  if (mv <= 0) return;
  mv += BATT_LOAD_MV;
  if (was_usb || mv_avg == 0.f) mv_avg = mv;   // just unplugged: start from the battery's own reading
  mv_avg += (mv - mv_avg) * 0.1f;              // ~50 s at one sample per 5 s
  was_usb = false;
  last_pause_ms = 0;                           // plugged in again: measure at once
  int level = batt_level(mv_avg);
  batt_show(level, now);
  if (!last_note_ms || now - last_note_ms >= 60000) { last_note_ms = now; batt_note(mv, level, 0); }
}

// 'p' = pickup log, 'a' = ten raw accelerometer readings (are they live?), 'b' = battery log
static void on_serial_char(char c) {
  if (c == 'p') pk_print();
  if (c == 'b') batt_print();
  if (c == 'a') {
    for (int i = 0; i < 10; i++) {
      float v[3];
      bool ok = read_accel(v);
      Serial.printf("accel %s  x=%+.4f y=%+.4f z=%+.4f g\n", ok ? "ok  " : "FAIL", v[0], v[1], v[2]);
      delay(200);
    }
  }
}

static void imu_setup() {
  if (M5.Imu.getType() != m5::imu_t::imu_bmi270) { Serial.println("imu: no BMI270"); return; }
  uint8_t addr = 0;
  for (uint8_t a : {0x68, 0x69}) if (M5.In_I2C.readRegister8(a, 0x00, 400000) == 0x24) addr = a;   // CHIP_ID
  if (!addr) { Serial.println("imu: BMI270 not answering"); return; }
  M5.In_I2C.writeRegister8(addr, 0x7D, 0x04, 400000); delay(2);   // PWR_CTRL: accel only, gyro off
  M5.In_I2C.writeRegister8(addr, 0x40, 0x17, 400000); delay(2);   // ACC_CONF: 50 Hz, avg 2, low power
  M5.In_I2C.writeRegister8(addr, 0x7C, 0x01, 400000); delay(2);   // PWR_CONF: advanced power save
  imu_addr = addr;
  imu_lsb_per_g = 32768.f / (2 << (M5.In_I2C.readRegister8(addr, 0x41, 400000) & 3));   // ACC_RANGE
  imu_ok = true;
  Serial.printf("imu: BMI270 at 0x%02x, %.0f LSB/g\n", addr, imu_lsb_per_g);
}

// Green power LED, driven by the PM1's LED_EN bit (PWR_CFG 0x06 bit 4). The LED is lit at boot,
// so whatever level the bit has then means "on".
static bool led_on_level = true;

static void led_setup() {
  led_on_level = M5.Power.M5pm1.readRegister8(0x06) & 0x10;
  Serial.printf("power LED: LED_EN=%d means on\n", led_on_level);
}

static void power_led(bool on) { M5.Power.M5pm1.setLedEnLevel(on ? led_on_level : !led_on_level); }

// The PM1 can only switch the LED, over a slow shared I2C bus, so it can't be dimmed smoothly: awake,
// it blinks. Runs while awake; sleep_now() stops it first.
static std::atomic<bool> led_blink{false};
static std::atomic<bool> led_idle{true};       // the task has let go of the LED (it is off)

static void led_task(void*) {
  bool lit = false;
  for (;;) {
    if (!led_blink) {
      if (!led_idle) { power_led(false); lit = false; led_idle = true; }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    led_idle = false;
    uint32_t t = millis() % AWAKE_BLINK_MS;
    bool on = t < AWAKE_BLINK_ON_MS;
    if (on != lit) { power_led(on); lit = on; }
    vTaskDelay(pdMS_TO_TICKS(on ? AWAKE_BLINK_ON_MS - t : min<uint32_t>(AWAKE_BLINK_MS - t, 20)));
  }
}

static void led_blinking(bool on) {
  led_blink = on;
  if (!on) for (int i = 0; i < 50 && !led_idle; i++) delay(1);
}

// Straight from the data registers: M5Unified only refreshes its copy when the data-ready flag
// is set, which is not dependable with the sensor in low-power mode.
static bool read_accel(float a[3]) {
  if (!imu_ok) return false;
  int16_t raw[3];
  if (!M5.In_I2C.readRegister(imu_addr, 0x0C, (uint8_t*)raw, sizeof(raw), 400000)) { accel_fails++; return false; }
  for (int i = 0; i < 3; i++) a[i] = raw[i] / imu_lsb_per_g;
  return true;
}

// A pickup = the reading moved more than PICKUP_THRESHOLD_G away from where the device rested.
// The rest position only follows while the device is still, so a slow tilt adds up instead of
// slipping under the threshold step by step.
static float dist2(const float a[3], const float b[3]) {
  float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return dx * dx + dy * dy + dz * dz;
}

static bool motion_moved(motion_t& m, const float a[3]) {
  if (!m.have) {
    memcpy(m.rest, a, sizeof(m.rest)); memcpy(m.prev, a, sizeof(m.prev));
    m.have = true; m.reads = 0; m.max_dev2 = 0;
    return false;
  }
  m.reads++;
  bool still = dist2(a, m.prev) < 0.02f * 0.02f;
  memcpy(m.prev, a, sizeof(m.prev));
  float dev2 = dist2(a, m.rest);
  if (dev2 > m.max_dev2) m.max_dev2 = dev2;
  if (dev2 > PICKUP_THRESHOLD_G * PICKUP_THRESHOLD_G) return true;
  if (still) for (int i = 0; i < 3; i++) m.rest[i] += (a[i] - m.rest[i]) * 0.3f;
  return false;
}

// Awake but dimmed or dark: a pickup lights the screen.
static void pickup_update() {
  static motion_t m;
  static uint32_t last_ms = 0;
  // With its dongle missing, movement doesn't count: in a moving bag it would keep the remote from powering off.
  if (!settings.wake_on_pickup || !imu_ok || !(dimmed || screen_off) || dongle_missing()) {
    if (m.have && m.reads) pk_note("awake: no pickup, %lu reads, max %.3f g", (unsigned long)m.reads, sqrtf(m.max_dev2));
    m.have = false;
    return;
  }
  uint32_t now = millis();
  if (now - last_ms < PICKUP_POLL_MS) return;
  last_ms = now;
  float a[3];
  if (!read_accel(a) || !motion_moved(m, a)) return;
  pk_note("awake: pickup after %lu reads, %.3f g", (unsigned long)m.reads, sqrtf(m.max_dev2));
  m.have = false;
  wake();
}

static void sleep_now() {
  bool touch = settings.wake_on_touch, pickup = settings.wake_on_pickup && imu_ok;
  for (gpio_num_t p : BTN_PINS) if (gpio_get_level(p) == 0) return;   // held: would wake at once
  if (touch && gpio_get_level(TOUCH_INT_PIN) == 0) return;
  Serial.printf("sleep: radio off (wake on%s%s)\n", touch ? " touch" : "", pickup ? " pickup" : "");
  Serial.flush();
  if (!screen_off) { M5.Display.sleep(); screen_off = true; }
  radio_sleep();

  for (gpio_num_t p : BTN_PINS) gpio_wakeup_enable(p, GPIO_INTR_LOW_LEVEL);
  if (touch) gpio_wakeup_enable(TOUCH_INT_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  bool blink = SLEEP_BLINK_MS > 0;
  uint32_t tick_ms = pickup ? PICKUP_POLL_MS : SLEEP_BLINK_MS;
  if (pickup || blink) esp_sleep_enable_timer_wakeup(tick_ms * 1000ULL);
  led_blinking(false);
  power_led(false);
  motion_t motion = {};
  float a[3];
  if (pickup && read_accel(a)) motion_moved(motion, a);
  bool moved = false;
  uint32_t t0 = millis(), next_blink = t0 + SLEEP_BLINK_MS;
  for (;;) {
    esp_light_sleep_start();
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) break;
    if (blink && (int32_t)(millis() - next_blink) >= 0) {
      next_blink += SLEEP_BLINK_MS;
      power_led(true);                  // the blink is slept through as well
      esp_sleep_enable_timer_wakeup(SLEEP_BLINK_ON_MS * 1000ULL);
      esp_light_sleep_start();
      power_led(false);
      esp_sleep_enable_timer_wakeup(tick_ms * 1000ULL);
      if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) break;
    }
    if (!pickup) continue;
    if (read_accel(a) && motion_moved(motion, a)) { moved = true; break; }
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  for (gpio_num_t p : BTN_PINS) gpio_wakeup_disable(p);
  gpio_wakeup_disable(TOUCH_INT_PIN);
  bool button = gpio_get_level(BTN_PINS[0]) == 0 || gpio_get_level(BTN_PINS[1]) == 0;
  led_blinking(true);

  radio_wake();
  link_wait_until = millis() + WAKE_LINK_WAIT_MS;
  last_activity_ms = millis();
  if (!button) wake();                  // a button press is left to buttons_update: it lights the screen
  pk_note("sleep %lus: woke by %s, %lu reads, max %.3f g", (unsigned long)((millis() - t0) / 1000),
          button ? "button" : moved ? "pickup" : "touch", (unsigned long)motion.reads, sqrtf(motion.max_dev2));
}

// ---------------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Power.setBatteryCharge(true);   // the PM1 keeps its registers: a reset may have hit a battery pause
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on logs: unread on the Mac, each line would stall ~2 s
  vb_diag_begin();
  settings_load();
  vb_diag_on_char = on_serial_char;
  imu_setup();
  led_setup();
  xTaskCreatePinnedToCore(led_task, "led", 2048, nullptr, 2, nullptr, 0);
  led_blinking(true);

  M5.Display.setBrightness(brightness_active());
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) Serial.println("canvas alloc failed");
  CX = M5.Display.width() / 2; CY = M5.Display.height() / 2; MIC_Y = CY - 28;

  M5.BtnA.setHoldThresh(RESYNC_HOLD_MS);
  M5.BtnB.setHoldThresh(RESYNC_HOLD_MS);
  M5.Touch.setFlickThresh(TAP_SLOP_PX);

  frame_queue = xQueueCreate(4, sizeof(mic_buf[0]));
  M5.Mic.setBufferReleaseCallback(nullptr, rec_released);
  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 3, nullptr, 1);
  M5.Speaker.end();

  radio_begin();
  if (store.count) link_activate();
  else open_discover();          // fresh device: go straight to ADD DONGLE

  last_activity_ms = millis();
  Serial.printf("VoiceInk Button handheld %s up, %d dongle(s), display %dx%d\n",
                my_id, store.count, M5.Display.width(), M5.Display.height());
  vb_diag_print();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  battery_update(now);

  buttons_update();
  touch_update();
  menu_update();
  radio_update(battery, charging);
  deferred_update();
  voiceink_sync();
  enter_update();
  pickup_update();
  mic_update();
  vib_update();
  vb_diag_update();

  // Fresh time: the input handlers above may have set last_activity_ms after `now` was taken,
  // and a negative difference would read as a huge idle time (instant dim / sleep).
  now = millis();
  // An open menu or list doesn't count: left alone, it closes when the screen goes off. Nor does
  // dictating without a link: nothing reaches the Mac, and it would keep the remote from powering off.
  bool busy = ((dictating || mic_on) && linked()) || enter_due_ms || deferred_btn || send_state != SEND_IDLE
           || pair_state != PS_NONE;
  if (busy) last_activity_ms = now;
  int32_t idle = (int32_t)(now - last_activity_ms);
  // On battery with the dongle missing: full brightness, a countdown, then power off. Otherwise: dim, then sleep.
  bool no_dongle = !usb_power && dongle_missing();
  off_warning_s = no_dongle && !screen_off && idle > SLEEP_AFTER_MS - OFF_WARNING_MS
                ? max((int)((SLEEP_AFTER_MS - idle + 999) / 1000), 1) : 0;
  if (no_dongle && dimmed && !screen_off) {
    M5.Display.setBrightness(brightness_active());
    dimmed = false;
  }
  if (linked() && dongle_usb_state == VB_USB_POWERING_OFF) auto_power_off("dongle unplugged");
  if (no_dongle && idle > SLEEP_AFTER_MS) {
    auto_power_off("no dongle");
  } else if (!usb_power && screen_off && idle > SLEEP_AFTER_MS) {
    sleep_now();
    return;
  }
  if (screen == SCR_DISCOVER && idle > SCREEN_OFF_AFTER_MS) {
    screen_back();                        // lit, with a fresh idle time: going dark here would power off at once
    menu_target = menu_pos = 0.f;
    wake();
  } else if (!screen_off && idle > SCREEN_OFF_AFTER_MS) {
    M5.Display.sleep();
    screen_off = true;
    if (screen != SCR_MAIN) screen_back();
    menu_target = menu_pos = 0.f;         // after screen_back, which reopens the menu
  } else if (!no_dongle && !dimmed && !screen_off && idle > DIM_AFTER_MS) {
    M5.Display.setBrightness(BRIGHTNESS_DIM);
    dimmed = true;
  }
  if (screen_off) { delay(5); return; }

  // ~30 fps while something moves, ~4 fps otherwise
  bool animating = busy || menu_pos > 0.f || screen != SCR_MAIN || !linked() || dongle_no_usb() || (int32_t)(enter_flash_until - now) > 0;
  static uint32_t last_frame = 0;
  if (now - last_frame >= (animating ? 33u : 250u)) { last_frame = now; render(); }
  else delay(1);
}
