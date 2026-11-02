# PRD: Secure pairing

Status: implemented as protocol v2. This is the design as written before the build. Where the code
went another way:

- **Link setup and sessions.** Each link starts with LINK_REQ / LINK_ACK. These are authenticated with
  the 32-byte `mac_key` from pairing (R7 reserved 16 bytes for it). The fresh nonces of both sides
  give a per-session key. Every session packet carries a counter and an HMAC tag made with that key.
- **Replay protection (R13).** A new session key on every link, plus a 32-packet sliding window on the
  counter. Nothing is persisted to NVS.
- **Recovery (R16).** While unlinked, the StopWatch sends LINK_REQ every 250 ms. After 5 s it steps
  through channels 1–13, one every 120 ms, until a LINK_ACK tells it the dongle's channel.
- **A 5th dongle (R10)** replaces the least recently used one without asking.
- **Not built:** greyed-out v1 devices (R14) and showing "already paired" in the discovery list.

## 1. Problem

Before this design there was no pairing. Both devices sit on Wi-Fi channel 1. The handheld broadcasts a heartbeat,
and each side locks onto the first partner it hears until it reboots. Nothing is stored or encrypted.

That is fine on one desk and wrong for anything else:

- **Cross-linking.** With two sets in one room, a handheld can bind to the wrong dongle and type into
  someone else's Mac.
- **Keystroke injection.** The dongle is a USB keyboard that accepts key commands from any nearby
  ESP32 sending the right 4-byte header.
- **Eavesdropping.** Packets are plaintext. Today they carry keys and battery level; after live text
  they would carry transcripts.

## 2. Model: the StopWatch is the parent

The StopWatch owns pairing.

- It discovers dongles, lists them, and you pick one on its screen.
- It can remember several dongles (e.g. work Mac and home Mac) and switch between them.
- The dongle stays simple. It shows its ID, and accepts or rejects a pairing request with one button
  press.

**Device IDs.** Each device has a short ID: the last 4 hex digits of its MAC, e.g. **VB-0810**
(dongle) and **VB-9B64** (StopWatch).

- The dongle shows its ID on its idle screen at all times, so you can match it against the list.
- Optionally print the ID on a label on the case.

## 3. Goals and non-goals

**Goals**
- G1. Pairing is started and driven from the StopWatch; the dongle needs one button press.
- G2. When several dongles are in range, the StopWatch lists them, and you pick one.
- G3. Pairings survive reboots and power-off.
- G4. Once paired, each device accepts traffic only from its partner. All traffic is encrypted and
  protected against replays.
- G5. Pairing is resistant to a nearby attacker, including a man-in-the-middle, at the level of
  Bluetooth "numeric comparison".
- G6. The StopWatch remembers up to 4 dongles, with one active at a time.

**Non-goals (v1)**
- Several StopWatches active on one dongle. A dongle has exactly one paired StopWatch.
- Protection against someone holding the device and reading its flash (no flash encryption in v1).
- Pairing from the Mac.

## 4. User experience

### 4.1 Discover and select (StopWatch)

1. Swipe down → **ADD DONGLE**. Screen shows "Looking for dongles…" with a scanning ring.
2. Available dongles appear in a list as they answer, closest first.
   - Each row shows the ID (`VB-0810`) and signal bars (from RSSI).
   - It also shows a hint: "new" for an unpaired dongle, or "already paired" (greyed out) for one in
     pairing mode that is still linked to another StopWatch.
   - The list scrolls by drag. Yellow/blue also move the highlight, and a long-press of yellow
     selects.
3. **Identify.** The highlighted row makes that dongle's screen flash "THIS ONE" and its LED blink, so
   you can tell two dongles on the same desk apart without reading IDs.
4. Tap a row to select. If only one dongle is found, it's preselected, and you just tap **PAIR**.
5. Nothing found after 10 s: "No dongles found. Plug one in, or hold its button 3 s to make it
   discoverable." Then **RETRY**.

### 4.2 Confirm (both devices)

- The StopWatch shows a 6-digit code and "Accept on VB-0810".
- The dongle shows the same code and "Pair with VB-9B64? A = yes, B = no".
- The StopWatch shows **✓ / ✗** too. Pairing completes only when both sides accept.
- Success: a long buzz, "PAIRED" on both screens, and the new dongle becomes active. On a reject, a
  code mismatch or a 30 s timeout, nothing is stored.

### 4.3 Manage (StopWatch menu)

- **DONGLES** lists the remembered dongles. The active one is marked, and each row shows its link
  state when in range.
  - Tap a dongle to make it **active**. The StopWatch drops the old link and links to that one.
  - A long-press on a row offers **FORGET**.
- The main screen's link line names the active dongle: "LINKED · VB-0810".
- **Auto-switch** (optional, off by default): if the active dongle is out of range for 10 s and
  another remembered one is in range, switch to it, which covers moving between desks.

### 4.4 Dongle states

| State | When | Discoverable |
|---|---|---|
| Unpaired | Fresh, or after forget | Yes, always |
| Paired | Normal use | No |
| Pairing mode | Long-press A for 3 s while paired | Yes, for 60 s; shows "already paired" in lists |

Long-press B for 6 s on the dongle: forget its StopWatch.

## 5. Requirements

### 5.1 Discovery

All discovery runs on a fixed **pairing channel** (1) over unencrypted broadcast.

- R1. **Request.** The StopWatch broadcasts `DISCOVER` {its ID, protocol version} every 500 ms while
  the Add Dongle screen is open.
- R2. **Answer.**
  - A discoverable dongle answers `ADVERTISE` {ID, firmware version, paired flag} after a random
    0–50 ms delay, so several dongles don't collide.
  - RSSI comes from the receive info of the incoming packet.
  - A non-discoverable dongle stays silent.
- R3. **List upkeep.** The StopWatch keeps the list live. A dongle that hasn't answered for 3 s
  disappears, and RSSI is smoothed for sorting.
- R4. **Identify.** `IDENTIFY` {target ID} is unicast to the highlighted dongle every 500 ms. The
  dongle flashes for as long as it keeps arriving, without authentication. All it does is make the
  screen flash.

### 5.2 Key exchange

The StopWatch is the initiator (A) and the dongle the responder (B). New packet types: `PAIR_HELLO`,
`PAIR_COMMIT`, `PAIR_NONCE`, `PAIR_CONFIRM`.

1. A unicasts `PAIR_HELLO` {pubA} to the selected dongle. pubA is a fresh X25519 public key.
2. B generates pubB and a random nonce nB. It replies with `PAIR_COMMIT` {pubB, Cb = SHA-256(pubB ‖
   pubA ‖ nB)}. The commitment comes *before* any nonce is revealed.
3. A sends a random nonce nA. B reveals nB. A checks that Cb matches.
4. Both sides compute the code = SHA-256(pubA ‖ pubB ‖ nA ‖ nB) mod 10⁶ and display it.
5. When the user accepts on both sides, they exchange `PAIR_CONFIRM`. Keys are stored only after each
   side has received the other's confirmation.

**Why the commitment step:** without it, an attacker in the middle can keep generating keys until the
two codes match. That takes about 2²⁰ attempts, a few milliseconds on a laptop. This is Bluetooth LE
Secure Connections' numeric comparison.

- R5. Implement the commit/reveal order exactly as above.
- R6. Crypto: X25519 ECDH and HKDF-SHA256 via mbedtls, which ships in the ESP32 Arduino core and
  ESP-IDF. Randomness from `esp_fill_random` with the radio on.
- R7. Derive from the ECDH secret with HKDF (salt = nA ‖ nB):
  - `lmk` (16 B): the ESP-NOW per-peer encryption key.
  - `mac_key` (16 B): reserved for application-level authentication.
- R8. Pairing times out after 30 s, and all ephemeral keys and nonces are wiped on exit.
- R9. A dongle handles one pairing at a time. While one is in progress, it answers other `PAIR_HELLO`s
  with `BUSY`.

### 5.3 Paired link

- R10. **Storage.**
  - StopWatch: up to 4 records {dongle MAC, ID, lmk, channel, last-used time} plus the active index,
    in NVS (`Preferences`, namespace `vb_pair`).
  - Dongle: one record {StopWatch MAC, ID, lmk, channel}.
  - Adding a 5th dongle replaces the least recently used one, after confirming on screen.
- R11. **Encrypted peers.** Only the active dongle is registered as an encrypted ESP-NOW peer
  (`encrypt = true`, stored lmk), with a fixed global PMK (`esp_now_set_pmk`). Switching dongles swaps
  the peer. That keeps well under ESP-NOW's limit of 7 encrypted peers.
- R12. **Filtering.** Drop every non-discovery packet that is not encrypted and from the paired MAC.
  `PING`/`PONG` become unicast between partners; there are no broadcast heartbeats once paired.
- R13. **Replay protection.** An application-level 32-bit counter in every header.
  - It is persisted in coarse steps: reserve the next 1024 values in NVS whenever they run out, so a
    reboot never reuses a counter.
  - The receiver rejects any counter at or below the last one accepted.
  - The receiver checks this before the existing per-key `seq` dedupe.
  - Required whether or not ESP-NOW's CCMP layer already checks replays, which still has to be
    verified on this IDF version.
- R14. Protocol version goes to 2. A v1 device found during discovery is shown greyed out as
  "update firmware".

### 5.4 Channel

- R15. **Choosing.** When it becomes discoverable, the dongle scans and picks the quietest of channels
  1/6/11 (lowest total AP RSSI, which usually means a different channel from the Mac's Wi-Fi).
  - It moves there after pairing, and the channel is sent in `PAIR_CONFIRM`.
  - Discovery and pairing themselves always use channel 1.
- R16. **Recovery.** If the active link is lost for more than 5 s, the StopWatch steps through channels
  1–13 with encrypted unicast `PING`s.
  - The radio's delivery confirmation tells it which channel the dongle is on, and it stores that.
  - It retries every 10 s while unlinked.

### 5.5 Forget / reset

- R17. **Forget.** Forgetting on the StopWatch erases that record and sends an encrypted `UNPAIR`. If
  the dongle is in range, it forgets too and becomes unpaired.
- R18. **One-sided forget.** A side whose partner forgot it while out of range shows LINK LOST until it
  is forgotten locally or re-paired.

## 6. Acceptance criteria

- **Selection.** Three unpaired dongles in range all appear in the list within 2 s.
  - Identify flashes only the highlighted one.
  - Pairing with the selected one leaves the other two untouched.
- **No cross-linking.** Two complete sets in the same room, each paired, never cross-link over 100
  power cycles.
- **Switching.** A StopWatch with two remembered dongles switches between them from the menu in under
  2 s.
- **Injection.** An unpaired ESP32 sending well-formed v1 or v2 `KEY` packets causes no keystrokes on a
  paired dongle.
- **Replay.** Replaying a captured, valid `KEY` packet causes no keystroke.
- **Speed.** Pairing a fresh dongle takes under 20 s, from ADD DONGLE to PAIRED. A power-cycled pair
  re-links in under 1 s.
- **Channel move.** Moving a dongle to another channel re-links within 10 s, without user action.

## 7. Risks and open questions

- **Dongle confirmation.** Should a fresh, never-paired dongle accept pairing without a button press,
  if the request arrives within 60 s of it being plugged in? It removes a step, but anyone nearby with a
  StopWatch could claim a newly plugged-in dongle in that window. Default: **require the press**.
- **Keys in plain flash.** The keys are readable by anyone who physically dumps the flash. That is
  accepted for v1; ESP32-S3 flash encryption can be added later, but it complicates reflashing.
- **Dongle buttons.** The StickS3's two buttons have to cover accept, reject, pairing mode and forget.
  Confirm the long-press timings don't collide with download mode, which uses the side power button.
- **Round-screen list.** On the round screen, rows near the top and bottom are narrow. Show at most 3
  rows, centered, with the highlighted row in the middle (a rotary-style list).
