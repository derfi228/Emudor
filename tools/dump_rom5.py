import os
path = r'E:\Claude_projects\Emu\NesEmu\roms\Batman - Return of the Joker (USA).nes'
rom = open(path,'rb').read()

def dump(label, file_off, cpu_base, count=128):
    print(f'\n--- {label} (file 0x{file_off:X}) ---')
    for i in range(0, count, 16):
        row = rom[file_off+i:file_off+i+16]
        print(f'{cpu_base+i:04X}: {" ".join(f"{b:02X}" for b in row)}')

b13off = 16 + 13 * 8192

# Bank 13 at $CEDE (the JMP target after dispatch)
# $CEDE is in slot 2 ($C000-$DFFF), bank 13
cede_off = b13off + (0xCEDE - 0xC000)
dump('Bank 13 $CEDE..CF1F', cede_off, 0xCEDE, 96)

# Bank 13 at $D3C2 (near $D394 we saw earlier) - more context
dump('Bank 13 $D320..D400', b13off + (0xD320 - 0xC000), 0xD320, 224)

# Bank 15 full init: $E310-E380 (to see what comes before the bank setup loop)
b15off = 16 + 15 * 8192
dump('Bank 15 $E310..E380', b15off + (0xE310 - 0xE000), 0xE310, 112)

# Verify: bank 15 at $E2E6 first byte (should be $78=SEI)
print(f'\nBank 15 $E2E6 first byte: 0x{rom[b15off + (0xE2E6 - 0xE000)]:02X} (expect 78=SEI)')
print(f'Bank 15 $E333 byte: 0x{rom[b15off + (0xE333 - 0xE000)]:02X} (should be D0=BNE)')
print(f'Bank 15 $E33C byte: 0x{rom[b15off + (0xE33C - 0xE000)]:02X} (should be 8E=STX)')
print(f'Bank 15 $E33F byte: 0x{rom[b15off + (0xE33F - 0xE000)]:02X} (should be 96=STX zpg,Y)')
