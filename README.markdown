# BLE Health Hub

## Handoff notes

If you're picking this up fresh (or handing off to another expert), start with:

- `HANDOFF.markdown` (protocol findings, current blocker, reproduction steps, and `tshark` via Docker commands)

## Finding the Pulse Ox

Use these Wireshark display filters to get rid of the noise:

```
btle.advertising_header.pdu_type == 0x0
&& !_ws.malformed 
&& !btcommon.eir_ad.entry.device_name ~ "ROL.*|Govee.*|ihome.*|MRBL.*" 
&& !btcommon.eir_ad.entry.company_id ~ "Sonos.*|Apple.*|Hunter.*|Samsung.*" 

&& btle.advertising_address == 54:14:a7:e0:d1:53
```

## Sniff a conversation

1. In Wireshark, under View > Interface Toolbars enable the `nRF Sniffer for Bluetooth LE` option
1. Set the `Key` Drop down to `Follow LE address`
1. Set the `Value` to `54:14:a7:e0:d1:53 public`
1. Begin recording while the Pulse Ox is still advertising (not connected).  If it's already connected you won't see anything.
1. Pair with the iPhone app, and the conversation will be recorded.

A reference recording can be found in `data/pulse-ox.pcapng`

## Sample Web Bluetooth app

A minimal sample app is included in `web/` that:

* uses the Web Bluetooth API to connect to the pulse ox
* subscribes to live measurement notifications
* decodes the vendor-specific 20-byte frames (documented below)

### Run it

Web Bluetooth requires a secure context. The easiest option is localhost.

1. Start a local server:

    ```sh
    cd web
    python3 -m http.server 8000
    ```

2. Open the app in a Chromium browser (Chrome / Edge):

    `http://localhost:8000`

3. Click **Connect** and select the pulse oximeter.

### Troubleshooting

* If changes to the web app don't seem to take effect, do a hard reload in Chrome
    (or open DevTools and disable cache) so the latest `web/app.js` is used.
* If you've previously granted Bluetooth permissions for this site without the
    vendor service, remove the site permissions in Chrome settings and reconnect.

### BLE details

The device advertises a vendor-specific primary service:

* Service UUID: `56313100-504f-6e2e-6175-696a2e6d6f63`

Some tools show the same 128-bit UUID with a different byte order:

* Alternate display: `00313156-4f50-2e6e-6175-696a2e6d6f63`

In Wireshark you may also see the raw 16 bytes as:

* `636f6d2e6a6975616e2e504f56313100` (ASCII-ish: `com.jiuan.POV11\0`)

The sample app will try all of these representations during service discovery.

## Vendor APK (MyVitals)

The reverse engineering work in this repo references the Android app APK (e.g. “iHealth MyVitals 4.13.1”). That APK is a large binary and usually proprietary.

- This repo does **not** commit the APK into git (GitHub blocks files >100MB, and committing third‑party binaries is often not appropriate).
- If you are allowed to redistribute the APK to your collaborators, publish it as a **GitHub Release asset** instead of committing it.

**Recommended workflow (Release assets + checksum)**

1. Keep the APK out of git (it’s listed in `.gitignore`).
2. Create a GitHub Release (tag like `vendor-myvitals-4.13.1`).
3. Upload the APK as a Release asset.
4. Record the SHA256 in the release notes so others can verify they downloaded the exact same binary.

To compute SHA256 locally:

```sh
shasum -a 256 "iHealth MyVitals_4.13.1_APKPure.apk"
```

If you do **not** have redistribution rights, the safe alternative is to store only:

- the app version string,
- where to obtain it (official source), and
- the SHA256 hash for verification.

The app filters by that service, then enumerates characteristics under it and
auto-selects the characteristic that emits frames beginning with `A0 11 F0`.

If the pulse ox does not appear in Chrome's chooser, it's often because the
device isn't advertising the service UUID at that moment (or it's already
connected to the phone app). The sample app uses `acceptAllDevices: true` and
then validates the selected device actually exposes the vendor service after
connecting.

# Vendor Pulse Oximeter – BLE Frame Format (Reverse Engineered)

This document describes the decoded structure of 20-byte live measurement frames emitted by the vendor-specific pulse oximeter over BLE notifications.

The device does **not** use the standard Bluetooth SIG Pulse Oximeter Service (0x1822). Instead, it transmits proprietary 20-byte frames.

---

## Frame Overview

Each BLE notification contains **exactly 20 bytes**:

```
Offset  Size  Description
------  ----  ----------------------------------------
0       3     Frame header (constant: A0 11 F0)
3       1     Sequence counter (increments by 2)
4       4     Session/device identifier (constant per session)
8       12    Measurement payload
```

Example frame:

```
a011f098aca76146062c00e80391053905d4044b
```

---

## Header

| Bytes | Value              | Meaning                                                |
| ----- | ------------------ | ------------------------------------------------------ |
| 0–1   | `A0 11`            | Frame marker                                           |
| 2     | e.g. `F0`          | Message type (measurements commonly use `0xF0`)         |
| 3     | Incrementing byte  | Sequence counter (increments by 2)                     |
| 4–7   | e.g. `AC A7 61 46` | Session or device identifier (constant during session) |

The sequence counter increments by 2 per frame and can be used for ordering and duplicate detection.

---

## Measurement Payload (12 bytes)

```
Offset  Size  Field
------  ----  ------------------------------------
8       1     Signal strength (0–8)
9       1     SpO₂ (encoding varies by firmware; see below)
10–11   2     Constant/config field (usually 00 E8)
12–13   2     Pulse rate ×10 (big-endian uint16)
14–15   2     Pleth waveform sample #1 (uint16 BE)
16–17   2     Pleth waveform sample #2 (uint16 BE)
18–19   2     Pleth waveform sample #3 (uint16 BE)
```

---

## Field Decoding Details

### 1. Signal Strength (`byte 8`)

Range observed: `0–8`

Represents signal quality / perfusion strength (similar to bar indicator on device display).

* `0` → weak or no signal
* `6–8` → strong signal

---

### 2. SpO₂ (`byte 9`)

SpO₂ encoding varies across captures/firmwares.

Observed encodings:

* **Direct percent** (byte9 already equals SpO₂), e.g. raw `0x5B–0x64` → `91–100%`
* **Compressed range**: live device report where raw `19` corresponded to `97%`.

The web decoder implements this deterministically (no guessing):

* If `byte9` is already in `50–110`, treat it as **direct percent**.
* Else if `byte9` is in `0–30`, treat it as **compressed** and decode as:
    `SpO₂ (%) = byte9 + 78`

Observed values:

| Raw  | Decoded |
| ---- | ------- |
| 0x61 | 97%     |
| 0x5E | 94%     |
| 0x5B | 91%     |
| 0x13 | 97%     |

This matches live readings during capture.

---

### 3. Constant Field (`bytes 10–11`)

Almost always:

```
00 E8  (decimal 232)
```

Purpose unknown. Likely:

* sampling rate constant
* configuration value
* device profile indicator

This field did not vary in live measurement captures.

---

### 4. Pulse Rate (`bytes 12–13`)

Big-endian unsigned 16-bit integer.

```
HR (bpm) = uint16_be / 10
```

Examples:

| Raw   | Decimal | HR       |
| ----- | ------- | -------- |
| 03 C3 | 963     | 96.3 bpm |
| 03 15 | 789     | 78.9 bpm |
| 03 AB | 939     | 93.9 bpm |

---

### 5. Plethysmography Waveform (`bytes 14–19`)

Three big-endian uint16 values per frame.

Each frame contains **3 waveform samples**, allowing high-resolution pleth capture.

Typical range observed:

* 0 – ~1500
* Zero values when signal lost

Example:

```
05 39 05 D4 04 4B
→ [1337, 1492, 1099]
```

---

## Signal Loss Behavior

Observed behaviors during weak or lost contact:

* Signal strength drops to `0`
* Pleth samples may become `[0, 0, 0]`
* HR and SpO₂ may temporarily retain last computed values

Therefore:

**Pleth == 0 does NOT necessarily mean SpO₂/HR are invalid.**

Recommended validity heuristic:

```
invalid if:
  signal_strength == 0
  OR pleth all zeros
  OR pleth amplitude below threshold
```

---

## Duplicate Frames

Identical frames were observed in capture logs.

Likely causes:

* BLE notification retransmission
* Buffer duplication in capture layer

Consumers should de-duplicate using:

```
(sequence, payload)
```

---

## Python Reference Decoder

```python
from dataclasses import dataclass

@dataclass
class OxFrame:
    seq: int
    session_hex: str
    strength: int
    spo2: int
    hr_bpm: float
    pleth: list[int]
    raw_hex: str

def decode_vendor_oximeter_frame(hexstr: str) -> OxFrame:
    b = bytes.fromhex(hexstr)
    if len(b) != 20:
        raise ValueError(f"Expected 20 bytes, got {len(b)}")

    if not (b[0] == 0xA0 and b[1] == 0x11 and b[2] == 0xF0):
        raise ValueError("Unexpected header")

    seq = b[3]
    session_hex = b[4:8].hex()

    strength = b[8]
    spo2_raw = b[9]

    # Deterministic decode (matches web/app.js):
    # - direct percent if it already looks like a percent
    # - else compressed range (0..30) representing (SpO2 - 78)
    if 50 <= spo2_raw <= 110:
        spo2 = spo2_raw
    elif 0 <= spo2_raw <= 30:
        spo2 = spo2_raw + 78
    else:
        spo2 = spo2_raw

    hr10 = int.from_bytes(b[12:14], "big")
    hr_bpm = hr10 / 10.0

    pleth = [
        int.from_bytes(b[14:16], "big"),
        int.from_bytes(b[16:18], "big"),
        int.from_bytes(b[18:20], "big"),
    ]

    return OxFrame(
        seq=seq,
        session_hex=session_hex,
        strength=strength,
        spo2=spo2,
        hr_bpm=hr_bpm,
        pleth=pleth,
        raw_hex=hexstr,
    )
```

---

## Summary

The device transmits:

* Proprietary 20-byte BLE frames
* SpO₂ in `byte 9` (encoding varies by firmware)
* Pulse rate encoded as bpm ×10
* 3 pleth waveform samples per frame
* Signal strength indicator
* Fixed session ID and config field

The format is stable across captures and suitable for real-time decoding and waveform reconstruction.

---

If you'd like, I can also produce:

* A waveform reconstruction example (Matplotlib / JS)
* A BLE integration guide
* A clean protocol diagram
* A formal spec version (v1.0 document style)
