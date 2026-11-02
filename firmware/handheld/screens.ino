// Secondary screens: ADD DONGLE (discovery list + pairing), DONGLES (manage remembered ones)
// and SETTINGS.
// Lists are "rotary": the highlighted row sits in the middle of the round screen.

static constexpr int ROW_H = 92;
static constexpr int PILL_W = 170, PILL_H = 54;
static int dongles_sel = 0;
static bool rows_linked = false;   // link state snapshot for draw_dongle_row()
static int settings_sel = 0;

static int pill_y() { return 2 * CY - 64; }

static void open_discover() {
  menu_target = menu_pos = 0.f;
  screen = SCR_DISCOVER;
  list_drag = 0; list_dragging = false;
  discover_begin();
}

static void close_discover() {
  discover_end();
  screen = SCR_MAIN;
}

static void open_dongles() {
  menu_target = menu_pos = 0.f;
  screen = SCR_DONGLES;
  dongles_sel = store.active >= 0 ? store.active : 0;
  forget_armed = -1;
  list_drag = 0; list_dragging = false;
}

static void open_settings() {
  menu_target = menu_pos = 0.f;
  screen = SCR_SETTINGS;
  settings_sel = 0;
  list_drag = 0; list_dragging = false;
}

// called by pair_update() once the result has been shown
static void on_pair_result(bool ok) {
  if (ok) {
    char id[8]; strcpy(id, pa.peer_id);
    close_discover();
    static char msg[20];
    snprintf(msg, sizeof(msg), "PAIRED %s", id);
    toast(msg, C_GREEN, 2500);
  } else {
    pair_state = PS_NONE;            // back to the list
    discover_started_ms = millis();
  }
}

// ---------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------
static constexpr int SETTINGS_ROWS = 3;   // 2 = brightness
static constexpr uint8_t BRIGHTNESS[] = BRIGHTNESS_LEVELS;
static const char* const BRIGHTNESS_NAME[] = BRIGHTNESS_NAMES;
static constexpr uint8_t BRIGHTNESS_COUNT = sizeof(BRIGHTNESS);

static int list_count() {
  return screen == SCR_DISCOVER ? found_count : screen == SCR_SETTINGS ? SETTINGS_ROWS : store.count;
}
static int& list_sel() {
  return screen == SCR_DISCOVER ? found_sel : screen == SCR_SETTINGS ? settings_sel : dongles_sel;
}

static void list_move(int d) {
  int n = list_count();
  if (!n) return;
  list_sel() = constrain(list_sel() + d, 0, n - 1);
  forget_armed = -1;
}

static void list_choose() {
  int i = list_sel();
  if (i >= list_count()) return;
  if (screen == SCR_SETTINGS) {
    if (i == 2) {
      settings.brightness = (settings.brightness + 1) % BRIGHTNESS_COUNT;
      M5.Display.setBrightness(brightness_active());
    } else {
      setting_value(i) ^= 1;
    }
    settings_save();
    VIBRATE(VIB_ENTER);
  } else if (screen == SCR_DISCOVER) {
    pair_start(i);
  } else if (forget_armed == i && (int32_t)(forget_armed_until - millis()) > 0) {
    char id[8]; vb_id_str(store.d[i].mac, id);
    forget_dongle(i);
    static char msg[20]; snprintf(msg, sizeof(msg), "FORGOT %s", id);
    toast(msg, C_MUTED);
    forget_armed = -1;
    dongles_sel = max(0, min(dongles_sel, (int)store.count - 1));
  } else if (i != store.active) {
    link_set_active(i);
    VIBRATE(VIB_ENTER);
  }
}

// Back from a screen goes to the menu it was opened from (a pairing in progress is cancelled first).
static void screen_back() {
  if (screen == SCR_DISCOVER && pair_state != PS_NONE) { pair_reject(); return; }
  if (screen == SCR_DISCOVER) close_discover();
  else screen = SCR_MAIN;
  menu_target = menu_pos = 1.f;
}

static void screen_buttons(bool yellow, bool yellow_hold, bool blue, bool blue_hold) {
  if (blue_hold) { screen_back(); return; }
  if (screen == SCR_DISCOVER && pair_state != PS_NONE) {
    if (pair_state == PS_CODE && !pa.local_accepted && yellow) pair_accept();
    else if (blue) pair_reject();
    return;
  }
  if (yellow) list_move(-1);
  if (blue) list_move(+1);
  if (yellow_hold) list_choose();
}

static bool in_circle(int x, int y, int cx, int cy, int r) {
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static void screen_touch(const m5::Touch_Class::touch_detail_t& t) {
  bool pairing = screen == SCR_DISCOVER && pair_state != PS_NONE;

  if (!pairing && (t.isDragging() || t.isFlicking())) {
    list_dragging = true;
    list_drag = t.distanceY();
    return;
  }
  if (list_dragging && !t.isPressed()) {
    list_dragging = false;
    list_move(-(int)lroundf(list_drag / ROW_H));
    list_drag = 0;
    return;
  }

  if (screen == SCR_DONGLES && t.wasHold() && abs(t.y - CY) < ROW_H / 2 && dongles_sel < store.count) {
    forget_armed = dongles_sel;
    forget_armed_until = millis() + 3000;
    VIBRATE(VIB_RESYNC);
    return;
  }
  if (!t.wasClicked()) return;

  bool pill_visible = !(pairing && (pair_state == PS_DONE || pair_state == PS_FAILED ||
                                    (pair_state == PS_CODE && !pa.local_accepted)));
  if (pill_visible && abs(t.x - CX) < PILL_W / 2 && abs(t.y - pill_y()) < PILL_H / 2) { screen_back(); return; }

  if (pairing) {
    if (pair_state == PS_CODE && !pa.local_accepted) {
      if (in_circle(t.x, t.y, CX - 75, CY + 95, 46)) pair_accept();
      else if (in_circle(t.x, t.y, CX + 75, CY + 95, 46)) pair_reject();
    }
    return;
  }

  if (screen == SCR_DISCOVER && !found_count) { discover_started_ms = millis(); return; }  // retry
  int dy = t.y - CY;
  if (abs(dy) < ROW_H / 2) list_choose();
  else if (dy < 0 && dy > -ROW_H * 3 / 2) list_move(-1);
  else if (dy > 0 && dy < ROW_H * 3 / 2) list_move(+1);
}

// ---------------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------------
static void draw_title(const char* text) {
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(text, CX, CY - 168);
}

static void draw_pill(const char* text) {
  int y = pill_y();
  uint16_t fill = RGB(44, 44, 46);
  canvas.fillSmoothRoundRect(CX - PILL_W / 2, y - PILL_H / 2, PILL_W, PILL_H, PILL_H / 2,
                             pressed_in_rect(CX, y, PILL_W, PILL_H) ? lighten(fill) : fill);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(text, CX, y + 1);
}

static void draw_comet(uint32_t now, uint16_t col) {
  const int r_out = 229, r_in = 223;
  float head = fmodf(now * 0.30f, 360.f);
  for (int k = 0; k < 14; k++) {
    float a1 = head - k * 6.f, a0 = a1 - 6.5f;
    canvas.fillArc(CX, CY, r_out, r_in, a0, a1, scale(col, 1.f - k / 14.f));
  }
}

static void draw_spinner(uint32_t now, int x, int y, int r, uint16_t col) {
  float a = fmodf(now * 0.45f, 360.f);
  canvas.fillArc(x, y, r, r - 6, 0, 360, scale(col, 0.18f));
  canvas.fillArc(x, y, r, r - 6, a, a + 80, col);
}

static void draw_signal(int x, int y, float rssi) {
  int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  for (int b = 0; b < 4; b++) {
    int h = 6 + b * 5;
    canvas.fillRoundRect(x + b * 8, y - h, 5, h, 1, b < bars ? C_TEXT : C_DIM);
  }
}

// Row background: the selected row has one, and the row under a finger gets a lighter one (a tap
// anywhere in a row's band picks it, see screen_touch).
static void row_bg(int y, bool hl, uint16_t col = C_DISC) {
  bool pressed = press_y >= 0 && abs(press_y - y) < ROW_H / 2;
  if (hl || pressed) canvas.fillSmoothRoundRect(CX - 150, y - 40, 300, 80, 24, pressed ? lighten(col) : col);
}

// Draws rows sel-2..sel+2 around the center, offset by the current drag.
static void draw_list(int n, int sel, row_fn_t row) {
  int off = list_dragging ? (int)list_drag : 0;
  for (int k = -2; k <= 2; k++) {
    int i = sel + k;
    if (i < 0 || i >= n) continue;
    int y = CY + k * ROW_H + off;
    if (y < CY - 150 || y > CY + 120) continue;     // keep clear of title and pill
    bool hl = k == 0 && !list_dragging;
    row(i, y, hl);
  }
}

// ---------------------------------------------------------------------------------
// ADD DONGLE
// ---------------------------------------------------------------------------------
static void render_discover(uint32_t now) {
  char buf[40];
  canvas.setTextDatum(middle_center);

  if (pair_state == PS_HELLO || pair_state == PS_NONCE) {
    draw_title("ADD DONGLE");
    draw_spinner(now, CX, CY - 30, 70, C_BLUE);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(pa.peer_id, CX, CY - 30);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.drawString("connecting...", CX, CY + 75);
    draw_pill("CANCEL");
    return;
  }
  if (pair_state == PS_CODE) {
    snprintf(buf, sizeof(buf), "Code for %s", pa.peer_id);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.drawString(buf, CX, CY - 125);
    snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)(pa.code / 1000), (unsigned long)(pa.code % 1000));
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(buf, CX, CY - 62);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    if (!pa.local_accepted) {
      canvas.drawString("Same code on the dongle?", CX, CY + 5);
      canvas.fillSmoothCircle(CX - 75, CY + 95, 42, pressed_in_circle(CX - 75, CY + 95, 46) ? lighten(RGB(20, 80, 36)) : RGB(20, 80, 36));
      canvas.drawWideLine(CX - 95, CY + 96, CX - 81, CY + 110, 4, C_GREEN);
      canvas.drawWideLine(CX - 81, CY + 110, CX - 55, CY + 80, 4, C_GREEN);
      canvas.fillSmoothCircle(CX + 75, CY + 95, 42, pressed_in_circle(CX + 75, CY + 95, 46) ? lighten(RGB(90, 20, 16)) : RGB(90, 20, 16));
      canvas.drawWideLine(CX + 60, CY + 80, CX + 90, CY + 110, 4, C_RED);
      canvas.drawWideLine(CX + 90, CY + 80, CX + 60, CY + 110, 4, C_RED);
      canvas.fillSmoothCircle(CX - 75, CY + 152, 5, C_YELLOW);
      canvas.fillSmoothCircle(CX + 75, CY + 152, 5, C_BLUE);
    } else {
      snprintf(buf, sizeof(buf), "Press A on %s", pa.peer_id);
      canvas.drawString(pa.peer_confirmed ? "Finishing..." : buf, CX, CY + 5);
      draw_spinner(now, CX, CY + 70, 24, C_BLUE);
      draw_pill("CANCEL");
    }
    return;
  }
  if (pair_state == PS_DONE || pair_state == PS_FAILED) {
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.setTextColor(pair_state == PS_DONE ? C_GREEN : C_RED);
    canvas.drawString(pa.result, CX, CY - 20);
    canvas.setFont(&fonts::FreeSans12pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.drawString(pa.peer_id, CX, CY + 22);
    return;
  }

  // the list
  draw_title("ADD DONGLE");
  draw_comet(now, C_BLUE);
  if (!found_count) {
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    if (now - discover_started_ms < 10000) {
      canvas.setTextColor(C_TEXT);
      canvas.drawString("Looking for", CX, CY - 32);
      canvas.drawString("dongles...", CX, CY);
    } else {
      canvas.setTextColor(C_AMBER);
      canvas.drawString("No dongles found", CX, CY - 48);
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextColor(C_MUTED);
      canvas.drawString("Plug one in, or hold its", CX, CY - 10);
      canvas.drawString("button A for 3 s.", CX, CY + 14);
      canvas.setTextColor(C_TEXT);
      canvas.drawString("Tap to retry", CX, CY + 60);
    }
  } else {
    draw_list(found_count, found_sel, draw_found_row);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_DIM);
    canvas.drawString("tap to pair", CX, CY + 128);
  }
  draw_pill("CANCEL");
}

static void draw_found_row(int i, int y, bool hl) {
      const found_t& f = found[i];
      row_bg(y, hl);
      canvas.setTextDatum(middle_left);
      canvas.setFont(hl ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b);
      canvas.setTextColor(hl ? C_TEXT : C_MUTED);
      canvas.drawString(f.id, CX - 125, y - 9);
      const char* sub; uint16_t sc;
      if (found_is_mine(f))              { sub = "already yours"; sc = C_MUTED; }
      else if (f.flags & VB_ADV_PAIRED)  { sub = "paired, will replace"; sc = C_AMBER; }
      else                               { sub = "new"; sc = C_GREEN; }
      canvas.setFont(&fonts::FreeSans9pt7b);
      canvas.setTextColor(hl ? sc : C_DIM);
      canvas.drawString(sub, CX - 125, y + 22);
      draw_signal(CX + 92, y + 6, f.rssi);
      canvas.setTextDatum(middle_center);
}

// ---------------------------------------------------------------------------------
// DONGLES
// ---------------------------------------------------------------------------------
static void render_dongles(uint32_t now) {
  draw_title("DONGLES");
  canvas.setTextDatum(middle_center);
  if (!store.count) {
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(C_TEXT);
    canvas.drawString("No dongles yet", CX, CY - 20);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.drawString("Menu > ADD DONGLE", CX, CY + 14);
    draw_pill("BACK");
    return;
  }
  if (forget_armed >= 0 && (int32_t)(forget_armed_until - now) <= 0) forget_armed = -1;
  rows_linked = linked();
  draw_list(store.count, dongles_sel, draw_dongle_row);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(C_DIM);
  canvas.drawString("hold to forget", CX, CY + 128);
  draw_pill("BACK");
}

static void draw_dongle_row(int i, int y, bool hl) {
    bool is_linked = rows_linked;
    char id[8]; vb_id_str(store.d[i].mac, id);
    bool armed = forget_armed == i;
    row_bg(y, hl, armed ? RGB(120, 24, 18) : C_DISC);
    canvas.setTextDatum(middle_left);
    canvas.setFont(hl ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b);
    canvas.setTextColor(hl ? C_TEXT : C_MUTED);
    canvas.drawString(id, CX - 125, y - 9);
    const char* sub; uint16_t sc;
    if (armed)                        { sub = "tap again to forget"; sc = C_TEXT; }
    else if (i == store.active)       { sub = is_linked ? "active, linked" : "active, not in range"; sc = is_linked ? C_GREEN : C_AMBER; }
    else                              { sub = "tap to use"; sc = C_MUTED; }
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(hl ? sc : C_DIM);
    canvas.drawString(sub, CX - 125, y + 22);
    if (i == store.active) canvas.fillSmoothCircle(CX + 115, y - 2, 7, is_linked ? C_GREEN : C_AMBER);
    canvas.setTextDatum(middle_center);
}

// ---------------------------------------------------------------------------------
// SETTINGS
// ---------------------------------------------------------------------------------
static constexpr uint8_t SETTINGS_MAGIC = 0x52;   // 0x51: before brightness
static Preferences settings_prefs;

static void settings_load() {
  settings_prefs.begin("vb_set", false);
  bool ok = settings_prefs.getBytes("s", &settings, sizeof(settings)) == sizeof(settings);
  if (ok && settings.magic == 0x51) settings = { SETTINGS_MAGIC, settings.wake_on_pickup, settings.wake_on_touch, DEFAULT_BRIGHTNESS };
  else if (!ok || settings.magic != SETTINGS_MAGIC)
    settings = { SETTINGS_MAGIC, DEFAULT_WAKE_ON_PICKUP, DEFAULT_WAKE_ON_TOUCH, DEFAULT_BRIGHTNESS };
  if (settings.brightness >= BRIGHTNESS_COUNT) settings.brightness = DEFAULT_BRIGHTNESS;
}

static uint8_t brightness_active() { return BRIGHTNESS[settings.brightness]; }

static void settings_save() { settings_prefs.putBytes("s", &settings, sizeof(settings)); }

static uint8_t& setting_value(int i) { return i == 0 ? settings.wake_on_pickup : settings.wake_on_touch; }

static void render_settings(uint32_t now) {
  draw_title("SETTINGS");
  draw_list(SETTINGS_ROWS, settings_sel, draw_setting_row);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(C_DIM);
  canvas.drawString("tap to change", CX, CY + 128);
  draw_pill("BACK");
}

static void draw_setting_row(int i, int y, bool hl) {
  static const char* names[SETTINGS_ROWS] = {"Wake on pickup", "Wake on touch", "Brightness"};
  static const char* subs[SETTINGS_ROWS] = {"lift it to wake", "tap the screen to wake", "screen when awake"};
  row_bg(y, hl);
  canvas.setTextDatum(middle_left);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextColor(hl ? C_TEXT : C_MUTED);
  canvas.drawString(names[i], CX - 125, y - 9);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(hl ? C_MUTED : C_DIM);
  canvas.drawString(subs[i], CX - 125, y + 20);
  if (i == 2) {   // level name over four bars
    int bx = CX + 100;
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(hl ? C_TEXT : C_MUTED);
    canvas.drawString(BRIGHTNESS_NAME[settings.brightness], bx, y - 10);
    for (int b = 0; b < BRIGHTNESS_COUNT; b++)
      canvas.fillSmoothRoundRect(bx - 26 + b * 14, y + 8, 10, 8, 3, b <= settings.brightness ? RGB(30, 140, 60) : C_DIM);
    return;
  }
  bool on = setting_value(i);
  // switch
  int sx = CX + 100, sw = 52, sh = 30;
  canvas.fillSmoothRoundRect(sx - sw / 2, y - sh / 2, sw, sh, sh / 2, on ? RGB(30, 140, 60) : C_DIM);
  canvas.fillSmoothCircle(on ? sx + sw / 2 - sh / 2 : sx - sw / 2 + sh / 2, y, sh / 2 - 3, C_TEXT);
  canvas.setTextDatum(middle_center);
}

static void render_screen(uint32_t now) {
  if (screen == SCR_DISCOVER) render_discover(now);
  else if (screen == SCR_SETTINGS) render_settings(now);
  else render_dongles(now);
}
