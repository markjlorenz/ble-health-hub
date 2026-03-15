# Wireshark sniffing workflow (BLE)

Start here when you’re capturing BLE traffic in a noisy RF environment and want a repeatable process for getting a clean `.pcapng` you can decode later.

## Advertising-only “noise gate” display filter

Paste this into Wireshark’s display filter bar to show *only advertising PDUs* and hide common “background” devices by **Company Id** (the same value shown in Wireshark’s “Company Id” column).

### What “Company Id” means in Wireshark

Wireshark’s **Company Id** column for BLE advertising typically comes from **Manufacturer Specific Data** (AD type `0xFF`). The field

- `btcommon.eir_ad.entry.company_id`

is the **Bluetooth SIG Company Identifier** (an assigned 16-bit number), and Wireshark displays it using the Bluetooth SIG company table.

This is why it looks like an “enumeration”: the on-air packet includes the numeric company identifier, and Wireshark resolves it to a human-readable name.

### Preferred: exclude by specific Company ID bytes (hex)

This is the most robust approach in a noisy environment: filter by the specific numeric company IDs you want to exclude.

IDs used below (from the Bluetooth SIG company table as shown by Wireshark):

- `0x004c` = Apple, Inc. (76)
- `0x05a7` = Sonos Inc (1447)
- `0x01bf` = Hong Kong HunterSun Electronic Limited (447)
- `0x0819` = Hunter Douglas Inc (2073)

```wireshark
btle.advertising_header
&& !(
   btcommon.eir_ad.entry.company_id == 0x004c
   || btcommon.eir_ad.entry.company_id == 0x05a7
   || btcommon.eir_ad.entry.company_id == 0x01bf
   || btcommon.eir_ad.entry.company_id == 0x0819
)
&& !(btcommon.eir_ad.entry.device_name matches "(?i)^govee")
```

If you want to exclude “Hunter*” more generally, you’ll need to enumerate all matching entries you care about (because the underlying values are distinct numeric IDs).

### How to look up the hex Company ID from a company name (using tshark)

When you have a manufacturer/company name (e.g. “Apple”, “Sonos”, “Hunter”), you can ask `tshark` for the Bluetooth SIG mapping and then convert the decimal ID to hex.

This repo’s convention is dockerized tshark:

```bash
docker run --rm --platform linux/amd64 cincan/tshark -G values \
   | grep -i "btcommon.eir_ad.entry.company_id" \
   | grep -i "apple" \
   | head
```

Example outputs we used:

- `Apple, Inc.` → `76` → `0x004c`
- `Sonos Inc` → `1447` → `0x05a7`
- `Hong Kong HunterSun Electronic Limited` → `447` → `0x01bf`
- `Hunter Douglas Inc` → `2073` → `0x0819`

To search multiple terms:

```bash
docker run --rm --platform linux/amd64 cincan/tshark -G values \
   | grep -i "btcommon.eir_ad.entry.company_id" \
   | grep -Ei "apple|sonos|hunter" \
   | head -n 50
```

Then plug the hex IDs into the display filter above.

### Optional: match the resolved company name (exact equality)

Wireshark displays `btcommon.eir_ad.entry.company_id` using the Bluetooth SIG company table.
In many builds, the display-filter engine also accepts **exact string equality** against that resolved label:

```wireshark
btle.advertising_header
&& !(
   btcommon.eir_ad.entry.company_id == "Apple, Inc."
   || btcommon.eir_ad.entry.company_id == "Sonos Inc"
   || btcommon.eir_ad.entry.company_id == "Hunter Douglas Inc"
)
```

However, **regex matching on the resolved label is not reliably supported** because the underlying field is numeric.
If you want regex-like selection by company name (e.g. exclude anything containing `hunter`), use the documented lookup workflow above:

1. Use `tshark -G values` + `grep -i hunter` to find all matching Company IDs.
2. Convert those decimal IDs to hex.
3. Exclude those hex IDs in the display filter.

Notes:

- `btle.advertising_header` restricts the view to advertising-channel PDUs (ADV_*).
- `btcommon.eir_ad.entry.company_id` is only present when the advertiser includes **Manufacturer Specific Data** (AD type `0xFF`). If a packet has no manufacturer data, it will still pass this filter.

Quick sanity check (recommended): click one of the packets you still see, expand the packet details to the **Company ID** field, then right-click it and use **Apply as Filter → Selected**. The generated filter tells you exactly which field Wireshark is using for that row, so you can confirm you’re filtering the right thing.

## Capture setup (nRF Sniffer for Bluetooth LE)

1. Install and configure **nRF Sniffer for Bluetooth LE** so it appears as a Wireshark capture interface.
2. In Wireshark, enable the sniffer toolbar:
   - View → Interface Toolbars → enable **nRF Sniffer for Bluetooth LE**.
3. Start the capture on the nRF Sniffer interface.
4. Apply the advertising-only filter above immediately to reduce noise.

## Identify the target device (from advertising)

Use one or more of these heuristics:

- **Device name** (if present): match the advertised local name.
- **Service UUIDs / Service Data**: many devices advertise a distinctive 16-bit/128-bit service UUID or Service Data.
- **RSSI**: if you’re physically close to the target, sort mentally by stronger (less negative) RSSI.

Fields you’ll commonly use:

- `btle.advertising_address` (the advertiser’s address)
- `btcommon.eir_ad.*` (decoded advertising payload entries)

## Follow the target connection

Once you have the device address, switch from “discovery” to “session capture”:

1. In the nRF Sniffer toolbar:
   - Set **Key** to **Follow LE address**.
   - Set **Value** to the device address (example format: `aa:bb:cc:dd:ee:ff`).
2. Keep the capture running while you perform the action that generates the data you care about (connect, start measurement, fetch history, etc.).
3. Save the capture as `.pcapng` under `data/`.

Tip: many peripherals use BLE privacy; the address you follow may change across power cycles. Re-discover the advertiser if a capture “goes silent”.

Important: over-the-air sniffers (including nRF Sniffer) generally **cannot join an existing connection mid-stream**. To capture GATT/ATT traffic, start the capture first (while the peripheral is advertising), then initiate the connection from the phone/app.

### Doing “Follow LE address” in tshark (post-capture)

If your capture is from **nRF Sniffer (over-the-air)**, you’re already capturing RF traffic, so you will see **both directions** (central → peripheral and peripheral → central) once the sniffer locks onto the connection.

In `tshark`, you can get most of the benefit of Wireshark’s “Follow LE address” by filtering on the peripheral address fields:

```text
(btle.advertising_address == <PERIPHERAL>) || (btle.slave_bd_addr == <PERIPHERAL>)
```

- `btle.advertising_address` matches the peripheral’s advertising packets.
- `btle.slave_bd_addr` matches *connected* packets where the peripheral is the BLE “slave”. This includes packets in **both directions**.

Example (GK+):

```text
(btle.advertising_address == e4:33:bb:84:83:66) || (btle.slave_bd_addr == e4:33:bb:84:83:66)
```

#### If there are multiple connections, narrow to a single session

In a noisy environment (or long captures), you may have multiple connection sessions involving the same peripheral address. A reliable way to isolate one session is to additionally filter by the connection’s **Access Address** (a 32-bit value that identifies the link once connected):

1. First, find candidate access addresses for the peripheral:

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work cincan/tshark \
   -r data/your-capture.pcapng \
   -Y "btle.slave_bd_addr == e4:33:bb:84:83:66" \
   -T fields -e btle.access_address \
   | head -n 20
```

2. Then pin to one access address:

```text
btle.access_address == 0x32e7b04e
```

3. Combine them:

```text
(btle.access_address == 0x32e7b04e) && (btle.slave_bd_addr == e4:33:bb:84:83:66)
```

This is the closest “tshark equivalent” to following a single connection instance.

## CLI live sniffing on macOS (how we got here)

This section records the exact steps we used to get from “Wireshark Follow LE address is tedious” to a workable **CLI-first** workflow.

### 1) Docker can’t do live extcap sniffing on macOS

Even with `--privileged`, Docker Desktop containers run inside a Linux VM and generally **cannot access** macOS USB/extcap devices directly. So the “dockerized tshark” approach is great for **post-capture analysis**, but not for **live capture** from the nRF Sniffer.

### 2) Confirm host `tshark` exists

On this machine, `tshark` is bundled with the Wireshark app:

```bash
command -v tshark || ls -1 /Applications/Wireshark.app/Contents/MacOS/tshark
```

Result (example):

- `/Applications/Wireshark.app/Contents/MacOS/tshark`

### 3) Check whether the nRF Sniffer interface is visible to tshark

List capture interfaces:

```bash
/Applications/Wireshark.app/Contents/MacOS/tshark -D
```

If the nRF Sniffer extcap is installed correctly, you’d expect to see an interface corresponding to it. In our case it did **not** show up.

### 4) Inspect Wireshark extcap helpers

Wireshark discovers “special” capture sources via **extcap** helper binaries.

We checked the app bundle’s extcap directory:

```bash
ls -la /Applications/Wireshark.app/Contents/MacOS/extcap
```

It contained common helpers (e.g. `sshdump`, `udpdump`, …) but not an `nrf_sniffer_ble` helper.

Then we checked user-level extcap directories:

```bash
ls -la "$HOME/Library/Application Support/Wireshark/extcap" 2>/dev/null
ls -la "$HOME/.config/wireshark/extcap" 2>/dev/null
ls -la "$HOME/.local/lib/wireshark/extcap" 2>/dev/null
```

We found Nordic’s nrfutil-based extcap shim here:

- `~/.local/lib/wireshark/extcap/nrfutil-ble-sniffer-shim`

### 5) Verify the shim supports extcap capture

The shim reports supported options like `--capture`, `--extcap-interface`, and `--fifo`:

```bash
~/.local/lib/wireshark/extcap/nrfutil-ble-sniffer-shim --help
```

Note: on this machine, `--extcap-interfaces` currently errors (panic) due to the underlying `nrfutil-ble-sniffer` subprocess returning exit code 101. That’s OK; we can still run captures if we know the serial port.

### 6) Find the nRF Sniffer serial port

On macOS, the sniffer typically appears as a `/dev/cu.usbmodem*` device.

```bash
ls -1 /dev/cu.usbmodem* /dev/tty.usbmodem* 2>/dev/null | sort
```

Result on this machine:

- `/dev/cu.usbmodem1401`
- `/dev/tty.usbmodem1401`

Prefer the `cu.*` device for capturing (`/dev/cu.usbmodem1401`).

### 7) Capture to a file, then “follow” in tshark

At this point the workflow becomes:

1. **Live capture** to a `.pcap`/`.pcapng` file using the shim.
2. **Post-capture filter** the conversation with `tshark` display filters (e.g. `btle.slave_bd_addr == <peripheral>` and/or `btle.access_address == <access>`).

### Address type note (public vs random)

Some devices appear with both a **Public** and **Random** address type in the sniffer’s discovery output/logs.

With `nrfutil-ble-sniffer sniff`, you can force the address type used for following by appending it to `--follow`:

```bash
~/.nrfutil/bin/nrfutil-ble-sniffer sniff \
   --port /dev/cu.usbmodem1401 \
   --follow 'e4:33:bb:84:83:66 public' \
   --output-pcap-file data/gkplus.pcap
```

If you suspect the device is using a random/private address for the session, try:

```bash
--follow 'e4:33:bb:84:83:66 random'
```

## GK+ “conversation” capture recipe (nrfutil-ble-sniffer)

This is the repeatable workflow we used to capture a GK+ session that includes the `0xdb` **records-count** notifications.

Note: `0xdb` is not glucose/ketone. To decode actual measurements + timestamps, we need captures that include the record-transfer (records-mode) frames.

### Prereqs

- Close Wireshark (or at least stop any extcap capture) so it doesn’t hold the sniffer port.
- Identify your sniffer’s serial port:

```bash
ls -1 /dev/cu.usbmodem* /dev/tty.usbmodem* 2>/dev/null | sort
```

On this machine, the sniffer is:

- `/dev/cu.usbmodem1401`

GK+ address we’ve observed in ads/pcaps:

- `e4:33:bb:84:83:66`

### User steps (order matters)

1. Ensure the GK+ is **not already connected** to the official app.
    - Force-quit the official app, or toggle phone Bluetooth OFF/ON.
    - If needed, power-cycle the meter so it’s definitely advertising.
2. Start the capture (command below) **before** connecting in the app.
3. Connect in the official app and perform the action you want to capture (connect + measurement + history view, etc.).
4. Wait ~20–30 seconds after connect.
5. Stop the capture with Ctrl+C.

### Capture command (recommended: follow RANDOM)

In our environment, the capture that successfully included connected traffic + `0xdb` used the **random** address type:

```bash
mkdir -p data
out="data/gkplus-$(date +%Y%m%d-%H%M%S).pcap"

~/.nrfutil/bin/nrfutil-ble-sniffer sniff \
   --port /dev/cu.usbmodem1401 \
   --follow 'e4:33:bb:84:83:66 random' \
   --timeout 8000 \
   --rssi-cut-off -80 \
   --output-pcap-file "$out"
```

### If that fails, try following PUBLIC

```bash
mkdir -p data
out="data/gkplus-$(date +%Y%m%d-%H%M%S).pcap"

~/.nrfutil/bin/nrfutil-ble-sniffer sniff \
   --port /dev/cu.usbmodem1401 \
   --follow 'e4:33:bb:84:83:66 public' \
   --timeout 8000 \
   --rssi-cut-off -80 \
   --output-pcap-file "$out"
```

### Post-capture: verify you actually got a connection

Advertising-only captures will have almost exclusively `btle.access_address == 0x8e89bed6`.
Connected captures will have at least one additional access address.

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work cincan/tshark \
   -r "$out" -T fields -e btle.access_address \
   | sort | uniq -c | sort -nr | head
```

### Post-capture: extract the `0xdb` payload(s)

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work cincan/tshark \
   -r "$out" \
   -Y 'btatt.opcode==0x1b && btatt.value contains db:aa' \
   -T fields -e frame.number -e frame.time_relative -e btatt.value
```

Then decode one payload with:

```bash
python3 tools/gkplus_decode_test.py --hex <btatt.value>
```

## Useful display filters during analysis

All traffic for a connected peripheral (common with nRF Sniffer captures):

```wireshark
btle.slave_bd_addr == aa:bb:cc:dd:ee:ff
```

ATT only (often the most actionable for reverse engineering):

```wireshark
btatt && btle.slave_bd_addr == aa:bb:cc:dd:ee:ff
```

Only notifications (where measurements often arrive):

```wireshark
btatt.opcode == 0x1b && btle.slave_bd_addr == aa:bb:cc:dd:ee:ff
```

Only writes from central → peripheral (when hunting commands/state machines):

```wireshark
(btatt.opcode == 0x52 || btatt.opcode == 0x12) && btle.slave_bd_addr == aa:bb:cc:dd:ee:ff
```

## Extracting timelines with dockerized tshark (repo convention)

This repo standardizes on running `tshark` in Docker:

```bash
docker run --rm -v "$PWD":/work -w /work cincan/tshark \
  -r data/your-capture.pcapng \
  -Y "<display-filter>" \
  -T fields -e frame.number -e frame.time_relative -e btatt.opcode -e btatt.handle -e btatt.value
```

On Apple Silicon, you may need:

```bash
docker run --platform linux/amd64 --rm -v "$PWD":/work -w /work cincan/tshark ...
```

## Checklist for a “good” capture

- Includes the target’s advertising so you can confirm the right device.
- Includes the connect + GATT discovery (or at least service/characteristic discovery).
- Includes notification subscription writes (CCCD writes) for the characteristic(s) that stream data.
- Includes at least one full measurement/notification payload (or the specific command/response pair you’re trying to decode).
