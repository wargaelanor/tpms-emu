import struct, os, sys

# Read CURRENT.UF2 from E:
current_path = "E:\\CURRENT.UF2"
# Read our firmware ELF
elf_path = sys.argv[1] if len(sys.argv) > 1 else ".pio\\build\\promicro_nrf52840\\firmware.elf"

from elftools.elf.elffile import ELFFile

# Read CURRENT.UF2
with open(current_path, 'rb') as f:
    uf2_data = bytearray(f.read())

# Parse ELF to get application data
with open(elf_path, 'rb') as f:
    elf = ELFFile(f)
    segments = []
    for seg in elf.iter_segments():
        if seg['p_type'] == 'PT_LOAD' and seg['p_filesz'] > 0:
            segments.append((seg['p_paddr'], bytes(seg.data())))

# Build a map of addr->data from ELF
elf_map = {}
for addr, data in segments:
    elf_map[addr] = data

print(f"ELF segments: {[(hex(a), len(d)) for a, d in segments]}")

# Parse UF2 blocks and patch ones that overlap our ELF
num_blocks = len(uf2_data) // 512
patched_count = 0
for i in range(num_blocks):
    off = i * 512
    magic0 = struct.unpack('<I', uf2_data[off:off+4])[0]
    if magic0 != 0x0A324655:
        print(f"Block {i}: bad magic 0x{magic0:08X}")
        continue
    family = struct.unpack('<I', uf2_data[off+4:off+8])[0]
    flags = struct.unpack('<I', uf2_data[off+8:off+12])[0]
    addr = struct.unpack('<I', uf2_data[off+12:off+16])[0]
    psize = struct.unpack('<I', uf2_data[off+16:off+20])[0]
    bno = struct.unpack('<I', uf2_data[off+20:off+24])[0]
    nblocks = struct.unpack('<I', uf2_data[off+24:off+28])[0]
    reserved = struct.unpack('<I', uf2_data[off+28:off+32])[0]
    
    # Check if this block's address range overlaps any ELF segment
    block_start = addr
    block_end = addr + psize
    patch_data = None
    patch_addr = None
    
    for el_addr, el_data in elf_map.items():
        el_start = el_addr
        el_end = el_addr + len(el_data)
        if block_start < el_end and block_end > el_start:
            # Overlap - patch this block
            overlap_start = max(block_start, el_start)
            overlap_end = min(block_end, el_end)
            src_off = overlap_start - el_start
            dst_off = overlap_start - block_start
            length = overlap_end - overlap_start
            payload = bytearray(uf2_data[off+32:off+32+psize])
            for j in range(length):
                payload[dst_off + j] = el_data[src_off + j]
            patch_data = bytes(payload)
            patch_addr = addr
            break
    
    if patch_data is not None:
        uf2_data[off+32:off+32+psize] = patch_data
        patched_count += 1

print(f"Patched {patched_count} blocks")

# Write patched UF2
out_path = "E:\\firmware.uf2"
with open(out_path, 'wb') as f:
    f.write(uf2_data)
print(f"Wrote {os.path.getsize(out_path)} bytes to {out_path}")

# Verify first block
with open(out_path, 'rb') as f:
    b = f.read(32)
    vals = struct.unpack('<IIIIIIII', b)
    print(f"Magic0=0x{vals[0]:08X} Family=0x{vals[1]:08X} Flags=0x{vals[2]:08X} Addr=0x{vals[3]:08X}")
    print(f"Payload={vals[4]} Block={vals[5]}/{vals[6]} Reserved=0x{vals[7]:08X}")
