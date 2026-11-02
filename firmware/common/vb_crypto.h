// VoiceInk Button crypto helpers (mbedtls, shipped with the ESP32 core). Header-only, shared by
// both devices so every derivation is byte-identical on each side.
#pragma once
#include <string.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/sha256.h>
#include "vb_protocol.h"

// Global ESP-NOW primary key. It only wraps the per-peer LMK in hardware; the secret part is the
// LMK derived during pairing.
static const uint8_t VB_PMK[16] = {'V','o','i','c','e','B','u','t','t','o','n','P','M','K','v','2'};

static int vb_rng(void*, unsigned char* out, size_t len) { esp_fill_random(out, len); return 0; }

// ---- X25519 -------------------------------------------------------------------------------
static inline bool vb_x25519_keygen(uint8_t priv[32], uint8_t pub[32]) {
  mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q;
  mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);
  size_t olen = 0;
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0
         && mbedtls_ecp_gen_keypair(&grp, &d, &Q, vb_rng, nullptr) == 0
         && mbedtls_mpi_write_binary_le(&d, priv, 32) == 0
         && mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, 32) == 0
         && olen == 32;
  mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
  return ok;
}

static inline bool vb_x25519_shared(const uint8_t priv[32], const uint8_t peer_pub[32], uint8_t out[32]) {
  mbedtls_ecp_group grp; mbedtls_mpi d, z; mbedtls_ecp_point Q;
  mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_mpi_init(&z); mbedtls_ecp_point_init(&Q);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0
         && mbedtls_mpi_read_binary_le(&d, priv, 32) == 0
         && mbedtls_ecp_point_read_binary(&grp, &Q, peer_pub, 32) == 0
         && mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, vb_rng, nullptr) == 0
         && mbedtls_mpi_cmp_int(&z, 0) != 0
         && mbedtls_mpi_write_binary_le(&z, out, 32) == 0;
  mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&z); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
  return ok;
}

// ---- hashes -------------------------------------------------------------------------------
struct vb_part_t { const void* p; size_t n; };

static inline void vb_sha256(const vb_part_t* parts, int count, uint8_t out[32]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  for (int i = 0; i < count; i++) mbedtls_sha256_update(&c, (const uint8_t*)parts[i].p, parts[i].n);
  mbedtls_sha256_finish(&c, out);
  mbedtls_sha256_free(&c);
}

static inline void vb_hmac(const uint8_t* key, size_t klen, const vb_part_t* parts, int count, uint8_t out[32]) {
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&c, key, klen);
  for (int i = 0; i < count; i++) mbedtls_md_hmac_update(&c, (const uint8_t*)parts[i].p, parts[i].n);
  mbedtls_md_hmac_finish(&c, out);
  mbedtls_md_free(&c);
}

static inline bool vb_ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// ---- pairing derivations ------------------------------------------------------------------
struct vb_keys_t { uint8_t lmk[16]; uint8_t mac_key[32]; };

// commitment the responder sends before revealing its nonce
static inline void vb_pair_commit(const uint8_t pubB[32], const uint8_t pubA[32], const uint8_t nB[16], uint8_t out[32]) {
  vb_part_t p[] = {{pubB, 32}, {pubA, 32}, {nB, 16}};
  vb_sha256(p, 3, out);
}

// 6-digit comparison code, identical on both screens iff no one is in the middle
static inline uint32_t vb_pair_code(const uint8_t pubA[32], const uint8_t pubB[32], const uint8_t nA[16], const uint8_t nB[16]) {
  uint8_t h[32];
  vb_part_t p[] = {{pubA, 32}, {pubB, 32}, {nA, 16}, {nB, 16}};
  vb_sha256(p, 4, h);
  uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
  return v % 1000000;
}

static inline bool vb_pair_derive(const uint8_t shared[32], const uint8_t nA[16], const uint8_t nB[16], vb_keys_t* k) {
  uint8_t salt[32], okm[48];
  memcpy(salt, nA, 16); memcpy(salt + 16, nB, 16);
  static const char info[] = "voicebutton-pair-v2";
  if (mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, 32, shared, 32,
                   (const uint8_t*)info, sizeof(info) - 1, okm, sizeof(okm)) != 0) return false;
  memcpy(k->lmk, okm, 16);
  memcpy(k->mac_key, okm + 16, 32);
  memset(okm, 0, sizeof(okm));
  return true;
}

// role: 'A' handheld, 'B' dongle. Binds both public keys and the dongle's chosen channel.
static inline void vb_pair_confirm_tag(const uint8_t mac_key[32], char role, const uint8_t pubA[32],
                                       const uint8_t pubB[32], uint8_t channel, uint8_t out[16]) {
  uint8_t h[32];
  vb_part_t p[] = {{"vb-confirm", 10}, {&role, 1}, {pubA, 32}, {pubB, 32}, {&channel, 1}};
  vb_hmac(mac_key, 32, p, 5, h);
  memcpy(out, h, 16);
}

// ---- link + session -----------------------------------------------------------------------
static inline void vb_link_tag(const uint8_t mac_key[32], const void* pkt, size_t len_without_tag, uint8_t out[VB_TAG_LEN]) {
  uint8_t h[32];
  vb_part_t p[] = {{"vb-link", 7}, {pkt, len_without_tag}};
  vb_hmac(mac_key, 32, p, 2, h);
  memcpy(out, h, VB_TAG_LEN);
}

static inline void vb_session_key(const uint8_t mac_key[32], const uint8_t nonce_h[16], const uint8_t nonce_d[16], uint8_t out[32]) {
  vb_part_t p[] = {{"vb-session", 10}, {nonce_h, 16}, {nonce_d, 16}};
  vb_hmac(mac_key, 32, p, 3, out);
}

// Session packets end with VB_TAG_LEN bytes of HMAC over everything before them.
static inline void vb_seal(void* pkt, size_t len, const uint8_t session_key[32]) {
  uint8_t h[32];
  vb_part_t p[] = {{pkt, len - VB_TAG_LEN}};
  vb_hmac(session_key, 32, p, 1, h);
  memcpy((uint8_t*)pkt + len - VB_TAG_LEN, h, VB_TAG_LEN);
}

static inline bool vb_open(const void* pkt, size_t len, const uint8_t session_key[32]) {
  if (len < sizeof(vb_hdr_t) + 4 + VB_TAG_LEN) return false;   // header, counter, tag
  uint8_t h[32];
  vb_part_t p[] = {{pkt, len - VB_TAG_LEN}};
  vb_hmac(session_key, 32, p, 1, h);
  return vb_ct_equal(h, (const uint8_t*)pkt + len - VB_TAG_LEN, VB_TAG_LEN);
}

// Sliding replay window (32 packets). Call only after the tag verified.
struct vb_replay_t { uint32_t last; uint32_t window; };

static inline bool vb_replay_accept(vb_replay_t* r, uint32_t ctr) {
  if (ctr == 0) return false;
  if (ctr > r->last) {
    uint32_t shift = ctr - r->last;
    r->window = (shift >= 32 ? 0 : r->window << shift) | 1u;
    r->last = ctr;
    return true;
  }
  uint32_t off = r->last - ctr;
  if (off >= 32 || (r->window & (1u << off))) return false;
  r->window |= 1u << off;
  return true;
}

// ---- misc ---------------------------------------------------------------------------------
// "VB-9B64" from the last two MAC bytes
static inline void vb_id_str(const uint8_t mac[6], char out[8]) {
  static const char hex[] = "0123456789ABCDEF";
  out[0] = 'V'; out[1] = 'B'; out[2] = '-';
  out[3] = hex[mac[4] >> 4]; out[4] = hex[mac[4] & 15];
  out[5] = hex[mac[5] >> 4]; out[6] = hex[mac[5] & 15];
  out[7] = 0;
}

// Add or update an ESP-NOW peer. channel 0 = whatever channel the radio is on.
static inline void vb_peer_set(const uint8_t mac[6], const uint8_t* lmk /* nullptr = plaintext */) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = lmk != nullptr;
  if (lmk) memcpy(peer.lmk, lmk, 16);
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);   // re-add: encrypt flag can change
  esp_now_add_peer(&peer);
}

static inline void vb_peer_del(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
}
