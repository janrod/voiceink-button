# PRD: Live VoiceInk text on the handheld

Status: draft, stretch goal. Depends on the button/HID milestone (done) and is independent of USB audio.

## 1. Summary

While dictating, the StopWatch screen shows what VoiceInk is recognizing, live, and then the final
(pasted) text. The same channel gives the handheld VoiceInk's real recording state, which replaces two
guesses the firmware makes today: whether dictation is on (toggle tracking) and when the paste has
happened (fixed `PASTE_WAIT_MS` before Enter).

## 2. Goals and non-goals

**Goals**
- G1. Show partial transcript on the handheld within 300 ms of VoiceInk updating it.
- G2. Show final text after VoiceInk pastes it (including AI enhancement, if enabled).
- G3. Mirror VoiceInk's state on the handheld (idle / recording / transcribing / enhancing).
- G4. "Stop, wait for paste, then Enter" fires on the actual paste event, not a timer.
- G5. Opt-in and local only. No transcript leaves the Mac except over the dongle's USB to the handheld.

**Non-goals**
- Editing or correcting text from the handheld.
- Supporting dictation apps other than VoiceInk.
- Transcript history on the handheld (only the current utterance).
- Upstreaming to VoiceInk (desirable later, not required).

## 3. Background: what VoiceInk offers today

Researched against VoiceInk HEAD `eb93628` (v2.22); the installed app is 2.13 and contains the same
symbols.

- **No external interface for text or state.** No URL scheme for it, no AppleScript dictionary, no
  local server, no distributed notifications. App Intents only toggle or dismiss the recorder.
- **Partial text exists internally.** `VoiceInkEngine.partialTranscript` (`@Published`,
  `Features/Recording/Workflows/VoiceInkEngine.swift:102`) holds committed segments plus the current
  partial. It is populated only for streaming models, and only while state is `.recording`.
- **State exists internally.** `VoiceInkEngine.recordingState` (same file, line 96) has a `didSet`.
  `RecordingState` is `idle, starting, recording, transcribing, enhancing, busy`.
- **Final text.** `TranscriptionPipeline.swift:~299` posts the in-process `.transcriptionCompleted`
  after saving, which is after the paste. `TranscriptionDelivery.deliver` (`TranscriptionDelivery.swift:27`)
  has the text just before the paste.
- **This Mac streams.** The only mode, "Dictation", uses Parakeet V3 with realtime on. Partials
  update about once per second, and earlier words can be revised.
- **Final text is readable without a fork.** The app is not sandboxed. History is SwiftData SQLite at
  `~/Library/Application Support/com.prakashjoshipax.VoiceInk/default.store`, table `ZTRANSCRIPTION`
  (`ZTEXT`, `ZENHANCEDTEXT`, `ZTRANSCRIPTIONSTATUS`, `ZTIMESTAMP`).
- **Licence.** GPLv3. A private fork carries no obligations; distributing a modified build requires
  publishing its source. A separate helper that only listens is not a derivative work.

## 4. Approach

Chosen: **patch VoiceInk to publish events → Mac helper → dongle USB serial → ESP-NOW → handheld.**

| Option | Live text | State | Fork | Verdict |
|---|---|---|---|---|
| A. Helper reads the SQLite history file | No | Final only | No | Phase 0 fallback: final text only |
| B. Helper reads the recorder panel via Accessibility | Yes, fragile | Inferred | No | Rejected: breaks on UI changes, needs AX permission and the panel visible |
| **C. ~20-line VoiceInk patch + helper** | **Yes** | **Yes** | **Yes** | **Chosen** |

```
VoiceInk (fork) ──events──► vbridge (Mac helper) ──USB CDC serial──► dongle ──ESP-NOW──► handheld
                 local IPC                          line protocol             VB_PKT_STATE/TEXT
```

The dongle already enumerates as a USB composite device (HID keyboard + CDC serial), so no new USB
interface is needed. The "no Mac software" property of the button path is unchanged: without the
helper, everything works as today and the text panel stays empty.

## 5. Requirements

### 5.1 VoiceInk patch

- R1. Add `ExternalEventBroadcaster` with one `post(event, payload)` entry point, behind a new setting
  "Share live transcript with local apps" (default **off**).
- R2. Emit events at three hook points:
  - `state`: in `recordingState.didSet`, after the existing guard. Payload `{state}`.
  - `partial`: in a new `didSet` on `partialTranscript` when the value changes. Payload `{text}`.
    An empty string means cleared.
  - `final`: next to the `.transcriptionCompleted` post. Payload `{text, status}`, where `text` is
    `enhancedText ?? text`.
- R3. Transport. **Preferred: a Unix domain socket** at
  `~/Library/Application Support/com.prakashjoshipax.VoiceInk/events.sock`, mode 0600, carrying
  newline-delimited JSON. It's private to the user and needs no new entitlement.
  - Acceptable for a prototype: `DistributedNotificationCenter` with names
    `com.prakashjoshipax.VoiceInk.{state,partial,final}`. Any local process can read those, so it
    stays prototype-only.
- R4. The patch must not block the main actor. Socket writes go off the main thread; with no listener,
  events are dropped.
- R5. Keep the diff small and isolated (one new file plus three call sites) so rebasing on upstream
  stays cheap. Open an upstream PR once it works.

### 5.2 Mac helper (`vbridge`)

- R6. Swift command-line tool in this repo (`mac/vbridge`), run as a per-user LaunchAgent.
- R7. Find the dongle's serial port by USB VID `0x303A` plus product string (later: a custom product
  name). Reconnect automatically on unplug or replug, and when VoiceInk restarts.
- R8. Forward events using the line protocol in 5.3.
  - Throttle partials to at most 5 per second and coalesce to the latest value.
  - Send only the **tail**: the last 180 bytes, cut on a UTF-8 boundary, preferably at a word start.
- R9. Never log transcript text. Debug logs may record lengths and timings only.
- R10. Phase 0 fallback: if no VoiceInk socket exists, watch `default.store-wal` (FSEvents) and send
  `final` from the newest `ZTRANSCRIPTION` row, opening the store read-only.

### 5.3 Helper ↔ dongle line protocol (USB CDC, 115200, `\n`-terminated)

```
@S <state>          state: idle|starting|recording|transcribing|enhancing|busy
@P <text>           partial tail (UTF-8, \n escaped as \\n)
@F <status> <text>  final; status: completed|failed|canceled
@H                  helper heartbeat, every 2 s
```

- Lines not starting with `@` are ignored by both sides, so the dongle's existing debug logs can share
  the port.
- The dongle replies `@OK` to `@H` so the helper can detect a stale port.

### 5.4 Radio protocol additions (`firmware/common/vb_protocol.h`)

- R11. `VB_PKT_STATE` {state, helper_alive}: sent dongle → handheld on change, and piggybacked on every
  `PONG` (add a `vi_state` byte), so the handheld converges within one heartbeat.
- R12. `VB_PKT_TEXT` {seq, kind (partial/final), len, utf8[≤200]}: fits ESP-NOW v1's 250 B, so no
  chunking. Text is always the tail, so a lost packet costs nothing; the next update supersedes it.
- R13. Bump `VB_VERSION` to 2. A v1 handheld ignores the new packet types.

### 5.5 Handheld UX

- R14. **Text panel**: while recording and up to 4 s after the final text, show up to 3 lines of text
  in the band between the mic disc and the bottom link line (about 300 px wide at that height).
  - Newest text bottom-aligned; overflow is trimmed from the front with a leading "…".
  - Partial text in `C_MUTED`; the last committed word in `C_TEXT`.
  - Final text turns white, then fades after 4 s.
- R15. **Space**: when text is present, shrink the mic disc (radius 80 → 56) and move it up. The
  timer and button hints yield to the text.
- R16. **State mirroring**: when the helper is alive, VoiceInk's state wins over the local toggle.
  - `recording` → LISTENING.
  - `transcribing` / `enhancing` → a new "TRANSCRIBING" state with a spinner arc.
  - `idle` → READY.
  - This fixes drift (long-press resync becomes a fallback).
- R17. **Enter after paste**: when blue stops dictation and the helper is alive, send Enter on `@F
  completed`, plus a 150 ms settle delay for the paste to land.
  - On `failed` / `canceled`: no Enter; show "NOTHING TO SEND".
  - Keep `PASTE_WAIT_MS` as the fallback when the helper is absent, and as a 10 s timeout.
- R18. **Fonts**: the current FreeSans fonts are Latin-only.
  - Use an M5GFX efont (e.g. `fonts::efontEN_16`) for the text panel, upgradeable to efont CJK/JA if
    other languages are dictated.
  - Render missing glyphs as "?" rather than blank.
- R19. A "helper" indicator next to the link line (small dot: present or absent), so it's obvious why
  text isn't showing.

### 5.6 Privacy and security

- Opt-in in VoiceInk (R1); transport private to the user (R3, socket mode 0600).
- Text is sent over ESP-NOW unencrypted in v1. **Requirement for this feature: the encrypted, paired
  link from [PRD-pairing.md](PRD-pairing.md)**, since transcripts are more sensitive than keypresses.
- Nothing is persisted on the dongle or handheld; the text buffer is cleared on idle and on screen-off.

## 6. Performance targets

| Path | Target |
|---|---|
| VoiceInk partial update → handheld pixels | < 300 ms (p95) |
| Paste → Enter typed (R17) | < 400 ms |
| Added handheld radio airtime | ≤ 5 packets/s while recording |
| Added handheld battery cost | negligible beyond the existing 30 fps animation |

## 7. Milestones

1. **M0, final text, no fork**: vbridge with the SQLite watcher (R10), line protocol, dongle
   forwarding, handheld text panel showing the final text. Proves the full Mac → handheld path.
2. **M1, VoiceInk fork**: broadcaster with a Unix socket (R1–R5), live partials, state mirroring (R16).
3. **M2, Enter on paste** (R17) and helper indicator (R19).
4. **M3, hardening**: ESP-NOW encryption, LaunchAgent install script, upstream PR to VoiceInk.

## 8. Risks and open questions

- **Fork upkeep.** Local builds lose auto-update and iCloud dictionary sync. Ad-hoc signing may
  re-prompt for microphone and Accessibility permissions after rebuilds. *Mitigation: keep the patch
  tiny; pursue upstream.*
- **Readability.** Partial text gets revised every ~1 s, and on a round 1.75" screen that may be more
  distracting than useful. *Decide in M1 testing: maybe show only committed words plus a typing
  indicator.*
- **Languages.** Which languages will be dictated determines the font (R18) and its flash cost
  (CJK fonts are ~1–2 MB; the handheld has ~1.9 MB of app space free).
- **Final event timing.** The `.transcriptionCompleted` post comes after `modelContext.save()`. Confirm
  the paste has actually landed in the target app by then; otherwise hook `TranscriptionDelivery`
  plus a settle delay.
- **Helper packaging.** A bare LaunchAgent binary or a menu-bar app. A menu-bar app is easier to
  discover and to grant permissions to, but is more code.
