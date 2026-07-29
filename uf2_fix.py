import struct, os, sys

UF2_MAGIC_START = 0x0A324655
UF2_MAGIC_END = 0x0AB32F51
UF2_FLAG_FAMILYID = 0x00002000
NRF52840_FAMILY = 0x52B66C2E
CHUNK = 256

from elftools.elf.elffile import ELFFile

elf_path = sys.argv[1]
out_path = sys.argv[2]

with open(elf_path, 'rb') as f:
    elf = ELFFile(f)
    segments = []
    for seg in elf.iter_segments():
        if seg['p_type'] == 'PT_LOAD' and seg['p_filesz'] > 0:
            segments.append((seg['p_paddr'], bytes(seg.data())))

min_addr = min(s[0] for s in segments)
max_addr = max(s[0] + len(s[1]) for s in segments)
total = max_addr - min_addr

data = bytearray(total)
for addr, seg_data in segments:
    off = addr - min_addr
    data[off:off+len(seg_data)] = seg_data

start_addr = min_addr
num_blocks = (total + CHUNK - 1) // CHUNK
print(f'Start: 0x{start_addr:X}, Size: {total}, Blocks: {num_blocks}')

with open(out_path, 'wb') as out:
    for i in range(num_blocks):
        chunk_off = i * CHUNK
        chunk = data[chunk_off:chunk_off + CHUNK]
        if len(chunk) < CHUNK:
            chunk = chunk + b'\xff' * (CHUNK - len(chunk))

        padding = b'\x00' * (476 - CHUNK)

        hd = struct.pack('<IIIIIIII',
            UF2_MAGIC_START,
            UF2_FLAG_FAMILYID,
            start_addr + chunk_off,
            CHUNK,
            i,
            num_blocks,
            NRF52840_FAMILY,
            0
        )

        block = hd + chunk + padding + struct.pack('<I', UF2_MAGIC_END)
        assert len(block) == 512, f'Block size {len(block)} != 512'
        out.write(block)

sz = os.path.getsize(out_path)
print(f'Wrote {sz} bytes')

with open(out_path, 'rb') as f:
    b = f.read(512)
    hd = struct.unpack('<IIIIIIII', b[0:32])
    print(f'Magic1=0x{hd[0]:X} Flags=0x{hd[1]:X} Addr=0x{hd[2]:X} Payload={hd[3]} Block={hd[4]}/{hd[5]} Family=0x{hd[6]:X}')
    m2 = struct.unpack('<I', b[508:512])[0]
    print(f'Magic2=0x{m2:X} (expect 0x{UF2_MAGIC_END:X})')
