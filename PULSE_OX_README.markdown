# Pulse Oximeter (PO3) — BLE Protocol + Reverse Engineering Notes

This repo contains a working Web Bluetooth demo and a reverse-engineered BLE protocol description for a vendor pulse oximeter (marketed/handled by iHealth/MyVitals as **PO3**).

The device does **not** use the Bluetooth SIG Pulse Oximeter service (0x1822). It uses a vendor GATT service/characteristic and a proprietary packet protocol (short `A0 ..` notifications + `B0 ..` writes).

## Sniffing (Wireshark)

See [WIRESHARK_SNIFFING_README.markdown](WIRESHARK_SNIFFING_README.markdown) for a repeatable Wireshark + nRF Sniffer workflow and a starting advertising-only noise filter.

## Project goals

- Connect from Chromium (Web Bluetooth)
- Complete the vendor handshake
- Subscribe to live measurement notifications
- Decode and display:
  - SpO₂
  - Heart rate
  - Pleth waveform (3 points per frame)
  - PI (perfusion index)

## Where to look in this repo

- PCAPs:
  - `data/pulse-ox.pcapng`
  - `data/pulse-ox-2.pcapng`
  - `data/pulse-ox-3-with-finger-loss-and-disconnect.pcapng`
  - (More in `data/`)
- Web Bluetooth demo:
  - `web/index.html`, `web/app.js`, `web/style.css`
- Tools (parsing / verification / helper scripts):
  - `tools/verify_po3_identify_response.py` (offline verifier against a PCAP)
  - `tools/analyze_pcap2.py`, `tools/analyze_pcap2_v2.py` (pattern scanners)
  - `tools/extract_libihealth_rodata.py`, `tools/inspect_libihealth_getkey_selectors.py` (native library inspection)

## Quick start (Web Bluetooth demo)

Web Bluetooth requires a secure context; localhost is easiest.

```sh
cd web
python3 -m http.server 8000
```

Open `http://localhost:8000` in Chrome/Edge, click **Connect**, and select the pulse oximeter.

## BLE / GATT details

### Vendor service UUID

The device advertises a vendor-specific primary service. Different tools/stacks may display the same 128-bit UUID in different byte orders; the demo tries multiple representations:

- `56313100-504f-6e2e-6175-696a2e6d6f63`
- `00313156-4f50-2e6e-6175-696a2e6d6f63`
- `636f6d2e-6a69-7561-6e2e-504f56313100`

Wireshark may show the raw bytes as:

- `636f6d2e6a6975616e2e504f56313100` (ASCII-ish: `com.jiuan.POV11\0`)

### Vendor characteristic UUID

The characteristic used for both writes and notifications is:

- `7274782e-6a69-7561-6e2e-504f56313100`

Observed properties:

- `write`
- `writeWithoutResponse`
- `notify`

## Protocol overview

The device protocol is a sequence of small binary frames. All examples below are hex.

### Message families

Device → Central (notifications):

- `A0 03 ...` — ACKs (step-based)
- `A0 04 ...` — prompts/challenges (provide two tail bytes used in the next response)
- `A0 06 ...` — observed around finger/no-finger transitions on some captures
- `A0 11 ...` — 20-byte messages (some are handshake/status; `A0 11 F0 ...` is live measurement)

Central → Device (writes):

- `B0 11 ...` — Identify stage 1 / stage 2 (session-derived)
- `B0 06 ...` — Identify companion write (session-derived)
- `B0 03 ...` — short step write
- `B0 04 ...` — prompt response

### Handshake stages (what matters)

The vendor app performs a staged handshake. At a high level:

1. **Identify (stage 1 / stage 2)**: the first long writes include session-specific bytes. We reverse engineered and reproduced these using the same `IdentifyIns` logic and key material as the vendor app.
2. **Prompt-driven stages (3–6)**: the device emits `A0 04` prompts that include two tail bytes; those bytes must be echoed into the next `B0 04` response.
3. **Streaming**: after the stage-6 ACK (`A0 03 ... step=0x2C`), the device emits repeating `A0 11 F0` measurement frames when a finger is detected.

Important protocol rule (from captures + confirmed in the working demo):

- Every `B0 04` at step `S` is a response to the preceding `A0 04` prompt at **base** `S-3`.
- The prompt’s tail bytes are not optional; they are part of what makes the `B0 04` acceptable to the device.

## Live measurement frames (APK-backed)

The official MyVitals 4.13.1 APK decodes PO3 live streaming via:

`Po3Control → AcInsSet.haveNewData(...) → POMethod.c(byte[]) → POMethod.a(int[])` (action: `liveData_po`).

On-wire, the device sends 20-byte notifications of the form `A0 11 F0 ...`. Those 20 bytes contain the APK’s 13-byte `POMethod` payload starting at byte offset **6**.

### `A0 11 F0` (20 bytes) layout

```text
offset  meaning                         type
------  ------------------------------  -----------------
0       0xA0                            frame prefix
1       0x11                            length (20 bytes)
2       0xF0                            measurement type
3       seq                             u8
4..5    opaque header bytes             u8[2]
6       SpO2 (%)                        u8 (unsigned)
7       HR (bpm)                        u8 (unsigned)
8       pulse strength                  u8 (0..8 typical)
9..10   PI numerator                    u16 little-endian
11..12  PI denominator                  u16 little-endian
13..14  pulseWave[0]                    u16 little-endian
15..16  pulseWave[1]                    u16 little-endian
17..18  pulseWave[2]                    u16 little-endian
19      checksum / trailing byte        u8 (not needed for decoding)
```

Notes:

- There is **no** `+78` mapping for SpO₂ in this APK-backed decoder; SpO₂ is read directly as an unsigned byte.
- PI computation matches the APK:

  $$pi = \mathrm{round}((piNum/piDen)\cdot 1000) / 10$$

  Only accept values in the range $[0.2, 20.0]$; otherwise fall back to the last known-good PI.

## How we reverse engineered this (only the important steps)

This is the “why” behind the final protocol spec above.

1. **Started from PCAPs (Wireshark) to discover the vendor protocol**
   - Found that the device advertises a vendor 128-bit service (POV11) rather than standard 0x1822.
   - Noticed all traffic on a single characteristic with write+notify.
   - Identified repeated frame prefixes (`A0 03`, `A0 04`, `A0 11`, `B0 03`, `B0 04`, `B0 06`, `B0 11`).

2. **Built a step-based handshake state machine from ACKs**
   - `A0 03 A0 <step> ...` ACK step values advance the handshake.
   - Stage boundaries are visible in the PCAP as deterministic step numbers (ending at `0x2C`).

3. **Resolved the “session-varying bytes” problem by following prompts**
   - Some writes can’t be hardcoded from one capture: the device validates session-specific tail bytes.
   - The key insight was that `A0 04` prompt tail bytes must be echoed into the subsequent `B0 04`.
   - Implemented prompt-driven `B0 04` generation in the web demo; this unblocked reliable handshakes.

4. **Reproduced Identify stage 1/2 using vendor key material**
   - The vendor app’s Identify logic is in the SDK: `IdentifyIns` + `GenerateKap` (JNI `libiHealth.so`).
   - We extracted the PO3 key blob from the native library and reproduced the Identify encryption/framing.
   - Verified correctness using an offline verifier against a vendor PCAP:

     ```sh
     ./.venv/bin/python tools/verify_po3_identify_response.py data/pulse-ox-2.pcapng
     ```

5. **Corrected live measurement decoding using the APK’s PO3 parser path**
   - Early versions of the demo used a capture-derived heuristic decode.
   - We traced the MyVitals PO3 path to `AcInsSet` and `POMethod` and rebuilt the mapping.
   - We then mapped the `POMethod` payload into the on-wire `A0 11 F0` 20-byte frame (payload starts at offset 6).

## Reverse engineering tutorial (detailed + reproducible)

This section is a step-by-step walkthrough of the exact “reverse engineering loop” we used:

1) start from a symptom / unknown (session secrets, SpO₂ decode)
2) find the vendor app code path responsible
3) dump/trace bytecode (DEX) + native code artifacts
4) convert what we see into a deterministic spec + verifier

Everything here is intended to be runnable by a collaborator who has:

- a copy of the vendor APK (kept out of git)
- the extracted `libiHealth` native library (or extracted from the APK)

### Prerequisites

- Python venv with the same dependencies used by the scripts here (`androguard`, `scapy`, `pyelftools`, `capstone`).
- A PO3 PCAP capture (any of the files under `data/`).
- The APK path and/or the `.so` path.

Throughout this tutorial, I’ll use:

- APK path: `"iHealth MyVitals_4.13.1_APKPure.apk"`
- Native library: `vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so`

Adjust as needed.

### Part A — Finding the “secrets” (Identify stage key material)

The high-level idea is:

- The Identify stage (early `B0 11 ...` writes) is generated by the SDK.
- The SDK calls JNI (`GenerateKap.getKey(...)`) to obtain per-device 16-byte blobs.
- For PO3, that blob is used as key seed material for the Identify response.

We want to answer:

1. Where does the app get the key material?
2. What exactly is the PO3 blob?
3. Can we re-generate the stage-2 Identify response and match a PCAP byte-for-byte?

#### A1) Find where `GenerateKap.getKey/getKa` is called

Start with static call-site discovery so you know what code paths to inspect.

1) Scan the whole APK for GenerateKap invocations:

```sh
.venv/bin/python tools/scan_genkap_calls_all.py "iHealth MyVitals_4.13.1_APKPure.apk"
```

2) If you specifically want the argument flow for PO3, print callsites with surrounding context:

```sh
python3 tools/trace_apk_generatekap_calls.py "iHealth MyVitals_4.13.1_APKPure.apk" --only-po3
```

This prints the `invoke-*` instruction and ~N lines of surrounding Dalvik instructions so you can see whether the selector argument is:

- a direct `const-string "PO3"`
- loaded from a field
- produced by a helper method

#### A2) Recover selector strings (“PO3”, etc.)

If the selector is passed as a direct const-string, this is quick:

```sh
python3 tools/scan_apk_generatekap_selectors.py "iHealth MyVitals_4.13.1_APKPure.apk"
```

If it’s not, fall back to string xrefs to see where the literal appears:

```sh
python3 tools/trace_apk_string_xrefs.py "iHealth MyVitals_4.13.1_APKPure.apk" PO3 --context 20
```

The output gives you candidate classes/methods to dump.

#### A3) Inspect `libiHealth.so` to locate selectors + key blobs

The native function `Java_com_ihealth_communication_ins_GenerateKap_getKey` typically:

- compares a selector string (e.g. `"PO3"`)
- then loads a pointer to a 16-byte blob via a Thumb “literal pool + add pc” pattern

There are two complementary scripts here:

1) Recover selector strings referenced by that function region:

```sh
python3 tools/inspect_libihealth_getkey_selectors.py \
  vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so \
  --start 0x1564 --stop 0x15d0
```

2) Dump the 16-byte blobs referenced by that function region:

```sh
python3 tools/extract_libihealth_getkey_blobs.py \
  vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so \
  --start 0x21f0 --stop 0x2270
```

Notes:

- The `--start/--stop` ranges are intentionally manual. In practice you iterate in chunks to avoid huge output.
- If you already know you’re targeting PO3 specifically, there’s also a “surgical” extractor:

```sh
python3 tools/elf_extract_po3_blob.py vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so
```

That script bakes in the previously-reversed PO3 branch/literal addresses for a known library version.

#### A4) Verify we have the correct PO3 blob (stage-2 Identify)

Once you have a candidate PO3 blob, validate it against a vendor capture.

```sh
.venv/bin/python tools/verify_po3_identify_response.py data/pulse-ox-2.pcapng
```

This script:

- extracts the Identify challenge bytes from the PCAP
- extracts the app’s `AC FC ...` response bytes from the PCAP
- recomputes the response using the reverse-engineered `IdentifyIns` / XXTEA logic
- compares byte-for-byte

If it matches, you’ve found the correct key material and reconstruction.

### Part B — Pulling out the SpO₂ calculation (and the whole measurement map)

The question we wanted to answer was:

> Is SpO₂ a “weird mapping” (like `+78`), or is it a direct percentage?

The vendor app is authoritative here because it must display the real number.

#### B1) Find the PO3 live-data parser path

There are multiple ways to find it; the simplest is to anchor on the onNotify event name.

In this APK, the live measurement callback key is `liveData_po`.

Search for that string:

```sh
python3 tools/trace_apk_string_xrefs.py "iHealth MyVitals_4.13.1_APKPure.apk" liveData_po --context 25
```

When you find a candidate class/method, dump it.

#### B2) Dump the key methods (`AcInsSet.haveNewData`, `POMethod.c`, `POMethod.a`)

To dump a Dalvik method, you need to know which DEX contains its class.

1) First locate the DEX file:

```sh
.venv/bin/python tools/find_class_dex.py "iHealth MyVitals_4.13.1_APKPure.apk" Lcom/ihealth/communication/ins/POMethod;
.venv/bin/python tools/find_class_dex.py "iHealth MyVitals_4.13.1_APKPure.apk" Lcom/ihealth/communication/ins/AcInsSet;
```

2) Then dump the method bytecode:

```sh
.venv/bin/python tools/dex_dump_method.py "iHealth MyVitals_4.13.1_APKPure.apk" <dexname> \
  Lcom/ihealth/communication/ins/POMethod; c > tmp/dump_POMethod_c.txt

.venv/bin/python tools/dex_dump_method.py "iHealth MyVitals_4.13.1_APKPure.apk" <dexname> \
  Lcom/ihealth/communication/ins/POMethod; a > tmp/dump_POMethod_a.txt

.venv/bin/python tools/dex_dump_method.py "iHealth MyVitals_4.13.1_APKPure.apk" <dexname> \
  Lcom/ihealth/communication/ins/AcInsSet; haveNewData > tmp/dump_AcInsSet_haveNewData.txt
```

(`tmp/` is intentionally ignored by git; these are generated artifacts.)

#### B3) How to read the dump files (what the lines mean)

The `dump_*.txt` files are a straight disassembly of Dalvik bytecode.

Each line is:

- an instruction index (or byte offset)
- opcode name (e.g. `aget-byte`, `and-int/lit16`, `mul-int/lit16`)
- operands (registers + constants)

Rules of thumb:

- `aget-byte vX, vArr, vIdx` means `vX = vArr[vIdx]` (a signed byte in Java, so it’s often followed by `& 0xff`).
- `and-int/lit16 vX, vX, 255` is the classic “unsigned byte” conversion.
- `(hi * 256) + lo` is a byte-pair to u16 conversion.

#### B4) Deriving the SpO₂ decode from `POMethod`

Open the existing dumps in this repo:

- `tmp/dump_POMethod_c.txt`
- `tmp/dump_POMethod_a.txt`

`POMethod.c([B)[I` converts a byte payload into an int array.

Key excerpt (see `tmp/dump_POMethod_c.txt`):

- It allocates an `int[8]`.
- It reads the first three bytes:
  - `v0[0] = (payload[0] & 0xff)`
  - `v0[1] = (payload[1] & 0xff)`
  - `v0[2] = (payload[2] & 0xff)`

Those become:

- `bloodoxygen` (SpO₂)
- `heartrate` (HR)
- `pulsestrength`

There is no range check and no `+78` transformation in this decoder path.
The only transformation is `& 0xff` to treat the Java signed byte as an unsigned $0..255$ integer.

Then it loops 5 times and reads little-endian u16s from the remainder:

- for i in 0..4:
  - `lo = payload[3 + 2*i] & 0xff`
  - `hi = payload[4 + 2*i] & 0xff`
  - `word = (hi * 256) + lo`
  - stored at `out[3+i]`

That yields:

- `out[3]` = PI numerator
- `out[4]` = PI denominator
- `out[5..7]` = 3 pleth waveform samples

Finally, `POMethod.a([I)->JSONObject` maps those array indices into JSON keys.
From the dump (see `tmp/dump_POMethod_a.txt`):

- `bloodoxygen = arr[0]`
- `heartrate = arr[1]`
- `pulsestrength = arr[2]`
- `pi` is computed from `arr[3] / arr[4]` with scaling and clamping
- `pulseWave = [arr[5], arr[6], arr[7]]`

#### B5) Mapping the APK’s 13-byte payload onto the on-wire 20-byte `A0 11 F0` frame

The PCAP shows the device emits 20-byte frames that begin with `a0 11 f0`.

Separately, the APK code expects a 13-byte payload:

- 3 x u8 (SpO₂, HR, strength)
- 5 x u16le (PI numerator, PI denominator, wave0, wave1, wave2)

The key PCAP-backed observation is:

- The APK’s 13 bytes appear *verbatim* starting at offset 6 of the on-wire 20-byte `A0 11 F0` notification.

That’s why the final decoder uses:

- `spo2 = frame[6]`
- `hr = frame[7]`
- `strength = frame[8]`
- `piNum = u16le(frame[9..10])`, `piDen = u16le(frame[11..12])`
- `wave0..2 = u16le(frame[13..18])`

### Part C — Understanding the other dump files in `tmp/`

The `tmp/dump_*.txt` files are snapshots of bytecode dumps for specific methods.

Common ones:

- `tmp/dump_AcInsSet_haveNewData.txt` — shows the dispatch point where measurement payloads are parsed and emitted as `liveData_po`.
- `tmp/dump_Po3Control_startMeasure.txt` — shows how `startMeasure()` registers for `ready_measure_po`, `resultData_po`, etc.

If you want to regenerate any dump, use [tools/find_class_dex.py](tools/find_class_dex.py) + [tools/dex_dump_method.py](tools/dex_dump_method.py) as shown above.

## Tools directory index

This is a brief “what is this script for?” index for everything under `tools/`.

### PCAP / protocol analysis

- [tools/analyze_pcap2.py](tools/analyze_pcap2.py)
  - One-off deep dump for `data/pulse-ox-2.pcapng` using ATT handle parsing; prints writes, notifications, and highlights key vendor message types.
- [tools/analyze_pcap2_v2.py](tools/analyze_pcap2_v2.py)
  - More robust PCAP scanner: pattern-searches raw packet bytes for vendor frame prefixes without relying on a fixed DLT/ATT layout.
- [tools/extract_vendor_frames.py](tools/extract_vendor_frames.py)
  - Extracts validated vendor frames (`A0`/`B0`) from a `.pcapng` by using the protocol’s length+checksum rule.
- [tools/gen_vendor_pcap_readme.py](tools/gen_vendor_pcap_readme.py)
  - Generates Wireshark-like timelines from `data/*.pcapng` using `tshark` in Docker (useful for new captures; the curated timelines live in this README).
- [tools/pcap_all.py](tools/pcap_all.py)
  - Legacy, hardcoded extractor for handle `0x0027` on `data/pulse-ox.pcapng`; prints a focused timeline around the stage-6/streaming transition.
- [tools/pcap_seq.py](tools/pcap_seq.py)
  - Legacy, hardcoded script to list early notifications (ACK/prompt/`A0 11` types) on `data/pulse-ox.pcapng`.
- [tools/pcap_writes.py](tools/pcap_writes.py)
  - Legacy, hardcoded script to list writes + notifications on handle `0x0027` in `data/pulse-ox.pcapng`.
- [tools/pcap_devices.py](tools/pcap_devices.py)
  - Legacy helper: scans for BD_ADDR byte patterns and guesses HCI connection handles. Useful when you’re unsure what a capture contains.
- [tools/find_mac.py](tools/find_mac.py)
  - Searches a PCAP for the target MAC in either byte order; mostly useful as a sanity check.
- [tools/analyze_session_tokens.py](tools/analyze_session_tokens.py)
  - Attempts to correlate early “session-varying” write suffix bytes against BLE CONNECT_IND parameters (rules out trivial derivations).

### APK / DEX reverse engineering

- [tools/find_class_dex.py](tools/find_class_dex.py)
  - Given an APK + class name, tells you which `classes*.dex` contains it.
- [tools/dex_dump_method.py](tools/dex_dump_method.py)
  - Dumps Dalvik bytecode instructions for a specific class+method inside a given DEX.
- [tools/scan_callsites.py](tools/scan_callsites.py)
  - Generic DEX callsite scanner: finds which methods `invoke-*` a target method substring.
- [tools/trace_apk_string_xrefs.py](tools/trace_apk_string_xrefs.py)
  - Finds `const-string` references to an exact string and prints surrounding instruction context.
- [tools/scan_apk_generatekap_selectors.py](tools/scan_apk_generatekap_selectors.py)
  - Best-effort extractor for selector strings passed into `GenerateKap.getKey/getKa` when they are direct const-strings.
- [tools/scan_genkap_calls_all.py](tools/scan_genkap_calls_all.py)
  - Broad scan: lists methods containing invocations of `GenerateKap` APIs (key discovery step).
- [tools/trace_apk_generatekap_calls.py](tools/trace_apk_generatekap_calls.py)
  - Verbose scanner: prints the exact `GenerateKap` callsites and the surrounding Dalvik instructions to understand argument flow.

### Native library reverse engineering (`libiHealth.so`)

- [tools/extract_libihealth_rodata.py](tools/extract_libihealth_rodata.py)
  - Extracts referenced C-strings by disassembling a function region and resolving literal-address references back into ELF sections.
- [tools/inspect_libihealth_getkey_selectors.py](tools/inspect_libihealth_getkey_selectors.py)
  - Recovers selector strings used by `GenerateKap.getKey` by resolving Thumb PIC pointer patterns.
- [tools/extract_libihealth_getkey_blobs.py](tools/extract_libihealth_getkey_blobs.py)
  - Extracts the 16-byte blobs returned by `GenerateKap.getKey` for various selectors by resolving Thumb PIC pointers.
- [tools/elf_extract_po3_blob.py](tools/elf_extract_po3_blob.py)
  - Version-specific “surgical” extractor for the PO3 blob (uses known addresses for the PO3 case).

### Verification / utilities

- [tools/verify_po3_identify_response.py](tools/verify_po3_identify_response.py)
  - Offline verifier: recomputes stage-2 Identify response and compares it against a PCAP capture.
- [tools/inspect_ihealth_sdk_jar.py](tools/inspect_ihealth_sdk_jar.py)
  - Scans iHealth SDK JARs without decompiling: constant-pool UTF8 strings, byte-pattern matches, and crypto API references.
- [tools/sha256_file.py](tools/sha256_file.py)
  - Convenience SHA256 calculator for verifying large binaries (APKs, `.so`s).

### Java harness

- [tools/java_generatekap_harness/](tools/java_generatekap_harness/)
  - Small Java harness used during reverse engineering to sanity-check/replicate SDK-side `GenerateKap` behavior.

## PCAP timelines (preserved) + annotations

These sections preserve the raw PCAP-derived timelines (verbatim values). Above each raw block is a small annotated table showing what we now know about the important steps.

Direction glyphs:

- 🟦C→P = Central write
- 🟩P→C = Peripheral notification

### data/pulse-ox-2.pcapng

Start: frame 838 at capture t=14.975502s (shown below as t=0.000000). Matched start write: `b0111101acfa293dbdd4d28fb10f5a953ab8d182`

Key events:

| Time (s) | Dir | Value | Meaning |
| --- | --- | --- | --- |
| 0.000000 | 🟦C→P | `b0 11 11 01 ac fa ...` | Identify stage 1 write (marker `FA`) |
| 0.160879 | 🟦C→P | `b0 11 11 0f ac fc ...` | Identify stage 2 write (marker `FC`) |
| 0.201071 | 🟩P→C | `a0 04 00 14 ac fd bd` | Prompt base `0x14` (tail bytes `fd bd`) → drives `B0 04 step=0x17` |
| 1.080005 | 🟦C→P | `b0 04 00 17 ac c1 84` | Prompt response (`B0 04`) at step `0x17` |
| 2.800206 | 🟩P→C | `a0 04 00 28 ac a6 7a` | Prompt base `0x28` (tail bytes `a6 7a`) → drives final `B0 04 step=0x2B` |
| 2.880453 | 🟦C→P | `b0 04 00 2b ac a6 7d` | Final prompt response (`B0 04`) at step `0x2B` |
| 3.120208 | 🟩P→C | `a0 11 f0 32 ac a7 60 47 00 61 00 e8 03 f4 06 30 06 4a 05 e7` | First live measurement frame: SpO₂=96, HR=71, strength=0 |

Annotated timeline (semantic, row-by-row):

```text
Time          Dir    Value                                                                 Meaning
------------  -----  --------------------------------------------------------------------  ----------------------------------------
0.000000      🟦C→P   b0 11 11 01 ac fa 29 3d bd d4 d2 8f b1 0f 5a 95 3a b8 d1 82          Identify stage 1 write (`B0 11`, marker `FA`; session-derived)
0.000495      🟦C→P   b0 06 10 03 ac c7 b5 1c 5 b0 06 10 03 ac c7 b5 1c 577                 Identify companion write (`B0 06`; session-derived; sent alongside Identify)
0.040193      🟩P→C   a0 03 a1 02 ac 4f                                                     ACK (`A0 03`) for early handshake step (`step=0x02`)
0.041071      🟩P→C   a0 11 33 06 ac fb 32 33 31 32 32 33 30 30 30 30 30 31 34 62          Handshake/status payload (`A0 11`, type `0x33`) emitted during Identify
0.041566      🟩P→C   a0 11 32 08 ac 31 34 34 8d 69 5c 07 36 90 33 a2 28 67 7b 7d          Handshake/status payload (`A0 11`, type `0x32`) emitted during Identify
0.120000      🟦C→P   b0 03 a3 07 ac 56                                                     Short step write (`B0 03`) advancing handshake (`step=0x07`)
0.120245      🟩P→C   a0 11 31 0a ac f3 51 6d 70 f5 95 d3 4e 25 07 e3 86 05 c9 16          Handshake/status payload (`A0 11`, type `0x31`) in response to step advances
0.120548      🟦C→P   b0 03 a2 09 ac 57                                                     Short step write (`B0 03`) advancing handshake (`step=0x09`)
0.120792      🟩P→C   a0 0a 30 0c ac f7 93 c1 7c 0a 19 f0 c2                              Short device status/config message (`A0 0A`, type `0x30`) during handshake
0.160001      🟦C→P   b0 03 a1 0b ac 58                                                     Short step write (`B0 03`) advancing handshake (`step=0x0B`)
0.160440      🟦C→P   b0 03 a0 0d ac 59                                                     Short step write (`B0 03`) advancing handshake (`step=0x0D`)
0.160879      🟦C→P   b0 11 11 0f ac fc 44 f3 bd 2f 8d 8d 97 0d b9 53 f5 46 5d 4d          Identify stage 2 write (`B0 11`, marker `FC`; session-derived)
0.161374      🟦C→P   b0 06 10 11 ac 07 03 0f e6                                           Identify companion write (`B0 06`) paired with stage-2 Identify
0.200193      🟩P→C   a0 03 a1 10 8c 5d                                                     ACK (`A0 03`) indicating Identify accepted / handshake progresses (`step=0x10`)
0.201071      🟩P→C   a0 04 00 14 ac fd bd                                                  Prompt/challenge (`A0 04`) base `0x14`; tail bytes `fd bd` must be echoed in next `B0 04`
0.240001      🟦C→P   b0 03 a0 15 ac 61                                                     Short step write (`B0 03`) advancing prompt-driven sequence (`step=0x15`)
1.080005      🟦C→P   b0 04 00 17 ac c1 84                                                  Prompt response (`B0 04`) at `step=0x17` for base `0x14` prompt (`step = base + 3`)
1.120198      🟩P→C   a0 03 a0 18 ac 64                                                     ACK (`A0 03`) acknowledging prompt response (`step=0x18`)
1.120637      🟩P→C   a0 05 00 1a ac c1 3c c3                                               Prompt-like/status message (`A0 05`) at base/step `0x1A` (drives next `B0 04 step=0x1D`)
1.200006      🟦C→P   b0 03 a0 1b ac 67                                                     Short step write (`B0 03`) advancing prompt-driven sequence (`step=0x1B`)
1.840009      🟦C→P   b0 04 00 1d ac c6 8f                                                  Prompt response (`B0 04`) at `step=0x1D` responding to prior base `0x1A` message
1.960010      🟦C→P   b0 03 a0 21 ac 6d                                                     Short step write (`B0 03`) advancing prompt-driven sequence (`step=0x21`)
2.040010      🟦C→P   b0 03 a0 21 ac 6d                                                     Retry of short step write (`B0 03`) at `step=0x21` (duplicate write)
2.640013      🟦C→P   b0 04 00 23 ac a5 74                                                  Prompt response (`B0 04`) at `step=0x23` (corresponding prompt not shown in this excerpt)
2.680206      🟩P→C   a0 03 a0 24 ac 70                                                     ACK (`A0 03`) acknowledging prior prompt response (`step=0x24`)
2.680645      🟩P→C   a0 04 00 26 ac a5 77                                                  Prompt/challenge (`A0 04`) base `0x26`; tail bytes `a5 77` must be echoed in subsequent `B0 04`
2.760014      🟦C→P   b0 03 a0 27 ac 73                                                     Short step write (`B0 03`) advancing prompt-driven sequence (`step=0x27`)
2.800206      🟩P→C   a0 04 00 28 ac a6 7a                                                  Prompt/challenge (`A0 04`) base `0x28`; tail bytes `a6 7a` must be echoed in subsequent `B0 04`
2.880014      🟦C→P   b0 03 a0 29 ac 75                                                     Short step write (`B0 03`) advancing prompt-driven sequence (`step=0x29`)
2.880453      🟦C→P   b0 04 00 2b ac a6 7d                                                  Final prompt response (`B0 04`) at `step=0x2B` responding to base `0x28` prompt (ends prompt-driven stages)
3.120208      🟩P→C   a0 11 f0 32 ac a7 60 47 00 61 00 e8 03 f4 06 30 06 4a 05 e7          Live measurement notify (`A0 11 F0`, seq `0x32`): SpO₂/HR/strength + PI + 3 pleth samples
3.280209      🟩P→C   a0 11 f0 34 ac a7 60 47 00 61 00 e8 03 52 04 53 03 5b 02 73          Live measurement notify (`A0 11 F0`, seq `0x34`)
3.680211      🟩P→C   a0 11 f0 3c ac a7 60 47 04 5e 00 e8 43 2b 0b d5 0b 04 0b 98          Live measurement notify (`A0 11 F0`, seq `0x3C`)
3.680706      🟩P→C   a0 11 f0 3e ac a7 60 47 03 5e 00 e8 03 ff 09 ec 08 01 08 79          Live measurement notify (`A0 11 F0`, seq `0x3E`)
3.840211      🟩P→C   a0 11 f0 40 ac a7 60 47 01 5e 00 e8 03 71 07 28 07 fa 06 1b          Live measurement notify (`A0 11 F0`, seq `0x40`)
3.880212      🟩P→C   a0 11 f0 42 ac a7 60 47 00 5e 00 e8 03 be 06 52 06 b6 05 4c          Live measurement notify (`A0 11 F0`, seq `0x42`)
3.960212      🟩P→C   a0 11 f0 44 ac a7 60 47 00 5e 00 e8 03 ec 04 07 04 0b 03 80          Live measurement notify (`A0 11 F0`, seq `0x44`)
4.200258      🟩P→C   a0 11 f0 48 ac a7 60 47 00 5e 00 e8 03 00 00 00 00 00 00 7b          Live measurement notify (`A0 11 F0`, seq `0x48`); pleth samples are all zeros (gap/blank frame)
4.360214      🟩P→C   a0 11 f0 4c ac a7 60 47 04 5e 00 e8 03 12 09 0c 0b 82 0b 42          Live measurement notify (`A0 11 F0`, seq `0x4C`)
4.440215      🟩P→C   a0 11 f0 4e ac a7 60 47 07 5b 00 e8 03 ab 0d 1e 0d 46 0c ba          Live measurement notify (`A0 11 F0`, seq `0x4E`)
4.560215      🟩P→C   a0 11 f0 50 ac a7 60 47 08 5b 00 68 03 46 0b 45 0a 9a 09 cb          Live measurement notify (`A0 11 F0`, seq `0x50`)
4.800217      🟩P→C   a0 11 f0 54 ac a7 60 47 00 5b 00 e8 03 6b 08 e9 07 41 07 2f          Live measurement notify (`A0 11 F0`, seq `0x54`)
4.840217      🟩P→C   a0 11 f0 56 ac a7 60 47 00 5b 00 e8 03 6e 06 70 05 68 04 db          Live measurement notify (`A0 11 F0`, seq `0x56`)
4.960217      🟩P→C   a0 11 f0 58 ac a7 60 47 00 5b 00 e8 03 6c 03 85 02 b0 01 2f          Live measurement notify (`A0 11 F0`, seq `0x58`)
5.040218      🟩P→C   a0 11 f0 5a ac a7 60 47 00 5b 00 e8 03 dd 00 09 00 00 00 70          Live measurement notify (`A0 11 F0`, seq `0x5A`)
5.200219      🟩P→C   a0 11 f0 5c ac a7 60 47 00 5b 00 e8 03 99 01 88 05 1c 0a d9          Live measurement notify (`A0 11 F0`, seq `0x5C`)
5.400219      🟩P→C   a0 11 f0 5e ac a7 60 c7 06 5b 00 e8 03 38 0d 6f 0e 93 0e f6          Live measurement notify (`A0 11 F0`, seq `0x5E`); header bytes differ slightly vs typical frames
5.400714      🟩P→C   a0 11 f0 60 ac a7 60 47 07 5b 00 e8 03 81 0e e0 0d f9 0c 18          Live measurement notify (`A0 11 F0`, seq `0x60`)
5.401209      🟩P→C   a0 11 f0 62 ac a7 60 47 06 5b 00 e8 03 f2 0b 1a 0b 9b 0a 5f          Live measurement notify (`A0 11 F0`, seq `0x62`)
5.520220      🟩P→C   a0 11 f0 64 ac a7 60 47 03 5b 00 e8 03 6c 0a 41 0a f8 09 59          Live measurement notify (`A0 11 F0`, seq `0x64`)
5.640221      🟩P→C   a0 11 f0 66 ac a7 60 47 00 5b 00 e8 03 7e 09 d9 08 12 08 18          Live measurement notify (`A0 11 F0`, seq `0x66`)
5.880222      🟩P→C   a0 11 f0 6a ac a7 60 47 01 5b 00 e8 03 47 04 67 03 8f 02 e1          Live measurement notify (`A0 11 F0`, seq `0x6A`)
5.880717      🟩P→C   a0 11 f0 6c ac a7 60 47 00 5b 00 e8 03 b1 01 dc 00 cc 00 f6          Live measurement notify (`A0 11 F0`, seq `0x6C`)
6.040223      🟩P→C   a0 11 f0 6e ac a7 60 47 01 5b 00 e8 03 dd 02 fe 06 82 0b 0f          Live measurement notify (`A0 11 F0`, seq `0x6E`)
6.240224      🟩P→C   a0 11 f0 72 ac a7 60 47 05 5f 00 e8 03 8b 0e da 0d d3 0c 0a          Live measurement notify (`A0 11 F0`, seq `0x72`)
6.280224      🟩P→C   a0 11 f0 74 ac a7 60 47 05 5f 00 e8 03 bb 0b e0 0a 7d 0a e4          Live measurement notify (`A0 11 F0`, seq `0x74`)
6.400225      🟩P→C   a0 11 f0 76 ac a7 60 47 03 5f 00 e8 03 5c 0a 43 0a ee 09 57          Live measurement notify (`A0 11 F0`, seq `0x76`)
6.480225      🟩P→C   a0 11 f0 78 ac a7 60 47 00 5f 00 e8 03 79 09 cc 08 00 08 0a          Live measurement notify (`A0 11 F0`, seq `0x78`)
6.720226      🟩P→C   a0 11 f0 7c ac a7 20 43 08 5f 04 e8 02 07 07 1b 03 40 02 1b          Live measurement notify (`A0 11 F0`, seq `0x7C`); deviating header/fields (capture/device variation)
6.760227      🟩P→C   a0 11 f0 7e ac a7 60 47 00 5f 00 e8 03 6b 01 b1 00 eb 00 ba          Live measurement notify (`A0 11 F0`, seq `0x7E`)
6.840227      🟩P→C   a0 11 f0 80 ac a7 60 47 00 5f 00 e8 03 54 03 92 07 d3 0b 82          Live measurement notify (`A0 11 F0`, seq `0x80`)
6.960228      🟩P→C   a0 11 f0 82 ac a7 60 47 05 63 00 e8 03 46 0e f1 0e 22 0e 42          Live measurement notify (`A0 11 F0`, seq `0x82`)
7.080228      🟩P→C   a0 11 f0 84 ac a7 60 47 03 63 00 e8 03 8c 0d aa 0c 9a 0b b3          Live measurement notify (`A0 11 F0`, seq `0x84`)
7.160229      🟩P→C   a0 11 f0 86 ac a7 60 47 06 63 00 e8 03 9d 0a 01 0a bc 09 3b          Live measurement notify (`A0 11 F0`, seq `0x86`)
7.240229      🟩P→C   a0 11 f0 88 ac a7 60 47 02 63 00 e8 03 9e 09 72 09 29 09 16          Live measurement notify (`A0 11 F0`, seq `0x88`)
7.360229      🟩P→C   a0 11 f0 8a ac a7 60 47 00 63 00 e8 03 b4 08 24 08 6a 07 1b          Live measurement notify (`A0 11 F0`, seq `0x8A`)
7.440230      🟩P→C   a0 11 f0 8c ac a7 60 47 00 63 00 e8 03 96 06 ab 05 d7 04 eb          Live measurement notify (`A0 11 F0`, seq `0x8C`)
7.520230      🟩P→C   a0 35 92 3e ab a6 66 47 70 72 84 61 03 1b 87 0c 4b bc 09 0c          Out-of-band/status notification (`A0 35`): not a live measurement frame
7.680231      🟩P→C   a0 11 f0 90 ac a7 60 47 00 63 00 e8 03 55 02 67 03 8f 06 1e          Live measurement notify (`A0 11 F0`, seq `0x90`)
7.720231      🟩P→C   a0 11 f0 92 ac a7 60 47 06 63 00 e8 03 e4 0a 66 0e 17 10 59          Live measurement notify (`A0 11 F0`, seq `0x92`)
7.800232      🟩P→C   a0 11 f0 94 ac a7 60 47 07 64 00 e8 03 51 10 9c 0d ed 0c d7          Live measurement notify (`A0 11 F0`, seq `0x94`)
7.920233      🟩P→C   a0 11 f0 96 ac a7 60 47 08 64 00 e8 03 14 0c 21 0b 66 0a 93          Live measurement notify (`A0 11 F0`, seq `0x96`)
8.040233      🟩P→C   a0 11 f0 98 ac a7 60 47 06 64 00 e8 03 10 0a 03 0a e9 09 f0          Live measurement notify (`A0 11 F0`, seq `0x98`)
8.160234      🟩P→C   a0 11 f0 9a ac a7 60 47 07 64 00 e8 03 aa 09 33 09 91 08 62          Live measurement notify (`A0 11 F0`, seq `0x9A`)
8.200234      🟩P→C   a0 11 f0 9c ac a7 60 47 00 64 00 e8 03 b4 07 b8 06 ac 05 ff          Live measurement notify (`A0 11 F0`, seq `0x9C`)
8.280234      🟩P→C   a0 11 f0 9e ac a7 60 47 00 64 00 e8 03 a8 04 a5 03 af 02 dc          Live measurement notify (`A0 11 F0`, seq `0x9E`)
8.400235      🟩P→C   a0 11 f0 a0 ac a7 60 47 00 64 00 e8 03 bd 01 e9 00 07 01 88          Live measurement notify (`A0 11 F0`, seq `0xA0`)
8.520235      🟩P→C   a0 11 f0 a2 ac a7 60 47 00 64 00 e8 03 4e 03 85 07 f5 0b b8          Live measurement notify (`A0 11 F0`, seq `0xA2`)
8.640236      🟩P→C   a0 11 f0 a4 ac a7 60 47 04 64 00 e8 03 cd 0e f2 0f 1c 10 e9          Live measurement notify (`A0 11 F0`, seq `0xA4`)
8.680237      🟩P→C   a0 11 f0 a6 ac a7 60 47 04 64 00 e8 03 21 0f 8f 0e ba 0d 77          Live measurement notify (`A0 11 F0`, seq `0xA6`)
8.760237      🟩P→C   a0 11 f0 a8 ac a7 60 47 08 64 00 e8 03 cb 0c 15 0c b8 0b a4          Live measurement notify (`A0 11 F0`, seq `0xA8`)
8.880237      🟩P→C   a0 11 f0 aa ac a7 60 47 07 64 00 e8 03 89 0b 52 0b f0 0a d5          Live measurement notify (`A0 11 F0`, seq `0xAA`)
9.000238      🟩P→C   a0 11 f0 ac ac a7 60 47 01 64 00 e8 03 6c 0a be 09 e2 08 0d          Live measurement notify (`A0 11 F0`, seq `0xAC`)
9.120239      🟩P→C   a0 11 f0 ae ac a7 60 47 00 64 00 e8 03 e2 07 d9 06 e1 05 95          Live measurement notify (`A0 11 F0`, seq `0xAE`)
9.160239      🟩P→C   a0 11 f0 b0 ac a7 60 47 00 64 00 e8 03 fc 04 1a 04 3c 03 46          Live measurement notify (`A0 11 F0`, seq `0xB0`)
9.240239      🟩P→C   a0 11 f0 b2 ac a7 60 47 00 64 00 e8 03 ea 02 74 04 11 08 68          Live measurement notify (`A0 11 F0`, seq `0xB2`)
9.400240      🟩P→C   a0 11 f0 b4 ac a7 60 47 04 64 00 e8 03 7d 0c be 0f 43 11 9b          Live measurement notify (`A0 11 F0`, seq `0xB4`)
9.480240      🟩P→C   a0 11 f0 b6 ac a7 60 47 04 64 00 e8 03 a7 11 ad 0e 4c 0e c0          Live measurement notify (`A0 11 F0`, seq `0xB6`)
9.600241      🟩P→C   a0 11 f0 b8 ac a7 60 8c e9 25 40 ab 60 8f 0d 8d 0c 9f 0b d3          Live measurement notify (`A0 11 F0`, seq `0xB8`); nonstandard header bytes in this capture segment
9.640241      🟩P→C   a0 11 f0 ba ac a7 60 47 06 64 00 e8 03 15 0b d8 0a a1 0a a6          Live measurement notify (`A0 11 F0`, seq `0xBA`)
9.720242      🟩P→C   a0 11 f0 bc ac a7 60 47 03 64 00 e8 03 41 0a af 09 f0 08 f3          Live measurement notify (`A0 11 F0`, seq `0xBC`)
9.840242      🟩P→C   a0 11 f0 be ac a7 60 47 00 64 00 e8 03 10 08 0b 07 00 06 27          Live measurement notify (`A0 11 F0`, seq `0xBE`)
9.960243      🟩P→C   a0 11 f0 c0 ac a7 60 47 00 64 00 e8 03 f7 04 06 04 17 03 18          Live measurement notify (`A0 11 F0`, seq `0xC0`)
10.120244     🟩P→C   a0 11 f0 c2 ac a7 60 47 00 64 00 e8 03 2a 02 49 01 27 01 99          Live measurement notify (`A0 11 F0`, seq `0xC2`)
10.120739     🟩P→C   a0 11 f0 c4 ac a7 60 47 00 64 00 e8 03 03 03 cc 06 14 0b f4          Live measurement notify (`A0 11 F0`, seq `0xC4`)
10.200244     🟩P→C   a0 11 f0 c6 ac a7 60 47 06 64 00 e8 03 f2 0d 1c 0f 40 0f 7e          Live measurement notify (`A0 11 F0`, seq `0xC6`)
10.360245     🟩P→C   a0 11 f0 c8 ac a7 60 47 06 66 00 e8 03 f0 0d 61 0d 80 0c 00          Live measurement notify (`A0 11 F0`, seq `0xC8`)
10.400245     🟩P→C   a0 11 f0 ca ac a7 60 47 08 66 00 e8 03 75 0b a5 0a 33 0a 79          Live measurement notify (`A0 11 F0`, seq `0xCA`)
10.560246     🟩P→C   a0 11 f0 cc ac a7 60 47 05 66 00 e8 03 f8 09 b8 09 56 09 2d          Live measurement notify (`A0 11 F0`, seq `0xCC`)
```

Raw timeline:

```text
Time          Dir    Value
------------  -----  -----
0.000000      🟦C→P   b0 11 11 01 ac fa 29 3d bd d4 d2 8f b1 0f 5a 95 3a b8 d1 82
0.000495      🟦C→P   b0 06 10 03 ac c7 b5 1c 5 b0 06 10 03 ac c7 b5 1c 577
0.040193      🟩P→C   a0 03 a1 02 ac 4f
0.041071      🟩P→C   a0 11 33 06 ac fb 32 33 31 32 32 33 30 30 30 30 30 31 34 62
0.041566      🟩P→C   a0 11 32 08 ac 31 34 34 8d 69 5c 07 36 90 33 a2 28 67 7b 7d
0.120000      🟦C→P   b0 03 a3 07 ac 56
0.120245      🟩P→C   a0 11 31 0a ac f3 51 6d 70 f5 95 d3 4e 25 07 e3 86 05 c9 16
0.120548      🟦C→P   b0 03 a2 09 ac 57
0.120792      🟩P→C   a0 0a 30 0c ac f7 93 c1 7c 0a 19 f0 c2
0.160001      🟦C→P   b0 03 a1 0b ac 58
0.160440      🟦C→P   b0 03 a0 0d ac 59
0.160879      🟦C→P   b0 11 11 0f ac fc 44 f3 bd 2f 8d 8d 97 0d b9 53 f5 46 5d 4d
0.161374      🟦C→P   b0 06 10 11 ac 07 03 0f e6
0.200193      🟩P→C   a0 03 a1 10 8c 5d
0.201071      🟩P→C   a0 04 00 14 ac fd bd
0.240001      🟦C→P   b0 03 a0 15 ac 61
1.080005      🟦C→P   b0 04 00 17 ac c1 84
1.120198      🟩P→C   a0 03 a0 18 ac 64
1.120637      🟩P→C   a0 05 00 1a ac c1 3c c3
1.200006      🟦C→P   b0 03 a0 1b ac 67
1.840009      🟦C→P   b0 04 00 1d ac c6 8f
1.960010      🟦C→P   b0 03 a0 21 ac 6d
2.040010      🟦C→P   b0 03 a0 21 ac 6d
2.640013      🟦C→P   b0 04 00 23 ac a5 74
2.680206      🟩P→C   a0 03 a0 24 ac 70
2.680645      🟩P→C   a0 04 00 26 ac a5 77
2.760014      🟦C→P   b0 03 a0 27 ac 73
2.800206      🟩P→C   a0 04 00 28 ac a6 7a
2.880014      🟦C→P   b0 03 a0 29 ac 75
2.880453      🟦C→P   b0 04 00 2b ac a6 7d
3.120208      🟩P→C   a0 11 f0 32 ac a7 60 47 00 61 00 e8 03 f4 06 30 06 4a 05 e7
3.280209      🟩P→C   a0 11 f0 34 ac a7 60 47 00 61 00 e8 03 52 04 53 03 5b 02 73
3.680211      🟩P→C   a0 11 f0 3c ac a7 60 47 04 5e 00 e8 43 2b 0b d5 0b 04 0b 98
3.680706      🟩P→C   a0 11 f0 3e ac a7 60 47 03 5e 00 e8 03 ff 09 ec 08 01 08 79
3.840211      🟩P→C   a0 11 f0 40 ac a7 60 47 01 5e 00 e8 03 71 07 28 07 fa 06 1b
3.880212      🟩P→C   a0 11 f0 42 ac a7 60 47 00 5e 00 e8 03 be 06 52 06 b6 05 4c
3.960212      🟩P→C   a0 11 f0 44 ac a7 60 47 00 5e 00 e8 03 ec 04 07 04 0b 03 80
4.200258      🟩P→C   a0 11 f0 48 ac a7 60 47 00 5e 00 e8 03 00 00 00 00 00 00 7b
4.360214      🟩P→C   a0 11 f0 4c ac a7 60 47 04 5e 00 e8 03 12 09 0c 0b 82 0b 42
4.440215      🟩P→C   a0 11 f0 4e ac a7 60 47 07 5b 00 e8 03 ab 0d 1e 0d 46 0c ba
4.560215      🟩P→C   a0 11 f0 50 ac a7 60 47 08 5b 00 68 03 46 0b 45 0a 9a 09 cb
4.800217      🟩P→C   a0 11 f0 54 ac a7 60 47 00 5b 00 e8 03 6b 08 e9 07 41 07 2f
4.840217      🟩P→C   a0 11 f0 56 ac a7 60 47 00 5b 00 e8 03 6e 06 70 05 68 04 db
4.960217      🟩P→C   a0 11 f0 58 ac a7 60 47 00 5b 00 e8 03 6c 03 85 02 b0 01 2f
5.040218      🟩P→C   a0 11 f0 5a ac a7 60 47 00 5b 00 e8 03 dd 00 09 00 00 00 70
5.200219      🟩P→C   a0 11 f0 5c ac a7 60 47 00 5b 00 e8 03 99 01 88 05 1c 0a d9
5.400219      🟩P→C   a0 11 f0 5e ac a7 60 c7 06 5b 00 e8 03 38 0d 6f 0e 93 0e f6
5.400714      🟩P→C   a0 11 f0 60 ac a7 60 47 07 5b 00 e8 03 81 0e e0 0d f9 0c 18
5.401209      🟩P→C   a0 11 f0 62 ac a7 60 47 06 5b 00 e8 03 f2 0b 1a 0b 9b 0a 5f
5.520220      🟩P→C   a0 11 f0 64 ac a7 60 47 03 5b 00 e8 03 6c 0a 41 0a f8 09 59
5.640221      🟩P→C   a0 11 f0 66 ac a7 60 47 00 5b 00 e8 03 7e 09 d9 08 12 08 18
5.880222      🟩P→C   a0 11 f0 6a ac a7 60 47 01 5b 00 e8 03 47 04 67 03 8f 02 e1
5.880717      🟩P→C   a0 11 f0 6c ac a7 60 47 00 5b 00 e8 03 b1 01 dc 00 cc 00 f6
6.040223      🟩P→C   a0 11 f0 6e ac a7 60 47 01 5b 00 e8 03 dd 02 fe 06 82 0b 0f
6.240224      🟩P→C   a0 11 f0 72 ac a7 60 47 05 5f 00 e8 03 8b 0e da 0d d3 0c 0a
6.280224      🟩P→C   a0 11 f0 74 ac a7 60 47 05 5f 00 e8 03 bb 0b e0 0a 7d 0a e4
6.400225      🟩P→C   a0 11 f0 76 ac a7 60 47 03 5f 00 e8 03 5c 0a 43 0a ee 09 57
6.480225      🟩P→C   a0 11 f0 78 ac a7 60 47 00 5f 00 e8 03 79 09 cc 08 00 08 0a
6.720226      🟩P→C   a0 11 f0 7c ac a7 20 43 08 5f 04 e8 02 07 07 1b 03 40 02 1b
6.760227      🟩P→C   a0 11 f0 7e ac a7 60 47 00 5f 00 e8 03 6b 01 b1 00 eb 00 ba
6.840227      🟩P→C   a0 11 f0 80 ac a7 60 47 00 5f 00 e8 03 54 03 92 07 d3 0b 82
6.960228      🟩P→C   a0 11 f0 82 ac a7 60 47 05 63 00 e8 03 46 0e f1 0e 22 0e 42
7.080228      🟩P→C   a0 11 f0 84 ac a7 60 47 03 63 00 e8 03 8c 0d aa 0c 9a 0b b3
7.160229      🟩P→C   a0 11 f0 86 ac a7 60 47 06 63 00 e8 03 9d 0a 01 0a bc 09 3b
7.240229      🟩P→C   a0 11 f0 88 ac a7 60 47 02 63 00 e8 03 9e 09 72 09 29 09 16
7.360229      🟩P→C   a0 11 f0 8a ac a7 60 47 00 63 00 e8 03 b4 08 24 08 6a 07 1b
7.440230      🟩P→C   a0 11 f0 8c ac a7 60 47 00 63 00 e8 03 96 06 ab 05 d7 04 eb
7.520230      🟩P→C   a0 35 92 3e ab a6 66 47 70 72 84 61 03 1b 87 0c 4b bc 09 0c
7.680231      🟩P→C   a0 11 f0 90 ac a7 60 47 00 63 00 e8 03 55 02 67 03 8f 06 1e
7.720231      🟩P→C   a0 11 f0 92 ac a7 60 47 06 63 00 e8 03 e4 0a 66 0e 17 10 59
7.800232      🟩P→C   a0 11 f0 94 ac a7 60 47 07 64 00 e8 03 51 10 9c 0d ed 0c d7
7.920233      🟩P→C   a0 11 f0 96 ac a7 60 47 08 64 00 e8 03 14 0c 21 0b 66 0a 93
8.040233      🟩P→C   a0 11 f0 98 ac a7 60 47 06 64 00 e8 03 10 0a 03 0a e9 09 f0
8.160234      🟩P→C   a0 11 f0 9a ac a7 60 47 07 64 00 e8 03 aa 09 33 09 91 08 62
8.200234      🟩P→C   a0 11 f0 9c ac a7 60 47 00 64 00 e8 03 b4 07 b8 06 ac 05 ff
8.280234      🟩P→C   a0 11 f0 9e ac a7 60 47 00 64 00 e8 03 a8 04 a5 03 af 02 dc
8.400235      🟩P→C   a0 11 f0 a0 ac a7 60 47 00 64 00 e8 03 bd 01 e9 00 07 01 88
8.520235      🟩P→C   a0 11 f0 a2 ac a7 60 47 00 64 00 e8 03 4e 03 85 07 f5 0b b8
8.640236      🟩P→C   a0 11 f0 a4 ac a7 60 47 04 64 00 e8 03 cd 0e f2 0f 1c 10 e9
8.680237      🟩P→C   a0 11 f0 a6 ac a7 60 47 04 64 00 e8 03 21 0f 8f 0e ba 0d 77
8.760237      🟩P→C   a0 11 f0 a8 ac a7 60 47 08 64 00 e8 03 cb 0c 15 0c b8 0b a4
8.880237      🟩P→C   a0 11 f0 aa ac a7 60 47 07 64 00 e8 03 89 0b 52 0b f0 0a d5
9.000238      🟩P→C   a0 11 f0 ac ac a7 60 47 01 64 00 e8 03 6c 0a be 09 e2 08 0d
9.120239      🟩P→C   a0 11 f0 ae ac a7 60 47 00 64 00 e8 03 e2 07 d9 06 e1 05 95
9.160239      🟩P→C   a0 11 f0 b0 ac a7 60 47 00 64 00 e8 03 fc 04 1a 04 3c 03 46
9.240239      🟩P→C   a0 11 f0 b2 ac a7 60 47 00 64 00 e8 03 ea 02 74 04 11 08 68
9.400240      🟩P→C   a0 11 f0 b4 ac a7 60 47 04 64 00 e8 03 7d 0c be 0f 43 11 9b
9.480240      🟩P→C   a0 11 f0 b6 ac a7 60 47 04 64 00 e8 03 a7 11 ad 0e 4c 0e c0
9.600241      🟩P→C   a0 11 f0 b8 ac a7 60 8c e9 25 40 ab 60 8f 0d 8d 0c 9f 0b d3
9.640241      🟩P→C   a0 11 f0 ba ac a7 60 47 06 64 00 e8 03 15 0b d8 0a a1 0a a6
9.720242      🟩P→C   a0 11 f0 bc ac a7 60 47 03 64 00 e8 03 41 0a af 09 f0 08 f3
9.840242      🟩P→C   a0 11 f0 be ac a7 60 47 00 64 00 e8 03 10 08 0b 07 00 06 27
9.960243      🟩P→C   a0 11 f0 c0 ac a7 60 47 00 64 00 e8 03 f7 04 06 04 17 03 18
10.120244     🟩P→C   a0 11 f0 c2 ac a7 60 47 00 64 00 e8 03 2a 02 49 01 27 01 99
10.120739     🟩P→C   a0 11 f0 c4 ac a7 60 47 00 64 00 e8 03 03 03 cc 06 14 0b f4
10.200244     🟩P→C   a0 11 f0 c6 ac a7 60 47 06 64 00 e8 03 f2 0d 1c 0f 40 0f 7e
10.360245     🟩P→C   a0 11 f0 c8 ac a7 60 47 06 66 00 e8 03 f0 0d 61 0d 80 0c 00
10.400245     🟩P→C   a0 11 f0 ca ac a7 60 47 08 66 00 e8 03 75 0b a5 0a 33 0a 79
10.560246     🟩P→C   a0 11 f0 cc ac a7 60 47 05 66 00 e8 03 f8 09 b8 09 56 09 2d
```

### data/pulse-ox-3-with-finger-loss-and-disconnect.pcapng

Start: frame 778 at capture t=9.885378s (shown below as t=0.000000). Matched start write: `b0111101acfaf4b5c42f174e7d1b7a5ab654c0ef`

This capture contains a finger-loss segment where measurement streaming stops and the device returns to the prompt loop.

Key events:

| Time (s) | Dir | Value | Meaning |
| --- | --- | --- | --- |
| 2.800209 | 🟩P→C | `a0 03 a0 2c ac 78` | Stage-6 ACK (handshake complete) |
| 2.800648 | 🟩P→C | `a0 11 f0 2e ...` | Streaming begins (measurements) |
| 13.880275 | 🟩P→C | `a0 04 00 16 ac a8 6a` | Prompt loop resumes (no-finger / idle state) |
| 17.640298 | 🟩P→C | `a0 11 f0 1a ...` | Streaming resumes (finger detected again) |

Raw timeline:

```text
Time          Dir    Value
------------  -----  -----
0.000000      🟦C→P   b0 11 11 01 ac fa f4 b5 c4 2f 17 4e 7d 1b 7a 5a b6 54 c0 ef
0.000495      🟦C→P   b0 06 10 03 ac 75 95 38 01
0.040192      🟩P→C   a0 03 a1 02 ac 4f
0.040632      🟩P→C   a0 03 a0 04 ac 50
0.041070      🟩P→C   a0 11 33 06 ac fb 32 33 31 32 32 73 28 32 01 31 b0 31 34 62
0.080000      🟦C→P   b0 03 a3 07 ac 56
0.080245      🟩P→C   a0 11 31 0a ac 57 33 9e b2 2f 0b 4d 3a c4 cb ad 0f df d2 7e
0.080548      🟦C→P   b0 03 a2 09 ac 57
0.080792      🟩P→C   a0 0a 30 0c ac f8 92 1e 19 52 d8 3e 11
0.120000      🟦C→P   b0 03 a1 0b ac 58
0.120439      🟦C→P   b0 03 a0 0d ac 59
0.120879      🟦C→P   b0 11 11 0f ac fc 9a 91 c2 50 9c 8c 91 24 1e 10 f0 25 14 39
0.121373      🟦C→P   b0 06 10 11 ac 62 05 52 86
0.160193      🟩P→C   a0 03 a1 10 ac 5d
0.160632      🟩P→C   a0 03 a0 12 ac 5e
0.161071      🟩P→C   a0 04 00 14 ac fd bd
0.240001      🟦C→P   b0 03 a0 15 ac 61
1.000006      🟦C→P   b0 04 00 17 ac c1 84
1.040198      🟩P→C   a0 03 a0 18 ac 64
1.040637      🟩P→C   a0 05 00 1a ac c1 32 b9
1.080006      🟦C→P   b0 03 a0 1b ac 67
1.800011      🟦C→P   b0 04 00 1d ac c6 8f
1.880011      🟦C→P   b0 03 a0 21 ac 6d
2.600015      🟦C→P   b0 04 00 23 ac a5 74
2.640208      🟩P→C   a0 03 a0 24 ac 70
2.640647      🟩P→C   a0 04 00 26 ac a5 77
2.680016      🟦C→P   b0 03 a0 27 ac 73
2.720209      🟩P→C   a0 04 00 28 ac a6 7a
2.760016      🟦C→P   b0 03 a0 29 ac 75
2.760455      🟦C→P   b0 04 00 2b ac a6 7d
2.800209      🟩P→C   a0 03 a0 2c ac 78
2.800648      🟩P→C   a0 11 f0 2e ac a7 60 43 00 58 00 e8 03 85 05 e5 04 45 04 13
2.840209      🟩P→C   a0 11 f0 30 ac a7 60 43 01 58 00 e8 03 aa 03 16 03 85 02 a7
2.920210      🟩P→C   a0 11 f0 32 ac a7 60 43 00 58 00 e8 03 fe 01 81 01 0b 01 e8
3.040210      🟩P→C   a0 11 f0 34 ac a7 60 43 01 58 00 e8 03 8d 00 01 00 00 00 de
3.120211      🟩P→C   a0 11 f0 36 ac a7 60 43 00 58 00 e8 03 00 00 41 00 c1 02 63
3.240212      🟩P→C   a0 11 f0 38 ac a7 60 43 04 58 00 e8 03 b6 05 c8 07 88 08 7f
3.320212      🟩P→C   a0 11 f0 3a ac a7 60 43 05 5a 00 e8 03 8c 08 09 09 84 08 9c
3.400213      🟩P→C   a0 11 f0 3c ac a7 60 43 06 5a 00 e8 03 da 07 3f 07 e3 06 7d
3.440213      🟩P→C   a0 11 f0 3c ac a7 60 43 06 5a 00 e8 03 da 07 3f 07 e3 06 7d
3.520214      🟩P→C   a0 11 f0 3e ac a7 60 43 07 5a 00 e8 03 be 06 ac 06 82 06 6e
3.600214      🟩P→C   a0 11 f0 40 ac a7 60 43 01 5a 00 e8 03 3d 06 d2 05 49 05 d4
3.720214      🟩P→C   a0 11 f0 42 ac a7 60 43 00 5a 00 e8 03 b0 04 0e 04 69 03 9f
3.840215      🟩P→C   a0 11 f0 44 ac a7 60 43 00 5a 00 e8 03 c0 02 1c 02 86 01 d6
3.920216      🟩P→C   a0 11 f0 46 ac a7 60 43 01 5a 00 e8 03 fd 00 79 00 00 00 e8
4.000216      🟩P→C   a0 11 f0 48 ac a7 60 43 00 5a 00 e8 03 00 00 00 00 00 00 73
4.080216      🟩P→C   a0 11 f0 4a ac a7 60 43 03 5a 00 e8 03 d8 01 fa 04 a3 07 f9
4.200217      🟩P→C   a0 11 f0 4c ac a7 60 43 07 5a 00 e8 03 00 09 6a 09 78 09 7b
4.280218      🟩P→C   a0 11 f0 4e ac a7 60 42 07 5b 00 e8 03 55 0a fc 09 70 09 5d
4.400218      🟩P→C   a0 11 f0 50 ac a7 60 42 07 5b 00 e8 03 e7 08 97 08 75 08 8d
4.480219      🟩P→C   a0 11 f0 52 ac a7 60 42 07 5b 00 e8 03 5f 08 37 08 ec 07 1d
4.560264      🟩P→C   a0 11 f0 54 ac a7 60 42 04 5b 00 e8 03 7b 07 e5 06 42 06 38
4.680220      🟩P→C   a0 11 f0 56 ac a7 60 42 00 5b 00 e8 03 99 05 f1 04 4f 04 67
4.760221      🟩P→C   a0 11 f0 58 ac a7 60 42 00 5b 00 e8 03 b9 03 33 03 b0 02 27
4.880221      🟩P→C   a0 11 f0 5a ac a7 60 42 01 5b 00 e8 03 29 02 99 01 1e 01 6a
4.960222      🟩P→C   a0 11 f0 5c ac a7 60 42 00 5b 00 e8 03 6e 01 37 03 19 06 4f
5.040223      🟩P→C   a0 11 f0 5e ac a7 60 42 04 5b 00 e8 03 cf 08 56 0a da 0a a8
5.240224      🟩P→C   a0 11 f0 62 ac a7 60 42 05 5b 00 e8 03 c7 08 3e 08 e9 07 97
5.360224      🟩P→C   a0 11 f0 64 ac a7 60 32 0d 5b 00 e8 03 c5 07 af 07 85 07 a6
5.400224      🟩P→C   a0 11 f0 66 ac a7 60 42 04 5b 00 e8 03 38 07 c8 06 38 06 e0
5.520225      🟩P→C   a0 11 f0 68 ac a7 60 42 00 5b 00 e8 03 99 05 f7 04 5a 04 8a
5.640226      🟩P→C   a0 11 f0 6a ac a7 60 42 01 5b 00 e8 03 c0 03 2d 03 a8 02 33
5.720226      🟩P→C   a0 11 f0 6c ac a7 60 42 00 5b 00 e8 03 2c 02 af 01 24 01 9a
5.840227      🟩P→C   a0 11 f0 6e ac a7 60 42 01 5b 00 e8 03 aa 00 e8 00 8e 02 bc
5.880227      🟩P→C   a0 11 f0 70 ac a7 60 42 02 5b 00 e8 03 46 05 d8 07 48 09 18
6.000228      🟩P→C   a0 11 f0 72 ac a7 60 42 05 5c 00 e8 03 b6 09 ae 09 c4 08 e5
6.120229      🟩P→C   a0 11 f0 74 ac a7 60 42 06 5c 00 e8 03 42 08 aa 07 3b 07 e3
6.200229      🟩P→C   a0 11 f0 76 ac a7 60 42 07 5c 00 e8 03 13 07 17 07 19 07 01
6.320230      🟩P→C   a0 11 f0 78 ac a7 60 42 07 5c 00 e8 03 01 07 c5 06 67 06 eb
6.360230      🟩P→C   a0 11 f0 7a ac a7 60 42 01 5c 00 e8 03 ed 05 6d 05 f6 04 05
6.480231      🟩P→C   a0 11 f0 7c ac a7 60 42 00 5c 00 e8 03 97 04 45 04 f7 03 86
6.600232      🟩P→C   a0 11 f0 7e ac a7 60 42 00 5c 00 e8 03 9a 03 32 03 c2 02 40
6.680232      🟩P→C   a0 11 f0 80 ac a7 60 42 00 5c 00 e8 03 a2 02 8e 03 a8 05 8e
6.800233      🟩P→C   a0 11 f0 82 ac a7 60 42 06 5c 00 e8 03 2d 08 fb 09 c1 0a b8
6.880233      🟩P→C   a0 11 f0 84 ac a7 60 42 06 59 00 e8 03 e1 0a 18 08 9e 07 63
6.960234      🟩P→C   a0 11 f0 86 ac a7 60 42 07 59 00 e8 03 f5 06 4e 06 e7 05 f1
7.080234      🟩P→C   a0 11 f0 88 ac a7 60 42 06 59 00 e8 03 bc 05 a9 05 93 05 be
7.160235      🟩P→C   a0 11 f0 8a ac a7 60 42 02 59 00 e8 03 66 05 19 05 ac 04 ee
7.280236      🟩P→C   a0 11 f0 8c ac a7 60 42 00 59 00 e8 03 2e 04 a8 03 2e 03 c3
7.360236      🟩P→C   a0 11 f0 8e ac a7 60 42 00 59 00 e8 03 b1 02 32 02 a8 01 47
7.440237      🟩P→C   a0 11 f0 90 ac a7 60 42 00 59 00 e8 03 1e 01 be 00 25 01 bc
7.520237      🟩P→C   a0 11 f0 92 ac a7 60 42 02 59 00 e8 03 cd 02 41 05 73 07 4c
7.640238      🟩P→C   a0 11 f0 94 ac a7 60 42 06 59 00 e8 03 b8 08 42 09 76 09 4d
7.760238      🟩P→C   a0 11 f0 96 ac a7 60 42 06 56 00 e8 03 70 09 62 08 e5 07 91
7.840239      🟩P→C   a0 11 f0 98 ac a7 60 42 06 56 00 e8 03 72 07 3d 07 25 07 ad
7.920239      🟩P→C   a0 11 f0 9a ac a7 60 42 07 56 00 e8 03 09 07 ce 06 7a 06 2b
8.000240      🟩P→C   a0 11 f0 9c ac a7 60 42 03 56 00 e8 03 07 06 82 05 eb 04 48
8.120241      🟩P→C   a0 11 f0 9e ac a7 60 42 00 56 00 e8 03 5e 04 de 03 68 03 72
8.240241      🟩P→C   a0 11 f0 a0 ac a7 60 42 00 56 00 e8 03 eb 02 69 02 eb 01 0a
8.320242      🟩P→C   a0 11 f0 a2 ac a7 60 42 00 56 00 e8 03 da 01 e5 02 ff 04 8d
8.400242      🟩P→C   a0 11 f0 a4 ac a7 60 42 03 56 00 e8 03 55 07 ea 08 a7 09 cb
8.480243      🟩P→C   a0 11 f0 a6 ac a7 60 44 04 53 00 e8 03 f1 09 00 0a 00 08 db
8.600244      🟩P→C   a0 11 f0 a8 ac a7 60 44 05 53 00 e8 03 82 07 f1 06 88 06 e0
8.680244      🟩P→C   a0 11 f0 aa ac a7 60 44 06 53 00 e8 03 59 06 40 06 18 06 98
8.800245      🟩P→C   a0 11 f0 ac ac a7 60 44 03 53 00 e8 03 cc 05 5f 05 d6 04 e3
8.880245      🟩P→C   a0 11 f0 ae ac a7 60 44 00 53 00 e8 03 42 04 ad 03 20 03 ec
8.960246      🟩P→C   a0 11 90 b0 ac a7 60 44 00 53 00 e8 03 9f 02 18 02 b1 01 52
9.080247      🟩P→C   a0 11 f0 b2 ac a7 60 44 00 53 00 e8 03 37 01 b9 00 77 00 3f
9.160247      🟩P→C   a0 11 f0 b4 ac a7 60 44 00 53 00 e8 03 17 01 da 02 20 05 f2
9.280248      🟩P→C   a0 11 f0 b6 ac a7 60 44 05 53 00 e8 03 e5 06 bf 07 03 08 9c
9.360248      🟩P→C   a0 11 f0 b8 ac a7 60 45 06 52 00 e8 03 00 08 4d 07 df 06 24
9.440249      🟩P→C   a0 11 f0 ba ac a7 60 45 05 52 00 e8 03 6d 06 20 06 ff 05 81
9.560249      🟩P→C   a0 11 f0 bc ac a7 60 45 07 52 00 e8 03 e4 05 b4 05 60 05 ef
9.640250      🟩P→C   a0 11 f0 be ac a7 60 45 00 52 00 e8 03 f8 04 8c 04 29 04 9c
9.760251      🟩P→C   a0 11 f0 c0 ac a7 60 45 00 52 00 e8 03 ce 03 75 03 23 03 54
9.840251      🟩P→C   a0 11 f0 c2 ac a7 60 45 00 52 00 e8 03 cc 02 79 02 18 02 4a
9.920251      🟩P→C   a0 11 f0 c4 ac a7 60 45 01 52 00 e8 03 a8 01 29 01 03 01 c1
10.040252     🟩P→C   a0 11 f0 c6 ac a7 60 45 00 52 00 e8 03 da 01 b3 03 d0 05 51
10.120253     🟩P→C   a0 11 f0 c8 ac a7 60 45 06 52 00 e8 03 4c 07 fd 07 35 08 87
10.320254     🟩P→C   a0 11 f0 cc ac a7 60 46 04 4f 00 e8 03 06 06 b1 05 7e 05 38
10.400254     🟩P→C   a0 11 f0 ce ac a7 60 46 08 4f 00 e8 03 61 05 35 05 f4 04 91
10.520255     🟩P→C   a0 11 f0 d0 ac a7 60 46 00 4f 00 e8 03 90 04 18 04 8c 03 32
10.600256     🟩P→C   a0 11 f0 d2 ac a7 60 46 00 4f 00 e8 03 f6 02 61 02 d4 01 25
10.720257     🟩P→C   a0 11 f0 d4 ac a7 60 46 00 4f 00 e8 03 54 01 e0 00 6f 00 9b
10.800257     🟩P→C   a0 11 f0 d6 ac a7 60 46 00 4f 00 e8 03 00 00 00 00 00 00 f9
10.880257     🟩P→C   a0 11 f0 d8 ac a7 60 46 00 4f 00 e8 03 00 00 00 00 88 00 83
11.080258     🟩P→C   a0 11 f0 dc ac a7 60 46 04 47 00 e8 03 0a 05 c4 04 2a 04 00
11.280260     🟩P→C   a0 11 f0 e0 ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 fb
11.360260     🟩P→C   a0 11 f0 e2 ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 fd
11.560261     🟩P→C   a0 11 f0 e6 ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 01
11.680262     🟩P→C   a0 11 f0 e8 ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 03
11.760263     🟩P→C   a0 11 f0 ea ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 05
11.840263     🟩P→C   a0 11 f0 ec ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 07
11.960264     🟩P→C   a0 11 f0 ee ac a7 60 46 00 47 00 e8 03 00 00 00 00 00 00 09
12.040264     🟩P→C   a0 11 f0 f0 ac a7 60 46 00 40 00 e8 03 00 00 35 08 95 07 dd
12.160265     🟩P→C   a0 11 f0 f2 ac a7 60 46 01 40 00 e8 03 25 07 0a 07 03 07 4e
12.240265     🟩P→C   a0 11 f0 f4 ac a7 60 46 00 40 00 e8 03 21 07 48 07 85 07 0b
12.320266     🟩P→C   a0 11 f0 f6 ac a7 60 46 00 40 00 e8 03 c0 07 fb 07 22 08 fd
12.440267     🟩P→C   a0 11 f0 f8 ac a7 60 46 00 40 00 e8 03 35 08 30 08 1e 08 a7
12.520267     🟩P→C   a0 11 f0 fa ac a7 60 46 00 40 00 e8 03 03 08 e9 07 d5 07 e5
12.640268     🟩P→C   a0 11 f0 fc ac a7 60 46 00 40 00 e8 03 c2 07 bb 07 b2 07 54
12.720268     🟩P→C   a0 11 f0 fe ac a7 60 46 00 40 00 e8 03 a8 07 94 07 87 07 ea
12.800269     🟩P→C   a0 11 f0 00 ac a7 60 46 00 40 00 e8 03 7e 07 75 07 90 07 ac
12.920270     🟩P→C   a0 11 f0 02 ac a7 60 46 03 40 00 e8 03 3a 08 9a 09 60 0b 69
13.000270     🟩P→C   a0 11 f0 04 ac a7 60 46 07 40 00 e8 03 fa 0c 17 0e cf 0e 27
13.120271     🟩P→C   a0 11 f0 06 ac a7 60 46 08 40 00 e8 03 41 0f 84 0f a8 0f bc
13.200271     🟩P→C   a0 11 f0 08 ac a7 60 46 06 40 00 e8 03 cd 0f 06 10 4f 10 73
13.280272     🟩P→C   a0 11 f0 0a ac a7 60 46 04 40 00 e8 03 96 10 c6 10 c8 10 76
13.400272     🟩P→C   a0 11 f0 0c ac a7 60 46 05 3a 00 e8 03 70 09 bf 08 9c 07 02
13.480273     🟩P→C   a0 11 f0 0e ac a7 60 46 00 3a 00 e8 03 2b 06 3f 04 df 01 70
13.600274     🟩P→C   a0 11 f0 10 ac a7 60 46 01 3a 00 e8 03 00 00 00 00 00 00 1f
13.680274     🟩P→C   a0 11 f0 12 ac a7 60 46 01 3a 00 e8 03 00 00 00 00 00 00 21
13.800275     🟩P→C   a0 11 f0 14 ac a7 60 46 00 3a 00 e8 03 00 00 00 00 00 00 22
13.880275     🟩P→C   a0 04 00 16 ac a8 6a
13.920083     🟦C→P   b0 03 a0 17 ac 63
14.840281     🟩P→C   a0 04 00 18 ac a8 6c
14.880089     🟦C→P   b0 03 a0 19 ac 65
17.640298     🟩P→C   a0 11 f0 1a ac a7 00 00 06 44 00 e8 03 00 00 00 00 3a 08 d4
17.760299     🟩P→C   a0 11 f0 1c ac a7 00 00 05 44 00 e8 03 fa 07 db 07 e0 07 5d
17.840299     🟩P→C   a0 11 f0 1e ac a7 00 00 08 44 00 e8 03 eb 07 df 07 b6 07 2d
17.920299     🟩P→C   a0 11 f0 20 ac c7 00 00 85 44 00 e8 43 6f 07 0c 07 97 07 bd
18.040300     🟩P→C   a0 11 f0 22 ac a7 00 00 00 44 00 e8 03 12 06 91 05 17 05 5e
18.120301     🟩P→C   a0 11 f0 24 ac a7 00 00 00 44 00 e8 03 aa 04 40 04 d5 03 60
18.240301     🟩P→C   a0 11 f0 26 ac a7 00 00 00 44 00 e8 03 5f 03 02 03 45 03 47
18.520303     🟩P→C   a0 11 f0 2c ac a7 00 00 05 3f 00 e8 03 5f 0b 49 08 fa 07 5a
18.600304     🟩P→C   a0 11 f0 2e ac a7 00 00 06 3f 00 e8 03 b0 07 92 07 90 07 88
18.720304     🟩P→C   a0 11 f0 30 ac a7 00 00 05 3f 00 e8 03 89 07 6b 07 2f 07 da
18.800305     🟩P→C   a0 11 f0 32 ac a7 00 00 06 3f 00 e8 03 d5 06 5f 06 d7 05 c1
18.880305     🟩P→C   a0 11 f0 34 ac a7 00 00 00 3f 00 e8 03 4c 05 cf 04 63 04 2c
19.040306     🟩P→C   a0 11 f0 36 ac a7 00 00 00 3f 00 e8 03 fd 03 8d 03 10 03 46
19.080306     🟩P→C   a0 11 f0 38 ac a7 00 00 00 3f 00 e8 03 a5 02 c7 02 f6 03 0e
19.200307     🟩P→C   a0 11 f0 3a ac a7 00 00 06 3f 00 e8 03 09 06 24 08 8c 09 7d
19.280308     🟩P→C   a0 11 f0 3c ac a7 00 00 05 3f 00 e8 03 36 0a 84 0a 99 0a 1f
19.360308     🟩P→C   a0 11 f0 3e ac a7 00 00 07 3e 00 e8 03 de 07 8b 07 39 07 68
19.560309     🟩P→C   a0 11 f0 42 ac a7 00 00 04 3e 00 e8 03 de 06 a0 06 42 06 84
19.960312     🟩P→C   a0 11 f0 4a ac a7 00 00 00 3e 00 e8 03 19 02 ff 02 e8 04 be
20.040312     🟩P→C   a0 11 f0 4c ac a7 00 00 04 3e 30 e8 03 1a 03 bd 08 94 09 2f
20.160313     🟩P→C   a0 11 f0 4e ac a7 60 45 04 3e 00 e8 03 f3 09 04 0a b9 07 2d
20.240313     🟩P→C   a0 11 f0 50 ac a7 60 45 05 3e 00 e8 03 42 07 d8 06 b3 06 46
20.480314     🟩P→C   a0 11 f0 54 ac a7 60 45 06 3e 00 e8 03 70 06 20 06 ae 05 ba
20.520315     🟩P→C   a0 11 f0 56 ac a7 60 45 00 3e 00 e8 03 24 05 8b 04 05 04 28
20.640316     🟩P→C   a0 11 f0 58 ac a7 60 45 00 3e 00 e8 03 97 03 30 03 c5 02 fd
20.720316     🟩P→C   a0 11 f0 5a ac a7 60 45 00 3e 00 e8 03 50 02 d7 01 a9 01 3f
20.920317     🟩P→C   a0 11 f0 5e ac a7 60 45 05 3e 00 e8 03 5a 08 5f 09 d9 09 20
21.000318     🟩P→C   a0 11 f0 60 ac a7 60 45 06 3f 00 e8 03 03 0a 42 08 e9 07 bf
21.120318     🟩P→C   a0 11 f0 62 ac a7 60 45 06 3f 00 e8 03 82 07 3e 07 1e 07 6d
21.320320     🟩P→C   a0 11 f0 66 ac a7 60 45 04 3f 00 e8 03 44 06 ca 05 3d 05 d7
21.680322     🟩P→C   a0 11 f0 6e ac a7 60 45 03 3f 00 e8 03 e5 03 45 06 5a 08 18
21.800323     🟩P→C   a0 11 f0 70 ac a7 60 45 07 3f 00 e8 03 a3 09 42 0a 7f 0a 0a
21.960323     🟩P→C   a0 11 f0 74 ac a7 60 46 03 40 00 e8 03 d7 07 a4 07 88 07 a3
22.080324     🟩P→C   a0 11 f0 76 ac a7 60 46 05 40 00 e8 03 61 07 26 07 c9 06 f3
22.200325     🟩P→C   a0 11 f0 78 ac a7 60 46 01 40 00 e8 03 4c 06 b7 05 1b 05 bb
22.360326     🟩P→C   a0 11 f0 7c ac a7 60 46 01 40 00 e8 03 16 03 94 02 0a 02 4c
22.480327     🟩P→C   a0 11 f0 7e ac a7 60 46 00 40 00 e8 03 85 01 71 01 6d 02 f9
22.560327     🟩P→C   a0 11 f0 80 ac a7 60 46 01 40 00 e8 03 7c 04 d1 06 83 08 77
22.640328     🟩P→C   a0 11 f0 82 ac a7 60 46 04 40 00 e8 03 67 09 ce 09 e3 09 cd
22.760328     🟩P→C   a0 11 f0 84 ac a7 60 46 05 42 00 e8 03 4d 08 f3 07 96 07 8b
22.840329     🟩P→C   a0 11 f0 86 ac a7 60 46 04 42 00 e8 03 57 07 27 07 e6 06 18
22.920329     🟩P→C   a0 11 f0 88 ac a7 60 46 04 42 00 e8 03 18 85 0a e7 b0 71 82
22.960329     🟩P→C   a0 11 f0 88 ac a7 60 46 04 42 00 e8 03 78 06 ab 05 70 04 44
23.000330     🟩P→C   a0 11 f0 8a ac a7 60 46 00 42 00 e8 03 0d 03 b2 01 7f 00 e2
23.120331     🟩P→C   a0 11 f0 8c ac a7 60 46 01 42 00 e8 03 00 00 00 00 00 00 a3
23.240331     🟩P→C   a0 11 f0 8e ac a7 60 46 00 42 00 e8 03 00 00 00 00 00 00 a4
23.320331     🟩P→C   a0 11 f0 90 ac a7 60 46 01 42 00 e8 03 00 00 00 00 00 00 a7
23.400332     🟩P→C   a0 11 f0 92 ac a7 60 46 00 42 00 e8 03 00 00 00 00 00 00 a8
23.480333     🟩P→C   a0 04 00 94 ac a8 e8
23.520140     🟦C→P   b0 03 a0 95 ac e1
24.440338     🟩P→C   a0 04 00 96 ac a8 ea
24.480146     🟦C→P   b0 03 a0 97 ac e3
25.440344     🟩P→C   a0 04 00 98 ac a8 ec
25.480152     🟦C→P   b0 03 a0 99 ac e5
26.400350     🟩P→C   a0 04 00 9a ac a8 ee
26.440158     🟦C→P   b0 03 a0 9b ac e7
27.360356     🟩P→C   a0 04 00 9c ac a8 f0
27.400164     🟦C→P   b0 03 a0 9d ac e9
28.320361     🟩P→C   a0 04 00 9e ac a8 f2
28.360169     🟦C→P   b0 03 a0 9f ac eb
29.280367     🟩P→C   a0 04 00 a0 ac a8 f4
29.320175     🟦C→P   b0 03 a0 a1 ac ed
30.280373     🟩P→C   a0 04 00 a2 ac a8 f6
30.320181     🟦C→P   b0 03 a0 a3 ac ef
```

### data/pulse-ox.pcapng

Start: frame 1312 at capture t=16.807952s (shown below as t=0.000000). Matched start write: `b0111101acfa9677649e1ebcebddab3b379ac6e6`

Key events:

| Time (s) | Dir | Value | Meaning |
| --- | --- | --- | --- |
| 0.000000 | 🟦C→P | `b0 11 11 01 ac fa ...` | Identify stage 1 write |
| 0.120880 | 🟦C→P | `b0 11 11 0f ac fc ...` | Identify stage 2 write |
| 1.840642 | 🟩P→C | `a0 06 00 20 ac c6 00 00 92` | Finger/status-type message observed in this capture |
| 2.800207 | 🟩P→C | `a0 03 a0 2c ac 78` | Handshake complete |
| 2.800646 | 🟩P→C | `a0 11 f0 2e ...` | First measurement frame: SpO₂=97, HR=70, strength=7 |

Raw timeline:

```text
Time          Dir    Value
------------  -----  -----
0.000000      🟦C→P   b0 11 11 01 ac fa 96 77 64 9e 1e bc eb dd ab 3b 37 9a c6 e6
0.000495      🟦C→P   b0 06 10 03 ac 69 5c ae 32
0.040193      🟩P→C   a0 03 a1 02 ac 4f
0.040632      🟩P→C   a0 03 a0 04 ac 50
0.041071      🟩P→C   a0 11 33 06 ac fb 32 33 31 32 32 33 30 30 30 30 30 31 34 62
0.041566      🟩P→C   a0 11 32 08 ac 31 34 34 30 80 dc fe 84 2d 43 48 af ee 50 32
0.080001      🟦C→P   b0 03 a3 07 ac 56
0.080245      🟩P→C   a0 11 31 0a ac c6 9c 26 66 72 72 0b c7 88 be 8e 71 c0 f5 85
0.080548      🟦C→P   b0 03 a2 09 ac 57
0.080792      🟩P→C   a0 0a 30 0c ac ae 88 57 aa 60 60 c7 a6
0.120001      🟦C→P   b0 03 a1 0b ac 58
0.120441      🟦C→P   b0 03 a0 0d ac 59
0.120880      🟦C→P   b0 11 11 0f ac fc 96 fb b4 51 3a 34 45 24 85 21 b0 6b 5b 51
0.121375      🟦C→P   b0 06 10 11 ac 3b b4 62 1e
0.160194      🟩P→C   a0 03 a1 10 ac 5d
0.160633      🟩P→C   a0 03 a0 12 ac 5e
0.161072      🟩P→C   a0 04 00 14 ac fd bd
0.200002      🟦C→P   b0 03 a0 15 ac 61
1.000006      🟦C→P   b0 04 00 17 ac c1 84
1.040239      🟩P→C   a0 03 a0 18 ac 64
1.080006      🟦C→P   b0 03 a0 1b ac 67
1.800010      🟦C→P   b0 04 00 1d ac c6 8f
1.840203      🟩P→C   a0 03 a0 1e ac 6a
1.840642      🟩P→C   a0 06 00 20 ac c6 00 00 92
1.880011      🟦C→P   b0 03 a0 21 ac 6d
2.600013      🟦C→P   b0 04 00 23 ac a5 74
2.640207      🟩P→C   a0 03 a0 24 ac 70
2.640646      🟩P→C   a0 04 00 26 ac a5 77
2.680014      🟦C→P   b0 03 a0 27 ac 73
2.720207      🟩P→C   a0 04 00 28 ac a6 7a
2.760015      🟦C→P   b0 03 a0 29 ac 75
2.760454      🟦C→P   b0 04 00 2b ac a6 7d
2.800207      🟩P→C   a0 03 a0 2c ac 78
2.800646      🟩P→C   a0 11 f0 2e ac a7 61 46 07 2b 00 e8 03 4a 05 14 05 fe 04 9f
2.880207      🟩P→C   a0 11 f0 30 ac a7 61 46 06 2b 00 e8 03 f0 04 d7 04 a2 04 ab
3.000206      🟩P→C   a0 11 f0 32 ac a7 61 46 02 2b 00 e8 03 58 04 fd 03 98 03 2b
3.080207      🟩P→C   a0 11 f0 34 ac a7 61 46 00 2b 00 e8 03 33 03 d0 02 6e 02 ac
3.160208      🟩P→C   a0 11 f0 36 ac a7 61 46 00 2b 00 e8 03 10 02 b4 01 5a 01 58
3.280208      🟩P→C   a0 11 f0 38 ac a7 61 46 00 2b 00 e8 03 ff 00 a9 00 c5 00 a5
3.360209      🟩P→C   a0 11 f0 3a ac a7 61 46 00 2b 00 e8 03 c4 01 92 03 61 05 fa
3.480209      🟩P→C   a0 11 f0 3c ac a7 61 46 07 2c 00 e8 03 7f 06 da 06 c7 06 76
3.600210      🟩P→C   a0 11 f0 3e ac a7 61 46 06 2c 00 e8 03 e1 05 82 05 1f 05 d6
3.640210      🟩P→C   a0 11 f0 40 ac a7 61 46 05 2c 00 e8 03 d7 04 ba 04 b0 04 93
3.760210      🟩P→C   a0 11 f0 42 ac a7 61 46 08 2c 00 e8 03 a4 04 78 04 3b 04 ae
3.840210      🟩P→C   a0 11 f0 44 ac a7 61 46 00 2c 00 e8 03 eb 03 92 03 32 03 fd
3.960210      🟩P→C   a0 11 f0 46 ac a7 61 46 01 2c 00 e8 03 d7 02 7b 02 23 02 c3
4.080211      🟩P→C   a0 11 f0 48 ac a7 61 46 00 2c 00 e8 03 c4 01 6b 01 10 01 8b
4.120211      🟩P→C   a0 11 f0 4a ac a7 61 46 00 2c 00 e8 03 b3 00 60 00 52 00 b0
4.240212      🟩P→C   a0 11 f0 4c ac a7 61 46 00 2c 00 e8 03 13 01 b0 02 91 04 a8
4.320213      🟩P→C   a0 11 f0 4e ac a7 61 46 07 2c 00 e8 03 e5 05 66 06 65 06 17
4.440213      🟩P→C   a0 11 f0 50 ac a7 61 46 07 2c 00 e8 03 d5 05 75 05 14 05 c5
4.560213      🟩P→C   a0 11 f0 52 ac a7 61 46 06 2c 00 e8 03 da 04 c7 04 c9 04 cf
4.600213      🟩P→C   a0 11 f0 54 ac a7 61 46 08 2c 00 e8 03 c4 04 ac 04 7c 04 55
4.720258      🟩P→C   a0 11 f0 56 ac a7 61 46 03 2c 00 e8 03 3c 04 e9 03 8b 03 14
4.800215      🟩P→C   a0 11 f0 58 ac a7 61 46 00 2c 00 e8 03 2d 03 d6 02 82 02 e5
4.920215      🟩P→C   a0 11 f0 5a ac a7 61 46 00 2c 00 e8 03 2c 02 d5 01 80 01 e0
5.040216      🟩P→C   a0 11 f0 5c ac a7 61 46 00 2c 00 e8 03 2e 01 f3 00 2f 01 af
5.080216      🟩P→C   a0 11 f0 5e ac a7 61 46 00 2c 00 e8 03 50 02 1a 04 d6 05 aa
5.200216      🟩P→C   a0 11 f0 60 ac a7 61 46 07 2c 00 e8 03 d5 06 28 07 16 07 8f
5.280216      🟩P→C   a0 11 f0 62 ac a7 61 46 07 2c 00 e8 03 db 05 76 05 1a 05 e4
5.400216      🟩P→C   a0 11 f0 64 ac a7 61 46 06 2c 00 e8 03 e9 04 d8 04 d9 04 11
5.520217      🟩P→C   a0 11 f0 66 ac a7 61 46 08 2c 00 e8 03 c9 04 9f 04 59 04 3c
5.560218      🟩P→C   a0 11 f0 68 ac a7 61 46 01 2c 00 e8 03 04 04 a1 03 41 03 5a
5.760218      🟩P→C   a0 11 f0 6c ac a7 61 46 00 2c 00 e8 03 ba 01 64 01 0c 01 9a
5.880220      🟩P→C   a0 11 f0 6e ac a7 61 46 00 2c 00 e8 03 b0 00 7b 00 ec 00 86
6.000219      🟩P→C   a0 11 f0 70 ac a7 61 46 02 2c 00 e8 03 4d 02 3e 04 e5 05 ee
6.040220      🟩P→C   a0 11 f0 72 ac a7 61 46 08 2c 00 e8 03 c8 06 0d 07 09 07 6d
6.200219      🟩P→C   a0 11 f0 74 ac a7 61 46 07 2c 00 e8 03 57 06 07 06 ae 05 99
6.240220      🟩P→C   a0 11 f0 76 ac a7 61 46 06 2c 00 e8 03 78 05 61 05 5a 05 bf
6.360220      🟩P→C   a0 11 f0 78 ac a7 61 46 07 2c 00 e8 03 46 05 1a 05 d4 04 c2
6.480221      🟩P→C   a0 11 f0 7a ac a7 61 46 01 2c 00 e8 03 7d 04 1e 04 b6 03 d8
6.520221      🟩P→C   a0 11 f0 7c ac a7 61 46 00 2c 00 e8 03 55 03 f5 02 9f 02 6d
6.640222      🟩P→C   a0 11 f0 7e ac a7 61 46 00 2c 00 e8 03 40 02 e8 01 8c 01 37
6.720222      🟩P→C   a0 11 f0 80 ac a7 61 46 00 2c 00 e8 03 3c 01 f4 00 1b 01 ce
6.840222      🟩P→C   a0 11 f0 82 ac a7 61 46 00 2c 00 e8 03 18 02 de 03 ac 05 2f
6.960223      🟩P→C   a0 11 f0 84 ac a7 61 46 04 2c 00 e8 03 dc 06 58 07 73 07 44
7.000223      🟩P→C   a0 11 f0 86 ac a7 61 46 05 2c 00 e8 03 63 06 0f 06 a7 05 b6
7.120223      🟩P→C   a0 11 f0 88 ac a7 61 46 06 2c 00 e8 03 57 05 33 05 28 05 50
7.200224      🟩P→C   a0 11 f0 8a ac a7 61 46 07 2c 00 e8 03 1a 05 f6 04 b6 04 65
7.320225      🟩P→C   a0 11 f0 8c ac a7 61 46 01 2c 00 e8 03 5d 04 fa 03 96 03 85
7.440225      🟩P→C   a0 11 f0 8e ac a7 61 46 00 2c 00 e8 03 2f 03 c7 02 5c 02 e8
7.480226      🟩P→C   a0 11 f0 90 ac a7 61 46 00 2c 00 e8 03 f8 01 92 01 2d 01 4b
7.640226      🟩P→C   a0 11 f0 92 ac a7 61 46 01 2c 00 e8 03 c3 00 5c 00 25 00 d8
7.680225      🟩P→C   a0 11 f0 94 ac a7 61 46 00 2c 00 e8 03 a4 00 05 02 d0 03 13
7.800227      🟩P→C   a0 11 f0 96 ac a7 61 46 08 2c 00 e8 03 2c 05 cb 05 e3 05 88
7.920227      🟩P→C   a0 11 f0 98 ac a7 61 46 06 2c 00 e8 03 91 05 39 05 d4 04 4b
7.960227      🟩P→C   a0 11 f0 9a ac a7 61 46 07 2c 00 e8 03 79 04 43 04 28 04 92
8.080228      🟩P→C   a0 11 f0 9c ac a7 61 46 03 2c 00 e8 03 15 04 f3 03 ba 03 6c
8.160229      🟩P→C   a0 11 f0 9e ac a7 61 46 00 2c 00 e8 03 71 03 16 03 b4 02 e2
8.280228      🟩P→C   a0 11 f0 a0 ac a7 61 46 00 2c 00 e8 03 58 02 f6 01 9b 01 8e
8.400228      🟩P→C   a0 11 f0 a2 ac a7 61 46 00 2c 00 e8 03 3a 01 e1 00 88 00 47
8.440228      🟩P→C   a0 11 f0 a4 ac a7 61 46 00 2c 00 e8 03 31 00 00 00 00 00 d6
8.560230      🟩P→C   a0 11 f0 a6 ac a7 61 46 01 2c 00 e8 03 64 00 ce 01 77 03 55
8.640229      🟩P→C   a0 11 f0 a8 ac a7 61 46 04 2b 00 e8 03 8d 04 ee 04 df 04 12
8.880232      🟩P→C   a0 11 f0 ac ac a7 61 46 05 2b 00 e8 03 a9 03 7e 03 67 03 48
8.920231      🟩P→C   a0 11 f0 ae ac a7 61 46 01 2b 00 e8 03 4d 03 22 03 e0 02 06
9.040232      🟩P→C   a0 11 f0 b0 ac a7 61 46 00 2b 00 e8 03 98 02 41 02 e2 01 70
9.120232      🟩P→C   a0 11 f0 b2 ac a7 61 46 00 2b 00 e8 03 78 01 0d 01 a7 00 f0
9.240234      🟩P→C   a0 11 f0 b4 ac a7 61 46 00 2b 00 e8 03 45 00 00 00 00 00 f9
9.360235      🟩P→C   a0 11 f0 b6 ac a7 61 46 00 2b 00 e8 03 00 00 00 00 00 00 b6
9.400235      🟩P→C   a0 11 f0 b8 ac a7 61 46 00 2b 00 e8 03 00 00 40 01 eb 02 e6
9.520236      🟩P→C   a0 11 f0 ba ac a7 61 46 08 27 00 e8 03 fb 03 54 04 49 04 61
9.600236      🟩P→C   a0 11 f0 bc ac a7 61 46 07 27 00 e8 03 44 05 e6 04 7e 04 74
9.720236      🟩P→C   a0 11 f0 be ac a7 61 46 06 27 00 e8 03 2a 04 fd 03 eb 03 dc
9.840238      🟩P→C   a0 11 f0 c0 ac a7 61 46 03 27 00 e8 03 d2 03 a8 03 66 03 a8
9.880238      🟩P→C   a0 11 f0 c2 ac a7 61 46 00 27 00 e8 03 15 03 b9 02 5c 02 ef
10.000239     🟩P→C   a0 11 f0 c4 ac a7 61 46 00 27 00 e8 03 fa 01 95 01 2d 01 7f
10.120239     🟩P→C   a0 11 f0 c6 ac a7 61 46 00 27 00 e8 03 cf 00 78 00 24 00 2d
10.200241     🟩P→C   a0 11 f0 c8 ac a7 61 46 01 27 00 e8 03 00 00 00 00 00 00 c5
10.320241     🟩P→C   a0 11 f0 ca ac a7 61 46 00 27 00 e8 03 00 00 76 00 2a 02 68
10.360241     🟩P→C   a0 11 f0 cc ac a7 61 46 06 27 00 e8 03 ab 03 6a 04 8d 04 7b
10.480243     🟩P→C   a0 11 f0 ce ac a7 61 46 06 25 00 e8 03 59 05 11 05 b3 04 f9
10.560244     🟩P→C   a0 11 f0 d0 ac a7 61 46 07 25 00 e8 03 4c 04 f8 03 c7 03 e6
10.680245     🟩P→C   a0 11 f0 d2 ac a7 61 46 02 25 00 e8 03 ab 03 87 03 55 03 5e
10.800245     🟩P→C   a0 11 f0 d4 ac a7 61 46 00 25 00 e8 03 0d 03 ba 02 5a 02 f6
10.840246     🟩P→C   a0 11 f0 d6 ac a7 61 46 00 25 00 e8 03 f0 01 7e 01 0e 01 4f
10.960247     🟩P→C   a0 11 f0 d8 ac a7 61 46 00 25 00 e8 03 a5 00 41 00 00 00 b8
```

## Skipped captures

These capture files did not contain a central stage-1 write matching the prefix `b0111101ac...`, so no vendor timeline section was emitted for them:

- `data/gk-plus-1.pcapng`
- `data/gk-plus-2.pcapng`
