// VoiceInk Button ESP-NOW protocol v2, shared by the handheld (StopWatch) and the dongle (StickS3).
//
// The handheld says which key to press (key_id); the dongle types its own combo for it
// (vb_keys.h), so a paired remote can only ever press the shortcut and Enter.
//
// Three layers (see docs/PRD-pairing.md):
//   discovery + pairing  plaintext on VB_PAIR_CHANNEL; X25519 + commitment + 6-digit comparison
//   link setup           LINK_REQ/LINK_ACK, authenticated with the long-term pairing key,
//                        exchange fresh nonces -> per-session key
//   session              PING/PONG/KEY/AUDIO/...: counter + HMAC tag with the session key,
//                        on an ESP-NOW encrypted peer (LMK from pairing)
#pragma once
#include <stdint.h>

#define VB_MAGIC        0x56B1
#define VB_VERSION      2
#define VB_PAIR_CHANNEL 1          // discovery and pairing always happen here

// Link timing
#define VB_PING_INTERVAL_MS  300   // handheld -> dongle heartbeat
#define VB_LINK_TIMEOUT_MS   1200  // no heartbeat/pong for this long = link lost
#define VB_PAIR_TIMEOUT_MS   30000

// HID modifier bits (USB HID boot keyboard report, byte 0)
#define VB_MOD_LCTRL  0x01
#define VB_MOD_LSHIFT 0x02
#define VB_MOD_LALT   0x04   // Option on macOS
#define VB_MOD_LGUI   0x08   // Command on macOS
#define VB_MOD_RCTRL  0x10
#define VB_MOD_RSHIFT 0x20
#define VB_MOD_RALT   0x40
#define VB_MOD_RGUI   0x80

// HID usage IDs (keyboard page 0x07). Positional: VB_KEY_SLASH is the ANSI "/" key,
// which is macOS virtual keyCode 44 regardless of the active layout.
#define VB_KEY_A      0x04   // A..Z = 0x04..0x1D
#define VB_KEY_1      0x1E   // 1..9 = 0x1E..0x26, 0 = 0x27
#define VB_KEY_ENTER  0x28
#define VB_KEY_ESC    0x29
#define VB_KEY_TAB    0x2B
#define VB_KEY_SPACE  0x2C
#define VB_KEY_SLASH  0x38
#define VB_KEY_F13    0x68   // F13..F24 = 0x68..0x73
#define VB_KEY_F18    0x6D

enum vb_pkt_type_t : uint8_t {
  // discovery (plaintext broadcast / unicast on VB_PAIR_CHANNEL)
  VB_PKT_DISCOVER     = 0x10,  // handheld -> broadcast
  VB_PKT_ADVERTISE    = 0x11,  // dongle   -> broadcast, only while discoverable
  VB_PKT_IDENTIFY     = 0x12,  // handheld -> dongle: flash your screen
  // pairing (plaintext unicast; handheld = A / initiator, dongle = B / responder)
  VB_PKT_PAIR_HELLO   = 0x20,  // A -> B  pubA
  VB_PKT_PAIR_COMMIT  = 0x21,  // B -> A  pubB, SHA-256(pubB | pubA | nB)
  VB_PKT_PAIR_NONCE_A = 0x22,  // A -> B  nA
  VB_PKT_PAIR_NONCE_B = 0x23,  // B -> A  nB
  VB_PKT_PAIR_CONFIRM = 0x24,  // both    user accepted; tag proves same keys
  VB_PKT_PAIR_ABORT   = 0x25,  // both    rejected / failed
  VB_PKT_PAIR_BUSY    = 0x26,  // B -> A  pairing with someone else
  // link setup (tag with long-term mac_key)
  VB_PKT_LINK_REQ     = 0x30,  // handheld -> dongle
  VB_PKT_LINK_ACK     = 0x31,  // dongle   -> handheld
  VB_PKT_NEED_LINK    = 0x32,  // dongle   -> handheld, unauthenticated hint: "no session, ask again"
  // session (ctr + tag with session key)
  VB_PKT_PING         = 0x40,  // handheld -> dongle: heartbeat + status
  VB_PKT_PONG         = 0x41,  // dongle   -> handheld
  VB_PKT_KEY          = 0x42,  // handheld -> dongle: tap a key combo
  VB_PKT_UNPAIR       = 0x43,  // handheld -> dongle: forget me
  VB_PKT_RESTART      = 0x44,  // handheld -> dongle: reboot (re-enumerates USB)
  VB_PKT_SLEEP        = 0x45,  // handheld -> dongle: radio going off until a button wakes it
  VB_PKT_AUDIO        = 0x46,  // handheld -> dongle: 20 ms of mic audio while dictating
};

// PONG.usb_state
enum vb_usb_state_t : uint8_t { VB_USB_NONE = 0, VB_USB_READY = 1, VB_USB_SUSPENDED = 2 };

// PONG.rec_state: VoiceInk opens its input device exactly while it records, so once VoiceInk
// uses the dongle's mic, the dongle knows whether it is recording. UNKNOWN until the Mac opens the
// mic, and again after a start tap that didn't open it (VoiceInk may be using another mic).
enum vb_rec_state_t : uint8_t { VB_REC_UNKNOWN = 0, VB_REC_IDLE = 1, VB_REC_ON = 2 };

// Handheld status flags (PING.flags)
#define VB_FLAG_CHARGING  0x01
#define VB_FLAG_DICTATING 0x02
#define VB_FLAG_DIMMED    0x04   // screen dimmed or off (idle, about to sleep)

// ADVERTISE.flags
#define VB_ADV_PAIRED     0x01   // dongle already has a handheld (it is in pairing mode)

// PAIR_ABORT.reason
enum vb_abort_t : uint8_t { VB_ABORT_REJECTED = 1, VB_ABORT_TIMEOUT = 2, VB_ABORT_MISMATCH = 3 };

// Key ids. DICTATE / DICTATE_STOP mean "recording should be on / off": the dongle taps the
// shortcut only when VoiceInk is not already there (see vb_rec_state_t), and holds the stop
// until the buffered audio has reached the Mac, so the last words are not cut off.
enum vb_key_id_t : uint8_t {
  VB_KEYID_DICTATE      = 1,
  VB_KEYID_ENTER        = 2,
  VB_KEYID_DICTATE_STOP = 3,
};

// Audio: 16 kHz mono 16-bit, 20 ms per packet (ESP-NOW v2 allows up to 1470 bytes)
#define VB_AUDIO_RATE   16000
#define VB_AUDIO_FRAME  320
#define VB_AUDIO_START  0x01     // first frame of a dictation: the dongle drops older audio

#define VB_TAG_LEN 8

struct __attribute__((packed)) vb_hdr_t {
  uint16_t magic;
  uint8_t  version;
  uint8_t  type;
};

// --- discovery ---
struct __attribute__((packed)) vb_discover_t  { vb_hdr_t hdr; };
struct __attribute__((packed)) vb_advertise_t { vb_hdr_t hdr; uint8_t flags; uint8_t paired_to[2]; };
struct __attribute__((packed)) vb_identify_t  { vb_hdr_t hdr; };

// --- pairing ---
struct __attribute__((packed)) vb_pair_hello_t   { vb_hdr_t hdr; uint8_t pub[32]; };
struct __attribute__((packed)) vb_pair_commit_t  { vb_hdr_t hdr; uint8_t pub[32]; uint8_t commit[32]; };
struct __attribute__((packed)) vb_pair_nonce_t   { vb_hdr_t hdr; uint8_t nonce[16]; };
struct __attribute__((packed)) vb_pair_confirm_t { vb_hdr_t hdr; uint8_t channel; uint8_t tag[16]; };
struct __attribute__((packed)) vb_pair_abort_t   { vb_hdr_t hdr; uint8_t reason; };

// --- link setup ---
struct __attribute__((packed)) vb_link_req_t { vb_hdr_t hdr; uint8_t nonce_h[16]; uint8_t tag[VB_TAG_LEN]; };
// channel: the dongle's real channel. Neighbouring channels overlap, so the handheld can hear the
// dongle while tuned one or two channels off; it moves to this one.
struct __attribute__((packed)) vb_link_ack_t { vb_hdr_t hdr; uint8_t nonce_h[16]; uint8_t nonce_d[16]; uint8_t channel; uint8_t tag[VB_TAG_LEN]; };
struct __attribute__((packed)) vb_need_link_t { vb_hdr_t hdr; };

// --- session ---
struct __attribute__((packed)) vb_ping_t {
  vb_hdr_t hdr; uint32_t ctr;
  uint8_t  battery;     // 0-100, 255 = unknown
  uint8_t  flags;
  uint8_t  tag[VB_TAG_LEN];
};
struct __attribute__((packed)) vb_pong_t {
  vb_hdr_t hdr; uint32_t ctr;
  uint8_t  usb_state;   // vb_usb_state_t
  uint8_t  rec_state;   // vb_rec_state_t
  uint8_t  tag[VB_TAG_LEN];
};
struct __attribute__((packed)) vb_key_t {
  vb_hdr_t hdr; uint32_t ctr;   // retransmits reuse ctr; the replay window drops duplicates
  uint8_t  key_id;      // VB_KEYID_*; the dongle types its own combo for it (vb_keys.h)
  uint8_t  modifiers;   // the handheld's combo, for the dongle's log only
  uint8_t  keycode;
  uint8_t  hold_ms;     // unused by the dongle (KEY_HOLD_MS)
  uint8_t  tag[VB_TAG_LEN];
};
struct __attribute__((packed)) vb_unpair_t { vb_hdr_t hdr; uint32_t ctr; uint8_t tag[VB_TAG_LEN]; };
struct __attribute__((packed)) vb_restart_t { vb_hdr_t hdr; uint32_t ctr; uint8_t tag[VB_TAG_LEN]; };
struct __attribute__((packed)) vb_sleep_t { vb_hdr_t hdr; uint32_t ctr; uint8_t tag[VB_TAG_LEN]; };
struct __attribute__((packed)) vb_audio_t {
  vb_hdr_t hdr; uint32_t ctr;
  uint32_t seq;         // frame number within the dictation; gaps are filled with silence
  uint8_t  flags;       // VB_AUDIO_*
  uint8_t  stream;      // changes with each dictation, so a lost START frame still starts a new one
  int16_t  pcm[VB_AUDIO_FRAME];
  uint8_t  tag[VB_TAG_LEN];
};

static inline vb_hdr_t vb_hdr(uint8_t type) {
  vb_hdr_t h = { VB_MAGIC, VB_VERSION, type };
  return h;
}

static inline bool vb_valid(const uint8_t* data, int len, uint8_t type, int size) {
  if (len != size) return false;
  const vb_hdr_t* h = (const vb_hdr_t*)data;
  return h->magic == VB_MAGIC && h->version == VB_VERSION && h->type == type;
}

static inline uint8_t vb_type(const uint8_t* data, int len) {
  if (len < (int)sizeof(vb_hdr_t)) return 0;
  const vb_hdr_t* h = (const vb_hdr_t*)data;
  return (h->magic == VB_MAGIC && h->version == VB_VERSION) ? h->type : 0;
}
