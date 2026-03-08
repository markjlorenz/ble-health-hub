from scapy.all import PcapNgReader

packets = []
for p in PcapNgReader('data/pulse-ox.pcapng'):
    packets.append(bytes(p))

print("=== ALL HOST WRITES TO 0x0027 ===")
print("(Write Command 0x52, Write Request 0x12)")
# Find writes with correct ATT framing
# In BLE: HCI ACL Data header (4 bytes) -> L2CAP header (4 bytes) -> ATT PDU
# But scapy strips some of it. Try finding by ATT opcode + handle bytes

for idx, raw in enumerate(packets):
    for opcode in (b'\x52\x27\x00', b'\x12\x27\x00'):
        pos = raw.find(opcode)
        if pos == -1:
            continue
        payload = raw[pos+3:]  # skip opcode+handle
        # ATT write payload follows. Take first 20 bytes
        p20 = payload[:20].hex()
        label = "WR_CMD" if opcode[0] == 0x52 else "WR_REQ"
        print(f"  pkt={idx:4d} {label} {p20}")

print("\n=== ALL NOTIFICATIONS ON 0x0027 ===")
for idx, raw in enumerate(packets):
    pos = raw.find(b'\x1b\x27\x00')
    if pos == -1:
        continue
    payload = raw[pos+3:]
    h = payload[:20].hex()
    note = ""
    if h.startswith("a003"):
        note = f"  ACK step=0x{int(h[6:8],16):02x}"
    elif h.startswith("a011"):
        note = f"  A011 type=0x{int(h[4:6],16):02x}"
    elif h.startswith("a004"):
        note = f"  PROMPT base=0x{int(h[6:8],16):02x} tail={h[10:14]}"
    elif h.startswith("a006"):
        note = f"  A006 !! {h[4:20]}"
    elif h.startswith("a00a"):
        note = f"  A00A info"
    print(f"  pkt={idx:4d} NOTIFY {h[:40]:40s}{note}")
