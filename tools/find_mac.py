"""Search the pcap for the target device MAC in any byte order."""
from scapy.all import PcapNgReader

target_le = bytes.fromhex('5314a7e0d153')   # 54:14:a7:e0:d1:53 little-endian
target_be = bytes(reversed(target_le))

found = []
for idx, p in enumerate(PcapNgReader('data/pulse-ox.pcapng')):
    b = bytes(p)
    for pat, label in [(target_le, 'LE'), (target_be, 'BE')]:
        pos = b.find(pat)
        if pos != -1:
            found.append(f"pkt {idx:4d} ({label}): offset {pos:3d}  context={b[max(0,pos-4):pos+10].hex()}")

if found:
    print(f"Found {len(found)} occurrence(s):")
    for line in found[:20]:
        print(line)
else:
    print("MAC 54:14:a7:e0:d1:53 NOT found anywhere in the pcap.")
    print("This is normal for btsnoop HCI captures — the MAC only appears in")
    print("HCI connection complete events, which may not be in this file.")
    print()
    print("All analysis was done by ATT handle (0x0027) which is device-specific.")
