# Keto-Mojo Classic APK analysis (GK+/VivaChek SDK)

This note records static findings from the official Android app APK:

- APK: `vendor/keto-mojo/Keto-Mojo Classic_2.6.6_APKPure.apk`
- Package: `com.ketomojo.app`

APK download (GitHub Release asset):

- Release: https://github.com/markjlorenz/ble-health-hub/releases/tag/keto-mojo-classic-apk-2.6.6
- Asset: `Keto-Mojo.Classic_2.6.6_APKPure.apk`

The GK+/GKI device support in this version routes through the internal SDK package `com/ketomojo/sdk/device/vivachek/...`.

## Where the logic lives

Key classes (DEX: `classes2.dex`):

- BLE wiring:
  - `Lcom/ketomojo/sdk/device/vivachek/VivaChekMeterImpl;`
- Packet framing/buffering:
  - `Lcom/ketomojo/sdk/device/vivachek/VivaChekDataBuffer;`
- Packet CRC validation:
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/CommandUtils;`
  - `Lcom/ketomojo/sdk/device/vivachek/Crc16Utils;`
- Records count parsing (`0xdb`):
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/CountParser;`
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/HexRecordsCountParser;`
- Record (history) parsing:
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/VivaCheckRecordsParser;`
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/VivaChekTimeParser;`
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/BloodValuesParser;`
- Derived GKI records:
  - `Lcom/ketomojo/sdk/device/vivachek/parsers/GkiRecordFactory;`

## Frame format

All vendor frames in this SDK are represented as hex strings and use:

- Start byte `0x7B`
- End byte `0x7D`

### CRC16/MODBUS (and its on-wire encoding)

Validation is performed by `CommandUtils.isCommandCorrect(dataHex)`.

- CRC16: **CRC-16/MODBUS** (poly `0xA001`, init `0xFFFF`)
- CRC input bytes: everything *between* `0x7B` and the 4-byte CRC, i.e. bytes `payload[1:-5]`

The app stores the CRC in an unusual nibble-expanded form:

- Take `crc16` and format as `%04X` (four hex digits), e.g. `"7303"`.
- Convert each digit to a byte `0..15`: `[7,3,0,3]`.
- Swap pairs: `[[7,3],[0,3]] -> [[0,3],[7,3]]`.
- The sent CRC bytes are thus `[0,3,7,3]`, and when hex-encoded become `"00030703"`.

## `0xdb` is **records count**

The SDK matches notify frames starting with:

- `7B01200110DBAA...7D`

This is **not** glucose/ketone.

The records count is encoded base-100 in two bytes:

- `count = b0*100 + b1`

In the GK+ pcap examples you can see `... 01 29 ...` which decodes to `1*100 + 0x29(41) = 141` records.

## Record transfer (where values + timestamps live)

Record payloads are decoded by `VivaCheckRecordsParser` and are emitted only when the SDK enters a special “records mode” in `VivaChekDataBuffer`.

### Records-mode triggers

`VivaChekDataBuffer` sets `recordsMode=true` when a notification starts with one of:

- `7B01200110DDAA0009`
- `7B0120011016AA0009`
- `7B01200110DEAA0015`

Records-mode ends when the tail matches:

- `7B01200110D16600000B0903047D`

### How the app triggers record download (central writes)

The SDK uses two related commands to trigger record transfer:

- **Parameterized 0x16 command** (preferred): built by
  `Lcom/ketomojo/sdk/device/vivachek/LatestRecordsCommandFactory;->generateCommand(I)`.
  - Body hex: `0110012016550002` + `%04x` (a 16-bit parameter, `0..1000`)
  - On-wire request: `0x7B` + body + CRC send-bytes + `0x7D`
    - CRC is CRC-16/MODBUS over the *body bytes* and encoded as 4 nibble-bytes (same encoding as responses).

- **Fallback constant 0xDD request**: `7B01100120DD550000030A060C7D`
  - Used by `VivaChekMeterImpl.getRecords()` / `getLatestRecords()` when the SDK decides it cannot do an incremental load.

On the notify side, record-transfer traffic begins with frames whose payload starts with one of:

- `7B01200110DDAA0009`
- `7B0120011016AA0009`
- `7B01200110DEAA0015`

### Where the 9-byte record snippet sits in each frame

`VivaCheckRecordsParser.parseRecords()` extracts, for each frame segment, a fixed substring:

- hex substring `[18:36]` (i.e. 9 bytes at `frame_bytes[9:18]`)

That 9-byte snippet is what carries **timestamp + value + metadata**.

### 9-byte record snippet layout

Decoded by `VivaCheckRecordsParser.extractRecord()` + `VivaChekTimeParser` + `BloodValuesParser`:

Byte offsets (within the 9-byte snippet):

- `0` : `YY` (years since 2000)
- `1` : `MM` (1-12)
- `2` : `DD`
- `3` : `HH`
- `4` : `mm`
- `5` : value-hundreds
- `6` : value-remainder (0-99)
- `7` : flags
  - high nibble (`flags >> 4`): prandial
    - `1` => PRE
    - `2` => POST
    - else => GENERAL
- `8` : sample type code
  - `0x11` => GLUCOSE
  - `0x12` => GLUCOSE (or GLUCOSE_GKI if paired with a KETONE_GKI record)
  - `0x22` => GLUCOSE_QC
  - `0x55` => KETONE
  - `0x56` => KETONE_GKI
  - `0x66` => KETONE_QC

### Value conversion

Values use a base-100 encoding:

- `raw = byte5*100 + byte6`

Then:

- Glucose (raw is mg/dL):
  - if displaying mmol/L: `raw / 18`
  - else: `raw`
- Ketone (mmol/L):
  - `raw / 100`

### Timestamp conversion

`VivaChekTimeParser.parseRegularTime()` interprets the first 5 bytes as:

- `year = 2000 + YY`
- `month = MM` (Calendar uses `MM-1` internally)
- `day = DD`
- `hour = HH`
- `minute = mm`

It uses the phone’s local timezone (`TimeZone.getDefault()`). Seconds are not present for regular record timestamps.

## GKI derivation

The SDK derives GKI as a synthetic record when it sees adjacent records of types:

- first: `KETONE_GKI`
- second: `GLUCOSE_GKI`

`GkiRecordFactory.createGkiRecord(glucose, ketone)`:

- converts both to mmol
- divides with BigDecimal precision=4, rounding mode HALF_EVEN
- then applies `NumberUtils.roundDown(x) = floor(x*18)/18`

This matches a “truncate to the nearest 1 mg/dL step in mmol-space” style for displayed GKI.
