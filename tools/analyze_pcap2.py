"""
Full analysis of pulse-ox-2.pcapng — compare with pulse-ox.pcapng.
Dumps:
  1. All host writes (ATT Write Command 0x52 / Write Request 0x12)
  2. All device notifications (ATT Notification 0x1b)
  3. Flags A0 06 / A0 04 / A0 11 / A0 03 types
  4. Searches for target MAC in both byte orders
"""
import sys
from scapy.all import PcapNgReader

PCAP = "data/pulse-ox-2.pcapng"
TARGET_LE = bytes.fromhex("5314a7e0d153")   # 54:14:a7:e0:d1:53
TARGET_BE  = bytes(reversed(TARGET_LE))

def find_bytes(h, needle, start=0):
    pos = h.find(needle, start)
    return pos

def scan_att(data):
    """Scan for ATT PDU header at any offset up to 16 bytes in."""
    for off in range(min(len(data), 16)):
        if off >= len(data):
            break
        op = data[off]
        if op in (0x52, 0x12, 0x1b):
            return off, op
    return None, None

events = []
mac_hits = []

for idx, p in enumerate(PcapNgReader(PCAP)):
    b = bytes(p)

    # MAC search
    for pat, label in [(TARGET_LE, "LE"), (TARGET_BE, "BE")]:
        pos = find_bytes(b, pat)
        if pos != -1:
            mac_hits.append(f"pkt {idx:4d} ({label}) offset={pos}  ctx={b[max(0,pos-4):pos+10].hex()}")

    # ATT opcode scan
    off, op = scan_att(b)
    if op is None:
        continue

    payload = b[off+1:]   # everything after the opcode

    if op == 0x1b:
        # Notification: next 2 bytes = handle (little-endian)
        if len(payload) < 3:
            continue
        handle = int.from_bytes(payload[0:2], 'little')
        value = payload[2:]
        events.append(("NOTIFY", idx, handle, bytes(value)))

    elif op in (0x52, 0x12):
        # Write: next 2 bytes = handle
        if len(payload) < 3:
            continue
        handle = int.from_bytes(payload[0:2], 'little')
        value = payload[2:]
        events.append(("WRITE", idx, handle, bytes(value)))

# ---- Report ----
all_handles = sorted(set(e[2] for e in events))
print(f"Pcap: {PCAP}")
print(f"Total ATT events found: {len(events)}")
print(f"ATT handles seen: {[hex(h) for h in all_handles]}")
print()

# Focus on the vendor characteristic (handle 0x0027)
vendor = [(kind, idx, val) for kind, idx, handle, val in events if handle == 0x0027]
print(f"Events on handle 0x0027: {len(vendor)}")
print()

writes  = [(idx, val) for kind, idx, val in vendor if kind == "WRITE"]
notifs  = [(idx, val) for kind, idx, val in vendor if kind == "NOTIFY"]
print(f"  Writes:        {len(writes)}")
print(f"  Notifications: {len(notifs)}")
print()

print("=== ALL WRITES (host→device) on 0x0027 ===")
for idx, val in writes:
    tag = ""
    if val[:2] == b'\xb0\x11': tag = "  ← stage1 b011"
    elif val[:2] == b'\xb0\x06': tag = "  ← stage1 b006"
    elif val[:1] == b'\xb0': tag = "  ← B0 cmd"
    print(f"  pkt {idx:4d}: {val.hex()}{tag}")

print()
print("=== ALL NOTIFICATIONS (device→host) on 0x0027 ===")
for idx, val in notifs:
    tag = ""
    if len(val) >= 2:
        if val[0] == 0xa0 and val[1] == 0x03: tag = "  ACK"
        elif val[0] == 0xa0 and val[1] == 0x04: tag = f"  A004 base=0x{val[3]:02x} tail={val[5]:02x}{val[6]:02x}" if len(val)>=7 else "  A004"
        elif val[0] == 0xa0 and val[1] == 0x06: tag = f"  *** A006 finger-detected base=0x{val[3]:02x} ***" if len(val)>=4 else "  A006"
        elif val[0] == 0xa0 and val[1] == 0x11: tag = f"  A011 type=0x{val[2]:02x}" if len(val)>=3 else "  A011"
        elif val[0] == 0xa0 and val[1] == 0x0a: tag = "  A00A"
    print(f"  pkt {idx:4d} ({len(val):2d}B): {val.hex()}{tag}")

print()
if mac_hits:
    print(f"=== MAC 54:14:a7:e0:d1:53 found in {len(mac_hits)} packets ===")
    for h in mac_hits[:10]:
        print(" ", h)
else:
    print("MAC 54:14:a7:e0:d1:53 not found in raw packet bytes (normal for HCI captures).")
