import os

rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())

header = 16
num_banks = (len(data) - header) // 0x2000
print(f'Total 8KB banks: {num_banks}')

# Search ALL banks for:
# 1. JSR $8000 (20 00 80) or JMP $8000 (4C 00 80)
# 2. LDA #$00 followed shortly by STA $11 + STA $12 (which could set ZP$11=0, $12=$80 for JMP $8000)
# 3. any writes to ZP $CC (85 CC = STA $CC, 86 CC = STX $CC, 84 CC = STY $CC, 95 CC etc.)
# 4. RTI instruction in the dispatch path

print('\n=== JSR/JMP to $8000 in all banks ===')
for bank in range(num_banks):
    off = header + bank * 0x2000
    base_addr = 0x8000  # banks are always $8000-$9FFF when mapped
    for i in range(0x1FFE):
        b0, b1, b2 = data[off+i], data[off+i+1], data[off+i+2]
        addr_in_bank = base_addr + i
        if (b0 == 0x20 or b0 == 0x4C) and b1 == 0x00 and b2 == 0x80:
            mn = 'JSR' if b0 == 0x20 else 'JMP'
            print(f'  Bank {bank:2d}, ${addr_in_bank:04X}: {mn} $8000')

print('\n=== LDA into $0012 with hi=$80 (could set task ptr) ===')
for bank in range(num_banks):
    off = header + bank * 0x2000
    base_addr = 0x8000
    for i in range(0x1FFD):
        b0, b1, b2 = data[off+i], data[off+i+1], data[off+i+2]
        # LDA #$80 (A9 80) followed within a few bytes by STA $12 (85 12)
        if b0 == 0xA9 and b1 == 0x80:
            # Look for STA $12 within next 6 bytes
            for j in range(2, 8):
                if i+j+1 < 0x2000 and data[off+i+j] == 0x85 and data[off+i+j+1] == 0x12:
                    print(f'  Bank {bank:2d}, ${base_addr+i:04X}: LDA #$80 ... STA $12 (could set task hi to $80)')
                    break

print('\n=== Writes to ZP $CC (sets task creation flag) ===')
for bank in range(num_banks):
    off = header + bank * 0x2000
    base_addr = 0x8000
    for i in range(0x1FFE):
        b0, b1 = data[off+i], data[off+i+1]
        if b1 == 0xCC and b0 in (0x85, 0x84, 0x86, 0x95, 0x96):
            mn = {0x85:'STA', 0x84:'STY', 0x86:'STX', 0x95:'STA zp,X', 0x96:'STX zp,Y'}[b0]
            print(f'  Bank {bank:2d}, ${base_addr+i:04X}: {mn} $CC')

print('\n=== Non-zero initial task values written to $7CC0 in bank 0 ===')
# Bank 0 is the "init" bank
b0off = header + 0 * 0x2000
for i in range(0x1FFE):
    b0, b1, b2 = data[b0off+i], data[b0off+i+1], data[b0off+i+2]
    # STA abs,X $7CC0 = 9D C0 7C
    if b0 == 0x9D and b1 == 0xC0 and b2 == 0x7C:
        print(f'  Bank 0, ${0x8000+i:04X}: STA $7CC0,X')
    # STA abs,Y $7CC0 = 99 C0 7C
    if b0 == 0x99 and b1 == 0xC0 and b2 == 0x7C:
        print(f'  Bank 0, ${0x8000+i:04X}: STA $7CC0,Y')

print('\n=== Context around first bank 0 STA $7CC0 ===')
# Find first one and dump context
for i in range(0x1FFE):
    b0, b1, b2 = data[b0off+i], data[b0off+i+1], data[b0off+i+2]
    if b0 == 0x9D and b1 == 0xC0 and b2 == 0x7C:
        start = max(0, i - 30)
        addr = 0x8000 + start
        print(f'First STA $7CC0,X at bank 0 offset {i:#x} (${0x8000+i:04X})')
        # Just dump bytes
        for j in range(50):
            if start+j >= 0x2000:
                break
            print(f'  ${addr+j:04X}: {data[b0off+start+j]:02X}')
        break
