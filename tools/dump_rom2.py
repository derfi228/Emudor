import os
path = r'E:\Claude_projects\Emu\NesEmu\roms\Batman - Return of the Joker (USA).nes'
rom = open(path,'rb').read()

def dump(label, file_off, cpu_base, count=128):
    print(f'\n--- {label} (file 0x{file_off:X}) ---')
    for i in range(0, count, 16):
        row = rom[file_off+i:file_off+i+16]
        print(f'{cpu_base+i:04X}: {" ".join(f"{b:02X}" for b in row)}')

# Bank 15 init code: $E2E6 = bank15 offset $02E6
b15off = 16 + 15 * 8192
dump('Bank 15 $E2E6..E37F (init code)', b15off + 0x02E6, 0xE2E6, 160)

# Bank 15 end: IRQ/NMI/RST vectors at $FFFA-$FFFF
dump('Bank 15 vectors at $FFFA', b15off + 0x1FFA, 0xFFFA, 6)

# Bank 14 vectors at $FFFA-$FFFF
b14off = 16 + 14 * 8192
dump('Bank 14 vectors at $FFFA', b14off + 0x1FFA, 0xFFFA, 6)

# Bank 14 at $E320-$E36F (context around $E33F)
dump('Bank 14 $E320..E37F', b14off + 0x0320, 0xE320, 96)

# Bank 11 dispatcher $9D4C: bank-relative $1D4C
b11off = 16 + 11 * 8192
dump('Bank 11 $9D4C..9DAF (dispatcher)', b11off + 0x1D4C, 0x9D4C, 100)
dump('Bank 11 $9DA0..9E0F', b11off + 0x1DA0, 0x9DA0, 112)
