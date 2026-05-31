import os
path = r'E:\Claude_projects\Emu\NesEmu\roms\Batman - Return of the Joker (USA).nes'
rom = open(path,'rb').read()

def dump(label, file_off, cpu_base, count=128):
    print(f'\n--- {label} (file 0x{file_off:X}) ---')
    for i in range(0, count, 16):
        row = rom[file_off+i:file_off+i+16]
        print(f'{cpu_base+i:04X}: {" ".join(f"{b:02X}" for b in row)}')

# Verify key bytes
b14off = 16 + 14 * 8192
b15off = 16 + 15 * 8192

# Confirm what bank 15 has at $E336-$E340 (the STX $A000 instruction and after)
print('\n=== Byte-by-byte verification ===')
for addr in [0xE333, 0xE334, 0xE335, 0xE336, 0xE337, 0xE338, 0xE339, 0xE33A, 0xE33B, 0xE33C, 0xE33D, 0xE33E, 0xE33F, 0xE340]:
    b15_byte = rom[b15off + (addr - 0xE000)]
    b14_byte = rom[b14off + (addr - 0xE000)]
    print(f'  ${addr:04X}: bank15={b15_byte:02X}, bank14={b14_byte:02X}')

# Bank 13 wider context $C300..C400
b13off = 16 + 13 * 8192
dump('Bank 13 $C300..C400', b13off + (0xC300 - 0xC000), 0xC300, 256)

# Bank 13 $C117 (called from bank 15 init after loop)
dump('Bank 13 $C100..C180', b13off + (0xC100 - 0xC000), 0xC100, 128)

# Bank 13 $D394 (JMP target after JSR $C117)
dump('Bank 13 $D394..D420', b13off + (0xD394 - 0xC000), 0xD394, 144)
