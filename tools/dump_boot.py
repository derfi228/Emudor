import os

rom_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'roms', 'Batman - Return of the Joker (USA).nes')
with open(rom_path, 'rb') as f:
    data = bytearray(f.read())

header = 16
NUM_PRG = 16  # 16x8KB PRG banks

opcodes = {
    0xA9:'LDA #', 0xA5:'LDA zp', 0xB5:'LDA zp,X', 0xAD:'LDA abs', 0xBD:'LDA abs,X', 0xB9:'LDA abs,Y',
    0x85:'STA zp', 0x95:'STA zp,X', 0x8D:'STA abs', 0x9D:'STA abs,X', 0x99:'STA abs,Y',
    0xA2:'LDX #', 0xA6:'LDX zp', 0xAE:'LDX abs', 0x86:'STX zp', 0x8E:'STX abs',
    0xA0:'LDY #', 0xA4:'LDY zp', 0xAC:'LDY abs', 0x84:'STY zp', 0x8C:'STY abs',
    0xE8:'INX', 0xCA:'DEX', 0xC8:'INY', 0x88:'DEY',
    0x4C:'JMP abs', 0x6C:'JMP ind', 0x20:'JSR abs', 0x60:'RTS', 0x40:'RTI',
    0xD0:'BNE', 0xF0:'BEQ', 0x10:'BPL', 0x30:'BMI', 0x90:'BCC', 0xB0:'BCS', 0x50:'BVC', 0x70:'BVS',
    0xE9:'SBC #', 0xE5:'SBC zp', 0x69:'ADC #', 0x65:'ADC zp', 0x6D:'ADC abs',
    0x29:'AND #', 0x25:'AND zp', 0x09:'ORA #', 0x05:'ORA zp', 0x49:'EOR #', 0x45:'EOR zp',
    0x38:'SEC', 0x18:'CLC', 0xF8:'SED', 0xD8:'CLD', 0x78:'SEI', 0x58:'CLI',
    0xAA:'TAX', 0x8A:'TXA', 0xA8:'TAY', 0x98:'TYA', 0x9A:'TXS', 0xBA:'TSX',
    0x48:'PHA', 0x68:'PLA', 0x08:'PHP', 0x28:'PLP',
    0xE6:'INC zp', 0xEE:'INC abs', 0xC6:'DEC zp', 0xCE:'DEC abs',
    0xC9:'CMP #', 0xC5:'CMP zp', 0xCC:'CPY abs', 0xEC:'CPX abs', 0xE0:'CPX #', 0xC0:'CPY #',
    0x24:'BIT zp', 0x2C:'BIT abs', 0xEA:'NOP',
    0x4A:'LSR A', 0x0A:'ASL A', 0x2A:'ROL A', 0x6A:'ROR A',
    0x46:'LSR zp', 0x56:'LSR zp,X', 0x4E:'LSR abs',
    0x26:'ROL zp', 0x66:'ROR zp', 0xBC:'LDY abs,X',
    0xD6:'DEC zp,X', 0xF6:'INC zp,X', 0xFE:'INC abs,X', 0xDE:'DEC abs,X',
    0x91:'STA (zp),Y', 0x81:'STA (zp,X)', 0xA1:'LDA (zp,X)', 0xB1:'LDA (zp),Y',
    0x00:'BRK', 0xCD:'CMP abs', 0xDD:'CMP abs,X',
    0x7E:'ROR abs,X', 0x1E:'ASL abs,X', 0x3E:'ROL abs,X', 0x5E:'LSR abs,X',
    0xD1:'CMP (zp),Y', 0xC1:'CMP (zp,X)',
    0xF5:'SBC zp,X', 0x75:'ADC zp,X', 0x35:'AND zp,X', 0x15:'ORA zp,X', 0x55:'EOR zp,X',
}
sizes = {
    'LDA #':2,'LDA zp':2,'LDA zp,X':2,'LDA abs':3,'LDA abs,X':3,'LDA abs,Y':3,
    'STA zp':2,'STA zp,X':2,'STA abs':3,'STA abs,X':3,'STA abs,Y':3,
    'LDX #':2,'LDX zp':2,'LDX abs':3,'STX zp':2,'STX abs':3,
    'LDY #':2,'LDY zp':2,'LDY abs':3,'STY zp':2,'STY abs':3,
    'INX':1,'DEX':1,'INY':1,'DEY':1,
    'JMP abs':3,'JMP ind':3,'JSR abs':3,'RTS':1,'RTI':1,
    'BNE':2,'BEQ':2,'BPL':2,'BMI':2,'BCC':2,'BCS':2,'BVC':2,'BVS':2,
    'SBC #':2,'SBC zp':2,'ADC #':2,'ADC zp':2,'ADC abs':3,
    'AND #':2,'AND zp':2,'ORA #':2,'ORA zp':2,'EOR #':2,'EOR zp':2,
    'SEC':1,'CLC':1,'SED':1,'CLD':1,'SEI':1,'CLI':1,
    'TAX':1,'TXA':1,'TAY':1,'TYA':1,'TXS':1,'TSX':1,
    'PHA':1,'PLA':1,'PHP':1,'PLP':1,
    'INC zp':2,'INC abs':3,'DEC zp':2,'DEC abs':3,'INC abs,X':3,'DEC abs,X':3,
    'CMP #':2,'CMP zp':2,'CMP abs':3,'CMP abs,X':3,'CMP (zp),Y':2,'CMP (zp,X)':2,
    'CPY abs':3,'CPX abs':3,'CPX #':2,'CPY #':2,
    'BIT zp':2,'BIT abs':3,'NOP':1,
    'LSR A':1,'ASL A':1,'ROL A':1,'ROR A':1,
    'LSR zp':2,'LSR zp,X':2,'LSR abs':3,'ROL zp':2,'ROR zp':2,
    'LDY abs,X':3,'INC zp,X':2,'DEC zp,X':2,
    'ROR abs,X':3,'ASL abs,X':3,'ROL abs,X':3,'LSR abs,X':3,
    'STA (zp),Y':2,'STA (zp,X)':2,'LDA (zp,X)':2,'LDA (zp),Y':2,
    'BRK':2,'CMP abs':3,
    'SBC zp,X':2,'ADC zp,X':2,'AND zp,X':2,'ORA zp,X':2,'EOR zp,X':2,
}

def disasm(bank_file_off, start_vaddr, slot_base, count, label=''):
    """Disassemble from start_vaddr (virtual address when bank is in slot at slot_base)."""
    if label:
        print(f'\n=== {label} ===')
    # Convert vaddr to file offset within bank
    pos = start_vaddr - slot_base
    addr = start_vaddr
    printed = 0
    while printed < count and 0 <= pos < 0x2000:
        op = data[bank_file_off + pos]
        mn = opcodes.get(op, f'db ${op:02X}')
        sz = sizes.get(mn, 1)
        b = [data[bank_file_off + pos + j] for j in range(min(sz, 0x2000 - pos))]
        bs = ' '.join(f'{x:02X}' for x in b)
        if sz == 1:
            print(f'  ${addr:04X}: {bs:<8}  {mn}')
        elif sz == 2:
            op2 = b[1] if len(b) > 1 else 0
            if mn in ('BNE','BEQ','BPL','BMI','BCC','BCS','BVC','BVS'):
                tgt = addr + 2 + (op2 if op2 < 128 else op2-256)
                print(f'  ${addr:04X}: {bs:<8}  {mn} ${tgt:04X}')
            else:
                print(f'  ${addr:04X}: {bs:<8}  {mn} ${op2:02X}')
        elif sz == 3:
            lo = b[1] if len(b) > 1 else 0
            hi = b[2] if len(b) > 2 else 0
            tgt = lo | (hi << 8)
            print(f'  ${addr:04X}: {bs:<8}  {mn} ${tgt:04X}')
        pos += sz
        addr += sz
        printed += 1
        if mn in ('RTS','RTI','JMP abs','JMP ind','BRK'):
            print()

# Bank 14 is slot 3: $E000-$FFFF
b14off = header + 14 * 0x2000
SLOT3 = 0xE000

# Read vectors
print('=== Bank 14 vectors ===')
for name, off in [('NMI', 0x1FFA), ('RST', 0x1FFC), ('IRQ', 0x1FFE)]:
    lo = data[b14off + off]
    hi = data[b14off + off + 1]
    print(f'  {name}: ${lo|(hi<<8):04X}')

nmi_lo = data[b14off + 0x1FFA]
nmi_hi = data[b14off + 0x1FFB]
rst_lo = data[b14off + 0x1FFC]
rst_hi = data[b14off + 0x1FFD]
nmi_addr = nmi_lo | (nmi_hi << 8)
rst_addr = rst_lo | (rst_hi << 8)

print(f'\nNMI handler: ${nmi_addr:04X}')
print(f'RST handler: ${rst_addr:04X}')

# Full init sequence from RST entry
disasm(b14off, rst_addr, SLOT3, 120, f'Bank 14 RST handler (${rst_addr:04X})')

# NMI handler
disasm(b14off, nmi_addr, SLOT3, 60, f'Bank 14 NMI handler (${nmi_addr:04X})')

# JSR $8000 locations in bank 14: $F3A1 and $F444
disasm(b14off, 0xF380, SLOT3, 40, 'Bank 14 around $F3A1 (JSR $8000)')
disasm(b14off, 0xF420, SLOT3, 40, 'Bank 14 around $F444 (JSR $8000)')

# Bank 2 at $8000 (slot 0)
b2off = header + 2 * 0x2000
SLOT0 = 0x8000
print('\n=== Bank 2 vectors (for reference) ===')
# Bank 2 cannot have vectors; it's not the boot bank
# Show bank 2 starting from $8000
disasm(b2off, 0x8000, SLOT0, 80, 'Bank 2 at $8000 (if in slot 0)')

# Show bank 2 around $8600 (where ZP $CC writes are)
disasm(b2off, 0x85F0, SLOT0, 60, 'Bank 2 around $8614 (STA $CC area)')
