import sys

import os
rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())

header = 16
b12off = header + 12 * 0x2000
b13off = header + 13 * 0x2000
b14off = header + 14 * 0x2000

print('=== Bank 14 vectors ===')
for name, off in [('NMI',0x1FFA),('RST',0x1FFC),('IRQ',0x1FFE)]:
    lo = data[b14off+off]
    hi = data[b14off+off+1]
    print(f'  {name}: ${lo:02X} ${hi:02X} = ${lo|(hi<<8):04X}')

cede_off = 0x0EDE
print(f'\n=== Bank 13 at $CEDE (offset {cede_off:#x}, file off {b13off + cede_off:#x}) ===')
for i in range(64):
    addr = 0xC000 + cede_off + i
    b = data[b13off + cede_off + i]
    if i % 16 == 0:
        print(f'  ${addr:04X}:', end=' ')
    print(f'{b:02X}', end=' ')
    if i % 16 == 15:
        print()
print()

print(f'=== Bank 12 at $A000 (file off {b12off:#x}) ===')
for i in range(64):
    addr = 0xA000 + i
    b = data[b12off + i]
    if i % 16 == 0:
        print(f'  ${addr:04X}:', end=' ')
    print(f'{b:02X}', end=' ')
    if i % 16 == 15:
        print()
print()

b = data[b13off + cede_off]
print(f'First byte at $CEDE = ${b:02X}')

# Search for RTI ($40) in bank 13
print('\nAll RTI ($40) offsets in bank 13:')
for i in range(0x2000):
    if data[b13off + i] == 0x40:
        print(f'  ${0xC000+i:04X} (file off {b13off+i:#x})')

# Search for RTI in bank 12
print('\nAll RTI ($40) offsets in bank 12:')
for i in range(0x2000):
    if data[b12off + i] == 0x40:
        print(f'  ${0xA000+i:04X} (file off {b12off+i:#x})')

# Also check what's at $9D4C in bank 11 - does it contain RTI anywhere?
b11off = header + 11 * 0x2000
print('\nAll RTI ($40) offsets in bank 11:')
for i in range(0x2000):
    if data[b11off + i] == 0x40:
        print(f'  ${0x8000+i:04X} (file off {b11off+i:#x})')
