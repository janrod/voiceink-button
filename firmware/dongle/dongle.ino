// VoiceInk Button dongle — M5Stack StickS3 plugged into the Mac.
//
// ESP-NOW receiver -> USB HID keyboard. Pairs with one handheld (docs/PRD-pairing.md), answers its
// heartbeats so both sides can show link state, and taps whatever key combo the handheld asks for
// (the shortcut lives in the handheld's config.h). Also a USB microphone ("VoiceInk Button Mic") that
// plays the handheld's mic audio, streamed over ESP-NOW while dictating. The mic is only offered
// while the handheld is linked (docs/PRD-mic-presence.md).
//
// Buttons: A (front) accept / hold 3 s = pairing mode.  B (side) reject / hold 6 s = forget remote.

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBCDC.h"
#include "USBAudioCard.h"
#include "tusb.h"

// USB is started by this sketch (CDCOnBoot off), so logs go to our own CDC port.
static USBCDC USBSerial;
#define Serial USBSerial

#include "vb_crypto.h"
#include "vb_diag.h"
#include "vb_keys.h"

static USBHIDKeyboard Keyboard;
static constexpr uint32_t MIC_RATE = VB_AUDIO_RATE;
static USBAudioCard uac(MIC_RATE, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);
static M5Canvas canvas(&M5.Display);
static Preferences prefs;

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t C_BG    = RGB(0, 0, 0);
static const uint16_t C_MUTED = RGB(142, 142, 147);
static const uint16_t C_TEXT  = RGB(235, 235, 240);
static const uint16_t C_RED   = RGB(255, 59, 48);
static const uint16_t C_AMBER = RGB(255, 176, 0);
static const uint16_t C_GREEN = RGB(48, 209, 88);
static const uint16_t C_BLUE  = RGB(10, 132, 255);

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint32_t PAIRING_MODE_MS = 60000;

// Packets are handed from the Wi-Fi task to loop() through a queue.
struct rx_msg_t { uint8_t mac[6]; uint8_t len; uint8_t data[80]; };
static QueueHandle_t rx_queue;
struct audio_msg_t { uint8_t mac[6]; vb_audio_t pkt; };   // audio frames take a separate queue
static QueueHandle_t audio_queue;
struct key_cmd_t { uint8_t key_id, mods, keycode, hold_ms; uint32_t since; };   // waiting for USB/mic

static uint8_t usb_state = VB_USB_NONE;   // polled from TinyUSB in loop()
static uint32_t usb_lost_since = 0;      // when it last stopped being mounted
static bool restarting = false;
static std::atomic<bool> mic_present{false};       // VoiceInk Button Mic is in our USB configuration
static bool usb_switching = false;                 // our own reconnect to add/remove the mic
static std::atomic<uint32_t> usb_mounted_ms{0};    // when the Mac last finished setting us up

static void usb_poll() {
  if (usb_switching) return;             // keep showing (and reporting) the state from before
  uint8_t s = !tud_mounted() ? VB_USB_NONE : tud_suspended() ? VB_USB_SUSPENDED : VB_USB_READY;
  if (s == VB_USB_NONE && usb_state != VB_USB_NONE) usb_lost_since = millis();
  usb_state = s;
}

static bool usb_missing() { return usb_state == VB_USB_NONE && millis() - usb_lost_since > 3000; }

static void restart_dongle() {
  restarting = true;
  Serial.println("restarting");
  tud_disconnect();          // make the Mac see a clean unplug before we reboot
  delay(250);
  ESP.restart();
}
static uint8_t my_mac[6];
static char my_id[8];
static uint8_t cur_channel = 0;

// ---------------------------------------------------------------------------------
// Pairing record (NVS)
// ---------------------------------------------------------------------------------
static constexpr uint8_t REC_MAGIC = 0xB2;
struct pair_rec_t { uint8_t magic; uint8_t mac[6]; uint8_t lmk[16]; uint8_t mac_key[32]; uint8_t channel; };
static pair_rec_t rec;
static char remote_id[8] = "";

static bool paired() { return rec.magic == REC_MAGIC; }

static void rec_save() {
  prefs.putBytes("rec", &rec, sizeof(rec));
  if (paired()) vb_id_str(rec.mac, remote_id); else remote_id[0] = 0;
}

static void rec_load() {
  if (prefs.getBytes("rec", &rec, sizeof(rec)) != sizeof(rec) || rec.magic != REC_MAGIC) memset(&rec, 0, sizeof(rec));
  if (paired()) vb_id_str(rec.mac, remote_id);
}

// ---------------------------------------------------------------------------------
// Discoverability + channel
// ---------------------------------------------------------------------------------
static uint32_t pairing_mode_until = 0;   // paired dongle temporarily discoverable
static uint8_t chosen_channel = 1;        // offered to the handheld at pairing
static uint32_t advertise_at = 0;
static uint32_t last_identify_ms = 0;

static bool discoverable() { return !paired() || pairing_mode_until != 0; }

static void set_channel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  cur_channel = ch;
}

static void draw_status(const char* title, const char* sub);

// Unplugged: the PM1 reports no valid 5 V input (PWR_SRC bit 0). USB data alone cannot tell, since
// a pulled cable looks like a sleeping Mac (suspend). Power off after 3 s; plugging it back in
// turns it on again. A failed register read decides nothing, and a working USB link always wins.
// After 1.5 s the PONGs tell the handheld to turn off as well; a quick cable swap reaches neither.
static bool powering_off = false;

static void power_update() {
  static uint32_t last_check = 0, unplugged_since = 0;
  uint32_t now = millis();
  if (now - last_check < 500) return;
  last_check = now;
  uint8_t src;
  if (!M5.Power.M5pm1.readRegister(0x04, &src, 1)) return;
  if ((src & 0x01) || usb_state == VB_USB_READY) { unplugged_since = 0; powering_off = false; return; }
  if (!unplugged_since) { unplugged_since = now; Serial.printf("power: unplugged (PWR_SRC 0x%02x)\n", src); return; }
  powering_off = now - unplugged_since >= 1500;
  if (now - unplugged_since < 3000) return;
  draw_status("POWER OFF", "plug into USB to turn on");
  delay(800);
  M5.Power.powerOff();
  unplugged_since = 0;                  // still running: something else powers the board
  powering_off = false;
}

// Pick the quietest of 1/6/11 by summed AP signal near each.
static void choose_channel() {
  draw_status("SCANNING", "picking a quiet channel");
  int n = WiFi.scanNetworks(false, true);
  static const uint8_t cand[] = {1, 6, 11};
  long best = LONG_MAX;
  for (uint8_t c : cand) {
    long score = 0;
    for (int i = 0; i < n; i++) {
      if (abs((int)WiFi.channel(i) - (int)c) <= 2) score += WiFi.RSSI(i) + 100;
    }
    Serial.printf("channel %u score %ld\n", c, score);
    if (score < best) { best = score; chosen_channel = c; }
  }
  WiFi.scanDelete();
  cur_channel = 0;  // scan moved the radio; force apply_radio_mode() to set it again
}

// Discoverable: plaintext on the pairing channel. Paired: encrypted peer on our channel.
static void apply_radio_mode() {
  if (discoverable()) {
    if (paired()) vb_peer_del(rec.mac);
    set_channel(VB_PAIR_CHANNEL);
  } else {
    set_channel(rec.channel);
    vb_peer_set(rec.mac, rec.lmk);
  }
  Serial.printf("radio: %s, channel %u\n", discoverable() ? "discoverable" : "paired", cur_channel);
}

// ---------------------------------------------------------------------------------
// Pairing (responder)
// ---------------------------------------------------------------------------------
enum bpair_t : uint8_t { BP_NONE, BP_COMMITTED, BP_CODE, BP_DONE, BP_FAILED };
static bpair_t bp = BP_NONE;
static struct {
  uint8_t peer[6];
  char peer_id[8];
  uint8_t priv[32], pubA[32], pubB[32], nA[16], nB[16], commit[32];
  vb_keys_t keys;
  uint32_t code;
  bool accepted, peer_confirmed, confirm_acked;
  uint32_t started, last_tx, result_until;
  const char* result;
} pr;
static volatile int confirm_cb = 0;   // 0 pending, 1 delivered, 2 failed

static void send(const uint8_t* mac, const void* pkt, size_t len) {
  esp_now_send(mac, (const uint8_t*)pkt, len);
}

static void pair_wipe() {
  memset(pr.priv, 0, sizeof(pr.priv));
  memset(&pr.keys, 0, sizeof(pr.keys));
}

static void pair_end(uint8_t result, const char* text) {
  if (!paired() || memcmp(pr.peer, rec.mac, 6) != 0) vb_peer_del(pr.peer);
  pair_wipe();
  bp = (bpair_t)result;
  pr.result = text;
  pr.result_until = millis() + 2500;
  Serial.printf("pairing: %s\n", text);
}

static void pair_send_confirm() {
  vb_pair_confirm_t c = { vb_hdr(VB_PKT_PAIR_CONFIRM), chosen_channel, {} };
  vb_pair_confirm_tag(pr.keys.mac_key, 'B', pr.pubA, pr.pubB, chosen_channel, c.tag);
  confirm_cb = 0;
  pr.last_tx = millis();
  send(pr.peer, &c, sizeof(c));
}

static void pair_abort(uint8_t reason, const char* text) {
  vb_pair_abort_t a = { vb_hdr(VB_PKT_PAIR_ABORT), reason };
  send(pr.peer, &a, sizeof(a));
  pair_end(BP_FAILED, text);
}

static bool pair_active() { return bp == BP_COMMITTED || bp == BP_CODE; }

static void on_pair_hello(const uint8_t* mac, const vb_pair_hello_t* h) {
  if (pair_active() && memcmp(mac, pr.peer, 6) != 0) {
    vb_peer_set(mac, nullptr);
    vb_hdr_t busy = vb_hdr(VB_PKT_PAIR_BUSY);
    send(mac, &busy, sizeof(busy));
    return;
  }
  if (bp == BP_COMMITTED && memcmp(h->pub, pr.pubA, 32) == 0) {
    vb_pair_commit_t c = { vb_hdr(VB_PKT_PAIR_COMMIT), {}, {} };     // retransmit
    memcpy(c.pub, pr.pubB, 32); memcpy(c.commit, pr.commit, 32);
    send(pr.peer, &c, sizeof(c));
    return;
  }
  if (pair_active()) return;
  memcpy(pr.peer, mac, 6);
  vb_id_str(mac, pr.peer_id);
  memcpy(pr.pubA, h->pub, 32);
  if (!vb_x25519_keygen(pr.priv, pr.pubB)) { Serial.println("keygen failed"); return; }
  esp_fill_random(pr.nB, 16);
  vb_pair_commit(pr.pubB, pr.pubA, pr.nB, pr.commit);
  pr.accepted = pr.peer_confirmed = pr.confirm_acked = false;
  pr.started = millis();
  bp = BP_COMMITTED;
  vb_peer_set(mac, nullptr);
  vb_pair_commit_t c = { vb_hdr(VB_PKT_PAIR_COMMIT), {}, {} };
  memcpy(c.pub, pr.pubB, 32); memcpy(c.commit, pr.commit, 32);
  send(mac, &c, sizeof(c));
  Serial.printf("pairing: hello from %s\n", pr.peer_id);
}

static void on_pair_nonce_a(const vb_pair_nonce_t* n) {
  vb_pair_nonce_t r = { vb_hdr(VB_PKT_PAIR_NONCE_B), {} };
  memcpy(r.nonce, pr.nB, 16);
  if (bp == BP_CODE) { send(pr.peer, &r, sizeof(r)); return; }   // retransmit
  if (bp != BP_COMMITTED) return;
  memcpy(pr.nA, n->nonce, 16);
  uint8_t shared[32];
  if (!vb_x25519_shared(pr.priv, pr.pubA, shared) || !vb_pair_derive(shared, pr.nA, pr.nB, &pr.keys)) {
    pair_abort(VB_ABORT_MISMATCH, "KEY ERROR");
    return;
  }
  memset(shared, 0, sizeof(shared));
  pr.code = vb_pair_code(pr.pubA, pr.pubB, pr.nA, pr.nB);
  bp = BP_CODE;
  send(pr.peer, &r, sizeof(r));
}

static void on_pair_confirm(const vb_pair_confirm_t* c) {
  if (bp != BP_CODE) return;
  uint8_t tag[16];
  vb_pair_confirm_tag(pr.keys.mac_key, 'A', pr.pubA, pr.pubB, c->channel, tag);
  if (!vb_ct_equal(tag, c->tag, 16)) { pair_abort(VB_ABORT_MISMATCH, "KEYS DIFFER"); return; }
  pr.peer_confirmed = true;
  if (pr.accepted) pair_send_confirm();
}

static void pair_update() {
  uint32_t now = millis();
  if ((bp == BP_DONE || bp == BP_FAILED) && (int32_t)(now - pr.result_until) >= 0) bp = BP_NONE;
  if (!pair_active()) return;
  if (now - pr.started > VB_PAIR_TIMEOUT_MS) { pair_abort(VB_ABORT_TIMEOUT, "TIMED OUT"); return; }
  if (bp != BP_CODE || !pr.accepted) return;

  if (confirm_cb == 1) pr.confirm_acked = true;
  if (!pr.confirm_acked && now - pr.last_tx > 300) pair_send_confirm();
  if (pr.confirm_acked && pr.peer_confirmed) {
    if (paired() && memcmp(rec.mac, pr.peer, 6) != 0) vb_peer_del(rec.mac);
    rec.magic = REC_MAGIC;
    memcpy(rec.mac, pr.peer, 6);
    memcpy(rec.lmk, pr.keys.lmk, 16);
    memcpy(rec.mac_key, pr.keys.mac_key, 32);
    rec.channel = chosen_channel;
    rec_save();
    pairing_mode_until = 0;
    pair_end(BP_DONE, "PAIRED");
    apply_radio_mode();
  }
}

// ---------------------------------------------------------------------------------
// USB microphone. The handheld's 20 ms frames go into a ring buffer (loop), and usb_mic_task
// keeps ~20 ms queued in the USB FIFO, from the ring or as silence. Audio from the start of a
// dictation is kept until the Mac opens the mic, so the first words survive VoiceInk's
// start-up; the stop key is held until that backlog has played out (stop_update).
// ---------------------------------------------------------------------------------
static constexpr uint32_t RING_LEN = 65536;                     // samples (~4 s), power of two
static constexpr uint32_t MAX_BACKLOG = MIC_RATE * 3;           // kept while the Mac is not listening
                                                                // (or the mic is still being added)
static constexpr uint32_t JITTER = MIC_RATE * 40 / 1000;        // buffered before playback starts
static constexpr uint32_t FIFO_TARGET = MIC_RATE * 20 / 1000;   // samples kept in the USB FIFO
static int16_t* ring;
static std::atomic<uint32_t> ring_w{0}, ring_r{0}, flush_pos{0};
static std::atomic<bool> ring_flush{false};   // loop asks usb_mic_task to drop audio before flush_pos
static std::atomic<bool> uac_open{false};     // the Mac is reading the mic
static uint32_t audio_seq = 0, last_audio_ms = 0, audio_frames = 0, audio_lost = 0;


static void on_uac_event(void*, esp_event_base_t, int32_t id, void* data) {
  auto* d = (arduino_usb_audio_card_event_data_t*)data;
  if (id == ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT && d->interface_enable.interface == UAC_INTERFACE_MIC)
    uac_open = d->interface_enable.enable;
}

static void usb_mic_log() {
  static bool was_open = false;
  if (uac_open == was_open) return;
  was_open = uac_open;
  Serial.printf("[%lus] usb mic %s (backlog %lu ms)\n", (unsigned long)(millis() / 1000), was_open ? "opened" : "closed",
                (unsigned long)((ring_w - ring_r) * 1000 / MIC_RATE));
}

static void ring_put(const int16_t* pcm, uint32_t n) {   // pcm nullptr = silence
  uint32_t w = ring_w;
  if (w - ring_r + n > RING_LEN) return;                  // full: drop (usb_mic_task trims while closed)
  for (uint32_t i = 0; i < n; i++) ring[(w + i) & (RING_LEN - 1)] = pcm ? pcm[i] : 0;
  ring_w = w + n;
}

static void on_audio(const vb_audio_t* a) {
  static uint8_t stream = 0;
  if ((a->flags & VB_AUDIO_START) || a->stream != stream) {   // a new dictation, even if its first frame was lost
    stream = a->stream;
    flush_pos = ring_w.load();
    ring_flush = true;
    audio_seq = 0;
    audio_frames = audio_lost = 0;
  }
  if (audio_seq && a->seq <= audio_seq) return;
  uint32_t gap = audio_seq ? a->seq - audio_seq - 1 : 0;
  if (gap <= 10) {
    for (uint32_t i = 0; i < gap; i++) ring_put(nullptr, VB_AUDIO_FRAME);   // lost frames -> silence
    audio_lost += gap;
  }
  ring_put(a->pcm, VB_AUDIO_FRAME);
  audio_seq = a->seq;
  last_audio_ms = millis();
  audio_frames++;
}

static void usb_mic_task(void*) {
  static int16_t chunk[FIFO_TARGET];
  bool primed = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2));
    uint32_t r = ring_r, w = ring_w;
    if (ring_flush.exchange(false)) { r = flush_pos; primed = false; }
    if (!uac_open) {
      if (w - r > MAX_BACKLOG) r = w - MAX_BACKLOG;
      ring_r = r;
      primed = false;
      continue;
    }
    tu_fifo_t* ff = tud_audio_get_ep_in_ff();
    uint32_t queued = ff ? tu_fifo_count(ff) / sizeof(int16_t) : 0;
    if (queued < FIFO_TARGET) {
      uint32_t need = FIFO_TARGET - queued, avail = w - r;
      if (!primed && avail >= JITTER) primed = true;
      if (primed && avail == 0) primed = false;             // underrun: rebuild the jitter buffer
      uint32_t n = primed ? min(avail, need) : 0;
      for (uint32_t i = 0; i < n; i++) chunk[i] = ring[(r + i) & (RING_LEN - 1)];
      memset(chunk + n, 0, (need - n) * sizeof(int16_t));
      r += n;
      uac.write(chunk, need * sizeof(int16_t));
    }
    ring_r = r;
  }
}

// ---------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------
static bool session_ok = false;
static uint8_t session_key[32];
static vb_replay_t rx_replay;
// A LINK_REQ only proposes a session. Its key becomes the session key once the handheld seals a
// packet with it, so a replayed LINK_REQ (authentic, but old) changes nothing. Two are kept in case
// a LINK_ACK got lost and the handheld asked again.
static uint8_t pending_key[2][32];
static uint8_t pending_n = 0;
static uint32_t tx_ctr = 0;
static uint32_t last_need_link_ms = 0;

static uint32_t last_ping_ms = 0;
static uint32_t remote_seen_ms = 0;     // last new session or PING (mic presence)
static uint8_t remote_battery = 255, remote_flags = 0;
static bool remote_asleep = false;      // the handheld said it is sleeping; it reconnects on a press
static uint8_t last_key_id = 0;
static uint32_t last_key_ms = 0, key_count = 0;

static bool linked() { return paired() && session_ok && millis() - last_ping_ms < VB_LINK_TIMEOUT_MS; }

static void forget_remote() {
  Serial.printf("forgetting %s\n", remote_id);
  if (paired()) vb_peer_del(rec.mac);
  memset(&rec, 0, sizeof(rec));
  rec_save();
  session_ok = false;
  pending_n = 0;
  pairing_mode_until = 0;
  choose_channel();
  apply_radio_mode();
}

static void on_link_req(const vb_link_req_t* r) {
  uint8_t tag[VB_TAG_LEN];
  vb_link_tag(rec.mac_key, r, sizeof(*r) - VB_TAG_LEN, tag);
  if (!vb_ct_equal(tag, r->tag, VB_TAG_LEN)) return;
  vb_link_ack_t a = { vb_hdr(VB_PKT_LINK_ACK), {}, {}, cur_channel, {} };
  memcpy(a.nonce_h, r->nonce_h, 16);
  esp_fill_random(a.nonce_d, 16);
  vb_link_tag(rec.mac_key, &a, sizeof(a) - VB_TAG_LEN, a.tag);
  if (pending_n == 2) { memcpy(pending_key[0], pending_key[1], 32); pending_n = 1; }
  vb_session_key(rec.mac_key, a.nonce_h, a.nonce_d, pending_key[pending_n++]);
  send(rec.mac, &a, sizeof(a));
}

// A packet sealed with a pending key starts that session. Returns whether one did.
static bool session_promote(const void* pkt, size_t len) {
  for (int i = pending_n - 1; i >= 0; i--) {   // newest first
    if (!vb_open(pkt, len, pending_key[i])) continue;
    memcpy(session_key, pending_key[i], 32);
    pending_n = 0;
    rx_replay = {0, 0};
    session_ok = true;
    remote_asleep = false;
    remote_seen_ms = millis();
    Serial.println("session: new");
    return true;
  }
  return false;
}

// Tap a combo: modifiers first, then key, release in reverse. Always ends with all keys up.
// If the Mac has suspended the bus (asleep), wake it first.
static void tap(uint8_t mods, uint8_t keycode, uint8_t hold_ms) {
  if (!tud_mounted()) { Serial.println("tap: no USB"); return; }
  if (tud_suspended()) {
    bool ok = tud_remote_wakeup();
    uint32_t t0 = millis();
    while (tud_suspended() && millis() - t0 < 1500) delay(5);
    Serial.printf("tap: remote wakeup %s, %s after %lums\n", ok ? "sent" : "refused",
                  tud_suspended() ? "still suspended" : "resumed", (unsigned long)(millis() - t0));
    if (tud_suspended()) return;
  }
  KeyReport r = {};
  if (mods) { r.modifiers = mods; Keyboard.sendReport(&r); delay(8); }
  r.keys[0] = keycode;
  Keyboard.sendReport(&r);
  delay(hold_ms ? hold_ms : 30);
  r.keys[0] = 0;
  Keyboard.sendReport(&r);
  if (mods) { delay(8); r.modifiers = 0; Keyboard.sendReport(&r); }
}

// VoiceInk's state, read from the mic: trusted once the Mac opens it, with or without a start tap,
// so a recording started on the Mac reaches the handheld. Another app opening the mic looks the same.
static bool mic_follows_voiceink = false;
static uint32_t start_tap_ms = 0;       // start tap still waiting for the mic to open

static uint8_t rec_state() { return !mic_follows_voiceink ? VB_REC_UNKNOWN : uac_open ? VB_REC_ON : VB_REC_IDLE; }

static void voiceink_update() {
  if (uac_open) {
    if (!mic_follows_voiceink) Serial.println("VoiceInk records from this mic: tracking its state");
    mic_follows_voiceink = true;
    start_tap_ms = 0;
  } else if (start_tap_ms && millis() - start_tap_ms > 2000) {   // before the handheld trusts the state (2.5 s)
    if (mic_follows_voiceink) Serial.println("VoiceInk did not open this mic: state unknown");
    mic_follows_voiceink = false;
    start_tap_ms = 0;
  }
}

// Audio still on its way to the Mac: the dictation stop key waits for it (at most STOP_HOLD_MAX_MS,
// a little more than MAX_BACKLOG).
static constexpr uint32_t STOP_HOLD_MAX_MS = 3500;
static struct { bool pending; uint8_t mods, keycode, hold_ms; uint32_t since; } held_stop;

static bool audio_backlog() {
  return uac_open && (ring_w != ring_r || millis() - last_audio_ms < 120);
}

static void stop_update(bool force = false) {
  if (!held_stop.pending) return;
  uint32_t waited = millis() - held_stop.since;
  if (!force && audio_backlog() && waited < STOP_HOLD_MAX_MS) return;
  held_stop.pending = false;
  Serial.printf("stop key after %lums of audio backlog; %lu frames, %lu lost\n", (unsigned long)waited,
                (unsigned long)audio_frames, (unsigned long)audio_lost);
  tap(held_stop.mods, held_stop.keycode, held_stop.hold_ms);
}

// ---------------------------------------------------------------------------------
// Mic presence (docs/PRD-mic-presence.md). VoiceInk Button Mic is on the Mac only while the handheld
// can stream to it; otherwise VoiceInk (input: Prioritized) records from the next mic in its list.
// Adding or removing it is a USB reconnect with another configuration descriptor (see
// __wrap_tud_descriptor_configuration_cb). Keys that arrive meanwhile wait in key_q.
// ---------------------------------------------------------------------------------
static constexpr uint32_t MIC_LOST_GRACE_MS = 5000;   // link lost without a SLEEP: keep the mic this long
static constexpr uint32_t MIC_SWITCH_GAP_MS = 2000;   // at most one switch per this
static constexpr uint32_t MIC_RECONNECT_MS = 300;     // off the bus, so the Mac sees an unplug
static constexpr uint32_t MIC_SETTLE_MS = 700;        // mounted -> VoiceInk can find the mic
static constexpr uint32_t KEY_HOLD_MAX_MS = 4000;     // a held key is tapped anyway after this
static uint32_t mic_switch_ms = 0, reconnect_at = 0;

static bool mic_wanted() {
  return uac_open || (session_ok && millis() - remote_seen_ms < MIC_LOST_GRACE_MS);
}

static void mic_presence_update() {
  uint32_t now = millis();
  if (reconnect_at) {
    if ((int32_t)(now - reconnect_at) >= 0) { reconnect_at = 0; tud_connect(); }
    return;
  }
  if (usb_switching) {
    if ((int32_t)(usb_mounted_ms - mic_switch_ms) > 0 || now - mic_switch_ms > 5000) {
      usb_switching = false;
      Serial.printf("[%lus] usb: back %s the mic after %lums\n", (unsigned long)(now / 1000),
                    mic_present ? "with" : "without", (unsigned long)(usb_mounted_ms - mic_switch_ms));
    }
    return;
  }
  bool want = mic_wanted();
  if (want == mic_present || restarting || now - mic_switch_ms < MIC_SWITCH_GAP_MS) return;
  if (tud_suspended()) return;            // Mac asleep: switch once it wakes
  mic_present = want;
  mic_switch_ms = now;
  if (!want) mic_follows_voiceink = false;   // VoiceInk may record from another mic from now on
  Serial.printf("[%lus] usb: %s the mic\n", (unsigned long)(now / 1000), want ? "adding" : "removing");
  usb_switching = true;
  tud_disconnect();
  reconnect_at = now + MIC_RECONNECT_MS;
}

static key_cmd_t key_q[4];
static uint8_t key_q_n = 0;

// USB up, and for a dictation start the mic present long enough for VoiceInk to pick it.
static bool key_ready(const key_cmd_t& k) {
  if (usb_switching || reconnect_at || !tud_mounted() || tud_suspended()) return false;
  if (k.key_id != VB_KEYID_DICTATE) return true;
  return mic_present && millis() - usb_mounted_ms >= MIC_SETTLE_MS;
}

static void process_key(const key_cmd_t& k) {
  stop_update(true);                    // keep key order
  bool want_on = k.key_id == VB_KEYID_DICTATE, want_off = k.key_id == VB_KEYID_DICTATE_STOP;
  if ((want_on || want_off) && mic_follows_voiceink && uac_open == want_on) {
    Serial.printf("VoiceInk already %s, no tap\n", want_on ? "recording" : "stopped");
  } else if (want_off && audio_backlog()) {
    held_stop = { true, k.mods, k.keycode, k.hold_ms, millis() };
  } else {
    tap(k.mods, k.keycode, k.hold_ms);
    if (want_on) start_tap_ms = millis();
  }
}

static void key_queue_update() {
  static uint32_t last_wake_ms = 0;
  while (key_q_n) {
    const key_cmd_t& k = key_q[0];
    uint32_t waited = millis() - k.since;
    if (!key_ready(k) && waited < KEY_HOLD_MAX_MS) {
      if (tud_suspended() && millis() - last_wake_ms > 1000) {   // Mac asleep: wake it for the switch
        last_wake_ms = millis();
        tud_remote_wakeup();
      }
      return;
    }
    if (waited > 20) Serial.printf("key id=%u held %lums for USB/mic%s\n", k.key_id, (unsigned long)waited,
                                   key_ready(k) ? "" : ", giving up");
    key_cmd_t c = k;
    memmove(key_q, key_q + 1, --key_q_n * sizeof(key_cmd_t));
    process_key(c);
  }
}

// Verify a session packet; on failure tell the handheld to set up a new session.
static bool session_open(const rx_msg_t& m) {
  if ((session_ok && vb_open(m.data, m.len, session_key)) || session_promote(m.data, m.len)) {
    uint32_t ctr;
    memcpy(&ctr, m.data + sizeof(vb_hdr_t), 4);
    return vb_replay_accept(&rx_replay, ctr);   // false = duplicate / replay, drop silently
  }
  if (millis() - last_need_link_ms > 250) {
    last_need_link_ms = millis();
    vb_need_link_t n = { vb_hdr(VB_PKT_NEED_LINK) };
    send(rec.mac, &n, sizeof(n));
  }
  return false;
}

static void handle_audio(const audio_msg_t& m) {
  if (!paired() || discoverable() || memcmp(m.mac, rec.mac, 6) != 0) return;
  if (!(session_ok && vb_open(&m.pkt, sizeof(m.pkt), session_key)) && !session_promote(&m.pkt, sizeof(m.pkt))) return;
  if (!vb_replay_accept(&rx_replay, m.pkt.ctr)) return;
  on_audio(&m.pkt);
}

static void on_session_packet(const rx_msg_t& m, uint8_t type) {
  if (!session_open(m)) return;
  if (type == VB_PKT_PING && m.len == sizeof(vb_ping_t)) {
    const vb_ping_t* p = (const vb_ping_t*)m.data;
    last_ping_ms = remote_seen_ms = millis();
    remote_battery = p->battery;
    remote_flags = p->flags;
    vb_pong_t pong = { vb_hdr(VB_PKT_PONG), ++tx_ctr, powering_off ? (uint8_t)VB_USB_POWERING_OFF : usb_state, rec_state(), {} };
    vb_seal(&pong, sizeof(pong), session_key);
    send(rec.mac, &pong, sizeof(pong));
  } else if (type == VB_PKT_KEY && m.len == sizeof(vb_key_t)) {
    const vb_key_t* k = (const vb_key_t*)m.data;
    uint8_t mods, keycode;
    if (!vb_key_combo(k->key_id, &mods, &keycode)) { Serial.printf("key id=%u unknown, ignored\n", k->key_id); return; }
    last_key_id = k->key_id; last_key_ms = millis(); key_count++;
    Serial.printf("key id=%u mods=0x%02x code=0x%02x usb=%u\n", k->key_id, mods, keycode, usb_state);
    if (key_q_n < sizeof(key_q) / sizeof(key_q[0]))
      key_q[key_q_n++] = { k->key_id, mods, keycode, KEY_HOLD_MS, millis() };
    else
      Serial.println("key queue full, dropped");
    key_queue_update();
  } else if (type == VB_PKT_UNPAIR && m.len == sizeof(vb_unpair_t)) {
    forget_remote();
  } else if (type == VB_PKT_RESTART && m.len == sizeof(vb_restart_t)) {
    Serial.println("restart requested by remote");
    restart_dongle();
  } else if (type == VB_PKT_SLEEP && m.len == sizeof(vb_sleep_t)) {
    Serial.printf("[%lus] remote asleep\n", (unsigned long)(millis() / 1000));
    remote_asleep = true;
    session_ok = false;
    pending_n = 0;
  }
}

// ---------------------------------------------------------------------------------
// Radio plumbing
// ---------------------------------------------------------------------------------
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len == (int)sizeof(vb_audio_t) && vb_type(data, len) == VB_PKT_AUDIO) {
    static audio_msg_t am;               // Wi-Fi task only
    memcpy(am.mac, info->src_addr, 6);
    memcpy(&am.pkt, data, len);
    xQueueSend(audio_queue, &am, 0);
    return;
  }
  if (len < (int)sizeof(vb_hdr_t) || len > (int)sizeof(rx_msg_t::data)) return;
  rx_msg_t m;
  memcpy(m.mac, info->src_addr, 6);
  m.len = len;
  memcpy(m.data, data, len);
  xQueueSend(rx_queue, &m, 0);
}

static void on_sent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  if (bp == BP_CODE && memcmp(info->des_addr, pr.peer, 6) == 0)
    confirm_cb = status == ESP_NOW_SEND_SUCCESS ? 1 : 2;
}

static void handle(const rx_msg_t& m) {
  uint8_t type = vb_type(m.data, m.len);
  bool from_pair_peer = pair_active() && memcmp(m.mac, pr.peer, 6) == 0;
  bool from_remote = paired() && memcmp(m.mac, rec.mac, 6) == 0;

  switch (type) {
    case VB_PKT_DISCOVER:
      if (discoverable() && vb_valid(m.data, m.len, type, sizeof(vb_discover_t)) && !pair_active())
        advertise_at = millis() + (esp_random() % 50) + 1;
      break;
    case VB_PKT_IDENTIFY:
      if (discoverable() && vb_valid(m.data, m.len, type, sizeof(vb_identify_t))) last_identify_ms = millis();
      break;
    case VB_PKT_PAIR_HELLO:
      if (discoverable() && vb_valid(m.data, m.len, type, sizeof(vb_pair_hello_t)))
        on_pair_hello(m.mac, (const vb_pair_hello_t*)m.data);
      break;
    case VB_PKT_PAIR_NONCE_A:
      if (from_pair_peer && vb_valid(m.data, m.len, type, sizeof(vb_pair_nonce_t)))
        on_pair_nonce_a((const vb_pair_nonce_t*)m.data);
      break;
    case VB_PKT_PAIR_CONFIRM:
      if (from_pair_peer && vb_valid(m.data, m.len, type, sizeof(vb_pair_confirm_t)))
        on_pair_confirm((const vb_pair_confirm_t*)m.data);
      break;
    case VB_PKT_PAIR_ABORT:
      if (from_pair_peer) pair_end(BP_FAILED, "CANCELLED");
      break;
    case VB_PKT_LINK_REQ:
      if (from_remote && !discoverable() && vb_valid(m.data, m.len, type, sizeof(vb_link_req_t)))
        on_link_req((const vb_link_req_t*)m.data);
      break;
    case VB_PKT_PING:
    case VB_PKT_KEY:
    case VB_PKT_UNPAIR:
    case VB_PKT_RESTART:
    case VB_PKT_SLEEP:
      if (from_remote && !discoverable()) on_session_packet(m, type);
      break;
  }
}

// State is polled in usb_poll(); events are only logged, to see what the Mac does.
static void on_usb_event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base != ARDUINO_USB_EVENTS) return;
  if (id == ARDUINO_USB_STARTED_EVENT) usb_mounted_ms = millis();
  static const char* names[] = {"plugged", "unplugged", "suspend", "resume"};
  const char* n = id == ARDUINO_USB_STARTED_EVENT ? names[0] : id == ARDUINO_USB_STOPPED_EVENT ? names[1]
                : id == ARDUINO_USB_SUSPEND_EVENT ? names[2] : id == ARDUINO_USB_RESUME_EVENT ? names[3] : "?";
  bool wake_ok = id == ARDUINO_USB_SUSPEND_EVENT && ((arduino_usb_event_data_t*)data)->suspend.remote_wakeup_en;
  Serial.printf("[%lus] usb: %s%s\n", (unsigned long)(millis() / 1000), n, wake_ok ? " (remote wakeup allowed)" : "");
}

// ---------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------
static void buttons_update() {
  if (bp == BP_CODE && !pr.accepted) {
    if (M5.BtnA.wasClicked()) { pr.accepted = true; pair_send_confirm(); }
    else if (M5.BtnB.wasClicked()) pair_abort(VB_ABORT_REJECTED, "REJECTED");
    return;
  }
  if (pair_active()) return;

  if (M5.BtnA.wasClicked() && usb_missing()) {
    restart_dongle();
  } else if (M5.BtnA.wasHold() && paired()) {
    pairing_mode_until = millis() + PAIRING_MODE_MS;
    session_ok = false;
    pending_n = 0;
    choose_channel();
    apply_radio_mode();
  } else if (M5.BtnB.wasClicked() && paired() && pairing_mode_until) {
    pairing_mode_until = 0;                         // leave pairing mode early
    apply_radio_mode();
  } else if (M5.BtnB.wasHold() && paired()) {
    forget_remote();
  }
}

// ---------------------------------------------------------------------------------
// Display (240x135)
// ---------------------------------------------------------------------------------
static void draw_header(uint16_t right_col, const char* right) {
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(C_MUTED);
  canvas.drawString(my_id, 8, 6);
  canvas.setTextDatum(top_right);
  canvas.setTextColor(right_col);
  canvas.drawString(right, canvas.width() - 8, 6);
}

static void draw_center(const char* title, uint16_t col, const char* sub, const char* sub2 = nullptr) {
  int w = canvas.width();
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(col);
  canvas.drawString(title, w / 2, 58);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(C_MUTED);
  if (sub) canvas.drawString(sub, w / 2, 96);
  if (sub2) canvas.drawString(sub2, w / 2, 118);
}

static void draw_status(const char* title, const char* sub) {
  canvas.fillScreen(C_BG);
  draw_header(C_MUTED, "");
  draw_center(title, C_TEXT, sub);
  canvas.pushSprite(0, 0);
}

static void render() {
  uint32_t now = millis();
  int w = canvas.width();
  char buf[48];
  canvas.fillScreen(C_BG);

  if (now - last_identify_ms < 700 && last_identify_ms) {
    bool on = (now / 200) & 1;
    canvas.fillScreen(on ? C_BLUE : C_BG);
    draw_center("THIS ONE", C_TEXT, my_id);
    canvas.pushSprite(0, 0);
    return;
  }

  const char* usb = usb_state == VB_USB_READY ? "USB OK" : usb_state == VB_USB_SUSPENDED ? "MAC ASLEEP" : "NO USB";
  uint16_t usb_col = usb_state == VB_USB_READY ? C_GREEN : usb_state == VB_USB_SUSPENDED ? C_MUTED : C_AMBER;

  if (restarting) {
    draw_header(usb_col, usb);
    draw_center("RESTARTING", C_TEXT, "");
    canvas.pushSprite(0, 0);
    return;
  }

  if (bp == BP_COMMITTED) {
    draw_header(usb_col, usb);
    snprintf(buf, sizeof(buf), "from %s", pr.peer_id);
    draw_center("PAIRING", C_TEXT, buf);
  } else if (bp == BP_CODE) {
    snprintf(buf, sizeof(buf), "Pair with %s?", pr.peer_id);
    canvas.setTextDatum(top_center);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    canvas.drawString(buf, w / 2, 8);
    snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)(pr.code / 1000), (unsigned long)(pr.code % 1000));
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(buf, w / 2, 62);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    if (!pr.accepted) {
      canvas.setTextColor(C_GREEN);
      canvas.drawString("A: YES", w / 4, 116);
      canvas.setTextColor(C_RED);
      canvas.drawString("B: NO", w * 3 / 4, 116);
    } else {
      canvas.setTextColor(C_MUTED);
      canvas.drawString(pr.peer_confirmed ? "Finishing..." : "Accept on the remote", w / 2, 116);
    }
  } else if (bp == BP_DONE || bp == BP_FAILED) {
    draw_header(usb_col, usb);
    draw_center(pr.result, bp == BP_DONE ? C_GREEN : C_RED, pr.peer_id);
  } else if (usb_missing()) {
    draw_header(usb_col, usb);
    draw_center("NO USB", C_AMBER, "Press A to restart,", "or blue on the remote");
  } else if (discoverable()) {
    draw_header(usb_col, usb);
    if (paired()) {
      uint32_t left = (pairing_mode_until - now) / 1000;
      snprintf(buf, sizeof(buf), "PAIRING %lu", (unsigned long)left);
      draw_center(buf, C_AMBER, "On the remote: swipe down,", "ADD DONGLE.  B = exit");
    } else {
      draw_center("READY", C_TEXT, "On the remote: swipe down,", "then ADD DONGLE");
    }
  } else {
    draw_header(usb_col, usb);
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.setTextDatum(middle_left);
    bool is_linked = linked();
    bool dictating = is_linked && (remote_flags & VB_FLAG_DICTATING);
    if (dictating) {
      bool on = (now / 500) & 1;
      canvas.fillSmoothCircle(20, 56, 9, on ? C_RED : RGB(90, 20, 16));
      canvas.setTextColor(C_RED);
      canvas.drawString("LISTENING", 38, 57);
    } else if (is_linked && (remote_flags & VB_FLAG_DIMMED)) {
      canvas.fillSmoothCircle(20, 56, 9, RGB(20, 90, 40));
      canvas.setTextColor(C_MUTED);
      canvas.drawString("DIMMED", 38, 57);
    } else if (is_linked) {
      canvas.fillSmoothCircle(20, 56, 9, C_GREEN);
      canvas.setTextColor(C_TEXT);
      canvas.drawString("READY", 38, 57);
    } else if (remote_asleep) {
      canvas.fillSmoothCircle(20, 56, 9, C_MUTED);
      canvas.fillSmoothCircle(25, 52, 8, C_BG);     // crescent
      canvas.setTextColor(C_MUTED);
      canvas.drawString("ASLEEP", 38, 57);
    } else {
      static const char* dots[] = {"SEARCH", "SEARCH.", "SEARCH..", "SEARCH..."};
      canvas.drawCircle(20, 56, 9, C_AMBER);
      canvas.setTextColor(C_AMBER);
      canvas.drawString(dots[(now / 400) % 4], 38, 57);
    }

    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_MUTED);
    if ((is_linked || remote_asleep) && remote_battery <= 100)
      snprintf(buf, sizeof(buf), "%s  %u%%%s", remote_id, remote_battery,
               (remote_flags & VB_FLAG_CHARGING) ? "  charging" : "");
    else
      snprintf(buf, sizeof(buf), "%s  --", remote_id);
    canvas.drawString(buf, 8, 96);

    if (last_key_ms) {
      bool fresh = now - last_key_ms < 800;
      canvas.setTextColor(fresh ? (last_key_id == VB_KEYID_ENTER ? C_BLUE : C_TEXT) : C_MUTED);
      snprintf(buf, sizeof(buf), "Sent  %s  (%lu)",
               last_key_id == VB_KEYID_ENTER ? "ENTER" : "DICTATE", (unsigned long)key_count);
      canvas.drawString(buf, 8, 120);
    }
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(RGB(72, 72, 74));
    bool switched = mic_switch_ms && now - mic_switch_ms < 2000;   // "MIC ON" / "MIC OFF" briefly
    if (switched) canvas.setTextColor(C_MUTED);
    if (switched) snprintf(buf, sizeof(buf), mic_present ? "MIC ON" : "MIC OFF");
    else snprintf(buf, sizeof(buf), uac_open ? "MIC  ch %u" : "ch %u", cur_channel);
    canvas.drawString(buf, w - 8, 120);
  }
  // why did we last reset? (first 15 s after boot)
  if (now < 15000) {
    char diag[40];
    vb_diag_summary(diag, sizeof(diag));
    canvas.fillRect(0, canvas.height() - 18, w, 18, RGB(28, 28, 30));
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_MUTED);
    canvas.drawString(diag, w / 2, canvas.height() - 9);
  }
  canvas.pushSprite(0, 0);
}

// 'v' on the serial port: power source as the PM1 sees it
static void on_serial_char(char c) {
  if (c == 'v') {
    Serial.printf("PWR_SRC 0x%02x (bit0 = 5 V in), VBUS %d mV, USB state %u\n",
                  M5.Power.M5pm1.readRegister8(0x04), M5.Power.getVBUSVoltage(), usb_state);
    Serial.printf("mic %s (wanted %d, switching %d), session %d, remote seen %lums ago, mounted %lums ago\n",
                  mic_present ? "present" : "absent", mic_wanted(), usb_switching, session_ok,
                  (unsigned long)(millis() - remote_seen_ms), (unsigned long)(millis() - usb_mounted_ms));
  }
}

// ---------------------------------------------------------------------------------
// The core names the audio interface "TinyUSB UAC1". The Makefile links with
// -Wl,--wrap=tinyusb_add_string_descriptor so that one string can be swapped for ours.
extern "C" uint8_t __real_tinyusb_add_string_descriptor(const char* str);
extern "C" uint8_t __wrap_tinyusb_add_string_descriptor(const char* str) {
  if (str && strncmp(str, "TinyUSB UAC", 11) == 0) str = "VoiceInk Button Mic";
  return __real_tinyusb_add_string_descriptor(str);
}

// Without the mic, the Mac gets the core's configuration descriptor minus the audio function
// (-Wl,--wrap=tud_descriptor_configuration_cb). The core puts audio after HID and CDC, so their
// interface numbers stay the same, and the mic keeps its number (and macOS UID) when it returns.
static uint8_t* strip_audio(const uint8_t* full) {
  uint16_t total = full[2] | (full[3] << 8);
  uint8_t* out = (uint8_t*)malloc(total);
  if (!out) return nullptr;
  uint16_t n = 0;
  uint32_t audio_itfs = 0;
  bool in_audio = false;
  for (uint16_t i = 0; i + 1 < total && full[i] >= 2; i += full[i]) {
    const uint8_t* d = full + i;
    if (d[1] == TUSB_DESC_INTERFACE_ASSOCIATION) in_audio = d[4] == TUSB_CLASS_AUDIO;
    else if (d[1] == TUSB_DESC_INTERFACE) {
      in_audio = d[5] == TUSB_CLASS_AUDIO;
      if (in_audio) audio_itfs |= 1u << d[2];
    }
    if (i == 0 || !in_audio) { memcpy(out + n, d, d[0]); n += d[0]; }
  }
  out[2] = n & 0xFF; out[3] = n >> 8;
  out[4] -= __builtin_popcount(audio_itfs);
  return out;
}

extern "C" const uint8_t* __real_tud_descriptor_configuration_cb(uint8_t index);
extern "C" const uint8_t* __wrap_tud_descriptor_configuration_cb(uint8_t index) {
  static uint8_t* without_mic = nullptr;
  const uint8_t* full = __real_tud_descriptor_configuration_cb(index);
  if (mic_present || !full) return full;
  if (!without_mic) without_mic = strip_audio(full);
  return without_mic ? without_mic : full;
}

// ---------------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(80);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  M5.BtnA.setHoldThresh(3000);
  M5.BtnB.setHoldThresh(6000);

  rx_queue = xQueueCreate(16, sizeof(rx_msg_t));
  audio_queue = xQueueCreate(12, sizeof(audio_msg_t));

  USB.onEvent(on_usb_event);
  USB.productName("VoiceInk Button Dongle");
  USB.manufacturerName("VoiceInk Button");
  USB.usbAttributes(TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);
  Keyboard.begin();
  ring = (int16_t*)heap_caps_malloc(RING_LEN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!ring) {   // the board has PSRAM; a build without it can't buffer audio
    draw_status("NO PSRAM", "build with PSRAM enabled");
    for (;;) delay(1000);
  }
  uac.onEvent(ARDUINO_USB_AUDIO_CARD_ANY_EVENT, on_uac_event);
  uac.begin();
  xTaskCreatePinnedToCore(usb_mic_task, "usb_mic", 3072, nullptr, 5, nullptr, 1);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on logs when nobody reads the port
  USB.begin();
  usb_lost_since = millis();
  vb_diag_begin();
  vb_diag_on_char = on_serial_char;

  prefs.begin("vb_pair", false);
  rec_load();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
  vb_id_str(my_mac, my_id);
  if (esp_now_init() != ESP_OK) Serial.println("esp_now_init failed");
  esp_now_set_pmk(VB_PMK);
  esp_now_register_recv_cb(on_recv);
  esp_now_register_send_cb(on_sent);
  vb_peer_set(BROADCAST, nullptr);
  if (!paired()) choose_channel();
  apply_radio_mode();
  Serial.printf("VoiceInk Button dongle %s up, %s, PWR_SRC 0x%02x\n", my_id, paired() ? remote_id : "unpaired",
                M5.Power.M5pm1.readRegister8(0x04));
  vb_diag_print();
}

void loop() {
  M5.update();
  usb_poll();
  power_update();
  usb_mic_log();
  rx_msg_t m;
  while (xQueueReceive(rx_queue, &m, 0) == pdTRUE) handle(m);
  static audio_msg_t am;
  while (xQueueReceive(audio_queue, &am, 0) == pdTRUE) handle_audio(am);
  stop_update();
  voiceink_update();
  mic_presence_update();
  key_queue_update();

  buttons_update();
  pair_update();
  vb_diag_update();

  uint32_t now = millis();
  if (advertise_at && (int32_t)(now - advertise_at) >= 0) {
    advertise_at = 0;
    vb_advertise_t a = { vb_hdr(VB_PKT_ADVERTISE), (uint8_t)(paired() ? VB_ADV_PAIRED : 0), {0, 0} };
    if (paired()) { a.paired_to[0] = rec.mac[4]; a.paired_to[1] = rec.mac[5]; }
    send(BROADCAST, &a, sizeof(a));
  }
  if (pairing_mode_until && !pair_active() && (int32_t)(now - pairing_mode_until) >= 0) {
    pairing_mode_until = 0;
    apply_radio_mode();
  }

  static uint32_t last_frame = 0;
  if (now - last_frame >= 100) { last_frame = now; render(); }
  delay(2);
}
