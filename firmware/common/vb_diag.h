// Boot diagnostics shared by both devices: why did we reset, and how long had we been running?
// Keeps the last 8 boots in NVS. Send 'd' over the USB serial port to print the history.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

struct vb_boot_rec_t { uint8_t reason; uint8_t reserved[3]; uint32_t prev_uptime_s; };
struct vb_diag_t { uint8_t count; uint8_t head; uint8_t reserved[2]; vb_boot_rec_t boots[8]; };

static Preferences vb_diag_prefs;
static vb_diag_t vb_diag;

static const char* vb_reset_name(uint8_t r) {
  switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON:   return "POWER-ON";     // power applied / PMIC power cycle
    case ESP_RST_EXT:       return "EXT PIN";
    case ESP_RST_SW:        return "SOFTWARE";     // esp_restart(), flashing
    case ESP_RST_PANIC:     return "PANIC";        // crash; coredump partition has details
    case ESP_RST_INT_WDT:   return "INT WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";     // supply dipped
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";          // USB peripheral reset (e.g. download mode exit)
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";
  }
}

static const vb_boot_rec_t& vb_diag_boot(int ago) {   // 0 = this boot
  return vb_diag.boots[(vb_diag.head + 8 - ago) % 8];
}

static void vb_diag_print() {
  Serial.printf("boot history (newest first), %u boots:\n", vb_diag.count);
  for (int i = 0; i < min<int>(vb_diag.count, 8); i++) {
    const vb_boot_rec_t& b = vb_diag_boot(i);
    Serial.printf("  %d: reset=%s, previous run lasted %lus\n", i, vb_reset_name(b.reason),
                  (unsigned long)b.prev_uptime_s);
  }
  Serial.printf("  now up %lus\n", (unsigned long)(millis() / 1000));
}

static void vb_diag_begin() {
  vb_diag_prefs.begin("vb_diag", false);
  if (vb_diag_prefs.getBytes("hist", &vb_diag, sizeof(vb_diag)) != sizeof(vb_diag)) memset(&vb_diag, 0, sizeof(vb_diag));
  uint32_t prev_up = vb_diag_prefs.getUInt("up", 0);
  vb_diag.head = (vb_diag.head + 1) % 8;
  if (vb_diag.count < 255) vb_diag.count++;
  vb_boot_rec_t& b = vb_diag.boots[vb_diag.head];
  b.reason = (uint8_t)esp_reset_reason();
  b.prev_uptime_s = prev_up;
  vb_diag_prefs.putBytes("hist", &vb_diag, sizeof(vb_diag));
  vb_diag_prefs.putUInt("up", 0);
}

// Other serial command characters go here, if the sketch sets it.
static void (*vb_diag_on_char)(char c) = nullptr;

// Call from loop(): records uptime every 30 s and answers 'd' on the serial port.
static void vb_diag_update() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last >= 30000) { last = now; vb_diag_prefs.putUInt("up", now / 1000); }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') vb_diag_print();
    else if (vb_diag_on_char) vb_diag_on_char(c);
  }
}

// "BROWNOUT after 12m" for the screen
static void vb_diag_summary(char* out, size_t n) {
  const vb_boot_rec_t& b = vb_diag_boot(0);
  uint32_t s = b.prev_uptime_s;
  if (s >= 3600) snprintf(out, n, "%s, ran %luh", vb_reset_name(b.reason), (unsigned long)(s / 3600));
  else if (s >= 60) snprintf(out, n, "%s, ran %lum", vb_reset_name(b.reason), (unsigned long)(s / 60));
  else snprintf(out, n, "%s, ran <%lus", vb_reset_name(b.reason), (unsigned long)(s + 30));
}
