import os
rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())
header = 16

for bname, bidx in [('bank14', 14), ('bank15', 15)]:
    off = header + bidx * 0x2000
    nmi  = data[off+0x1FFA] | (data[off+0x1FFB] << 8)
    rst  = data[off+0x1FFC] | (data[off+0x1FFD] << 8)
    irq  = data[off+0x1FFE] | (data[off+0x1FFF] << 8)
    print('%s: NMI=$%04X  RST=$%04X  IRQ/BRK=$%04X' % (bname, nmi, rst, irq))

b11 = header + 11 * 0x2000
print('\nBank11 first 8 bytes: ' + ' '.join('%02X' % data[b11+i] for i in range(8)))

b14 = header + 14 * 0x2000
print('\nBank14[$33C-$348]: ' + ' '.join('%02X' % data[b14+0x33C+i] for i in range(13)))

b15 = header + 15 * 0x2000
print('Bank15[$335-$350]: ' + ' '.join('%02X' % data[b15+0x335+i] for i in range(28)))

print('\nBank15[$2A9-$2C6] (IRQ handler):')
for i in range(0, 30):
    b = data[b15 + 0x2A9 + i]
    print('  $%04X: %02X' % (0xE2A9+i, b))

# What does the 'CEDE' location look like? It's the BRK yield point
# $CEDE is in slot2 ($C000-$DFFF) = bank13
b13 = header + 13 * 0x2000
ce_off = 0xCEDE - 0xC000  # = $0EDE
print('\nBank13 $CEDE (yield point) first 4 bytes: ' + ' '.join('%02X' % data[b13+ce_off+i] for i in range(4)))

# Also check bank12 $CEDE in case slot1 is involved
b12 = header + 12 * 0x2000
ce_off12 = 0xCEDE - 0xA000  # would be negative, so $CEDE is in bank13 range
print('Note: $CEDE maps to slot2 (bank13), offset in bank = $%04X' % ce_off)
