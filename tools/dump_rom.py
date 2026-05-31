import sys

rom = open('E:/Claude_projects/Emu/NesEmu/roms/Batman - Return of the Joker (USA).nes','rb').read()
print(f'ROM size: 0x{len(rom):X} bytes')

# Bank 12 offset: 16 header + 12*8192
b12off = 16 + 12 * 8192
print(f'Bank 12 file offset: 0x{b12off:X}')

# $A000 = slot 1 = bank-relative offset 0x2000
a000off = b12off + 0x2000
print(f'Bank 12 $A000 file offset: 0x{a000off:X}')
print('--- Bank 12 at $A000 (256 bytes) ---')
for i in range(0, 256, 16):
    row = rom[a000off+i:a000off+i+16]
    print(f'{0xA000+i:04X}: {" ".join(f"{b:02X}" for b in row)}')

# Also dump bank 12 first 256 bytes ($8000)
b12_8000 = b12off + 0x0000
print()
print('--- Bank 12 at $8000 (128 bytes) ---')
for i in range(0, 128, 16):
    row = rom[b12_8000+i:b12_8000+i+16]
    print(f'{0x8000+i:04X}: {" ".join(f"{b:02X}" for b in row)}')
