// Radio side of the handheld: remembered dongles, encrypted session to the active dongle,
// discovery and pairing (initiator). See docs/PRD-pairing.md.

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static Preferences prefs;

static QueueHandle_t rx_queue;

// One unicast in flight at a time, so each send callback can be attributed.
enum tx_kind_t : uint8_t { TX_NONE, TX_KEY, TX_CONFIRM, TX_AUDIO, TX_OTHER };
static std::atomic<uint8_t> tx_kind{TX_NONE};
static uint32_t tx_sent_ms = 0;

// session
static uint8_t session_key[32];
static vb_replay_t rx_replay;
static std::atomic<uint32_t> tx_ctr{0};   // shared with the audio task
static uint8_t nonce_h[16];
static uint32_t last_link_req_ms = 0, link_lost_since = 0, last_ping_ms = 0;
static uint8_t hop_channel = 1;
static uint8_t pending_attempts = 0;

// ---------------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------------
static dongle_rec_t* active_rec() {
  return (store.active >= 0 && store.active < store.count) ? &store.d[store.active] : nullptr;
}

static void active_id(char out[8]) {
  dongle_rec_t* r = active_rec();
  if (r) vb_id_str(r->mac, out); else strcpy(out, "");
}

static void store_save() { prefs.putBytes("store", &store, sizeof(store)); }

static void store_load() {
  if (prefs.getBytes("store", &store, sizeof(store)) != sizeof(store) || store.magic != STORE_MAGIC) {
    memset(&store, 0, sizeof(store));
    store.magic = STORE_MAGIC;
    store.active = -1;
  }
}

static int store_find(const uint8_t mac[6]) {
  for (int i = 0; i < store.count; i++) if (memcmp(store.d[i].mac, mac, 6) == 0) return i;
  return -1;
}

// Add or replace; when full, replace the least recently used one. Returns the index.
static int store_add(const uint8_t mac[6], const vb_keys_t& keys, uint8_t channel) {
  int i = store_find(mac);
  if (i < 0 && store.count < MAX_DONGLES) i = store.count++;
  if (i < 0) {
    i = 0;
    for (int j = 1; j < store.count; j++) if (store.d[j].last_used < store.d[i].last_used) i = j;
    char old[8]; vb_id_str(store.d[i].mac, old);
    Serial.printf("store full, replacing %s\n", old);
  }
  dongle_rec_t& r = store.d[i];
  memcpy(r.mac, mac, 6);
  memcpy(r.lmk, keys.lmk, 16);
  memcpy(r.mac_key, keys.mac_key, 32);
  r.channel = channel;
  r.last_used = ++store.clock;
  store.active = i;
  store_save();
  return i;
}

static void store_remove(int i) {
  if (i < 0 || i >= store.count) return;
  vb_peer_del(store.d[i].mac);
  for (int j = i; j < store.count - 1; j++) store.d[j] = store.d[j + 1];
  store.count--;
  memset(&store.d[store.count], 0, sizeof(dongle_rec_t));
  if (store.active == i) {
    store.active = -1;                    // fall back to the most recently used one
    for (int j = 0; j < store.count; j++)
      if (store.active < 0 || store.d[j].last_used > store.d[store.active].last_used) store.active = j;
  } else if (store.active > i) {
    store.active--;
  }
  store_save();
}

// ---------------------------------------------------------------------------------
// Radio plumbing
// ---------------------------------------------------------------------------------
static void set_channel(uint8_t ch) {
  if (ch == cur_channel) return;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  cur_channel = ch;
}

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < (int)sizeof(vb_hdr_t) || len > (int)sizeof(rx_msg_t::data)) return;
  rx_msg_t m;
  memcpy(m.mac, info->src_addr, 6);
  m.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -100;
  m.len = len;
  memcpy(m.data, data, len);
  xQueueSend(rx_queue, &m, 0);
}

static void on_sent(const esp_now_send_info_t* info, esp_now_send_status_t status) {
  if (memcmp(info->des_addr, BROADCAST, 6) == 0) return;
  uint8_t k = tx_kind.exchange(TX_NONE);
  bool ok = status == ESP_NOW_SEND_SUCCESS;
  if (k == TX_KEY && send_state == SEND_WAIT_CB) send_state = ok ? SEND_OK : SEND_FAIL;
  if (k == TX_CONFIRM) confirm_cb = ok ? 1 : 2;
  if (k == TX_AUDIO && !ok) audio_failed++;
}

static bool unicast(const uint8_t* mac, const void* pkt, size_t len, uint8_t kind = TX_OTHER) {
  uint8_t expected = TX_NONE;
  if (!tx_kind.compare_exchange_strong(expected, kind)) return false;
  tx_sent_ms = millis();
  if (esp_now_send(mac, (const uint8_t*)pkt, len) != ESP_OK) { tx_kind = TX_NONE; return false; }
  return true;
}

static void espnow_up() {
  if (esp_now_init() != ESP_OK) { Serial.println("esp_now_init failed"); return; }
  esp_now_set_pmk(VB_PMK);
  esp_now_register_recv_cb(on_recv);
  esp_now_register_send_cb(on_sent);
  cur_channel = 0;              // force set_channel to apply
  set_channel(VB_PAIR_CHANNEL);
  vb_peer_set(BROADCAST, nullptr);
}

static void radio_begin() {
  rx_queue = xQueueCreate(16, sizeof(rx_msg_t));
  prefs.begin("vb_pair", false);
  store_load();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
  vb_id_str(my_mac, my_id);
  tx_ctr = esp_random() >> 8;   // any start works; the dongle resets its window per session
  espnow_up();
}

// ---------------------------------------------------------------------------------
// Session with the active dongle
// ---------------------------------------------------------------------------------
static bool linked() {
  return radio_mode == RM_LINK && session_ok && millis() - last_pong_ms < VB_LINK_TIMEOUT_MS;
}

static void session_drop() {
  if (session_ok) link_lost_since = millis();
  session_ok = false;
  last_link_req_ms = 0;
}

// (Re)attach to the active dongle: encrypted peer on its channel, new session.
static void link_activate() {
  for (int i = 0; i < store.count; i++) vb_peer_del(store.d[i].mac);
  session_ok = false;
  link_lost_since = millis();
  last_link_req_ms = 0;
  dongle_rec_t* r = active_rec();
  if (!r) return;
  set_channel(r->channel);
  hop_channel = r->channel;
  vb_peer_set(r->mac, r->lmk);
  // 6 Mbps OFDM instead of the 1 Mbps default: an audio frame takes ~1 ms on air instead of ~5
  esp_now_rate_config_t rate = {};
  rate.phymode = WIFI_PHY_MODE_11G;
  rate.rate = WIFI_PHY_RATE_6M;
  esp_now_set_peer_rate_config(r->mac, &rate);
  char id[8]; vb_id_str(r->mac, id);
  Serial.printf("link: active %s on channel %u\n", id, r->channel);
}

static void link_set_active(int i) {
  if (i < 0 || i >= store.count) return;
  store.active = i;
  store.d[i].last_used = ++store.clock;
  store_save();
  link_activate();
}

static void send_link_req() {
  dongle_rec_t* r = active_rec();
  vb_link_req_t q = { vb_hdr(VB_PKT_LINK_REQ), {}, {} };
  esp_fill_random(nonce_h, 16);
  memcpy(q.nonce_h, nonce_h, 16);
  vb_link_tag(r->mac_key, &q, sizeof(q) - VB_TAG_LEN, q.tag);
  if (unicast(r->mac, &q, sizeof(q))) last_link_req_ms = millis();
}

static void send_ping(uint8_t battery, bool charging) {
  vb_ping_t p = { vb_hdr(VB_PKT_PING), ++tx_ctr, (uint8_t)(battery > 100 ? 255 : battery), 0, {} };
  if (charging)  p.flags |= VB_FLAG_CHARGING;
  if (dictating) p.flags |= VB_FLAG_DICTATING;
  if (dimmed || screen_off) p.flags |= VB_FLAG_DIMMED;
  vb_seal(&p, sizeof(p), session_key);
  if (unicast(active_rec()->mac, &p, sizeof(p))) last_ping_ms = millis();
}

static void on_link_ack(const vb_link_ack_t* a) {
  dongle_rec_t* r = active_rec();
  uint8_t tag[VB_TAG_LEN];
  vb_link_tag(r->mac_key, a, sizeof(*a) - VB_TAG_LEN, tag);
  // only the answer to our latest LINK_REQ, once: a replayed LINK_ACK can't restart a session
  if (session_ok || !vb_ct_equal(tag, a->tag, VB_TAG_LEN) || memcmp(a->nonce_h, nonce_h, 16) != 0) return;
  vb_session_key(r->mac_key, a->nonce_h, a->nonce_d, session_key);
  memset(nonce_h, 0, 16);
  rx_replay = {0, 0};
  session_ok = true;
  last_pong_ms = millis();
  if (a->channel >= 1 && a->channel <= 13) {
    set_channel(a->channel);            // may have heard it from a neighbouring channel
    hop_channel = a->channel;
  }
  if (r->channel != cur_channel) {      // the dongle moved: remember where it is now
    r->channel = cur_channel;
    store_save();
  }
  Serial.printf("link: session up on channel %u\n", cur_channel);
}

static void on_pong(const rx_msg_t& m) {
  if (!session_ok || !vb_open(m.data, m.len, session_key)) return;
  uint32_t ctr; memcpy(&ctr, m.data + sizeof(vb_hdr_t), 4);
  if (!vb_replay_accept(&rx_replay, ctr)) return;
  last_pong_ms = millis();
  dongle_usb_state = ((const vb_pong_t*)m.data)->usb_state;
  voiceink_state = ((const vb_pong_t*)m.data)->rec_state;
}

// battery/charging are owned by handheld.ino and passed in for the heartbeat
static void link_update(int battery, bool charging) {
  if (radio_mode != RM_LINK || !active_rec()) return;
  uint32_t now = millis();
  if (session_ok && now - last_pong_ms > VB_LINK_TIMEOUT_MS) { session_drop(); Serial.println("link: lost"); }

  if (!session_ok) {
    // after 5 s without a session, step through channels: the dongle may have moved
    bool hopping = now - link_lost_since > 5000;
    if (now - last_link_req_ms < (hopping ? 120u : 250u) || tx_kind != TX_NONE) return;
    if (hopping) { hop_channel = hop_channel % 13 + 1; set_channel(hop_channel); }
    send_link_req();
  } else if (now - last_ping_ms >= VB_PING_INTERVAL_MS && send_state != SEND_WAIT_CB) {
    send_ping(battery < 0 ? 255 : battery, charging);
  }
}

// ---------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------
static bool send_key(uint8_t key_id, uint8_t mods, uint8_t keycode) {
  if (!linked() || send_state != SEND_IDLE) return false;
  pending_key = { vb_hdr(VB_PKT_KEY), ++tx_ctr, key_id, mods, keycode, KEY_HOLD_MS, {} };
  vb_seal(&pending_key, sizeof(pending_key), session_key);
  pending_attempts = 0;
  send_state = SEND_QUEUED;
  return true;
}

static void send_update() {
  uint8_t s = send_state;
  if (s == SEND_WAIT_CB && millis() - tx_sent_ms > 150) { tx_kind = TX_NONE; s = SEND_FAIL; }
  if (s == SEND_QUEUED || (s == SEND_FAIL && pending_attempts < 4)) {
    if (tx_kind != TX_NONE) return;     // radio busy with a ping; try next loop
    // identical bytes each attempt: the dongle's replay window drops a copy that already arrived
    send_state = SEND_WAIT_CB;
    pending_attempts++;
    if (!unicast(active_rec()->mac, &pending_key, sizeof(pending_key), TX_KEY)) send_state = SEND_FAIL;
    return;
  }
  if (s == SEND_OK || s == SEND_FAIL) {
    send_state = SEND_IDLE;
    on_key_result(s == SEND_OK, pending_key.key_id);
  }
}

// From the audio task: seal one frame and send it, waiting if a ping or key is in flight.
// A frame that cannot go out within 15 ms is dropped; the dongle fills the gap with silence.
static void send_audio(vb_audio_t* a) {
  dongle_rec_t* r = active_rec();
  if (!r || !linked()) return;
  a->hdr = vb_hdr(VB_PKT_AUDIO);
  a->ctr = ++tx_ctr;
  vb_seal(a, sizeof(*a), session_key);
  for (int i = 0; i < 15; i++) {
    if (unicast(r->mac, a, sizeof(*a), TX_AUDIO)) { audio_sent++; return; }
    vTaskDelay(1);
  }
  audio_busy++;
}

static void send_unpair() {
  vb_unpair_t u = { vb_hdr(VB_PKT_UNPAIR), ++tx_ctr, {} };
  vb_seal(&u, sizeof(u), session_key);
  for (int i = 0; i < 20 && tx_kind != TX_NONE; i++) delay(5);
  unicast(active_rec()->mac, &u, sizeof(u));
  for (int i = 0; i < 20 && tx_kind != TX_NONE; i++) delay(5);
}

// Reboot the dongle; its USB re-enumerates on the Mac.
static void send_restart() {
  vb_restart_t r = { vb_hdr(VB_PKT_RESTART), ++tx_ctr, {} };
  vb_seal(&r, sizeof(r), session_key);
  for (int i = 0; i < 20 && tx_kind != TX_NONE; i++) delay(5);
  unicast(active_rec()->mac, &r, sizeof(r));
}

// Forget a remembered dongle; tells it to forget us too when it is the linked one.
static void forget_dongle(int i) {
  if (i == store.active && linked()) send_unpair();
  store_remove(i);
  link_activate();
}

// ---------------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------------
static void discover_begin() {
  radio_mode = RM_DISCOVER;
  session_ok = false;
  for (int i = 0; i < store.count; i++) vb_peer_del(store.d[i].mac);
  set_channel(VB_PAIR_CHANNEL);
  found_count = 0;
  found_sel = 0;
  pair_state = PS_NONE;
  discover_started_ms = millis();
}

static void discover_end() {
  for (int i = 0; i < found_count; i++) vb_peer_del(found[i].mac);
  found_count = 0;
  pair_state = PS_NONE;
  radio_mode = RM_LINK;
  link_activate();
}

static void on_advertise(const rx_msg_t& m) {
  const vb_advertise_t* a = (const vb_advertise_t*)m.data;
  int i = 0;
  while (i < found_count && memcmp(found[i].mac, m.mac, 6) != 0) i++;
  if (i == found_count) {
    if (found_count >= MAX_FOUND) return;
    found_count++;
    memcpy(found[i].mac, m.mac, 6);
    vb_id_str(m.mac, found[i].id);
    found[i].rssi = m.rssi;
    Serial.printf("discover: %s rssi %d\n", found[i].id, m.rssi);
  }
  found[i].rssi += (m.rssi - found[i].rssi) * 0.3f;
  found[i].flags = a->flags;
  memcpy(found[i].paired_to, a->paired_to, 2);
  found[i].last_seen = millis();
}

// paired to this handheld already?
static bool found_is_mine(const found_t& f) {
  return (f.flags & VB_ADV_PAIRED) && f.paired_to[0] == my_mac[4] && f.paired_to[1] == my_mac[5];
}

static void discover_update() {
  if (radio_mode != RM_DISCOVER || pair_state != PS_NONE) return;
  uint32_t now = millis();

  static uint32_t last_discover = 0, last_identify = 0, last_sort = 0;
  if (now - last_discover >= 500) {
    last_discover = now;
    vb_discover_t d = { vb_hdr(VB_PKT_DISCOVER) };
    esp_now_send(BROADCAST, (const uint8_t*)&d, sizeof(d));
  }

  // drop stale entries; re-sort by signal once a second, keeping the highlight on the same dongle
  uint8_t sel_mac[6] = {0};
  bool have_sel = found_sel < found_count;
  if (have_sel) memcpy(sel_mac, found[found_sel].mac, 6);
  int n = 0;
  for (int i = 0; i < found_count; i++) {
    if (now - found[i].last_seen < 3000) found[n++] = found[i];
    else vb_peer_del(found[i].mac);
  }
  found_count = n;
  if (now - last_sort > 1000) {
    last_sort = now;
    for (int i = 1; i < found_count; i++)
      for (int j = i; j > 0 && found[j].rssi > found[j - 1].rssi; j--) std::swap(found[j], found[j - 1]);
  }
  found_sel = 0;
  if (have_sel) for (int i = 0; i < found_count; i++) if (memcmp(found[i].mac, sel_mac, 6) == 0) found_sel = i;

  // make the highlighted dongle flash
  if (found_count && now - last_identify >= 400 && tx_kind == TX_NONE) {
    last_identify = now;
    vb_peer_set(found[found_sel].mac, nullptr);
    vb_identify_t idf = { vb_hdr(VB_PKT_IDENTIFY) };
    unicast(found[found_sel].mac, &idf, sizeof(idf));
  }
}

// ---------------------------------------------------------------------------------
// Pairing (initiator)
// ---------------------------------------------------------------------------------
static void pair_send_hello() {
  vb_pair_hello_t h = { vb_hdr(VB_PKT_PAIR_HELLO), {} };
  memcpy(h.pub, pa.pubA, 32);
  if (unicast(pa.peer, &h, sizeof(h))) pa.last_tx = millis();
}

static void pair_send_nonce() {
  vb_pair_nonce_t n = { vb_hdr(VB_PKT_PAIR_NONCE_A), {} };
  memcpy(n.nonce, pa.nA, 16);
  if (unicast(pa.peer, &n, sizeof(n))) pa.last_tx = millis();
}

static void pair_send_confirm() {
  vb_pair_confirm_t c = { vb_hdr(VB_PKT_PAIR_CONFIRM), 0, {} };
  vb_pair_confirm_tag(pa.keys.mac_key, 'A', pa.pubA, pa.pubB, 0, c.tag);
  confirm_cb = 0;
  if (unicast(pa.peer, &c, sizeof(c), TX_CONFIRM)) pa.last_tx = millis();
}

static void pair_finish(bool ok, const char* text) {
  memset(pa.priv, 0, sizeof(pa.priv));
  if (!ok) memset(&pa.keys, 0, sizeof(pa.keys));
  pair_state = ok ? PS_DONE : PS_FAILED;
  pa.result = text;
  pa.result_until = millis() + (ok ? 1500 : 2500);
  Serial.printf("pairing: %s\n", text);
}

static void pair_abort(uint8_t reason, const char* text) {
  vb_pair_abort_t a = { vb_hdr(VB_PKT_PAIR_ABORT), reason };
  for (int i = 0; i < 20 && tx_kind != TX_NONE; i++) delay(5);
  unicast(pa.peer, &a, sizeof(a));
  pair_finish(false, text);
}

static void pair_start(int i) {
  if (i < 0 || i >= found_count) return;
  memcpy(pa.peer, found[i].mac, 6);
  strcpy(pa.peer_id, found[i].id);
  if (!vb_x25519_keygen(pa.priv, pa.pubA)) { pair_finish(false, "KEY ERROR"); return; }
  esp_fill_random(pa.nA, 16);
  pa.local_accepted = pa.peer_confirmed = pa.confirm_acked = false;
  pa.started = millis();
  pa.last_tx = 0;
  pair_state = PS_HELLO;
  vb_peer_set(pa.peer, nullptr);
  Serial.printf("pairing: hello -> %s\n", pa.peer_id);
}

static void pair_accept() {
  if (pair_state != PS_CODE || pa.local_accepted) return;
  pa.local_accepted = true;
  pair_send_confirm();
}

static void pair_reject() {
  if (pair_state == PS_HELLO || pair_state == PS_NONCE || pair_state == PS_CODE)
    pair_abort(VB_ABORT_REJECTED, "CANCELLED");
}

static void on_pair_packet(const rx_msg_t& m, uint8_t type) {
  if (memcmp(m.mac, pa.peer, 6) != 0) return;
  if (type == VB_PKT_PAIR_COMMIT && pair_state == PS_HELLO && vb_valid(m.data, m.len, type, sizeof(vb_pair_commit_t))) {
    const vb_pair_commit_t* c = (const vb_pair_commit_t*)m.data;
    memcpy(pa.pubB, c->pub, 32);
    memcpy(pa.commitB, c->commit, 32);
    pair_state = PS_NONCE;
    pa.last_tx = 0;
  } else if (type == VB_PKT_PAIR_NONCE_B && pair_state == PS_NONCE && vb_valid(m.data, m.len, type, sizeof(vb_pair_nonce_t))) {
    memcpy(pa.nB, ((const vb_pair_nonce_t*)m.data)->nonce, 16);
    uint8_t check[32], shared[32];
    vb_pair_commit(pa.pubB, pa.pubA, pa.nB, check);
    if (!vb_ct_equal(check, pa.commitB, 32)) { pair_abort(VB_ABORT_MISMATCH, "CHECK FAILED"); return; }
    if (!vb_x25519_shared(pa.priv, pa.pubB, shared) || !vb_pair_derive(shared, pa.nA, pa.nB, &pa.keys)) {
      pair_abort(VB_ABORT_MISMATCH, "KEY ERROR");
      return;
    }
    memset(shared, 0, sizeof(shared));
    pa.code = vb_pair_code(pa.pubA, pa.pubB, pa.nA, pa.nB);
    pair_state = PS_CODE;
    VIBRATE(VIB_ENTER);
  } else if (type == VB_PKT_PAIR_CONFIRM && pair_state == PS_CODE && vb_valid(m.data, m.len, type, sizeof(vb_pair_confirm_t))) {
    const vb_pair_confirm_t* c = (const vb_pair_confirm_t*)m.data;
    uint8_t tag[16];
    vb_pair_confirm_tag(pa.keys.mac_key, 'B', pa.pubA, pa.pubB, c->channel, tag);
    if (!vb_ct_equal(tag, c->tag, 16)) { pair_abort(VB_ABORT_MISMATCH, "KEYS DIFFER"); return; }
    pa.peer_confirmed = true;
    pa.peer_channel = c->channel;
  } else if (type == VB_PKT_PAIR_ABORT && pair_state != PS_DONE && pair_state != PS_FAILED) {
    uint8_t reason = m.len >= (int)sizeof(vb_pair_abort_t) ? ((const vb_pair_abort_t*)m.data)->reason : 0;
    pair_finish(false, reason == VB_ABORT_REJECTED ? "REJECTED" : reason == VB_ABORT_TIMEOUT ? "TIMED OUT" : "FAILED");
  } else if (type == VB_PKT_PAIR_BUSY && pair_state == PS_HELLO) {
    pair_finish(false, "DONGLE BUSY");
  }
}

static void pair_update() {
  uint32_t now = millis();
  switch (pair_state) {
    case PS_NONE: return;
    case PS_DONE:
    case PS_FAILED:
      if ((int32_t)(now - pa.result_until) >= 0) on_pair_result(pair_state == PS_DONE);
      return;
    default: break;
  }
  if (now - pa.started > VB_PAIR_TIMEOUT_MS) { pair_abort(VB_ABORT_TIMEOUT, "TIMED OUT"); return; }
  bool resend = now - pa.last_tx > 300 && tx_kind == TX_NONE;
  if (pair_state == PS_HELLO && resend) pair_send_hello();
  else if (pair_state == PS_NONCE && resend) pair_send_nonce();
  else if (pair_state == PS_CODE && pa.local_accepted) {
    if (confirm_cb == 1) pa.confirm_acked = true;
    if (!pa.confirm_acked && resend) pair_send_confirm();
    if (pa.confirm_acked && pa.peer_confirmed) {
      store_add(pa.peer, pa.keys, pa.peer_channel);
      memset(&pa.keys, 0, sizeof(pa.keys));
      pair_finish(true, "PAIRED");
      VIBRATE(VIB_START);
    }
  }
}

// ---------------------------------------------------------------------------------
// Sleep (handheld.ino decides when)
// ---------------------------------------------------------------------------------
// Radio off for sleep. Tells the linked dongle first, so it shows "asleep" instead of searching.
static void radio_sleep() {
  if (linked() && send_state == SEND_IDLE) {
    vb_sleep_t z = { vb_hdr(VB_PKT_SLEEP), ++tx_ctr, {} };
    vb_seal(&z, sizeof(z), session_key);
    for (int i = 0; i < 20 && tx_kind != TX_NONE; i++) delay(5);
    unicast(active_rec()->mac, &z, sizeof(z));
    for (int i = 0; i < 20 && tx_kind != TX_NONE; i++) delay(5);
  }
  session_drop();
  esp_now_deinit();
  esp_wifi_stop();
  tx_kind = TX_NONE;
}

static void radio_wake() {
  esp_wifi_start();
  espnow_up();
  tx_kind = TX_NONE;
  if (radio_mode == RM_LINK) link_activate();
}

// ---------------------------------------------------------------------------------
// Dispatch (called from loop)
// ---------------------------------------------------------------------------------
static void radio_update(int battery, bool charging) {
  if (tx_kind != TX_NONE && millis() - tx_sent_ms > 200) tx_kind = TX_NONE;   // lost callback

  rx_msg_t m;
  while (xQueueReceive(rx_queue, &m, 0) == pdTRUE) {
    uint8_t type = vb_type(m.data, m.len);
    dongle_rec_t* r = active_rec();
    bool from_active = r && memcmp(m.mac, r->mac, 6) == 0;
    if (radio_mode == RM_DISCOVER) {
      if (type == VB_PKT_ADVERTISE && pair_state == PS_NONE && vb_valid(m.data, m.len, type, sizeof(vb_advertise_t)))
        on_advertise(m);
      else if (type >= VB_PKT_PAIR_HELLO && type <= VB_PKT_PAIR_BUSY && pair_state != PS_NONE)
        on_pair_packet(m, type);
    } else if (from_active) {
      if (type == VB_PKT_LINK_ACK && vb_valid(m.data, m.len, type, sizeof(vb_link_ack_t))) on_link_ack((const vb_link_ack_t*)m.data);
      else if (type == VB_PKT_PONG && m.len == sizeof(vb_pong_t)) on_pong(m);
      else if (type == VB_PKT_NEED_LINK && session_ok) { session_drop(); Serial.println("link: dongle asked for new session"); }
    }
  }

  link_update(battery, charging);
  send_update();
  discover_update();
  pair_update();
}
