import os

rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())

header = 16

# Check ALL 16 PRG banks' vectors
print('=== Vectors in each PRG bank ===')
for bank in range(16):
    off = header + bank * 0x2000
    nmi_lo, nmi_hi = data[off+0x1FFA], data[off+0x1FFB]
    rst_lo, rst_hi = data[off+0x1FFC], data[off+0x1FFD]
    irq_lo, irq_hi = data[off+0x1FFE], data[off+0x1FFF]
    nmi = nmi_lo|(nmi_hi<<8)
    rst = rst_lo|(rst_hi<<8)
    irq = irq_lo|(irq_hi<<8)
    print(f'  Bank {bank:2d}: NMI=${nmi:04X} RST=${rst:04X} IRQ=${irq:04X}')

# Check if bank 0 and bank 15 share the same reset vector
b0off = header + 0 * 0x2000
b15off = header + 15 * 0x2000

rst0_lo, rst0_hi = data[b0off+0x1FFC], data[b0off+0x1FFD]
rst0 = rst0_lo|(rst0_hi<<8)
rst15_lo, rst15_hi = data[b15off+0x1FFC], data[b15off+0x1FFD]
rst15 = rst15_lo|(rst15_hi<<8)

print(f'\nBank 0 RST vector: ${rst0:04X}')
print(f'Bank 15 RST vector: ${rst15:04X}')

# Compare first 16 bytes of bank 0 and bank 15 at the reset vector offset
if rst0 >= 0xE000:
    off0 = rst0 - 0xE000
    print(f'\nBank 0 first bytes at RST=${rst0:04X} (offset ${off0:04X}):')
    for i in range(32):
        if off0+i < 0x2000:
            print(f'  ${rst0+i:04X}: ${data[b0off+off0+i]:02X}')

    print(f'\nBank 15 first bytes at RST=${rst15:04X}:')
    off15 = rst15 - 0xE000
    for i in range(32):
        if off15+i < 0x2000:
            print(f'  ${rst15+i:04X}: ${data[b15off+off15+i]:02X}')
