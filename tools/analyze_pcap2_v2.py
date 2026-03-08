"""
Pattern-based analysis of pulse-ox-2.pcapng.
Instead of parsing ATT headers (which vary by capture tool/DLT),
scan every packet for the distinctive vendor payload patterns.
"""
from scapy.all import PcapNgReader

PCAP = "data/pulse-ox-2.pcapng"

# Vendor patterns to look for anywhere in each packet
PATTERNS = {
    "A003_ACK":   (b'\xa0\x03\xa0', "Device ACK"),
    "A004_PROMPT":(b'\xa0\x04\x00', "Device A0 04 prompt"),
    "A006_FINGER":(b'\xa0\x06\x00', "Device A0 06 finger-detect"),
    "A011_F0":    (b'\xa0\x11\xf0', "Device A0 11 F0 measurement"),
    "A011_OTHER": (b'\xa0\x11',     "Device A0 11 (any type)"),
    "A00A":       (b'\xa0\x0a',     "Device A0 0A"),
    "B011":       (b'\xb0\x11',     "Host B0 11 write"),
    "B006":       (b'\xb0\x06',     "Host B0 06 write"),
    "B003":       (b'\xb0\x03\xa0', "Host B0 03 write"),
    "B004":       (b'\xb0\x04\x00', "Host B0 04 write"),
}

results = {k: [] for k in PATTERNS}
raw_packets = []

for idx, p in enumerate(PcapNgReader(PCAP)):
    b = bytes(p)
    raw_packets.append((idx, b))

    for key, (pat, _) in PATTERNS.items():
        pos = b.find(pat)
        if pos != -1:
            results[key].append((idx, pos, b[pos:pos+20]))

print(f"Pcap: {PCAP}")
print(f"Total packets: {len(raw_packets)}")
print()

for key, (pat, label) in PATTERNS.items():
    hits = results[key]
    if not hits:
        continue
    print(f"=== {label} ({len(hits)} occurrences) ===")
    for idx, pos, ctx in hits[:60]:
        ctxhex = ctx.hex()
        extra = ""
        if key == "A004_PROMPT" and len(ctx) >= 7:
            base = ctx[3]
            t1, t2 = ctx[5], ctx[6]
            extra = f"  base=0x{base:02x} tail={t1:02x}{t2:02x}"
        elif key == "A006_FINGER" and len(ctx) >= 7:
            base = ctx[3]
            t1, t2 = ctx[5], ctx[6]
            extra = f"  base=0x{base:02x} tail={t1:02x}{t2:02x} len={len(ctx)}"
        elif key == "A003_ACK" and len(ctx) >= 4:
            step = ctx[3]
            extra = f"  step=0x{step:02x}"
        elif key == "A011_F0" and len(ctx) >= 10:
            strength = ctx[8]
            spo2 = ctx[9] + 50
            config = (ctx[10] << 8) | ctx[11] if len(ctx) >= 12 else 0
            hr = ((ctx[12] << 8) | ctx[13]) / 10.0 if len(ctx) >= 14 else 0
            extra = f"  strength={strength} SpO2={spo2}% HR={hr:.1f}bpm config=0x{config:04x}"
        elif key.startswith("B00"):
            extra = f"  full={ctx.hex()}"
        print(f"  pkt {idx:4d} offset={pos:3d}: {ctxhex[:40]}{extra}")
    if len(hits) > 60:
        print(f"  ... ({len(hits)-60} more)")
    print()

# Show the DLT types and first few raw bytes
print("=== First 5 packets raw bytes (first 32 bytes each) ===")
for idx, b in raw_packets[:5]:
    print(f"  pkt {idx:2d} len={len(b):3d}: {b[:32].hex()}")
