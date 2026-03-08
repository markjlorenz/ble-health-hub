# BLE Health Hub — Pulse Ox Handoff Notes (2026-03-02)

This document is a protocol + implementation handoff for another engineer.

## Goal

Build a Chromium Web Bluetooth demo that connects to a vendor pulse oximeter and displays live:

- SpO₂
- Heart rate
- Pleth waveform samples

We reverse engineered the vendor protocol primarily from:

- `data/pulse-ox.pcapng` (BLE traffic from the phone app)
- Live logs from the Web Bluetooth demo

The current blocker: the Web Bluetooth app can complete the vendor handshake and the device is actively measuring (SpO₂/HR shown on the device screen), but we still do not reliably get the streaming measurement frames (`A0 11 F0 ...`) that appear in the capture.

---

## Workspace layout

- `data/pulse-ox.pcapng`: reference capture (phone app)
- `web/`: Web Bluetooth demo
  - `web/index.html`: UI
  - `web/app.js`: BLE connect + handshake + decode
  - `web/style.css`: styling

## Vendor APK availability

The vendor APK is not committed to git (GitHub blocks >100MB files, and APKs are typically proprietary).

- If you have permission to share it, attach it to a **GitHub Release asset** for this repo and record its SHA256.
- Otherwise, store only a download pointer (official source) + SHA256 in documentation.

---

## How to run the Web Bluetooth demo

1. Start a local web server:

   ```bash
   cd web
   python3 -m http.server 8000
   ```

   Then open:

   - http://localhost:8000

2. Use a Chromium browser (Chrome / Edge).

   Web Bluetooth requires a secure context:

   - `https://...` OR `http://localhost`

3. Click **Connect** and select the pulse oximeter.

---

## BLE / GATT facts (from pcap + live)

### Vendor service UUID

The device exposes the vendor service UUID in a Wireshark “ASCII-ish bytes” form:

- `636f6d2e-6a69-7561-6e2e-504f56313100`

Wireshark shows the raw bytes as:

- `636f6d2e6a6975616e2e504f56313100` → `"com.jiuan.POV11\0"`

Web Bluetooth UUID presentation can differ by byte order depending on stack/tooling, so the app tries multiple UUID strings:

- `56313100-504f-6e2e-6175-696a2e6d6f63`
- `00313156-4f50-2e6e-6175-696a2e6d6f63`
- `636f6d2e-6a69-7561-6e2e-504f56313100`

### Vendor characteristic UUID

The vendor characteristic used for both writes and notifications is:

- `7274782e-6a69-7561-6e2e-504f56313100`

Observed properties:

- `write`, `writeWithoutResponse`, `notify`

### Capture handle

In `data/pulse-ox.pcapng`, the vendor protocol traffic rides on ATT handle:

- `0x0027`

(Confirmed by scanning for `A0 11 F0` frames and locating the preceding ATT Notify opcode.)

---

## Vendor protocol overview (what we know)

### Message families

All bytes below are shown as hex.

**ACK notifications (device → client)**

- Format: `A0 03 A0 <step> AC <xx>`
- Length: 6 bytes
- We key off `step` to advance stages.

**Prompt notifications (device → client)**

- Format: `A0 04 00 <base> AC <t1> <t2>`
- Length: 7 bytes
- Meaning: appears to request the *next* stage chunk and provides two “tail” bytes (`t1`, `t2`) that influence the next write(s).

**Measurement streaming notifications (device → client)**

- Capture shows repeated 20-byte notifications:
  - `A0 11 F0 ...` (20 bytes)

There are also `A0 11 31/32/33 ...` 20-byte notifications that appear during handshake; those do not match the measurement payload shape we expect.

**Status-like message (device → client)**

- Observed near handshake: `A0 0A ...` (13 bytes)

### The staged handshake

The phone app performs a staged series of vendor writes (`B0 ...`) and advances on specific ACK steps.

The Web Bluetooth implementation models this as 6 stages with expected ACK steps:

- Stage 1 → expect ACK `0x04`
- Stage 2 → expect ACK `0x12`
- Stage 3 → expect ACK `0x18`
- Stage 4 → expect ACK `0x1E`
- Stage 5 → expect ACK `0x24`
- Stage 6 → expect ACK `0x2C`

Stages 1–5 are “fixed writes” derived from the pcap and are stable.

Stage 6 is the critical transition to measurement streaming in the capture and is where most live-device divergence occurs.

---

## Android SDK notes (why stage-1/stage-2 bytes vary)

We checked the official Android example (`Po3.java`) and the bundled SDK jars.
The Activity code is just a wrapper; the real protocol logic lives in the SDK.

Key findings from jar inspection (no decompilation required):

- `com/ihealth/communication/control/Po3Control` is a thin wrapper around obfuscated `com/ihealth/communication/ins/*` classes.
- `com/ihealth/communication/ins/x` references `java/util/Random` and `com/ihealth/communication/ins/GenerateKap`.
- `com/ihealth/communication/ins/GenerateKap` calls `System.loadLibrary("iHealth")` (JNI) and exposes methods like `getKey` / `getKa`.

Implication:

- The session-varying bytes we see in stage 1/2 (e.g. the trailing bytes in `B0 11 ...` and `B0 06 10 03 AC ?? ??`) are very likely derived from a per-session RNG nonce plus a device/app secret processed in the *native* `iHealth` library.
- That explains why attempts to derive those bytes from PCAP-visible BLE link-layer parameters (AA/CRCInit/LL control PDUs) didn’t find a relationship.
- Without the native library (or the full vendor SDK runtime), we should assume we cannot reproduce “valid” stage-1/stage-2 tails from scratch.

Repo tool:

- `tools/inspect_ihealth_sdk_jar.py` can scan a jar for constant-pool strings and basic indicators (e.g. `Random`, `loadLibrary`, `GenerateKap`).

Native library note:

- We downloaded and inspected one candidate `libiHealth.so` (ARM32) from a public repo.
- It exports JNI entrypoints including `Java_com_ihealth_communication_ins_GenerateKap_getKey` and appears to produce 16-byte keys.
- Its embedded selector strings include device IDs like `BP3L`, `BG5L`, `HS4S`, etc., but **no obvious PO3/SpO2/pulse-ox identifier**.
  This suggests that specific `.so` build may not be the one used by the pulse-ox SDK/app, or that PO3 shares a non-obvious selector.

---

## Stage 6: exact capture-aligned transition

In the pcap, measurement frames begin immediately after this sequence:

- (Earlier) device prompt: `A0 04 00 26 AC A5 77`
- client write: `B0 03 A0 27 AC 73`
- device prompt: `A0 04 00 28 AC A6 7A`
- client write: `B0 03 A0 29 AC 75`
- client write: `B0 04 00 2B AC A6 7D`
- device ACK: `A0 03 A0 2C AC 78`
- then streaming begins: `A0 11 F0 ...` (repeating)

Key point: in the capture, after ACK `0x2C`, there is *no* cyclic prompt loop; the device just streams `A0 11 F0`.

---

## Live device divergence (current blocker)

On the live device we repeatedly see:

- Handshake completes through ACK `0x2C`.
- Immediately after, the device starts emitting a cyclic prompt sequence:
  - `A0 04 00 2E ...`, `A0 04 00 34 ...`, `A0 04 00 3A ...` …
  - Base increments by `+0x06` and eventually wraps (`FA → 00`).
- If we respond to these prompts, the device ACKs each one and continues prompting.
- Despite the device actively measuring (SpO₂/HR visible on the device screen), we do not see `A0 11 F0` streaming.

This is **not** seen in the pcap, which suggests either:

- We are missing a “start streaming” detail that the phone app triggers (possibly outside the vendor handle/characteristic we focused on), OR
- This firmware uses a different streaming mode than the one in the capture, OR
- Responding to the post-handshake prompt loop keeps it from streaming (but note: even when we respond correctly, still no stream so far).

---

## Root cause and fix (identified 2026-03-02 — app version 2026-03-02.5)

### The universal protocol rule

Every `B0 04` write at ATT step `S` is a response to the preceding `A0 04` prompt at
`base = S - 3`. The prompt's two tail bytes are passed directly to `buildB004(S, tail1, tail2)`,
which appends `tail2 + 3` as the final byte.

This rule holds for every stage including the post-handshake prompt loop:

| Stage | B0 04 step | Prompt base | Notes |
|-------|-----------|-------------|-------|
| 3 | 0x17 | 0x14 | |
| 4 | 0x1d | 0x1a | |
| 5 | 0x23 | 0x20 | Capture shows `A0 06` here, live firmware uses `A0 04` |
| 6 | 0x2b | 0x28 | Second prompt; first prompt (0x26) triggers B0 03 0x27 |
| Post-handshake | base+3 | base | Cyclic loop |

### Why the previous app didn't stream

The phone app in the capture had per-device hardcoded B0 04 tail bytes for stages 3–5
(probably stored at first pairing). The web app blindly replayed those capture-specific
values. On the live device, which has **different session-specific tail bytes** in its
`A0 04` prompts, this caused the device to receive incorrect B0 04 content in stages 3, 4, 5.  

Evidence: the live device emits `A0 04` prompts at bases `0x1a` and `0x20` after each of
those stages — extra "retry/continue" prompts not seen in the capture because the capture
device's B0 04 values were already correct. When stage 3–5 B0 04 content is wrong, the
device's state machine never emits `A0 04 base=0x28` in stage 6. Without that prompt,
the stage 6 streaming-unlock write used synthesized wrong tail bytes. The device ACKed
`0x2c` (confirming receipt) but entered the post-handshake prompt loop instead of streaming.

### Fix applied

- Stages 3, 4, 5 now wait for their corresponding `A0 04` prompts (bases `0x14`, `0x1a`,
  `0x20`) and compute the `B0 04` tail bytes dynamically. Hardcoded capture values are
  kept only as a fallback if the device does not emit the prompt within 400 ms.
- Stage 6 fallback (when `base=0x28` isn't emitted): use `base=0x26` tails directly in
  `buildB004` (not the incorrect `+1/+3` synthesis used before).
- Stage 6 wait for `base=0x28` reduced from 1000 ms to 200 ms (faster fallback path).
- Post-handshake `A0 04` responses deferred 8 seconds after handshake completion to give
  streaming a chance to start before the app enters the prompt loop.

### Files changed (version 2026-03-02.5, cache buster v=29)

- `web/app.js`: stages 3–5 `VENDOR_HANDSHAKE_STAGES` entries gain `promptBase`/`b004Step`
  fields; handshake loop gains a "prompt-driven" branch; stage 6 fallback and timeout fixed.
- `web/index.html`: cache buster bumped to `?v=29`.

---

## tshark access (Docker)

There is no local `tshark` installed in this repo. We use a Docker image:

- `cincan/tshark`

### Basic pattern

From the repo root:

```bash
docker run --rm -v "$PWD":/work -w /work cincan/tshark \
  -r data/pulse-ox.pcapng \
  -Y "<display-filter>" \
  -T fields -e frame.number -e btatt.opcode -e btatt.handle -e btatt.value
```

On Apple Silicon, Docker may print a platform warning (linux/amd64 on arm64). If needed, you can force the platform:

```bash
docker run --platform linux/amd64 --rm -v "$PWD":/work -w /work cincan/tshark ...
```

### Commands used during reverse engineering

**Dump the measurement-start window**

```bash
docker run --rm -v "$PWD":/work -w /work cincan/tshark \
  -r data/pulse-ox.pcapng \
  -Y "frame.number>=1445 && frame.number<=1495 && (btatt.opcode==0x1b || btatt.opcode==0x52 || btatt.opcode==0x12)" \
  -T fields -e frame.number -e btatt.opcode -e btatt.handle -e btatt.value
```

Notes:

- `0x1b` = ATT Handle Value Notification
- `0x52` = Write Command (Write Without Response)
- `0x12` = Write Request (rare here; included to be safe)

**Show only `A0 04` prompts**

```bash
docker run --rm -v "$PWD":/work -w /work cincan/tshark \
  -r data/pulse-ox.pcapng \
  -Y "btatt.value[0:2]==a0:04" \
  -T fields -e frame.number -e btatt.handle -e btatt.value
```

In the capture this returns only three prompts: `base=0x14`, `0x26`, `0x28`.

---

## Suggested next debugging steps (for the next expert)

These are the highest-leverage avenues based on the evidence so far:

1. **Confirm whether the phone app enables notifications on any other characteristic(s)**

   We assumed the vendor stream is only on `7274782e-...` and only on handle `0x0027`.

   Re-check the capture for:

   - CCCD writes (Client Characteristic Configuration) on other handles
   - Any other notification handles besides `0x0027`

2. **Look for any post-handshake writes in the capture outside the stage6 window**

   In the immediate measurement-start window, there are no `A0 04` prompts after ACK `0x2C` and no extra writes.

   But it’s possible the phone app sends something earlier/later (or on reconnect) that “arms” streaming.

3. **Reconcile the live device’s post-handshake prompt loop with the capture**

   Hypotheses:

   - The prompt loop is a different “mode” (maybe configuration/keepalive) and the phone app ignores it.
   - The prompt loop is a flow-control mechanism and we’re supposed to respond differently.
   - Streaming is gated on a state bit that is set by a write we don’t yet know.

4. **Experiment: never respond to post-handshake prompts**

   The app currently defers prompt responses for a short window. Try longer defers (e.g., 30–60s) or completely disable prompt responses.

5. **If possible: capture the live session used with the web app**

   A new pcap that includes the prompt loop would be extremely valuable. Even a BLE HCI snoop from macOS isn’t trivial, but an Android phone capture paired with the same oximeter firmware would help.

---

## Key implementation locations

- `web/app.js`
  - Connect + service/characteristic discovery
  - Notification handler:
    - ACK (`A0 03`) buffering and stage wait
    - Prompt (`A0 04`) buffering and optional post-handshake responder
    - Measurement decode for 20-byte `A0 11 ..` frames
  - Stage 6 special-case logic:
    - waits for prompt base `0x26` and (optionally) `0x28`
    - if `0x28` is missing, synthesizes the `0x28` tail based on the pcap relationship

---

## Suggested next debugging steps

The "Suggested next debugging steps" section below is now partially superseded by the root cause
section above. If streaming still doesn't work after testing v2026-03-02.5, the remaining avenues are:

1. **Capture a new pcap with the live device** — compare it frame-by-frame against the reference.
   A new capture would immediately show whether the stages 3–5 B0 04 writes are now correct.

2. **Check `A0 11` frame decoding** — the decoder currently filters for `config == 0x00E8` and
   reasonable SpO₂/HR values. If the live device uses a different config word or different field
   positions, frames would arrive but be silently discarded. Add a catch-all log for any
   20-byte `A0 11` notification regardless of type.

3. **Never respond to post-handshake prompts** — try removing the 8-second defer and indefinitely
   ignoring all post-handshake `A0 04` prompts to see if the device streams on its own.

---

## Continuation plan

### Pending Task 1 — Test version 2026-03-02.5

Connect and watch the log for:

1. `Handshake stage3: got prompt base=0x14 tail=...` and a matching `write (dynamic)` line
2. Same for stage4 (`0x1a`) and stage5 (`0x20`)
3. `Handshake stage6: got prompt base=0x28 tail=...` ← **new, critical** (previously never seen)
4. `Vendor handshake complete`
5. `Measurement frames started (A0 11 0xf0 …)` ← success condition

If stages 3–5 logs say `no prompt ... using capture-hardcoded B0 04`, something is wrong with
the prompt buffering. If base=0x28 is still not seen, the earlier B0 04 content may still be wrong.

### Pending Task 2 — If streaming still doesn't start

See "Suggested next debugging steps" above.

---

## Current status (as of app version 2026-03-02.5)

- ✅ Connect/discover/subscribe reliably
- ✅ Handshake stages 1–6 complete; stage6 ACK `0x2C` observed live
- ✅ Stages 3–5 `B0 04` now computed dynamically from device prompts
- ✅ Stage 6 fallback corrected (uses `base=0x26` tails directly)
- 🔵 Untested: whether the fix causes the device to emit `A0 04 base=0x28` and start `A0 11 F0` streaming

