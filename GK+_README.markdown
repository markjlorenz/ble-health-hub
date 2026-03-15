# GK+ (reverse engineering notes)

This doc captures what we can reliably infer from:

- `data/gk-plus-1.pcapng`
- `data/gk-plus-2.pcapng`

Observed device address in these captures:

- GK+ BLE address: `e4:33:bb:84:83:66` (as seen in advertising/connection packets)

Note: depending on OS/device settings, some peripherals may use BLE privacy and change addresses across sessions. Re-confirm via advertising if a capture “goes silent”.

The device speaks a small vendor protocol over BLE GATT. Payloads are framed with
`0x7b` … `0x7d` (ASCII `{` / `}`), and the official app polls for **records count**
(`0xdb`) before requesting history records (which contain the actual glucose/ketone values and timestamps).

APK-derived protocol notes (authoritative decoding + timestamp logic):

- [vendor/keto-mojo/APK_ANALYSIS.md](vendor/keto-mojo/APK_ANALYSIS.md)

## Reverse engineering workflow (step-by-step)

This is the process we actually followed (and can repeat) to go from “unknown BLE traffic” to a decoder that matches the official app.

### 1) Capture the traffic

Goal: capture a full GK+ session over the air so we can see ATT writes/notifies.

- Capture workflow + commands (recommended):
	- [WIRESHARK_SNIFFING_README.markdown](WIRESHARK_SNIFFING_README.markdown)
- Put captures under `data/`.

Minimum useful capture:

- A connect + notification subscription
- At least one `0xdb` notify response

Best capture (needed to decode *measurements*):

- A record-transfer / records-mode sequence (see “Where the actual measurements live” below)

### 2) Identify the vendor protocol frames

From the capture, filter down to ATT payloads on the GK+ notify/write characteristics.
Then look for the clear framing boundary:

- Start byte: `0x7b`
- End byte: `0x7d`

That gives us a reliable way to split and parse messages.

### 3) Inventory the command IDs (pcap-driven)

From the pcaps we first saw only a handful of commands:

- `0x66`, `0x77`, `0xaa`, `0x44`, `0xdb`

At this point we still *did not* have enough information to decode glucose/ketone.

### 4) Determine what `0xdb` means (and validate CRC)

We initially treated `0xdb` as “measurement-ish” because it was regularly polled, but APK analysis confirmed the truth:

- `0xdb` is **records count**, encoded base-100 in 2 bytes.
- Frames also contain a CRC16/MODBUS encoded as 4 “nibble bytes” before the trailing `0x7d`.

You can validate a captured `0xdb` payload with:

```bash
.venv/bin/python tools/gkplus_decode_test.py --hex <btatt.value>
```

For a set of known-good samples, see:

- [GK+_SAMPLES.markdown](GK+_SAMPLES.markdown)

### 5) Use the APK to find where measurements really live

Static analysis of the official Android app (Keto-Mojo Classic 2.6.6) showed:

- Glucose/ketone values + timestamps are carried in **record-transfer** frames, not `0xdb`.
- The parser extracts a 9-byte “record snippet” from each record-transfer frame.
- The app triggers record download using a parameterized **0x16** request (with CRC).

The full details (class names, CRC rules, record layout) are documented here:

- [vendor/keto-mojo/APK_ANALYSIS.md](vendor/keto-mojo/APK_ANALYSIS.md)

### 6) Implement decoding and validate against real vectors

Once we had the record snippet layout and value conversions, we implemented:

- `0xdb` records-count decoding + CRC validation
- 9-byte record snippet decoding (timestamp, sample type, prandial tag, value)
- Web Bluetooth tooling to request records and decode record-transfer notifications

We then captured real readings and saved them as regression vectors.

## Regression test (official-app-aligned)

We maintain a small set of record-snippet vectors (glucose+ketone pairs) and assert:

- Glucose shown as integer mg/dL
- Ketone shown rounded to 1 decimal
- GKI computed from *shown* values, then truncated to 1 decimal

Run it with:

```bash
.venv/bin/python tools/gkplus_regression_test.py
```

Expected output:

```text
OK (5 cases)
```

The fixtures live here:

- `tools/gkplus_regression_vectors.json`
- `tools/gkplus_regression_table_20260314.md`

## Sniffing (Wireshark)

See [WIRESHARK_SNIFFING_README.markdown](WIRESHARK_SNIFFING_README.markdown) for a repeatable Wireshark + nRF Sniffer workflow and a starting advertising-only noise filter.

## Quick start (web)

There is a dedicated GK+ Web Bluetooth page:

- `web/gk-plus.html`

It:

- Connects to the GK+ vendor service
- Enables notifications
- Sends the same request sequence observed in the pcaps
- Decodes the `0xdb` response as **records count** (APK-derived)
- Shows a Wireshark-like TX/RX timeline in the Debug log

Notes:

- Web Bluetooth requires a Chromium browser (Chrome/Edge).
- Use HTTPS or `http://localhost`.
- The GK+ page uses the tightest Web Bluetooth chooser filter we can justify
	from these pcaps: advertised Service Data UUID `0x78ac` (AD type `0x16`).
	In Web Bluetooth filter syntax this is passed as the numeric alias `0x78ac`.

	Important limitation: Web Bluetooth can only filter the chooser on advertised
	name/services. In these pcaps the meter does not advertise a local name, and
	it does not advertise the vendor GATT service UUID either — so a filtered
	chooser can appear “blank”.

	The GK+ page therefore tries a tight chooser filter first (currently a
	`namePrefix` of `Keto-Mojo`, as observed in Chrome: “Keto-Mojo GK+ Meter - Paired”).
	If that yields no results, it falls back to `acceptAllDevices: true`.

	In all cases it validates after connection that the selected device exposes
	the GK+ vendor service, and fails fast with a clear error if you picked the
	wrong device.

## GATT: service + characteristics

From the pcaps, the vendor service is exposed as a 128-bit UUID:

- Service UUID: `0003cdd0-0000-1000-8000-00805f9b0131`

On the live device/OS stack, this may also appear via a 16-bit wrapper service
`0xFFF0` with a single `0xFFF3` characteristic. In our testing, that
`FFF0/FFF3` path emits empty notifications and does not carry the framed payloads.

Within the `0003cdd0…` service, there are two characteristics we care about:


- Notify characteristic (peripheral → central)
	- UUID: `0003cdd1-0000-1000-8000-00805f9b0131`
- Write characteristic (central → peripheral)
	- UUID: `0003cdd2-0000-1000-8000-00805f9b0131`
	- Properties observed: Write Without Response

In both pcaps, the app writes framed requests to the write characteristic and
receives framed notifications from the notify characteristic.

## Vendor message framing

All vendor messages we observed are delimited:

- Start byte: `0x7b`
- End byte: `0x7d`

Examples:

- Request (central → peripheral):
	- `7b 01 10 01 20 db 55 00 00 03 0a 0e 04 7d`
- Response (peripheral → central):
	- `7b 01 20 01 10 db aa 00 02 01 0f 08 02 0a 09 7d`

The leading header bytes are stable across all messages:

```
7b  01  <src> 01 <dst>  <cmd> <marker> ...  7d
```

- `src` and `dst` appear swapped between request/response (`0x10` vs `0x20`).
- No simple sum/xor checksum matched across frames; the penultimate byte appears
	to be data, not a checksum.

## Observed commands

The phone app issues only a handful of command IDs in these pcaps:

### `0x66` (info)

Request:

- `7b0110012066550000010e08087d`

Response (contains ASCII):

- `7b0120011066aa0010 53573230373133323330313141303031 060b0f0a7d`
- ASCII section: `SW2071323011A001` (likely a model/serial string)

### `0x77` (info)

Request:

- `7b0110012077550000010b0b047d`

Response (contains ASCII):

- `7b0120011077aa0010 30303030303331364130303336363032 0f000a087d`
- ASCII section: `00000316A0036602` (likely version/build)

### `0xaa` (init?)

Request:

- `7b01100120aa55000002010d087d`

Response:

- `7b01200110aaaa0001110e0f07047d`

### `0x44` (time sync?)

In both pcaps the app also sends a longer `0x44` request containing bytes that
look like a timestamp (year/month/day/hour/minute/second). The exact format of
the trailing bytes is still unclear.

Example request (from `gk-plus-1`):

- `7b01100120446600061a03010a05150c000a0b7d`

Response:

- `7b012001104499000111000802077d`

### `0xdb` (records count)

The app repeatedly writes the same `0xdb` request:

- `7b01100120db550000030a0e047d`

The device responds (notification):

- `7b01200110dbaa0002010f08020a097d` (pcap 1)
- `7b01200110dbaa0002011100020a017d` (pcap 2)

APK-derived meaning:

- This is **records count**, not glucose/ketone.
- Count is base-100 in two bytes:
	- `count = payload[9] * 100 + payload[10]`
- The 4 bytes before the trailing `0x7d` are a CRC16/MODBUS encoded as 4 nibble-bytes (see vendor/keto-mojo/APK_ANALYSIS.md).

## Where the actual measurements live

Per the official SDK (APK): glucose/ketone values and timestamps are carried in **record-transfer** packets (records-mode), not in `0xdb`.

To decode real readings end-to-end we need captures that include records-mode frames starting with:

- `7B01200110DDAA0009`
- `7B0120011016AA0009`
- `7B01200110DEAA0015`

## Capturing a session (Wireshark + nRF Sniffer)

Captured example (2026-03-14):

- `data/gkplus-random-20260314-122048.pcap` contains the official-app session and repeated `0xdb` records-count notifications (payload `7b01200110dbaa00020129000307037d`).

1. In Wireshark, under View > Interface Toolbars enable `nRF Sniffer for Bluetooth LE`.
2. Set the `Key` dropdown to `Follow LE address`.
3. Set the `Value` to the device address (example from these pcaps: `e4:33:bb:84:83:66 public`).

If you capture a full connection + measurement session, add it under `data/`.
The most useful captures include:

- A connect + GATT discovery
- The initial notification subscription (CCCD write)
- At least one `0xdb` response (records count)
- Ideally: records-mode / record-transfer frames (see above) so we can decode actual glucose/ketone values and timestamps

