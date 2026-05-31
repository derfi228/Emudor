import os
path = r'E:\Claude_projects\Emu\NesEmu\roms\Batman - Return of the Joker (USA).nes'
rom = open(path,'rb').read()

def dump(label, file_off, cpu_base, count=128):
    print(f'\n--- {label} (file 0x{file_off:X}) ---')
    for i in range(0, count, 16):
        row = rom[file_off+i:file_off+i+16]
        print(f'{cpu_base+i:04X}: {" ".join(f"{b:02X}" for b in row)}')

# Bank 13 at $C324: file offset = 16 + 13*8192 + ($C324 - $C000) = header + bank + relative
b13off = 16 + 13 * 8192
c324off = b13off + (0xC324 - 0xC000)
dump('Bank 13 $C324..C3AF', c324off, 0xC324, 144)

# Bank 13 starting at $C000 (first 128 bytes)
dump('Bank 13 $C000..C07F', b13off, 0xC000, 128)

# Bank 14 $E339 context (to confirm JMP)
b14off = 16 + 14 * 8192
e339off = b14off + (0xE339 - 0xE000)
dump('Bank 14 $E320..E360', b14off + (0xE320 - 0xE000), 0xE320, 64)

# Bank 15 $E333..E360 loop (to confirm)
b15off = 16 + 15 * 8192
dump('Bank 15 $E333..E370', b15off + (0xE333 - 0xE000), 0xE333, 64)
