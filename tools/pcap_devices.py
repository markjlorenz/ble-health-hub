"""Check what BD_ADDRs appear in the pcap and which connection handles are used."""
from scapy.all import PcapNgReader, raw

target = bytes.fromhex('5314a7e0d153')   # 54:14:a7:e0:d1:53 little-endian
target_be = bytes(reversed(target))       # big-endian form

addr_hits = {}
conn_handles = {}
total = 0

for idx, p in enumerate(PcapNgReader('data/pulse-ox.pcapng')):
    b = bytes(p)
    total += 1

    # Search for the target address in any byte order
    for pattern in (target, target_be):
        pos = b.find(pattern)
        if pos != -1:
            key = f"addr_found_at_offset_{pos}_pkt_{idx}"
            addr_hits[key] = toHex = b[pos:pos+6].hex()

    # HCI ACL handle is in the first 2 bytes of the HCI ACL payload.
    # In btsnoop/hcilog captures the packet starts with HCI indicator byte.
    # Try a few offsets for the connection handle field.
    for off in (1, 0, 4):
        if off + 2 <= len(b):
            h = int.from_bytes(b[off:off+2], 'little') & 0x0fff
            if 0 < h < 0x100:   # plausible HCI connection handle range
                conn_handles[h] = conn_handles.get(h, 0) + 1

print(f"Total packets: {total}")
print(f"\nTarget MAC occurrences (first 5): {list(addr_hits.items())[:5]}")
print(f"Total packets containing target MAC: {len(addr_hits)}")
print(f"\nTop HCI connection handles: {sorted(conn_handles.items(), key=lambda x: -x[1])[:10]}")
