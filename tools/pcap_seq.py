from scapy.all import PcapNgReader

packets = []
for p in PcapNgReader('data/pulse-ox.pcapng'):
    packets.append(bytes(p))

results = []
for idx, raw in enumerate(packets):
    pos = raw.find(b'\x1b\x27\x00')
    if pos == -1:
        continue
    payload = raw[pos+3:]
    if len(payload) < 2:
        continue
    results.append((idx, payload[:20].hex()))

print(f"Total ATT notifications on 0x0027: {len(results)}")
print("\nFirst 80 notifications:")
for i, (idx, h) in enumerate(results[:80]):
    note = ""
    if h.startswith("a003"):
        step = int(h[6:8], 16)
        note = f"  <- ACK step=0x{step:02x}"
    elif h.startswith("a011"):
        t = int(h[4:6], 16)
        note = f"  <- A0 11 type=0x{t:02x}"
    elif h.startswith("a004"):
        base = int(h[6:8], 16)
        note = f"  <- A0 04 prompt base=0x{base:02x}"
    elif h.startswith("a00a"):
        note = "  <- A0 0A info"
    print(f"  [{i:3d}] pkt={idx:4d} {h[:40]:40s}{note}")
