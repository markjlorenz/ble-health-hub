# GK+ known samples

This document records GK+ `0xdb` payload samples.

## Correction (from official APK analysis)

Static analysis of **Keto-Mojo Classic 2.6.6** (package `com.ketomojo.app`) shows:

- `0xdb` packets are used for **records count** (not glucose/ketone values).
- Actual glucose/ketone measurements (and their timestamps) are carried in **record payloads** parsed by the SDK (`VivaCheckRecordsParser`). Those record payloads are **not** present in the pcaps/log snippets currently listed in this table.

## Samples (`0xdb` records count)

The `0xdb` response contains a **records count** (not glucose/ketone).

Count is encoded base-100 in two bytes:

- `count = payload[9] * 100 + payload[10]`

| Sample | Source | `0x44` set-time (decoded) | `0xdb` payload | Records count |
|---|---|---|---|---:|
| A | live hex | (unknown) | `7b01200110dbaa0002012100020b057d` | 133 |
| B | pcap: `data/gk-plus-1.pcapng` | 2026-03-01 10:05:21 | `7b01200110dbaa0002010f08020a097d` | 115 |
| C | pcap: `data/gk-plus-2.pcapng` | 2026-03-02 18:35:08 | `7b01200110dbaa0002011100020a017d` | 117 |
| D | live log (2026-03-11) | 2026-03-11 08:49:41 | `7b01200110dbaa00020123080307047d` | 135 |
| E | live log (2026-03-12) | 2026-03-12 09:17:17 | `7b01200110dbaa00020125000307067d` | 137 |
| F | live log (2026-03-13) | 2026-03-13 09:02:15 | `7b01200110dbaa0002012708020b077d` | 139 |
| G | live log (2026-03-14) + pcap: `data/gkplus-random-20260314-122048.pcap` | 2026-03-14 10:26:39 | `7b01200110dbaa00020129000307037d` | 141 |

## Notes

- Samples **B** and **C** each repeat the same payload multiple times in their capture.
- The pcap timestamps above are **capture-time (on-air)** times.
- The `0x44` “set time” values above are decoded from the central write payload in the pcap; they appear to be `YY MM DD HH MM SS` with `YY` as years since 2000.
- In the confirming over-the-air pcap for **G**, the same `0xdb` payload repeats across multiple notifications.

### Official `0xdb` meaning (APK-derived)

In the official SDK (inside the APK), `0xdb` is used for **records count**.

- Request (C->P): `7B01100120DB55...7D`
- Response (P->C notify): `7B01200110DBAA...7D`
- Count is encoded base-100 in two bytes (hundreds, remainder):
	- `count = payload[9] * 100 + payload[10]`

The packets also carry a CRC16/MODBUS (poly `0xA001`, init `0xFFFF`) encoded as 4 bytes of *nibbles* before the trailing `0x7d`.

## Working theory (updated)

- The meter uses `0x44` (set time) to sync its internal clock.
- Measurement timestamps are carried inside **record payloads**, not inside `0xdb`.

Record timestamp decode (VivaChek SDK) for the 9-byte record snippet:

- `YY MM DD HH mm` as raw bytes, where `YY` is years since 2000.
- Interpreted in the phone's local timezone (TimeZone.getDefault()).

## Next experiment

- Capture a pcap while the official app actually **downloads records** (not just records count). We should see response packets starting with one of:
	- `7B01200110DDAA0009`
	- `7B0120011016AA0009`
	- `7B01200110DEAA0015`

Those packets contain the 9-byte record snippet at bytes `[9:18]` (hex substring `[18:36]`), which includes the timestamp and the base-100 value bytes.

