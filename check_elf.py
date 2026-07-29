from elftools.elf.elffile import ELFFile
with open(r'D:\OpenCode_projeck\test\TPMS-NRF52840\.pio\build\promicro_nrf52840\firmware.elf', 'rb') as f:
    elf = ELFFile(f)
    for seg in elf.iter_segments():
        t = seg['p_type']
        a = seg['p_paddr']
        v = seg['p_vaddr']
        fs = seg['p_filesz']
        ms = seg['p_memsz']
        print(f'Segment: type={t} paddr=0x{a:08X} vaddr=0x{v:08X} filesz={fs} memsz={ms}')
