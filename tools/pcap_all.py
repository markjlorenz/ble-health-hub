from scapy.all import PcapNgReader

packets = []
for p in PcapNgReader('data/pulse-ox.pcapng'):
    packets.append(bytes(p))

# Find ALL relevant ATT traffic (both writes and notifications) on handle 0x0027
# ATT Notification opcode = 0x1b  (device->host)
# ATT Write Command opcode = 0x52 (host->device, write without response)
# ATT Write Request opcode = 0x12 (host->device, write with response)

all_events = []
for idx, raw in enumerate(packets):
    # Notification
    pos = raw.find(b'\x1b\x27\x00')
    if pos != -1:
        payload = raw[pos+3:]
        all_events.append((idx, 'NOTIFY', payload[:20].hex()))
    # Write without response
    pos = raw.find(b'\x52\x27\x00')
    if pos != -1:
        payload = raw[pos+3:]
        all_events.append((idx, 'WRITE ', payload[:20].hex()))
    # Write request
    pos = raw.find(b'\x12\x27\x00')
    if pos != -1:
        payload = raw[pos+3:]
        all_events.append((idx, 'WREQ  ', payload[:20].hex()))

all_events.sort(key=lambda x: x[0])

def classify(direction, h):
    if direction.startswith('WRITE') or direction.startswith('WREQ'):
        return f"write: {h}"
    if h.startswith("a003"):
        step = int(h[6:8], 16)
        return f"<- ACK step=0x{step:02x}"
    elif h.startswith("a011"):
        t = int(h[4:6], 16)
        return f"<- A0 11 type=0x{t:02x}"
    elif h.startswith("a004"):
        base = int(h[6:8], 16)
        return f"<- A0 04 prompt base=0x{base:02x}"
    elif h.startswith("a006"):
        return f"<- A0 06 {h[4:8]}"
    elif h.startswith("a00a"):
        return f"<- A0 0A info"
    else:
        return f"<- {h}"

# Show everything from packet 1460 to 1490 (around handshake stage5-6 and streaming start)
print("Events around stage 5-6 transition and streaming start:")
for idx, direction, h in all_events:
    if 1460 <= idx <= 1490:
        print(f"  pkt={idx:4d} {direction}  {classify(direction, h)}")

print("\nAll post-handshake (from ACK 0x2c sighting):")
found_2c = False
count = 0
for idx, direction, h in all_events:
    if not found_2c:
        if direction == 'NOTIFY' and h.startswith("a003a02c"):
            found_2c = True
        else:
            continue
    print(f"  pkt={idx:4d} {direction}  {classify(direction, h)}")
    count += 1
    if count > 30:
        break
