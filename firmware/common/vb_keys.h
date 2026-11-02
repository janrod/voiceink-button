// The keys VoiceInk Button types, shared by both devices. The dongle types only these: the handheld
// says which one (key_id), never what to type, so a paired remote can't type anything else.
// Changing them means reflashing both devices.
#pragma once
#include "vb_protocol.h"

// ---- Dictation shortcut (yellow button) ------------------------------------------
// Must match VoiceInk's Primary Shortcut (toggle mode). Currently: Option + /
// Combine modifiers with |, e.g. (VB_MOD_LGUI | VB_MOD_LSHIFT).
#define DICTATE_MODIFIERS  VB_MOD_LALT
#define DICTATE_KEY        VB_KEY_SLASH
#define DICTATE_LABEL      "OPT /"   // shown on the handheld

// ---- Send key (blue button) --------------------------------------------------------
#define SEND_MODIFIERS     0
#define SEND_KEY           VB_KEY_ENTER

#define KEY_HOLD_MS        40    // how long the dongle holds each combo down

// The combo for a key_id; false for an unknown one.
static inline bool vb_key_combo(uint8_t key_id, uint8_t* mods, uint8_t* keycode) {
  switch (key_id) {
    case VB_KEYID_DICTATE:
    case VB_KEYID_DICTATE_STOP: *mods = DICTATE_MODIFIERS; *keycode = DICTATE_KEY; return true;
    case VB_KEYID_ENTER:        *mods = SEND_MODIFIERS;    *keycode = SEND_KEY;    return true;
    default: return false;
  }
}
