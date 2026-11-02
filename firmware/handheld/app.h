// Shared state for the handheld sketch. Arduino concatenates handheld.ino, link.ino and
// screens.ino into one translation unit; everything used across those files lives here.
#pragma once
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <Preferences.h>
#include <atomic>
#include <math.h>
#include "config.h"
#include "vb_crypto.h"
#include "vb_diag.h"

// ---------------------------------------------------------------------------------
// Colors / geometry
// ---------------------------------------------------------------------------------
static int CX = 233, CY = 233;          // set from the panel size in setup()
static int MIC_Y = 205;                 // mic disc center
static constexpr int DISC_R = 80;

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t C_BG      = RGB(0, 0, 0);
static const uint16_t C_DISC    = RGB(28, 28, 30);
static const uint16_t C_DIM     = RGB(72, 72, 74);
static const uint16_t C_MUTED   = RGB(142, 142, 147);
static const uint16_t C_TEXT    = RGB(235, 235, 240);
static const uint16_t C_RED     = RGB(255, 59, 48);
static const uint16_t C_AMBER   = RGB(255, 176, 0);
static const uint16_t C_GREEN   = RGB(48, 209, 88);
static const uint16_t C_BLUE    = RGB(10, 132, 255);
static const uint16_t C_YELLOW  = RGB(255, 204, 0);

static M5Canvas canvas(&M5.Display);

// ---------------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------------
enum screen_t : uint8_t { SCR_MAIN, SCR_DISCOVER, SCR_DONGLES, SCR_SETTINGS };
static screen_t screen = SCR_MAIN;

// ---------------------------------------------------------------------------------
// Remembered dongles (NVS)
// ---------------------------------------------------------------------------------
static constexpr int MAX_DONGLES = 4;
static constexpr uint8_t STORE_MAGIC = 0xA2;

struct dongle_rec_t {
  uint8_t  mac[6];
  uint8_t  lmk[16];
  uint8_t  mac_key[32];
  uint8_t  channel;
  uint8_t  reserved;
  uint32_t last_used;   // store.clock value, for LRU replacement
};

struct store_t {
  uint8_t  magic;
  uint8_t  count;
  int8_t   active;      // index into d[], -1 = none
  uint8_t  reserved;
  uint32_t clock;
  dongle_rec_t d[MAX_DONGLES];
};
static store_t store;

static uint8_t my_mac[6];
static char my_id[8];

// ---------------------------------------------------------------------------------
// Radio / session
// ---------------------------------------------------------------------------------
// Packets are handed from the Wi-Fi task to loop() through a queue.
struct rx_msg_t { uint8_t mac[6]; int8_t rssi; uint8_t len; uint8_t data[80]; };
typedef void (*row_fn_t)(int i, int y, bool highlighted);

// ---------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------
enum radio_mode_t : uint8_t { RM_LINK, RM_DISCOVER };
static radio_mode_t radio_mode = RM_LINK;

static bool session_ok = false;
static uint32_t last_pong_ms = 0;
static uint8_t dongle_usb_state = VB_USB_NONE;   // from the dongle's PONG
static uint8_t voiceink_state = VB_REC_UNKNOWN;  // from the dongle's PONG (vb_rec_state_t)
static uint8_t cur_channel = 0;

// Pending key transmission (one at a time)
enum send_state_t : uint8_t { SEND_IDLE, SEND_QUEUED, SEND_WAIT_CB, SEND_OK, SEND_FAIL };
static std::atomic<uint8_t> send_state{SEND_IDLE};
static vb_key_t pending_key;

// Audio frames this dictation (link.ino counts, handheld.ino logs)
static std::atomic<uint32_t> audio_sent{0}, audio_busy{0}, audio_failed{0};

// ---------------------------------------------------------------------------------
// Discovery + pairing (initiator)
// ---------------------------------------------------------------------------------
static constexpr int MAX_FOUND = 8;
struct found_t {
  uint8_t  mac[6];
  char     id[8];
  float    rssi;        // smoothed dBm
  uint8_t  flags;       // VB_ADV_*
  uint8_t  paired_to[2];
  uint32_t last_seen;
};
static found_t found[MAX_FOUND];
static int found_count = 0;
static int found_sel = 0;
static uint32_t discover_started_ms = 0;

enum pair_state_t : uint8_t { PS_NONE, PS_HELLO, PS_NONCE, PS_CODE, PS_DONE, PS_FAILED };
static pair_state_t pair_state = PS_NONE;
static struct {
  uint8_t peer[6];
  char peer_id[8];
  uint8_t priv[32], pubA[32], pubB[32], commitB[32], nA[16], nB[16];
  vb_keys_t keys;
  uint32_t code;
  uint8_t peer_channel;
  bool local_accepted, peer_confirmed, confirm_acked;
  uint32_t started, last_tx, result_until;
  const char* result;
} pa;
static std::atomic<int> confirm_cb{0};   // 0 pending, 1 delivered, 2 failed

// ---------------------------------------------------------------------------------
// UI state shared between files
// ---------------------------------------------------------------------------------
static bool dictating = false;

// Pickup detection (handheld.ino)
struct motion_t { float rest[3]; float prev[3]; bool have; uint32_t reads; float max_dev2; };

// SETTINGS (screens.ino keeps them in NVS)
struct settings_t { uint8_t magic; uint8_t wake_on_pickup; uint8_t wake_on_touch; uint8_t brightness; };
static settings_t settings;
static uint32_t last_activity_ms = 0;
static const char* toast_text = nullptr;
static uint16_t toast_color = C_TEXT;
static uint32_t toast_until = 0;

static float menu_pos = 0.f;            // 0 = closed, 1 = fully open
static float menu_target = 0.f;

// list widget scroll state (discover + dongles screens)
static float list_drag = 0.f;           // pixels, while dragging
static bool list_dragging = false;
static int press_x = -1, press_y = -1;   // a finger resting on the screen (not dragging): pressed states
static int forget_armed = -1;           // dongles screen: row armed for forget
static uint32_t forget_armed_until = 0;
